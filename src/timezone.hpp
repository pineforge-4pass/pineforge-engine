#pragma once

#include <mutex>
#include <string>

namespace pineforge {

namespace tz_util {

std::string normalize_timezone_for_posix(const std::string& tz);

// RAII guard: swaps the process ``TZ`` environment to the normalized ``tz``
// unless getenv("TZ") already matches, and holds this engine image's mutex
// for the guard's ENTIRE lifetime, including localtime_r / mktime calls.
// Independently linked engine images have separate mutexes; cross-image
// parallel atomicity remains open. TZ is intentionally not restored.
// Do not nest two ScopedTimezone scopes on the same thread — the mutex
// is non-recursive.
class ScopedTimezone {
public:
    explicit ScopedTimezone(const std::string& tz);
    ~ScopedTimezone();

    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;

private:
    std::unique_lock<std::mutex> lock_;
};

}  // namespace tz_util

} // namespace pineforge
