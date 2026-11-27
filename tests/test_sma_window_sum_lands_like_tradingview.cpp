// Regression pin: the exact window sum lands where TradingView's does.
//
// On NYSE:F 15m at 2025-05-16 14:15 UTC, the 20 closes below sum to 214.8 in
// decimal, and the bar's close is 10.74. TradingView's ta.sma(close, 20) on
// that bar (checked 2026-11-27, issue #1017) puts the average ONE binary64
// step under 10.74, the same value the engine computes. So this pins the binary64 results, not the decimal ones: a change
// to the summation (order, compensation, contraction) that moved them would
// move the engine away from TradingView on ties like this one.
#include <pineforge/math.hpp>
#include <pineforge/ta.hpp>

#include <array>
#include <cmath>
#include <cstdio>

namespace {
constexpr int length = 20;
// NYSE:F 15m, nyse-f-15m.csv:26292-26313, close column in feed order.
// The first 20 end at 1747403100000; the last 20 end at 1747404900000.
constexpr std::array closes{
    10.75,  10.715, 10.735, 10.735, 10.735, 10.75, 10.735, 10.705,
    10.715, 10.735, 10.735, 10.745, 10.755, 10.745, 10.76, 10.77,
    10.77,  10.74,  10.735, 10.725, 10.735, 10.74,
};
static_assert(closes.size() == length + 2);

bool check_equal(const char* row, double actual, double expected) {
    if (actual == expected) {
        std::printf("PASS %s: %.17g\n", row, actual);
        return true;
    }
    std::printf("FAIL %s: got %.17g expected %.17g\n", row, actual, expected);
    return false;
}
}  // namespace

int main() {
    pineforge::ta::SMA control(length);
    double mean = 0.0;
    for (int i = 0; i < length; ++i) mean = control.compute(closes[i]);
    // 13:45: no tie; the engine and TradingView both read the decimal mean.
    if (!check_equal("13:45 SMA", mean, 10.7395)) return 1;

    pineforge::ta::SMA sma(length);
    pineforge::math::Sum sum(length);
    double total = 0.0;
    for (int i = 0; i < length; ++i) {
        mean = sma.compute(closes[i + 2]);
        total = sum.compute(closes[i + 2]);
    }
    // 14:15: one binary64 step under the decimal values, as TradingView measured.
    const bool mean_ok = check_equal("14:15 SMA", mean, std::nextafter(10.74, 0.0));
    const bool sum_ok = check_equal("14:15 Sum", total, std::nextafter(214.8, 0.0));
    return mean_ok && sum_ok ? 0 : 1;
}
