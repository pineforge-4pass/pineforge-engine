#pragma once

#include <cmath>
#include <cstddef>
#include <utility>

namespace pineforge::detail {

// Insert a source into an expansion of nonoverlapping doubles, smallest first
// (Shewchuk's expansion sum, as in math.fsum). Each window supplies its own
// storage for the partials. False means a nonfinite source or overflow.
template <class Partials>
bool expand_window_sum(double x, std::size_t& parts, const Partials& partial) {
    std::size_t kept = 0;
    for (std::size_t j = 0; j < parts; ++j) {
        double y = partial(j);
        if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
        const double high = x + y;
        const double low = y - (high - x);  // exact: |x| >= |y|
        if (low != 0.0) partial(kept++) = low;
        x = high;
    }
    partial(kept++) = x;
    parts = kept;
    return std::isfinite(x);
}

struct RoundedWindowSum {
    double sum;
    double remainder;
    bool fits_pair;
};

template <class Partials>
RoundedWindowSum round_window_sum(std::size_t rest, const Partials& partial) {
    double high = 0.0;
    double low = 0.0;
    if (rest > 0) {
        high = partial(--rest);
        while (rest > 0) {
            const double x = high;
            const double y = partial(--rest);
            high = x + y;
            low = y - (high - x);
            if (low != 0.0) break;
        }
        // Exactly halfway between two doubles, the partial sums still below
        // decide the direction (round half to even otherwise).
        if (rest > 0 && ((low < 0.0 && partial(rest - 1) < 0.0)
                         || (low > 0.0 && partial(rest - 1) > 0.0))) {
            const double twice = low * 2.0;
            const double up = high + twice;
            if (twice == up - high) high = up;
        }
    }
    return {high, low, rest == 0};
}

}  // namespace pineforge::detail
