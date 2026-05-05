#pragma once
#include "na.hpp"
#include <cstdint>
#include <deque>

namespace pineforge {

inline double pine_random(double lo, uint32_t call_site, double hi, uint32_t seed, int bar_index) {
    // SplitMix64-style deterministic mixer. This is a PineForge PRNG contract:
    // stable across platforms/runs, intentionally not a TradingView PRNG clone.
    uint64_t x = static_cast<uint64_t>(seed);
    x ^= (static_cast<uint64_t>(call_site) + 0x9e3779b97f4a7c15ULL);
    x ^= (static_cast<uint64_t>(bar_index) + 1ULL) * 0xbf58476d1ce4e5b9ULL;
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x ^= (x >> 31);
    double u = static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0);
    return lo + u * (hi - lo);
}

namespace math {

/// Rolling-window sum for PineScript `math.sum(source, length)` (not a `ta.*` builtin).
class Sum {
    int length_;
    std::deque<double> buffer_;
    double sum_;

public:
    explicit Sum(int length);
    double compute(double src);
    double recompute(double src);
};

} // namespace math

} // namespace pineforge
