#include "report_sink_transcode_v2.hpp"

#include "report_sink_reader.hpp"
#include "report_sink_v2_codec.hpp"
#include "report_sink_v2_support.hpp"
#include "sha256.hpp"
#include "report_sink_phase_log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cfenv>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pineforge { namespace report_sink {
namespace {
namespace L = v2::layout;
constexpr std::string_view tags = "ETOVKXH";

[[noreturn]] void refuse(int status, const std::string& message) {
    throw TranscodeError(status, message);
}
void need(bool condition, const char* message) {
    if (!condition) refuse(3, message);
}
[[noreturn]] void io(const char* operation) {
    const int number = errno;
    refuse(number == EFBIG ? 9 : 5,
        std::string(operation) + ": " + std::error_code(number, std::generic_category()).message());
}

class Fd {
    int fd_ = -1;
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) (void)::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return fd_; }
};

// No seek, pread, mmap, or stream-sized buffer on either input descriptor.
class Input {
    int fd_;
    std::array<char, 65536> buffer_{};
    std::size_t at_ = 0, end_ = 0;
    std::optional<std::uint64_t> cap_;
    std::uint64_t* observed_;
    PhaseLog* phase_log_;
    bool fill() {
        if (at_ != end_) return true;
        auto length = buffer_.size();
        if (cap_ && bytes <= *cap_ && *cap_ - bytes < length)
            length = static_cast<std::size_t>(*cap_ - bytes) + 1;
        ssize_t n;
        do { n = ::read(fd_, buffer_.data(), length); } while (n < 0 && errno == EINTR);
        if (n < 0) io("read input");
        if (n > 0 && phase_log_) {
            phase_log_->mark("first_input_byte");
            phase_log_ = nullptr; // this boundary is observed once, not per record
        }
        at_ = 0;
        end_ = static_cast<std::size_t>(n);
        if (end_ > L::safe - bytes) refuse(5, "input exceeds safe byte-count domain");
        bytes += end_;
        if (observed_) *observed_ = bytes;
        if (cap_ && bytes > *cap_) refuse(8, "input exceeds --max-bytes");
        return end_ != 0;
    }
public:
    std::uint64_t bytes = 0;
    explicit Input(int fd, std::optional<std::uint64_t> cap = {}, std::uint64_t* observed = nullptr,
                   PhaseLog* phase_log = nullptr)
        : fd_(fd), cap_(cap), observed_(observed), phase_log_(phase_log) {}
    int peek() { return fill() ? static_cast<unsigned char>(buffer_[at_]) : -1; }
    char take() {
        if (!fill()) refuse(3, "truncated report JSON");
        return buffer_[at_++];
    }
    bool line(std::string& out) {
        out.clear();
        while (fill()) {
            const auto* begin = buffer_.data() + at_;
            const auto* lf = static_cast<const char*>(std::memchr(begin, '\n', end_ - at_));
            const auto count = lf ? static_cast<std::size_t>(lf - begin) + 1 : end_ - at_;
            if (count > L::max_line - out.size()) refuse(3, "line exceeds 1 MiB including LF");
            out.append(begin, count);
            at_ += count;
            if (lf) return true;
        }
        // Even a half-written last line is a child that never sealed a trailer.
        if (!out.empty()) refuse(6, "stream ended without a complete trailer");
        return false;
    }
};

// Parser for the CANONICAL v1 producer spelling. The independent Python
// reader is exercised against the same accepted and corrupt streams in tests.
// Canonical re-spelling checks are essential: v2 cannot preserve alternative
// JSON escape spellings or integer -0 in its byte-exact to_v1 inverse.
class Cursor {
    std::string_view text_;
    std::size_t at_ = 0;
    unsigned hex4() {
        unsigned word = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = take();
            const int digit = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            need(digit >= 0, "invalid Unicode escape");
            word = word * 16 + static_cast<unsigned>(digit);
        }
        return word;
    }
    static void scalar(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
        }
    }
    std::string_view token() {
        const auto begin = at_;
        while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != ']' && text_[at_] != '}') ++at_;
        need(at_ != begin, "missing scalar");
        return text_.substr(begin, at_ - begin);
    }
public:
    explicit Cursor(std::string_view text) : text_(text) {}
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    char take() {
        need(at_ < text_.size(), "truncated JSON line");
        return text_[at_++];
    }
    bool eat(char c) {
        if (peek() != c) return false;
        ++at_; return true;
    }
    void expect(std::string_view literal) {
        need(text_.substr(at_, literal.size()) == literal, "unexpected JSON token or member order");
        at_ += literal.size();
    }
    void end() const { need(at_ == text_.size(), "trailing JSON bytes"); }
    std::string string(bool canonical = true) {
        const auto begin = at_;
        expect("\"");
        std::string out;
        for (;;) {
            const unsigned char c = static_cast<unsigned char>(take());
            if (c == '"') break;
            need(c >= 0x20, "control byte in string");
            if (c != '\\') { out += static_cast<char>(c); continue; }
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
                    expect("\\u");
                    const auto low = hex4();
                    need(low >= 0xdc00 && low <= 0xdfff, "unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else need(cp < 0xdc00 || cp > 0xdfff, "unpaired surrogate");
                scalar(out, cp); break;
            }
            default: refuse(3, "invalid JSON string escape");
            }
        }
        if (canonical)
            need(escaped_string(out) == text_.substr(begin, at_ - begin), "noncanonical or invalid UTF-8 string");
        return out;
    }
    std::int64_t integer() {
        const auto raw = token();
        std::int64_t value = 0;
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        need(parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size()
            && value >= -static_cast<std::int64_t>(L::safe) && value <= static_cast<std::int64_t>(L::safe)
            && std::to_string(value) == raw, "noncanonical or unsafe integer");
        return value;
    }
    double floating() {
        const auto raw = token();
        if (raw == "null") return std::numeric_limits<double>::quiet_NaN();
        double value = 0;
        const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        need(parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size() && std::isfinite(value)
            && f64(value) == raw, "f64 is not Python repr of its binary64 value");
        return value;
    }
    std::int64_t i() { expect(","); return integer(); }
    std::uint64_t u() {
        const auto value = i();
        need(value >= 0, "negative u64");
        return static_cast<std::uint64_t>(value);
    }
    template<class T> T narrow() {
        const auto value = i();
        need(value >= static_cast<std::int64_t>(std::numeric_limits<T>::min())
            && value <= static_cast<std::int64_t>(std::numeric_limits<T>::max()), "integer outside record ABI domain");
        return static_cast<T>(value);
    }
    double f() { expect(","); return floating(); }
    std::string s() { expect(","); return string(); }
    std::int64_t time() {
        expect(",");
        if (peek() == 'n') { expect("null"); return INT64_MIN; }
        return integer();
    }
    bool boolean() {
        expect(",");
        if (peek() == 't') { expect("true"); return true; }
        expect("false"); return false;
    }
    std::uint64_t hex() {
        const auto raw = s();
        need(raw.size() == 16, "hex64 requires 16 digits");
        std::uint64_t value = 0;
        for (char c : raw) {
            need((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "hex64 requires lowercase digits");
            value = (value << 4) | static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        return value;
    }
};

struct Header {
    RecordSet records{false, false, false, 0, 0};
    std::string run_id;
    std::array<bool, 7> present{};
};
// Share the fixed field declarations with the independent reference reader.
const L::FieldSpec* fixed_fields(char tag, std::size_t& count) {
    switch (tag) {
    case 'E': count = std::size(L::equity); return L::equity;
    case 'T': count = std::size(L::trade); return L::trade;
    case 'O': count = std::size(L::output); return L::output;
    case 'V': count = std::size(L::event); return L::event;
    case 'K': count = 0; return nullptr;
    case 'X': count = std::size(L::trace); return L::trace;
    case 'H': count = std::size(L::hash); return L::hash;
    default: refuse(3, "unknown record descriptor");
    }
}
Header header(std::string_view text) {
    Cursor p(text);
    Header h;
    p.expect("{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":");
    h.run_id = p.string();
    need(h.run_id.find('\0') == std::string::npos, "NUL in run id is not representable by the writer");
    p.expect(",\"engine\":");
    (void)p.string();
    p.expect(",\"records\":[");
    std::size_t previous = 0;
    bool first = true;
    do {
        p.expect("{\"type\":");
        const auto name = p.string();
        need(name.size() == 1, "unknown record type");
        const auto index = tags.find(name[0]);
        need(index != std::string_view::npos && (first || index > previous), "repeated or reordered descriptor");
        first = false;
        previous = index;
        h.present[index] = true;
        std::size_t fixed = 0;
        const auto* spec = fixed_fields(name[0], fixed);
        p.expect(",\"fields\":[");
        std::size_t count = 0;
        if (!p.eat(']')) {
            do {
                const auto field = p.string();
                if (count < fixed) need(field == spec[count].name, "wrong fixed field name");
                else {
                    need(name == "O" || name == "K", "extra field in descriptor");
                    const auto prefix = name == "O" ? "v" : "c";
                    need(field == prefix + std::to_string(count - fixed), "noncontiguous variable field");
                    need(count - fixed < L::max_slots, "too many output columns");
                }
                ++count;
            } while (p.eat(','));
            p.expect("]");
        }
        need(count >= fixed, "missing descriptor fields");
        for (unsigned part = 0; part < 2; ++part) {
            p.expect(part == 0 ? ",\"types\":[" : ",\"units\":[");
            for (std::size_t i = 0; i < count; ++i) {
                if (i) p.expect(",");
                const auto expected = i < fixed ? (part == 0 ? spec[i].type : spec[i].unit)
                    : (part == 0 ? "f64?" : "");
                need(p.string() == expected, "wrong descriptor type or unit");
            }
            p.expect("]");
        }
        p.expect("}");
        if (name == "O") h.records.series_slots = static_cast<std::int64_t>(count - fixed);
        if (name == "K") h.records.constant_count = static_cast<std::int64_t>(count);
    } while (p.eat(','));
    p.expect("]}");
    p.end();
    need(h.present[0] && h.present[1], "E and T descriptors required");
    need(h.present[2] == h.present[3] && h.present[3] == h.present[4], "O/V/K must be selected together");
    h.records.outputs = h.present[2];
    h.records.trace = h.present[5];
    h.records.hashes = h.present[6];
    // Also binds the producer's engine string. Rebuilding a historical header
    // with today's version would silently change logical identity.
    need(v1_header_json(h.run_id, h.records) == text, "header is not the current canonical v1 writer header");
    return h;
}

char record(std::string_view line, const Header& h, BlockWriter& writer, std::vector<double>& values) {
    Cursor p(line);
    p.expect("[");
    const auto tag = p.string();
    need(tag.size() == 1 && tags.find(tag[0]) != std::string_view::npos
        && h.present[tags.find(tag[0])], "record tag absent from header");
    // Parse the complete line before submitting it; malformed tails must never
    // be accepted by the writer. All strings are owned only for this call.
    switch (tag[0]) {
    case 'E': {
        pf_equity_point_t e{};
        e.time_ms = p.i(); e.equity = p.f(); e.open_profit = p.f();
        p.expect("]"); p.end(); writer.write_e(e); break;
    }
    case 'T': {
        const auto n = p.i();
        const auto side = p.s();
        need(side == "long" || side == "short", "trade side must be long or short");
        pf_trade_t t{};
        t.is_long = side == "long";
        t.entry_time = p.i(); t.exit_time = p.i();
        t.entry_price = p.f(); t.exit_price = p.f(); t.qty = p.f();
        t.pnl = p.f(); t.pnl_pct = p.f(); t.max_runup = p.f();
        t.max_drawdown = p.f(); t.commission = p.f();
        t.entry_bar_index = p.narrow<std::int32_t>(); t.exit_bar_index = p.narrow<std::int32_t>();
        t.open_at_end = p.boolean() ? 1 : 0;
        const auto incarnation = p.u();
        const auto cause = p.i();
        const auto entry = p.s(), exit = p.s(), comment = p.s();
        p.expect("]"); p.end();
        writer.write_t(n, t, incarnation, cause, entry, exit, comment); break;
    }
    case 'O': {
        const auto row = p.i(), open = p.time(), close = p.time();
        values.clear();
        for (std::int64_t i = 0; i < h.records.series_slots; ++i) values.push_back(p.f());
        p.expect("]"); p.end();
        writer.write_o(row, open, close, values.data(), h.records.series_slots); break;
    }
    case 'V': {
        pf_output_event_v1_t e{};
        e.sequence = p.u(); e.output_index = p.narrow<std::int32_t>(); e.bar_index = p.narrow<std::int32_t>();
        e.bar_open_ms = p.time(); e.bar_close_ms = p.time();
        e.ordinal_in_bar = p.narrow<std::uint32_t>(); e.phase = p.narrow<std::uint32_t>();
        e.confirmed = p.narrow<std::uint32_t>(); e.value = p.f(); e.message_hash64 = p.hex();
        p.expect(",");
        const bool null = p.peek() == 'n';
        std::string message;
        if (null) p.expect("null"); else message = p.string();
        e.message = null ? nullptr : message.c_str();
        p.expect("]"); p.end(); writer.write_v(e, message); break;
    }
    case 'K':
        values.clear();
        for (std::int64_t i = 0; i < h.records.constant_count; ++i) values.push_back(p.f());
        p.expect("]"); p.end();
        writer.write_k(values.data(), h.records.constant_count); break;
    case 'X': {
        pf_trace_entry_t e{};
        e.timestamp = p.i(); e.bar_index = p.narrow<std::int32_t>();
        const auto name = p.s(); e.value = p.f();
        p.expect("]"); p.end(); writer.write_x(e, name); break;
    }
    case 'H': {
        const auto word = p.hex();
        p.expect("]"); p.end(); writer.write_h(word); break;
    }
    default: refuse(3, "unknown record type");
    }
    return tag[0];
}

void trailer(std::string_view line, const Header& h, const std::array<std::uint64_t, 7>& counts,
             const std::string& digest) {
    Cursor p(line);
    p.expect("{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{");
    bool first = true;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (!h.present[i]) continue;
        if (!first) p.expect(",");
        first = false;
        need(p.string() == std::string(1, tags[i]), "wrong trailer count keys");
        p.expect(":");
        const auto count = p.integer();
        if (count < 0 || static_cast<std::uint64_t>(count) != counts[i]) refuse(4, "trailer count mismatch");
    }
    p.expect("},\"sha256\":");
    const auto sha = p.string();
    need(sha.size() == 64 && sha.find_first_not_of("0123456789abcdef") == std::string::npos, "invalid SHA-256 text");
    if (sha != digest) refuse(4, "trailer SHA-256 mismatch");
    p.expect("}"); p.end();
}

// Streaming projection of the same run_json stdout. Only the <=1 MiB
// resulting metadata is retained, even if legacy stdout has large arrays.
// Exact scalar bytes/order survive; BlockWriter owns final schema, Unicode,
// duplicate-key, finite-number, depth and sink_folds validation.
class Projection {
    Input input_;
    std::string out_;
    std::string* capture_ = nullptr;
    static constexpr std::size_t limit = L::max_summary;
    int peek() {
        const int c = input_.peek();
        need(c != ' ' && c != '\n' && c != '\r' && c != '\t', "report JSON must be compact");
        return c;
    }
    char take() {
        const char c = input_.take();
        if (capture_) {
            if (capture_->size() == limit) refuse(4, "summary_limit");
            capture_->push_back(c);
        }
        return c;
    }
    void expect(char c) {
        (void)peek();
        need(take() == c, "malformed report JSON");
    }
    bool eat(char c) {
        if (peek() != c) return false;
        (void)take(); return true;
    }
    void append(std::string_view text) {
        if (text.size() > limit - out_.size()) refuse(4, "summary_limit");
        out_.append(text);
    }
    unsigned hex4() {
        unsigned word = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = take();
            const int digit = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            need(digit >= 0, "invalid report Unicode escape");
            word = word * 16 + static_cast<unsigned>(digit);
        }
        return word;
    }
    void string() {
        expect('"');
        while (true) {
            const auto c = static_cast<unsigned char>(take());
            if (c == '"') return;
            need(c >= 0x20, "control byte in report string");
            if (c == '\\') {
                const char escape = take();
                need(std::string_view("\"\\/bfnrtu").find(escape) != std::string_view::npos, "invalid report escape");
                if (escape == 'u') {
                    const auto cp = hex4();
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        expect('\\'); expect('u');
                        const auto low = hex4();
                        need(low >= 0xdc00 && low <= 0xdfff, "unpaired report surrogate");
                    } else need(cp < 0xdc00 || cp > 0xdfff, "unpaired report surrogate");
                }
            } else if (c >= 0x80) {
                const unsigned width = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3
                    : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
                need(width != 0, "invalid report UTF-8");
                for (unsigned i = 1; i < width; ++i) {
                    const auto next = static_cast<unsigned char>(take());
                    need(next >= 0x80 && next <= 0xbf && !(i == 1
                        && ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0)
                            || (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90))), "invalid report UTF-8");
                }
            }
        }
    }
    struct Key { std::string name, raw; };
    Key key() {
        std::string raw;
        capture_ = &raw;
        string();
        capture_ = nullptr;
        Cursor p(raw);
        auto result = p.string(false);
        p.end();
        return {std::move(result), std::move(raw)};
    }
    void value(unsigned depth) {
        const int c = peek();
        if (c == '[' || c == '{') {
            need(depth < L::max_depth, "report JSON depth exceeds 64");
            (void)take();
            const char close = c == '[' ? ']' : '}';
            if (eat(close)) return;
            do {
                if (c == '{') { string(); expect(':'); }
                value(depth + 1);
            } while (eat(','));
            expect(close);
        } else if (c == '"') string();
        else if (c == 't' || c == 'f' || c == 'n') {
            for (const char expected : c == 't' ? std::string_view("true")
                    : c == 'f' ? std::string_view("false") : std::string_view("null"))
                expect(expected);
        } else {
            // Finite-domain checks on retained tokens remain the writer's.
            const auto digit = [&] { const auto next = peek(); return next >= '0' && next <= '9'; };
            (void)eat('-');
            need(digit(), "invalid report number");
            if (!eat('0')) while (digit()) (void)take();
            if (eat('.')) {
                need(digit(), "invalid report fraction");
                while (digit()) (void)take();
            }
            if (eat('e') || eat('E')) {
                if (!eat('+')) (void)eat('-');
                need(digit(), "invalid report exponent");
                while (digit()) (void)take();
            }
        }
    }
    void outputs() {
        static constexpr std::array<std::string_view, 6> allowed{{
            "schema_version", "message_format", "manifest_sha256", "manifest", "constants", "hlines"}};
        static constexpr std::array<std::string_view, 3> payload{{"bars", "series", "events"}};
        unsigned seen = 0;
        expect('{'); append("{");
        bool first = true;
        if (!eat('}')) {
            do {
                const auto member = key();
                const auto& name = member.name;
                expect(':');
                const auto found = std::find(allowed.begin(), allowed.end(), name);
                const auto skipped = std::find(payload.begin(), payload.end(), name);
                need(found != allowed.end() || skipped != payload.end(), "unknown report outputs member");
                const auto bit = found != allowed.end() ? static_cast<unsigned>(found - allowed.begin())
                    : 6u + static_cast<unsigned>(skipped - payload.begin());
                need(!(seen & (1u << bit)), "duplicate report outputs key");
                seen |= 1u << bit;
                if (found != allowed.end()) {
                    if (!first) append(",");
                    first = false;
                    append(member.raw); append(":");
                    capture_ = &out_;
                }
                value(2); capture_ = nullptr;
            } while (eat(','));
            expect('}');
        }
        need((seen & 63u) == 63u, "missing outputs metadata");
        append("}");
    }
public:
    explicit Projection(int fd) : input_(fd) { out_.reserve(limit); }
    std::string project(const pf_report_sink_receipt_v2& receipt) {
        const auto folds = "{\"closed_count\":" + std::to_string(receipt.closed_count)
            + ",\"open_at_end_count\":" + std::to_string(receipt.open_at_end_count)
            + ",\"open_at_end_pnl\":" + f64(receipt.open_at_end_pnl) + "}";
        unsigned removed = 0;
        expect('{'); append("{");
        bool first = true;
        if (!eat('}')) {
            do {
                const auto member = key();
                const auto& name = member.name;
                expect(':');
                static constexpr std::array<std::string_view, 4> drop{{
                    "trades", "equity_curve", "report_sink", "sink_folds"}};
                const auto found = std::find(drop.begin(), drop.end(), name);
                if (found != drop.end()) {
                    const auto bit = 1u << static_cast<unsigned>(found - drop.begin());
                    need(!(removed & bit), "duplicate report payload key");
                    removed |= bit;
                    std::string supplied;
                    if (name == "sink_folds") capture_ = &supplied;
                    value(1); capture_ = nullptr;
                    if (name == "sink_folds" && supplied != folds) refuse(4, "supplied sink_folds differ from stream");
                } else {
                    if (!first) append(",");
                    first = false;
                    append(member.raw); append(":");
                    if (name == "outputs") outputs();
                    else { capture_ = &out_; value(1); capture_ = nullptr; }
                }
            } while (eat(','));
            expect('}');
        }
        if (input_.peek() == '\n') (void)take();
        need(peek() == -1, "trailing report bytes");
        if (!first) append(",");
        append("\"sink_folds\":"); append(folds); append("}");
        return std::move(out_);
    }
};

class FileSource final : public v2::RandomAccessSource {
    Fd file_;
    std::uint64_t size_;
public:
    explicit FileSource(const std::string& path) : file_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)), size_(0) {
        if (file_.get() < 0) io("open staging for verification");
        struct stat status{};
        if (::fstat(file_.get(), &status) < 0) io("stat staging");
        need(S_ISREG(status.st_mode) && status.st_size >= 0, "invalid staging file");
        size_ = static_cast<std::uint64_t>(status.st_size);
    }
    std::uint64_t size() const override { return size_; }
    std::string generation() const override { return "private-transcoder-staging"; }
    bool concurrent_reads() const noexcept override { return true; }
    v2::Bytes readRange(std::uint64_t offset, std::size_t length) override {
        if (offset > size_ || length > size_ - offset) refuse(4, "verification range outside staging");
        v2::Bytes result(length);
        std::size_t at = 0;
        while (at < length) {
            const auto n = ::pread(file_.get(), result.data() + at, length - at, static_cast<off_t>(offset + at));
            if (n < 0) { if (errno == EINTR) continue; io("read staging"); }
            if (n == 0) refuse(5, "short staging read");
            at += static_cast<std::size_t>(n);
        }
        return result;
    }
};

class Staging {
    int fd_ = -1;
public:
    std::string path;
    explicit Staging(const std::string& directory, int borrowed = -1) {
        if (borrowed >= 0) {
            do { fd_ = ::fcntl(borrowed, F_DUPFD_CLOEXEC, 0); } while (fd_ < 0 && errno == EINTR);
            if (fd_ < 0) io("duplicate staging descriptor");
            return;
        }
        const auto pattern = directory + "/.pf-transcode-v2-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        fd_ = ::mkstemp(name.data());
        if (fd_ < 0) io("create staging");
        path = name.data();
        if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) {
            const int saved = errno;
            (void)::close(fd_); fd_ = -1;
            (void)::unlink(path.c_str());
            errno = saved; io("set staging close-on-exec");
        }
    }
    int fd() const noexcept { return fd_; }
    void close() {
        const int descriptor = fd_;
        fd_ = -1;
        if (::close(descriptor) < 0) io("close staging inode");
    }
    ~Staging() {
        if (fd_ >= 0 && ::close(fd_) < 0)
            std::fprintf(stderr, "pf_sink_transcode_v2: staging close failed: %s\n", std::strerror(errno));
        if (!path.empty() && ::unlink(path.c_str()) < 0)
            std::fprintf(stderr, "pf_sink_transcode_v2: staging cleanup failed: %s: %s\n",
                path.c_str(), std::strerror(errno));
    }
};

std::string parent(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}
void absent(const std::string& path) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) == 0) refuse(5, "--out already exists");
    if (errno != ENOENT) io("stat output");
}
bool same_input_file(const struct stat& before, const struct stat& after) {
#if defined(__APPLE__)
    const auto& old_mtime = before.st_mtimespec;
    const auto& new_mtime = after.st_mtimespec;
#else
    const auto& old_mtime = before.st_mtim;
    const auto& new_mtime = after.st_mtim;
#endif
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino
        && before.st_size == after.st_size && old_mtime.tv_sec == new_mtime.tv_sec
        && old_mtime.tv_nsec == new_mtime.tv_nsec;
}
TranscodeResult run(const TranscodeOptions& options, std::uint64_t& observed,
                    std::unique_ptr<ColumnCodec> codec) {
    if (options.in_fd < 0 || options.summary_fd < 0 || options.in_fd == options.summary_fd
        || (!options.publish_none && options.out.empty()) || options.out.find('\0') != std::string::npos
        || (options.publish_none != (options.staging_fd >= 0))
        || (options.staging_fd >= 0 && (options.staging_fd == options.in_fd || options.staging_fd == options.summary_fd))
        || (options.late_verify != TranscodeOptions::LateVerify::full
            && options.late_verify != TranscodeOptions::LateVerify::structure)
        || options.spool_dir.find('\0') != std::string::npos || options.level < 1 || options.level > 19
        || options.policy.late_threads < 1 || options.policy.late_threads > 4)
        refuse(2, "invalid transcoder options");
    if (!sink_v2_available() || !v2::Reader::has_zstd()) refuse(2, "format 2 requires zstd encoder and decoder");
    if (std::fegetround() != FE_TONEAREST) refuse(7, "transcoder requires round-to-nearest binary64");
    const auto directory = options.spool_dir.empty() ? parent(options.out) : options.spool_dir;
    if (!options.publish_none) {
        absent(options.out);
        Fd directory_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (directory_fd.get() < 0) io("open private spool directory");
        struct stat directory_status{};
        if (::fstat(directory_fd.get(), &directory_status) < 0) io("stat spool directory");
        if (directory_status.st_uid != ::geteuid() || (directory_status.st_mode & 0022))
            refuse(5, "spool directory must be owned by this uid and not group/world writable");
    }
    std::optional<struct stat> input_status;
    if (options.stage_a_binding) {
        struct stat status{};
        if (::fstat(options.in_fd, &status) < 0) io("stat bound input");
        if (S_ISREG(status.st_mode)) input_status = status;
    }
    Input input(options.in_fd, options.max_bytes, &observed, options.phase_log);
    std::string line;
    line.reserve(L::max_line);
    if (!input.line(line)) refuse(6, "stream ended without a header or trailer");
    const auto h = header(std::string_view(line).substr(0, line.size() - 1));
    if (options.run_id && *options.run_id != h.run_id) refuse(3, "run-id mismatch");
    Sha256 prefix, full;
    prefix.update(line); full.update(line);
    // One staging/output object is the entire spool population. Reject before
    // creating it; future work-file additions must update this reservation.
    if (options.spool_max_files && *options.spool_max_files < 1)
        refuse(10, "output requires one spool file");
    Staging staging(directory, options.staging_fd);
    TranscodeResult result;
    result.late_verify = options.late_verify;
    v2::IncrementalProof proof;
    {
        pf_report_sink_v2 descriptor{sizeof(pf_report_sink_v2), 2,
            options.publish_none ? "owner-staging-fd" : staging.path.c_str(), h.run_id.c_str(),
            directory.c_str(), options.level, 1, 1, 0};
        BlockPolicy policy = options.policy;
        policy.zstd_level = options.level;
        policy.threaded = false; // trusted conversion needs no extra producer queue
        policy.queue_depth = 1;
        policy.max_output_bytes = options.spool_max_bytes;
        policy.staging_fd = staging.fd();
        policy.phase_log = options.phase_log;
        auto incremental = std::make_unique<v2::IncrementalValidator>(policy, line);
        policy.observer = incremental.get();
        BlockWriter writer(descriptor, policy, std::move(codec));
        writer.write_header(h.records);
        std::array<std::uint64_t, 7> counts{};
        std::vector<double> values;
        // O/K width is bounded by the header/line cap, never by the run length.
        values.reserve(static_cast<std::size_t>(std::max(h.records.series_slots, h.records.constant_count)));
        bool sealed = false;
        while (input.line(line)) {
            const auto body = std::string_view(line).substr(0, line.size() - 1);
            if (!body.empty() && body.front() == '{') {
                trailer(body, h, counts, prefix.hex_digest());
                full.update(line);
                need(input.peek() == -1, "bytes after trailer");
                sealed = true;
                break;
            }
            incremental->input_line(line);
            const auto tag = record(body, h, writer, values);
            auto& count = counts[tags.find(tag)];
            need(count < L::safe, "record count exceeds safe integer bound");
            ++count;
            prefix.update(line); full.update(line);
        }
        if (!sealed) refuse(6, "stream ended without a trailer");
        if (options.phase_log) options.phase_log->mark("input_eof", true);
        if (options.stage_a_binding) {
            // These observations come from the consumed stream, independently
            // of both its verified trailer and stage A's expected receipt.
            const auto& expected = *options.stage_a_binding;
            if (expected.sha256 != prefix.hex_digest())
                refuse(4, "stage-A digest disagrees with consumed input");
            if (expected.counts != counts)
                refuse(4, "stage-A counts disagree with consumed input");
            if (expected.sink_bytes != input.bytes)
                refuse(4, "stage-A byte length disagrees with consumed input");
            if (input_status) {
                struct stat status{};
                if (::fstat(options.in_fd, &status) < 0) io("restat bound input");
                if (!S_ISREG(status.st_mode) || status.st_size < 0
                    || !same_input_file(*input_status, status)
                    || static_cast<std::uint64_t>(status.st_size) != input.bytes)
                    refuse(4, "stage-A input changed during consumption");
            }
        }
        if (options.phase_log) options.phase_log->mark("stage_a_binding_done");
        writer.write_trailer();
        proof = incremental->finish(staging.fd(), counts);
        incremental.reset(); // no validation buffers overlap SUMMARY or F
        if (options.phase_log) options.phase_log->mark("incremental_validation_done");
        // Release the v1 line/variable-slot buffers before SUMMARY/index work.
        std::string().swap(line);
        std::vector<double>().swap(values);
        const auto summary = Projection(options.summary_fd).project(writer.receipt());
        if (options.phase_log) options.phase_log->mark("summary_parsed");
        writer.finalize(summary); // existing 9.1 admission, folds and summary_limit
        result.receipt = writer.receipt();
        result.memory = writer.memory();
        result.input_bytes = input.bytes;
        result.input_sha256 = prefix.hex_digest();
        if (result.receipt.complete != 1) refuse(4, "writer did not seal v2");
    }
    // Full verification and byte-exact inverse without materializing either
    // file in RAM. The v1 source descriptor is never read a second time.
    {
        std::shared_ptr<v2::RandomAccessSource> source = options.publish_none
            ? std::shared_ptr<v2::RandomAccessSource>(std::make_shared<v2::FdSource>(staging.fd()))
            : std::shared_ptr<v2::RandomAccessSource>(std::make_shared<FileSource>(staging.path));
        auto reader = v2::Reader::open(std::move(source));
        Sha256 inverse;
        std::uint64_t bytes = 0;
        v2::Conversion converted;
        if (options.late_verify == TranscodeOptions::LateVerify::full) {
            converted = reader.to_v1([&](std::string_view chunk) {
                inverse.update(chunk); bytes += chunk.size(); return true;
            }, options.phase_log);
        } else {
            const auto verified = reader.verify_structure(proof, options.policy.late_threads);
            if (verified.scope != v2::VerificationScope::structure)
                refuse(4, "structure verification did not complete");
            if (options.phase_log) options.phase_log->mark("structure_verify_done");
        }
        const auto inverse_digest = inverse.hex_digest();
        if (options.phase_log && options.late_verify == TranscodeOptions::LateVerify::full)
            options.phase_log->mark("to_v1_done");
        std::string logical;
        constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : reader.logical_sha256().value) {
            logical += hex[byte >> 4]; logical += hex[byte & 15];
        }
        if (logical != result.receipt.sha256_hex
            || (options.late_verify == TranscodeOptions::LateVerify::full
                && (inverse_digest != full.hex_digest() || bytes != result.input_bytes
                    || converted.scope != v2::VerificationScope::whole_file)))
            refuse(4, "verified v2 inverse or logical identity differs from input");
    }
    if (::fsync(staging.fd()) < 0) io("fsync staging");
    staging.close();
    if (options.phase_log) options.phase_log->mark("fsync_close_done");
    if (options.publish_none) return result;
    // link() publishes the same inode without an overwrite race. The only
    // output object has two names briefly; no second spool object is created.
    if (::link(staging.path.c_str(), options.out.c_str()) < 0)
        io("publish staging (spool and out must share a filesystem)");
    if (::unlink(staging.path.c_str()) < 0) {
        const int saved = errno;
        (void)::unlink(options.out.c_str());
        errno = saved; io("remove staging link");
    }
    staging.path.clear();
    return result;
}
} // namespace

TranscodeResult transcode_v2(const TranscodeOptions& options) {
    return transcode_v2(options, {});
}
TranscodeResult transcode_v2(const TranscodeOptions& options, std::unique_ptr<ColumnCodec> codec) {
    std::uint64_t observed = 0;
    try { return run(options, observed, std::move(codec)); }
    catch (const TranscodeError& error) { throw TranscodeError(error.status(), error.what(), observed); }
    catch (const BlockError& error) {
        throw TranscodeError(error.reason() == BlockFailure::spool_bytes ? 9
            : error.reason() == BlockFailure::io ? 5 : error.reason() == BlockFailure::summary_limit ? 4 : 3,
            error.what(), observed);
    }
    catch (const v2::ReaderError& error) {
        throw TranscodeError(error.failure() == v2::Failure::io ? 5 : 4, error.what(), observed);
    }
    catch (const std::exception& error) { throw TranscodeError(7, error.what(), observed); }
}

const char* transcode_failure_reason(int status) noexcept {
    switch (status) {
    case 4: return "v2_input_mismatch";
    case 2: case 3: case 6: return "v2_unrepresentable";
    case 9: case 10: return "v2_spool_exhausted";
    default: return "v2_resource"; // I/O, internal/resource and input-byte cap
    }
}

}} // namespace pineforge::report_sink
