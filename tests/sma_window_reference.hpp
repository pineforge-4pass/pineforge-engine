#pragma once

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sma_window_reference {

static_assert(std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::radix == 2 &&
              std::numeric_limits<double>::digits == 53,
              "The reference requires IEEE binary64");

// This is a test-only floating expansion, independent of a running window
// accumulator. FastTwoSum retains each addition's error in a smaller partial;
// no source information is discarded before the final rounding.
//
// Preconditions: round-to-nearest/even, gradual underflow, finite inputs and
// no intermediate overflow. The bounded test inputs satisfy the range
// restriction. Volatile stores force the elementary operations to binary64,
// including on implementations that otherwise keep extended intermediates.
inline double add(double x, double y) {
    volatile double result = x + y;
    return result;
}

inline double subtract(double x, double y) {
    volatile double result = x - y;
    return result;
}

inline double rounded_sum(const std::vector<double>& values) {
    std::vector<double> partials;
    for (double x : values) {
        if (!std::isfinite(x)) {
            throw std::invalid_argument("nonfinite reference input");
        }
        std::size_t kept = 0;
        for (double y : partials) {
            if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
            const double high = add(x, y);
            if (!std::isfinite(high)) {
                throw std::overflow_error("reference expansion overflow");
            }
            const double low = subtract(y, subtract(high, x));
            if (low != 0.0) partials[kept++] = low;
            x = high;
        }
        partials.resize(kept);
        if (x != 0.0) partials.push_back(x);
    }

    if (partials.empty()) return 0.0;
    std::size_t remaining = partials.size();
    double high = partials[--remaining];
    double low = 0.0;
    while (remaining != 0) {
        const double before = high;
        const double next = partials[--remaining];
        high = add(before, next);
        low = subtract(next, subtract(high, before));
        if (low != 0.0) break;
    }

    // At a halfway rounding, a still smaller partial with the same sign as
    // the residual puts the exact sum beyond the midpoint. Doubling the
    // residual changes high only when it lands exactly on the adjacent
    // binary64 value; otherwise high already has the correctly rounded bits.
    if (remaining != 0 &&
        ((low < 0.0 && partials[remaining - 1] < 0.0) ||
         (low > 0.0 && partials[remaining - 1] > 0.0))) {
        const double twice_low = add(low, low);
        const double adjacent = add(high, twice_low);
        if (subtract(adjacent, high) == twice_low) high = adjacent;
    }
    return high;
}

inline double mean(const std::vector<double>& values) {
    if (values.empty()) throw std::invalid_argument("empty reference window");
    // Round the sum first, then perform ordinary division. Multiplication
    // by a pre-rounded reciprocal has a different binary64 result in general.
    volatile double result = rounded_sum(values) /
                             static_cast<double>(values.size());
    return result;
}

} // namespace sma_window_reference
