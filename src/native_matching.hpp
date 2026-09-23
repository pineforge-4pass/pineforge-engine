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
inline namespace engine_script_run_v19 {
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
//
// Exactness (R7 ruling): a ladder price is a fixed point of the grid. A level
// bit-identical to either binary64 spelling of its ladder point — k * tick,
// or k / n for a decimal tick 1 / n, which is what a decimal literal parses
// to — maps to index k without consulting the quotient's last bit (the guard
// alone misindexes ladder prices from k ~ 1.7e7 on), and the directional
// forms hand such a price back unchanged instead of re-spelling it as k *
// tick one ULP away. Off the ladder the guard decides as before.
inline constexpr double kGridBoundaryTicks = 1e-9;

inline bool grid_active(double tick) noexcept {
    return std::isfinite(tick) && tick > 0.0;
}

// The ladder index of a price that IS a ladder point, NaN otherwise. Both
// spellings of the point are recognized; the decimal one only when 1 / tick
// is an integer to within the same 1e-6 band the reference arithmetic uses.
inline double grid_exact_index(double price, double tick) noexcept {
    if (!std::isfinite(price) || !grid_active(tick)) return std::numeric_limits<double>::quiet_NaN();
    const double k = std::round(price / tick);
    if (k * tick == price) return k;
    const double inverse = 1.0 / tick;
    const double n = std::round(inverse);
    if (n > 0.0 && std::abs(inverse - n) <= 1e-6 * n && k / n == price) return k;
    return std::numeric_limits<double>::quiet_NaN();
}

inline double grid_index_up(double price, double tick) noexcept {
    const double exact = grid_exact_index(price, tick);
    if (!std::isnan(exact)) return exact;
    return std::ceil(price / tick - kGridBoundaryTicks);
}

inline double grid_index_down(double price, double tick) noexcept {
    const double exact = grid_exact_index(price, tick);
    if (!std::isnan(exact)) return exact;
    return std::floor(price / tick + kGridBoundaryTicks);
}

// A ladder point, spelled so no binary64 ULP hides it from the side it is
// reached from. A point has two spellings the exactness rule above already
// reads as one index -- k * tick, and k / n for a decimal tick 1 / n, which
// is what a price literal parses to -- and they can differ by one ULP. A
// level the kernel puts ON the ladder names the outer of the two on the
// reachable side (the higher for a `price <= level` region, the lower for a
// `price >= level` one), so a print that IS the point lies inside the region
// under either spelling.
inline double grid_ladder_price(double index, double tick, bool le) noexcept {
    const double product = index * tick;
    if (!grid_active(tick) || !std::isfinite(product)) return product;
    const double inverse = 1.0 / tick;
    const double n = std::round(inverse);
    if (!(n > 0.0) || std::abs(inverse - n) > 1e-6 * n) return product;
    const double quotient = index / n;
    if (!std::isfinite(quotient)) return product;
    return le ? std::max(product, quotient) : std::min(product, quotient);
}

// Nearest tick, ties away from zero. Spelled as the product on purpose: the
// sizing rule SizePrice::SignalOnTick pins this exact form.
inline double grid_round_half_up(double price, double tick) noexcept {
    if (!std::isfinite(price) || !grid_active(tick)) return price;
    return std::round(price / tick) * tick;
}

// The tick on one named side of the price; a ladder price is its own tick.
inline double grid_round_directional(double price, double tick, bool up) noexcept {
    if (!std::isfinite(price) || !grid_active(tick)) return price;
    if (!std::isnan(grid_exact_index(price, tick))) return price;
    return (up ? grid_index_up(price, tick) : grid_index_down(price, tick)) * tick;
}

// Quantized-path trigger test. A zero tick leaves every raw level as its own
// threshold, so the default matcher arithmetic is unchanged by construction.
struct GridThreshold {
    double tick = 0.0;
    bool half_up = true;
};

// The outermost price whose half-up ladder index is still inside the closed
// region (index <= for le, >= otherwise): the exact binary64 boundary of
// grid_round_half_up, not the product (index +/- 0.5) * tick. The product is
// not that boundary — a tie print rounds away from zero and so lies OUTSIDE
// a `<=` region, and a decimal half tick lands one or two ULPs to either side
// of the product — so the nominal half tick is walked to the last price the
// rounding itself keeps inside. Bounded: a half tick is never more than a few
// ULPs from its nominal spelling.
inline double grid_half_up_boundary(double index, bool le, double tick) noexcept {
    double boundary = (le ? index + 0.5 : index - 0.5) * tick;
    if (!std::isfinite(boundary)) return boundary;
    const auto inside = [&](double price) noexcept {
        const double k = std::round(price / tick);
        return le ? k <= index : k >= index;
    };
    const double inf = std::numeric_limits<double>::infinity();
    const double inward = le ? -inf : inf;    // toward the level
    const double outward = le ? inf : -inf;   // away from it
    for (int i = 0; i < 16 && !inside(boundary); ++i) boundary = std::nextafter(boundary, inward);
    for (int i = 0; i < 16; ++i) {
        const double candidate = std::nextafter(boundary, outward);
        if (!inside(candidate)) break;
        boundary = candidate;
    }
    return boundary;
}

// The raw price at which the quantized path first enters the closed region
// first_region_entry tests. Half-up rounding opens the region at the exact
// boundary of its own rounding (grid_half_up_boundary); a directional path
// extends its own excursion to the enclosing tick, so the region opens a tick
// early with the boundary itself left outside by the same nanotick.
inline double grid_region_threshold(
        double level, bool le, const GridThreshold& grid) noexcept {
    if (!std::isfinite(level) || !grid_active(grid.tick)) return level;
    const double index = le ? grid_index_down(level, grid.tick)
                            : grid_index_up(level, grid.tick);
    if (grid.half_up) return grid_half_up_boundary(index, le, grid.tick);
    const double shift = 1.0 - kGridBoundaryTicks;
    return (le ? index + shift : index - shift) * grid.tick;
}

// Region is price <= level when le is true, otherwise price >= level.
inline bool in_region(double price, double level, bool le) noexcept {
    return le ? price <= level : price >= level;
}

// --- The activation rule (L8b ruling) ---------------------------------------
// Under QuantizeFillsAndTriggers the tick-quantized print IS the reached
// price: the matcher's verdict is authoritative, and the core re-validates an
// activation (stop, stop-limit, trail arm, trail stop) with these same two
// functions on the same print, so a hit the matcher reports is never refused.
// Without a grid both are the raw compare and the raw print, bit for bit.
// Reached: the level itself always is (a crossing hit books the level, and a
// quantized path enters the region exactly there); any other print must lie
// inside the region the matcher tests, i.e. on the level's side of the same
// grid_region_threshold. A nonfinite print or level is never reached.
inline bool region_reached(double reached, double level, bool le,
                           const GridThreshold& grid = {}) noexcept {
    if (!std::isfinite(reached) || !std::isfinite(level)) return false;
    if (reached == level) return true;
    return in_region(reached, grid_region_threshold(level, le, grid), le);
}

// The quantized print of a reached activation. On a crossing (the print is
// the level) it is the level's own ladder point: the level when it is one,
// else the enclosing tick on the region's side, which is where a quantized
// path first stands inside the region under either rounding. Otherwise it is
// the print's tick: the nearest under HalfUp, the enclosing tick on the
// region's side under Directional. Identity without a grid.
inline double grid_reached_print(double reached, double level, bool le,
                                 const GridThreshold& grid = {}) noexcept {
    if (!std::isfinite(reached) || !grid_active(grid.tick)) return reached;
    if (reached == level) return grid_round_directional(level, grid.tick, !le);
    if (grid.half_up) return grid_round_half_up(reached, grid.tick);
    return grid_round_directional(reached, grid.tick, !le);
}

// A trail's running best on the quantized path: a sell trail's best is a high
// and extends upward, a buy trail's best is a low and extends downward, so
// the Directional tick is on the favourable side. Identity without a grid.
inline double grid_best_print(double price, bool buy, const GridThreshold& grid = {}) noexcept {
    if (!std::isfinite(price) || !grid_active(grid.tick)) return price;
    if (grid.half_up) return grid_round_half_up(price, grid.tick);
    return grid_round_directional(price, grid.tick, !buy);
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

// A trailing stop that stands a whole number of ticks from a best that is
// itself a ladder point IS the ladder point that many ticks away (R5 lane
// E16). The subtraction cannot name it: 11.44 - 5 * 0.01 is
// 11.389999999999998792, one binary64 ULP under 11.390000000000000568, so a
// print that IS 11.39 never reaches a stop the run put five ticks under
// 11.44. Both ends are decimal numbers the host measured on its own ladder,
// and the ladder index is the only exact arithmetic between them.
//
// It is the LEVEL that moves, not the comparison, and only where the ladder
// names it: a best inside a tick cell, an offset that is not a whole number
// of ticks, and a run that declares no price tick are not ladder distances
// and keep the raw subtraction. Nor is this a quantization of the stop --
// nothing is rounded ONTO the ladder, a level already on it is merely spelled
// without the ULP (design native-feature-parity.md row B2 keeps the trail
// stop and the running best on the raw path). Returns false when the ladder
// does not name the level, leaving the caller's raw arithmetic in force.
inline bool ladder_trail_stop(double best, double offset, bool buy, double tick,
                              double* stop_out) noexcept {
    if (!grid_active(tick)) return false;
    const double best_index = grid_exact_index(best, tick);
    if (std::isnan(best_index)) return false;
    const double offset_index = grid_exact_index(offset, tick);
    if (std::isnan(offset_index) || !(offset_index >= 1.0)) return false;
    const double stop = grid_ladder_price(
        buy ? best_index + offset_index : best_index - offset_index, tick, !buy);
    if (!std::isfinite(stop)) return false;
    if (stop_out) *stop_out = stop;
    return true;
}

// Sell trail: stop = best - offset, must sit strictly below best.
// Buy trail: stop = best + offset, must sit strictly above best.
// A zero offset is the "ride the best" spelling: the stop IS the best and the
// strictness requirement does not apply, because the exit is the first move
// strictly past it (trail_zero_stop_hit). Returns false on a nonfinite or
// negative offset or an absorbed/overflowed level. A finite nonpositive stop
// is still reported; callers do not clamp it.
// `ladder_tick` is the run's own declared price tick (NativeRunSpec::price_tick),
// whatever the quantization mode: a stop it names a whole number of ticks away
// from a best on the ladder is that ladder point (ladder_trail_stop). Zero --
// the default every caller without a ladder keeps -- is the raw subtraction.
inline bool checked_trail_stop(double best, double offset, bool buy, double* stop_out,
                               double ladder_tick = 0.0) noexcept {
    if (stop_out) *stop_out = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(best) || !std::isfinite(offset) || offset < 0.0) return false;
    double stop = buy ? best + offset : best - offset;
    if (!std::isfinite(stop)) return false;
    if (offset != 0.0) {
        double ladder = 0.0;
        if (ladder_trail_stop(best, offset, buy, ladder_tick, &ladder)) stop = ladder;
        if (buy && !(stop > best)) return false;
        if (!buy && !(stop < best)) return false;
    }
    if (stop_out) *stop_out = stop;
    return true;
}

inline bool trail_best_improves(double best, double price, bool buy) noexcept {
    if (!std::isfinite(best) || !std::isfinite(price)) return false;
    return buy ? price < best : price > best;
}

// The ladder index of a best on the quantized path (grid_best_print's index):
// exact on the ladder, else the nearest under HalfUp and the favourable
// enclosing tick under Directional.
inline double grid_best_index(double best, bool buy, const GridThreshold& grid) noexcept {
    const double exact = grid_exact_index(best, grid.tick);
    if (!std::isnan(exact)) return exact;
    if (grid.half_up) return std::round(best / grid.tick);
    return buy ? grid_index_down(best, grid.tick) : grid_index_up(best, grid.tick);
}

// Zero-offset ride: the level is the best itself, so only a move strictly
// past it exits. A cursor already strictly past the best exits at the cursor;
// an adverse remaining suffix exits at the best. A favorable or flat suffix
// never exits, so arming at the best cannot immediately fire the same trail.
// Under a grid "strictly past" is on the ladder (L8b): the closed region one
// tick beyond the best's ladder point, tested exactly like every other
// trigger, with the crossing still booked at the best.
inline std::optional<GeometricHit> trail_zero_stop_hit(
        double from, double to, const GeometricHit& start, double best, bool buy,
        const GridThreshold& grid = {}) noexcept {
    const double t_start = start.t;
    const double current = start.price;
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(best)
        || !std::isfinite(current) || !std::isfinite(t_start)
        || t_start < 0.0 || t_start > 1.0) {
        return std::nullopt;
    }
    if (grid_active(grid.tick)) {
        const bool le = !buy;
        const double past = (grid_best_index(best, buy, grid) + (buy ? 1.0 : -1.0)) * grid.tick;
        auto hit = first_region_entry(from, to, start, past, le, true, grid);
        if (hit && hit->at_level) hit->price = best;
        return hit;
    }
    if (buy ? current > best : current < best) {
        return GeometricHit{t_start, current, false};
    }
    if (from == to) return std::nullopt;
    if (!(buy ? to > best : to < best)) return std::nullopt;
    const auto t_level = t_for_price(from, to, best);
    if (!t_level) return std::nullopt;
    const double t_hit = *t_level > t_start ? *t_level : t_start;
    if (t_hit > 1.0) return std::nullopt;
    return GeometricHit{t_hit, best, true};
}

// Adverse remaining suffix against a frozen best. Favorable monotonic
// motion cannot hit the trailing stop on one linear segment.
inline std::optional<GeometricHit> trail_stop_hit(
        double from, double to, const GeometricHit& start, double best, double offset, bool buy,
        const GridThreshold& grid = {}, double ladder_tick = 0.0) noexcept {
    const double t_start = start.t;
    double stop = 0.0;
    if (!checked_trail_stop(best, offset, buy, &stop, ladder_tick)) return std::nullopt;
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(t_start)
        || t_start < 0.0 || t_start > 1.0) {
        return std::nullopt;
    }
    if (offset == 0.0) return trail_zero_stop_hit(from, to, start, best, buy, grid);
    const bool le = !buy;
    return first_region_entry(from, to, start, stop, le, true, grid);
}

}  // namespace native_matching
}  // inline namespace engine_script_run_v19
}  // namespace pineforge
