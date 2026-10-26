#include "report_sink_v2_support.hpp"
#include <pineforge/version.h>
#include <array>
#include <charconv>
#include <system_error>

namespace pineforge { namespace report_sink { namespace {
constexpr std::int64_t safe_integer = INT64_C(9007199254740991);
constexpr std::size_t max_line = 1024 * 1024;
constexpr std::array<char, 7> tags{{'E', 'T', 'O', 'V', 'K', 'X', 'H'}};

[[noreturn]] void invalid(const char* message) {
    throw BlockError(BlockFailure::invalid_input, message);
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

std::string v1_header_json(const std::string& run_id, const RecordSet& records) {
    return header(run_id, records);
}

}} // namespace pineforge::report_sink
