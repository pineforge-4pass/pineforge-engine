#pragma once

// Private R2 path geometry. Driver delivery and the consumer matcher include
// this header; it is not a public native API and does not own working-request
// state, fees, or settlement.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace pineforge {
inline namespace engine_script_run_v17 {
namespace native_matching {

struct GeometricHit {
    double t = 0.0;
    double price = 0.0;
    bool at_level = false;
};

inline std::uint64_t double_bits(double value) noexcept {
    static_assert(sizeof(double) == sizeof(std::uint64_t), "native binary64 width");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline bool finite_positive(double v) noexcept {
    return std::isfinite(v) && v > 0.0;
}

inline double price_at(double from, double to, double t) noexcept {
    if (t == 0.0) return from;
    if (t == 1.0) return to;
    return from + t * (to - from);
}

inline std::optional<double> t_for_price(double from, double to, double price) noexcept {
    const double denom = to - from;
    if (denom == 0.0) {
        if (price == from) return 0.0;
        return std::nullopt;
    }
    const double t = (price - from) / denom;
    if (!std::isfinite(t)) return std::nullopt;
    return t;
}

// --- Instrument price grid (NativeRunSpec::price_grid) ---------------------
// Generic tick-ladder arithmetic. Every entry point is an identity on a
// nonfinite price or a nonpositive tick, so a host that leaves the grid unset
// keeps the kernel's unquantized prices.
//
// The nanotick boundary guard on the directional forms mirrors the reference
// arithmetic in engine.hpp: a computed level that lands one ULP off an exact
// tick keeps that tick instead of jumping a whole one. Nearest rounding needs
// no guard, a nanotick being far inside the half-tick it would have to cross.
inline constexpr double kGridBoundaryTicks = 1e-9;

inline bool grid_active(double tick) noexcept {
    return std::isfinite(tick) && tick > 0.0;
}

inline double grid_index_up(double price, double tick) noexcept {
    return std::ceil(price / tick - kGridBoundaryTicks);
}

inline double grid_index_down(double price, double tick) noexcept {
    return std::floor(price / tick + kGridBoundaryTicks);
}

// Nearest tick, ties away from zero.
inline double grid_round_half_up(double price, double tick) noexcept {
    if (!std::isfinite(price) || !grid_active(tick)) return price;
    return std::round(price / tick) * tick;
}

// The tick on one named side of the price.
inline double grid_round_directional(double price, double tick, bool up) noexcept {
    if (!std::isfinite(price) || !grid_active(tick)) return price;
    return (up ? grid_index_up(price, tick) : grid_index_down(price, tick)) * tick;
}

// Quantized-path trigger test. A zero tick leaves every raw level as its own
// threshold, so the default matcher arithmetic is unchanged by construction.
struct GridThreshold {
    double tick = 0.0;
    bool half_up = true;
};

// The raw price at which the quantized path first enters the closed region
// first_region_entry tests. Half-up rounding opens the region at the
// enclosing half-tick; a directional path extends its own excursion to the
// enclosing tick, so the region opens a tick early with the boundary itself
// left outside by the same nanotick.
inline double grid_region_threshold(
        double level, bool le, const GridThreshold& grid) noexcept {
    if (!std::isfinite(level) || !grid_active(grid.tick)) return level;
    const double index = le ? grid_index_down(level, grid.tick)
                            : grid_index_up(level, grid.tick);
    const double shift = grid.half_up ? 0.5 : 1.0 - kGridBoundaryTicks;
    return (le ? index + shift : index - shift) * grid.tick;
}

// Region is price <= level when le is true, otherwise price >= level.
inline bool in_region(double price, double level, bool le) noexcept {
    return le ? price <= level : price >= level;
}

// First remaining-suffix entry into a closed price region. include_current
// reports t_start when the cursor is already inside; otherwise the first
// later crossing uses the threshold as the modeled raw price.
inline std::optional<GeometricHit> first_region_entry(
        double from, double to, const GeometricHit& start, double level, bool le,
        bool include_current, const GridThreshold& grid = {}) noexcept {
    const double t_start = start.t;
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(level)
        || !std::isfinite(t_start) || t_start < 0.0 || t_start > 1.0) {
        return std::nullopt;
    }
    // Only the region test moves onto the grid: the order's own level remains
    // the modeled price a crossing books.
    const double threshold = grid_region_threshold(level, le, grid);
    if (!std::isfinite(threshold)) return std::nullopt;
    // A reached threshold is the authoritative price of this cursor. Do not
    // reconstruct it from its rounded fraction on an activation/fill rescan.
    const double current = start.price;
    if (!std::isfinite(current)) return std::nullopt;
    if (include_current && in_region(current, threshold, le)) {
        return GeometricHit{t_start, current, false};
    }
    if (from == to) return std::nullopt;
    const auto t_cross = t_for_price(from, to, threshold);
    if (!t_cross) return std::nullopt;
    if (!(*t_cross > t_start) || *t_cross > 1.0) return std::nullopt;
    const double crossed = price_at(from, to, *t_cross);
    if (!in_region(crossed, threshold, le) && !in_region(threshold, threshold, le)) {
        return std::nullopt;
    }
    return GeometricHit{*t_cross, level, true};
}

inline double apply_slippage(double raw, double slip, bool buy) noexcept {
    return buy ? raw + slip : raw - slip;
}

inline double protect_limit(double slipped, double limit, bool buy) noexcept {
    return buy ? std::min(slipped, limit) : std::max(slipped, limit);
}

// Sell trail: stop = best - offset, must sit strictly below best.
// Buy trail: stop = best + offset, must sit strictly above best.
// Returns false on nonfinite/nonpositive offset or an absorbed/overflowed
// level. A finite nonpositive stop is still reported; callers do not clamp it.
inline bool checked_trail_stop(double best, double offset, bool buy, double* stop_out) noexcept {
    if (stop_out) *stop_out = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(best) || !std::isfinite(offset) || offset <= 0.0) return false;
    const double stop = buy ? best + offset : best - offset;
    if (!std::isfinite(stop)) return false;
    if (buy && !(stop > best)) return false;
    if (!buy && !(stop < best)) return false;
    if (stop_out) *stop_out = stop;
    return true;
}

inline bool trail_best_improves(double best, double price, bool buy) noexcept {
    if (!std::isfinite(best) || !std::isfinite(price)) return false;
    return buy ? price < best : price > best;
}

// Adverse remaining suffix against a frozen best. Favorable monotonic
// motion cannot hit the trailing stop on one linear segment.
inline std::optional<GeometricHit> trail_stop_hit(
        double from, double to, const GeometricHit& start, double best, double offset, bool buy,
        const GridThreshold& grid = {}) noexcept {
    const double t_start = start.t;
    double stop = 0.0;
    if (!checked_trail_stop(best, offset, buy, &stop)) return std::nullopt;
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(t_start)
        || t_start < 0.0 || t_start > 1.0) {
        return std::nullopt;
    }
    const bool le = !buy;
    return first_region_entry(from, to, start, stop, le, true, grid);
}

}  // namespace native_matching
}  // inline namespace engine_script_run_v17
}  // namespace pineforge
