// R5 lane D2-A: the magnifier sampler as it stood at the lane's base
// (10f20197), and the bars its witnesses sample.
//
// The lane rewrites the sampler twice over -- it keeps no scratch between
// calls and is handed its leg order (tests/test_magnifier_ordered_sampler.cpp),
// and it takes four ENDPOINTS samples directly
// (tests/test_magnifier_endpoints4.cpp) -- and both witnesses hold it to this
// transcription, bit for bit. The arithmetic is the base routine's, statement
// for statement; its three thread_local scratch vectors are locals here (each
// was fully assigned before it was read), and the leg order it read through
// bar_path_uses_high_first under the path-order override is passed in.
//
// Source-free: kernel-only builds register the rows that use it.
#pragma once

#include "../src/engine_internal.hpp"
#include "native_match_book_fixture.hpp"

#include <pineforge/magnifier.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace magnifier_base {

using namespace pineforge;

struct Legs {
    double p0, p1, p2, p3;
    double len0, len1, len2;
    double total;
};

inline Legs legs(const Bar& bar, bool high_first) {
    Legs out;
    out.p0 = bar.open;
    out.p3 = bar.close;
    if (high_first) {
        out.p1 = bar.high;
        out.p2 = bar.low;
    } else {
        out.p1 = bar.low;
        out.p2 = bar.high;
    }
    out.len0 = std::fabs(out.p1 - out.p0);
    out.len1 = std::fabs(out.p2 - out.p1);
    out.len2 = std::fabs(out.p3 - out.p2);
    out.total = out.len0 + out.len1 + out.len2;
    return out;
}

inline void endpoints(std::vector<double>& t_values, int N, double len0, double len1,
                      double total) {
    if (total <= 0.0) {
        for (int i = 0; i < N; ++i)
            t_values[i] = static_cast<double>(i) / (N - 1);
        return;
    }
    double b0 = len0 / total;
    double b1 = (len0 + len1) / total;
    std::vector<double> mandatory;
    mandatory.assign({0.0, b0, b1, 1.0});
    std::sort(mandatory.begin(), mandatory.end());
    mandatory.erase(std::unique(mandatory.begin(), mandatory.end(),
        [](double a, double b) { return std::fabs(a - b) < internal::kPathTimeEps; }),
        mandatory.end());
    if (N == static_cast<int>(mandatory.size())) {
        t_values.resize(N);
        for (int i = 0; i < N; ++i)
            t_values[i] = mandatory[i];
        return;
    }
    if (N < static_cast<int>(mandatory.size())) {
        t_values.resize(N);
        t_values[0] = 0.0;
        t_values[N - 1] = 1.0;
        for (int i = 1; i < N - 1; ++i)
            t_values[i] = static_cast<double>(i) / (N - 1);
        return;
    }
    int remaining = N - static_cast<int>(mandatory.size());
    std::vector<double> all_t;
    all_t = mandatory;
    for (int i = 1; i <= remaining; ++i) {
        double t = static_cast<double>(i) / (remaining + 1);
        bool dup = false;
        for (double m : mandatory) {
            if (std::fabs(t - m) < internal::kPathTimeEps) { dup = true; break; }
        }
        if (!dup) all_t.push_back(t);
    }
    while (static_cast<int>(all_t.size()) < N) {
        double t = static_cast<double>(all_t.size()) / (N + 1);
        all_t.push_back(t);
    }
    std::sort(all_t.begin(), all_t.end());
    all_t.resize(N);
    t_values = all_t;
}

inline std::vector<double> sample(const Bar& bar, bool high_first, int n_samples,
                           MagnifierDistribution dist) {
    if (n_samples < 2) n_samples = 2;
    Legs l = legs(bar, high_first);
    std::vector<double> t_values;
    t_values.assign(n_samples, 0.0);
    int N = n_samples;
    switch (dist) {
    case MagnifierDistribution::UNIFORM:
        for (int i = 0; i < N; ++i) t_values[i] = static_cast<double>(i) / (N - 1);
        break;
    case MagnifierDistribution::COSINE:
        for (int i = 0; i < N; ++i) t_values[i] = 0.5 * (1.0 - std::cos(M_PI * i / (N - 1)));
        break;
    case MagnifierDistribution::TRIANGLE:
        for (int i = 0; i < N; ++i) {
            double u = static_cast<double>(i) / (N - 1);
            if (u <= 0.5)
                t_values[i] = 0.5 * std::sqrt(2.0 * u);
            else
                t_values[i] = 1.0 - 0.5 * std::sqrt(2.0 * (1.0 - u));
        }
        break;
    case MagnifierDistribution::ENDPOINTS:
        endpoints(t_values, N, l.len0, l.len1, l.total);
        break;
    case MagnifierDistribution::FRONT_LOADED:
        for (int i = 0; i < N; ++i) {
            double u = static_cast<double>(i) / (N - 1);
            t_values[i] = u * u;
        }
        break;
    case MagnifierDistribution::BACK_LOADED:
        for (int i = 0; i < N; ++i) {
            double u = static_cast<double>(i) / (N - 1);
            t_values[i] = 1.0 - (1.0 - u) * (1.0 - u);
        }
        break;
    }
    std::vector<double> out;
    out.resize(N);
    for (int i = 0; i < N; ++i)
        out[i] = path_at(t_values[i], l.p0, l.p1, l.p2, l.p3, l.len0, l.len1, l.len2, l.total);
    out[0] = l.p0;
    out[N - 1] = l.p3;
    return out;
}

inline int volume_count(const Bar& bar, int base_samples, double mean_volume, int min_samples,
                 int max_samples) {
    int n = base_samples;
    if (mean_volume > 0.0 && bar.volume > 0.0) {
        double ratio = bar.volume / mean_volume;
        if (ratio < 0.25) ratio = 0.25;
        if (ratio > 4.0) ratio = 4.0;
        n = static_cast<int>(std::round(base_samples * ratio));
    }
    if (n < min_samples) n = min_samples;
    if (n > max_samples) n = max_samples;
    return n;
}

// The thread-local override's verdict under a declared order: 1 HIGH_FIRST,
// 2 LOW_FIRST, and the open-proximity rule under AUTO (mode 0).
inline bool override_high_first(NativePathOrder order, const Bar& bar) {
    const int mode = order == NativePathOrder::HighFirst ? 1
        : (order == NativePathOrder::LowFirst ? 2 : 0);
    if (mode == 1) return true;
    if (mode == 2) return false;
    return std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
}

inline bool same_bits(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size()
        && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

// Whether the four turning times of `bar` in this order are four distinct
// ones: what the direct path requires, derived here independently.
inline bool four_distinct_times(const Bar& bar, bool high_first) {
    const Legs l = legs(bar, high_first);
    if (!(l.total > 0.0) || !std::isfinite(l.total)) return false;
    const double t[4] = {0.0, l.len0 / l.total, (l.len0 + l.len1) / l.total, 1.0};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(t[i] - t[i + 1]) < internal::kPathTimeEps) return false;
    }
    return true;
}

inline Bar make_bar(double open, double high, double low, double close, double volume = 1.0) {
    Bar bar{};
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.volume = volume;
    return bar;
}

inline std::vector<Bar> sampler_bars() {
    std::vector<Bar> bars;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double sub = std::numeric_limits<double>::denorm_min();
    // Every zero-length leg, ties and the degenerate bar, in both orders.
    bars.push_back(make_bar(100, 100, 100, 100));
    bars.push_back(make_bar(100, 100, 90, 95));   // O == H
    bars.push_back(make_bar(100, 110, 100, 105)); // O == L
    bars.push_back(make_bar(100, 110, 90, 90));   // C == L
    bars.push_back(make_bar(100, 110, 90, 110));  // C == H
    bars.push_back(make_bar(100, 110, 90, 100));  // a tie: |H-O| == |O-L|
    bars.push_back(make_bar(100, 100, 100, 101)); // H == L, C apart
    bars.push_back(make_bar(100, 110, 110, 105)); // H == L, O apart
    // Legs closer than the dedup tolerance relative to the path.
    bars.push_back(make_bar(100, 100 + 1e-11, 90, 95));
    bars.push_back(make_bar(1e6, 1e6 + 1e-7, 1e6 - 50, 1e6 - 10));
    bars.push_back(make_bar(100, 110, 90, 90 + 1e-12));
    // Inverted, negative, huge, tiny and subnormal.
    bars.push_back(make_bar(100, 90, 110, 95));
    bars.push_back(make_bar(-5, -1, -9, -3));
    bars.push_back(make_bar(1e300, 1.7e300, 3e299, 9e299));
    bars.push_back(make_bar(1e308, 1.79e308, -1.79e308, 0.0));
    bars.push_back(make_bar(1e-300, 3e-300, 1e-301, 2e-300));
    bars.push_back(make_bar(sub, 4 * sub, 0.0, 2 * sub));
    // Non-finite fields.
    for (int field = 0; field < 4; ++field) {
        for (const double bad : {nan, inf, -inf}) {
            double v[4] = {100, 110, 90, 105};
            v[field] = bad;
            bars.push_back(make_bar(v[0], v[1], v[2], v[3]));
        }
    }
    // Randomized: quarter ticks (with zero legs and ties by construction),
    // and random magnitudes and signs.
    k3_book::Rng rng(90001);
    for (int i = 0; i < 20000; ++i) {
        const long open = 400 + rng.between(-40, 40);
        const long close = open + rng.between(-8, 8);
        const long high = std::max(open, close) + rng.between(0, 4);
        const long low = std::min(open, close) - rng.between(0, 4);
        bars.push_back(make_bar(k3_book::ticks(open), k3_book::ticks(high), k3_book::ticks(low),
                                k3_book::ticks(close), 1.0 + rng.below(8)));
    }
    for (int i = 0; i < 20000; ++i) {
        const double scale = std::ldexp(1.0, rng.between(-60, 60));
        double v[4];
        for (double& x : v) {
            x = scale * (static_cast<double>(rng.next() >> 11) * 0x1.0p-53 - 0.25);
        }
        // Mostly OHLC-shaped, sometimes not.
        if (rng.percent(80)) {
            const double hi = std::max({v[0], v[1], v[2], v[3]});
            const double lo = std::min({v[0], v[1], v[2], v[3]});
            bars.push_back(make_bar(v[0], hi, lo, v[3], rng.between(0, 5)));
        } else {
            bars.push_back(make_bar(v[0], v[1], v[2], v[3], rng.between(0, 5)));
        }
    }
    return bars;
}

}  // namespace magnifier_base
