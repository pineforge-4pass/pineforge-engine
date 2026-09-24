#include "engine_internal.hpp"

#include <pineforge/magnifier.hpp>
#include <cmath>
#include <algorithm>

namespace pineforge {

using internal::kPathTimeEps;

// ─── path_at ─────────────────────────────────────────────────────────────────

double path_at(double t, double p0, double p1, double p2, double p3,
               double len0, double len1, double len2, double total) {
    if (total <= 0.0) return p0;  // degenerate: all points equal

    double d = t * total;  // distance along the path

    if (d <= len0) {
        // Segment 0: p0 -> p1
        double frac = (len0 > 0.0) ? d / len0 : 0.0;
        return p0 + frac * (p1 - p0);
    }
    d -= len0;
    if (d <= len1) {
        // Segment 1: p1 -> p2
        double frac = (len1 > 0.0) ? d / len1 : 0.0;
        return p1 + frac * (p2 - p1);
    }
    d -= len1;
    // Segment 2: p2 -> p3
    double frac = (len2 > 0.0) ? d / len2 : 0.0;
    return p2 + frac * (p3 - p2);
}

// ─── sample_price_path helpers (file-local) ──────────────────────────────────

namespace {

// Bundles the four OHLC waypoints + the three segment lengths + total path
// length so ``compute_ohlc_path_legs`` can return them as one struct, sparing
// ``sample_price_path`` from juggling eight independent locals.
struct OhlcPathLegs {
    double p0, p1, p2, p3;
    double len0, len1, len2;
    double total;
};

// Construct the 3-segment OHLC waypoint path in the leg order the caller
// resolved: O -> H -> L -> C when `high_first`, otherwise O -> L -> H -> C.
// The public sampler resolves it with TradingView-style first-leg selection
// (internal::bar_path_uses_high_first: high first when the open is closer to
// the high, ties low-first), which mirrors the order-fill path exactly so
// magnifier sampling lines up with intra-bar fill resolution; the native
// intrabar driver passes the order its run declares (NativeRunSpec::
// path_order), so a forced order steers the sampling too.
OhlcPathLegs compute_ohlc_path_legs(const Bar& bar, bool high_first) {
    OhlcPathLegs legs;
    legs.p0 = bar.open;
    legs.p3 = bar.close;

    if (high_first) {
        // Open nearer high: O -> H -> L -> C
        legs.p1 = bar.high;
        legs.p2 = bar.low;
    } else {
        // Open nearer low (or tied): O -> L -> H -> C
        legs.p1 = bar.low;
        legs.p2 = bar.high;
    }

    legs.len0 = std::fabs(legs.p1 - legs.p0);
    legs.len1 = std::fabs(legs.p2 - legs.p1);
    legs.len2 = std::fabs(legs.p3 - legs.p2);
    legs.total = legs.len0 + legs.len1 + legs.len2;
    return legs;
}

// ENDPOINTS distribution: always include the exact turning points — t=0 (O),
// boundary0 = len0/total, boundary1 = (len0+len1)/total, t=1 (C) — and fill
// any remaining slots with uniform t-values, dedup'd against the mandatory
// set. Falls back to plain uniform when the bar is degenerate (total==0).
// `t_values` holds N values on entry and all N on return; the mandatory set
// (at most four) lives on the stack and the uniform fill is written straight
// into `t_values`, so no scratch outlives the call (R5 lane D2-A: these were
// two thread_local vectors, a TLS-descriptor call per use in a dlopen'd
// strategy). The arithmetic, the dedup and both sorts are the ones they fed.
void fill_endpoints_t_values(std::vector<double>& t_values, int N,
                              double len0, double len1, double total) {
    if (total <= 0.0) {
        // Degenerate: all same price
        for (int i = 0; i < N; ++i)
            t_values[i] = static_cast<double>(i) / (N - 1);
        return;
    }
    double b0 = len0 / total;
    double b1 = (len0 + len1) / total;

    // Collect mandatory points
    double mandatory[4] = {0.0, b0, b1, 1.0};
    // Remove duplicates (e.g. if a segment has zero length)
    std::sort(mandatory, mandatory + 4);
    const int kept = static_cast<int>(std::unique(mandatory, mandatory + 4,
        [](double a, double b) { return std::fabs(a - b) < kPathTimeEps; }) - mandatory);

    if (N == kept) {
        // Exact match: use all mandatory points
        for (int i = 0; i < N; ++i)
            t_values[i] = mandatory[i];
        return;
    }
    if (N < kept) {
        // Fewer samples than mandatory points — use evenly spaced
        t_values[0] = 0.0;
        t_values[N - 1] = 1.0;
        for (int i = 1; i < N - 1; ++i)
            t_values[i] = static_cast<double>(i) / (N - 1);
        return;
    }
    // Start with mandatory, fill remaining with uniform
    int remaining = N - kept;
    int filled = 0;
    for (; filled < kept; ++filled)
        t_values[filled] = mandatory[filled];
    for (int i = 1; i <= remaining; ++i) {
        double t = static_cast<double>(i) / (remaining + 1);
        // Avoid duplicating mandatory points
        bool dup = false;
        for (int m = 0; m < kept; ++m) {
            if (std::fabs(t - mandatory[m]) < kPathTimeEps) { dup = true; break; }
        }
        if (!dup) t_values[filled++] = t;
    }
    // If we still need more (because some uniform pts coincided
    // with mandatory), fill with finer uniform
    while (filled < N) {
        double t = static_cast<double>(filled) / (N + 1);
        t_values[filled++] = t;
    }
    std::sort(t_values.begin(), t_values.end());
}

}  // namespace

// ─── sample_price_path ───────────────────────────────────────────────────────

std::vector<double> sample_price_path(const Bar& bar, int n_samples,
                                       MagnifierDistribution dist) {
    std::vector<double> result;
    sample_price_path(bar, n_samples, dist, result);
    return result;
}

void sample_price_path(const Bar& bar, int n_samples,
                       MagnifierDistribution dist, std::vector<double>& out) {
    internal::sample_price_path_ordered(bar, internal::bar_path_uses_high_first(bar),
                                        n_samples, dist, out);
}

void internal::sample_price_path_ordered(const Bar& bar, bool high_first, int n_samples,
                                         MagnifierDistribution dist, std::vector<double>& out) {
    if (n_samples < 2) n_samples = 2;

    OhlcPathLegs legs = compute_ohlc_path_legs(bar, high_first);

    // Generate t-values based on distribution, into the caller's buffer
    // (assign keeps its capacity); they are mapped to prices in place below.
    int N = n_samples;
    out.assign(N, 0.0);
    std::vector<double>& t_values = out;

    switch (dist) {
    case MagnifierDistribution::UNIFORM:
        for (int i = 0; i < N; ++i)
            t_values[i] = static_cast<double>(i) / (N - 1);
        break;

    case MagnifierDistribution::COSINE:
        for (int i = 0; i < N; ++i)
            t_values[i] = 0.5 * (1.0 - std::cos(M_PI * i / (N - 1)));
        break;

    case MagnifierDistribution::TRIANGLE: {
        // Triangle CDF: denser at midpoints of each segment
        // Use a triangle distribution centered at t=0.5
        for (int i = 0; i < N; ++i) {
            double u = static_cast<double>(i) / (N - 1);  // uniform in [0,1]
            // Triangle CDF inverse: maps uniform to triangle-distributed
            if (u <= 0.5)
                t_values[i] = 0.5 * std::sqrt(2.0 * u);
            else
                t_values[i] = 1.0 - 0.5 * std::sqrt(2.0 * (1.0 - u));
        }
        break;
    }

    case MagnifierDistribution::ENDPOINTS:
        fill_endpoints_t_values(t_values, N, legs.len0, legs.len1, legs.total);
        break;

    case MagnifierDistribution::FRONT_LOADED: {
        // Quadratic easing toward 0 so sample density is highest near bar.open.
        // Useful when the user wants to emphasize opening-impulse price action
        // (e.g. breakout markets where most volume concentrates in the first
        // few ticks of the bar).
        for (int i = 0; i < N; ++i) {
            double u = static_cast<double>(i) / (N - 1);
            t_values[i] = u * u;
        }
        break;
    }

    case MagnifierDistribution::BACK_LOADED: {
        // Inverse of FRONT_LOADED — density highest near bar.close. Useful
        // for strategies expecting most intra-bar movement in the last part
        // of the bar (e.g. session-close runs).
        for (int i = 0; i < N; ++i) {
            double u = static_cast<double>(i) / (N - 1);
            t_values[i] = 1.0 - (1.0 - u) * (1.0 - u);
        }
        break;
    }
    }

    // Map t-values to prices, each in its own slot.
    for (int i = 0; i < N; ++i)
        out[i] = path_at(t_values[i], legs.p0, legs.p1, legs.p2, legs.p3,
                         legs.len0, legs.len1, legs.len2, legs.total);

    // Guarantee exact endpoints
    out[0] = legs.p0;
    out[N - 1] = legs.p3;
}

// ─── sample_price_path_volume_weighted ───────────────────────────────────────

static int volume_weighted_sample_count(const Bar& bar, int base_samples,
                                        double mean_volume, int min_samples,
                                        int max_samples) {
    int n = base_samples;
    // Only scale when we have a meaningful volume reference.
    if (mean_volume > 0.0 && bar.volume > 0.0) {
        double ratio = bar.volume / mean_volume;
        // Clamp the scaling factor to avoid wild swings on thin/bursty bars.
        if (ratio < 0.25) ratio = 0.25;
        if (ratio > 4.0) ratio = 4.0;
        n = static_cast<int>(std::round(base_samples * ratio));
    }
    if (n < min_samples) n = min_samples;
    if (n > max_samples) n = max_samples;
    return n;
}

std::vector<double> sample_price_path_volume_weighted(const Bar& bar,
                                                       int base_samples,
                                                       double mean_volume,
                                                       int min_samples,
                                                       int max_samples,
                                                       MagnifierDistribution dist) {
    int n = volume_weighted_sample_count(bar, base_samples, mean_volume,
                                         min_samples, max_samples);
    return sample_price_path(bar, n, dist);
}

void sample_price_path_volume_weighted(const Bar& bar,
                                       int base_samples,
                                       double mean_volume,
                                       int min_samples,
                                       int max_samples,
                                       MagnifierDistribution dist,
                                       std::vector<double>& out) {
    int n = volume_weighted_sample_count(bar, base_samples, mean_volume,
                                         min_samples, max_samples);
    sample_price_path(bar, n, dist, out);
}

void internal::sample_price_path_volume_weighted_ordered(
        const Bar& bar, bool high_first, int base_samples, double mean_volume,
        int min_samples, int max_samples, MagnifierDistribution dist,
        std::vector<double>& out) {
    int n = volume_weighted_sample_count(bar, base_samples, mean_volume,
                                         min_samples, max_samples);
    sample_price_path_ordered(bar, high_first, n, dist, out);
}

} // namespace pineforge
