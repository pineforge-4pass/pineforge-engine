// CR-397: bounded full-sink validation and E/T projection. No engine state.
#include "report_sink_json.hpp"
#include "sha256.hpp"
#include <pineforge/report_sink_writer.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <locale.h>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

namespace pineforge {
namespace report_sink {
namespace {
constexpr std::size_t line_cap = PF_REPORT_SINK_JSON_LINE_BYTES;
constexpr std::size_t window = PF_REPORT_SINK_JSON_WINDOW_BYTES;
constexpr std::int64_t i53 = INT64_C(9007199254740991);
using Request = pf_report_sink_json_request_v1;
using Result = pf_report_sink_json_result_v1;

struct Refusal {
    std::uint32_t status;
    const char* message;
    int number = 0;
};
[[noreturn]] void invalid(const char* message) {
    throw Refusal{PF_SINK_JSON_SOURCE_INVALID, message};
}
[[noreturn]] void unsupported() {
    throw Refusal{PF_SINK_JSON_NOT_IMPLEMENTED, "record or JSON shape not implemented"};
}

// The parser keeps scalars, never a DOM/history. The depth/key-table limits
// select strict fallback, not a new refusal. They do not limit Python's input
// language. Every successful native input has completed grammar/token checks.
enum class Kind { null_value, integer, real, boolean, string, array, object };
struct Value {
    Kind kind = Kind::null_value;
    std::int64_t integer = 0;
    double real = 0;
    std::string text;
    std::string_view token;
    bool surrogate = false;
};
class Json {
public:
    explicit Json(std::string_view body) : body_(body) {}
    char peek() { space(); return at_ < body_.size() ? body_[at_] : '\0'; }
    bool take(char byte) {
        space();
        if (at_ == body_.size() || body_[at_] != byte) return false;
        ++at_;
        return true;
    }
    void need(char byte) { if (!take(byte)) invalid("invalid UTF-8 or JSON"); }
    void end() { space(); if (at_ != body_.size()) invalid("invalid UTF-8 or JSON"); }
    Value value(unsigned depth = 0) {
        if (depth > 16) unsupported();
        const char first = peek();
        if (first == '"') return string();
        if (first == '[') {
            ++at_;
            if (!take(']')) {
                do { (void)value(depth + 1); } while (take(','));
                need(']');
            }
            Value result; result.kind = Kind::array; return result;
        }
        if (first == '{') {
            ++at_;
            std::array<std::string, 64> keys;
            std::size_t count = 0;
            bool duplicate = false;
            if (!take('}')) {
                do {
                    if (count == keys.size()) unsupported();
                    const auto key = string();
                    for (std::size_t i = 0; i < count; ++i)
                        duplicate |= keys[i] == key.text;
                    keys[count++] = key.text;
                    need(':');
                    (void)value(depth + 1);
                } while (take(','));
                need('}');
            }
            // Python's object_pairs_hook runs after all values in this object.
            if (duplicate) invalid("repeated JSON object key");
            Value result; result.kind = Kind::object; return result;
        }
        for (const auto literal : {std::string_view("null"), std::string_view("true"),
                                   std::string_view("false")}) {
            if (body_.substr(at_, literal.size()) == literal) {
                at_ += literal.size();
                Value result;
                result.kind = literal == "null" ? Kind::null_value : Kind::boolean;
                result.integer = literal == "true" ? 1 : 0;
                return result;
            }
        }
        if (first == '-' || (first >= '0' && first <= '9')) return number();
        invalid("invalid UTF-8 or JSON");
    }
    Value string() {
        need('"');
        Value result; result.kind = Kind::string;
        while (at_ < body_.size()) {
            const auto byte = static_cast<unsigned char>(body_[at_++]);
            if (byte == '"') return result;
            if (byte < 0x20) invalid("invalid UTF-8 or JSON");
            if (byte == '\\') {
                if (at_ == body_.size()) invalid("invalid UTF-8 or JSON");
                const char escape = body_[at_++];
                switch (escape) {
                    case '"': case '\\': case '/': result.text += escape; break;
                    case 'b': result.text += '\b'; break;
                    case 'f': result.text += '\f'; break;
                    case 'n': result.text += '\n'; break;
                    case 'r': result.text += '\r'; break;
                    case 't': result.text += '\t'; break;
                    case 'u': {
                        auto code = hex4();
                        if (code >= 0xd800 && code <= 0xdbff && at_ + 6 <= body_.size()
                            && body_.substr(at_, 2) == "\\u") {
                            const auto saved = at_;
                            at_ += 2;
                            const auto low = hex4();
                            if (low >= 0xdc00 && low <= 0xdfff)
                                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                            else at_ = saved;
                        }
                        result.surrogate |= code >= 0xd800 && code <= 0xdfff;
                        utf8(result.text, code);
                        break;
                    }
                    default: invalid("invalid UTF-8 or JSON");
                }
            } else if (byte < 0x80) {
                result.text += static_cast<char>(byte);
            } else {
                const std::size_t start = at_ - 1;
                const std::size_t length = byte >= 0xc2 && byte <= 0xdf ? 2
                    : byte >= 0xe0 && byte <= 0xef ? 3
                    : byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
                if (!length || length - 1 > body_.size() - at_) invalid("invalid UTF-8 or JSON");
                const auto second = static_cast<unsigned char>(body_[at_]);
                if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0)
                    || (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90))
                    invalid("invalid UTF-8 or JSON");
                for (std::size_t i = 1; i < length; ++i) {
                    const auto next = static_cast<unsigned char>(body_[at_++]);
                    if (next < 0x80 || next > 0xbf) invalid("invalid UTF-8 or JSON");
                }
                result.text.append(body_.substr(start, length));
            }
        }
        invalid("invalid UTF-8 or JSON");
    }
    void key(std::string_view expected) {
        if (string().text != expected) invalid("object keys differ");
        need(':');
    }
    std::string text() {
        auto item = value();
        if (item.kind != Kind::string || item.surrogate)
            invalid("expected a Unicode string without unpaired surrogates");
        return std::move(item.text);
    }
private:
    std::string_view body_;
    std::size_t at_ = 0;
    void space() {
        while (at_ < body_.size() && (body_[at_] == ' ' || body_[at_] == '\t'
               || body_[at_] == '\r' || body_[at_] == '\n')) ++at_;
    }
    unsigned hex4() {
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_ == body_.size()) invalid("invalid UTF-8 or JSON");
            const char byte = body_[at_++];
            const int digit = byte >= '0' && byte <= '9' ? byte - '0'
                : byte >= 'a' && byte <= 'f' ? byte - 'a' + 10
                : byte >= 'A' && byte <= 'F' ? byte - 'A' + 10 : -1;
            if (digit < 0) invalid("invalid UTF-8 or JSON");
            code = code * 16 + static_cast<unsigned>(digit);
        }
        return code;
    }
    static void utf8(std::string& out, unsigned code) {
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) {
            out += static_cast<char>(0xc0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 63));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 63));
            out += static_cast<char>(0x80 | (code & 63));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 63));
            out += static_cast<char>(0x80 | ((code >> 6) & 63));
            out += static_cast<char>(0x80 | (code & 63));
        }
    }
    Value number() {
        const auto start = at_;
        if (body_[at_] == '-') ++at_;
        if (at_ == body_.size()) invalid("invalid UTF-8 or JSON");
        const auto digits = at_;
        if (body_[at_] == '0') ++at_;
        else {
            if (body_[at_] < '1' || body_[at_] > '9') invalid("invalid UTF-8 or JSON");
            while (at_ < body_.size() && body_[at_] >= '0' && body_[at_] <= '9') ++at_;
        }
        bool floating = false;
        if (at_ < body_.size() && body_[at_] == '.') {
            floating = true; ++at_;
            const auto fraction = at_;
            while (at_ < body_.size() && body_[at_] >= '0' && body_[at_] <= '9') ++at_;
            if (fraction == at_) invalid("invalid UTF-8 or JSON");
        }
        if (at_ < body_.size() && (body_[at_] == 'e' || body_[at_] == 'E')) {
            floating = true; ++at_;
            if (at_ < body_.size() && (body_[at_] == '+' || body_[at_] == '-')) ++at_;
            const auto exponent = at_;
            while (at_ < body_.size() && body_[at_] >= '0' && body_[at_] <= '9') ++at_;
            if (exponent == at_) invalid("invalid UTF-8 or JSON");
        }
        Value result;
        result.token = body_.substr(start, at_ - start);
        if (!floating) {
            if (at_ - digits > 16) invalid("integer exceeds the exact JSON integer bound");
            const auto parsed = std::from_chars(result.token.data(),
                result.token.data() + result.token.size(), result.integer);
            if (parsed.ec != std::errc{} || result.integer < -i53 || result.integer > i53)
                invalid("integer exceeds the exact JSON integer bound");
            result.kind = Kind::integer;
        } else {
            result.kind = Kind::real;
            const auto parsed = std::from_chars(result.token.data(),
                result.token.data() + result.token.size(), result.real, std::chars_format::general);
            if (parsed.ec == std::errc::result_out_of_range) {
                // Python accepts underflow as a finite zero/subnormal; preserve
                // it before the later exact-repr check, in the C locale.
                const std::string token(result.token);
                const auto locale = ::newlocale(LC_NUMERIC_MASK, "C", nullptr);
                if (!locale) throw std::bad_alloc();
                char* end = nullptr;
                result.real = ::strtod_l(token.c_str(), &end, locale);
                ::freelocale(locale);
                if (end != token.c_str() + token.size()) invalid("invalid UTF-8 or JSON");
            } else if (parsed.ec != std::errc{}) invalid("invalid UTF-8 or JSON");
            if (!std::isfinite(result.real)) invalid("non-finite JSON number");
        }
        return result;
    }
};

std::string_view body(std::string_view line, bool strict, bool guard = false) {
    // Strict SinkReader tests LF before length. Raw guard caps pending bytes
    // before calling its decoder, so the two operations intentionally differ.
    if (guard && line.size() > line_cap) invalid("line exceeds 1 MiB including LF");
    if (line.empty() || line.back() != '\n') invalid("line does not end with LF");
    if (line.size() > line_cap) invalid("line exceeds 1 MiB including LF");
    line.remove_suffix(1);
    if (line.empty()) invalid("blank line");
    if (line.substr(0, 3) == "\xef\xbb\xbf") invalid("UTF-8 BOM is forbidden");
    if (strict) {
        bool quoted = false, escaped = false;
        for (const auto item : line) {
            const auto byte = static_cast<unsigned char>(item);
            if (quoted) {
                if (escaped) escaped = false;
                else if (byte == '\\') escaped = true;
                else if (byte == '"') quoted = false;
            } else if (byte == '"') quoted = true;
            else if (byte <= 0x20) invalid("whitespace or control byte outside a JSON string");
        }
    }
    return line;
}
void syntax(std::string_view text) {
    Json parser(text);
    (void)parser.value();
    parser.end();
}
struct Column { const char* field; const char* type; const char* unit; };
constexpr Column e_columns[] = {
    {"time_ms", "i64", "unix_ms"}, {"equity", "f64?", "account_ccy"},
    {"open_profit", "f64?", "account_ccy"}};
constexpr Column t_columns[] = {
    {"n","i64",""},{"side","str",""},{"entry_time","i64","unix_ms"},{"exit_time","i64","unix_ms"},
    {"entry_price","f64?","quote_ccy"},{"exit_price","f64?","quote_ccy"},{"qty","f64?","qty"},
    {"pnl","f64?","account_ccy"},{"pnl_pct","f64?","percent"},{"max_runup","f64?","account_ccy"},
    {"max_drawdown","f64?","account_ccy"},{"commission","f64?","account_ccy"},
    {"entry_bar_index","i64","script_bar"},{"exit_bar_index","i64","script_bar"},
    {"open_at_end","bool",""},{"entry_incarnation","u64",""},{"close_cause","i64",""},
    {"entry_id","str",""},{"exit_id","str",""},{"exit_comment","str",""}};
constexpr Column o_columns[] = {
    {"row","i64",""},{"open_ms","i64?","unix_ms"},{"close_ms","i64?","unix_ms"}};
constexpr Column v_columns[] = {
    {"sequence","u64",""},{"output_index","i64",""},{"bar_index","i64",""},
    {"bar_open_ms","i64?","unix_ms"},{"bar_close_ms","i64?","unix_ms"},
    {"ordinal_in_bar","i64",""},{"phase","i64",""},{"confirmed","i64",""},
    {"value","f64?",""},{"message_hash64","hex64",""},{"message","str?",""}};
constexpr Column x_columns[] = {
    {"timestamp","i64","unix_ms"},{"bar_index","i64",""},{"name","str",""},{"value","f64?",""}};
constexpr Column h_columns[] = {{"hash","hex64",""}};
constexpr std::string_view tag_order = "ETOVKXH";
struct Schema {
    std::array<bool, 7> present{};
    std::array<std::size_t, 7> fields{};
};
int tag_index(std::string_view tag) {
    if (tag.size() != 1) return -1;
    const auto at = tag_order.find(tag[0]);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}
Column column(int tag, std::size_t at) {
    switch (tag) {
        case 0: return e_columns[at];
        case 1: return t_columns[at];
        case 2: return at < 3 ? o_columns[at] : Column{"", "f64?", ""};
        case 3: return v_columns[at];
        case 4: return {"", "f64?", ""};
        case 5: return x_columns[at];
        default: return h_columns[at];
    }
}
std::size_t fixed_fields(int tag) {
    constexpr std::size_t counts[] = {3, 20, 3, 11, 0, 4, 1};
    return counts[tag];
}
Schema header(std::string_view line) {
    const auto text = body(line, true);
    syntax(text); // Complete JSON/token checks precede descriptor semantics.
    Schema schema;
    Json parser(text);
    parser.need('{'); parser.key("schema");
    if (parser.text() != "pf-report-sink") invalid("unknown report sink schema");
    parser.need(','); parser.key("version");
    const auto version = parser.value();
    if (version.kind != Kind::integer || version.integer != 1) invalid("expected integer version 1");
    parser.need(','); parser.key("run_id"); (void)parser.text();
    parser.need(','); parser.key("engine"); (void)parser.text();
    parser.need(','); parser.key("records"); parser.need('[');
    int previous = -1;
    if (!parser.take(']')) do {
        parser.need('{'); parser.key("type");
        const int tag = tag_index(parser.text());
        if (tag < 0 || tag <= previous)
            invalid("unknown, repeated or out-of-order record type");
        previous = tag;
        schema.present[tag] = true;
        for (int list = 0; list < 3; ++list) {
            parser.need(','); parser.key(list == 0 ? "fields" : list == 1 ? "types" : "units");
            parser.need('[');
            std::size_t count = 0;
            if (!parser.take(']')) {
                do {
                    if (tag != 2 && tag != 4 && count >= fixed_fields(tag))
                        invalid("record differs from fixed v1 descriptor");
                    const auto spec = column(tag, count);
                    const std::string expected = list == 0
                        ? (tag == 2 && count >= 3 ? "v" + std::to_string(count - 3)
                           : tag == 4 ? "c" + std::to_string(count) : spec.field)
                        : list == 1 ? spec.type : spec.unit;
                    if (parser.text() != expected)
                        invalid("record differs from fixed v1 descriptor");
                    ++count;
                } while (parser.take(','));
                parser.need(']');
            }
            if (list == 0) {
                if ((tag == 2 && count < 3)
                    || (tag != 2 && tag != 4 && count != fixed_fields(tag)))
                    invalid("record differs from fixed v1 descriptor");
                schema.fields[tag] = count;
            } else if (count != schema.fields[tag]) {
                invalid("record differs from fixed v1 descriptor");
            }
        }
        parser.need('}');
    } while (parser.take(','));
    if (previous >= 0) parser.need(']');
    parser.need('}'); parser.end();
    if (!schema.present[0] || !schema.present[1]) invalid("E and T descriptors are required");
    const int outputs = int(schema.present[2]) + int(schema.present[3]) + int(schema.present[4]);
    if (outputs != 0 && outputs != 3) invalid("O, V and K descriptors must be selected together");
    return schema;
}

// Both regular buffered fds and O_DIRECT fds use aligned 64 KiB pread windows.
// Like _UncachedSinkFile, reads are bounded by the size captured for this open.
// EOF probes never issue an unaligned one-byte pread on an O_DIRECT fd.
class alignas(4096) Input {
public:
    Input(int fd, std::uint64_t size) : fd_(fd), end_(size) {}
    std::uint64_t position() const { return position_; }
    std::string_view span() {
        if (position_ >= end_) return {};
        if (position_ < base_ || position_ - base_ >= size_) {
            if (loaded_ && position_ >= base_ && position_ - base_ == size_ && size_ < window)
                throw Refusal{PF_SINK_JSON_SOURCE_IO, "short report artifact read"};
            base_ = position_ / window * window;
            ssize_t got;
            do { got = ::pread(fd_, data_.data(), data_.size(), static_cast<off_t>(base_)); }
            while (got < 0 && errno == EINTR);
            if (got < 0) throw Refusal{PF_SINK_JSON_SOURCE_IO, "source read failed", errno};
            size_ = static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(got), end_ - base_));
            loaded_ = true;
            if (position_ - base_ >= size_)
                throw Refusal{PF_SINK_JSON_SOURCE_IO, "short report artifact read"};
        }
        const auto offset = static_cast<std::size_t>(position_ - base_);
        return {data_.data() + offset, size_ - offset};
    }
    void consume(std::size_t count) { position_ += count; }
    int get() {
        const auto bytes = span();
        if (bytes.empty()) return -1;
        const auto byte = static_cast<unsigned char>(bytes[0]);
        consume(1);
        return byte;
    }
private:
    int fd_;
    std::uint64_t position_ = 0, base_ = 0, end_;
    std::size_t size_ = 0;
    bool loaded_ = false;
    alignas(4096) std::array<char, window> data_{};
};
class alignas(4096) Output {
public:
    explicit Output(int fd) : fd_(fd) {}
    std::uint64_t size = 0;
    int error = 0;
    Sha256 hash;
    void append(std::string_view text) {
        hash.update(text);
        size += text.size();
        if (fd_ < 0 || error) return;
        while (!text.empty()) {
            const auto count = std::min(window - used_, text.size());
            std::memcpy(data_.data() + used_, text.data(), count);
            used_ += count;
            text.remove_prefix(count);
            if (used_ == window) flush();
            if (error) return;
        }
    }
    void flush() {
        if (fd_ < 0 || error || !used_) return;
        const auto aligned = (used_ + 4095) & ~std::size_t(4095);
        std::fill(data_.begin() + used_, data_.begin() + aligned, '\0');
        ssize_t written;
        do { written = ::pwrite(fd_, data_.data(), aligned, static_cast<off_t>(base_)); }
        while (written < 0 && errno == EINTR);
        if (written < 0) { error = errno; return; }
        if (static_cast<std::size_t>(written) != aligned) { error = EIO; return; }
        // size may include not-yet-copied bytes from this append; only expose
        // the completed prefix until the final flush.
        if (::ftruncate(fd_, static_cast<off_t>(base_ + used_)) < 0) { error = errno; return; }
#if defined(__APPLE__)
        if (::fsync(fd_) < 0) { error = errno; return; }
#else
        if (::fdatasync(fd_) < 0) { error = errno; return; }
#endif
        base_ += used_;
        used_ = 0;
    }
private:
    int fd_;
    std::size_t used_ = 0;
    std::uint64_t base_ = 0;
    alignas(4096) std::array<char, window> data_{};
};
struct alignas(4096) Context {
    const Request& request;
    Result& result;
    Input input;
    Output equity;
    Output trades;
    Schema schema;
    double cumulative = 0.0, peak = 0.0;
    std::array<char, line_cap + 1> line{};
    std::size_t length = 0;
    std::uint64_t number = 0, offset = 0;
    std::uint32_t stage = PF_SINK_JSON_PREPARE_HEADER;
    std::uint32_t category = PF_SINK_JSON_LINE, tag = 0, field = 0;
    bool reading_line = false;
    Context(const Request& r, Result& out)
        : request(r), result(out), input(r.sink_fd, out.source_size),
          equity(r.equity_fd), trades(r.trades_fd) {}
    std::string_view bytes() const { return {line.data(), length}; }
    void start(std::uint32_t next_stage) {
        ++number; offset = input.position(); length = 0;
        stage = next_stage; category = PF_SINK_JSON_LINE; tag = field = 0;
    }
    void rest(bool raw_guard = false) {
        while (length < line.size()) {
            const auto available = input.span();
            if (available.empty()) break;
            const auto* lf = static_cast<const char*>(
                std::memchr(available.data(), '\n', available.size()));
            const auto count = std::min(line.size() - length,
                lf ? static_cast<std::size_t>(lf - available.data()) + 1 : available.size());
            std::memcpy(line.data() + length, available.data(), count);
            length += count; input.consume(count);
            if (line[length - 1] == '\n') break;
        }
        if (raw_guard && length > line_cap) {
            category = PF_SINK_JSON_FRAMING;
            invalid("line exceeds 1 MiB including LF");
        }
    }
    void next(std::uint32_t next_stage) {
        start(next_stage);
        reading_line = true;
        rest();
        reading_line = false;
        if (!length) { category = PF_SINK_JSON_FRAMING; invalid("missing header or trailer"); }
    }
    void capture(const Refusal& refusal) {
        auto& error = result.error;
        result.status = refusal.status;
        error.stage = stage; error.category = category;
        if (refusal.status == PF_SINK_JSON_SOURCE_IO) error.category = PF_SINK_JSON_READ;
        if (refusal.status == PF_SINK_JSON_NOT_IMPLEMENTED) error.category = PF_SINK_JSON_UNSUPPORTED;
        error.line_number = number - (refusal.status == PF_SINK_JSON_SOURCE_IO
            && request.operation == PF_SINK_JSON_PREPARE && reading_line ? 1 : 0);
        error.byte_offset = offset;
        error.field_index = field; error.tag = tag; error.os_errno = refusal.number;
        std::snprintf(error.message, sizeof error.message, "%s", refusal.message);
        result.error_bytes = static_cast<std::uint32_t>(length);
        error.offending_bytes = static_cast<std::uint32_t>(std::min(length, sizeof error.offending));
        if (length) std::memcpy(request.error_buffer, line.data(), length);
        std::memcpy(error.offending, line.data(), error.offending_bytes);
    }
};
void field(const Value& value, std::string_view type) {
    if (value.kind == Kind::null_value) {
        if (type.back() == '?') return;
        invalid("null in a non-optional column");
    }
    if (type.back() == '?') type.remove_suffix(1);
    if (type == "i64" || type == "u64") {
        if (value.kind != Kind::integer || (type == "u64" && value.integer < 0))
            invalid(type == "u64" ? "invalid u64 column" : "invalid i64 column");
    } else if (type == "f64") {
        if (value.kind != Kind::real || f64(value.real) != value.token)
            invalid("f64 column is not the Python repr of its double");
    } else if (type == "bool") {
        if (value.kind != Kind::boolean) invalid("bool column requires true or false");
    } else {
        if (value.kind != Kind::string || value.surrogate)
            invalid("expected a Unicode string without unpaired surrogates");
        if (type == "hex64") {
            if (value.text.size() != 16) invalid("hex64 requires 16 lowercase hex digits");
            for (char byte : value.text)
                if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
                    invalid("hex64 requires 16 lowercase hex digits");
        }
    }
}
struct Row {
    // E/T have fixed arity. Optional wide O/K rows are walked again after
    // syntax, without retaining their variable number of values or columns.
    std::array<Value, 21> values;
    std::size_t count = 0;
    int tag = -1;
};
Row record(Context& context, bool verified_equity = false, bool guard = false) {
    const auto text = body(context.bytes(), !verified_equity, guard);
    Json parser(text);
    if (!parser.take('[')) {
        (void)parser.value(); parser.end();
        invalid("record must be a nonempty array with a string tag");
    }
    Row row;
    if (!parser.take(']')) {
        do {
            auto item = parser.value();
            if (row.count < row.values.size()) row.values[row.count] = std::move(item);
            ++row.count;
        } while (parser.take(','));
        parser.need(']');
    }
    parser.end(); // All token/JSON errors precede any field semantics.
    if (!row.count || row.values[0].kind != Kind::string)
        invalid("record must be a nonempty array with a string tag");
    row.tag = tag_index(row.values[0].text);
    if (row.tag < 0 || !context.schema.present[row.tag])
        invalid("record tag is absent from the header");
    context.tag = static_cast<unsigned char>(tag_order[row.tag]);
    if (row.count != context.schema.fields[row.tag] + 1)
        invalid("record field count differs from its descriptor");
    if (row.count <= row.values.size()) {
        for (std::size_t at = 1; at < row.count; ++at) {
            context.field = static_cast<std::uint32_t>(at);
            field(row.values[at], column(row.tag, at - 1).type);
        }
    } else {
        Json fields(text);
        fields.need('['); (void)fields.value();
        for (std::size_t at = 1; at < row.count; ++at) {
            fields.need(',');
            context.field = static_cast<std::uint32_t>(at);
            field(fields.value(), column(row.tag, at - 1).type);
        }
        fields.need(']'); fields.end();
    }
    if (row.tag == 1 && row.values[2].text != "long" && row.values[2].text != "short")
        invalid("trade side must be long or short");
    return row;
}
void scalar(Output& output, const Value& value) {
    switch (value.kind) {
        case Kind::null_value: output.append("null"); break;
        case Kind::integer: output.append(std::to_string(value.integer)); break;
        case Kind::real: output.append(f64(value.real)); break;
        case Kind::boolean: output.append(value.integer ? "true" : "false"); break;
        case Kind::string:
            // Only validated long/short is ever projected as a string.
            output.append("\""); output.append(value.text); output.append("\""); break;
        default: invalid("non-scalar projection");
    }
}
void projection(Output& output, const Row& row, std::size_t count, bool comma) {
    if (comma) output.append(",");
    output.append("{");
    for (std::size_t at = 0; at < count; ++at) {
        if (at) output.append(",");
        output.append("\""); output.append(column(row.tag, at).field); output.append("\":");
        scalar(output, row.values[at + 1]);
    }
    output.append("}");
}
void trade_failure(Context& context, std::uint32_t field_index) {
    // Trade fold failures outrank an earlier E result failure, but never a
    // source/trailer refusal. Later T failures cannot replace the first one.
    if (context.result.result_tag != 'T') {
        context.result.result_tag = 'T';
        context.result.result_field = field_index;
    }
}
void fold_trade(Context& context, const Row& row) {
    auto& result = context.result;
    result.diagnostics.trade_at_start |= row.values[13].integer <= 0;
    if (result.result_tag == 'T') return;
    auto& summary = result.summary;
    if (row.values[1].integer != summary.count_t + 1) {
        trade_failure(context, 1);
        return;
    }
    for (std::uint32_t at = 5; at <= 12; ++at) {
        if (row.values[at].kind == Kind::null_value) {
            summary.nonfinite_t_field = static_cast<std::int32_t>(at);
            trade_failure(context, at);
            return;
        }
    }
    const double pnl = row.values[8].real;
    // Match max(previous, pnl)/min(previous, pnl), including +/-0 ties.
    if (!summary.count_t || pnl > summary.best_trade) summary.best_trade = pnl;
    if (!summary.count_t || pnl < summary.worst_trade) summary.worst_trade = pnl;
    ++summary.count_t;
    summary.wins += pnl > 0;
    summary.losses += pnl < 0;
    context.cumulative += pnl;
    context.peak = std::max(context.peak, context.cumulative);
    const double drawdown = context.cumulative - context.peak;
    if (!std::isfinite(context.cumulative) || !std::isfinite(drawdown)) {
        trade_failure(context, 21); // Deferred summary.max_drawdown failure.
        return;
    }
    summary.max_drawdown = std::min(summary.max_drawdown, drawdown);
}
void fold_equity(Context& context, const Row& row) {
    pf_equity_point_t point{};
    point.time_ms = row.values[1].integer;
    for (std::uint32_t at = 2; at <= 3; ++at) {
        const bool absent = row.values[at].kind == Kind::null_value;
        const double value = absent ? std::numeric_limits<double>::quiet_NaN() : row.values[at].real;
        if (absent && !context.result.result_field) {
            context.result.result_tag = 'E';
            context.result.result_field = at;
        }
        if (at == 2) point.equity = value;
        else point.open_profit = value;
    }
    auto& diagnostics = context.result.diagnostics;
    if (!context.result.counts[0]) diagnostics.first = point;
    diagnostics.last = point;
}
void version(Json& parser) {
    const auto value = parser.value();
    if (value.kind != Kind::integer || value.integer != 1) invalid("expected integer version 1");
}
void trailer(Context& context, const Sha256& hash) {
    context.stage = PF_SINK_JSON_PREPARE_TRAILER;
    const auto prefix_digest = hash.hex_digest();
    std::memcpy(context.result.sha256_hex, prefix_digest.c_str(), 65);
    const auto text = body(context.bytes(), true);
    syntax(text);
    context.category = PF_SINK_JSON_TRAILER;
    Json parser(text);
    parser.need('{'); parser.key("trailer");
    if (parser.text() != "pf-report-sink") invalid("unknown trailer");
    parser.need(','); parser.key("version"); version(parser);
    parser.need(','); parser.key("counts"); parser.need('{');
    bool comma = false;
    for (int i = 0; i < 7; ++i) {
        if (!context.schema.present[i]) continue;
        if (comma) parser.need(',');
        comma = true;
        parser.key(tag_order.substr(static_cast<std::size_t>(i), 1));
        const auto value = parser.value();
        if (value.kind != Kind::integer || value.integer != context.result.counts[i]) {
            context.tag = static_cast<unsigned char>(tag_order[i]);
            invalid("trailer count mismatch");
        }
    }
    parser.need('}'); parser.need(','); parser.key("sha256");
    const auto digest = parser.text();
    parser.need('}'); parser.end();
    if (digest != hash.hex_digest()) invalid("trailer SHA-256 mismatch");
    if (context.input.get() >= 0) invalid("bytes after the trailer");
    std::memcpy(context.result.sha256_hex, digest.c_str(), 65);
}
void prepare(Context& context) {
    context.next(PF_SINK_JSON_PREPARE_HEADER);
    context.category = PF_SINK_JSON_HEADER;
    context.schema = header(context.bytes());
    std::memcpy(context.request.header_buffer, context.line.data(), context.length);
    context.result.header_bytes = static_cast<std::uint32_t>(context.length);
    Sha256 hash;
    hash.update(context.bytes());
    for (;;) {
        context.next(PF_SINK_JSON_PREPARE_RECORD);
        // SinkReader treats any decoded JSON object as the trailer, including
        // a syntactically valid but wrongly keyed object.
        if (!context.bytes().empty() && context.bytes()[0] == '{') {
            trailer(context, hash);
            break;
        }
        const auto row = record(context);
        const auto index = static_cast<std::size_t>(row.tag);
        if (context.result.counts[index] == i53)
            invalid("record count exceeds the exact JSON integer bound");
        if (index == 0) {
            fold_equity(context, row);
            projection(context.equity, row, 3, context.result.counts[0] != 0);
        } else if (index == 1) {
            fold_trade(context, row);
            projection(context.trades, row, 16, context.result.counts[1] != 0);
        }
        ++context.result.counts[index];
        hash.update(context.bytes());
    }
    context.result.complete = 1;
    context.result.summary.complete = context.result.diagnostics.complete = 1;
    // The stream count remains useful even after a deferred fold failure.
    context.result.summary.count_t = context.result.counts[1];
    context.result.diagnostics.count_e = context.result.counts[0];
    context.stage = PF_SINK_JSON_FRAGMENT;
    context.category = PF_SINK_JSON_WRITE;
    context.equity.flush();
    context.trades.flush();
    context.result.equity_json_bytes = context.equity.size;
    context.result.trades_json_bytes = context.trades.size;
    const auto digest = context.equity.hash.hex_digest();
    std::memcpy(context.result.equity_json_sha256_hex, digest.c_str(), 65);
    const auto trades_digest = context.trades.hash.hex_digest();
    std::memcpy(context.result.trades_json_sha256_hex, trades_digest.c_str(), 65);
    if (context.equity.error || context.trades.error) {
        context.length = 0;
        context.number = context.offset = 0;
        throw Refusal{PF_SINK_JSON_SCRATCH_IO, "fragment write failed",
            context.trades.error ? context.trades.error : context.equity.error};
    }
}

void guard(Context& context) {
    context.stage = PF_SINK_JSON_GUARD_IDENTITY;
    context.category = PF_SINK_JSON_IDENTITY;
    if (context.result.source_ino != context.request.expected_ino
        || context.result.source_size != context.request.expected_size)
        throw Refusal{PF_SINK_JSON_IDENTITY_DIFFERS,
            "the report sink changed after verification: file identity differs"};
    // This metadata was validated at prepare; reconstruct only its descriptor.
    context.schema = header({context.request.verified_header, context.request.verified_header_bytes});
    context.start(PF_SINK_JSON_GUARD_HEADER);
    context.rest(true);
    context.category = PF_SINK_JSON_FRAMING;
    if (!context.length) invalid("missing header or trailer");
    if (context.line[context.length - 1] != '\n') invalid("line does not end with LF");
    // Preserve this SAME read for the host's bounded, pinned _parse. It runs
    // before any later witness is surfaced; never reopen a changed header.
    std::memcpy(context.request.header_buffer, context.line.data(), context.length);
    context.result.header_bytes = static_cast<std::uint32_t>(context.length);
    Sha256 hash;
    hash.update(context.bytes());
    std::int64_t count = 0;
    for (;;) {
        context.start(PF_SINK_JSON_GUARD_RECORD);
        const int first = context.input.get();
        context.category = PF_SINK_JSON_FRAMING;
        if (first < 0) invalid("missing header or trailer");
        context.line[context.length++] = static_cast<char>(first);
        if (first == '{') {
            context.stage = PF_SINK_JSON_GUARD_TRAILER;
            context.rest(true);
            if (context.line[context.length - 1] != '\n') invalid("line does not end with LF");
            if (context.input.get() >= 0) invalid("bytes after the trailer");
            break; // No parsing/count/hash verification of the raw trailer.
        }
        while (context.length < 5 && context.line[context.length - 1] != '\n') {
            const int next = context.input.get();
            if (next < 0) invalid("line does not end with LF");
            context.line[context.length++] = static_cast<char>(next);
        }
        const char wanted = static_cast<char>(context.request.guard_tag);
        const char needle[] = {'[', '"', wanted, '"', ','};
        bool selected = context.length == 5
            && std::memcmp(context.line.data(), needle, sizeof needle) == 0;
        if (context.length >= 3 && std::string_view(context.line.data(), 3) == "[\"\\") {
            // Decode only the escaped first tag; skipped bodies are raw.
            bool escaped = false, closed = false;
            std::size_t at = 2;
            for (;;) {
                while (at < context.length) {
                    const char byte = context.line[at++];
                    if (byte == '\n') invalid("invalid UTF-8 or JSON: unterminated record tag");
                    if (escaped) escaped = false;
                    else if (byte == '\\') escaped = true;
                    else if (byte == '"') { closed = true; break; }
                }
                if (closed) break;
                const int next = context.input.get();
                if (next < 0) invalid("line does not end with LF");
                context.line[context.length++] = static_cast<char>(next);
                if (context.length > line_cap) invalid("line exceeds 1 MiB including LF");
            }
            context.category = PF_SINK_JSON_LINE; context.tag = 0;
            Json tag_parser({context.line.data() + 1, context.length - 1});
            const auto decoded = tag_parser.string();
            tag_parser.end();
            selected = decoded.text.size() == 1 && decoded.text[0] == wanted;
        }
        if (selected) {
            context.tag = static_cast<unsigned char>(wanted);
            context.category = PF_SINK_JSON_LINE;
            context.rest(true);
            try {
                (void)record(context, wanted == 'E', true);
            } catch (const Refusal& error) {
                // A selected E/T row with nested/object field values cannot be
                // a valid fixed-v1 row. Its captured line lets the pinned cold
                // parser recover exact JSON/field priority, without a reread.
                if (error.status == PF_SINK_JSON_NOT_IMPLEMENTED)
                    throw Refusal{PF_SINK_JSON_SOURCE_INVALID, error.message};
                throw;
            }
            ++count;
            hash.update(context.bytes());
        } else {
            hash.update(context.bytes());
            // No line cap, field parsing, JSON walk or retained skipped body.
            bool lf = context.line[context.length - 1] == '\n';
            while (!lf) {
                const auto bytes = context.input.span();
                if (bytes.empty()) invalid("line does not end with LF");
                const auto* newline = static_cast<const char*>(
                    std::memchr(bytes.data(), '\n', bytes.size()));
                const auto take = newline ? static_cast<std::size_t>(newline - bytes.data()) + 1
                                          : bytes.size();
                hash.update(bytes.substr(0, take));
                context.input.consume(take);
                lf = newline != nullptr;
            }
        }
    }
    context.stage = PF_SINK_JSON_GUARD_DIGEST;
    context.category = PF_SINK_JSON_DIGEST;
    const auto digest = hash.hex_digest();
    const auto index = context.request.guard_tag == 'E' ? 0 : 1;
    context.result.counts[index] = count;
    std::memcpy(context.result.sha256_hex, digest.c_str(), 65);
    if (digest != context.request.expected_sha256_hex)
        throw Refusal{PF_SINK_JSON_DIGEST_DIFFERS,
            "the report sink changed after verification: digest differs"};
    context.category = PF_SINK_JSON_COUNT;
    if (count != context.request.expected_counts[index])
        throw Refusal{PF_SINK_JSON_COUNT_DIFFERS, index == 0
            ? "the report sink E scan disagrees with its verified count"
            : "the report sink T scan disagrees with its verified count"};
    context.result.complete = 1;
}

struct Range { std::uintptr_t start; std::size_t size; };
bool overlaps(Range a, Range b) {
    return a.start < b.start ? b.start - a.start < a.size : a.start - b.start < b.size;
}
bool descriptor(const Request* request, const Result* result, struct stat& source) {
    if (!request || !result
        || reinterpret_cast<std::uintptr_t>(request) % alignof(Request)
        || reinterpret_cast<std::uintptr_t>(result) % alignof(Result)) return false;
    if (request->struct_size != sizeof(Request) || request->version != 1
        || result->struct_size != sizeof(Result) || result->version != 1 || request->flags
        || request->reserved_word || request->sink_fd < 0 || request->trades_fd < -1
        || request->equity_fd < -1) return false;
    for (const auto byte : request->reserved) if (byte) return false;
    if (request->operation != PF_SINK_JSON_PREPARE && request->operation != PF_SINK_JSON_GUARD)
        return false;
    if (!request->error_buffer || request->error_capacity < line_cap + 1
        || !request->header_buffer || request->header_capacity < line_cap + 1) return false;
    const bool prepare = request->operation == PF_SINK_JSON_PREPARE;
    if (prepare) {
        if (request->verified_header || request->verified_header_bytes || request->guard_tag)
            return false;
    } else {
        if (request->equity_fd != -1 || request->trades_fd != -1 || !request->verified_header
            || !request->verified_header_bytes || request->verified_header_bytes > line_cap
            || (request->guard_tag != 'E' && request->guard_tag != 'T')
            || request->expected_sha256_hex[64]) return false;
        for (int i = 0; i < 64; ++i) {
            const char byte = request->expected_sha256_hex[i];
            if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
        }
        for (const auto count : request->expected_counts) if (count < 0 || count > i53) return false;
    }
    const Range ranges[] = {
        {reinterpret_cast<std::uintptr_t>(request), sizeof *request},
        {reinterpret_cast<std::uintptr_t>(result), sizeof *result},
        {reinterpret_cast<std::uintptr_t>(request->header_buffer), request->header_capacity},
        {reinterpret_cast<std::uintptr_t>(request->error_buffer), request->error_capacity},
        {reinterpret_cast<std::uintptr_t>(request->verified_header),
            prepare ? 0 : request->verified_header_bytes}};
    for (const auto range : ranges)
        if (range.size && range.start > std::numeric_limits<std::uintptr_t>::max() - range.size)
            return false;
    for (std::size_t i = 0; i < 5; ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (ranges[i].size && ranges[j].size && overlaps(ranges[i], ranges[j])) return false;
    if (::fstat(request->sink_fd, &source) < 0 || !S_ISREG(source.st_mode) || source.st_size < 0)
        return false;
    const int input_flags = ::fcntl(request->sink_fd, F_GETFL);
    if (input_flags < 0 || (input_flags & O_ACCMODE) == O_WRONLY) return false;
    struct stat previous{};
    bool have_previous = false;
    for (int fd : {request->equity_fd, request->trades_fd}) {
        if (fd == -1) continue;
        struct stat target{};
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY || (flags & O_APPEND)
            || ::fstat(fd, &target) < 0 || !S_ISREG(target.st_mode) || target.st_size != 0
            || (target.st_dev == source.st_dev && target.st_ino == source.st_ino)
            || (have_previous && target.st_dev == previous.st_dev && target.st_ino == previous.st_ino))
            return false;
        previous = target; have_previous = true;
    }
    return true;
}
} // namespace

int json_v1(const Request* request, Result* result) noexcept {
    struct stat source{};
    if (!descriptor(request, result, source)) return -1;
    Result local{};
    local.struct_size = sizeof local; local.version = 1;
    local.error.struct_size = sizeof local.error; local.error.version = 1;
    local.summary.struct_size = sizeof local.summary; local.summary.version = 1;
    local.diagnostics.struct_size = sizeof local.diagnostics; local.diagnostics.version = 1;
    local.source_ino = static_cast<std::uint64_t>(source.st_ino);
    local.source_size = static_cast<std::uint64_t>(source.st_size);
    try {
        auto context = std::make_unique<Context>(*request, local);
        try {
            if (request->operation == PF_SINK_JSON_PREPARE) prepare(*context);
            else guard(*context);
        } catch (const Refusal& error) {
            context->capture(error);
        }
    } catch (const std::bad_alloc&) {
        local.status = PF_SINK_JSON_RESOURCE;
    } catch (...) {
        local.status = PF_SINK_JSON_INTERNAL;
    }
    *result = local;
    return 0;
}
} // namespace report_sink
} // namespace pineforge
