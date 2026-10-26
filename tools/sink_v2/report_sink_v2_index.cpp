#include "report_sink_v2_internal.hpp"

#include <cmath>
#include <charconv>
#include <memory>
#if defined(__linux__)
#include <sys/vfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif

namespace pineforge { namespace report_sink { namespace v2 {
namespace {

struct Tuple {
    std::uint64_t position;
    std::uint64_t key;
    bool valid;
};
constexpr std::size_t tuple_bytes = 17;

Tuple decode(const std::uint8_t* bytes) {
    return {get(bytes, 8), get(bytes + 9, 8), bytes[8] != 0};
}
void encode(std::uint8_t* bytes, const Tuple& t) {
    put(bytes, t.position, 8);
    bytes[8] = t.valid ? 1 : 0;
    put(bytes + 9, t.key, 8);
}
struct Less {
    bool descending;
    bool integer;
    bool operator()(const Tuple& a, const Tuple& b) const {
        // APP Q5 default: missing last in either direction; n/position
        // remains ascending even when the primary key descends.
        if (a.valid != b.valid) return a.valid;
        if (a.valid) {
            if (integer) {
                const auto x = signed_word(a.key), y = signed_word(b.key);
                if (x != y) return descending ? x > y : x < y;
            } else {
                const auto x = number(a.key), y = number(b.key);
                if (x != y) return descending ? x > y : x < y;
            }
        }
        return a.position < b.position;
    }
};

std::size_t length_of(const Table& table, std::uint32_t number) {
    return number + 1 == table.pages ? table.last_page_bytes : page_bytes;
}
Digest leaf_of(const std::array<std::uint8_t, page_bytes>& page, std::size_t length) {
    Sha256 hash;
    hash.update(std::string_view("PFS2-INDEX\0", 11));
    hash.update(view(page.data(), 24));
    hash.update(view(page.data() + page_head, length - page_head));
    return hash.digest();
}

// Logical payload I/O skips final IDX2 headers. All generations occupy the
// SAME slots; neither runs nor merge results are external files.
void positions(Output& out, const Table& table, std::uint64_t first,
               std::uint8_t* data, std::size_t count, bool writing) {
    const auto capacity = page_payload / table.width;
    while (count) {
        const auto n = std::min<std::size_t>(count, capacity - first % capacity);
        const auto at = entry_offset(table, first);
        if (writing) out.rewrite(at, view(data, n * table.width));
        else out.read(at, data, n * table.width);
        first += n;
        count -= n;
        data += n * table.width;
    }
}
class KeyReader {
    Output& out_;
    const Table& trades_;
    unsigned key_;
    report_sink::ColumnCodec& codec_;
    DecodeScratch& scratch_;
    Frame cached_{};
public:
    KeyReader(Output& out, const Table& trades, unsigned key,
              report_sink::ColumnCodec& codec, DecodeScratch& scratch)
        : out_(out), trades_(trades), key_(key), codec_(codec), scratch_(scratch) {}
    Tuple operator()(std::uint64_t position) {
        if (!cached_.n || position < cached_.rec0 || position - cached_.rec0 >= cached_.n) {
            std::uint64_t lo = 0, hi = trades_.count;
            std::array<std::uint8_t, 32> descriptor{};
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                out_.read(entry_offset(trades_, mid), descriptor.data(), descriptor.size());
                if (get(descriptor.data() + 24, 8) <= position) lo = mid + 1;
                else hi = mid;
            }
            if (!lo) invalid("rank position precedes T descriptors");
            out_.read(entry_offset(trades_, lo - 1), descriptor.data(), descriptor.size());
            cached_ = read_frame(out_, get(descriptor.data(), 8),
                get(descriptor.data(), 8) + get(descriptor.data() + 8, 4));
            if (cached_.tag != 'T' || position < cached_.rec0 || position - cached_.rec0 >= cached_.n)
                invalid("rank position outside T block");
            constexpr unsigned columns[] = {3, 7, 8, 9, 10};
            read_numeric(out_, cached_, columns[key_], key_ != 0, key_ != 0,
                         codec_, scratch_, 0);
        }
        auto value = numeric_at(scratch_.columns[0], static_cast<unsigned>(position - cached_.rec0));
        if (key_ == 4 && !value.null)
            value.word = bits(number(value.word) == 0.0 ? 0.0 : -std::fabs(number(value.word)));
        return {position, value.word, !value.null};
    }
};
class InPlaceMerge {
    Output& out_;
    const Table& table_;
    KeyReader& keys_;
    Less less_;
    RankArena arena_;
    Tuple at(std::uint64_t i) {
        std::array<std::uint8_t, 8> value{};
        out_.read(entry_offset(table_, i), value.data(), table_.width);
        return keys_(get(value.data(), table_.width));
    }
    void reverse(std::uint64_t first, std::uint64_t last) {
        const auto block = arena_.io_bytes / table_.width;
        while (last - first > 1) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(block, (last - first) / 2));
            auto* left = arena_.io[0];
            auto* right = arena_.io[1];
            auto* output = arena_.io[2];
            positions(out_, table_, first, left, n, false);
            positions(out_, table_, last - n, right, n, false);
            for (std::size_t j = 0; j < n; ++j)
                std::memcpy(output + j * table_.width,
                            right + (n - 1 - j) * table_.width, table_.width);
            positions(out_, table_, first, output, n, true);
            for (std::size_t j = 0; j < n; ++j)
                std::memcpy(output + j * table_.width,
                            left + (n - 1 - j) * table_.width, table_.width);
            positions(out_, table_, last - n, output, n, true);
            first += n;
            last -= n;
        }
    }
public:
    InPlaceMerge(Output& out, const Table& table, KeyReader& keys, Less less, RankArena arena)
        : out_(out), table_(table), keys_(keys), less_(less), arena_(arena) {}
    void merge(std::uint64_t first, std::uint64_t middle, std::uint64_t last) {
        if (first == middle || middle == last) return;
        const auto left_last = at(middle - 1), right_first = at(middle);
        if (!less_(right_first, left_last)) return;
        if (last - first == 2) { reverse(first, last); return; }
        std::uint64_t a, b;
        if (middle - first > last - middle) {
            a = first + (middle - first) / 2;
            const auto pivot = at(a);
            auto lo = middle, hi = last;
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (less_(at(mid), pivot)) lo = mid + 1;
                else hi = mid;
            }
            b = lo;
        } else {
            b = middle + (last - middle) / 2;
            const auto pivot = at(b);
            auto lo = first, hi = middle;
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (!less_(pivot, at(mid))) lo = mid + 1;
                else hi = mid;
            }
            a = lo;
        }
        const auto new_middle = a + (b - middle);
        reverse(a, middle);
        reverse(middle, b);
        reverse(a, b);
        // Both children shrink, including when either binary cut is empty.
        merge(first, a, new_middle);
        merge(new_middle, b, last);
    }
};

Digest subtree(const Output& out, const Table& table, std::uint64_t first, unsigned level,
               const Digest& last, std::array<Digest, 32>& frontier) {
    if (first >= table.pages) {
        auto hash = last;
        for (unsigned i = 0; i < level; ++i) hash = parent(hash, hash);
        return hash;
    }
    if (!level) {
        Digest leaf{};
        out.read(table.offset + first * page_bytes + 24, leaf.data(), leaf.size());
        return leaf;
    }
    frontier[level - 1] = subtree(out, table, first, level - 1, last, frontier);
    const auto right = subtree(out, table, first + (UINT64_C(1) << (level - 1)),
                               level - 1, last, frontier);
    return parent(frontier[level - 1], right);
}

// Bounded strict JSON validation, independent of the record reader. Retain
// exact caller bytes; parsing never reformats signed zero or selected fields.
class SummaryParser {
    std::string_view text_;
    std::size_t pos_ = 0;
    SinkFolds folds_;
    void ws() {
        if (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n'
                || text_[pos_] == '\r' || text_[pos_] == '\t'))
            invalid("summary JSON must be compact");
    }
    char take() {
        if (pos_ == text_.size()) invalid("short summary JSON");
        return text_[pos_++];
    }
    bool eat(char c) {
        ws();
        if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    void expect(char c) { if (!eat(c)) invalid("invalid summary JSON punctuation"); }
    unsigned hex4() {
        unsigned value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = take();
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else invalid("invalid summary Unicode escape");
            value = value * 16 + digit;
        }
        return value;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 128) out += static_cast<char>(cp);
        else if (cp < 2048) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 63));
        } else if (cp < 65536) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 63));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        }
    }
    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            const auto c = static_cast<unsigned char>(take());
            if (c == '"') return out;
            if (c < 32) invalid("control byte in summary string");
            if (c == '\\') {
                switch (take()) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (take() != '\\' || take() != 'u') invalid("unpaired summary surrogate");
                        const unsigned low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) invalid("unpaired summary surrogate");
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) invalid("unpaired summary surrogate");
                    utf8(out, cp);
                    break;
                }
                default: invalid("invalid summary escape");
                }
            } else if (c < 128) out += static_cast<char>(c);
            else {
                unsigned length = c >= 0xc2 && c <= 0xdf ? 2
                    : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
                if (!length) invalid("invalid summary UTF-8");
                out += static_cast<char>(c);
                for (unsigned i = 1; i < length; ++i) {
                    const auto next = static_cast<unsigned char>(take());
                    if (next < 0x80 || next > 0xbf || (i == 1
                            && ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0)
                                || (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90))))
                        invalid("invalid summary UTF-8");
                    out += static_cast<char>(next);
                }
            }
        }
    }
    void literal(std::string_view value) {
        if (text_.substr(pos_, value.size()) != value) invalid("invalid summary scalar");
        pos_ += value.size();
    }
    static bool tiny_decimal(std::string_view token) {
        // from_chars reports BOTH overflow and rounding-to-zero underflow as
        // out_of_range. Only overflow is nonfinite under section 9.1.
        std::int64_t digits = 0, before_point = 0, first_nonzero = -1;
        bool fractional = false;
        std::size_t i = token.front() == '-' ? 1 : 0;
        for (; i < token.size() && token[i] != 'e' && token[i] != 'E'; ++i) {
            if (token[i] == '.') { fractional = true; continue; }
            if (first_nonzero < 0 && token[i] != '0') first_nonzero = digits;
            ++digits;
            if (!fractional) ++before_point;
        }
        std::int64_t exponent = 0;
        bool negative = false;
        if (i < token.size()) {
            ++i;
            if (token[i] == '-' || token[i] == '+') negative = token[i++] == '-';
            for (; i < token.size(); ++i)
                exponent = std::min<std::int64_t>(2097152, exponent * 10 + token[i] - '0');
        }
        if (negative) exponent = -exponent;
        return first_nonzero < 0 || before_point - first_nonzero - 1 + exponent <= -324;
    }
    struct Number { double value; bool integer; };
    Number number_value() {
        const auto start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        auto digit = [&] { return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9'; };
        if (!digit()) invalid("invalid summary number");
        if (text_[pos_] == '0') ++pos_;
        else while (digit()) ++pos_;
        bool integer = true;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            integer = false;
            ++pos_;
            if (!digit()) invalid("invalid summary fraction");
            while (digit()) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            integer = false;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            if (!digit()) invalid("invalid summary exponent");
            while (digit()) ++pos_;
        }
        double value = 0.0;
        const auto parsed = std::from_chars(text_.data() + start, text_.data() + pos_, value);
        const bool underflow = parsed.ec == std::errc::result_out_of_range
            && parsed.ptr == text_.data() + pos_ && tiny_decimal(text_.substr(start, pos_ - start));
        if (underflow) value = text_[start] == '-' ? -0.0 : 0.0;
        if ((!underflow && parsed.ec != std::errc{}) || parsed.ptr != text_.data() + pos_ || !std::isfinite(value))
            invalid("non-finite summary number");
        if (integer) {
            std::int64_t exact = 0;
            const auto converted = std::from_chars(text_.data() + start, text_.data() + pos_, exact);
            if (converted.ec != std::errc{} || converted.ptr != text_.data() + pos_
                    || exact < -static_cast<std::int64_t>(safe) || exact > static_cast<std::int64_t>(safe))
                invalid("summary integer exceeds safe domain");
        }
        return {value, integer};
    }
    void sink_folds() {
        expect('{');
        constexpr std::array<std::string_view, 3> names{{
            "closed_count", "open_at_end_count", "open_at_end_pnl"}};
        for (unsigned i = 0; i < names.size(); ++i) {
            if (i) expect(',');
            if (string() != names[i]) invalid("invalid sink_folds member order");
            expect(':');
            const auto number = number_value();
            if (i < 2) {
                if (!number.integer || number.value < 0) invalid("invalid sink_folds count");
                const auto count = static_cast<std::uint64_t>(number.value);
                if (i == 0) folds_.closed_count = count;
                else folds_.open_at_end_count = count;
            } else folds_.open_at_end_pnl = number.value;
        }
        expect('}');
    }
    void value(unsigned depth, bool root = false, bool outputs = false) {
        ws();
        if (pos_ == text_.size()) invalid("short summary value");
        const auto c = text_[pos_];
        if (outputs && c != '{') invalid("outputs metadata must be an object");
        if (c == '{' || c == '[') {
            if (depth >= 64) invalid("summary depth exceeds 64");
            ++pos_;
            if (c == '{') {
                std::vector<std::string> keys;
                unsigned required = 0;
                if (!eat('}')) {
                    do {
                        auto key = string();
                        if (root) {
                            static constexpr std::array<std::string_view, 14> known{{
                                "engine", "input", "applied_inputs", "applied_overrides", "applied_runtime",
                                "elapsed_seconds", "summary", "metrics", "diagnostics", "fingerprint",
                                "sink_folds", "outputs", "report_window", "report_shape"}};
                            static constexpr std::array<std::string_view, 19> payload{{
                                "report_sink", "trades", "equity_curve", "records", "figure", "figures",
                                "marks", "pdf", "output_rows", "output_series", "output_events",
                                "broker_state_hash", "broker_state_hashes", "broker_hashes", "trace",
                                "traces", "hashes", "bars", "events"}};
                            if (std::find(payload.begin(), payload.end(), key) != payload.end())
                                invalid("unexpected summary payload field");
                            const auto found = std::find(known.begin(), known.end(), key);
                            const auto index = static_cast<unsigned>(found - known.begin());
                            if (index < 11) required |= 1u << index;
                        }
                        if (outputs) {
                            static constexpr std::array<std::string_view, 6> allowed{{
                                "schema_version", "message_format", "manifest_sha256", "manifest",
                                "constants", "hlines"}};
                            const auto found = std::find(allowed.begin(), allowed.end(), key);
                            if (found == allowed.end()) invalid("unexpected outputs metadata payload");
                            required |= 1u << static_cast<unsigned>(found - allowed.begin());
                        }
                        const bool child_outputs = root && key == "outputs";
                        const bool child_folds = root && key == "sink_folds";
                        keys.push_back(std::move(key));
                        expect(':');
                        if (root) {
                            const auto& name = keys.back();
                            if (pos_ == text_.size()) invalid("missing summary root value");
                            const char kind = text_[pos_];
                            if (name == "engine") {
                                if (kind != '"') invalid("summary engine must be a string");
                            } else if (name == "elapsed_seconds") {
                                if (kind != '-' && (kind < '0' || kind > '9'))
                                    invalid("summary elapsed_seconds must be a number");
                            } else if (name == "fingerprint") {
                                if (kind != '{' && kind != 'n') invalid("invalid summary fingerprint");
                            } else if (name == "input" || name == "applied_inputs" || name == "applied_overrides"
                                    || name == "applied_runtime" || name == "summary" || name == "metrics"
                                    || name == "diagnostics" || name == "sink_folds" || name == "outputs"
                                    || name == "report_window" || name == "report_shape") {
                                if (kind != '{') invalid("summary metadata member must be an object");
                            }
                        }
                        if (child_folds) sink_folds();
                        else value(depth + 1, false, child_outputs);
                    } while (eat(','));
                    expect('}');
                }
                std::sort(keys.begin(), keys.end());
                if (std::adjacent_find(keys.begin(), keys.end()) != keys.end())
                    invalid("duplicate summary key");
                if (root && required != 2047) invalid("missing required summary metadata");
                if (outputs && required != 63) invalid("missing outputs metadata field");
            } else if (!eat(']')) {
                do { value(depth + 1); } while (eat(','));
                expect(']');
            }
        } else if (c == '"') (void)string();
        else if (c == 't') literal("true");
        else if (c == 'f') literal("false");
        else if (c == 'n') literal("null");
        else (void)number_value();
    }
public:
    explicit SummaryParser(std::string_view text) : text_(text) {}
    SinkFolds parse() {
        ws();
        if (pos_ == text_.size() || text_[pos_] != '{') invalid("summary must be an object");
        value(0, true);
        ws();
        if (pos_ != text_.size()) invalid("trailing summary bytes");
        return folds_;
    }
};
} // namespace

SinkFolds validate_summary(std::string_view text) {
    check_summary_json_length(text.size());
    return SummaryParser(text).parse();
}

std::uint64_t table_bytes(const Table& table) {
    return table.pages ? std::uint64_t{table.pages - 1} * page_bytes + table.last_page_bytes : 0;
}
std::uint64_t entry_offset(const Table& table, std::uint64_t ordinal) {
    if (ordinal >= table.count) invalid("index entry outside table");
    const auto capacity = page_payload / table.width;
    return table.offset + (ordinal / capacity) * page_bytes + page_head + (ordinal % capacity) * table.width;
}
void plan_table(Table& table, std::uint64_t& next) {
    if (!table.count) return;
    const auto capacity = page_payload / table.width;
    const auto pages = (table.count + capacity - 1) / capacity;
    if (pages > UINT32_MAX) invalid("too many index pages");
    table.pages = static_cast<std::uint32_t>(pages);
    table.last_page_bytes = static_cast<std::uint32_t>(
        page_head + table.width * (table.count - (pages - 1) * capacity));
    table.offset = next;
    if (table_bytes(table) > safe - next) invalid("index exceeds safe offsets");
    next += table_bytes(table);
}

Frame read_frame(const Output& out, std::uint64_t offset, std::uint64_t end) {
    std::array<std::uint8_t, 32> head{};
    if (offset > end || end - offset < head.size()) invalid("short immutable block");
    out.read(offset, head.data(), head.size());
    if (std::memcmp(head.data(), "BLK2", 4) || get(head.data() + 5, 3))
        invalid("invalid immutable block framing");
    Frame f;
    f.offset = offset;
    f.tag = static_cast<char>(head[4]);
    f.ncols = static_cast<std::uint32_t>(get(head.data() + 8, 4));
    f.n = static_cast<std::uint32_t>(get(head.data() + 12, 4));
    f.rec0 = get(head.data() + 16, 8);
    const auto length = 32 + get(head.data() + 24, 4);
    const auto base = 32 + UINT64_C(48) * f.ncols;
    if (length > block_limit || length > end - offset || base > length || !f.n
            || f.n > (f.tag == 'I' ? 1048576u : 65536u) || f.rec0 > safe - f.n)
        invalid("immutable block exceeds limits");
    f.length = static_cast<std::uint32_t>(length);
    Crc framing;
    framing.update(view(head.data(), 28));
    std::array<std::uint8_t, 32> bytes{};
    std::uint64_t stored = base, decoded = base;
    for (unsigned j = 0; j < f.ncols; ++j) {
        out.read(offset + 32 + 16 * std::uint64_t{j}, bytes.data(), 16);
        framing.update(view(bytes.data(), 16));
        const auto raw = get(bytes.data() + 4, 4), size = get(bytes.data() + 8, 4);
        if (bytes[0] > 1 || bytes[1] > 6 || bytes[2] > 1 || bytes[3]
                || raw > column_limit || size > column_limit || (!bytes[0] && raw != size))
            invalid("invalid immutable column directory");
        stored += size;
        decoded += raw;
        if (stored > length || decoded > block_limit) invalid("immutable block budget");
    }
    Sha256 hash;
    hash.update(std::string_view("PFS2-BLOCK\0", 11));
    std::array<std::uint8_t, 13> identity{};
    identity[0] = head[4];
    put(identity.data() + 1, f.rec0, 8);
    put(identity.data() + 9, f.n, 4);
    hash.update(view(identity));
    for (unsigned j = 0; j < f.ncols; ++j) {
        out.read(offset + 32 + 16 * std::uint64_t{f.ncols} + 32 * std::uint64_t{j},
                 bytes.data(), 32);
        framing.update(view(bytes));
        hash.update(view(bytes));
    }
    if (stored != length || framing.digest() != get(head.data() + 28, 4))
        invalid("immutable block CRC/length");
    f.decoded = static_cast<std::uint32_t>(decoded);
    f.digest = hash.digest();
    return f;
}

void read_numeric(const Output& out, const Frame& f, unsigned column, bool nullable, bool floating,
                  report_sink::ColumnCodec& codec, DecodeScratch& scratch, unsigned destination) {
    if (column >= f.ncols || destination >= scratch.columns.size()) invalid("numeric column outside schema");
    std::uint64_t at = f.offset + 32 + 48 * std::uint64_t{f.ncols};
    std::array<std::uint8_t, 16> dir{};
    for (unsigned j = 0; j <= column; ++j) {
        out.read(f.offset + 32 + 16 * std::uint64_t{j}, dir.data(), dir.size());
        if (j != column) at += get(dir.data() + 8, 4);
    }
    const auto raw_length = static_cast<std::size_t>(get(dir.data() + 4, 4));
    const auto stored_length = static_cast<std::size_t>(get(dir.data() + 8, 4));
    auto exact_size = [](Bytes& b, std::size_t n) {
        if (b.capacity() < n) Bytes(n).swap(b);
        else b.resize(n);
    };
    exact_size(scratch.stored, stored_length);
    out.read(at, scratch.stored.data(), stored_length);
    if (dir[0]) codec.decompress_column(view(scratch.stored), raw_length, scratch.raw);
    else {
        exact_size(scratch.raw, raw_length);
        std::copy(scratch.stored.begin(), scratch.stored.end(), scratch.raw.begin());
    }
    if (scratch.raw.size() != raw_length || (dir[2] && !nullable))
        invalid("numeric column length/flags");
    auto& values = scratch.columns[destination];
    exact_size(values, std::size_t{f.n} * 9);
    if (scratch.capacity() > 2 * block_limit) invalid("reused decoder exceeds two-Umax bound");
    const auto& raw = scratch.raw;
    std::size_t pos = dir[2] ? (f.n + 7) / 8 : 0;
    if (pos > raw.size()) invalid("short numeric null bitmap");
    const auto is_null = [&](unsigned row) { return dir[2] && ((raw[row / 8] >> (row % 8)) & 1); };
    unsigned m = f.n;
    for (unsigned row = 0; row < f.n; ++row) m -= is_null(row) ? 1u : 0u;
    auto uv = [&]() {
        std::uint64_t value = 0;
        for (unsigned j = 0; j < 8; ++j) {
            if (pos == raw.size()) invalid("short numeric varint");
            const auto b = raw[pos++];
            value |= std::uint64_t{b & 127u} << (7 * j);
            if (!(b & 128)) {
                if ((j && !b) || value > safe) invalid("nonminimal/unsafe numeric varint");
                return value;
            }
        }
        invalid("long numeric varint");
    };
    auto zz = [&]() {
        const auto z = uv();
        return z & 1 ? -static_cast<std::int64_t>(z / 2) - 1 : static_cast<std::int64_t>(z / 2);
    };
    const unsigned enc = dir[1];
    if ((!floating && enc > 1) || (floating && enc != 2 && enc != 3))
        invalid("invalid finalization numeric encoding");
    unsigned k = 0;
    std::int64_t current = 0;
    std::uint64_t gcd = 0, previous = 0;
    if (enc == 3) {
        if (pos == raw.size() || (k = raw[pos++]) > 12) invalid("decimal scale exceeds 12");
    }
    if (enc == 1 || enc == 3) {
        if (!m) invalid("empty numeric DELTA");
        current = zz();
        gcd = uv();
        if (!gcd) invalid("zero numeric GCD");
    }
    constexpr double power[] = {1., 10., 100., 1000., 10000., 100000., 1000000.,
        10000000., 100000000., 1000000000., 10000000000., 100000000000., 1000000000000.};
    const auto body = pos;
    if (enc == 2 && raw.size() - body != 8 * std::size_t{m}) invalid("short XORSHUF");
    Crc checksum;
    Sha256 hash;
    checksum.update(std::string_view("PFS2-COL\0", 9));
    hash.update(std::string_view("PFS2-COL\0", 9));
    std::array<std::uint8_t, 17> identity{};
    identity[0] = static_cast<std::uint8_t>(f.tag);
    put(identity.data() + 1, column, 4);
    put(identity.data() + 5, f.rec0, 8);
    put(identity.data() + 13, f.n, 4);
    checksum.update(view(identity));
    hash.update(view(identity));
    unsigned index = 0;
    for (unsigned row = 0; row < f.n; ++row) {
        auto* value = values.data() + 9 * std::size_t{row};
        value[0] = is_null(row) ? 0 : 1;
        std::uint64_t word = 0;
        if (value[0]) {
            if (!enc) {
                if (raw.size() - pos < 8) invalid("short PLAIN64");
                word = get(raw.data() + pos, 8);
                pos += 8;
            } else if (enc == 2) {
                std::uint64_t x = 0;
                for (unsigned plane = 0; plane < 8; ++plane)
                    x |= std::uint64_t{raw[body + std::size_t{plane} * m + index]} << (8 * plane);
                word = previous ^ x;
                previous = word;
            } else {
                if (index) {
                    const auto q = zz();
                    const auto magnitude = static_cast<std::uint64_t>(q < 0 ? -q : q);
                    if (magnitude >= (UINT64_C(1) << 52)
                            || (magnitude && gcd > (UINT64_C(1) << 53) / magnitude))
                        invalid("numeric DELTA product domain");
                    const auto delta = static_cast<std::int64_t>(magnitude * gcd);
                    // Both previous and product fit +/-2^53: no signed overflow.
                    current += q < 0 ? -delta : delta;
                }
                if (current <= -INT64_C(4503599627370496) || current >= INT64_C(4503599627370496))
                    invalid("numeric DELTA value domain");
                if (enc == 3) {
                    if (current < -INT64_C(1125899906842623) || current > INT64_C(1125899906842623))
                        invalid("decimal integer domain");
                    word = bits(div64(static_cast<double>(current), power[k]));
                } else word = static_cast<std::uint64_t>(current);
            }
            if (floating ? !std::isfinite(number(word))
                         : signed_word(word) < -static_cast<std::int64_t>(safe)
                           || signed_word(word) > static_cast<std::int64_t>(safe))
                invalid("numeric value domain");
            ++index;
        }
        put(value + 1, word, 8);
        const auto canonical = value[0] ? view(value + (nullable ? 0 : 1), nullable ? 9 : 8) : view(value, 1);
        checksum.update(canonical);
        hash.update(canonical);
    }
    if (enc == 2) pos = raw.size();
    if (pos != raw.size() || checksum.digest() != get(dir.data() + 12, 4))
        invalid("finalization column CRC");
    Digest expected{};
    out.read(f.offset + 32 + 16 * std::uint64_t{f.ncols} + 32 * std::uint64_t{column},
             expected.data(), expected.size());
    if (hash.digest() != expected) invalid("finalization column SHA");
}

void build_rank(Output& out, Table& table, const Table& trades, unsigned key, bool descending,
                report_sink::ColumnCodec& codec, DecodeScratch& scratch, RankArena arena) {
    if (!table.count) return;
    const Less less{descending, key == 0};
    KeyReader keys(out, trades, key, codec, scratch);
    auto* packed = arena.tuples;
    auto tuple = [&](std::size_t i) { return decode(packed + i * tuple_bytes); };
    auto swap = [&](std::size_t a, std::size_t b) {
        const auto x = tuple(a), y = tuple(b);
        encode(packed + a * tuple_bytes, y);
        encode(packed + b * tuple_bytes, x);
    };
    // Packed in-place heapsort; no padded Tuple vector or sorting allocation.
    auto sift = [&](std::size_t root, std::size_t n) {
        while (root < n / 2) {
            auto child = 2 * root + 1;
            if (child + 1 < n && less(tuple(child), tuple(child + 1))) ++child;
            if (!less(tuple(root), tuple(child))) break;
            swap(root, child);
            root = child;
        }
    };
    for (std::uint64_t first = 0; first < table.count;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(arena.capacity, table.count - first));
        for (std::size_t j = 0; j < n; ++j) encode(packed + j * tuple_bytes, keys(first + j));
        for (std::size_t j = n / 2; j; --j) sift(j - 1, n);
        for (std::size_t j = n; j > 1; --j) { swap(0, j - 1); sift(0, j - 1); }
        for (std::size_t j = 0; j < n;) {
            const auto count = std::min<std::size_t>(arena.io_bytes / table.width, n - j);
            for (std::size_t p = 0; p < count; ++p)
                put(arena.io[2] + p * table.width, tuple(j + p).position, table.width);
            positions(out, table, first + j, arena.io[2], count, true);
            j += count;
        }
        first += n;
    }
    InPlaceMerge merger(out, table, keys, less, arena);
    for (std::uint64_t run = arena.capacity; run < table.count; run *= 2) {
        for (std::uint64_t first = 0; first < table.count; first += 2 * run)
            merger.merge(first, std::min(table.count, first + run), std::min(table.count, first + 2 * run));
    }
}

void seal_table(Table& table, unsigned id, Output& out, FinalArena& arena) {
    if (!table.count) return;
    unsigned depth = 0;
    for (std::uint64_t n = 1; n < table.pages; n *= 2) ++depth;
    auto& page = arena.pages[0];
    const auto capacity = page_payload / table.width;
    for (std::uint32_t i = 0; i < table.pages; ++i) {
        const auto length = length_of(table, i);
        page.fill(0);
        out.read(table.offset + std::uint64_t{i} * page_bytes + page_head,
                 page.data() + page_head, length - page_head);
        std::memcpy(page.data(), "IDX2", 4);
        page[4] = static_cast<std::uint8_t>(id);
        page[6] = static_cast<std::uint8_t>(depth);
        put(page.data() + 8, i, 4);
        put(page.data() + 12, std::min<std::uint64_t>(capacity, table.count - std::uint64_t{i} * capacity), 4);
        put(page.data() + 16, table.width, 4);
        put(page.data() + 20, table.pages, 4);
        const auto leaf = leaf_of(page, length);
        std::memcpy(page.data() + 24, leaf.data(), 32);
        out.rewrite(table.offset + std::uint64_t{i} * page_bytes, view(page.data(), page_head));
    }
    Digest last{};
    out.read(table.offset + std::uint64_t{table.pages - 1} * page_bytes + 24, last.data(), 32);
    table.root = subtree(out, table, 0, depth, last, arena.frontier);
    // Reduce disjoint sibling subtrees once per level, then distribute that
    // scalar hash to their real pages: O(P log P), no retained tree.
    for (unsigned level = 0; level < depth; ++level) {
        const auto span = UINT64_C(1) << level;
        for (std::uint64_t first = 0; first < table.pages; first += 2 * span) {
            const auto left = subtree(out, table, first, level, last, arena.frontier);
            const auto right = subtree(out, table, first + span, level, last, arena.frontier);
            for (auto i = first; i < std::min<std::uint64_t>(table.pages, first + 2 * span); ++i)
                out.rewrite(table.offset + i * page_bytes + 56 + 32 * level,
                            view(i < first + span ? right : left));
        }
    }
    for (std::uint32_t i = 0; i < table.pages; ++i) {
        const auto length = length_of(table, i);
        out.read(table.offset + std::uint64_t{i} * page_bytes, page.data(), length);
        Crc checksum;
        checksum.update(view(page.data(), 4092));
        checksum.update(view(page.data() + page_head, length - page_head));
        put(page.data() + 4092, checksum.digest(), 4);
        out.rewrite(table.offset + std::uint64_t{i} * page_bytes + 4092, view(page.data() + 4092, 4));
    }
}

std::uint32_t scratch_backing(const std::string& directory, int staging_fd) {
    // No work files are used, but inspect the resolved scratch directory.
    const int fd = staging_fd >= 0 ? ::fcntl(staging_fd, F_DUPFD_CLOEXEC, 0)
        : ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) io("open scratch directory");
    struct stat status{};
    if (::fstat(fd, &status) < 0 || (staging_fd >= 0 ? !S_ISREG(status.st_mode) : !S_ISDIR(status.st_mode))) {
        const int error = errno ? errno : ENOTDIR;
        (void)::close(fd);
        io("stat scratch directory", error);
    }
    std::uint32_t flags = 2; // unrecognized/opaque backing is UNKNOWN
#if defined(__linux__)
    struct statfs fs{};
    if (::fstatfs(fd, &fs) == 0) {
        switch (static_cast<unsigned long>(fs.f_type)) {
        case 0x01021994UL: case 0x858458f6UL: flags = 1; break; // tmpfs/ramfs
        case 0xef53UL: case 0x58465342UL: case 0x9123683eUL: case 0x2fc12fc1UL:
            flags = 0; break; // ext, xfs, btrfs, zfs
        default: break; // overlay, FUSE, network and unknown remain opaque
        }
    }
#elif defined(__APPLE__)
    struct statfs fs{};
    if (::fstatfs(fd, &fs) == 0) {
        if (!std::strcmp(fs.f_fstypename, "tmpfs") || !std::strcmp(fs.f_fstypename, "ramfs"))
            flags = 1;
        // APFS can be on a RAM disk; its type alone does not prove backing.
    }
#endif
    if (::close(fd) < 0) io("close scratch directory");
    return flags;
}

}}} // namespace pineforge::report_sink::v2
