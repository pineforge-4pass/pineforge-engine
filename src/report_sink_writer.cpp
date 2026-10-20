#include "report_sink_writer.hpp"
#include "sha256.hpp"

#include <pineforge/version.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace pineforge {
namespace report_sink {
namespace {

constexpr std::int64_t safe_integer = INT64_C(9007199254740991);
constexpr std::size_t max_line = 1024 * 1024;
constexpr std::array<char, 7> tags{{'E', 'T', 'O', 'V', 'K', 'X', 'H'}};

[[noreturn]] void invalid(const char* message) {
    throw WriterError(WriterFailure::invalid_input, message);
}

void check_line(const std::string& line) {
    if (line.size() >= max_line) invalid("report sink line exceeds 1 MiB including LF");
}

std::string integer(std::int64_t value) {
    if (value < -safe_integer || value > safe_integer)
        invalid("report sink i64 exceeds the exact JSON integer bound");
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) invalid("report sink integer conversion failed");
    return {buffer, result.ptr};
}

std::string unsigned_integer(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(safe_integer))
        invalid("report sink u64 exceeds the exact JSON integer bound");
    return integer(static_cast<std::int64_t>(value));
}

std::string wire_string(std::string_view value) {
    if (value.size() >= max_line) invalid("report sink string exceeds the line bound");
    auto result = escaped_string(value);
    check_line(result);
    return result;
}

struct Column { const char* field; const char* type; const char* unit; };

constexpr Column equity_columns[] = {
    {"time_ms", "i64", "unix_ms"}, {"equity", "f64?", "account_ccy"},
    {"open_profit", "f64?", "account_ccy"},
};
constexpr Column trade_columns[] = {
    {"n", "i64", ""}, {"side", "str", ""},
    {"entry_time", "i64", "unix_ms"}, {"exit_time", "i64", "unix_ms"},
    {"entry_price", "f64?", "quote_ccy"}, {"exit_price", "f64?", "quote_ccy"},
    {"qty", "f64?", "qty"}, {"pnl", "f64?", "account_ccy"}, {"pnl_pct", "f64?", "percent"},
    {"max_runup", "f64?", "account_ccy"}, {"max_drawdown", "f64?", "account_ccy"},
    {"commission", "f64?", "account_ccy"}, {"entry_bar_index", "i64", "script_bar"},
    {"exit_bar_index", "i64", "script_bar"}, {"open_at_end", "bool", ""},
    {"entry_incarnation", "u64", ""}, {"close_cause", "i64", ""},
    {"entry_id", "str", ""}, {"exit_id", "str", ""}, {"exit_comment", "str", ""},
};
constexpr Column event_columns[] = {
    {"sequence", "u64", ""}, {"output_index", "i64", ""}, {"bar_index", "i64", ""},
    {"bar_open_ms", "i64?", "unix_ms"}, {"bar_close_ms", "i64?", "unix_ms"},
    {"ordinal_in_bar", "i64", ""}, {"phase", "i64", ""}, {"confirmed", "i64", ""},
    {"value", "f64?", ""}, {"message_hash64", "hex64", ""}, {"message", "str?", ""},
};
constexpr Column trace_columns[] = {
    {"timestamp", "i64", "unix_ms"}, {"bar_index", "i64", ""},
    {"name", "str", ""}, {"value", "f64?", ""},
};

bool enabled(char tag, const RecordSet& records) {
    if (tag == 'O' || tag == 'V' || tag == 'K') return records.outputs;
    if (tag == 'X') return records.trace;
    if (tag == 'H') return records.hashes;
    return true;
}

template <class Emit>
void columns(char tag, const RecordSet& records, Emit emit) {
    switch (tag) {
    case 'E': for (const auto& column : equity_columns) emit(column); break;
    case 'T': for (const auto& column : trade_columns) emit(column); break;
    case 'V': for (const auto& column : event_columns) emit(column); break;
    case 'X': for (const auto& column : trace_columns) emit(column); break;
    case 'H': emit(Column{"hash", "hex64", ""}); break;
    case 'O':
        emit(Column{"row", "i64", ""});
        emit(Column{"open_ms", "i64?", "unix_ms"});
        emit(Column{"close_ms", "i64?", "unix_ms"});
        for (std::int64_t i = 0; i < records.series_slots; ++i) {
            const auto name = "v" + integer(i);
            emit(Column{name.c_str(), "f64?", ""});
        }
        break;
    case 'K':
        for (std::int64_t i = 0; i < records.constant_count; ++i) {
            const auto name = "c" + integer(i);
            emit(Column{name.c_str(), "f64?", ""});
        }
        break;
    }
}

void append_descriptor(std::string& line, char tag, const RecordSet& records) {
    line += "{\"type\":\"";
    line += tag;
    line += '"';
    const char* keys[] = {",\"fields\":[", ",\"types\":[", ",\"units\":["};
    for (unsigned list = 0; list < 3; ++list) {
        line += keys[list];
        bool first = true;
        columns(tag, records, [&](const Column& column) {
            if (!first) line += ',';
            first = false;
            line += escaped_string(list == 0 ? column.field : list == 1 ? column.type : column.unit);
            check_line(line);
        });
        line += ']';
    }
    line += '}';
    check_line(line);
}

std::string header(const std::string& run_id, const RecordSet& records) {
    if (records.series_slots < 0 || records.constant_count < 0
            || records.series_slots > static_cast<std::int64_t>(max_line)
            || records.constant_count > static_cast<std::int64_t>(max_line))
        invalid("report sink descriptor count exceeds the line bound");
    std::string line = "{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":";
    line += wire_string(run_id);
    line += ",\"engine\":";
    line += escaped_string(PINEFORGE_VERSION_STRING);
    line += ",\"records\":[";
    bool first = true;
    for (const char tag : tags) {
        if (!enabled(tag, records)) continue;
        if (!first) line += ',';
        first = false;
        append_descriptor(line, tag, records);
    }
    line += "]}";
    check_line(line);
    return line;
}

} // namespace

std::string f64(double value) {
    if (!std::isfinite(value)) return "null";
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                         std::chars_format::scientific);
    if (converted.ec != std::errc{})
        invalid("report sink shortest scientific conversion failed");
    const std::string_view text(buffer, static_cast<std::size_t>(converted.ptr - buffer));
    const auto exponent_at = text.find('e');
    if (exponent_at == std::string_view::npos) invalid("report sink scientific exponent is absent");
    const bool negative = text.front() == '-';
    std::string digits;
    for (std::size_t i = negative ? 1 : 0; i < exponent_at; ++i)
        if (text[i] != '.') digits += text[i];
    int exponent = 0;
    for (std::size_t i = exponent_at + 2; i < text.size(); ++i)
        exponent = 10 * exponent + (text[i] - '0');
    if (text[exponent_at + 1] == '-') exponent = -exponent;
    std::string result = negative ? "-" : "";
    if (exponent >= -4 && exponent < 16) {
        const int point = exponent + 1;
        if (point <= 0) {
            result += "0.";
            result.append(static_cast<std::size_t>(-point), '0');
            result += digits;
        } else if (static_cast<std::size_t>(point) >= digits.size()) {
            result += digits;
            result.append(static_cast<std::size_t>(point) - digits.size(), '0');
            result += ".0";
        } else {
            result.append(digits, 0, static_cast<std::size_t>(point));
            result += '.';
            result.append(digits, static_cast<std::size_t>(point), std::string::npos);
        }
    } else {
        result += digits.front();
        if (digits.size() > 1) {
            result += '.';
            result.append(digits, 1, std::string::npos);
        }
        result += exponent < 0 ? "e-" : "e+";
        const int magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude < 10) result += '0';
        result += integer(magnitude);
    }
    return result;
}

std::string escaped_string(std::string_view utf8) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (std::size_t i = 0; i < utf8.size();) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        if (byte < 0x20) {
            result += "\\u00";
            result += hex[byte >> 4];
            result += hex[byte & 15];
            ++i;
        } else if (byte < 0x80) {
            if (byte == '"' || byte == '\\') result += '\\';
            result += static_cast<char>(byte);
            ++i;
        } else {
            std::size_t length = 0;
            if (byte >= 0xc2 && byte <= 0xdf) length = 2;
            else if (byte >= 0xe0 && byte <= 0xef) length = 3;
            else if (byte >= 0xf0 && byte <= 0xf4) length = 4;
            bool valid = length != 0 && length <= utf8.size() - i;
            for (std::size_t j = 1; valid && j < length; ++j) {
                const auto next = static_cast<unsigned char>(utf8[i + j]);
                valid = next >= 0x80 && next <= 0xbf;
            }
            if (valid) {
                const auto second = static_cast<unsigned char>(utf8[i + 1]);
                if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0)
                        || (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90))
                    valid = false;
            }
            if (valid) {
                result.append(utf8.data() + i, length);
                i += length;
            } else {
                result += "\xef\xbf\xbd";
                ++i;
            }
        }
    }
    result += '"';
    return result;
}

struct LineWriter::State {
    enum class Phase { configured, writing, complete, failed };
    std::string path;
    std::string run_id;
    std::string scratch_dir;
    int descriptor = -1;
    Phase phase = Phase::configured;
    RecordSet records{};
    Trailer trailer{};
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    std::size_t used = 0;
    bool has_equity_time = false;
    std::int64_t equity_time = 0;

    explicit State(const pf_report_sink_v1& sink) {
        if (sink.struct_size != sizeof(pf_report_sink_v1) || sink.version != 1
                || !sink.path || !*sink.path)
            invalid("invalid report sink descriptor");
        path = sink.path;
        run_id = sink.run_id ? sink.run_id : "";
        if (sink.scratch_dir) scratch_dir = sink.scratch_dir;
        else {
            const auto slash = path.find_last_of('/');
            scratch_dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
        }
    }

    ~State() {
        // Resource release only: abandoned buffered bytes never cause a
        // successful trailer or an unchecked destructor-time write.
        if (descriptor >= 0) ::close(descriptor);
    }

    void require_writing() const {
        if (phase != Phase::writing)
            throw WriterError(WriterFailure::invalid_state, "report sink is not accepting records");
    }

    void fail() noexcept {
        if (phase != Phase::complete) {
            phase = Phase::failed;
            used = 0;
        }
    }

    [[noreturn]] void io_error(const char* operation, int number) {
        fail();
        throw WriterError(WriterFailure::io, std::string("report sink ") + operation + ": "
                          + std::error_code(number, std::generic_category()).message(), number);
    }

    void flush() {
        std::size_t written = 0;
        while (written < used) {
            const auto count = ::write(descriptor, buffer.data() + written, used - written);
            if (count < 0) {
                if (errno == EINTR) continue;
                io_error("write failed", errno);
            }
            if (count == 0) io_error("write made no progress", EIO);
            written += static_cast<std::size_t>(count);
        }
        used = 0;
    }

    void append(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto take = std::min(buffer.size() - used, bytes.size());
            std::memcpy(buffer.data() + used, bytes.data(), take);
            used += take;
            bytes.remove_prefix(take);
            if (used == buffer.size()) flush();
        }
    }

    void line(const std::string& value, bool hashed = true) {
        check_line(value);
        if (hashed) {
            hash.update(value);
            hash.update("\n");
        }
        append(value);
        append("\n");
    }

    std::array<std::int64_t, 7> counts() const {
        return {{trailer.count_e, trailer.count_t, trailer.count_o, trailer.count_v,
                 trailer.count_k, trailer.count_x, trailer.count_h}};
    }

    template<class Build>
    void record(char tag, std::int64_t& count, Build build) {
        try {
            require_writing();
            if (!enabled(tag, records)) invalid("report sink record is absent from the header");
            if (count == safe_integer) invalid("report sink record count exceeds its bound");
            line(build());
            ++count;
        } catch (...) {
            fail();
            throw;
        }
    }
};

LineWriter::LineWriter(const pf_report_sink_v1& sink) : state_(std::make_unique<State>(sink)) {}
LineWriter::~LineWriter() = default;

void LineWriter::write_header(const RecordSet& records) {
    auto& state = *state_;
    try {
        if (state.phase != State::Phase::configured)
            throw WriterError(WriterFailure::invalid_state, "report sink header was already attempted");
        const auto line = header(state.run_id, records);
        int descriptor;
        do { descriptor = ::open(state.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); }
        while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) state.io_error("open failed", errno);
        state.descriptor = descriptor;
        state.records = records;
        state.phase = State::Phase::writing;
        state.line(line);
    } catch (...) {
        state.fail();
        throw;
    }
}

void LineWriter::write_e(const pf_equity_point_t& point) {
    auto& state = *state_;
    try {
        state.require_writing();
        if (state.trailer.count_e == safe_integer) invalid("report sink E count exceeds its bound");
        if (state.has_equity_time && point.time_ms < state.equity_time)
            invalid("report sink equity times are not ordered");
        const auto line = "[\"E\"," + integer(point.time_ms) + ',' + f64(point.equity)
            + ',' + f64(point.open_profit) + ']';
        state.line(line);
        ++state.trailer.count_e;
        state.has_equity_time = true;
        state.equity_time = point.time_ms;
    } catch (...) {
        state.fail();
        throw;
    }
}

void LineWriter::write_t(std::int64_t n, const pf_trade_t& trade,
                         std::uint64_t entry_incarnation, std::int64_t close_cause,
                         std::string_view entry_id, std::string_view exit_id,
                         std::string_view exit_comment) {
    auto& state = *state_;
    try {
        state.require_writing();
        if (state.trailer.count_t == safe_integer || n != state.trailer.count_t + 1)
            invalid("report sink trade row numbers must start at one and be contiguous");
        std::string line = "[\"T\"," + integer(n) + (trade.is_long ? ",\"long\"," : ",\"short\",");
        line += integer(trade.entry_time) + ',' + integer(trade.exit_time);
        const double values[] = {trade.entry_price, trade.exit_price, trade.qty, trade.pnl,
                                 trade.pnl_pct, trade.max_runup, trade.max_drawdown, trade.commission};
        for (const double value : values) line += ',' + f64(value);
        line += ',' + integer(trade.entry_bar_index) + ',' + integer(trade.exit_bar_index);
        line += trade.open_at_end ? ",true," : ",false,";
        line += unsigned_integer(entry_incarnation) + ',' + integer(close_cause);
        for (const auto value : {entry_id, exit_id, exit_comment}) {
            line += ',' + wire_string(value);
            check_line(line);
        }
        line += ']';
        state.line(line);
        ++state.trailer.count_t;
    } catch (...) {
        state.fail();
        throw;
    }
}

Trailer LineWriter::write_trailer() {
    auto& state = *state_;
    if (state.phase == State::Phase::complete) return state.trailer;
    try {
        state.require_writing();
        state.flush();
        const auto digest = state.hash.hex_digest();
        std::string line = "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{";
        const auto counts = state.counts();
        bool first = true;
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (!enabled(tags[i], state.records)) continue;
            if (!first) line += ',';
            first = false;
            line += '"';
            line += tags[i];
            line += "\":";
            line += integer(counts[i]);
        }
        line += "},\"sha256\":\"" + digest + "\"}";
        state.line(line, false);
        state.flush();
        std::memcpy(state.trailer.sha256_hex, digest.c_str(), digest.size() + 1);
        state.phase = State::Phase::complete;
        return state.trailer;
    } catch (...) {
        state.fail();
        throw;
    }
}

namespace {
std::string nullable_time(std::int64_t time) {
    return time == INT64_MIN ? "null" : integer(time);
}

std::string hash64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"0000000000000000\"";
    for (int i = 16; i > 0; --i) {
        result[static_cast<std::size_t>(i)] = digits[value & 15];
        value >>= 4;
    }
    return result;
}

void append_values(std::string& line, const double* values, std::int64_t count,
                   std::int64_t expected) {
    if (count != expected || count < 0 || (count > 0 && !values))
        invalid("report sink values differ from the header field count");
    for (std::int64_t i = 0; i < count; ++i) {
        line += ',' + f64(values[i]);
        check_line(line);
    }
    line += ']';
}
} // namespace

void LineWriter::write_o(std::int64_t row, std::int64_t open_ms, std::int64_t close_ms,
                         const double* values, std::int64_t count) {
    auto& state = *state_;
    state.record('O', state.trailer.count_o, [&] {
        if (row != state.trailer.count_o)
            invalid("report sink output rows must start at zero and be contiguous");
        std::string line = "[\"O\"," + integer(row) + ',' + nullable_time(open_ms)
            + ',' + nullable_time(close_ms);
        append_values(line, values, count, state.records.series_slots);
        return line;
    });
}

void LineWriter::write_v(const pf_output_event_v1_t& event, std::string_view message) {
    auto& state = *state_;
    state.record('V', state.trailer.count_v, [&] {
        std::string line = "[\"V\"," + unsigned_integer(event.sequence)
            + ',' + integer(event.output_index) + ',' + integer(event.bar_index)
            + ',' + nullable_time(event.bar_open_ms) + ',' + nullable_time(event.bar_close_ms)
            + ',' + integer(event.ordinal_in_bar) + ',' + integer(event.phase)
            + ',' + integer(event.confirmed) + ',' + f64(event.value)
            + ',' + hash64(event.message_hash64) + ',';
        line += event.message ? wire_string(message) : "null";
        return line + ']';
    });
}

void LineWriter::write_k(const double* values, std::int64_t count) {
    auto& state = *state_;
    state.record('K', state.trailer.count_k, [&] {
        if (state.trailer.count_k != 0) invalid("report sink constants were already written");
        std::string line = "[\"K\"";
        append_values(line, values, count, state.records.constant_count);
        return line;
    });
}

void LineWriter::write_x(const pf_trace_entry_t& entry, std::string_view name) {
    auto& state = *state_;
    state.record('X', state.trailer.count_x, [&] {
        return "[\"X\"," + integer(entry.timestamp) + ',' + integer(entry.bar_index)
            + ',' + wire_string(name) + ',' + f64(entry.value) + ']';
    });
}

void LineWriter::write_h(std::uint64_t hash) {
    auto& state = *state_;
    state.record('H', state.trailer.count_h, [&] { return "[\"H\"," + hash64(hash) + ']'; });
}

} // namespace report_sink
} // namespace pineforge
