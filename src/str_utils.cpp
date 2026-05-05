#include <pineforge/str_utils.hpp>
#include "timezone.hpp"
#include <cmath>
#include <ctime>
#include <regex>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace pineforge {

// ---------- str.format ----------

std::string pine_str_format(const std::string& fmt,
                            const std::vector<std::string>& args) {
    std::string result = fmt;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string placeholder = "{" + std::to_string(i) + "}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), args[i]);
            pos += args[i].size();
        }
    }
    return result;
}

// ---------- str.format_time ----------

std::string pine_str_format_time(long long timestamp_ms,
                                 const std::string& format,
                                 const std::string& timezone) {
    time_t secs = static_cast<time_t>(timestamp_ms / 1000);
    struct tm tm_buf;
    if (!timezone.empty() && timezone != "UTC" && timezone != "Etc/UTC") {
        pine_tz::ScopedTimezone guard(timezone);
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

std::string pine_str_match(const std::string& source,
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

std::vector<std::string> pine_str_split(const std::string& source,
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

std::string pine_str_tostring(double value,
                              const std::string& format_mode,
                              double mintick) {
    if (std::isnan(value)) {
        return "NaN";
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
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (value * 100.0) << "%";
        return oss.str();
    }

    if (format_mode == "volume") {
        double abs_val = std::fabs(value);
        std::string sign = value < 0 ? "-" : "";
        std::ostringstream oss;
        if (abs_val >= 1e9) {
            oss << std::fixed << std::setprecision(2) << (abs_val / 1e9);
            return sign + oss.str() + "B";
        } else if (abs_val >= 1e6) {
            oss << std::fixed << std::setprecision(2) << (abs_val / 1e6);
            return sign + oss.str() + "M";
        } else if (abs_val >= 1e3) {
            oss << std::fixed << std::setprecision(2) << (abs_val / 1e3);
            return sign + oss.str() + "K";
        }
        oss << std::fixed << std::setprecision(2) << abs_val;
        return sign + oss.str();
    }

    // Default
    return std::to_string(value);
}

} // namespace pineforge
