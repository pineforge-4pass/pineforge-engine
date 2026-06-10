#include "timezone.hpp"
#include <cstdlib>
#include <ctime>

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

}  // namespace

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

    std::string target = (tz.empty() || tz == "UTC" || tz == "Etc/UTC") ? "UTC" : tz;

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
} // namespace pineforge
