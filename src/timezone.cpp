#include "timezone.hpp"
#include <pineforge/run_failure.hpp>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <stdexcept>

namespace pineforge {
namespace tz_util {
namespace {

std::mutex& timezone_mutex() {
    static std::mutex mutex;
    return mutex;
}

// std::stoi of a UTC offset's hours or minutes. A timezone the script builds
// at run time can fail it (too many digits): the same exception type and text
// as ever, coded pine_invalid_argument. Valid input takes exactly the path it
// always took.
int timezone_offset_part(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (const std::invalid_argument& error) {
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_invalid_argument,
            {{"argument", "timezone"}, {"rule", "unparseable"}}, error.what());
    } catch (const std::out_of_range& error) {
        throw coded<std::out_of_range>(
            RunFailureCode::pine_invalid_argument,
            {{"argument", "timezone"}, {"rule", "unparseable"}}, error.what());
    }
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char ch : s) {
        if (!std::isdigit(ch)) return false;
    }
    return true;
}

}  // namespace

std::string normalize_timezone_for_posix(const std::string& tz) {
    if (tz.empty() || tz == "UTC" || tz == "Etc/UTC" || tz == "GMT" ||
        tz == "Etc/GMT") {
        return "UTC";
    }

    std::size_t prefix = std::string::npos;
    if (tz.rfind("GMT", 0) == 0) {
        prefix = 3;
    } else if (tz.rfind("UTC", 0) == 0) {
        prefix = 3;
    }
    if (prefix == std::string::npos || prefix >= tz.size()) {
        return tz;
    }

    char offset_sign = tz[prefix];
    if (offset_sign != '+' && offset_sign != '-') {
        return tz;
    }

    std::string body = tz.substr(prefix + 1);
    std::string hour_s;
    std::string minute_s;
    std::size_t colon = body.find(':');
    if (colon != std::string::npos) {
        hour_s = body.substr(0, colon);
        minute_s = body.substr(colon + 1);
    } else if (body.size() > 2) {
        hour_s = body.substr(0, body.size() - 2);
        minute_s = body.substr(body.size() - 2);
    } else {
        hour_s = body;
        minute_s = "0";
    }

    if (!all_digits(hour_s) || !all_digits(minute_s)) {
        return tz;
    }

    int hours = timezone_offset_part(hour_s);
    int minutes = timezone_offset_part(minute_s);
    if (hours > 23 || minutes > 59) {
        return tz;
    }
    if (hours == 0 && minutes == 0) {
        return "UTC";
    }

    char posix_sign = (offset_sign == '+') ? '-' : '+';
    std::ostringstream out;
    out << "UTC" << posix_sign << hours;
    if (minutes != 0) {
        out << ':' << (minutes < 10 ? "0" : "") << minutes;
    }
    return out.str();
}

// Hold this engine image's TZ mutex for the lifetime of the guard, including
// the caller's localtime_r / mktime decomposition. Independently linked engine
// images have separate mutexes; cross-image parallel atomicity remains open.
//
// Compare with the actual process environment so a preceding TZ switch by
// another engine image cannot leave an image-local last-zone cache stale.
// Skip setenv/tzset only when TZ already equals the normalized target.
ScopedTimezone::ScopedTimezone(const std::string& tz)
    : lock_(timezone_mutex()) {

    std::string target = normalize_timezone_for_posix(tz);

    const char* active_tz = std::getenv("TZ");
    if (active_tz != nullptr && target == active_tz) {
        return;  // lock stays held until destruction
    }

    ::setenv("TZ", target.c_str(), 1);
    ::tzset();
}

ScopedTimezone::~ScopedTimezone() {
    // lock_ releases the mutex here. TZ is intentionally NOT restored —
    // the next scope checks the process environment before skipping setenv/tzset.
}

}  // namespace tz_util

std::string normalize_timezone_for_posix(const std::string& tz) {
    return tz_util::normalize_timezone_for_posix(tz);
}

} // namespace pineforge
