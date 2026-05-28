#pragma once
#include <vector>
#include "bar.hpp"

namespace pineforge {

enum class MagnifierDistribution {
    UNIFORM,          // Equal spacing along path length
    COSINE,           // Dense at segment endpoints (Chebyshev-like)
    TRIANGLE,         // Dense at midpoints of each segment
    ENDPOINTS,        // Always include exact O,H,L,C + uniform fill between (default)
    FRONT_LOADED,     // Dense near O (simulates opening-impulse price action)
    BACK_LOADED,      // Dense near C (simulates closing-impulse price action)
};

/// Maps parameter t in [0,1] to a price on the 3-segment path.
/// Segments: p0->p1, p1->p2, p2->p3 with arc lengths len0, len1, len2.
double path_at(double t, double p0, double p1, double p2, double p3,
               double len0, double len1, double len2, double total);

/// Sample n_samples points along the OHLC price path of the bar.
/// The first leg follows TradingView-style open proximity: O -> H -> L -> C
/// when open is nearer high, otherwise O -> L -> H -> C (ties low-first).
/// First sample is always O, last is always C. Minimum 2 samples.
std::vector<double> sample_price_path(const Bar& bar, int n_samples,
                                       MagnifierDistribution dist = MagnifierDistribution::ENDPOINTS);

/// Out-parameter overload of sample_price_path. Writes the sampled prices into
/// `out` (cleared first; retained capacity is reused), avoiding a per-call heap
/// allocation in hot sampling loops. Behaviour is identical to the
/// value-returning overload.
void sample_price_path(const Bar& bar, int n_samples,
                       MagnifierDistribution dist, std::vector<double>& out);

/// Sample n_samples points along the OHLC price path, weighting the sample
/// density by the ratio of this bar's volume to a reference `mean_volume`. A
/// bar with 2x average volume receives up to 2x as many samples as a bar at
/// average volume, clamped within [min_samples, max_samples].
///
/// Intended for magnifier modes that should simulate denser tick streams when
/// volume is higher (more trades per bar → more fill opportunities).
std::vector<double> sample_price_path_volume_weighted(const Bar& bar,
                                                       int base_samples,
                                                       double mean_volume,
                                                       int min_samples = 2,
                                                       int max_samples = 64,
                                                       MagnifierDistribution dist = MagnifierDistribution::ENDPOINTS);

/// Out-parameter overload of sample_price_path_volume_weighted. Writes into
/// `out` (cleared first; retained capacity reused). Behaviour is identical to
/// the value-returning overload.
void sample_price_path_volume_weighted(const Bar& bar,
                                       int base_samples,
                                       double mean_volume,
                                       int min_samples,
                                       int max_samples,
                                       MagnifierDistribution dist,
                                       std::vector<double>& out);

} // namespace pineforge
