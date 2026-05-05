#pragma once
#include <string>
#include <vector>

namespace pineforge {

std::string pine_str_format(const std::string& fmt,
                            const std::vector<std::string>& args);

std::string pine_str_format_time(long long timestamp_ms,
                                 const std::string& format,
                                 const std::string& timezone);

std::string pine_str_match(const std::string& source,
                           const std::string& regex_pattern);

std::vector<std::string> pine_str_split(const std::string& source,
                                        const std::string& separator);

std::string pine_str_tostring(double value,
                              const std::string& format_mode = "",
                              double mintick = 0.0);

} // namespace pineforge
