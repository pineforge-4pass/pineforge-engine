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

}  // namespace

ScopedTimezone::ScopedTimezone(const std::string& tz)
    : lock_(timezone_mutex()) {
    const char* old = std::getenv("TZ");
    if (old != nullptr) {
        old_tz_ = old;
        had_old_ = true;
    }

    if (tz.empty() || tz == "UTC" || tz == "Etc/UTC") {
        ::setenv("TZ", "UTC", 1);
    } else {
        ::setenv("TZ", tz.c_str(), 1);
    }
    ::tzset();
}

ScopedTimezone::~ScopedTimezone() {
    if (had_old_) {
        ::setenv("TZ", old_tz_.c_str(), 1);
    } else {
        ::unsetenv("TZ");
    }
    ::tzset();
}

}  // namespace pine_tz

} // namespace pineforge
