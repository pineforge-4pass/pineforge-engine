#pragma once

#include <cstdint>

namespace pineforge::detail {

// A host captures the calling thread's setting at construction. While the
// host runs, its value is in force for any call sites it constructs.
std::int64_t ta_lookback_limit() noexcept;
std::int64_t exchange_ta_lookback_limit(std::int64_t maximum) noexcept;

class TaLookbackScope {
public:
    explicit TaLookbackScope(std::int64_t maximum) noexcept
        : previous_(exchange_ta_lookback_limit(maximum)) {}
    ~TaLookbackScope() { exchange_ta_lookback_limit(previous_); }
    TaLookbackScope(const TaLookbackScope&) = delete;
    TaLookbackScope& operator=(const TaLookbackScope&) = delete;

private:
    std::int64_t previous_;
};

} // namespace pineforge::detail
