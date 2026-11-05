#pragma once

#include <pineforge/bar.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <string>

namespace pineforge::detail::selected_capacity {

enum class CsvStatus { Ok, End, FormatError, IoError };
enum class CsvReason { None, Columns, Value, Syntax, Encoding };

struct CsvRead {
    CsvStatus status = CsvStatus::Ok;
    CsvReason reason = CsvReason::None;
    Bar bar{};
    std::uint64_t csv_bytes = 0;
    const char* message = "";
};

// Private, single-pass CHART CSV reader, following pf_bars.py's chart branch:
// UTF-8, case-sensitive named timestamp/open/high/low/close/volume columns,
// reordered/quoted columns and ignored extras. Duplicate names use the last
// column, like DictReader. Quotes, doubled quotes and embedded newlines follow
// its default, non-strict dialect. A chart BOM is not stripped from a name.
// Timestamp: ASCII int() spelling (outer whitespace, sign, decimal digits and
// single underscores between digits), representable as int64 milliseconds.
// OHLC and finite volume: [+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?
// with outer ASCII numeric whitespace and underscores between digits.
// OHLC must be representable finite doubles. Volume must be nonnegative and
// finite, or [+-]?[nN][aA][nN] without a payload. Require low <= open/close
// <= high (a retained reader restriction, NOT a pf_bars.py requirement).
// Prices and timestamps may be negative. LF, CRLF, CR and EOF are accepted.
// Missing/empty chart volume is refused. time_close is an ignored chart extra;
// this interface cannot validate feed closes or derive them without a timeframe.
// Feed BOM stripping and optional volume are not inferred from column shape.
// Non-ASCII numeric spelling and blank records have named Encoding/Syntax
// refusals; this is not the full Python int()/float()/DictReader input domain.
//
// One reusable field string and six column positions accompany one parsed Bar.
// There is no lexical byte cap; allocation and string-length failures propagate.
// No line/file string, seeking, second read or owning stream.
// csv_bytes is the actual octet count for this header/record, including its
// delimiters and terminator. Only an Ok result is a validated observation.
class CsvReader {
public:
    explicit CsvReader(std::istream& input) noexcept : input_(input) {}
    CsvRead read_header();
    CsvRead next();

private:
    static constexpr int kEnd = -1;
    static constexpr int kIo = -2;
    static constexpr int kInvalid = -3;
    static constexpr std::uint64_t kNoColumn = std::numeric_limits<std::uint64_t>::max();
    int read_byte(CsvRead& result, bool peek = false);
    bool read_field(int& value, CsvRead& result);
    bool finish_line(int delimiter, CsvRead& result);

    std::istream& input_;
    std::string field_;
    std::array<std::uint64_t, 6> columns_{
        kNoColumn, kNoColumn, kNoColumn, kNoColumn, kNoColumn, kNoColumn};
    unsigned utf8_remaining_ = 0;
    unsigned utf8_min_ = 0x80;
    unsigned utf8_max_ = 0xbf;
    bool header_read_ = false;
    bool ended_ = false;
};

}  // namespace pineforge::detail::selected_capacity
