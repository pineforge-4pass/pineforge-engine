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

std::string g_active_tz = "NOT_SET";

}  // namespace

ScopedTimezone::ScopedTimezone(const std::string& tz)
    : lock_(timezone_mutex(), std::defer_lock) {

    std::string target = (tz.empty() || tz == "UTC" || tz == "Etc/UTC") ? "UTC" : tz;

    lock_.lock();
    if (g_active_tz == target) {
        lock_.unlock();
        had_old_ = false;
        return;
    }

    const char* old = std::getenv("TZ");
    if (old != nullptr) {
        old_tz_ = old;
        had_old_ = true;
    } else {
        had_old_ = false;
    }

    ::setenv("TZ", target.c_str(), 1);
    ::tzset();
    g_active_tz = target;
    lock_.unlock();
}

ScopedTimezone::~ScopedTimezone() {
    // Destructor does nothing to allow lazy caching across calls!
}

}  // namespace pine_tz
} // namespace pineforge
