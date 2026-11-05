#include "selected_capacity_csv_reader.hpp"

#include <pineforge/run_failure.hpp>

#include <charconv>
#include <cmath>
#include <ios>
#include <limits>
#include <new>
#include <system_error>

namespace pineforge::detail::selected_capacity {
namespace {

constexpr const char* kColumns[] = {"timestamp", "open", "high", "low", "close", "volume"};

void format_error(CsvRead& result, CsvReason reason, const char* message) noexcept {
    result.status = CsvStatus::FormatError;
    result.reason = reason;
    result.message = message;
}

bool digit(char value) noexcept { return value >= '0' && value <= '9'; }

bool numeric_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r'
        || value == '\v' || value == '\f';
}

// Compact in place, without a second field buffer or a lexical size limit.
// Python permits underscores only between digits, including in exponents.
bool number_text(std::string& text, CsvRead& result) {
    for (unsigned char value : text) {
        if (value >= 128) {
            format_error(result, CsvReason::Encoding,
                         "CSV non-ASCII numeric spelling is outside the chart reader scope");
            return false;
        }
    }
    std::size_t begin = 0, end = text.size();
    while (begin < end && numeric_space(text[begin])) ++begin;
    while (end > begin && numeric_space(text[end - 1])) --end;
    for (std::size_t i = begin; i < end; ++i) {
        if (text[i] == '_' && (i == begin || i + 1 == end
            || !digit(text[i - 1]) || !digit(text[i + 1]))) {
            format_error(result, CsvReason::Value, "CSV numeric underscore must separate digits");
            return false;
        }
    }
    std::size_t out = 0;
    for (std::size_t i = begin; i < end; ++i) {
        if (text[i] != '_') text[out++] = text[i];
    }
    text.resize(out);
    return true;
}

bool timestamp_value(const char* text, std::size_t size, std::int64_t& value) {
    const bool negative = size != 0 && text[0] == '-';
    std::size_t at = size != 0 && (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (at == size) return false;
    const std::uint64_t limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        + (negative ? 1 : 0);
    std::uint64_t magnitude = 0;
    for (std::size_t i = at; i < size; ++i) {
        if (!digit(text[i])) return false;
        const unsigned next = static_cast<unsigned>(text[i] - '0');
        if (magnitude > (limit - next) / 10) return false;
        magnitude = magnitude * 10 + next;
    }
    if (negative && magnitude == limit) {
        value = std::numeric_limits<std::int64_t>::min();
    } else {
        value = static_cast<std::int64_t>(magnitude);
        if (negative) value = -value;
    }
    return true;
}

bool real_value(const char* text, std::size_t size, double& value) {
    std::size_t at = 0;
    if (at < size && (text[at] == '-' || text[at] == '+')) ++at;
    std::size_t digits = 0;
    while (at < size && digit(text[at])) {
        ++digits;
        ++at;
    }
    if (at < size && text[at] == '.') {
        ++at;
        while (at < size && digit(text[at])) {
            ++digits;
            ++at;
        }
    }
    if (digits == 0) return false;
    if (at < size && (text[at] == 'e' || text[at] == 'E')) {
        ++at;
        if (at < size && (text[at] == '-' || text[at] == '+')) ++at;
        const std::size_t exponent_start = at;
        while (at < size && digit(text[at])) ++at;
        if (at == exponent_start) return false;
    }
    if (at != size) return false;
    // from_chars accepts a minus but not a leading plus.
    const char* begin = text + (text[0] == '+' ? 1 : 0);
    const auto parsed = std::from_chars(begin, text + size, value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == text + size && std::isfinite(value);
}

bool nan_volume_value(const char* text, std::size_t size, double& value) noexcept {
    const std::size_t at = size != 0 && (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (size - at != 3 || (text[at] != 'n' && text[at] != 'N')
        || (text[at + 1] != 'a' && text[at + 1] != 'A')
        || (text[at + 2] != 'n' && text[at + 2] != 'N')) {
        return false;
    }
    // Match float()'s signed, case-insensitive NaN token for volume only.
    value = std::copysign(std::numeric_limits<double>::quiet_NaN(),
                          text[0] == '-' ? -1.0 : 1.0);
    return true;
}

bool assign_field(Bar& bar, std::size_t column, const char* text, std::size_t size) {
    if (column == 0) return timestamp_value(text, size, bar.timestamp);
    double value = 0;
    if (column == 5 && nan_volume_value(text, size, value)) {
        bar.volume = value;
        return true;
    }
    if (!real_value(text, size, value)) return false;
    switch (column) {
    case 1: bar.open = value; break;
    case 2: bar.high = value; break;
    case 3: bar.low = value; break;
    case 4: bar.close = value; break;
    case 5: bar.volume = value; break;
    default: return false;
    }
    return true;
}

}  // namespace

int CsvReader::read_byte(CsvRead& result, bool peek) {
    std::istream::int_type value;
    try {
        value = peek ? input_.peek() : input_.get();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        // A stream with EOF/fail exceptions enabled still has an ordinary EOF.
        // This catch covers only the borrowed stream read, never a callback.
        if (input_.eof() && !input_.bad()) {
            if (utf8_remaining_ != 0) {
                format_error(result, CsvReason::Encoding, "CSV ends inside a UTF-8 sequence");
                return kInvalid;
            }
            return kEnd;
        }
        result.status = CsvStatus::IoError;
        result.message = "CSV input read failed";
        return kIo;
    }
    if (std::istream::traits_type::eq_int_type(value, std::istream::traits_type::eof())) {
        if (input_.eof() && !input_.bad()) {
            if (utf8_remaining_ != 0) {
                format_error(result, CsvReason::Encoding, "CSV ends inside a UTF-8 sequence");
                return kInvalid;
            }
            return kEnd;
        }
        result.status = CsvStatus::IoError;
        result.message = "CSV input read failed";
        return kIo;
    }
    const unsigned byte =
        static_cast<unsigned char>(std::istream::traits_type::to_char_type(value));
    // Peeking after CR distinguishes CR from CRLF without charging the next
    // record's first byte. Only get() consumes, validates and charges a byte.
    if (peek) return static_cast<int>(byte);
    // Fields have no lexical cap. Keep physical byte-counter exhaustion on
    // the same invariant path as the driver's aggregate counters.
    if (result.csv_bytes == std::numeric_limits<std::uint64_t>::max()) {
        throw coded<std::runtime_error>(
            RunFailureCode::engine_invariant, {}, "Pine native run counter exhausted");
    }
    ++result.csv_bytes;
    if (utf8_remaining_ != 0) {
        if (byte < utf8_min_ || byte > utf8_max_) {
            format_error(result, CsvReason::Encoding, "CSV contains invalid UTF-8");
            return kInvalid;
        }
        --utf8_remaining_;
        utf8_min_ = 0x80;
        utf8_max_ = 0xbf;
    } else if (byte >= 128) {
        utf8_min_ = 0x80;
        utf8_max_ = 0xbf;
        if (byte >= 0xc2 && byte <= 0xdf) {
            utf8_remaining_ = 1;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            utf8_remaining_ = 2;
            if (byte == 0xe0) utf8_min_ = 0xa0;
            if (byte == 0xed) utf8_max_ = 0x9f;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            utf8_remaining_ = 3;
            if (byte == 0xf0) utf8_min_ = 0x90;
            if (byte == 0xf4) utf8_max_ = 0x8f;
        } else {
            format_error(result, CsvReason::Encoding, "CSV contains invalid UTF-8");
            return kInvalid;
        }
    }
    return static_cast<int>(byte);
}

bool CsvReader::read_field(int& value, CsvRead& result) {
    field_.clear();
    bool quoted = value == '"';
    if (quoted) value = read_byte(result);
    for (;;) {
        if (value == kIo || value == kInvalid) return false;
        // DictReader's default dialect is non-strict: EOF also ends an
        // unclosed quoted field. Numeric validation still applies afterwards.
        if (value == kEnd) return true;
        if (quoted) {
            if (value == '"') {
                value = read_byte(result);
                if (value == kIo || value == kInvalid) return false;
                if (value != '"') {
                    quoted = false;
                    continue;
                }
            }
        } else if (value == ',' || value == '\n' || value == '\r') {
            return true;
        }
        field_.push_back(static_cast<char>(value));
        value = read_byte(result);
    }
}

bool CsvReader::finish_line(int delimiter, CsvRead& result) {
    if (delimiter == kIo || delimiter == kInvalid) return false;
    if (delimiter == kEnd) {
        ended_ = true;
        return true;
    }
    if (delimiter == '\n') return true;
    if (delimiter == '\r') {
        const int next_byte = read_byte(result, true);
        if (next_byte == kIo || next_byte == kInvalid) return false;
        if (next_byte == kEnd) ended_ = true;
        if (next_byte == '\n') return read_byte(result) == '\n';
        return true;
    }
    format_error(result, CsvReason::Syntax, "CSV record terminator is invalid");
    return false;
}

CsvRead CsvReader::read_header() {
    CsvRead result;
    if (header_read_) {
        format_error(result, CsvReason::Syntax, "CSV header was already read");
        return result;
    }
    int value = read_byte(result);
    for (std::uint64_t column = 0;; ++column) {
        if (!read_field(value, result)) return result;
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (field_ == kColumns[i]) columns_[i] = column;
        }
        if (value != ',') break;
        value = read_byte(result);
    }
    if (!finish_line(value, result)) return result;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i] == kNoColumn) {
            format_error(result, CsvReason::Columns, i == 5
                ? "CSV chart requires volume; optional-volume feeds require a feed reader"
                : "CSV chart requires timestamp,open,high,low,close,volume without BOM stripping");
            return result;
        }
    }
    header_read_ = true;
    return result;
}

CsvRead CsvReader::next() {
    CsvRead result;
    if (!header_read_) {
        format_error(result, CsvReason::Syntax, "CSV header must be read first");
        return result;
    }
    if (ended_) {
        result.status = CsvStatus::End;
        return result;
    }
    int value = read_byte(result);
    if (value == kIo || value == kInvalid) return result;
    if (value == kEnd) {
        ended_ = true;
        result.status = CsvStatus::End;
        return result;
    }

    if (value == '\n' || value == '\r') {
        format_error(result, CsvReason::Syntax, "CSV blank records are outside the chart reader scope");
        return result;
    }
    unsigned seen = 0;
    for (std::uint64_t column = 0;; ++column) {
        if (!read_field(value, result)) return result;
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i] != column) continue;
            if (!number_text(field_, result)) return result;
            if (!assign_field(result.bar, i, field_.data(), field_.size())) {
                format_error(result, CsvReason::Value, i == 0
                    ? "CSV timestamp must be an int64 integer count of Unix milliseconds"
                    : i == 5 ? "CSV chart volume must be nonnegative finite or NaN, never missing"
                             : "CSV OHLC price must be a finite decimal double");
                return result;
            }
            seen |= 1u << i;
        }
        if (value != ',') break;
        value = read_byte(result);
    }
    if (!finish_line(value, result)) return result;
    if (seen != (1u << columns_.size()) - 1) {
        format_error(result, CsvReason::Value, "CSV record is missing a required chart value");
        return result;
    }
    const Bar& bar = result.bar;
    if (bar.volume < 0) {
        format_error(result, CsvReason::Value, "CSV volume must be nonnegative or NaN");
    } else if (bar.low > bar.high || bar.open < bar.low || bar.open > bar.high
        || bar.close < bar.low || bar.close > bar.high) {
        format_error(result, CsvReason::Value, "CSV OHLC range violates the chart reader invariant");
    }
    return result;
}

}  // namespace pineforge::detail::selected_capacity
