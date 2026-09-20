#pragma once
#include <string>
#include <vector>

namespace pineforge {

std::string str_format(const std::string& fmt,
                       const std::vector<std::string>& args);

std::string str_format_time(long long timestamp_ms,
                            const std::string& format,
                            const std::string& timezone);

std::string str_match(const std::string& source,
                      const std::string& regex_pattern);

std::vector<std::string> str_split(const std::string& source,
                                   const std::string& separator);

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
