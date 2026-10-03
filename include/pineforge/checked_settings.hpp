#pragma once

#include <pineforge/pineforge.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace pineforge::checked_settings {

struct Error {
    int status;
    const char* message;
};

inline void require(bool condition, const char* message,
                    int status = PF_SETTINGS_INVALID_ARGUMENT) {
    if (!condition) throw Error{status, message};
}

inline void copy_error(char* error, std::size_t capacity, const char* message) noexcept {
    if (!error || capacity == 0) return;
    const auto count = std::min(capacity - 1, std::strlen(message));
    std::memcpy(error, message, count);
    error[count] = '\0';
}

template <typename Function>
int boundary(char* error, std::size_t capacity, Function&& function) noexcept {
    copy_error(error, capacity, "");
    try {
        std::forward<Function>(function)();
        return PF_SETTINGS_OK;
    } catch (const Error& failure) {
        copy_error(error, capacity, failure.message);
        return failure.status;
    } catch (const std::exception& failure) {
        copy_error(error, capacity, failure.what());
    } catch (...) {
        copy_error(error, capacity, "unknown C++ exception");
    }
    return PF_SETTINGS_EXCEPTION;
}

inline std::string number(double value) {
    if (!std::isfinite(value)) return "na";
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

template <typename Integer, std::enable_if_t<std::is_integral_v<Integer>
    && !std::is_same_v<Integer, bool>, int> = 0>
inline std::string number(Integer value) { return std::to_string(value); }
inline std::string number(bool value) { return value ? "true" : "false"; }

inline std::int64_t integer(const std::string& value, int bits = 32) {
    require(!value.empty(), "expected an integer");
    const char* begin = value.data();
    const char* end = begin + value.size();
    if (*begin == '+') {
        ++begin;
        require(begin != end && *begin != '-' && *begin != '+', "invalid integer sign");
    }
    require(begin != end && (*begin != '-' || begin + 1 != end), "expected an integer");
    std::int64_t parsed = 0;
    const auto result = std::from_chars(begin, end, parsed);
    require(result.ec == std::errc{} && result.ptr == end, "invalid integer or trailing bytes");
    require(bits == 64 || (parsed >= std::numeric_limits<int>::min()
                          && parsed <= std::numeric_limits<int>::max()), "integer out of range");
    return parsed;
}

inline double real(const std::string& value) {
    require(!value.empty(), "expected a finite decimal number");
    std::size_t offset = 0;
    if (value[offset] == '+' || value[offset] == '-') ++offset;
    bool digits = false;
    while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') {
        digits = true;
        ++offset;
    }
    if (offset < value.size() && value[offset] == '.') {
        ++offset;
        while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') {
            digits = true;
            ++offset;
        }
    }
    require(digits, "expected a finite decimal number");
    const auto mantissa_end = offset;
    if (offset < value.size() && (value[offset] == 'e' || value[offset] == 'E')) {
        ++offset;
        if (offset < value.size() && (value[offset] == '+' || value[offset] == '-')) ++offset;
        const auto exponent_begin = offset;
        while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') ++offset;
        require(offset != exponent_begin, "invalid numeric exponent");
    }
    require(offset == value.size(), "invalid number or trailing bytes");
    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    double parsed = 0;
    stream >> std::noskipws >> parsed;
    require(!stream.fail() && stream.eof() && std::isfinite(parsed), "number out of finite range");
    if (parsed == 0) {
        require(value.substr(0, mantissa_end).find_first_of("123456789") == std::string::npos,
                "number underflows to zero");
    }
    const auto canonical = number(parsed);
    try {
        std::size_t consumed = 0;
        const double roundtrip = std::stod(canonical, &consumed);
        require(consumed == canonical.size() && roundtrip == parsed,
                "number cannot be consumed by the strategy getter");
    } catch (const std::invalid_argument&) {
        throw Error{PF_SETTINGS_INVALID_ARGUMENT, "number cannot be consumed by the strategy getter"};
    } catch (const std::out_of_range&) {
        throw Error{PF_SETTINGS_INVALID_ARGUMENT, "number cannot be consumed by the strategy getter"};
    }
    return parsed;
}

struct Setting {
    std::string name;
    std::string type;
    std::string default_value;
    std::vector<std::string> options{};
    double minimum = std::numeric_limits<double>::quiet_NaN();
    double maximum = std::numeric_limits<double>::quiet_NaN();
    double step = std::numeric_limits<double>::quiet_NaN();
    int integer_bits = 32;
    bool supported = true;
    std::vector<std::string> option_values{};
};

inline std::string validate(const Setting& setting, const std::string& value) {
    require(setting.supported, "input cannot be honoured by this compiled strategy", PF_SETTINGS_UNSUPPORTED);
    std::string canonical = value;
    double numeric = 0;
    bool is_numeric = false;
    if (setting.type == "bool") {
        require(value == "true" || value == "false" || value == "1" || value == "0", "invalid boolean");
        canonical = (value == "true" || value == "1") ? "true" : "false";
    } else if (setting.type == "int") {
        const auto parsed = integer(value, setting.integer_bits);
        canonical = number(parsed);
        numeric = static_cast<double>(parsed);
        is_numeric = true;
    } else if (setting.type == "float") {
        numeric = real(value);
        canonical = number(numeric);
        is_numeric = true;
    } else if (setting.type == "enum") {
        const auto found = std::find(setting.options.begin(), setting.options.end(), value);
        const auto index = found == setting.options.end() ? integer(value)
            : setting.option_values.empty() ? std::distance(setting.options.begin(), found)
            : integer(setting.option_values[static_cast<std::size_t>(std::distance(setting.options.begin(), found))]);
        require(setting.option_values.empty()
            ? index >= 0 && static_cast<std::size_t>(index) < setting.options.size()
            : std::find(setting.option_values.begin(), setting.option_values.end(), number(index)) != setting.option_values.end(), "invalid enum option");
        return number(static_cast<std::int64_t>(index));
    }
    if (is_numeric) {
        require(!std::isfinite(setting.minimum) || numeric >= setting.minimum, "value below minimum");
        require(!std::isfinite(setting.maximum) || numeric <= setting.maximum, "value above maximum");
    }
    if (!setting.options.empty()) {
        require(std::find(setting.options.begin(), setting.options.end(), canonical) != setting.options.end(), "invalid input option");
    }
    return canonical;
}

inline std::string quote(const std::string& value) {
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            result += '\\';
            result += static_cast<char>(byte);
        } else if (byte < 0x20) {
            result += "\\u00";
            result += hex[byte >> 4];
            result += hex[byte & 15];
        } else {
            result += static_cast<char>(byte);
        }
    }
    return result + '"';
}

inline std::string describe(const Setting& setting, const std::string& effective) {
    std::string result = "{\"name\":" + quote(setting.name)
        + ",\"type\":" + quote(setting.type) + ",\"default\":" + quote(setting.default_value)
        + ",\"effective_value\":" + quote(effective)
        + ",\"supported\":" + (setting.supported ? "true" : "false") + ",\"options\":[";
    for (std::size_t index = 0; index < setting.options.size(); ++index) {
        if (index) result += ',';
        result += quote(setting.options[index]);
    }
    result += ']';
    result += ",\"option_values\":[";
    for (std::size_t index = 0; index < setting.option_values.size(); ++index) {
        if (index) result += ',';
        result += quote(setting.option_values[index]);
    }
    result += ']';
    for (const auto& constraint : std::vector<std::pair<const char*, double>>{
             {"min", setting.minimum}, {"max", setting.maximum}, {"step", setting.step}}) {
        result += ',';
        result += quote(constraint.first) + ':';
        result += std::isfinite(constraint.second) ? number(constraint.second) : "null";
    }
    return result + '}';
}

inline void receipt(const std::string& document, char* json, std::size_t capacity,
                    std::size_t* required) {
    require(required != nullptr, "required byte count pointer is null");
    *required = document.size() + 1;
    if (!json || capacity < *required) {
        if (json && capacity) json[0] = '\0';
        throw Error{PF_SETTINGS_BUFFER_TOO_SMALL, "receipt buffer too small"};
    }
    std::memcpy(json, document.c_str(), *required);
}

}
