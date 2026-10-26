#include "report_sink_reader.hpp"
#include "sha256.hpp"
#include "report_sink_phase_log.hpp"
#include "report_sink_late_workers.hpp"

#include <pineforge/report_sink_writer.hpp> // v1's already-tested scalar formatting only

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cfenv>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

#if defined(PINEFORGE_READER_WITH_ZSTD) && PINEFORGE_READER_WITH_ZSTD
#include <zstd.h>
#endif

namespace pineforge {
namespace report_sink {
namespace v2 {

FdSource::FdSource(int fd) : fd_(fd), size_(0) {
    struct stat status{};
    if (::fstat(fd_, &status) < 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
        throw ReaderError(Failure::io, "invalid staging descriptor");
    size_ = static_cast<std::uint64_t>(status.st_size);
}
Bytes FdSource::readRange(std::uint64_t offset, std::size_t length) {
    Bytes result;
    read_into(offset, length, result);
    return result;
}
void FdSource::read_into(std::uint64_t offset, std::size_t length, Bytes& result) {
    if (offset > size_ || length > size_ - offset)
        throw ReaderError(Failure::integrity, "verification range outside staging");
    if (result.capacity() < length) Bytes(length).swap(result);
    else result.resize(length);
    std::size_t at = 0;
    while (at < length) {
        const auto n = ::pread(fd_, result.data() + at, length - at, static_cast<off_t>(offset + at));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw ReaderError(Failure::io, "staging pread failed or short");
        at += static_cast<std::size_t>(n);
    }
}

namespace {
namespace L = layout;

[[noreturn]] void fail(const std::string& text, Failure reason = Failure::malformed) {
    throw ReaderError(reason, "sink v2: " + text);
}
void need(bool condition, const char* text) { if (!condition) fail(text); }
void integrity(bool condition, const char* text) { if (!condition) fail(text, Failure::integrity); }
std::string_view view(const Bytes& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::uint64_t le(std::string_view bytes, std::size_t pos, unsigned width) {
    need(pos <= bytes.size() && width <= bytes.size() - pos, "short fixed-width value");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= std::uint64_t(static_cast<unsigned char>(bytes[pos + i])) << (8 * i);
    return value;
}
std::int64_t signed_word(std::uint64_t word) {
    // Avoid implementation-defined unsigned-to-signed conversion.
    return word <= std::uint64_t(INT64_MAX) ? static_cast<std::int64_t>(word)
        : -1 - static_cast<std::int64_t>(~word);
}
double number(std::uint64_t word) {
    static_assert(sizeof(double) == sizeof(word) && std::numeric_limits<double>::is_iec559);
    double value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}
std::uint64_t bits(double value) {
    std::uint64_t word;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}
bool finite_word(std::uint64_t word) { return ((word >> 52) & 0x7ff) != 0x7ff; }
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    need(a <= L::safe && b <= L::safe - a, "overflowed safe offset/count");
    return a + b;
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    need(a <= L::safe && (a == 0 || b <= L::safe / a), "overflowed safe offset product");
    return a * b;
}
std::size_t host_size(std::uint64_t n) {
    need(n <= std::numeric_limits<std::size_t>::max(), "length does not fit size_t");
    return static_cast<std::size_t>(n);
}
void zero(std::string_view data) {
    need(std::all_of(data.begin(), data.end(), [](char c) { return c == 0; }), "nonzero reserved/padding bytes");
}
Digest digest_at(std::string_view data, std::size_t at) {
    need(at <= data.size() && L::digest <= data.size() - at, "short digest");
    Digest result{};
    std::memcpy(result.data(), data.data() + at, result.size());
    return result;
}
std::string_view digest_view(const Digest& d) {
    return {reinterpret_cast<const char*>(d.data()), d.size()};
}
Digest sha(std::string_view data) { Sha256 h; h.update(data); return h.digest(); }
std::string hex(const Digest& d) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 * d.size());
    for (auto b : d) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
struct Crc {
    std::uint32_t state = UINT32_C(0xffffffff);
    void update(std::string_view data) {
        for (unsigned char byte : data) {
            state ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit)
                state = (state >> 1) ^ ((state & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    std::uint32_t digest() const { return state ^ UINT32_C(0xffffffff); }
};
std::uint32_t crc(std::string_view bytes) { Crc c; c.update(bytes); return c.digest(); }
template<class Hash> void fixed(Hash& hash, std::uint64_t value, unsigned width) {
    char bytes[8];
    for (unsigned i = 0; i < width; ++i) bytes[i] = static_cast<char>(value >> (8 * i));
    hash.update({bytes, width});
}
std::size_t tag_index(char tag) {
    const auto pos = L::tags.find(tag);
    need(pos != std::string_view::npos, "unknown record tag");
    return pos;
}
void utf8(std::string_view bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        if (b < 0x80) { ++i; continue; }
        std::size_t n = b >= 0xc2 && b <= 0xdf ? 2
            : b >= 0xe0 && b <= 0xef ? 3 : b >= 0xf0 && b <= 0xf4 ? 4 : 0;
        need(n != 0 && n <= bytes.size() - i, "invalid UTF-8");
        for (std::size_t j = 1; j < n; ++j) {
            const auto c = static_cast<unsigned char>(bytes[i + j]);
            need(c >= 0x80 && c <= 0xbf, "invalid UTF-8 continuation");
        }
        const auto second = static_cast<unsigned char>(bytes[i + 1]);
        need(!((b == 0xe0 && second < 0xa0) || (b == 0xed && second >= 0xa0)
            || (b == 0xf0 && second < 0x90) || (b == 0xf4 && second >= 0x90)), "invalid Unicode scalar");
        i += n;
    }
}

// A bounded, ordered JSON tree used only during open. Summary bytes are kept
// verbatim; no JSON library, lossy map, or record-sized DOM is retained.
struct Json {
    enum class Type { object, array, string, numeric, boolean, null };
    Type type = Type::null;
    std::string key, text;
    double numeric = 0.0;
    std::vector<Json> children;
};
double json_number(std::string_view token) {
    double value = 0;
    const auto r = std::from_chars(token.data(), token.data() + token.size(), value);
    need(r.ptr == token.data() + token.size(), "invalid JSON number");
    if (r.ec == std::errc::result_out_of_range) {
        // from_chars reports underflow too. JSON permits a finite conversion
        // to signed zero; distinguish it from overflow without locale parsing.
        const auto exponent_at = token.find_first_of("eE");
        const auto significand = token.substr(token.front() == '-' ? 1 : 0,
            exponent_at == std::string_view::npos ? token.size() : exponent_at - (token.front() == '-' ? 1 : 0));
        std::int64_t exponent = 0;
        if (exponent_at != std::string_view::npos) {
            auto digits = token.substr(exponent_at + 1);
            const bool negative = digits.front() == '-';
            if (digits.front() == '+' || negative) digits.remove_prefix(1);
            for (char c : digits)
                exponent = std::min<std::int64_t>(4 * L::max_head, exponent * 10 + c - '0');
            if (negative) exponent = -exponent;
        }
        const auto dot = significand.find('.');
        std::int64_t place = static_cast<std::int64_t>(dot == std::string_view::npos ? significand.size() : dot);
        bool nonzero = false;
        for (char c : significand) {
            if (c == '.') continue;
            --place;
            if (c != '0') { nonzero = true; break; }
        }
        need(!nonzero || exponent + place < 0, "non-finite JSON number");
        value = token.front() == '-' ? -0.0 : 0.0;
    } else need(r.ec == std::errc{} && std::isfinite(value), "non-finite JSON number");
    return value;
}
class JsonParser {
public:
    JsonParser(std::string_view data, bool compact, bool canonical_strings = false)
        : data_(data), compact_(compact), canonical_strings_(canonical_strings) { utf8(data); }
    Json parse() {
        auto result = value(0);
        space();
        need(pos_ == data_.size(), "trailing JSON bytes");
        return result;
    }
private:
    std::string_view data_;
    bool compact_, canonical_strings_;
    std::size_t pos_ = 0;
    void space() {
        while (pos_ < data_.size() && (data_[pos_] == ' ' || data_[pos_] == '\n'
                || data_[pos_] == '\r' || data_[pos_] == '\t')) {
            need(!compact_, "whitespace in compact JSON");
            ++pos_;
        }
    }
    char take() { need(pos_ < data_.size(), "truncated JSON"); return data_[pos_++]; }
    unsigned hex4() {
        unsigned v = 0;
        for (unsigned j = 0; j < 4; ++j) {
            const char c = take();
            const int digit = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            need(digit >= 0, "bad JSON Unicode escape");
            v = v * 16 + static_cast<unsigned>(digit);
        }
        return v;
    }
    static void scalar(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
        }
    }
    std::string string() {
        const auto begin = pos_;
        need(take() == '"', "expected JSON string");
        std::string result;
        while (true) {
            const unsigned char c = static_cast<unsigned char>(take());
            if (c == '"') {
                if (canonical_strings_)
                    need(data_.substr(begin, pos_ - begin) == escaped_string(result), "noncanonical HEAD string escaping");
                return result;
            }
            need(c >= 0x20, "control byte in JSON string");
            if (c != '\\') { result += static_cast<char>(c); continue; }
            switch (take()) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    need(take() == '\\' && take() == 'u', "unpaired JSON surrogate");
                    const unsigned low = hex4();
                    need(low >= 0xdc00 && low <= 0xdfff, "unpaired JSON surrogate");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else need(cp < 0xdc00 || cp > 0xdfff, "unpaired JSON surrogate");
                scalar(result, cp); break;
            }
            default: fail("unknown JSON escape");
            }
        }
    }
    Json value(std::size_t depth) {
        space();
        need(pos_ < data_.size(), "missing JSON value");
        Json j;
        const char c = data_[pos_];
        if (c == '{' || c == '[') {
            need(depth < L::max_depth, "JSON exceeds depth 64");
            j.type = c == '{' ? Json::Type::object : Json::Type::array;
            ++pos_; space();
            const char close = c == '{' ? '}' : ']';
            if (pos_ < data_.size() && data_[pos_] == close) { ++pos_; return j; }
            for (;;) {
                std::string key;
                if (c == '{') {
                    space(); key = string(); space(); need(take() == ':', "missing JSON colon");
                }
                auto child = value(depth + 1);
                child.key = std::move(key);
                j.children.push_back(std::move(child));
                space();
                const char sep = take();
                if (sep == close) break;
                need(sep == ',', "missing JSON comma");
            }
            if (c == '{') {
                std::vector<std::string_view> keys;
                keys.reserve(j.children.size());
                for (const auto& child : j.children) keys.emplace_back(child.key);
                std::sort(keys.begin(), keys.end());
                need(std::adjacent_find(keys.begin(), keys.end()) == keys.end(), "duplicate JSON key");
            }
            return j;
        }
        if (c == '"') { j.type = Json::Type::string; j.text = string(); return j; }
        for (const auto token : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
            if (data_.substr(pos_, token.size()) == token) {
                j.type = token == "null" ? Json::Type::null : Json::Type::boolean;
                j.text = std::string(token); pos_ += token.size(); return j;
            }
        }
        const auto begin = pos_;
        if (data_[pos_] == '-') ++pos_;
        need(pos_ < data_.size(), "short JSON number");
        if (data_[pos_] == '0') ++pos_;
        else {
            need(data_[pos_] >= '1' && data_[pos_] <= '9', "invalid JSON token");
            while (pos_ < data_.size() && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
        }
        if (pos_ < data_.size() && data_[pos_] == '.') {
            const auto start = ++pos_;
            while (pos_ < data_.size() && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
            need(pos_ != start, "short JSON fraction");
        }
        if (pos_ < data_.size() && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < data_.size() && (data_[pos_] == '+' || data_[pos_] == '-')) ++pos_;
            const auto start = pos_;
            while (pos_ < data_.size() && data_[pos_] >= '0' && data_[pos_] <= '9') ++pos_;
            need(pos_ != start, "short JSON exponent");
        }
        j.type = Json::Type::numeric; j.text = std::string(data_.substr(begin, pos_ - begin));
        j.numeric = json_number(j.text);
        if (j.text.find_first_of(".eE") == std::string::npos) {
            std::int64_t integer = 0;
            const auto parsed = std::from_chars(j.text.data(), j.text.data() + j.text.size(), integer);
            need(parsed.ec == std::errc{} && parsed.ptr == j.text.data() + j.text.size()
                && integer >= -static_cast<std::int64_t>(L::safe) && integer <= static_cast<std::int64_t>(L::safe),
                "unsafe JSON integer token");
        }
        return j;
    }
};
void keys(const Json& j, std::initializer_list<std::string_view> expected) {
    need(j.type == Json::Type::object && j.children.size() == expected.size(), "wrong JSON object keys");
    std::size_t i = 0;
    for (auto key : expected) need(j.children[i++].key == key, "reordered JSON object keys");
}
const Json& member(const Json& j, std::string_view name) {
    need(j.type == Json::Type::object, "expected JSON object");
    for (const auto& child : j.children) if (child.key == name) return child;
    fail("missing JSON member " + std::string(name));
}
const std::string& text(const Json& j) {
    need(j.type == Json::Type::string, "expected JSON string"); return j.text;
}
std::int64_t integer(const Json& j, std::int64_t low, std::int64_t high) {
    need(j.type == Json::Type::numeric, "expected JSON integer");
    std::int64_t n = 0;
    const auto r = std::from_chars(j.text.data(), j.text.data() + j.text.size(), n);
    need(r.ec == std::errc{} && r.ptr == j.text.data() + j.text.size() && n >= low && n <= high,
         "out-of-domain JSON integer");
    return n;
}
Field field(std::string name, std::string type, std::string unit) {
    const bool nullable = !type.empty() && type.back() == '?';
    const auto base = std::string_view(type).substr(0, type.size() - (nullable ? 1 : 0));
    Kind kind = base == "i64" ? Kind::i64 : base == "u64" ? Kind::u64
        : base == "f64" ? Kind::f64 : base == "bool" ? Kind::boolean
        : base == "str" ? Kind::string : base == "hex64" ? Kind::hex64 : Kind::order;
    need(kind != Kind::order, "unknown wire type");
    return {std::move(name), std::move(type), std::move(unit), kind, nullable};
}
std::vector<Field> schema_fields(char tag, std::size_t size) {
    const L::FieldSpec* specs = nullptr;
    std::size_t count = 0;
    switch (tag) {
    case 'E': specs = L::equity; count = std::size(L::equity); break;
    case 'T': specs = L::trade; count = std::size(L::trade); break;
    case 'O': specs = L::output; count = std::size(L::output); break;
    case 'V': specs = L::event; count = std::size(L::event); break;
    case 'X': specs = L::trace; count = std::size(L::trace); break;
    case 'H': specs = L::hash; count = std::size(L::hash); break;
    case 'K': break;
    default: fail("invalid header tag");
    }
    need((tag == 'O' ? size >= count && size - count <= L::max_slots
        : tag == 'K' ? size <= L::max_slots : size == count), "wrong descriptor field count");
    std::vector<Field> out;
    out.reserve(size);
    for (std::size_t i = 0; i < count; ++i) out.push_back(field(specs[i].name, specs[i].type, specs[i].unit));
    for (std::size_t i = count; i < size; ++i)
        out.push_back(field(std::string(tag == 'O' ? "v" : "c") + std::to_string(i - count), "f64?", ""));
    return out;
}
Head parse_head(std::string json) {
    const auto j = JsonParser(json, true, true).parse();
    keys(j, {"schema","version","run_id","engine","records","format"});
    need(text(j.children[0]) == "pf-report-sink", "unknown schema");
    integer(j.children[1], L::version, L::version);
    Head h;
    h.schema = text(j.children[0]); h.run_id = text(j.children[2]); h.engine = text(j.children[3]);
    for (std::size_t i = 0; i < h.records.size(); ++i) h.records[i].tag = L::tags[i];
    const auto& records = j.children[4];
    need(records.type == Json::Type::array, "records is not an array");
    std::size_t previous = 0;
    bool first = true;
    for (const auto& rec : records.children) {
        keys(rec, {"type","fields","types","units"});
        const auto& tag = text(rec.children[0]);
        need(tag.size() == 1 && tag[0] != 'I', "invalid header type");
        const auto index = tag_index(tag[0]);
        need(first || index > previous, "repeated/out-of-order header type");
        previous = index; first = false;
        for (unsigned i = 1; i <= 3; ++i) need(rec.children[i].type == Json::Type::array, "descriptor list is not array");
        auto fields = schema_fields(tag[0], rec.children[1].children.size());
        need(rec.children[2].children.size() == fields.size() && rec.children[3].children.size() == fields.size(),
             "mismatched descriptor arrays");
        for (std::size_t c = 0; c < fields.size(); ++c) {
            need(text(rec.children[1].children[c]) == fields[c].name
                && text(rec.children[2].children[c]) == fields[c].type
                && text(rec.children[3].children[c]) == fields[c].unit, "fixed schema mismatch");
        }
        h.records[index].present = true; h.records[index].fields = std::move(fields);
    }
    need(h.records[0].present && h.records[1].present, "E/T descriptors required");
    need(h.records[2].present == h.records[3].present && h.records[3].present == h.records[4].present,
         "O/V/K must be selected together");
    auto& interleave = h.records.back();
    interleave.present = true;
    interleave.fields.push_back({"tag","order","",Kind::order,false});
    const auto& format = j.children[5];
    keys(format, {"byte_order","codec","level","block_records","block_bytes"});
    need(text(format.children[0]) == "le" && text(format.children[1]) == "zstd", "unknown byte order/format codec");
    h.level = static_cast<int>(integer(format.children[2], -7, 22));
    const auto& caps = format.children[3];
    need(caps.type == Json::Type::object, "block_records must be object");
    std::size_t cap = 0;
    for (auto& s : h.records) {
        if (!s.present) continue;
        need(cap < caps.children.size() && caps.children[cap].key == std::string(1, s.tag), "wrong cap keys/order");
        s.block_records = static_cast<std::uint32_t>(integer(caps.children[cap++], 1,
            s.tag == 'I' ? L::max_interleave : L::max_records));
        if (s.tag == 'K') need(s.block_records == 1, "K cap must be 1");
    }
    need(cap == caps.children.size(), "extra block cap");
    h.block_bytes = static_cast<std::uint32_t>(integer(format.children[4], L::min_block_bytes, L::max_column));
    // Reconstruct canonical v1 bytes, rather than editing the input lexemes.
    h.v1_header = "{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":" + escaped_string(h.run_id)
        + ",\"engine\":" + escaped_string(h.engine) + ",\"records\":[";
    bool comma = false;
    for (const auto& s : h.records) {
        if (!s.present || s.tag == 'I') continue;
        if (comma) h.v1_header += ',';
        comma = true;
        h.v1_header += "{\"type\":" + escaped_string(std::string(1, s.tag));
        for (unsigned part = 0; part < 3; ++part) {
            h.v1_header += part == 0 ? ",\"fields\":[" : part == 1 ? ",\"types\":[" : ",\"units\":[";
            for (std::size_t c = 0; c < s.fields.size(); ++c) {
                if (c) h.v1_header += ',';
                h.v1_header += escaped_string(part == 0 ? s.fields[c].name : part == 1 ? s.fields[c].type : s.fields[c].unit);
            }
            h.v1_header += ']';
        }
        h.v1_header += '}';
    }
    h.v1_header += "]}\n";
    need(h.v1_header.size() <= L::max_line, "canonical v1 header exceeds line cap");
    h.json = std::move(json);
    return h;
}
SinkFolds validate_summary(std::string_view bytes, std::uint64_t trades) {
    const auto j = JsonParser(bytes, true).parse();
    need(j.type == Json::Type::object, "summary must be an object");
    // APP Q2 default mapping/cap, section 9.1; selected nested members remain opaque.
    constexpr std::string_view required[] = {"engine","input","applied_inputs","applied_overrides",
        "applied_runtime","elapsed_seconds","summary","metrics","diagnostics","fingerprint","sink_folds"};
    for (auto key : required) {
        const auto& child = member(j, key);
        need(key == "engine" ? child.type == Json::Type::string
            : key == "elapsed_seconds" ? child.type == Json::Type::numeric
            : key == "fingerprint" ? child.type == Json::Type::object || child.type == Json::Type::null
            : child.type == Json::Type::object, "wrong required SUMMARY member type");
    }
    const auto& folds = member(j, "sink_folds");
    keys(folds, {"closed_count","open_at_end_count","open_at_end_pnl"});
    SinkFolds result;
    result.closed_count = static_cast<std::uint64_t>(integer(folds.children[0], 0, L::safe));
    result.open_at_end_count = static_cast<std::uint64_t>(integer(folds.children[1], 0, L::safe));
    need(folds.children[2].type == Json::Type::numeric, "open_at_end_pnl must be a finite number");
    result.open_at_end_pnl = folds.children[2].numeric;
    need(add(result.closed_count, result.open_at_end_count) == trades, "sink_folds counts differ from T count");
    for (const auto& child : j.children) {
        constexpr std::string_view payload[] = {"report_sink","trades","equity_curve","records","figure","figures",
            "marks","pdf","bars","series","events","output_rows","output_series","output_events","trace","hashes"};
        need(std::find(std::begin(payload), std::end(payload), child.key) == std::end(payload),
             "payload/self-referential SUMMARY member");
        if (child.key == "report_window" || child.key == "report_shape")
            need(child.type == Json::Type::object, "selected SUMMARY member must be an object");
        if (child.key == "outputs") {
            need(child.type == Json::Type::object, "outputs metadata is not an object");
            constexpr std::string_view output_keys[] = {"schema_version","message_format","manifest_sha256","manifest","constants","hlines"};
            need(child.children.size() == std::size(output_keys), "wrong outputs metadata keys");
            for (auto key : output_keys) (void)member(child, key);
        }
    }
    return result; // unknown additive metadata survives as the exact original bytes
}
} // namespace

ReaderError::ReaderError(Failure reason, const std::string& message) : std::runtime_error(message), failure_(reason) {}
std::uint64_t TableLocator::byte_length() const {
    return page_count ? add(multiply(page_count - 1, layout::page), last_page_bytes) : 0;
}
Bytes RandomAccessSource::readRange(std::uint64_t offset, std::size_t length,
                                   const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) fail("range cancelled", Failure::cancelled);
    auto result = readRange(offset, length);
    if (cancelled && cancelled()) fail("range cancelled", Failure::cancelled);
    return result;
}
std::int64_t Value::as_i64() const {
    need(!null && kind == Kind::i64, "value is not i64"); return signed_word(word);
}
double Value::as_f64() const {
    need(!null && kind == Kind::f64, "value is not f64"); return number(word);
}
bool Column::is_null(std::size_t row) const { return !nulls.empty() && ((nulls.at(row / 8) >> (row % 8)) & 1); }
Value Column::value(std::size_t row) const {
    Value v; v.kind = kind; v.null = is_null(row);
    if (v.null) return v;
    if (kind == Kind::string) {
        const auto ref = strings.at(row);
        v.text = arena.substr(ref.offset, ref.length);
    } else v.word = words.at(row);
    return v;
}
std::string record_json(const Record& record) {
    std::string out = "[\"" + std::string(1, record.tag) + "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto& v : record.values) {
        out += ',';
        if (v.null) out += "null";
        else switch (v.kind) {
        case Kind::i64: out += std::to_string(signed_word(v.word)); break;
        case Kind::u64: out += std::to_string(v.word); break;
        case Kind::f64: out += f64(number(v.word)); break;
        case Kind::boolean: out += v.word ? "true" : "false"; break;
        case Kind::string: out += escaped_string(v.text); break;
        case Kind::hex64:
            out += '"';
            for (int i = 15; i >= 0; --i) out += digits[(v.word >> (4 * i)) & 15];
            out += '"'; break;
        case Kind::order: fail("I is not a v1 record");
        }
        need(out.size() < L::max_line, "canonical v1 record exceeds line cap");
    }
    out += "]\n";
    need(out.size() <= L::max_line, "canonical v1 record exceeds line cap");
    return out;
}
struct BufferLease {
    std::shared_ptr<MemoryStats> stats;
    std::size_t bytes;
    BufferLease(std::shared_ptr<MemoryStats> s, std::size_t n) : stats(std::move(s)), bytes(n) {
        ++stats->live_decoded_blocks;
        stats->peak_decoded_blocks = std::max(stats->peak_decoded_blocks, stats->live_decoded_blocks);
        stats->live_decoded_bytes += bytes;
        stats->peak_decoded_bytes = std::max(stats->peak_decoded_bytes, stats->live_decoded_bytes);
    }
    ~BufferLease() { --stats->live_decoded_blocks; stats->live_decoded_bytes -= bytes; }
    void resize(std::size_t n) {
        stats->live_decoded_bytes -= bytes; bytes = n; stats->live_decoded_bytes += bytes;
        stats->peak_decoded_bytes = std::max(stats->peak_decoded_bytes, stats->live_decoded_bytes);
    }
};
DecodedBlock::DecodedBlock() = default;
DecodedBlock::~DecodedBlock() = default;
DecodedBlock::DecodedBlock(DecodedBlock&&) noexcept = default;
DecodedBlock& DecodedBlock::operator=(DecodedBlock&&) noexcept = default;
Record DecodedBlock::row(std::uint32_t local) const {
    need(local < entry.n, "block row outside range");
    Record r; r.tag = entry.tag; r.ordinal = entry.rec0 + local; r.values.reserve(columns.size());
    r.file_scope = file_scope;
    if (scope == VerificationScope::block) {
        r.values.resize(columns.size());
        for (const auto& c : columns) r.values[c.id] = c.value(local);
    } else for (const auto& c : columns) r.values.push_back(c.value(local));
    return r;
}

namespace {
std::uint64_t varint(std::string_view raw, std::size_t& pos) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        need(pos < raw.size(), "truncated varint");
        const auto b = static_cast<unsigned char>(raw[pos++]);
        result |= std::uint64_t(b & 127) << (7 * i);
        if (!(b & 128)) {
            need((i == 0 || b != 0) && result <= L::safe, "nonminimal/out-of-domain varint");
            return result;
        }
    }
    fail("varint exceeds eight bytes");
}
std::int64_t unzig(std::uint64_t value) {
    return (value & 1) ? -static_cast<std::int64_t>(value / 2) - 1 : static_cast<std::int64_t>(value / 2);
}
void delta_domain(std::int64_t value, bool unsigned_kind) {
    need(value > -L::delta_limit && value < L::delta_limit && (!unsigned_kind || value >= 0),
         "delta reconstructed value outside domain");
}
std::int64_t delta_step(std::int64_t previous, std::uint64_t gcd, std::int64_t q) {
    need(q > -L::delta_limit && q < L::delta_limit, "delta quotient outside domain");
    const auto magnitude = static_cast<std::uint64_t>(q < 0 ? -q : q);
    need(magnitude == 0 || gcd <= std::uint64_t(INT64_MAX) / magnitude, "delta multiplication overflow");
    const auto product = static_cast<std::int64_t>(gcd * magnitude);
    if (q >= 0) {
        need(previous <= INT64_MAX - product, "delta addition overflow");
        return previous + product;
    }
    need(previous >= INT64_MIN + product, "delta subtraction overflow");
    return previous - product;
}
void mode(Kind kind, unsigned enc) {
    need((kind == Kind::i64 || kind == Kind::u64) ? enc == L::plain64 || enc == L::delta
        : kind == Kind::f64 ? enc == L::xorshuf || enc == L::decimal
        : kind == Kind::hex64 ? enc == L::xorshuf
        : kind == Kind::string ? enc == L::dict || enc == L::plainstr : enc == L::bytes, "encoding not allowed for kind");
}

// RFC 8878 frame inspection uses no decoder/context/output allocation.
// It is also run in stored-only builds so oversized-window refusals do not
// depend on whether libzstd is installed.
struct ZstdHeader { std::size_t bytes; std::uint64_t window; };
ZstdHeader inspect_zstd_header(std::string_view frame, std::uint32_t raw_len,
                               std::size_t window_limit, Failure window_failure = Failure::malformed) {
    need(frame.size() >= 5 && le(frame, 0, 4) == UINT32_C(0xfd2fb528), "invalid zstd frame magic");
    const auto descriptor = static_cast<unsigned char>(frame[4]);
    // RFC 8878's unused bit 4 is ignored; reserved bit 3 must be zero.
    need((descriptor & 0x0b) == 0, "zstd dictionary/reserved bits forbidden");
    const bool single = (descriptor & 0x20) != 0;
    const unsigned size_flag = descriptor >> 6;
    std::size_t pos = 5;
    std::uint64_t window = 0;
    if (!single) {
        need(pos < frame.size(), "missing zstd window descriptor");
        const auto wd = static_cast<unsigned char>(frame[pos++]);
        const auto base = UINT64_C(1) << (10 + (wd >> 3));
        window = base + (base / 8) * (wd & 7);
    }
    const unsigned size_bytes = size_flag == 0 ? (single ? 1 : 0) : size_flag == 1 ? 2 : size_flag == 2 ? 4 : 8;
    need(size_bytes != 0, "zstd frame has no declared content size");
    auto content = le(frame, pos, size_bytes); pos += size_bytes;
    if (size_flag == 1) content += 256;
    // Sections 5.2/9.1: content-size refusal precedes window admission.
    need(content == raw_len, "zstd content size differs from raw_len");
    if (single) window = content;
    if (window > window_limit) fail("zstd window exceeds cap before allocation", window_failure);
    return {pos, window};
}
void inspect_zstd(std::string_view frame, std::uint32_t raw_len,
                  std::size_t window_limit = L::max_column, Failure window_failure = Failure::malformed) {
    const auto header = inspect_zstd_header(frame, raw_len, window_limit, window_failure);
    auto pos = header.bytes;
    const auto descriptor = static_cast<unsigned char>(frame[4]);
    bool last = false;
    while (!last) {
        const auto bh = le(frame, pos, 3); pos += 3;
        last = (bh & 1) != 0;
        const auto type = (bh >> 1) & 3;
        const auto size = bh >> 3;
        need(type != 3 && size <= 131072 && size <= std::max<std::uint64_t>(header.window, 1),
             "invalid zstd block header");
        const auto stored = type == 1 ? 1 : size;
        need(pos <= frame.size() && stored <= frame.size() - pos, "truncated zstd block");
        pos += host_size(stored);
    }
    if (descriptor & 4) {
        need(pos <= frame.size() && 4 <= frame.size() - pos, "truncated zstd checksum");
        pos += 4;
    }
    need(pos == frame.size(), "trailing/concatenated zstd frame");
}
Bytes decompress(Bytes stored, unsigned codec, std::uint32_t raw_len, MemoryStats& stats) {
    if (codec == 0) return stored;
    need(codec == 1, "unknown column codec");
    inspect_zstd(view(stored), raw_len);
#if defined(PINEFORGE_READER_WITH_ZSTD) && PINEFORGE_READER_WITH_ZSTD
    need(ZSTD_getFrameContentSize(stored.data(), stored.size()) == raw_len, "zstd library content-size mismatch");
    const auto frame_size = ZSTD_findFrameCompressedSize(stored.data(), stored.size());
    need(!ZSTD_isError(frame_size) && frame_size == stored.size(), "malformed zstd frame");
    Bytes raw(raw_len);
    stats.peak_column_scratch_bytes = std::max(stats.peak_column_scratch_bytes, stored.capacity() + raw.capacity());
    const auto got = ZSTD_decompress(raw.data(), raw.size(), stored.data(), stored.size());
    need(!ZSTD_isError(got) && got == raw_len, "zstd decoding failed/wrong output length");
    return raw;
#else
    (void)stats;
    fail("codec-1 column requires libzstd (PINEFORGE_WITH_ZSTD)", Failure::unsupported_codec);
#endif
}
void check_summary_lengths(std::uint64_t stored_len, std::uint64_t json_len) {
    // Section 5.2: these refusals precede even the bounded frame-header read.
    if (json_len > L::max_summary) fail("summary decoded length exceeds cap", Failure::summary_limit);
    if (stored_len > L::max_summary_stored) fail("summary stored length exceeds cap", Failure::summary_limit);
    need(stored_len >= 1, "summary stored length exceeds cap");
    need(json_len >= 1, "summary decoded length exceeds cap");
}
std::string decompress_summary(std::string_view frame, std::uint32_t json_len) {
    check_summary_lengths(frame.size(), json_len);
    inspect_zstd(frame, json_len, L::max_summary, Failure::summary_limit);
#if defined(PINEFORGE_READER_WITH_ZSTD) && PINEFORGE_READER_WITH_ZSTD
    need(ZSTD_getFrameContentSize(frame.data(), frame.size()) == json_len, "zstd library content-size mismatch");
    const auto frame_size = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    need(!ZSTD_isError(frame_size) && frame_size == frame.size(), "malformed zstd SUMMARY frame");
    // Every length and window is checked before this exact-size output buffer.
    // The destination capacity is the hard output limit, independent of FCS.
    std::string json(json_len, '\0');
    const auto got = ZSTD_decompress(&json[0], json.size(), frame.data(), frame.size());
    need(!ZSTD_isError(got) && got == json_len, "zstd SUMMARY decoding failed/wrong output length");
    return json;
#else
    fail("SUMMARY requires libzstd (PINEFORGE_WITH_ZSTD)", Failure::unsupported_codec);
#endif
}
template<class Hash> void canonical(Hash& hash, const Column& c, std::size_t row) {
    const bool is_null = c.is_null(row);
    if (c.nullable) fixed(hash, is_null ? 0 : 1, 1);
    if (is_null) return;
    if (c.kind == Kind::string) {
        const auto ref = c.strings[row];
        fixed(hash, ref.length, 4);
        hash.update(std::string_view(c.arena).substr(ref.offset, ref.length));
    } else fixed(hash, c.words[row], (c.kind == Kind::boolean || c.kind == Kind::order) ? 1 : 8);
}
Column decode(std::string_view raw, const Field& field, std::uint32_t id, unsigned enc,
              unsigned flags, std::uint32_t n, const Head& head,
              const std::function<void()>& poll, Column c = {}) {
    mode(field.kind, enc);
    need((flags & ~1u) == 0 && (!(flags & 1) || field.nullable), "illegal null flags");
    c.nulls.clear(); c.words.clear(); c.strings.clear(); c.arena.clear();
    c.id = id; c.kind = field.kind; c.nullable = field.nullable;
    std::size_t pos = 0, m = n;
    if (flags & 1) {
        const std::size_t nb = (n + 7) / 8;
        need(raw.size() >= nb, "short null bitmap");
        c.nulls.assign(raw.begin(), raw.begin() + nb); pos = nb;
        if (n % 8) need((c.nulls.back() >> (n % 8)) == 0, "nonzero unused null bits");
        for (std::uint32_t i = 0; i < n; ++i) if (c.is_null(i)) --m;
    }
    raw.remove_prefix(pos); pos = 0;
    if (c.kind == Kind::string) {
        c.strings.resize(n);
        const auto count = enc == L::dict ? varint(raw, pos) : m;
        need(count <= m, "dictionary larger than non-null population");
        std::vector<StringRef> entries(host_size(count));
        std::uint64_t total = 0;
        for (auto& ref : entries) {
            const auto length = varint(raw, pos);
            need(length <= L::max_string, "string exceeds cap");
            ref.offset = static_cast<std::uint32_t>(total); ref.length = static_cast<std::uint32_t>(length);
            total = add(total, length);
            need(total <= L::max_column, "string arena exceeds column cap");
        }
        need(pos <= raw.size() && total <= raw.size() - pos, "string lengths overrun body");
        c.arena.assign(raw.data() + pos, host_size(total)); pos += host_size(total);
        for (const auto& ref : entries) utf8(std::string_view(c.arena).substr(ref.offset, ref.length));
        if (enc == L::dict) {
            std::vector<std::string_view> unique;
            unique.reserve(entries.size());
            for (const auto& ref : entries) unique.push_back(std::string_view(c.arena).substr(ref.offset, ref.length));
            std::sort(unique.begin(), unique.end());
            need(std::adjacent_find(unique.begin(), unique.end()) == unique.end(), "duplicate dictionary string");
        }
        std::size_t next = 0;
        for (std::uint32_t row = 0; row < n; ++row) {
            poll();
            if (c.is_null(row)) continue;
            const auto index = enc == L::dict ? varint(raw, pos) : next;
            need(index < entries.size(), "dictionary index outside range");
            if (enc == L::dict) {
                need(index <= next, "dictionary not in first-occurrence order");
                if (index == next) ++next;
            } else ++next;
            c.strings[row] = entries[host_size(index)];
        }
        need(next == entries.size() && pos == raw.size(), "unused dictionary/trailing string bytes");
        return c;
    }
    c.words.resize(n);
    if (enc == L::plain64 || enc == L::xorshuf) need(raw.size() == 8 * m, "wrong PLAIN64/XORSHUF body length");
    if (enc == L::bytes) need(raw.size() == m, "wrong BYTES body length");
    std::int64_t delta = 0;
    std::uint64_t gcd = 1, previous = 0;
    unsigned k = 0;
    if (enc == L::delta || enc == L::decimal) {
        need(m != 0, "DELTA/DECIMAL requires non-null value");
        if (enc == L::decimal) {
            k = static_cast<unsigned>(le(raw, pos++, 1));
            need(k < L::pow10.size(), "DECIMAL scale exceeds 12");
        }
        delta = unzig(varint(raw, pos));
        gcd = varint(raw, pos); need(gcd >= 1, "zero DELTA GCD");
    }
    std::size_t j = 0;
    for (std::uint32_t row = 0; row < n; ++row) {
        poll();
        if (c.is_null(row)) continue;
        std::uint64_t word = 0;
        if (enc == L::plain64) { word = le(raw, pos, 8); pos += 8; }
        else if (enc == L::xorshuf) {
            for (unsigned plane = 0; plane < 8; ++plane)
                word |= std::uint64_t(static_cast<unsigned char>(raw[plane * m + j])) << (8 * plane);
            word ^= previous; previous = word;
        } else if (enc == L::bytes) word = le(raw, pos++, 1);
        else {
            if (j) delta = delta_step(delta, gcd, unzig(varint(raw, pos)));
            delta_domain(delta, c.kind == Kind::u64);
            if (enc == L::decimal) {
                need(delta >= -L::decimal_limit && delta <= L::decimal_limit, "DECIMAL integer outside domain");
                static_assert(FLT_EVAL_METHOD == 0, "DECIMAL requires binary64 evaluation without excess precision");
                need(std::fegetround() == FE_TONEAREST, "DECIMAL requires round-to-nearest ties-to-even");
                // 6.4: strict-FP compile options, one division, no reciprocal.
                word = bits(static_cast<double>(delta) / L::pow10[k]);
            } else word = static_cast<std::uint64_t>(delta);
        }
        if (c.kind == Kind::i64) {
            const auto v = signed_word(word);
            need(v >= -static_cast<std::int64_t>(L::safe) && v <= static_cast<std::int64_t>(L::safe), "i64 outside safe domain");
        } else if (c.kind == Kind::u64) need(word <= L::safe, "u64 outside safe domain");
        else if (c.kind == Kind::f64) need(finite_word(word), "non-finite stored f64");
        else if (c.kind == Kind::boolean) need(word <= 1, "invalid bool byte");
        else if (c.kind == Kind::order) {
            const auto t = tag_index(static_cast<char>(word));
            need(t != L::stream_count - 1 && head.records[t].present, "I contains absent/invalid type");
        }
        c.words[row] = word; ++j;
    }
    need(enc == L::xorshuf || pos == raw.size(), "trailing column body bytes");
    return c;
}
EquityWitness witness(std::string_view raw, const BlockEntry& entry) {
    EquityWitness w;
    w.ordinal = le(raw, L::Witness::ordinal, 8);
    w.time_ms = signed_word(le(raw, L::Witness::time, 8));
    w.mtm_bits = le(raw, L::Witness::mtm, 8); w.book_bits = le(raw, L::Witness::book, 8);
    w.valid_mask = static_cast<std::uint32_t>(le(raw, L::Witness::mask, 4));
    need(le(raw, L::Witness::reserved, 4) == 0 && w.valid_mask <= 3, "invalid witness mask/reserved");
    need(w.ordinal >= entry.rec0 && w.ordinal < add(entry.rec0, entry.n), "witness ordinal outside block");
    need(w.time_ms >= -static_cast<std::int64_t>(L::safe) && w.time_ms <= static_cast<std::int64_t>(L::safe), "witness time outside domain");
    need((w.valid_mask & 1) ? finite_word(w.mtm_bits) : w.mtm_bits == 0, "invalid MTM witness bits");
    need((w.valid_mask & 2) ? finite_word(w.book_bits) : w.book_bits == 0, "invalid book witness bits");
    return w;
}
SeriesSummary series(std::string_view raw, const BlockEntry& e, unsigned mask, bool population) {
    SeriesSummary s;
    s.finite_count = le(raw, L::Series::count, 4);
    s.flags = static_cast<std::uint32_t>(le(raw, L::Series::flags, 4));
    need(s.finite_count <= e.n && s.flags == (s.finite_count ? 15u : population ? 9u : 0u),
         "invalid E summary flags/count");
    zero(raw.substr(L::Series::padding));
    for (std::size_t i = 0; i < s.witnesses.size(); ++i) {
        const auto bytes = raw.substr(L::Series::witnesses + i * L::Series::witness_size, L::Series::witness_size);
        if (s.flags & (1u << i)) {
            s.witnesses[i] = witness(bytes, e);
            need(s.witnesses[i].valid_mask & 1, "null-equity summary witness");
            if (i == 1 || i == 2) need(s.witnesses[i].valid_mask & mask, "missing extreme witness value");
        } else zero(bytes);
    }
    if (s.flags) need(s.witnesses[0].ordinal <= s.witnesses[3].ordinal, "reversed E summary endpoints");
    return s;
}
Digest logical(const Digest& v1, const Digest& summary,
               const std::array<std::uint64_t, L::stream_count>& counts,
               const std::array<Digest, L::stream_count>& streams) {
    Sha256 h; h.update(L::logical_domain); h.update(digest_view(v1)); h.update(digest_view(summary));
    for (std::size_t i = 0; i < counts.size(); ++i) {
        fixed(h, static_cast<unsigned char>(L::tags[i]), 1); fixed(h, counts[i], 8);
        h.update(digest_view(streams[i]));
    }
    return h.digest();
}
} // namespace

// One bounded, reusable block/column workspace; used only during ingestion.
struct IncrementalDecode {
    Bytes stored, raw;
    std::vector<Column> columns;
};

struct ReaderState {
    std::shared_ptr<RandomAccessSource> source;
    ReaderOptions options;
    std::string generation;
    std::uint64_t file_bytes = 0, root_offset = 0, data_start = 0, data_end = 0;
    std::uint32_t nblocks = 0;
    std::size_t root_length = L::root;
    Profile profile = Profile::full;
    Bytes compact_descriptors;
    std::uint64_t finite_e_count = 0;
    SinkFolds folds;
    bool complete = false, stored_present = false;
    Head head;
    SummaryFrameInfo summary_frame_info;
    std::array<std::uint64_t, L::stream_count> counts{};
    std::array<Digest, L::stream_count> streams{};
    std::array<TableLocator, L::table_count> tables{};
    Digest logical_sha{}, stored_sha{}, summary_sha{}, v1_sha{};
    std::shared_ptr<MemoryStats> stats = std::make_shared<MemoryStats>();
    std::array<std::unique_ptr<IndexPage>, L::page_cache> cache;
    std::array<std::uint64_t, L::page_cache> ticks{};
    std::uint64_t tick = 0;
    IncrementalDecode* reuse = nullptr;

    void poll() const {
        if (options.cancelled && options.cancelled()) fail("cancelled/incomplete", Failure::cancelled);
        if (source->size() != file_bytes || source->generation() != generation)
            fail("source generation/size changed", Failure::source_changed);
    }
    VerificationScope scope() const noexcept {
        return complete ? VerificationScope::whole_file : VerificationScope::unverified;
    }
    template<class T> ReadResult<T> observe(T value) const {
        poll();
        return {value, scope()};
    }
    Bytes range(std::uint64_t offset, std::size_t length) const {
        poll(); need(offset <= file_bytes && length <= file_bytes - offset, "range outside file");
        Bytes bytes;
        try { bytes = source->readRange(offset, length, options.cancelled); }
        catch (const ReaderError&) { throw; }
        catch (const std::exception& e) { fail(std::string("range I/O: ") + e.what(), Failure::io); }
        poll();
        need(bytes.size() == length, "short readRange");
        return bytes;
    }
    void open();
    std::unique_ptr<ReaderState> isolated() const {
        // Copy immutable, bounded metadata only. Never share cache/stats or
        // an incremental decoder; source explicitly admits concurrent reads.
        auto out = std::make_unique<ReaderState>();
        out->source = source; out->options = options; out->generation = generation;
        out->file_bytes = file_bytes; out->root_offset = root_offset;
        out->data_start = data_start; out->data_end = data_end;
        out->nblocks = nblocks; out->root_length = root_length; out->profile = profile;
        out->compact_descriptors = compact_descriptors; out->finite_e_count = finite_e_count;
        out->folds = folds; out->stored_present = stored_present; out->head = head;
        out->summary_frame_info = summary_frame_info; out->counts = counts;
        out->streams = streams; out->tables = tables; out->logical_sha = logical_sha;
        out->stored_sha = stored_sha; out->summary_sha = summary_sha; out->v1_sha = v1_sha;
        return out;
    }
    const IndexPage& page(std::uint8_t table, std::uint32_t number);
    BlockEntry entry(std::uint8_t table, std::uint64_t ordinal);
    std::uint64_t find_record(std::uint8_t table, std::uint64_t record);
    std::uint64_t find_finite(std::uint64_t record);
    std::uint64_t position(std::uint8_t table, std::uint64_t rank);
    EquityBucket figure(std::uint64_t ordinal);
    std::vector<std::uint32_t> compact_positions(unsigned column, bool descending);
    EquityEnvelope scan_equity(std::uint64_t buckets,
        std::optional<std::array<std::int64_t, 2>> time_range, bool adjust_final,
        const std::function<void(EquityBucket&&)>& output = {});
    void report_json(const std::function<void(std::string_view)>& output);
    DecodedBlock block(const BlockEntry&, const std::vector<std::uint32_t>& ids, bool full);
    DecodedBlock block(const BlockEntry& e) {
        std::vector<std::uint32_t> ids(head.records[e.table].fields.size());
        std::iota(ids.begin(), ids.end(), 0u);
        return block(e, ids, true);
    }
};

void ReaderState::open() {
    need(file_bytes <= L::safe && file_bytes >= L::tail + L::compact_root + L::head_overhead + L::summary_header + 2,
         "file too small/large");
    need(std::fegetround() == FE_TONEAREST, "reader requires round-to-nearest ties-to-even");
    need(options.scratch_flags <= 2, "invalid local scratch flags");
    const auto suffix_length = host_size(std::min<std::uint64_t>(file_bytes,
        (options.trusted_compact ? L::compact_root : L::root) + L::tail));
    const auto suffix_offset = file_bytes - suffix_length;
    const auto suffix_bytes = range(suffix_offset, suffix_length);
    const auto tail = view(suffix_bytes).substr(suffix_length - L::tail);
    need(tail.substr(L::Tail::magic) == L::tail_magic, "missing/extra/bad tail");
    root_length = host_size(le(tail, L::Tail::root_length, 4));
    need(root_length == L::root || root_length == L::compact_root, "unknown root length");
    need(root_length + L::tail <= suffix_length, "selected root outside discovery suffix");
    const auto flags = le(tail, L::Tail::flags, 4);
    need((flags & ~UINT64_C(1)) == 0, "unknown tail flags");
    stored_present = flags & 1;
    zero(tail.substr(L::Tail::reserved, L::Tail::magic - L::Tail::reserved));
    stored_sha = digest_at(tail, L::Tail::stored_sha);
    if (!stored_present) zero(digest_view(stored_sha));
    root_offset = le(tail, L::Tail::root_offset, 8);
    need(add(add(root_offset, root_length), L::tail) == file_bytes
        && le(tail, L::Tail::file_bytes, 8) == file_bytes, "tail size/root pointer mismatch");
    logical_sha = digest_at(tail, L::Tail::logical_sha);
    const auto root = view(suffix_bytes).substr(suffix_length - L::tail - root_length, root_length);
    integrity(sha(root) == digest_at(tail, L::Tail::root_sha), "root SHA mismatch");
    const auto root_flags = le(root, L::Root::flags, 2);
    need(root.substr(0, L::root_magic.size()) == L::root_magic && le(root, L::Root::version, 2) == L::version
        && root_flags <= 1, "unknown root version/flags/magic");
    profile = root_flags ? Profile::compact : Profile::full;
    const bool compact = profile == Profile::compact;
    need(root_length == (compact ? L::compact_root : L::root), "profile flag/root length mismatch");
    const auto root_crc = compact ? L::CompactRoot::crc : L::Root::crc;
    integrity(crc(root.substr(0, root_crc)) == le(root, root_crc, 4), "root CRC mismatch");
    if (compact) zero(root.substr(L::CompactRoot::padding, L::CompactRoot::crc - L::CompactRoot::padding));
    else {
        zero(root.substr(L::Root::padding1, L::Root::tables - L::Root::padding1));
        zero(root.substr(L::Root::padding2, L::Root::crc - L::Root::padding2));
    }
    data_end = le(root, L::Root::data_end, 8);
    const auto summary_length = le(root, compact ? L::CompactRoot::summary_length : L::Root::summary_length, 4);
    // Use only the fixed SUM2 header until both length budgets pass. An
    // oversized J/C must win over a bogus root length/range, with no next read.
    const auto sum_bytes = range(data_end, L::summary_header);
    const auto sum = view(sum_bytes);
    need(sum.substr(0, L::summary_magic.size()) == L::summary_magic && le(sum, L::Summary::flags, 4) == 0,
         "invalid summary magic/flags");
    const auto stored_len = le(sum, L::Summary::stored, 4), json_len = le(sum, L::Summary::length, 4);
    check_summary_lengths(stored_len, json_len);
    need(summary_length > L::summary_header && summary_length <= L::max_summary_length,
         "summary length exceeds cap");
    need(summary_length == add(L::summary_header, stored_len), "summary root/header length mismatch");
    if (!compact) need(data_end == le(root, L::Root::summary_offset, 8), "missing/misplaced summary");
    const auto summary_end = add(data_end, summary_length);
    need(summary_end <= root_offset, "summary range overlaps root/outside file");
    nblocks = static_cast<std::uint32_t>(le(root, compact ? L::CompactRoot::blocks : L::Root::blocks, 4));
    summary_sha = digest_at(root, compact ? L::CompactRoot::summary_sha : L::Root::summary_sha);
    v1_sha = digest_at(root, compact ? L::CompactRoot::v1_sha : L::Root::v1_sha);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        counts[i] = le(root, (compact ? L::CompactRoot::counts : L::Root::counts) + 8 * i, 8);
        need(counts[i] <= L::safe, "count exceeds safe integer");
        if (i + 1 < counts.size()) total = add(total, counts[i]);
        if (!compact) streams[i] = digest_at(root, L::Root::streams + L::digest * i);
    }
    need(counts.back() == total && counts[4] <= 1, "I/K count mismatch");
    finite_e_count = le(root, compact ? L::CompactRoot::finite_count : L::Root::finite_count, 8);
    need(finite_e_count <= counts[0], "invalid finite E count");
    if (!compact) need(le(root, L::Root::figure_buckets, 4) == L::figure_buckets, "invalid default figure buckets");
    std::uint64_t next = summary_end, blocks = 0;
    if (compact) {
        need(counts[0] < L::compact_e_limit && counts[1] < L::compact_t_limit && nblocks <= L::compact_blocks,
             "COMPACT population/block ceiling exceeded");
        const auto offset = le(root, L::CompactRoot::descriptors_offset, 8);
        const auto length = le(root, L::CompactRoot::descriptors_length, 8);
        need(offset == next && length == multiply(nblocks, L::compact_descriptor)
            && add(offset, length) == root_offset, "compact descriptor table tiling/length mismatch");
        for (std::size_t i = 0; i < L::stream_count; ++i) {
            auto& table = tables[i];
            table.entry_count = le(root, L::CompactRoot::block_counts + 4 * i, 4);
            table.entry_width = L::compact_descriptor;
            table.offset = next;
            next = add(next, multiply(table.entry_count, L::compact_descriptor));
            blocks = add(blocks, table.entry_count);
            need((counts[i] == 0) == (table.entry_count == 0) && table.entry_count <= counts[i],
                 "compact descriptor/count mismatch");
        }
        need(blocks == nblocks && next == root_offset, "compact block-count mismatch");
        // Reuse every table byte already in the first suffix. At most one
        // additional discovery request fetches the missing prefix.
        const auto missing = offset < suffix_offset ? std::min(length, suffix_offset - offset) : 0;
        compact_descriptors.resize(host_size(length));
        if (missing) {
            const auto prefix = range(offset, host_size(missing));
            std::copy(prefix.begin(), prefix.end(), compact_descriptors.begin());
        }
        if (missing < length) {
            const auto cached = view(suffix_bytes).substr(host_size(offset + missing - suffix_offset), host_size(length - missing));
            std::copy(cached.begin(), cached.end(), compact_descriptors.begin() + static_cast<std::ptrdiff_t>(missing));
        }
        Sha256 h; h.update(L::compact_index_domain); h.update(view(compact_descriptors));
        integrity(h.digest() == digest_at(root, L::CompactRoot::descriptors_sha), "compact descriptor table SHA mismatch");
        stats->compact_descriptor_bytes = compact_descriptors.capacity();
    } else for (std::size_t i = 0; i < tables.size(); ++i) {
        const auto t = root.substr(L::Root::tables + i * L::Root::table_size, L::Root::table_size);
        auto& table = tables[i];
        table.offset = le(t, L::Table::offset, 8);
        table.page_count = static_cast<std::uint32_t>(le(t, L::Table::pages, 4));
        table.entry_width = static_cast<std::uint32_t>(le(t, L::Table::width, 4));
        table.entry_count = le(t, L::Table::count, 8);
        table.last_page_bytes = static_cast<std::uint32_t>(le(t, L::Table::last_bytes, 4));
        table.flags = static_cast<std::uint32_t>(le(t, L::Table::flags, 4));
        table.merkle_root = digest_at(t, L::Table::sha);
        need(table.entry_count <= L::safe && table.entry_width == (i < L::stream_count
             ? L::descriptor : i == L::figure_table ? L::figure_entry
             : counts[1] <= L::u32_position_count ? 4u : 8u), "invalid table width/count");
        const auto capacity = L::page_payload / table.entry_width;
        const auto pages = table.entry_count / capacity + (table.entry_count % capacity != 0);
        need(pages == table.page_count, "table page-count mismatch");
        if (table.entry_count == 0) {
            need(table.offset == 0 && table.page_count == 0 && table.last_page_bytes == 0 && table.flags == 0,
                 "nonzero empty-table locator/PRESENT");
            zero(digest_view(table.merkle_root));
        } else {
            need(table.offset == next, "table overlap/gap/order");
            const auto last_entries = table.entry_count - multiply(table.page_count - 1, capacity);
            need(table.flags == L::present, "missing/unknown table PRESENT flag");
            need(table.last_page_bytes == add(L::page_header, multiply(last_entries, table.entry_width)),
                 "last_page_bytes differs from entry arithmetic");
            next = add(next, table.byte_length());
        }
        if (i < L::stream_count) {
            blocks = add(blocks, table.entry_count);
            need(table.entry_count <= UINT32_MAX && (counts[i] == 0) == (table.entry_count == 0)
                && table.entry_count <= counts[i], "descriptor/count mismatch");
        } else if (i == L::figure_table)
            need(table.entry_count == std::min<std::uint64_t>(L::figure_buckets, finite_e_count),
                 "figure table length differs from finite E count");
        else need(table.entry_count == counts[1], "rank table length/PRESENT differs from T count");
    }
    need(next == root_offset && blocks == nblocks, "index tiling/block-count mismatch");
    const auto prefix_bytes = range(0, L::head_prefix);
    const auto prefix = view(prefix_bytes);
    need(std::equal(L::magic.begin(), L::magic.end(), prefix_bytes.begin()), "unknown head magic");
    const auto head_length = le(prefix, L::magic.size(), 4);
    need(head_length >= 1 && head_length <= L::max_head, "head length exceeds cap");
    data_start = add(L::head_overhead, head_length);
    need(data_start <= data_end && data_end <= root_offset, "overlapping head/data/summary");
    const auto json_bytes = range(L::head_prefix, host_size(head_length + 4));
    const auto data = view(json_bytes);
    Crc head_crc; head_crc.update(prefix); head_crc.update(data.substr(0, host_size(head_length)));
    integrity(head_crc.digest() == le(data, host_size(head_length), 4), "head CRC mismatch");
    head = parse_head(std::string(data.substr(0, host_size(head_length))));
    integrity(sha(head.v1_header) == v1_sha, "v1 header SHA mismatch");
    for (std::size_t i = 0; i < counts.size(); ++i) {
        need(head.records[i].present || counts[i] == 0, "absent type has records");
        need(counts[i] <= multiply(tables[i].entry_count, head.records[i].present ? head.records[i].block_records : 1),
             "record count exceeds descriptor capacity");
    }
    // Lengths and exact SUMMARY/table tiling are checked above. Inspect only
    // the bounded frame header before allocating/fetching the stored C bytes.
    const auto frame_offset = add(data_end, L::summary_header);
    const auto frame_header = range(frame_offset, host_size(std::min<std::uint64_t>(stored_len, L::zstd_header)));
    inspect_zstd_header(view(frame_header), static_cast<std::uint32_t>(json_len), L::max_summary, Failure::summary_limit);
    const auto frame = range(frame_offset, host_size(stored_len));
    auto json = decompress_summary(view(frame), static_cast<std::uint32_t>(json_len));
    integrity(crc(json) == le(sum, L::Summary::crc, 4), "summary CRC mismatch");
    integrity(sha(json) == summary_sha && digest_at(sum, L::Summary::sha) == summary_sha, "summary SHA mismatch");
    folds = validate_summary(json, counts[1]);
    std::copy_n(sum_bytes.begin(), summary_frame_info.magic.size(), summary_frame_info.magic.begin());
    summary_frame_info.stored_len = static_cast<std::uint32_t>(stored_len);
    summary_frame_info.json_len = static_cast<std::uint32_t>(json_len);
    summary_frame_info.json_crc = static_cast<std::uint32_t>(le(sum, L::Summary::crc, 4));
    summary_frame_info.flags = static_cast<std::uint32_t>(le(sum, L::Summary::flags, 4));
    summary_frame_info.json_sha256 = digest_at(sum, L::Summary::sha);
    head.summary = std::move(json);
    // Consistency only: root-declared stream digests are record-bound by verify().
    if (!compact) integrity(logical(v1_sha, summary_sha, counts, streams) == logical_sha, "logical SHA consistency mismatch on open");
    else for (std::uint8_t t = 0; t < L::stream_count; ++t) {
        std::uint64_t seen = 0, previous_end = data_start;
        for (std::uint64_t j = 0; j < tables[t].entry_count; ++j) {
            const auto e = entry(t, j);
            need(e.rec0 == seen && e.offset >= previous_end, "compact per-tag descriptor order mismatch");
            seen = add(seen, e.n); previous_end = add(e.offset, e.length);
        }
        need(seen == counts[t], "compact descriptor record totals mismatch");
    }
}

const IndexPage& ReaderState::page(std::uint8_t table_id, std::uint32_t number) {
    poll();
    need(profile == Profile::full, "COMPACT has no physical index pages");
    need(table_id < tables.size(), "unknown table");
    const auto& table = tables[table_id];
    need(number < table.page_count, "page outside table");
    for (std::size_t i = 0; i < cache.size(); ++i) {
        if (cache[i] && cache[i]->table == table_id && cache[i]->number == number) {
            ticks[i] = ++tick; return *cache[i];
        }
    }
    std::size_t slot = 0;
    for (std::size_t i = 0; i < cache.size(); ++i)
        if (!cache[i] || ticks[i] < ticks[slot]) slot = i;
    if (cache[slot]) { cache[slot].reset(); --stats->live_index_pages; }
    auto page = std::make_unique<IndexPage>();
    const auto length = number == table.page_count - 1 ? table.last_page_bytes : L::page;
    page->bytes = range(add(table.offset, multiply(number, L::page)), host_size(length));
    const auto p = view(page->bytes);
    unsigned depth = 0;
    for (std::uint64_t n = 1; n < table.page_count; n *= 2) ++depth;
    need(p.substr(0, L::index_magic.size()) == L::index_magic && le(p, L::Page::id, 1) == table_id
        && le(p, L::Page::flags, 1) == 0 && le(p, L::Page::reserved, 1) == 0
        && le(p, L::Page::number, 4) == number && le(p, L::Page::pages, 4) == table.page_count
        && le(p, L::Page::width, 4) == table.entry_width
        && le(p, L::Page::depth, 1) == depth && depth <= L::Page::max_depth, "index page framing mismatch");
    const auto capacity = L::page_payload / table.entry_width;
    const auto used = std::min<std::uint64_t>(capacity, table.entry_count - multiply(number, capacity));
    need(le(p, L::Page::used, 4) == used, "page used_entries mismatch");
    need(p.size() == L::page_header + multiply(used, table.entry_width),
         "short non-final page/incorrect final page length");
    zero(p.substr(L::Page::siblings + depth * L::digest, L::Page::crc - L::Page::siblings - depth * L::digest));
    Crc pc; pc.update(p.substr(0, L::Page::crc)); pc.update(p.substr(L::page_header));
    integrity(pc.digest() == le(p, L::Page::crc, 4), "index page CRC mismatch");
    Sha256 leaf; leaf.update(L::index_domain); leaf.update(p.substr(0, L::Page::leaf)); leaf.update(p.substr(L::page_header));
    auto computed = leaf.digest();
    integrity(computed == digest_at(p, L::Page::leaf), "index leaf SHA mismatch");
    for (unsigned level = 0; level < depth; ++level) {
        const auto sibling = p.substr(L::Page::siblings + level * L::digest, L::digest);
        Sha256 parent; parent.update(L::node_domain);
        if ((number >> level) & 1) { parent.update(sibling); parent.update(digest_view(computed)); }
        else { parent.update(digest_view(computed)); parent.update(sibling); }
        computed = parent.digest();
    }
    integrity(computed == table.merkle_root, "index Merkle proof mismatch");
    page->table = table_id; page->number = number;
    page->used_entries = static_cast<std::uint32_t>(used); page->entry_width = table.entry_width;
    cache[slot] = std::move(page); ticks[slot] = ++tick;
    ++stats->live_index_pages;
    stats->peak_index_pages = std::max(stats->peak_index_pages, stats->live_index_pages);
    return *cache[slot];
}

BlockEntry ReaderState::entry(std::uint8_t table, std::uint64_t ordinal) {
    poll();
    need(table < L::stream_count && ordinal < tables[table].entry_count, "descriptor ordinal outside table");
    const bool compact = profile == Profile::compact;
    std::string_view raw;
    if (compact) {
        const auto start = tables[table].offset - tables[0].offset + multiply(ordinal, L::compact_descriptor);
        raw = view(compact_descriptors).substr(host_size(start), L::compact_descriptor);
    } else {
        const auto& p = page(table, static_cast<std::uint32_t>(ordinal / L::descriptors_per_page));
        raw = view(p.bytes).substr(L::page_header + host_size(ordinal % L::descriptors_per_page) * L::descriptor, L::descriptor);
    }
    BlockEntry e; e.file_scope = scope();
    e.table = table; e.ordinal = ordinal;
    e.offset = le(raw, L::Entry::offset, 8); e.length = static_cast<std::uint32_t>(le(raw, L::Entry::length, 4));
    e.tag = static_cast<char>(le(raw, L::Entry::tag, 1)); e.flags = static_cast<std::uint8_t>(le(raw, L::Entry::flags, 1));
    e.n = static_cast<std::uint32_t>(le(raw, L::Entry::n, 4));
    e.decoded_bytes = static_cast<std::uint32_t>(le(raw, L::Entry::decoded, 4));
    e.rec0 = le(raw, L::Entry::rec0, 8);
    if (!compact) e.finite_prefix = le(raw, L::Entry::finite_prefix, 8);
    e.a_min = signed_word(le(raw, L::Entry::amin, 8)); e.a_max = signed_word(le(raw, L::Entry::amax, 8));
    e.b_min = signed_word(le(raw, L::Entry::bmin, 8)); e.b_max = signed_word(le(raw, L::Entry::bmax, 8));
    e.sha = digest_at(raw, L::Entry::sha);
    need(e.tag == L::tags[table] && head.records[table].present && e.n >= 1
        && e.n <= head.records[table].block_records && e.length >= L::bhead && e.length <= L::max_block
        && e.decoded_bytes >= L::bhead && e.decoded_bytes <= L::max_block, "invalid descriptor fields/caps");
    need(le(raw, L::Entry::reserved, 2) == 0 && (e.flags & ~7u) == 0, "invalid descriptor flags/reserved");
    if (!compact) zero(raw.substr(L::Entry::padding));
    else need(!(e.flags & 4), "COMPACT descriptor has E summary flag");
    need(e.offset >= data_start && add(e.offset, e.length) <= data_end
        && add(e.rec0, e.n) <= counts[table], "descriptor range outside data/count");
    if (table == 4) need(e.n == 1, "K n must be 1");
    const int columns[] = {L::a_column[table], L::b_column[table]};
    const std::int64_t lo[] = {e.a_min, e.b_min}, hi[] = {e.a_max, e.b_max};
    for (unsigned k = 0; k < 2; ++k) {
        if (e.flags & (1u << k)) {
            need(columns[k] >= 0 && lo[k] >= -static_cast<std::int64_t>(L::safe)
                && hi[k] <= static_cast<std::int64_t>(L::safe) && lo[k] <= hi[k], "invalid descriptor time range");
        } else {
            need(lo[k] == 0 && hi[k] == 0, "invalid absent time range");
            if (columns[k] >= 0) need(head.records[table].fields[static_cast<std::size_t>(columns[k])].nullable,
                                     "nonnullable time range missing");
        }
    }
    if (e.tag == 'E' && !compact) {
        need(e.flags & 4, "missing E summaries");
        const bool population = le(raw, L::Entry::mtm + L::Series::count, 4) != 0;
        e.mtm = series(raw.substr(L::Entry::mtm, L::Series::size), e, 1, population);
        e.book = series(raw.substr(L::Entry::book, L::Series::size), e, 2, population);
        need(e.book.finite_count <= e.mtm.finite_count
            && add(e.finite_prefix, e.mtm.finite_count) <= finite_e_count, "E finite prefix/count outside root");
        if (population) {
            need(e.mtm.witnesses[0].ordinal == e.book.witnesses[0].ordinal
                && e.mtm.witnesses[3].ordinal == e.book.witnesses[3].ordinal, "E series endpoints differ");
        }
    } else {
        need(!(e.flags & 4), "non-E summary flag");
        if (!compact) zero(raw.substr(L::Entry::mtm, L::Entry::padding - L::Entry::mtm));
    }
    return e;
}

std::uint64_t ReaderState::find_record(std::uint8_t table, std::uint64_t record) {
    need(table < L::stream_count && record < counts[table], "record ordinal outside stream");
    std::uint64_t lo = 0, hi = tables[table].entry_count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto e = entry(table, mid);
        if (add(e.rec0, e.n) <= record) lo = mid + 1; else hi = mid;
    }
    need(lo < tables[table].entry_count, "record missing from descriptors");
    const auto e = entry(table, lo);
    need(e.rec0 <= record && record < add(e.rec0, e.n), "gap/overlap in record descriptors");
    return lo;
}
std::uint64_t ReaderState::find_finite(std::uint64_t record) {
    need(record < finite_e_count, "finite equity ordinal outside stream");
    std::uint64_t lo = 0, hi = tables[0].entry_count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto e = entry(0, mid);
        if (add(e.finite_prefix, e.mtm.finite_count) <= record) lo = mid + 1; else hi = mid;
    }
    need(lo < tables[0].entry_count, "finite equity ordinal missing from descriptors");
    const auto e = entry(0, lo);
    need(e.finite_prefix <= record && record < add(e.finite_prefix, e.mtm.finite_count),
         "gap/overlap in finite equity prefixes");
    return lo;
}
std::uint64_t ReaderState::position(std::uint8_t table, std::uint64_t rank) {
    need(table >= L::stream_count && table < L::rank_table_end && rank < tables[table].entry_count, "rank outside table");
    const auto width = tables[table].entry_width;
    const auto capacity = L::page_payload / width;
    const auto& p = page(table, static_cast<std::uint32_t>(rank / capacity));
    const auto result = le(view(p.bytes), L::page_header + host_size(rank % capacity) * width, width);
    need(result < counts[1], "rank position outside T stream");
    return result;
}

DecodedBlock ReaderState::block(const BlockEntry& e, const std::vector<std::uint32_t>& ids, bool full) {
    poll();
    const auto& fields = head.records[e.table].fields;
    auto selected = ids;
    std::sort(selected.begin(), selected.end());
    need(std::adjacent_find(selected.begin(), selected.end()) == selected.end()
        && (selected.empty() || selected.back() < fields.size()), "invalid/duplicate requested column");
    const auto header_bytes = range(e.offset, L::bhead);
    const auto h = view(header_bytes);
    const auto ncols = le(h, L::Block::ncols, 4);
    need(h.substr(0, L::block_magic.size()) == L::block_magic && le(h, L::Block::tag, 1) == static_cast<unsigned char>(e.tag)
        && le(h, L::Block::flags, 1) == 0 && le(h, L::Block::reserved, 2) == 0
        && ncols == fields.size() && ncols <= L::max_columns
        && le(h, L::Block::n, 4) == e.n && le(h, L::Block::rec0, 8) == e.rec0
        && add(L::bhead, le(h, L::Block::body, 4)) == e.length, "block/header descriptor mismatch");
    const auto dir_length = multiply(ncols, L::directory);
    const auto framing_length = multiply(ncols, L::column_framing);
    need(add(L::bhead, framing_length) <= e.length, "directory/SHA table exceeds block before allocation");
    const auto framing_bytes = range(add(e.offset, L::bhead), host_size(framing_length));
    const auto dir = view(framing_bytes).substr(0, host_size(dir_length));
    const auto column_shas = view(framing_bytes).substr(host_size(dir_length));
    Crc hc; hc.update(h.substr(0, L::Block::crc)); hc.update(view(framing_bytes));
    integrity(hc.digest() == le(h, L::Block::crc, 4), "block header/directory CRC mismatch");
    Sha256 bh; bh.update(L::block_domain); fixed(bh, static_cast<unsigned char>(e.tag), 1);
    fixed(bh, e.rec0, 8); fixed(bh, e.n, 4); bh.update(column_shas);
    integrity(bh.digest() == e.sha, "column SHA table/block SHA mismatch");
    std::uint64_t stored_total = L::bhead + framing_length, raw_total = stored_total, decoded_budget = stored_total;
    std::vector<std::uint32_t> offsets;
    offsets.reserve(fields.size());
    for (std::size_t c = 0; c < fields.size(); ++c) {
        poll();
        const auto d = dir.substr(c * L::directory, L::directory);
        const auto codec = le(d, L::Dir::codec, 1), enc = le(d, L::Dir::enc, 1), flags = le(d, L::Dir::flags, 1);
        const auto raw = le(d, L::Dir::raw, 4), stored = le(d, L::Dir::stored, 4);
        need(codec <= 1 && le(d, L::Dir::reserved, 1) == 0, "unknown codec/directory reserved");
        mode(fields[c].kind, static_cast<unsigned>(enc));
        need((flags & ~UINT64_C(1)) == 0 && (!(flags & 1) || fields[c].nullable), "invalid null bitmap flags");
        need(raw <= L::max_column && stored <= L::max_block && (codec != 0 || raw == stored), "column length exceeds cap");
        offsets.push_back(static_cast<std::uint32_t>(stored_total));
        stored_total = add(stored_total, stored); raw_total = add(raw_total, raw);
        need(stored_total <= L::max_block && raw_total <= L::max_block, "aggregate block cap before allocation");
        const auto kind = fields[c].kind;
        const std::uint64_t base = (kind == Kind::boolean || kind == Kind::order) ? e.n
            : kind == Kind::string ? 3 + 6 * std::uint64_t(e.n) : 9 * std::uint64_t(e.n);
        need(base <= L::max_column, "decoded column budget before allocation");
        decoded_budget = add(decoded_budget, base);
        need(decoded_budget <= L::max_block, "decoded U budget before allocation");
    }
    need(stored_total == e.length && raw_total == e.decoded_bytes, "stored/decoded length mismatch");
    DecodedBlock result; result.entry = e; result.file_scope = scope();
    result.entry.file_scope = result.file_scope;
    result.lease_ = std::make_unique<BufferLease>(stats, 0);
    result.columns.reserve(ids.size());
    result.verified_columns.reserve(ids.size());
    std::size_t resident = result.columns.capacity() * sizeof(Column)
        + result.verified_columns.capacity() * sizeof(std::uint32_t);
    result.lease_->resize(resident);
    for (auto id : ids) {
        poll();
        const auto d = dir.substr(id * L::directory, L::directory);
        const auto start = add(e.offset, offsets[id]);
        const auto stored_len = static_cast<std::uint32_t>(le(d, L::Dir::stored, 4));
        const auto raw_len = static_cast<std::uint32_t>(le(d, L::Dir::raw, 4));
        Bytes ordinary_raw;
        std::string_view raw_view;
        if (reuse) {
            // Only the private local FdSource participates in this path.
            need(stored_len <= L::max_column, "incremental stored column exceeds writer cap");
            auto* local = dynamic_cast<FdSource*>(source.get());
            need(local != nullptr, "incremental decoder requires a local staging fd");
            poll(); local->read_into(start, stored_len, reuse->stored); poll();
            const auto codec = le(d, L::Dir::codec, 1);
            if (codec == 0) raw_view = view(reuse->stored);
            else {
                inspect_zstd(view(reuse->stored), raw_len);
#if defined(PINEFORGE_READER_WITH_ZSTD) && PINEFORGE_READER_WITH_ZSTD
                need(ZSTD_getFrameContentSize(reuse->stored.data(), reuse->stored.size()) == raw_len,
                     "zstd library content-size mismatch");
                const auto frame_size = ZSTD_findFrameCompressedSize(reuse->stored.data(), reuse->stored.size());
                need(!ZSTD_isError(frame_size) && frame_size == reuse->stored.size(), "malformed zstd frame");
                if (reuse->raw.capacity() < raw_len) Bytes(raw_len).swap(reuse->raw);
                else reuse->raw.resize(raw_len);
                const auto got = ZSTD_decompress(reuse->raw.data(), reuse->raw.size(),
                                                reuse->stored.data(), reuse->stored.size());
                need(!ZSTD_isError(got) && got == raw_len, "zstd decoding failed/wrong output length");
                raw_view = view(reuse->raw);
#else
                fail("incremental decoding requires zstd", Failure::unsupported_codec);
#endif
            }
        } else {
            auto stored = range(start, stored_len);
            stats->peak_column_scratch_bytes = std::max(stats->peak_column_scratch_bytes, stored.capacity());
            ordinary_raw = decompress(std::move(stored), static_cast<unsigned>(le(d, L::Dir::codec, 1)), raw_len, *stats);
            raw_view = view(ordinary_raw);
        }
        Column recycled;
        if (reuse && id < reuse->columns.size()) recycled = std::move(reuse->columns[id]);
        auto column = decode(raw_view, fields[id], id, static_cast<unsigned>(le(d, L::Dir::enc, 1)),
            static_cast<unsigned>(le(d, L::Dir::flags, 1)), e.n, head, [this] { poll(); }, std::move(recycled));
        if (column.kind == Kind::string) {
            std::uint64_t bytes = 0;
            for (std::uint32_t r = 0; r < e.n; ++r)
                if (!column.is_null(r)) bytes = add(bytes, column.strings[r].length);
            need(add(3 + 6 * std::uint64_t(e.n), bytes) <= L::max_column, "decoded string column budget exceeded");
            decoded_budget = add(decoded_budget, bytes);
            need(decoded_budget <= L::max_block, "decoded U string budget exceeded");
        }
        Crc cc; cc.update(L::col_domain); fixed(cc, static_cast<unsigned char>(e.tag), 1);
        fixed(cc, id, 4); fixed(cc, e.rec0, 8); fixed(cc, e.n, 4);
        Sha256 ch; ch.update(L::col_domain); fixed(ch, static_cast<unsigned char>(e.tag), 1);
        fixed(ch, id, 4); fixed(ch, e.rec0, 8); fixed(ch, e.n, 4);
        for (std::uint32_t r = 0; r < e.n; ++r) {
            poll(); canonical(cc, column, r); canonical(ch, column, r);
        }
        integrity(cc.digest() == le(d, L::Dir::crc, 4), "decoded column CRC mismatch");
        integrity(ch.digest() == digest_at(column_shas, id * L::digest), "decoded column SHA mismatch");
        if (e.tag == 'T' && id == 1)
            for (std::uint32_t r = 0; r < e.n; ++r) {
                const auto ref = column.strings[r];
                const auto side = std::string_view(column.arena).substr(ref.offset, ref.length);
                need(side == "long" || side == "short", "invalid T.side");
            }
        resident += column.nulls.capacity() + column.words.capacity() * sizeof(std::uint64_t)
            + column.strings.capacity() * sizeof(StringRef) + column.arena.capacity();
        result.columns.push_back(std::move(column));
        result.verified_columns.push_back(id);
        result.lease_->resize(resident);
    }
    if (full) {
        need(ids.size() == fields.size(), "incomplete block requested as full");
        result.scope = VerificationScope::block; // every authenticated column hash was recomputed
        for (std::uint32_t r = 0; r < e.n; ++r) {
            poll();
            if (e.tag != 'I') (void)record_json(result.row(r));
        }
    }
    return result;
}

namespace {
void record_rules(const Record& r, std::optional<std::int64_t>& previous_time) {
    if (r.tag == 'E') {
        const auto now = r.values[0].as_i64();
        need(!previous_time || now >= *previous_time, "E time decreases");
        previous_time = now;
    } else if (r.tag == 'T') {
        need(r.values[0].as_i64() == static_cast<std::int64_t>(r.ordinal + 1), "T.n is not ordinal+1");
        need(r.values[1].text == "long" || r.values[1].text == "short", "invalid T.side");
    } else if (r.tag == 'O')
        need(r.values[0].as_i64() == static_cast<std::int64_t>(r.ordinal), "O.row is not ordinal");
}
bool same(const EquityWitness& a, const EquityWitness& b) {
    return a.ordinal == b.ordinal && a.time_ms == b.time_ms && a.mtm_bits == b.mtm_bits
        && a.book_bits == b.book_bits && a.valid_mask == b.valid_mask;
}
bool same(const SeriesSummary& a, const SeriesSummary& b) {
    if (a.finite_count != b.finite_count || a.flags != b.flags) return false;
    for (std::size_t i = 0; i < a.witnesses.size(); ++i)
        if (!same(a.witnesses[i], b.witnesses[i])) return false;
    return true;
}
double series_number(const EquityWitness& w, bool book) { return number(book ? w.book_bits : w.mtm_bits); }
EquityWitness sample(const DecodedBlock& block, std::uint32_t row, const SinkFolds* final_adjustment = nullptr) {
    EquityWitness w;
    w.ordinal = block.entry.rec0 + row;
    w.time_ms = signed_word(block.columns[0].words[row]);
    if (!block.columns[1].is_null(row)) {
        w.valid_mask |= 1; w.mtm_bits = block.columns[1].words[row];
        double profit = block.columns[2].is_null(row) ? 0.0 : number(block.columns[2].words[row]);
        if (final_adjustment && final_adjustment->open_at_end_count && std::abs(profit) <= 1e-9)
            profit = final_adjustment->open_at_end_pnl;
        const double book = number(w.mtm_bits) - profit; // one binary64 subtraction
        if (std::isfinite(book)) { w.valid_mask |= 2; w.book_bits = bits(book); }
    }
    return w;
}
void accumulate(SeriesSummary& s, const EquityWitness& w, bool book) {
    if (!(w.valid_mask & 1)) return; // null equity is outside BOTH series' population
    if (!(s.flags & 1)) { s.witnesses[0] = w; s.flags |= 1; }
    s.witnesses[3] = w; s.flags |= 8;
    if (!(w.valid_mask & (book ? 2u : 1u))) return;
    const auto value = series_number(w, book);
    if (!s.finite_count || value < series_number(s.witnesses[1], book)
        || (value == series_number(s.witnesses[1], book) && w.ordinal < s.witnesses[1].ordinal))
        s.witnesses[1] = w;
    if (!s.finite_count || value > series_number(s.witnesses[2], book)
        || (value == series_number(s.witnesses[2], book) && w.ordinal < s.witnesses[2].ordinal))
        s.witnesses[2] = w;
    s.flags |= 6; s.finite_count = add(s.finite_count, 1);
}
void compose(SeriesSummary& a, const SeriesSummary& b, bool book) {
    if (!(b.flags & 1)) return;
    if (!(a.flags & 1)) { a = b; return; }
    a.witnesses[3] = b.witnesses[3];
    if (b.finite_count) {
        for (unsigned i : {1u, 2u}) {
            const auto bv = series_number(b.witnesses[i], book);
            const auto av = series_number(a.witnesses[i], book);
            if (!a.finite_count || (i == 1 ? bv < av : bv > av)
                || (bv == av && b.witnesses[i].ordinal < a.witnesses[i].ordinal))
                a.witnesses[i] = b.witnesses[i];
        }
        a.flags |= 6;
    }
    a.flags |= 9; a.finite_count = add(a.finite_count, b.finite_count);
}
void verify_derived(const DecodedBlock& block) {
    const auto& e = block.entry;
    const auto table = e.table;
    for (unsigned k = 0; k < 2; ++k) {
        const auto col = k == 0 ? L::a_column[table] : L::b_column[table];
        if (col < 0) continue;
        const auto& c = block.columns[static_cast<std::size_t>(col)];
        std::optional<std::int64_t> low, high;
        for (std::uint32_t row = 0; row < e.n; ++row) {
            if (c.is_null(row)) continue;
            const auto value = signed_word(c.words[row]);
            if (!low || value < *low) low = value;
            if (!high || value > *high) high = value;
        }
        integrity(bool(e.flags & (1u << k)) == low.has_value(), "time-range validity mismatch");
        if (low) integrity(*low == (k == 0 ? e.a_min : e.b_min)
            && *high == (k == 0 ? e.a_max : e.b_max), "descriptor time-range mismatch");
    }
    if (e.tag == 'E' && (e.flags & 4)) {
        SeriesSummary mtm, book;
        for (std::uint32_t row = 0; row < e.n; ++row) {
            const auto w = sample(block, row); accumulate(mtm, w, false); accumulate(book, w, true);
        }
        integrity(same(mtm, e.mtm) && same(book, e.book), "E witness summary mismatch");
    }
}
// A private commitment, not a new wire hash. Both sides fold the same ordered
// descriptor claims; the producer side derives them from DECODED records.
void commit_descriptor(Sha256& hash, const BlockEntry& e, bool full_e) {
    fixed(hash, e.offset, 8); fixed(hash, e.length, 4);
    fixed(hash, static_cast<unsigned char>(e.tag), 1);
    fixed(hash, e.flags & (full_e ? 7u : 3u), 1);
    fixed(hash, e.n, 4); fixed(hash, e.decoded_bytes, 4); fixed(hash, e.rec0, 8);
    fixed(hash, static_cast<std::uint64_t>(e.a_min), 8);
    fixed(hash, static_cast<std::uint64_t>(e.a_max), 8);
    fixed(hash, static_cast<std::uint64_t>(e.b_min), 8);
    fixed(hash, static_cast<std::uint64_t>(e.b_max), 8);
    hash.update(digest_view(e.sha));
    if (!full_e) return;
    fixed(hash, e.finite_prefix, 8);
    for (const auto* series : {&e.mtm, &e.book}) {
        fixed(hash, series->finite_count, 4); fixed(hash, series->flags, 4);
        for (const auto& w : series->witnesses) {
            fixed(hash, w.ordinal, 8); fixed(hash, static_cast<std::uint64_t>(w.time_ms), 8);
            fixed(hash, w.mtm_bits, 8); fixed(hash, w.book_bits, 8); fixed(hash, w.valid_mask, 4);
        }
    }
}
struct SortKey {
    bool null = true;
    double value = 0;
    std::uint64_t position = 0;
};
SortKey sort_key(const DecodedBlock& block, std::uint32_t row) {
    const auto& c = block.columns[0]; // a verified single-key-column read
    double value = c.is_null(row) ? 0.0 : c.kind == Kind::i64
        ? static_cast<double>(signed_word(c.words[row])) : number(c.words[row]);
    if (c.id == L::rank_columns.back()) value = -std::abs(value);
    if (value == 0.0) value = 0.0; // comparison only; retain every original wire bit
    return {c.is_null(row), value, block.entry.rec0 + row};
}
bool before(const SortKey& a, const SortKey& b, bool descending) {
    if (a.null != b.null) return !a.null;
    if (!a.null && a.value != b.value) return descending ? a.value > b.value : a.value < b.value;
    return a.position < b.position; // n=p+1, so this also enforces the n tie break
}
std::uint64_t first_e_block(ReaderState& state, std::int64_t t) {
    std::uint64_t lo = 0, hi = state.tables[0].entry_count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (state.entry(0, mid).a_max < t) lo = mid + 1; else hi = mid;
    }
    return lo;
}
std::uint64_t lower_e_finite_time(ReaderState& state, std::int64_t t) {
    const auto ordinal = first_e_block(state, t);
    if (ordinal == state.tables[0].entry_count) return state.finite_e_count;
    auto block = state.block(state.entry(0, ordinal), {0,1}, false);
    auto finite = block.entry.finite_prefix;
    for (std::uint32_t row = 0; row < block.entry.n && signed_word(block.columns[0].words[row]) < t; ++row) {
        state.poll();
        if (!block.columns[1].is_null(row)) finite = add(finite, 1);
    }
    return finite;
}
void finish_bucket(EquityBucket& bucket) {
    for (const auto* s : {&bucket.mtm, &bucket.book})
        for (std::size_t k = 0; k < s->witnesses.size(); ++k)
            if (s->flags & (1u << k)) bucket.points.push_back(s->witnesses[k]);
    std::sort(bucket.points.begin(), bucket.points.end(), [](const EquityWitness& a, const EquityWitness& b) {
        return a.ordinal < b.ordinal;
    });
    bucket.points.erase(std::unique(bucket.points.begin(), bucket.points.end(),
        [](const EquityWitness& a, const EquityWitness& b) { return a.ordinal == b.ordinal; }), bucket.points.end());
}
} // namespace

EquityBucket ReaderState::figure(std::uint64_t ordinal) {
    const auto& table = tables[L::figure_table];
    need(profile == Profile::full && ordinal < table.entry_count, "figure ordinal outside FULL table");
    const auto capacity = L::page_payload / L::figure_entry;
    const auto& p = page(L::figure_table, static_cast<std::uint32_t>(ordinal / capacity));
    const auto raw = view(p.bytes).substr(L::page_header + host_size(ordinal % capacity) * L::figure_entry, L::figure_entry);
    const auto count = le(raw, L::Figure::count, 1);
    need(count >= 1 && count <= L::Figure::slots, "invalid figure witness count");
    zero(raw.substr(L::Figure::reserved, L::Figure::selectors - L::Figure::reserved));
    EquityBucket bucket;
    const auto boundary = [&](std::uint64_t j) {
        return add(multiply(finite_e_count / table.entry_count, j),
            multiply(finite_e_count % table.entry_count, j) / table.entry_count);
    };
    bucket.begin = boundary(ordinal); bucket.end = boundary(ordinal + 1);
    BlockEntry population; population.rec0 = 0;
    // witness() checks its range against rec0+n (u32). FULL can exceed u32,
    // so parse each witness with a one-record range at its safe raw ordinal.
    for (std::size_t i = 0; i < count; ++i) {
        const auto bytes = raw.substr(L::Figure::witnesses + i * L::Series::witness_size, L::Series::witness_size);
        population.rec0 = le(bytes, L::Witness::ordinal, 8); population.n = 1;
        need(population.rec0 < counts[0], "figure raw ordinal outside E");
        const auto w = witness(bytes, population);
        need((w.valid_mask & 1) && (bucket.points.empty() || bucket.points.back().ordinal < w.ordinal),
             "figure witnesses not finite/sorted/unique");
        bucket.points.push_back(w);
    }
    zero(raw.substr(L::Figure::witnesses + host_size(count) * L::Series::witness_size));
    unsigned used = 0;
    for (unsigned s = 0; s < 2; ++s) {
        auto& series = s ? bucket.book : bucket.mtm;
        series.finite_count = s ? 0 : bucket.end - bucket.begin;
        series.finite_count_known = s == 0;
        for (unsigned k = 0; k < 4; ++k) {
            const auto selector = le(raw, L::Figure::selectors + 4 * s + k, 1);
            if (selector == L::Figure::absent) {
                need(s == 1 && (k == 1 || k == 2), "missing required figure selector");
                continue;
            }
            need(selector < count, "figure selector outside witnesses");
            const auto& w = bucket.points[host_size(selector)];
            if (k == 1 || k == 2) need(w.valid_mask & (s ? 2 : 1), "null figure extreme");
            series.flags |= 1u << k; series.witnesses[k] = w;
            used |= 1u << selector;
        }
        need(series.flags == 15 || (s == 1 && series.flags == 9), "inconsistent figure extrema selectors");
        if (series.flags == 9) series.finite_count_known = true;
        need(same(series.witnesses[0], bucket.points.front()) && same(series.witnesses[3], bucket.points.back()),
             "figure endpoint selector mismatch");
    }
    need(used == (1u << count) - 1, "unused figure witness");
    return bucket;
}

std::vector<std::uint32_t> ReaderState::compact_positions(unsigned column, bool descending) {
    need(profile == Profile::compact && counts[1] < L::compact_t_limit, "compact rank ceiling");
    const auto n = host_size(counts[1]);
    std::vector<std::uint64_t> values(n);
    Bytes nulls(n);
    std::vector<std::uint32_t> positions(n), scratch(n);
    const auto bytes = values.capacity() * sizeof(std::uint64_t) + nulls.capacity()
        + (positions.capacity() + scratch.capacity()) * sizeof(std::uint32_t);
    stats->peak_rank_scratch_bytes = std::max(stats->peak_rank_scratch_bytes, bytes);
    std::uint64_t seen = 0;
    for (std::uint64_t j = 0; j < tables[1].entry_count; ++j) {
        auto b = block(entry(1, j), {column}, false);
        need(b.entry.rec0 == seen, "compact rank rec0 discontinuity");
        for (std::uint32_t row = 0; row < b.entry.n; ++row) {
            poll(); const auto key = sort_key(b, row);
            const auto at = host_size(key.position);
            values[at] = bits(key.value); nulls[at] = key.null;
            positions[at] = static_cast<std::uint32_t>(at);
        }
        seen = add(seen, b.entry.n);
    }
    need(seen == counts[1], "compact rank record total mismatch");
    auto less = [&](std::uint32_t a, std::uint32_t b) {
        poll();
        return before({nulls[a] != 0, number(values[a]), a}, {nulls[b] != 0, number(values[b]), b}, descending);
    };
    // Bottom-up merge sort has a fixed second index buffer and <=T*ceil(log2 T)
    // comparisons. No retained row objects, implementation-dependent sort arena,
    // or scan on a FULL-table miss.
    for (std::size_t width = 1; width < n; width *= 2) {
        for (std::size_t lo = 0; lo < n; lo += 2 * width) {
            const auto mid = std::min(lo + width, n), end = std::min(lo + 2 * width, n);
            auto a = lo, b = mid;
            for (auto out = lo; out < end; ++out) {
                poll();
                scratch[out] = b == end || (a < mid && less(positions[a], positions[b])) ? positions[a++] : positions[b++];
            }
        }
        positions.swap(scratch);
    }
    return positions;
}

EquityEnvelope ReaderState::scan_equity(std::uint64_t requested,
        std::optional<std::array<std::int64_t, 2>> time_range, bool adjust_final,
        const std::function<void(EquityBucket&&)>& output) {
    auto matches = [&](std::int64_t t) { return !time_range || (t >= (*time_range)[0] && t < (*time_range)[1]); };
    std::uint64_t population = finite_e_count, begin = 0;
    if (time_range) {
        population = 0;
        std::uint64_t finite = 0;
        for (std::uint64_t j = 0; j < tables[0].entry_count; ++j) {
            auto b = block(entry(0, j), {0,1,2}, false);
            for (std::uint32_t row = 0; row < b.entry.n; ++row) {
                poll(); if (b.columns[1].is_null(row)) continue;
                if (matches(signed_word(b.columns[0].words[row]))) {
                    if (!population) begin = finite;
                    ++population;
                }
                ++finite;
            }
        }
        integrity(finite == finite_e_count, "finite E count mismatch");
    }
    EquityEnvelope out; out.population = population; out.scope = scope();
    const auto buckets = std::min(requested, population);
    if (!buckets) return out;
    if (!output) out.buckets.reserve(host_size(buckets));
    std::uint64_t carry = 0, finite = 0, accepted = 0, raw_seen = 0, emitted = 0;
    const auto q = population / buckets, remainder = population % buckets;
    auto next_width = [&] {
        auto width = q;
        if (carry >= buckets - remainder) { ++width; carry -= buckets - remainder; }
        else carry += remainder;
        return width;
    };
    EquityBucket bucket; bucket.begin = begin; bucket.end = add(begin, next_width());
    for (std::uint64_t j = 0; j < tables[0].entry_count; ++j) {
        auto b = block(entry(0, j), {0,1,2}, false);
        need(b.entry.rec0 == raw_seen, "equity scan rec0 discontinuity");
        for (std::uint32_t row = 0; row < b.entry.n; ++row) {
            poll(); if (b.columns[1].is_null(row)) continue;
            need(finite < finite_e_count, "finite E count exceeds root");
            const bool final = ++finite == finite_e_count;
            if (!matches(signed_word(b.columns[0].words[row]))) continue;
            const auto w = sample(b, row, adjust_final && final ? &folds : nullptr);
            accumulate(bucket.mtm, w, false); accumulate(bucket.book, w, true);
            if (add(begin, ++accepted) == bucket.end) {
                finish_bucket(bucket);
                const auto end = bucket.end;
                if (output) output(std::move(bucket)); else out.buckets.push_back(std::move(bucket));
                if (++emitted < buckets) {
                    bucket = {}; bucket.begin = end; bucket.end = add(end, next_width());
                }
            }
        }
        raw_seen = add(raw_seen, b.entry.n);
    }
    integrity(finite == finite_e_count && raw_seen == counts[0] && accepted == population && emitted == buckets,
              "finite equity scan population mismatch");
    return out;
}

struct IncrementalValidator::State {
    ReaderState reader;
    IncrementalDecode workspace;
    IncrementalProof proof;
    std::array<std::vector<Digest>, 7> expected;
    std::array<std::size_t, 7> used{};
    std::vector<char> order;
    std::size_t order_used = 0;
    std::array<Sha256, 7> input_lines, decoded_lines;
    std::array<Sha256, L::stream_count> streams, descriptors;
    Sha256 full_e, stored;
    std::optional<std::int64_t> previous_time;
    std::string input_header;
    std::string_view pending;
    bool initialized = false;

    State(const BlockPolicy& policy, std::string_view header) : input_header(header) {
        for (std::size_t t = 0; t < L::stream_count; ++t) {
            const auto cap = policy.block_records[t];
            need(cap && cap <= (t == 7 ? L::max_interleave : L::max_records),
                 "incremental record cap outside domain");
            if (t < 7) expected[t].resize(cap);
            else order.resize(cap);
            streams[t].update(L::stream_domain);
            fixed(streams[t], static_cast<unsigned char>(L::tags[t]), 1);
        }
        reader.reuse = &workspace;
    }
    void source(int fd) {
        reader.source = std::make_shared<FdSource>(fd);
        reader.file_bytes = reader.source->size();
        reader.generation = reader.source->generation();
        if (initialized) return;
        const auto prefix = reader.range(0, L::head_prefix);
        need(std::equal(L::magic.begin(), L::magic.end(), prefix.begin()), "incremental HEAD magic");
        const auto n = le(view(prefix), L::magic.size(), 4);
        need(n >= 1 && n <= L::max_head, "incremental HEAD length");
        const auto data = reader.range(L::head_prefix, host_size(n + 4));
        Crc crc; crc.update(view(prefix)); crc.update(view(data).substr(0, host_size(n)));
        integrity(crc.digest() == le(view(data), host_size(n), 4), "incremental HEAD CRC");
        reader.head = parse_head(std::string(view(data).substr(0, host_size(n))));
        integrity(reader.head.v1_header == input_header, "incremental input header mismatch");
        proof.data_start = proof.data_end = L::head_overhead + n;
        initialized = true;
    }
};

IncrementalValidator::IncrementalValidator(const BlockPolicy& policy, std::string_view header)
    : state_(std::make_unique<State>(policy, header)) {}
IncrementalValidator::~IncrementalValidator() = default;
void IncrementalValidator::input_line(std::string_view line) {
    integrity(state_->pending.empty(), "incremental input was not admitted");
    state_->pending = line;
}
void IncrementalValidator::admitted(char tag) {
    auto& s = *state_;
    const auto t = tag_index(tag);
    integrity(t < 7 && s.pending.size() >= 6 && s.pending.substr(0, 2) == "[\""
        && s.pending[2] == tag && s.pending[3] == '"' && s.pending.back() == '\n',
        "incremental input tag mismatch");
    integrity(s.used[t] < s.expected[t].size() && s.order_used < s.order.size(),
              "incremental current-block snapshot overflow");
    s.expected[t][s.used[t]++] = sha(s.pending);
    s.input_lines[t].update(s.pending);
    s.order[s.order_used++] = s.pending[2]; // actual consumed tag, not a builder value
    s.pending = {};
}
void IncrementalValidator::block_written(int fd, std::uint64_t offset, std::uint32_t length,
                                         const std::array<std::string_view, 4>& encoded) {
    auto& s = *state_;
    try {
        s.source(fd);
        integrity(offset == s.proof.data_end && length <= L::max_block,
                  "incremental block tiling mismatch");
        need(encoded[0].size() == L::bhead, "incremental encoded header size");
        const auto t = tag_index(encoded[0][L::Block::tag]);
        BlockEntry e;
        e.table = static_cast<std::uint8_t>(t); e.tag = L::tags[t];
        e.offset = offset; e.length = length;
        e.n = static_cast<std::uint32_t>(t == 7 ? s.order_used : s.used[t]);
        e.rec0 = s.proof.counts[t];
        integrity(e.n != 0, "incremental block has no input records");
        const auto cols = s.reader.head.records[t].fields.size();
        need(cols <= L::max_columns && L::bhead + cols * L::column_framing <= length,
             "incremental framing exceeds block");
        need(encoded[1].size() == cols * L::directory && encoded[2].size() == cols * L::digest,
             "incremental encoded directory/SHA size");
        std::uint64_t decoded = L::bhead + cols * L::column_framing;
        for (std::size_t c = 0; c < cols; ++c)
            decoded = add(decoded, le(encoded[1], c * L::directory + L::Dir::raw, 4));
        need(decoded <= L::max_block, "incremental decoded block cap");
        e.decoded_bytes = static_cast<std::uint32_t>(decoded);
        // Expected framing comes from the bytes passed to Output::write;
        // the decoder rereads the inode and must match it. Record counts and
        // rec0 come from independent input admissions, not the stored header.
        Sha256 bh; bh.update(L::block_domain); fixed(bh, static_cast<unsigned char>(e.tag), 1);
        fixed(bh, e.rec0, 8); fixed(bh, e.n, 4);
        bh.update(encoded[2]); e.sha = bh.digest();
        std::vector<std::uint32_t> ids(cols); std::iota(ids.begin(), ids.end(), 0u);
        // The same reader decoder and CRC/SHA/domain checks, without the
        // full-reader formatting walk (we format and compare exactly once).
        auto block = s.reader.block(e, ids, false);
        block.scope = VerificationScope::block;
        e.finite_prefix = s.proof.finite_e_count;
        for (unsigned k = 0; k < 2; ++k) {
            const auto col = k ? L::b_column[t] : L::a_column[t];
            if (col < 0) continue;
            std::optional<std::int64_t> low, high;
            const auto& c = block.columns[static_cast<std::size_t>(col)];
            for (std::uint32_t row = 0; row < e.n; ++row) {
                if (c.is_null(row)) continue;
                const auto v = signed_word(c.words[row]);
                if (!low || v < *low) low = v;
                if (!high || v > *high) high = v;
            }
            if (low) {
                e.flags |= static_cast<std::uint8_t>(1u << k);
                (k ? e.b_min : e.a_min) = *low; (k ? e.b_max : e.a_max) = *high;
            }
        }
        for (std::uint32_t row = 0; row < e.n; ++row) {
            for (const auto& c : block.columns) canonical(s.streams[t], c, row);
            if (e.tag == 'I') {
                const auto actual = static_cast<char>(block.columns[0].words[row]);
                integrity(actual == s.order[row], "incremental input I order mismatch");
                ++s.proof.multiplicities[tag_index(actual)];
                continue;
            }
            const auto record = block.row(row);
            record_rules(record, s.previous_time);
            const auto line = record_json(record);
            integrity(sha(line) == s.expected[t][row], "incremental input record mismatch");
            s.decoded_lines[t].update(line);
            if (e.tag == 'E') {
                const auto w = sample(block, row);
                accumulate(e.mtm, w, false); accumulate(e.book, w, true);
                if (!block.columns[1].is_null(row)) ++s.proof.finite_e_count;
            } else if (e.tag == 'T') {
                auto& folds = s.proof.folds;
                if (block.columns[14].words[row]) {
                    ++folds.open_at_end_count;
                    if (!block.columns[7].is_null(row)) {
                        folds.open_at_end_pnl = folds.open_at_end_pnl + number(block.columns[7].words[row]);
                        need(std::isfinite(folds.open_at_end_pnl), "non-finite incremental sink fold");
                    }
                } else ++folds.closed_count;
            }
        }
        if (e.tag == 'E') {
            e.flags |= 4;
            commit_descriptor(s.full_e, e, true);
        }
        commit_descriptor(s.descriptors[t], e, false);
        // Bind the bytes supplied to Output::write, not a later possibly
        // damaged reread that could merely seal its own corruption.
        std::size_t bytes = 0;
        for (const auto part : encoded) { s.stored.update(part); bytes += part.size(); }
        integrity(bytes == length, "incremental encoded byte count mismatch");
        s.proof.counts[t] = add(s.proof.counts[t], e.n); ++s.proof.blocks[t];
        s.proof.data_end = add(offset, length);
        if (t == 7) s.order_used = 0; else s.used[t] = 0;
        s.workspace.columns = std::move(block.columns);
    } catch (const ReaderError& error) {
        throw ReaderError(error.failure(), std::string("incremental validation: ") + error.what());
    }
}
IncrementalProof IncrementalValidator::finish(int fd, const std::array<std::uint64_t, 7>& counts) {
    auto& s = *state_;
    s.source(fd); // empty runs still independently check the encoded header
    integrity(s.pending.empty() && !s.order_used && s.reader.file_bytes == s.proof.data_end,
              "incremental unfinished input or block");
    for (std::size_t t = 0; t < L::stream_count; ++t) {
        if (t < 7) {
            integrity(!s.used[t] && s.proof.counts[t] == counts[t]
                && s.proof.multiplicities[t] == counts[t], "incremental final counts mismatch");
            integrity(s.input_lines[t].digest() == s.decoded_lines[t].digest(),
                      "incremental per-type v1 digest mismatch");
        }
        s.proof.streams[t] = s.streams[t].digest();
        s.proof.descriptors[t] = s.descriptors[t].digest();
    }
    s.proof.full_e_descriptors = s.full_e.digest();
    s.proof.stored_data = s.stored.digest();
    s.proof.complete = true;
    return s.proof;
}

Reader::Reader(std::shared_ptr<ReaderState> state) : state_(std::move(state)) {}
Reader Reader::open(std::shared_ptr<RandomAccessSource> source, ReaderOptions options) {
    need(bool(source), "null range source");
    auto state = std::make_shared<ReaderState>();
    state->source = std::move(source); state->options = std::move(options);
    state->file_bytes = state->source->size(); state->generation = state->source->generation();
    need(!state->generation.empty(), "source must supply an immutable generation");
    state->open();
    return Reader(std::move(state));
}
bool Reader::has_zstd() noexcept {
#if defined(PINEFORGE_READER_WITH_ZSTD) && PINEFORGE_READER_WITH_ZSTD
    return true;
#else
    return false;
#endif
}
ReadResult<const Head&> Reader::head() const { return state_->observe<const Head&>(state_->head); }
ReadResult<const std::array<std::uint64_t, L::stream_count>&> Reader::counts() const {
    return state_->observe<const std::array<std::uint64_t, L::stream_count>&>(state_->counts);
}
ReadResult<const std::array<TableLocator, L::table_count>&> Reader::tables() const {
    return state_->observe<const std::array<TableLocator, L::table_count>&>(state_->tables);
}
ReadResult<Profile> Reader::profile() const { return state_->observe(state_->profile); }
ReadResult<std::uint64_t> Reader::finite_equity_count() const { return state_->observe(state_->finite_e_count); }
ReadResult<const SinkFolds&> Reader::sink_folds() const { return state_->observe<const SinkFolds&>(state_->folds); }
ReadResult<const std::string&> Reader::generation() const { return state_->observe<const std::string&>(state_->generation); }
ReadResult<Digest> Reader::logical_sha256() const { return state_->observe(state_->logical_sha); }
ReadResult<bool> Reader::verified() const { return state_->observe(state_->complete); }
ReadResult<MemoryStats> Reader::memory_stats() const { return {*state_->stats, state_->scope()}; }
ReadResult<SummaryFrameInfo> Reader::summary_frame_info() const {
    // Open already checked framing/window/ranges and decoded integrity before
    // caching this fixed header. Reassert its budgets before exposing C/J.
    const auto& info = state_->summary_frame_info;
    check_summary_lengths(info.stored_len, info.json_len);
    return state_->observe(info);
}
ReadResult<const IndexPage&> Reader::index_page(std::uint8_t table, std::uint32_t page) {
    return state_->observe<const IndexPage&>(state_->page(table, page));
}
BlockEntry Reader::descriptor(std::uint8_t table, std::uint64_t ordinal) { return state_->entry(table, ordinal); }
DecodedBlock Reader::read_columns(const BlockEntry& e, const std::vector<std::uint32_t>& ids) {
    // Never accept caller-forged pointers/hashes: resolve the authenticated entry again.
    const auto trusted = state_->entry(e.table, e.ordinal);
    return state_->block(trusted, ids, ids.size() == state_->head.records[trusted.table].fields.size());
}
DecodedBlock Reader::read_block(const BlockEntry& e) { return state_->block(state_->entry(e.table, e.ordinal)); }
ReadResult<std::string> Reader::summary() const {
    state_->poll();
    auto out = state_->head.summary;
    const auto close = out.find_last_not_of(" \t\r\n");
    need(close != std::string::npos && out[close] == '}', "invalid retained summary");
    out.resize(close);
    out += ",\"report_sink\":{\"path\":" + escaped_string(state_->options.locator)
        + ",\"schema\":\"pf-report-sink\",\"version\":2,\"run_id\":" + escaped_string(state_->head.run_id)
        + ",\"counts\":{";
    bool comma = false;
    for (std::size_t i = 0; i + 1 < state_->counts.size(); ++i) {
        if (!state_->head.records[i].present) continue;
        if (comma) out += ',';
        comma = true;
        out += escaped_string(std::string(1, L::tags[i])) + ":" + std::to_string(state_->counts[i]);
    }
    out += "},\"sha256\":\"" + hex(state_->logical_sha) + "\",\"bytes\":" + std::to_string(state_->file_bytes)
        + ",\"scratch_flags\":" + std::to_string(state_->options.scratch_flags) + "}}";
    return {std::move(out), state_->scope()};
}

struct Reader::Cursor::Impl {
    std::shared_ptr<ReaderState> state;
    std::uint8_t table = 0;
    std::uint64_t next = 0, end = 0, next_entry = 0;
    bool finite_only = false;
    bool adjust_final = false;
    std::uint32_t local = 0;
    std::uint64_t finite_at = 0;
    std::unique_ptr<DecodedBlock> block;
    std::optional<std::int64_t> previous_time;
    bool take(Record& r) {
        state->poll();
        if (next == end) { block.reset(); return false; }
        if (finite_only) {
            for (;;) {
                state->poll();
                if (!block || local == block->entry.n) {
                    block.reset();
                    need(next_entry < state->tables[0].entry_count, "finite cursor exhausted before interval");
                    const auto e = state->entry(0, next_entry++);
                    if (state->profile == Profile::full) {
                        need(e.finite_prefix <= next, "finite cursor prefix discontinuity");
                        if (!e.mtm.finite_count) continue;
                        finite_at = e.finite_prefix;
                    }
                    block = std::make_unique<DecodedBlock>(state->block(e));
                    local = 0;
                }
                const auto row = local++;
                if (block->columns[1].is_null(row)) continue;
                const auto ordinal = finite_at++;
                if (ordinal < next) continue;
                need(ordinal == next, "finite cursor validity/prefix mismatch");
                r = block->row(row); record_rules(r, previous_time); ++next;
                if (adjust_final && next == state->finite_e_count && state->folds.open_at_end_count) {
                    auto& profit = r.values[2];
                    if (std::abs(profit.null ? 0.0 : profit.as_f64()) <= 1e-9) {
                        profit.null = false; profit.word = bits(state->folds.open_at_end_pnl);
                    }
                }
                return true;
            }
        }
        if (!block || next >= block->entry.rec0 + block->entry.n) {
            block.reset(); // release before loading the successor: one live block per cursor
            const auto e = state->entry(table, next_entry++);
            need(e.rec0 <= next && next < add(e.rec0, e.n), "cursor descriptor rec0 discontinuity");
            block = std::make_unique<DecodedBlock>(state->block(e));
        }
        r = block->row(static_cast<std::uint32_t>(next - block->entry.rec0));
        record_rules(r, previous_time);
        ++next;
        return true;
    }
};
Reader::Cursor::Cursor() = default;
Reader::Cursor::~Cursor() = default;
Reader::Cursor::Cursor(Cursor&&) noexcept = default;
Reader::Cursor& Reader::Cursor::operator=(Cursor&&) noexcept = default;
Reader::Cursor::Cursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VerificationScope Reader::Cursor::file_scope() const {
    if (!impl_) return VerificationScope::unverified;
    impl_->state->poll(); return impl_->state->scope();
}
bool Reader::Cursor::next(Record& r) {
    if (!impl_ || !impl_->take(r)) return false;
    r.file_scope = impl_->state->scope();
    return true;
}
Reader::Cursor Reader::iter_type(char tag) {
    state_->poll();
    const auto table = tag_index(tag);
    need(state_->head.records[table].present, "iterator type absent from head");
    auto impl = std::make_unique<Cursor::Impl>();
    impl->state = state_; impl->table = static_cast<std::uint8_t>(table);
    impl->end = state_->counts[table];
    return Cursor(std::move(impl));
}
Reader::Cursor Reader::trade_slice(std::uint64_t first, std::uint64_t last) {
    state_->poll();
    need(first >= 1 && first <= L::safe && last <= state_->counts[1]
        && (last >= first || (state_->counts[1] == 0 && first == 1 && last == 0)), "invalid inclusive trade slice");
    auto impl = std::make_unique<Cursor::Impl>();
    impl->state = state_; impl->table = 1; impl->next = first - 1; impl->end = last;
    if (impl->next < impl->end) impl->next_entry = state_->find_record(1, impl->next);
    return Cursor(std::move(impl));
}
Reader::Cursor Reader::equity_slice(std::uint64_t begin, std::uint64_t end, bool adjust_final) {
    state_->poll();
    need(begin <= end && end <= state_->finite_e_count, "invalid finite equity interval");
    auto impl = std::make_unique<Cursor::Impl>();
    impl->state = state_; impl->finite_only = true; impl->next = begin; impl->end = end;
    impl->adjust_final = adjust_final;
    if (begin < end && state_->profile == Profile::full) impl->next_entry = state_->find_finite(begin);
    return Cursor(std::move(impl));
}

struct Reader::MergedCursor::Impl {
    std::shared_ptr<ReaderState> state;
    std::array<Cursor, L::stream_count - 1> types;
    Cursor interleave;
    std::uint64_t consumed = 0;
    bool finished = false;
};
Reader::MergedCursor::MergedCursor() = default;
Reader::MergedCursor::~MergedCursor() = default;
Reader::MergedCursor::MergedCursor(MergedCursor&&) noexcept = default;
Reader::MergedCursor& Reader::MergedCursor::operator=(MergedCursor&&) noexcept = default;
Reader::MergedCursor::MergedCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VerificationScope Reader::MergedCursor::file_scope() const {
    if (!impl_) return VerificationScope::unverified;
    impl_->state->poll(); return impl_->state->scope();
}
bool Reader::MergedCursor::next(Record& r) {
    if (!impl_) return false;
    impl_->state->poll();
    if (impl_->finished) return false;
    Record order;
    if (impl_->interleave.next(order)) {
        const auto table = tag_index(static_cast<char>(order.values[0].word));
        need(table < impl_->types.size() && impl_->types[table].next(r), "I exhausts a type cursor");
        ++impl_->consumed; return true;
    }
    need(impl_->consumed == impl_->state->counts.back(), "I total count mismatch");
    Record extra;
    for (auto& cursor : impl_->types) need(!cursor.next(extra), "I leaves unconsumed type rows");
    impl_->finished = true;
    return false;
}
Reader::MergedCursor Reader::iter_merged() {
    auto impl = std::make_unique<MergedCursor::Impl>();
    impl->state = state_; impl->interleave = iter_type('I');
    for (std::size_t i = 0; i < impl->types.size(); ++i)
        if (state_->head.records[i].present) impl->types[i] = iter_type(L::tags[i]);
    return MergedCursor(std::move(impl));
}

Reader::BlockCursor Reader::blocks_for_time_range(char tag, char key, std::int64_t t0, std::int64_t t1) {
    state_->poll();
    const auto table = tag_index(tag);
    need((key == 'A' || key == 'B') && t0 <= t1 && state_->head.records[table].present, "invalid time query");
    need((key == 'A' ? L::a_column[table] : L::b_column[table]) >= 0, "time key does not exist for tag");
    BlockCursor c; c.state_ = state_; c.table_ = static_cast<std::uint8_t>(table);
    c.key_ = key; c.t0_ = t0; c.t1_ = t1;
    if (t0 == t1) return c;
    c.end_ = state_->tables[table].entry_count;
    if (tag == 'E') {
        c.next_ = first_e_block(*state_, t0);
        std::uint64_t lo = c.next_, hi = c.end_;
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            if (state_->entry(0, mid).a_min < t1) lo = mid + 1; else hi = mid;
        }
        c.end_ = lo;
    }
    return c;
}
VerificationScope Reader::BlockCursor::file_scope() const {
    if (!state_) return VerificationScope::unverified;
    state_->poll(); return state_->scope();
}
bool Reader::BlockCursor::next(BlockEntry& e) {
    if (!state_) return false;
    state_->poll();
    while (next_ < end_) {
        const auto candidate = state_->entry(table_, next_++);
        const auto flag = key_ == 'A' ? 1u : 2u;
        const auto low = key_ == 'A' ? candidate.a_min : candidate.b_min;
        const auto high = key_ == 'A' ? candidate.a_max : candidate.b_max;
        if ((candidate.flags & flag) && high >= t0_ && low < t1_) { e = candidate; return true; }
    }
    return false;
}

TradeRankPage Reader::trade_rank_page(std::string_view key, bool descending,
                                     std::int64_t requested_page, std::uint64_t size) {
    state_->poll();
    need(size > 0 && size <= L::safe, "rank page size must be positive safe integer");
    std::uint8_t table = 0;
    if (key != "n") {
        const auto it = std::find(L::rank_keys.begin(), L::rank_keys.end(), key);
        need(it != L::rank_keys.end(), "unknown trade sort key");
        table = static_cast<std::uint8_t>(L::stream_count + 2 * (it - L::rank_keys.begin()) + (descending ? 1 : 0));
    }
    TradeRankPage out; out.total = state_->counts[1]; out.scope = state_->scope();
    out.page_count = std::max<std::uint64_t>(1, out.total / size + (out.total % size != 0));
    out.effective_page = std::min<std::uint64_t>(requested_page < 1 ? 1 : static_cast<std::uint64_t>(requested_page),
                                               std::max<std::uint64_t>(out.page_count, 1));
    const auto start = multiply(out.effective_page - 1, size);
    const auto length = std::min(size, out.total - start);
    if (length) { out.from = start + 1; out.to = start + length; }
    struct Selection { std::uint64_t position, block; std::size_t rank; };
    std::vector<Selection> selection;
    selection.reserve(host_size(length)); out.rows.resize(host_size(length));
    std::vector<std::uint32_t> compact_order;
    if (length && key != "n" && state_->profile == Profile::compact)
        compact_order = state_->compact_positions(L::rank_columns[(table - L::stream_count) / 2], descending);
    for (std::uint64_t i = 0; i < length; ++i) {
        state_->poll();
        const auto rank = start + i;
        const auto position = key == "n" ? (descending ? out.total - 1 - rank : rank)
            : state_->profile == Profile::compact ? compact_order[host_size(rank)] : state_->position(table, rank);
        selection.push_back({position, state_->find_record(1, position), host_size(i)});
    }
    std::sort(selection.begin(), selection.end(), [](const Selection& a, const Selection& b) {
        return a.block < b.block || (a.block == b.block && a.position < b.position);
    });
    // One payload fetch per distinct selected block, even for scattered ranks.
    for (std::size_t i = 0; i < selection.size();) {
        const auto e = state_->entry(1, selection[i].block);
        auto block = state_->block(e);
        do {
            auto r = block.row(static_cast<std::uint32_t>(selection[i].position - e.rec0));
            std::optional<std::int64_t> ignored; record_rules(r, ignored);
            out.rows[selection[i].rank] = std::move(r);
            ++i;
        } while (i < selection.size() && selection[i].block == e.ordinal);
    }
    return out;
}

ReadResult<std::uint64_t> Reader::trade_non_null_count(std::string_view key, bool descending) {
    state_->poll();
    if (key == "n") return {state_->counts[1], state_->scope()};
    const auto it = std::find(L::rank_keys.begin(), L::rank_keys.end(), key);
    need(it != L::rank_keys.end(), "unknown trade sort key");
    const auto column = L::rank_columns[static_cast<std::size_t>(it - L::rank_keys.begin())];
    if (state_->profile == Profile::compact) {
        std::uint64_t count = 0;
        for (std::uint64_t j = 0; j < state_->tables[1].entry_count; ++j) {
            auto b = state_->block(state_->entry(1, j), {column}, false);
            for (std::uint32_t row = 0; row < b.entry.n; ++row) {
                state_->poll(); if (!b.columns[0].is_null(row)) ++count;
            }
        }
        return {count, state_->scope()};
    }
    const auto table = static_cast<std::uint8_t>(L::stream_count + 2 * (it - L::rank_keys.begin()) + descending);
    std::uint64_t lo = 0, hi = state_->counts[1];
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto position = state_->position(table, mid);
        auto b = state_->block(state_->entry(1, state_->find_record(1, position)), {column}, false);
        if (b.columns[0].is_null(host_size(position - b.entry.rec0))) hi = mid; else lo = mid + 1;
    }
    return {lo, state_->scope()};
}

EquityEnvelope Reader::equity_summary(std::uint64_t requested,
        std::optional<std::array<std::int64_t, 2>> time_range, bool adjust_final) {
    state_->poll(); need(requested > 0 && requested <= L::safe, "bucket count must be positive safe integer");
    EquityEnvelope out; out.scope = state_->scope();
    if (time_range) {
        need((*time_range)[0] <= (*time_range)[1], "reversed equity time range");
        if ((*time_range)[0] == (*time_range)[1]) return out;
    }
    const bool default_figure = !time_range && requested == L::figure_buckets;
    adjust_final = adjust_final || default_figure;
    if (state_->profile == Profile::compact) return state_->scan_equity(requested, time_range, adjust_final);
    std::uint64_t begin = 0, end = state_->finite_e_count;
    if (time_range) {
        begin = lower_e_finite_time(*state_, (*time_range)[0]);
        end = lower_e_finite_time(*state_, (*time_range)[1]);
        need(begin <= end, "invalid E time index");
    }
    out.population = end - begin;
    const auto buckets = std::min(requested, out.population);
    if (!buckets) return out;
    out.buckets.reserve(host_size(buckets));
    if (default_figure) {
        for (std::uint64_t j = 0; j < buckets; ++j) out.buckets.push_back(state_->figure(j));
        return out;
    }
    std::unique_ptr<DecodedBlock> decoded;
    std::uint64_t descriptor_ordinal = state_->find_finite(begin), at = begin, carry = 0;
    std::uint32_t decoded_row = 0;
    const auto q = out.population / buckets, remainder = out.population % buckets;
    // Equivalent to floor(j*N/b), without a potentially overflowing j*N.
    for (std::uint64_t j = 0; j < buckets; ++j) {
        state_->poll();
        std::uint64_t width = q;
        if (carry >= buckets - remainder) { ++width; carry -= buckets - remainder; }
        else carry += remainder;
        EquityBucket bucket; bucket.begin = at; bucket.end = add(at, width);
        while (at < bucket.end) {
            state_->poll();
            const auto e = state_->entry(0, descriptor_ordinal);
            const auto finite_end = add(e.finite_prefix, e.mtm.finite_count);
            if (e.mtm.finite_count == 0) { ++descriptor_ordinal; continue; }
            need(e.finite_prefix <= at && at < finite_end, "E finite descriptor gap in envelope");
            const auto stop = std::min(bucket.end, finite_end);
            const bool final_block = adjust_final && state_->folds.open_at_end_count && finite_end == state_->finite_e_count;
            if (at == e.finite_prefix && stop == finite_end && !final_block) {
                decoded.reset();
                compose(bucket.mtm, e.mtm, false); compose(bucket.book, e.book, true);
            } else {
                if (!decoded || decoded->entry.ordinal != e.ordinal) {
                    decoded.reset();
                    decoded = std::make_unique<DecodedBlock>(state_->block(e, {0,1,2}, false));
                    decoded_row = 0;
                    auto skip = e.finite_prefix;
                    while (skip < at) {
                        need(decoded_row < e.n, "finite envelope mask/prefix mismatch");
                        if (!decoded->columns[1].is_null(decoded_row++)) ++skip;
                    }
                }
                for (auto finite = at; finite < stop;) {
                    state_->poll();
                    need(decoded_row < e.n, "finite envelope mask/prefix mismatch");
                    const auto row = decoded_row++;
                    if (decoded->columns[1].is_null(row)) continue;
                    const auto w = sample(*decoded, row,
                        adjust_final && finite + 1 == state_->finite_e_count ? &state_->folds : nullptr);
                    accumulate(bucket.mtm, w, false); accumulate(bucket.book, w, true);
                    ++finite;
                }
            }
            at = stop;
            if (at == finite_end) { ++descriptor_ordinal; decoded.reset(); }
        }
        finish_bucket(bucket);
        out.buckets.push_back(std::move(bucket));
    }
    return out;
}

Verification Reader::verify(const std::function<bool(const Record&)>& callback) {
    return verify_impl(callback, nullptr);
}
Verification Reader::verify_structure(const IncrementalProof& proof, unsigned threads) {
    state_->complete = false;
    integrity(proof.complete, "missing completed incremental validation");
    need(threads >= 1 && threads <= late_worker_limit, "invalid late worker count");
    need(threads == 1 || (!state_->options.cancelled && state_->source->concurrent_reads()),
         "parallel structure verification requires immutable concurrent ranges without cancellation callback");
    return verify_impl({}, &proof, threads);
}
Verification Reader::verify_impl(const std::function<bool(const Record&)>& callback,
                                  const IncrementalProof* proof, unsigned threads) {
    state_->complete = false;
    Verification result;
    try {
        state_->poll();
        std::array<Sha256, L::stream_count> hashes;
        std::array<std::uint64_t, L::stream_count> blocks{}, seen{}, multiplicities{};
        std::uint64_t finite_seen = 0;
        SinkFolds folds;
        for (std::size_t t = 0; t < hashes.size(); ++t) {
            hashes[t].update(L::stream_domain); fixed(hashes[t], static_cast<unsigned char>(L::tags[t]), 1);
        }
        std::optional<std::int64_t> previous_time;
        std::array<Sha256, L::stream_count> descriptor_hashes;
        Sha256 full_e_descriptors;
        if (proof) {
            integrity(proof->data_start == state_->data_start && proof->data_end == state_->data_end,
                      "incremental data extent mismatch");
            finite_seen = proof->finite_e_count;
            folds = proof->folds;
            multiplicities = proof->multiplicities;
        }
        auto at = state_->data_start;
        while (at < state_->data_end) {
            state_->poll();
            need(state_->data_end - at >= L::bhead, "truncated physical block");
            const auto raw = state_->range(at, L::bhead);
            const auto h = view(raw);
            need(h.substr(0, L::block_magic.size()) == L::block_magic, "physical data is not block tiled");
            const auto t = tag_index(static_cast<char>(le(h, L::Block::tag, 1)));
            const auto e = state_->entry(static_cast<std::uint8_t>(t), blocks[t]++);
            need(e.offset == at && e.rec0 == seen[t], "physical descriptor/rec0 order mismatch");
            if (proof) {
                // Framing, lengths, encoding/null flags and block SHA remain
                // checked here. Column decoding was proved at block close.
                (void)state_->block(e, {}, false);
                commit_descriptor(descriptor_hashes[t], e, false);
                if (e.tag == 'E' && state_->profile == Profile::full)
                    commit_descriptor(full_e_descriptors, e, true);
                seen[t] = add(seen[t], e.n); at = add(at, e.length); ++result.blocks;
                continue;
            }
            auto block = state_->block(e);
            verify_derived(block);
            if (e.tag == 'E' && state_->profile == Profile::full)
                need(e.finite_prefix == finite_seen, "finite E prefix discontinuity");
            for (std::uint32_t row = 0; row < e.n; ++row) {
                state_->poll();
                for (const auto& c : block.columns) canonical(hashes[t], c, row);
                if (e.tag == 'E' && !block.columns[1].is_null(row)) ++finite_seen;
                if (e.tag == 'T') {
                    if (block.columns[14].words[row]) {
                        ++folds.open_at_end_count;
                        if (!block.columns[7].is_null(row)) {
                            folds.open_at_end_pnl = folds.open_at_end_pnl + number(block.columns[7].words[row]);
                            need(std::isfinite(folds.open_at_end_pnl), "non-finite sink fold");
                        }
                    } else ++folds.closed_count;
                }
                if (e.tag == 'I') {
                    const auto type = tag_index(static_cast<char>(block.columns[0].words[row]));
                    multiplicities[type] = add(multiplicities[type], 1);
                } else {
                    const auto r = block.row(row);
                    record_rules(r, previous_time);
                    if (callback && !callback(r)) fail("verification callback cancelled", Failure::cancelled);
                }
            }
            seen[t] = add(seen[t], e.n); at = add(at, e.length); ++result.blocks;
        }
        need(at == state_->data_end && result.blocks == state_->nblocks, "physical block tiling mismatch");
        integrity(finite_seen == state_->finite_e_count, "finite E count mismatch");
        integrity(folds.closed_count == state_->folds.closed_count
            && folds.open_at_end_count == state_->folds.open_at_end_count
            && bits(folds.open_at_end_pnl) == bits(state_->folds.open_at_end_pnl), "sink_folds mismatch");
        std::array<Digest, L::stream_count> computed{};
        for (std::size_t t = 0; t < seen.size(); ++t) {
            need(seen[t] == state_->counts[t] && blocks[t] == state_->tables[t].entry_count, "stream counts mismatch");
            if (t + 1 < seen.size()) need(multiplicities[t] == seen[t], "I multiplicities mismatch");
            computed[t] = proof ? proof->streams[t] : hashes[t].digest();
            if (proof) {
                integrity(seen[t] == proof->counts[t] && blocks[t] == proof->blocks[t],
                          "incremental stream/block counts mismatch");
                integrity(descriptor_hashes[t].digest() == proof->descriptors[t],
                          "incremental descriptor claims mismatch");
            }
            if (state_->profile == Profile::full)
                integrity(computed[t] == state_->streams[t], "decoded stream SHA mismatch");
        }
        integrity(logical(state_->v1_sha, state_->summary_sha, seen, computed) == state_->logical_sha,
                  "decoded logical SHA mismatch");
        if (proof && state_->profile == Profile::full)
            integrity(full_e_descriptors.digest() == proof->full_e_descriptors,
                      "incremental E witnesses/prefix mismatch");
        const auto verify_pages = [](ReaderState* state_) {
            // Independently rebuild each table tree with fixed hash state:
            // proofs alone would permit noncanonical padded leaves (9.2).
            for (std::size_t t = 0; t < state_->tables.size(); ++t) {
                const auto pages = state_->tables[t].page_count;
                if (!pages) continue;
                std::array<Digest, L::Page::max_depth + 1> stack{};
                std::uint64_t occupied = 0, padded = 1;
                unsigned depth = 0;
                while (padded < pages) { padded *= 2; ++depth; }
                auto append = [&](Digest current) {
                    unsigned level = 0;
                    while (occupied & (UINT64_C(1) << level)) {
                        Sha256 h; h.update(L::node_domain);
                        h.update(digest_view(stack[level])); h.update(digest_view(current));
                        current = h.digest(); occupied ^= UINT64_C(1) << level; ++level;
                    }
                    stack[level] = current; occupied |= UINT64_C(1) << level;
                };
                Digest last{};
                for (std::uint32_t p = 0; p < pages; ++p) {
                    const auto& page = state_->page(static_cast<std::uint8_t>(t), p);
                    last = digest_at(view(page.bytes), L::Page::leaf); append(last);
                }
                for (std::uint64_t p = pages; p < padded; ++p) { state_->poll(); append(last); }
                integrity(stack[depth] == state_->tables[t].merkle_root, "Merkle repeated-last padding/tree mismatch");
            }
        };
        const auto verify_rank = [](ReaderState* state_, std::uint8_t table) {
            if (state_->profile != Profile::full) return;
            const auto key = L::rank_columns[(table - L::stream_count) / 2];
            const bool descending = (table - L::stream_count) % 2 != 0;
            std::optional<SortKey> previous;
            struct Probe { std::size_t order; SortKey key; };
            constexpr auto capacity = L::page_payload / sizeof(Probe);
            static_assert(capacity > 0, "rank verification window");
            for (std::uint64_t rank = 0; rank < state_->counts[1];) {
                const auto length = host_size(std::min<std::uint64_t>(capacity, state_->counts[1] - rank));
                std::vector<Probe> probes(length);
                state_->stats->peak_rank_scratch_bytes = std::max(state_->stats->peak_rank_scratch_bytes,
                    probes.capacity() * sizeof(Probe));
                for (std::size_t j = 0; j < length; ++j) {
                    state_->poll(); probes[j] = {j,{true,0.0,state_->position(table, rank + j)}};
                }
                // Group this fixed window by canonical position. Each selected
                // T key column is fetched once per window; the complete data
                // scan above already checked all block/stream digests.
                std::sort(probes.begin(), probes.end(), [](const Probe& a, const Probe& b) {
                    return a.key.position < b.key.position;
                });
                auto ordinal = state_->find_record(1, probes.front().key.position);
                auto e = state_->entry(1, ordinal);
                for (std::size_t j = 0; j < length;) {
                    while (probes[j].key.position >= e.rec0 + e.n)
                        e = state_->entry(1, ++ordinal);
                    need(probes[j].key.position >= e.rec0, "rank resolves into descriptor gap");
                    auto block = state_->block(e, {key}, false);
                    do {
                        probes[j].key = sort_key(block, static_cast<std::uint32_t>(probes[j].key.position - e.rec0));
                        ++j;
                    } while (j < length && probes[j].key.position < e.rec0 + e.n);
                }
                std::sort(probes.begin(), probes.end(), [](const Probe& a, const Probe& b) { return a.order < b.order; });
                for (const auto& probe : probes) {
                    state_->poll();
                    need(!previous || before(*previous, probe.key, descending), "invalid rank permutation/comparator");
                    previous = probe.key;
                }
                rank += length;
            }
            // Explicit position-window permutation proof, independent of the
            // comparator proof. Successive fixed bitsets never scale with T.
            constexpr std::uint64_t window = L::page * 8;
            for (std::uint64_t start = 0; start < state_->counts[1];) {
                const auto length = std::min(window, state_->counts[1] - start);
                Bytes present(host_size((length + 7) / 8), 0);
                state_->stats->peak_rank_scratch_bytes = std::max(state_->stats->peak_rank_scratch_bytes, present.capacity());
                std::uint64_t found = 0;
                for (std::uint64_t rank = 0; rank < state_->counts[1]; ++rank) {
                    state_->poll();
                    const auto position = state_->position(table, rank);
                    if (position < start || position - start >= length) continue;
                    const auto local = position - start;
                    const auto mask = static_cast<std::uint8_t>(1u << (local % 8));
                    auto& byte = present[host_size(local / 8)];
                    need(!(byte & mask), "duplicate rank permutation position");
                    byte |= mask; ++found;
                }
                need(found == length, "missing rank permutation position");
                start += length;
            }
        };
        const auto verify_figures = [finite_seen](ReaderState* state_) {
            if (state_->profile == Profile::full && finite_seen) {
                std::uint64_t ordinal = 0;
                // One active recomputed bucket and decoded E block; never retain
                // the whole derived table as a verifier-side output buffer.
                state_->scan_equity(L::figure_buckets, std::nullopt, true, [&](EquityBucket&& expected) {
                    const auto actual = state_->figure(ordinal++);
                    integrity(actual.begin == expected.begin && actual.end == expected.end
                        && actual.points.size() == expected.points.size(), "default figure bucket mismatch");
                    for (std::size_t i = 0; i < actual.points.size(); ++i)
                        integrity(same(actual.points[i], expected.points[i]), "default figure witness mismatch");
                    for (const auto book : {false, true}) {
                        const auto& a = book ? actual.book : actual.mtm;
                        const auto& b = book ? expected.book : expected.mtm;
                        integrity(a.flags == b.flags, "default figure flags mismatch");
                        for (unsigned k = 0; k < 4; ++k)
                            integrity(same(a.witnesses[k], b.witnesses[k]), "default figure selector mismatch");
                    }
                });
                need(ordinal == state_->tables[L::figure_table].entry_count, "default figure entry count mismatch");
            }
        };
        const auto verify_stored = [proof](ReaderState* state_) {
            if (state_->stored_present) {
                Sha256 stored;
                Sha256 stored_data;
                const auto stop = add(state_->root_offset, state_->root_length);
                for (std::uint64_t offset = 0; offset < stop;) {
                    state_->poll();
                    const auto length = host_size(std::min<std::uint64_t>(L::page, stop - offset));
                    const auto bytes = state_->range(offset, length);
                    stored.update(view(bytes));
                    if (proof) {
                        const auto begin = std::max(offset, state_->data_start);
                        const auto end = std::min(offset + length, state_->data_end);
                        if (begin < end) stored_data.update(view(bytes).substr(host_size(begin - offset), host_size(end - begin)));
                    }
                    offset += length;
                }
                integrity(stored.digest() == state_->stored_sha, "stored prefix SHA mismatch");
                if (proof) integrity(stored_data.digest() == proof->stored_data,
                                     "incremental stored data changed after validation");
            }
            if (proof) integrity(state_->stored_present, "structure verification requires stored digest");
        };
        if (proof && threads > 1 && state_->profile == Profile::full) {
            std::array<std::unique_ptr<ReaderState>, late_worker_limit> local;
            // Start long linear units early; failure ordinals retain the
            // original order: trees, ten ranks, figures, stored reread.
            constexpr std::array<std::size_t, 13> schedule{{11,12,0,1,2,3,4,5,6,7,8,9,10}};
            ordered_tasks(threads, schedule, [&](std::size_t unit, unsigned worker) {
                if (!local[worker]) local[worker] = state_->isolated();
                auto* reader = local[worker].get();
                if (unit == 0) verify_pages(reader);
                else if (unit <= 10) verify_rank(reader, static_cast<std::uint8_t>(7 + unit));
                else if (unit == 11) verify_figures(reader);
                else verify_stored(reader);
            });
            // Sum private peaks conservatively; no counter is concurrently
            // mutated. All private decoded leases have ended before this fold.
            MemoryStats peaks;
            peaks.peak_index_pages = state_->stats->live_index_pages;
            peaks.peak_decoded_blocks = state_->stats->live_decoded_blocks;
            peaks.peak_decoded_bytes = state_->stats->live_decoded_bytes;
            for (const auto& reader : local) if (reader) {
                const auto& p = *reader->stats;
                peaks.peak_index_pages += p.peak_index_pages;
                peaks.peak_decoded_blocks += p.peak_decoded_blocks;
                peaks.peak_decoded_bytes += p.peak_decoded_bytes;
                peaks.peak_column_scratch_bytes += p.peak_column_scratch_bytes;
                peaks.peak_rank_scratch_bytes += p.peak_rank_scratch_bytes;
            }
            auto& stats = *state_->stats;
            stats.peak_index_pages = std::max(stats.peak_index_pages, peaks.peak_index_pages);
            stats.peak_decoded_blocks = std::max(stats.peak_decoded_blocks, peaks.peak_decoded_blocks);
            stats.peak_decoded_bytes = std::max(stats.peak_decoded_bytes, peaks.peak_decoded_bytes);
            stats.peak_column_scratch_bytes = std::max(stats.peak_column_scratch_bytes, peaks.peak_column_scratch_bytes);
            stats.peak_rank_scratch_bytes = std::max(stats.peak_rank_scratch_bytes, peaks.peak_rank_scratch_bytes);
        } else {
            verify_pages(state_.get());
            for (std::uint8_t table = L::stream_count; table < L::rank_table_end; ++table)
                verify_rank(state_.get(), table);
            verify_figures(state_.get());
            verify_stored(state_.get());
        }
        state_->poll(); state_->complete = !proof;
        result.scope = proof ? VerificationScope::structure : VerificationScope::whole_file;
        result.logical_sha = state_->logical_sha;
    } catch (const ReaderError& error) {
        if (error.failure() != Failure::cancelled) throw;
        result.scope = VerificationScope::cancelled;
    }
    return result;
}

Conversion Reader::to_v1(const std::function<bool(std::string_view)>& output, PhaseLog* phase_log) {
    need(bool(output), "missing conversion output callback");
    Conversion result;
    if (verify().scope != VerificationScope::whole_file) return result;
    if (phase_log) phase_log->mark("full_verify_done");
    try {
        Sha256 content;
        auto emit = [&](std::string_view line, bool hash) {
            state_->poll();
            if (!output(line)) fail("conversion output cancelled", Failure::cancelled);
            state_->poll();
            if (hash) content.update(line);
            result.bytes = add(result.bytes, line.size());
        };
        emit(state_->head.v1_header, true);
        auto cursor = iter_merged(); Record r;
        while (cursor.next(r)) emit(record_json(r), true);
        state_->poll();
        const auto digest = content.hex_digest();
        std::string trailer = "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{";
        bool comma = false;
        for (std::size_t t = 0; t + 1 < state_->counts.size(); ++t) {
            if (!state_->head.records[t].present) continue;
            if (comma) trailer += ',';
            comma = true;
            trailer += escaped_string(std::string(1, L::tags[t])) + ":" + std::to_string(state_->counts[t]);
        }
        trailer += "},\"sha256\":\"" + digest + "\"}\n";
        emit(trailer, false);
        result.scope = VerificationScope::whole_file; result.v1_sha256 = digest;
    } catch (const ReaderError& error) {
        if (error.failure() != Failure::cancelled) throw;
        result.scope = VerificationScope::cancelled; result.v1_sha256.clear();
    }
    return result;
}
std::string Reader::content_sha256() {
    const auto result = to_v1([](std::string_view) { return true; });
    if (result.scope != VerificationScope::whole_file) fail("content hash cancelled", Failure::cancelled);
    return result.v1_sha256;
}

namespace {
const Json* optional_member(const Json& object, std::string_view key) {
    const auto it = std::find_if(object.children.begin(), object.children.end(),
        [&](const Json& child) { return child.key == key; });
    return it == object.children.end() ? nullptr : &*it;
}
std::string python_string(std::string_view s) {
    utf8(s);
    std::string out = "\"";
    constexpr char digits[] = "0123456789abcdef";
    auto escaped = [&](unsigned cp) {
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out += digits[(cp >> shift) & 15];
    };
    for (std::size_t at = 0; at < s.size();) {
        unsigned cp = static_cast<unsigned char>(s[at++]);
        if (cp >= 128) {
            const unsigned n = cp < 224 ? 1 : cp < 240 ? 2 : 3;
            cp &= (1u << (6 - n)) - 1;
            for (unsigned j = 0; j < n; ++j) cp = (cp << 6) | (static_cast<unsigned char>(s[at++]) & 63);
        }
        switch (cp) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (cp < 32 || cp >= 127) {
                if (cp > 65535) { cp -= 65536; escaped(0xd800 + (cp >> 10)); escaped(0xdc00 + (cp & 1023)); }
                else escaped(cp);
            } else out += static_cast<char>(cp);
        }
    }
    out += '"';
    return out;
}
std::string python_json(const Json& value) {
    switch (value.type) {
    case Json::Type::string: return python_string(value.text);
    case Json::Type::numeric:
        if (value.text.find_first_of(".eE") != std::string::npos) return f64(value.numeric);
        return std::to_string(integer(value, -static_cast<std::int64_t>(L::safe), L::safe));
    case Json::Type::boolean: return value.text;
    case Json::Type::null: return "null";
    case Json::Type::object:
    case Json::Type::array: {
        const bool object = value.type == Json::Type::object;
        std::string out = object ? "{" : "[";
        bool comma = false;
        for (const auto& child : value.children) {
            if (comma) out += ',';
            comma = true;
            if (object) out += python_string(child.key) + ":";
            out += python_json(child);
        }
        return out + (object ? "}" : "]");
    }
    }
    fail("unknown metadata JSON type");
}
std::string report_scalar(const Value& v, bool rgba = false) {
    if (v.null) return "null";
    if (v.kind == Kind::string) return python_string(v.text);
    if (v.kind == Kind::i64) return std::to_string(v.as_i64());
    if (v.kind == Kind::u64) return std::to_string(v.word);
    if (v.kind == Kind::boolean) return v.word ? "true" : "false";
    need(v.kind == Kind::f64, "invalid report scalar kind");
    if (!rgba) return f64(v.as_f64());
    // Python int(binary64) truncates, including for finite magnitudes beyond
    // machine integer width. Fixed notation of the integral double is exact.
    const auto truncated = std::trunc(v.as_f64());
    if (truncated == 0.0) return "0";
    char buffer[400];
    const auto r = std::to_chars(buffer, buffer + sizeof(buffer), truncated, std::chars_format::fixed, 0);
    need(r.ec == std::errc{}, "report rgba integer conversion failed");
    return {buffer, static_cast<std::size_t>(r.ptr - buffer)};
}
bool rgba_encoding(const Json& entry) {
    const auto* encoding = optional_member(entry, "encoding");
    return encoding && encoding->type == Json::Type::string && encoding->text == "rgba-u32";
}
std::optional<std::int64_t> python_index(const Json* value, bool allow_bool = false) {
    if (!value) return std::nullopt;
    if (allow_bool && value->type == Json::Type::boolean) return value->text == "true" ? 1 : 0;
    if (value->type != Json::Type::numeric || value->text.find_first_of(".eE") != std::string::npos) return std::nullopt;
    const auto n = integer(*value, -static_cast<std::int64_t>(L::safe), L::safe);
    return n >= 0 ? std::optional<std::int64_t>(n) : std::nullopt;
}
[[noreturn]] void report_nonfinite(const std::string& field) {
    fail("report_result_invalid: field=" + field + " reason=nonfinite", Failure::report_result_invalid);
}
} // namespace

void ReaderState::report_json(const std::function<void(std::string_view)>& output) {
    auto emit = [&](std::string_view bytes) { poll(); output(bytes); poll(); };
    auto rows = [&](std::uint8_t table, const std::function<void(const Record&)>& take) {
        for (std::uint64_t j = 0; j < tables[table].entry_count; ++j) {
            auto b = block(entry(table, j));
            for (std::uint32_t row = 0; row < b.entry.n; ++row) { poll(); take(b.row(row)); }
        }
    };
    // Match the current v1 report admission before looking at metadata. The
    // binary schema still permits nullable columns for reader fault controls.
    rows(1, [&](const Record& row) {
        for (unsigned c = 4; c <= 11; ++c)
            if (row.values[c].null) report_nonfinite("trades." + head.records[1].fields[c].name);
    });
    rows(0, [&](const Record& row) {
        if (row.values[1].null) report_nonfinite("equity_curve.equity");
        if (row.values[2].null) report_nonfinite("equity_curve.open_profit");
    });
    const auto metadata = JsonParser(head.summary, true).parse();
    const auto& summary = member(metadata, "summary");
    for (const auto key : {"net_pnl","avg_trade","best_trade","worst_trade","max_drawdown"}) {
        const auto& value = member(summary, key);
        if (value.type != Json::Type::numeric || !std::isfinite(value.numeric)) report_nonfinite(std::string("summary.") + key);
    }
    auto record_array = [&](std::uint8_t table, std::size_t fields) {
        emit("["); bool comma = false;
        rows(table, [&](const Record& row) {
            if (table == 1) for (unsigned c = 4; c <= 11; ++c)
                if (row.values[c].null) report_nonfinite("trades." + head.records[1].fields[c].name);
            if (comma) emit(",");
            comma = true; emit("{");
            for (std::size_t c = 0; c < fields; ++c) {
                if (c) emit(",");
                emit(python_string(head.records[table].fields[c].name)); emit(":"); emit(report_scalar(row.values[c]));
            }
            emit("}");
        });
        emit("]");
    };
    auto column_array = [&](unsigned column, bool rgba) {
        emit("["); bool comma = false;
        for (std::uint64_t j = 0; j < tables[2].entry_count; ++j) {
            auto b = block(entry(2, j), {column}, false);
            for (std::uint32_t row = 0; row < b.entry.n; ++row) {
                if (comma) emit(",");
                comma = true; emit(report_scalar(b.columns[0].value(row), rgba));
            }
        }
        emit("]");
    };
    auto outputs = [&](const Json& saved) {
        need(head.records[2].present && head.records[3].present && head.records[4].present && counts[4] == 1,
             "outputs_manifest_invalid: missing O/V/K constants");
        const auto& manifest = member(saved, "manifest");
        need(manifest.type == Json::Type::object, "outputs_manifest_invalid: manifest is not an object");
        need(text(member(saved, "schema_version")) == "pineforge-outputs/v1", "outputs schema_version mismatch");
        const auto* message_format = optional_member(manifest, "message_format");
        need(python_json(member(saved, "message_format")) == (message_format ? python_json(*message_format) : "\"pineforge/v1\""),
             "outputs message_format mismatch");
        const Json absent;
        auto list = [&](std::string_view key, std::string_view index, std::string_view id) {
            std::vector<const Json*> result;
            const auto* items = optional_member(manifest, key);
            if (!items) return result;
            need(items->type == Json::Type::array, "outputs_manifest_invalid: manifest list required");
            for (const auto& item : items->children) {
                need(item.type == Json::Type::object && python_index(optional_member(item, index)).has_value(),
                     "outputs_manifest_invalid: nonnegative strict integer index required");
                if (!id.empty()) need(!text(member(item, id)).empty(), "outputs_manifest_invalid: nonempty id/output required");
                result.push_back(&item);
            }
            return result;
        };
        const auto output_entries = list("outputs", "index", "id");
        auto series = list("series", "slot", "output");
        const auto constants = list("constants", "index", {});
        std::map<std::int64_t, const Json*> output_by_index, encoding_by_index;
        for (const auto* item : output_entries) output_by_index[*python_index(optional_member(*item, "index"))] = item;
        for (const auto* item : constants) encoding_by_index[*python_index(optional_member(*item, "index"))] = item;
        std::stable_sort(series.begin(), series.end(), [](const Json* a, const Json* b) {
            return *python_index(optional_member(*a, "slot")) < *python_index(optional_member(*b, "slot"));
        });
        const auto& saved_constants = member(saved, "constants");
        const auto& saved_hlines = member(saved, "hlines");
        need(saved_constants.type == Json::Type::array && saved_hlines.type == Json::Type::array
            && saved_constants.children.size() == head.records[4].fields.size(), "outputs constants/hlines metadata mismatch");
        {
            auto k = block(entry(4, 0));
            for (std::size_t c = 0; c < k.columns.size(); ++c) {
                const auto encoding = encoding_by_index.find(static_cast<std::int64_t>(c));
                const bool rgba = encoding != encoding_by_index.end() && rgba_encoding(*encoding->second);
                need(report_scalar(k.columns[c].value(0), rgba) == python_json(saved_constants.children[c]),
                     "outputs constants differ from K");
            }
        }
        std::size_t hline = 0;
        for (const auto* item : output_entries) {
            const auto* kind = optional_member(*item, "kind");
            if (!kind || kind->type != Json::Type::string || kind->text != "hline") continue;
            const auto* price = optional_member(*item, "price");
            const auto index = price && price->type == Json::Type::object
                ? python_index(optional_member(*price, "constant"), true) : std::nullopt;
            const auto token = index && static_cast<std::uint64_t>(*index) < saved_constants.children.size()
                ? python_json(saved_constants.children[static_cast<std::size_t>(*index)]) : "null";
            const auto expected = "{\"output\":" + python_json(member(*item, "id")) + ",\"price\":" + token + "}";
            need(hline < saved_hlines.children.size() && python_json(saved_hlines.children[hline++]) == expected,
                 "outputs hlines differ from manifest/K");
        }
        need(hline == saved_hlines.children.size(), "extra outputs hlines");
        emit("{");
        for (const auto key : {"schema_version","message_format","manifest_sha256","manifest"}) {
            if (std::string_view(key) != "schema_version") emit(",");
            emit(python_string(key)); emit(":"); emit(python_json(member(saved, key)));
        }
        emit(",\"bars\":{\"open_ms\":"); column_array(1, false);
        emit(",\"close_ms\":"); column_array(2, false);
        emit("},\"series\":[");
        bool comma = false;
        for (const auto* item : series) {
            const auto slot = *python_index(optional_member(*item, "slot"));
            need(static_cast<std::uint64_t>(slot) < head.records[2].fields.size() - 3, "outputs_manifest_invalid: unrecorded series slot");
            if (comma) emit(",");
            comma = true; emit("{\"slot\":"); emit(std::to_string(slot));
            emit(",\"output\":"); emit(python_json(member(*item, "output"))); emit(",\"values\":");
            column_array(static_cast<unsigned>(3 + slot), rgba_encoding(*item)); emit("}");
        }
        emit("],\"constants\":"); emit(python_json(saved_constants));
        emit(",\"hlines\":"); emit(python_json(saved_hlines)); emit(",\"events\":[");
        comma = false;
        rows(3, [&](const Record& row) {
            const auto it = output_by_index.find(row.values[1].as_i64());
            need(it != output_by_index.end(), "outputs_manifest_invalid: event output index missing");
            const auto& item = *it->second;
            const auto phase = row.values[6].as_i64();
            const auto phase_text = phase == 0 ? "batch" : phase == 1 ? "warmup" : phase == 2 ? "realtime" : std::to_string(phase);
            if (comma) emit(",");
            comma = true;
            emit("{\"sequence\":"); emit(report_scalar(row.values[0]));
            emit(",\"output\":"); emit(python_json(member(item, "id")));
            for (const auto c : {2u,3u,4u,5u}) {
                emit(","); emit(python_string(head.records[3].fields[c].name)); emit(":"); emit(report_scalar(row.values[c]));
            }
            emit(",\"phase\":"); emit(python_string(phase_text)); emit(",\"value\":"); emit(report_scalar(row.values[8]));
            emit(",\"message\":"); emit(report_scalar(row.values[10]));
            const auto* kind = optional_member(item, "kind");
            if (kind && kind->type == Json::Type::string && kind->text == "alert") {
                const auto* freq = optional_member(item, "freq");
                emit(",\"freq\":"); emit(python_json(freq ? *freq : absent));
            }
            emit("}");
        });
        emit("]}");
    };
    emit("{"); bool comma = false;
    for (const auto& child : metadata.children) {
        if (child.key == "sink_folds") continue;
        if (comma) emit(",");
        comma = true;
        emit(python_string(child.key)); emit(":");
        if (child.key == "outputs") outputs(child); else emit(python_json(child));
        if (child.key == "diagnostics") { emit(",\"trades\":"); record_array(1, 16); }
        if (child.key == "metrics") { emit(",\"equity_curve\":"); record_array(0, 3); }
    }
    emit("}\n");
}

ReportConversion Reader::to_report_json(const std::function<bool(std::string_view)>& output) {
    need(bool(output), "missing report conversion output callback");
    ReportConversion result;
    if (verify().scope != VerificationScope::whole_file) return result;
    try {
        Sha256 counted; std::uint64_t length = 0;
        state_->report_json([&](std::string_view bytes) { counted.update(bytes); length = add(length, bytes.size()); });
        state_->poll();
        Sha256 emitted; std::uint64_t written = 0;
        state_->report_json([&](std::string_view bytes) {
            if (!output(bytes)) fail("report conversion cancelled", Failure::cancelled);
            emitted.update(bytes); written = add(written, bytes.size());
        });
        integrity(written == length && counted.digest() == emitted.digest(), "report count/emit identity mismatch");
        result.scope = VerificationScope::whole_file; result.sha256 = counted.hex_digest(); result.bytes = length;
    } catch (const ReaderError& error) {
        if (error.failure() != Failure::cancelled) throw;
    }
    return result;
}

} // namespace v2
} // namespace report_sink
} // namespace pineforge
