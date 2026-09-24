#include <pineforge/str_utils.hpp>
#include "timezone.hpp"
#include <charconv>
#include <cmath>
#include <ctime>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <algorithm>
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
        throw std::runtime_error("shortest-decimal conversion failed");
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

} // namespace

// ---------- str.format ----------

std::string str_format_values(const std::string& fmt,
                              const std::vector<StrFormatValue>& args) {
    std::string result;
    bool quoted = false;
    for (size_t i = 0; i < fmt.size();) {
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
        const size_t end = fmt.find('}', i + 1);
        if (end == std::string::npos) {
            result += fmt.substr(i);
            break;
        }
        const std::string inside = fmt.substr(i + 1, end - i - 1);
        const size_t first_comma = inside.find(',');
        const std::string index_text = trim_blanks(inside.substr(0, first_comma));
        if (index_text.empty() || !std::all_of(index_text.begin(), index_text.end(),
                                               [](char ch) { return ch >= '0' && ch <= '9'; })) {
            result += fmt.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }
        // An index too long for size_t names no argument either.
        const size_t index = index_text.size() > 9 ? args.size()
            : static_cast<size_t>(std::stoul(index_text));
        if (index >= args.size()) {
            result += fmt.substr(i, end - i + 1);
            i = end + 1;
            continue;
        }
        const auto& arg = args[index];
        if (arg.kind == StrFormatValue::Kind::Text) {
            result += arg.text;
        } else if (first_comma == std::string::npos) {
            result += number_style_text(arg.number, "");
        } else {
            const size_t second_comma = inside.find(',', first_comma + 1);
            const std::string type = trim_blanks(inside.substr(
                first_comma + 1, second_comma - first_comma - 1));
            if (type == "number") {
                const std::string style = second_comma == std::string::npos
                    ? "" : inside.substr(second_comma + 1);
                result += number_style_text(arg.number, style);
            } else {
                result += fmt.substr(i, end - i + 1);
            }
        }
        i = end + 1;
    }
    return result;
}

std::string str_format(const std::string& fmt,
                            const std::vector<std::string>& args) {
    return str_format_values(fmt, std::vector<StrFormatValue>(args.begin(), args.end()));
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
