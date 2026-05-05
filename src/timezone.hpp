#pragma once

#include <mutex>
#include <string>

namespace pineforge {

namespace pine_tz {

class ScopedTimezone {
public:
    explicit ScopedTimezone(const std::string& tz);
    ~ScopedTimezone();

    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;

private:
    std::unique_lock<std::mutex> lock_;
    std::string old_tz_;
    bool had_old_ = false;
};

}  // namespace pine_tz

} // namespace pineforge
