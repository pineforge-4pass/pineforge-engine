#pragma once
// TradingView's trailing-exit tick arithmetic: how strategy.exit's
// trail_points / trail_offset are read as tick counts and how a trailing
// level is materialized on the tick grid. Every constant here is a
// TradingView calibration (the pins are quoted below), so it belongs to the
// source layer; the kernel's own trail is TrailTicks (native_order.hpp),
// which needs none of it (R5 lane N14: moved verbatim from
// src/engine_internal.hpp, where no kernel translation unit used it).
#include <cmath>

namespace pineforge::compat::pine {

// ── Trailing-exit tick arithmetic ─────────────────────────────────────
//
// TradingView reads strategy.exit's trail_points / trail_offset as TICK
// COUNTS: trail_points is ceiled WITH a tolerance that swallows floating-
// point residue — a value computed as close * 0.02 / syminfo.mintick that
// lands a few ulps past a whole number is that whole number, not the next
// one — while trail_offset is floored EXACTLY. Pinned with `lab tv` on
// NYSE:F 15m 2025-04-01..05, short from the 04-02 19:00Z signal (entry
// 10.11, mintick 0.01; scratchpad/r7/pins/trail-eq-*), each zero-offset
// exit filling at its activation level:
//   trail_points 14.00001                -> 14 ticks (9.97)
//   trail_points 14.0001 / 14.001        -> 15 ticks (9.96)
//   trail_points 0.14 / syminfo.mintick  (= 14.000000000000002) -> 14 (9.97)
//   trail_points 14.0000001              -> 14 (9.97)
//   trail_points 18.2                    -> 19 (9.92)
//   trail_offset 0.3 / (syminfo.mintick * 10)  (= 2.9999999999999996)
//     -> 2 ticks (trail_points 18: fills 04-03 14:15Z @9.85 = trough 9.83
//     + 2t; a tolerant floor would trail 3t and print 9.86)
// and BINANCE:BTCUSDT 15m short 2025-08-17 23:30Z @117559.99 with
// trail_points = 117560 * 0.02 / 0.01 = 235120.00000000003: TV exits
// 08-18 03:30Z @115208.79 (235120 ticks); std::ceil gave 235121 -> .78.
// The ceil tolerance is bounded in [1e-5, 1e-4) by the 14.00001 / 14.0001
// pair; 5e-5 sits in the middle. Round 5's sub-tick pins hold: 0.0006 ->
// 1 tick, offsets 0 / 0.5 / 0.9 -> 0, 1.4 -> 1.
constexpr double kTrailPointsCeilEps = 5e-5;

inline double trail_points_to_ticks(double trail_points) {
    return std::ceil(trail_points - kTrailPointsCeilEps);  // NaN stays NaN
}

inline double trail_offset_to_ticks(double trail_offset) {
    return std::floor(trail_offset);  // exact; NaN stays NaN
}

// The broker keeps trailing levels ON the tick grid: an activation level
// entry -/+ N ticks and a trailing level best -/+ K ticks are tick counts,
// and a bar extreme landing exactly on the level TOUCHES it. Built in
// doubles, 10.11 - 21 * 0.01 is 9.899999999999999 — one ulp under the 9.9
// low — and the inclusive segment test reads it as "not reached" (NYSE:F
// 15m 2025-04-03 13:45Z, bar O 10.165 H 10.18 L 9.9 C 9.9: TV fills the
// trail there @9.90, `lab tv` trail-eq-S-off0-tp21; the engine gap-filled
// the next open @9.89). Materialize a level within a millionth of a tick
// of the grid as the grid point, spelled the way tick_grid_price
// (engine.hpp) spells it so it compares EQUAL to a feed print bit for bit;
// a genuinely sub-tick level (best 196.135 - 1 tick = 196.125) stays raw
// and takes the directional fill snap downstream.
inline double snap_trail_level_to_tick_grid(double price, double mintick) {
    if (std::isnan(price) || !(mintick > 0.0)) return price;
    const double r = price / mintick;
    const double k = std::floor(r + 0.5);
    if (std::abs(r - k) > 1e-6) return price;
    const double inv = 1.0 / mintick;
    const double inv_int = std::floor(inv + 0.5);
    if (inv_int > 0.0 && std::abs(inv - inv_int) <= 1e-6 * inv_int) {
        return k / inv_int;
    }
    return k * mintick;
}

}  // namespace pineforge::compat::pine
