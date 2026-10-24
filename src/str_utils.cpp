#include <pineforge/str_utils.hpp>
#include <pineforge/run_failure.hpp>
#include "timezone.hpp"
#include <charconv>
#include <cmath>
#include <ctime>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <algorithm>
#include <array>
#include <cstring>
#include <system_error>

namespace pineforge {

// ---------- number text ----------
//
// The published rendering of Pine's str.tostring / str.format numbers, which
// TradingView's exported tapes pin (the codegen formatter's rule, lane C6b).
// A number is rendered from its shortest round-trip decimal spelling
// (std::to_chars): every scaling and every rounding is done on those digits,
// half-up on the first discarded digit, never on a floating-point
// intermediate. Kernel order, fill and settlement code never calls these.

namespace {

std::string trim_blanks(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

void increment_digits(std::string& digits) {
    for (size_t i = digits.size(); i > 0; --i) {
        if (digits[i - 1] != '9') {
            ++digits[i - 1];
            return;
        }
        digits[i - 1] = '0';
    }
    digits.insert(digits.begin(), '1');
}

// `value` times 10^decimal_shift with between min_fraction and max_fraction
// (at most 15) fraction digits, grouped by thousands when asked. NaN, the two
// infinities and a value that rounds to zero have their own spellings.
std::string decimal_text(double value, int min_fraction, int max_fraction,
                         bool grouping, int decimal_shift = 0) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    max_fraction = std::max(0, std::min(max_fraction, 15));
    min_fraction = std::max(0, std::min(min_fraction, max_fraction));

    char buffer[128];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value);
    if (converted.ec != std::errc{})
        throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                        "shortest-decimal conversion failed");
    std::string spelling(buffer, converted.ptr);
    const bool negative = !spelling.empty() && spelling[0] == '-';
    if (negative) spelling.erase(0, 1);

    const size_t exponent_pos = spelling.find_first_of("eE");
    const std::string mantissa = spelling.substr(0, exponent_pos);
    int exponent = 0;
    if (exponent_pos != std::string::npos) {
        size_t i = exponent_pos + 1;
        bool exponent_negative = false;
        if (i < spelling.size() && (spelling[i] == '+' || spelling[i] == '-')) {
            exponent_negative = spelling[i] == '-';
            ++i;
        }
        for (; i < spelling.size(); ++i)
            exponent = exponent * 10 + (spelling[i] - '0');
        if (exponent_negative) exponent = -exponent;
    }

    std::string digits;
    int decimal_point = 0;
    bool after_point = false;
    for (char ch : mantissa) {
        if (ch == '.') {
            after_point = true;
        } else {
            digits += ch;
            if (!after_point) ++decimal_point;
        }
    }
    decimal_point += exponent + decimal_shift;
    const size_t leading_zeroes = digits.find_first_not_of('0');
    if (leading_zeroes == std::string::npos) {
        digits = "0";
        decimal_point = 1;
    } else {
        digits.erase(0, leading_zeroes);
        decimal_point -= static_cast<int>(leading_zeroes);
    }

    // The retained digits form an integer in units of 10^-max_fraction; the
    // first discarded digit decides a half-up tie exactly.
    std::string units;
    if (digits == "0") {
        units = "0";
    } else {
        const int keep = decimal_point + max_fraction;
        if (keep < 0) {
            units = "0";
        } else if (keep == 0) {
            units = digits[0] >= '5' ? "1" : "0";
        } else if (keep >= static_cast<int>(digits.size())) {
            units = digits;
            units.append(static_cast<size_t>(keep) - digits.size(), '0');
        } else {
            units = digits.substr(0, static_cast<size_t>(keep));
            if (digits[static_cast<size_t>(keep)] >= '5') increment_digits(units);
        }
    }
    const bool nonzero = units.find_first_not_of('0') != std::string::npos;
    if (!nonzero) units = "0";
    if (units.size() <= static_cast<size_t>(max_fraction))
        units.insert(0, static_cast<size_t>(max_fraction) + 1 - units.size(), '0');
    const size_t integer_size = units.size() - static_cast<size_t>(max_fraction);
    std::string integer = units.substr(0, integer_size);
    std::string fraction = units.substr(integer_size);
    while (fraction.size() > static_cast<size_t>(min_fraction) && fraction.back() == '0')
        fraction.pop_back();
    if (grouping) {
        std::string grouped;
        for (size_t i = 0; i < integer_size; ++i) {
            if (i && (integer_size - i) % 3 == 0) grouped += ',';
            grouped += integer[i];
        }
        integer = grouped;
    }
    return (negative && nonzero ? "-" : "") + integer + (fraction.empty() ? "" : "." + fraction);
}

// A decimal pattern: '0' is a required fraction digit and '#' an optional
// one, a ',' before the point groups by thousands, and a '%' anywhere scales
// by 100 and appends the sign.
std::string pattern_text(double value, const std::string& pattern) {
    if (!std::isfinite(value)) return decimal_text(value, 0, 0, false);
    const std::string fmt = trim_blanks(pattern);
    const size_t dot = fmt.find('.');
    const std::string integer = fmt.substr(0, dot);
    const std::string fraction = dot == std::string::npos ? "" : fmt.substr(dot + 1);
    const int max_fraction = static_cast<int>(std::count_if(
        fraction.begin(), fraction.end(), [](char c) { return c == '0' || c == '#'; }));
    const int min_fraction = static_cast<int>(std::count(fraction.begin(), fraction.end(), '0'));
    const bool grouping = integer.find(',') != std::string::npos;
    const bool percent = fmt.find('%') != std::string::npos;
    return decimal_text(value, min_fraction, max_fraction, grouping, percent ? 2 : 0)
        + (percent ? "%" : "");
}

// A `{i,number,<style>}` placeholder's style: none (three grouped decimals),
// integer, percent, currency, or a decimal pattern.
std::string number_style_text(double value, const std::string& style) {
    if (!std::isfinite(value)) return decimal_text(value, 0, 0, false);
    const std::string fmt = trim_blanks(style);
    if (fmt.empty()) return pattern_text(value, "#,###.###");
    if (fmt == "integer") return pattern_text(value, "#,###");
    if (fmt == "percent") return decimal_text(value, 0, 0, true, 2) + "%";
    if (fmt == "currency") {
        const std::string digits = decimal_text(value, 2, 2, true);
        return digits[0] == '-' ? "-$" + digits.substr(1) : "$" + digits;
    }
    return pattern_text(value, fmt);
}

ScriptString trim_blanks(const ScriptString& value) {
    auto first = ScriptString::size_type{0}, last = value.size();
    while (first < last && (value[first] == ' ' || value[first] == '\t')) ++first;
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t')) --last;
    return value.substr(first, last - first);
}

// Binary64 has at most max_exponent10+1 integer digits. The only positive
// decimal scale below is percent (+2); reserve one more digit for rounding,
// fifteen fraction digits, grouping separators, sign, point and two affixes.
// This is a bound on a NUMBER's spelling, never on script string length.
struct NumberText {
    static constexpr std::size_t integer_digits =
        static_cast<std::size_t>(std::numeric_limits<double>::max_exponent10) + 4;
    static constexpr std::size_t fraction_digits = 15;
    static constexpr std::size_t capacity =
        integer_digits + integer_digits / 3 + fraction_digits + 4;
    std::array<char, capacity> data{};
    std::size_t size = 0;
    std::string_view view() const noexcept { return {data.data(), size}; }
    void append(char ch) { data[size++] = ch; }
    void append(std::string_view text) {
        std::memcpy(data.data() + size, text.data(), text.size());
        size += text.size();
    }
};

NumberText script_decimal_text(double value, int min_fraction, int max_fraction,
                               bool grouping, int decimal_shift = 0) {
    NumberText result;
    if (std::isnan(value)) { result.append("NaN"); return result; }
    if (std::isinf(value)) {
        result.append(value < 0 ? "-Infinity" : "Infinity");
        return result;
    }
    max_fraction = std::max(0, std::min(max_fraction, 15));
    min_fraction = std::max(0, std::min(min_fraction, max_fraction));
    std::array<char, std::numeric_limits<double>::max_digits10 + 8> spelling{};
    const auto converted = std::to_chars(spelling.data(), spelling.data() + spelling.size(), value);
    if (converted.ec != std::errc{})
        throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                        "shortest-decimal conversion failed");
    const auto spelling_size = static_cast<std::size_t>(converted.ptr - spelling.data());
    const bool negative = spelling[0] == '-';
    std::size_t exponent_at = negative ? 1 : 0;
    while (exponent_at < spelling_size && spelling[exponent_at] != 'e' &&
           spelling[exponent_at] != 'E') ++exponent_at;
    int exponent = 0;
    if (exponent_at != spelling_size) {
        auto at = exponent_at + 1;
        const bool minus = at < spelling_size && spelling[at] == '-';
        if (at < spelling_size && (spelling[at] == '-' || spelling[at] == '+')) ++at;
        for (; at < spelling_size; ++at) exponent = exponent * 10 + spelling[at] - '0';
        if (minus) exponent = -exponent;
    }
    // A shortest FIXED spelling can contain more digits than max_digits10
    // (e.g. an exactly representable 18-digit integer). It is still no longer
    // than the scientific alternative bounded by the spelling buffer.
    std::array<char, std::numeric_limits<double>::max_digits10 + 8> digits{};
    std::size_t digit_count = 0;
    int decimal_point = 0;
    bool after_point = false;
    for (std::size_t at = negative ? 1 : 0; at < exponent_at; ++at) {
        const auto ch = spelling[at];
        if (ch == '.') after_point = true;
        else {
            digits[digit_count++] = ch;
            if (!after_point) ++decimal_point;
        }
    }
    decimal_point += exponent + decimal_shift;
    std::size_t leading = 0;
    while (leading < digit_count && digits[leading] == '0') ++leading;
    if (leading == digit_count) {
        digits[0] = '0'; digit_count = 1; decimal_point = 1;
    } else {
        std::memmove(digits.data(), digits.data() + leading, digit_count - leading);
        digit_count -= leading;
        decimal_point -= static_cast<int>(leading);
    }
    std::array<char, NumberText::integer_digits + NumberText::fraction_digits> units{};
    std::size_t unit_count = 0;
    const int keep = decimal_point + max_fraction;
    if ((digit_count == 1 && digits[0] == '0') || keep < 0) {
        units[unit_count++] = '0';
    } else if (keep == 0) {
        units[unit_count++] = digits[0] >= '5' ? '1' : '0';
    } else {
        const auto kept = static_cast<std::size_t>(keep);
        const auto copied = std::min(kept, digit_count);
        std::memcpy(units.data(), digits.data(), copied);
        unit_count = kept;
        std::fill(units.begin() + static_cast<std::ptrdiff_t>(copied),
                  units.begin() + static_cast<std::ptrdiff_t>(unit_count), '0');
        if (kept < digit_count && digits[kept] >= '5') {
            std::size_t at = unit_count;
            while (at != 0 && units[at - 1] == '9') units[--at] = '0';
            if (at != 0) ++units[at - 1];
            else {
                std::memmove(units.data() + 1, units.data(), unit_count);
                units[0] = '1'; ++unit_count;
            }
        }
    }
    bool nonzero = false;
    for (std::size_t at = 0; at < unit_count; ++at) nonzero = nonzero || units[at] != '0';
    if (!nonzero) { units[0] = '0'; unit_count = 1; }
    const auto max_digits = static_cast<std::size_t>(max_fraction);
    if (unit_count <= max_digits) {
        const auto padding = max_digits + 1 - unit_count;
        std::memmove(units.data() + padding, units.data(), unit_count);
        std::fill_n(units.data(), padding, '0');
        unit_count += padding;
    }
    const auto integer_size = unit_count - max_digits;
    auto fraction_size = max_digits;
    while (fraction_size > static_cast<std::size_t>(min_fraction) &&
           units[integer_size + fraction_size - 1] == '0') --fraction_size;
    if (negative && nonzero) result.append('-');
    for (std::size_t at = 0; at < integer_size; ++at) {
        if (grouping && at != 0 && (integer_size - at) % 3 == 0) result.append(',');
        result.append(units[at]);
    }
    if (fraction_size != 0) {
        result.append('.');
        result.append(std::string_view(units.data() + integer_size, fraction_size));
    }
    return result;
}

NumberText script_pattern_text(double value, const ScriptString& pattern) {
    if (!std::isfinite(value)) return script_decimal_text(value, 0, 0, false);
    const auto fmt = trim_blanks(pattern);
    std::uint64_t required = 0, optional = 0;
    bool fraction = false, grouping = false, percent = false;
    std::array<char, ScriptString::chunk_bytes> buffer{};
    for (ScriptString::size_type offset = 0; offset < fmt.size();) {
        const auto n = fmt.read(offset, buffer.data(), buffer.size());
        for (std::size_t i = 0; i < n; ++i) {
            const char ch = buffer[i];
            if (ch == '.' && !fraction) { fraction = true; continue; }
            grouping = grouping || (!fraction && ch == ',');
            percent = percent || ch == '%';
            if (fraction && ch == '0') ++required;
            if (fraction && ch == '#') ++optional;
        }
        offset += n;
    }
    auto result = script_decimal_text(value, static_cast<int>(required),
                                      static_cast<int>(required + optional), grouping, percent ? 2 : 0);
    if (percent) result.append('%');
    return result;
}

NumberText number_style_text(double value, const ScriptString& style) {
    if (!std::isfinite(value)) return script_decimal_text(value, 0, 0, false);
    const auto fmt = trim_blanks(style);
    if (fmt.empty()) return script_decimal_text(value, 0, 3, true);
    if (fmt == "integer") return script_decimal_text(value, 0, 0, true);
    if (fmt == "percent") {
        auto result = script_decimal_text(value, 0, 0, true, 2);
        result.append('%');
        return result;
    }
    if (fmt == "currency") {
        auto result = script_decimal_text(value, 2, 2, true);
        const auto at = result.data[0] == '-' ? std::size_t{1} : std::size_t{0};
        std::memmove(result.data.data() + at + 1, result.data.data() + at, result.size - at);
        result.data[at] = '$'; ++result.size;
        return result;
    }
    return script_pattern_text(value, fmt);
}

template <class TextType> struct TextOnlyArg {
    enum class Kind { Text };
    Kind kind = Kind::Text;
    double number = 0;
    const TextType& text;
};
template <class Text> struct TextArgs {
    const std::vector<Text>& values;
    std::size_t size() const { return values.size(); }
    TextOnlyArg<Text> operator[](std::size_t i) const { return {TextOnlyArg<Text>::Kind::Text, 0, values[i]}; }
};

// One grammar for legacy and external strings. TextArgs borrows arguments;
// formatting an accumulated text no longer constructs owning argument copies.
template <class Text, class Args>
Text format_values(const Text& fmt, const Args& args) {
    Text result;
    if constexpr (std::is_same_v<Text, ScriptString>) {
        result = fmt;
        result.clear(); // Keep a compatibility Store owner with the result.
    }
    bool quoted = false;
    using Index = typename Text::size_type;
    for (Index i = 0; i < fmt.size();) {
        const char c = fmt[i];
        if (c == '\'') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '\'') {
                result += '\'';
                i += 2;
            } else {
                quoted = !quoted;
                ++i;
            }
            continue;
        }
        if (c != '{' || quoted) {
            result += c;
            ++i;
            continue;
        }
        const Index end = fmt.find('}', i + 1);
        if (end == Text::npos) {
            result += fmt.substr(i);
            break;
        }
        const Text inside = fmt.substr(i + 1, end - i - 1);
        const Index first_comma = inside.find(',');
        const Text index_text = trim_blanks(inside.substr(0, first_comma));
        bool digits = !index_text.empty();
        for (Index j = 0; digits && j < index_text.size(); ++j)
            digits = index_text[j] >= '0' && index_text[j] <= '9';
        if (!digits) {
            result += fmt.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }
        // An index too long for size_t names no argument either.
        size_t index = 0;
        if (index_text.size() > 9) index = args.size();
        else for (Index j = 0; j < index_text.size(); ++j)
            index = index * 10 + static_cast<unsigned>(index_text[j] - '0');
        if (index >= args.size()) {
            result += fmt.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }
        const auto& arg = args[index];
        if (arg.kind == std::decay_t<decltype(arg)>::Kind::Text) {
            result += arg.text;
        } else if (first_comma == Text::npos) {
            if constexpr (std::is_same_v<Text, ScriptString>) {
                const auto rendered = script_decimal_text(arg.number, 0, 3, true);
                result.append(rendered.view());
            } else result += number_style_text(arg.number, std::string(""));
        } else {
            const Index second_comma = inside.find(',', first_comma + 1);
            const Text type = trim_blanks(inside.substr(
                first_comma + 1, second_comma - first_comma - 1));
            if (type == "number") {
                const Text style = second_comma == Text::npos
                    ? Text{} : inside.substr(second_comma + 1);
                if constexpr (std::is_same_v<Text, ScriptString>) {
                    const auto rendered = number_style_text(arg.number, style);
                    result.append(rendered.view());
                } else result += number_style_text(arg.number, style);
            } else {
                result += fmt.substr(i, end - i + 1);
            }
        }
        i = end + 1;
    }
    return result;
}

} // namespace

std::string str_format_values(const std::string& fmt,
                              const std::vector<StrFormatValue>& args) {
    return format_values(fmt, args);
}
std::string str_format(const std::string& fmt,
                            const std::vector<std::string>& args) {
    return format_values(fmt, TextArgs<std::string>{args});
}
ScriptString script_str_format_values(const ScriptString& fmt,
                                      const std::vector<ScriptStrFormatValue>& args) {
    return format_values(fmt, args);
}
ScriptString script_str_format(const ScriptString& fmt, const std::vector<ScriptString>& args) {
    return format_values(fmt, TextArgs<ScriptString>{args});
}
ScriptString str_tostring(detail::o1::Store& store, double value,
                          const ScriptString& mode, double mintick) {
    if (!std::isfinite(value) || mode.empty()) {
        const auto rendered = script_decimal_text(value, 0, 10, false);
        return ScriptString(store, rendered.view());
    }
    if (mode == "mintick" && mintick > 0) {
        // Keep the legacy stream's locale and binary tick-rounding behavior
        // at this explicit bounded numeric boundary.
        return ScriptString(store, str_tostring(value, "mintick", mintick));
    }
    if (mode == "mintick") {
        const auto rendered = script_decimal_text(value, 0, 10, false);
        return ScriptString(store, rendered.view());
    }
    if (mode == "percent") {
        auto rendered = script_decimal_text(value, 0, 2, false);
        rendered.append('%');
        return ScriptString(store, rendered.view());
    }
    if (mode == "volume") {
        const double magnitude = std::fabs(value);
        int shift = 0;
        char unit = '\0';
        if (magnitude >= 1e12) { shift = -12; unit = 'T'; }
        else if (magnitude >= 1e9) { shift = -9; unit = 'B'; }
        else if (magnitude >= 1e6) { shift = -6; unit = 'M'; }
        else if (magnitude >= 1e3) { shift = -3; unit = 'K'; }
        auto rendered = script_decimal_text(value, 0, shift ? 2 : 0, false, shift);
        if (unit) rendered.append(unit);
        return ScriptString(store, rendered.view());
    }
    const auto rendered = script_pattern_text(value, mode);
    return ScriptString(store, rendered.view());
}

// ---------- str.format_time ----------

std::string str_format_time(long long timestamp_ms,
                                 const std::string& format,
                                 const std::string& timezone) {
    time_t secs = static_cast<time_t>(timestamp_ms / 1000);
    struct tm tm_buf;
    if (!timezone.empty() && timezone != "UTC" && timezone != "Etc/UTC") {
        tz_util::ScopedTimezone guard(timezone);
        localtime_r(&secs, &tm_buf);
    } else {
        gmtime_r(&secs, &tm_buf);
    }

    // Map Pine tokens to strftime specifiers
    std::string fmt = format;
    // Order matters: longer tokens first to avoid partial replacement
    auto replace_all = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = fmt.find(from, pos)) != std::string::npos) {
            fmt.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all("yyyy", "%Y");
    replace_all("MM", "%m");
    replace_all("dd", "%d");
    replace_all("HH", "%H");
    replace_all("mm", "%M");
    replace_all("ss", "%S");

    char buf[256];
    strftime(buf, sizeof(buf), fmt.c_str(), &tm_buf);
    return std::string(buf);
}

// ---------- str.match ----------

std::string str_match(const std::string& source,
                           const std::string& regex_pattern) {
    try {
        std::regex re(regex_pattern);
        std::smatch match;
        if (std::regex_search(source, match, re)) {
            // Return first capture group if exists, else full match
            if (match.size() > 1) {
                return match[1].str();
            }
            return match[0].str();
        }
    } catch (...) {
        // regex error
    }
    return "";
}

// ---------- str.split ----------

std::vector<std::string> str_split(const std::string& source,
                                        const std::string& separator) {
    if (separator.empty()) {
        return {source};
    }
    std::vector<std::string> result;
    size_t start = 0;
    size_t pos;
    while ((pos = source.find(separator, start)) != std::string::npos) {
        result.push_back(source.substr(start, pos - start));
        start = pos + separator.size();
    }
    result.push_back(source.substr(start));
    return result;
}

// ---------- str.tostring ----------

std::string str_tostring(double value,
                              const std::string& format_mode,
                              double mintick) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-Infinity" : "Infinity";
    }

    if (format_mode == "mintick" && mintick > 0.0) {
        // Determine decimal places from mintick
        int decimals = 0;
        double mt = mintick;
        while (mt < 1.0 - 1e-10 && decimals < 15) {
            mt *= 10.0;
            ++decimals;
        }
        double rounded = std::round(value / mintick) * mintick;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(decimals) << rounded;
        return oss.str();
    }

    if (format_mode == "percent") {
        return pattern_text(value, "#.##") + "%";
    }

    if (format_mode == "volume") {
        // The unit only chooses the shift; the digits are the value's own.
        const double magnitude = std::fabs(value);
        int decimal_shift = 0;
        const char* unit = "";
        if (magnitude >= 1.0e12) { decimal_shift = -12; unit = "T"; }
        else if (magnitude >= 1.0e9) { decimal_shift = -9; unit = "B"; }
        else if (magnitude >= 1.0e6) { decimal_shift = -6; unit = "M"; }
        else if (magnitude >= 1.0e3) { decimal_shift = -3; unit = "K"; }
        return decimal_text(value, 0, decimal_shift ? 2 : 0, false, decimal_shift) + unit;
    }

    // Default: up to ten fraction digits. A mintick request without a
    // positive tick renders as the default too; any other mode is a decimal
    // pattern.
    if (format_mode.empty() || format_mode == "mintick") {
        return pattern_text(value, "#.##########");
    }
    return pattern_text(value, format_mode);
}

} // namespace pineforge
