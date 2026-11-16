#pragma once
#include "tv_fixed_period_75_fact.hpp"
#include <pineforge/bar.hpp>
#include <vector>

namespace native_fixed_period_fixture {
using namespace pineforge;
using namespace tv_fixed_period_75_fact;

// Deliberately unrelated input prices: copying an input or holding a literal
// cannot satisfy the native substitution or reporter quantity assertions.
inline std::vector<Bar> input(int step, int end = 2880) {
    std::vector<Bar> bars;
    for (int minute = 0; minute < end; minute += step) {
        const double value = 100.25 + minute * 0.125;
        bars.push_back({value - 0.5, value + 1, value - 1, value, 3.0,
                       kOriginMs + minute * kMinuteMs});
    }
    return bars;
}
inline std::vector<Bar> native() {
    std::vector<Bar> bars;
    for (int i = 0; i < kBarCount; ++i) {
        const auto& row = kBars[i];
        // Only close/time are TV facts. O/H/L/V are synthetic payload sentinels.
        bars.push_back({row.value - 2, row.value + 4, row.value - 5, row.value,
                       1000.5 + i, kOriginMs + row.open_minute * kMinuteMs});
    }
    return bars;
}
} // namespace native_fixed_period_fixture
