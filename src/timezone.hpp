#pragma once

#include <mutex>
#include <string>

namespace pineforge {

namespace pine_tz {

// RAII guard: swaps the process ``TZ`` environment to ``tz`` (lazily — a
// same-zone request skips the setenv/tzset pair) and holds a process-global
// mutex for the guard's ENTIRE lifetime, so the caller's localtime_r /
// mktime decomposition inside the scope is safe against concurrent threads
// running under a different timezone. Do not nest two ScopedTimezone
// scopes on the same thread — the mutex is non-recursive.
class ScopedTimezone {
public:
    explicit ScopedTimezone(const std::string& tz);
    ~ScopedTimezone();

    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;

private:
    std::unique_lock<std::mutex> lock_;
};

}  // namespace pine_tz

} // namespace pineforge
