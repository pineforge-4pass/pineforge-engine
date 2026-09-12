#pragma once

// Private R2 path geometry. Driver delivery and the consumer matcher include
// this header; it is not a public native API and does not own working-request
// state, fees, or settlement.

#include <cmath>
#include <limits>
#include <optional>

namespace pineforge {
inline namespace engine_script_run_v13 {
namespace native_matching {

struct GeometricHit {
    double t = 0.0;
    double price = 0.0;
};

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

// Region is price <= level when le is true, otherwise price >= level.
inline bool in_region(double price, double level, bool le) noexcept {
    return le ? price <= level : price >= level;
}

// First remaining-suffix entry into a closed price region. include_current
// reports t_start when the cursor is already inside; otherwise the first
// later crossing uses the threshold as the modeled raw price.
inline std::optional<GeometricHit> first_region_entry(
        double from, double to, const GeometricHit& start, double level, bool le,
        bool include_current) noexcept {
    const double t_start = start.t;
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(level)
        || !std::isfinite(t_start) || t_start < 0.0 || t_start > 1.0) {
        return std::nullopt;
    }
    // A reached threshold is the authoritative price of this cursor. Do not
    // reconstruct it from its rounded fraction on an activation/fill rescan.
    const double current = start.price;
    if (!std::isfinite(current)) return std::nullopt;
    if (include_current && in_region(current, level, le)) {
        return GeometricHit{t_start, current};
    }
    if (from == to) return std::nullopt;
    const auto t_cross = t_for_price(from, to, level);
    if (!t_cross) return std::nullopt;
    if (!(*t_cross > t_start) || *t_cross > 1.0) return std::nullopt;
    const double crossed = price_at(from, to, *t_cross);
    if (!in_region(crossed, level, le) && !in_region(level, level, le)) {
        return std::nullopt;
    }
    return GeometricHit{*t_cross, level};
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
        double from, double to, const GeometricHit& start, double best, double offset, bool buy) noexcept {
    const double t_start = start.t;
    double stop = 0.0;
    if (!checked_trail_stop(best, offset, buy, &stop)) return std::nullopt;
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(t_start)
        || t_start < 0.0 || t_start > 1.0) {
        return std::nullopt;
    }
    const bool le = !buy;
    return first_region_entry(from, to, start, stop, le, true);
}

}  // namespace native_matching
}  // inline namespace engine_script_run_v13
}  // namespace pineforge
