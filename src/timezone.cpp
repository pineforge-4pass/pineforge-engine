#include "timezone.hpp"
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace pineforge {
namespace pine_tz {
namespace {

std::mutex& timezone_mutex() {
    static std::mutex mutex;
    return mutex;
}

// Last TZ written into the process environment. Guarded by timezone_mutex();
// lets a same-TZ scope skip the setenv/tzset syscall pair (lazy caching).
std::string g_active_tz = "NOT_SET";

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

    char tv_sign = tz[prefix];
    if (tv_sign != '+' && tv_sign != '-') {
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

    int hours = std::stoi(hour_s);
    int minutes = std::stoi(minute_s);
    if (hours > 23 || minutes > 59) {
        return tz;
    }
    if (hours == 0 && minutes == 0) {
        return "UTC";
    }

    char posix_sign = (tv_sign == '+') ? '-' : '+';
    std::ostringstream out;
    out << "UTC" << posix_sign << hours;
    if (minutes != 0) {
        out << ':' << (minutes < 10 ? "0" : "") << minutes;
    }
    return out.str();
}

// Acquire the process-global TZ mutex and keep it locked for the lifetime
// of this object. The caller's localtime_r / mktime decomposition runs
// inside the scope, so concurrent harness threads on different chart
// timezones cannot corrupt each other's wall time — this is the guarantee
// documented on strategy_set_chart_timezone in pineforge.h.
//
// The lazy-TZ cache is preserved: when the requested zone is already the
// active one we skip the setenv/tzset pair entirely, but the lock is held
// either way so the environment stays stable for the whole scope.
ScopedTimezone::ScopedTimezone(const std::string& tz)
    : lock_(timezone_mutex()) {

    std::string target = normalize_timezone_for_posix(tz);

    if (g_active_tz == target) {
        return;  // lock stays held until destruction
    }

    ::setenv("TZ", target.c_str(), 1);
    ::tzset();
    g_active_tz = target;
}

ScopedTimezone::~ScopedTimezone() {
    // lock_ releases the mutex here. TZ is intentionally NOT restored —
    // g_active_tz caches it so the next same-zone scope skips setenv/tzset.
}

}  // namespace pine_tz

std::string normalize_timezone_for_posix(const std::string& tz) {
    return pine_tz::normalize_timezone_for_posix(tz);
}

} // namespace pineforge
