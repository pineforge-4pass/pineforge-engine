#pragma once
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include <pineforge/na.hpp>

namespace pineforge {

// One argument of str_format_values: a number, which its placeholder's style
// renders, or a text, which is inserted as it is. A bool is its text, "true"
// or "false"; a signed integer's na is the number na.
struct StrFormatValue {
    enum class Kind { Number, Text };
    Kind kind;
    double number = 0.0;
    std::string text;

    template <class T, std::enable_if_t<std::is_arithmetic_v<T>
                                        && !std::is_same_v<T, bool>, int> = 0>
    StrFormatValue(T value) : kind(Kind::Number), number(static_cast<double>(value)) {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            if (value == std::numeric_limits<T>::min()) number = na<double>();
        }
    }
    StrFormatValue(bool value) : kind(Kind::Text), text(value ? "true" : "false") {}
    StrFormatValue(const std::string& value) : kind(Kind::Text), text(value) {}
    StrFormatValue(const char* value) : kind(Kind::Text), text(value) {}
};

// The source language's MessageFormat subset: `{i}` inserts argument i, and
// a number argument is rendered by `{i}` as the grouped "#,###.###" pattern
// and by `{i,number,<style>}` in the style -- none (the same pattern),
// `integer`, `percent` (x100, no fraction), `currency` ("$", two decimals,
// grouped) or a decimal pattern (see str_tostring). A text argument is
// inserted as it is whatever the placeholder's type and style. Text between
// single quotes is literal and `''` is one quote. A placeholder whose index
// is not a number, names no argument, or (for a number) has a type other
// than `number` is kept as written; an unclosed `{` keeps the rest.
std::string str_format_values(const std::string& fmt,
                              const std::vector<StrFormatValue>& args);

// str_format_values with every argument a text: `{i}` and
// `{i,number,<style>}` insert args[i] as it is, and quoting and the kept
// placeholders follow the same rules. A number reaches it already rendered,
// by str_tostring.
std::string str_format(const std::string& fmt,
                       const std::vector<std::string>& args);

std::string str_format_time(long long timestamp_ms,
                            const std::string& format,
                            const std::string& timezone);

std::string str_match(const std::string& source,
                      const std::string& regex_pattern);

std::vector<std::string> str_split(const std::string& source,
                                   const std::string& separator);

// A number's text, from its shortest round-trip decimal spelling, every
// scaling and rounding (half-up) done on those digits. NaN is "NaN", the
// infinities "Infinity" and "-Infinity", and a value that rounds to zero has
// no sign. format_mode:
//   ""         up to ten fraction digits, trailing zeros dropped
//              (0.1234567890123 -> "0.123456789", 1234.5 -> "1234.5")
//   "percent"  up to two fraction digits and a "%": the value is not scaled
//              (1.005 -> "1.01%")
//   "volume"   up to two fraction digits of the value in thousands,
//              millions, billions or trillions with a "K", "M", "B" or "T",
//              none below a thousand (1005 -> "1.01K", 12.34 -> "12")
//   "mintick"  the value rounded to `mintick` (> 0) with its decimals kept
//              (1.2 at 0.01 -> "1.20"); a non-positive tick is the default
//   otherwise  a decimal pattern: '0' a required and '#' an optional
//              fraction digit, ',' before the point groups by thousands,
//              '%' scales by 100 (0.1234 at "#.##%" -> "12.34%")
std::string str_tostring(double value,
                         const std::string& format_mode = "",
                         double mintick = 0.0);

// Deprecated `pine_*` spellings: exact inline forwards kept for generated code
// and the source adapter. New code calls the neutral names above.

inline std::string pine_str_format(const std::string& fmt,
                                   const std::vector<std::string>& args) {
    return str_format(fmt, args);
}

inline std::string pine_str_format_time(long long timestamp_ms,
                                        const std::string& format,
                                        const std::string& timezone) {
    return str_format_time(timestamp_ms, format, timezone);
}

inline std::string pine_str_match(const std::string& source,
                                  const std::string& regex_pattern) {
    return str_match(source, regex_pattern);
}

inline std::vector<std::string> pine_str_split(const std::string& source,
                                               const std::string& separator) {
    return str_split(source, separator);
}

inline std::string pine_str_tostring(double value,
                                     const std::string& format_mode = "",
                                     double mintick = 0.0) {
    return str_tostring(value, format_mode, mintick);
}

} // namespace pineforge
