/*
 * pine_path_resolve.cpp — the TradingView half of the modeled OHLC path.
 *
 * Moved verbatim out of src/engine_path_resolve.cpp (R5 lane L12, 2.ii g).
 * R5 lane N10 then deleted every symbol of that move whose only remaining
 * references were its own declaration and the tests: the priced-entry
 * activation, the cross-event list, the tick-quantized trail machinery and
 * the whole exit-path resolver were a second fill simulation beside the
 * kernel matcher (src/native_matching.hpp + NativeExecutionConsumer), which
 * is what the adapter has resolved exits on since the R5 re-lowerings.  What
 * is left is the one function a live caller still reaches:
 * `entry_stop_first_touch`, the direction-aware first-touch position a
 * TradingView stop ENTRY fires at (pine_adapter.cpp).  The generic helpers it
 * calls (`bar_path_uses_high_first`, `fill_bar_path_points_ordered`) stay in
 * the kernel TU and are reached through the same engine_internal.hpp
 * declarations as before.
 */

#include "../engine_internal.hpp"
#include <pineforge/compat/pine/trail_ticks.hpp>

#include <algorithm>
#include <cmath>

namespace pineforge {
namespace internal {

// TradingView's trail tick arithmetic lives with the source layer (R5 lane N14).
using compat::pine::snap_trail_level_to_tick_grid;
using compat::pine::trail_offset_to_ticks;
using compat::pine::trail_points_to_ticks;

// First path position where a stop ENTRY can fire, accounting for direction:
// long stops only fire on up-segments (price rising through the stop), short
// stops only fire on down-segments. The gap-fill shortcut uses non-strict
// comparisons: when bar.open already sits at or beyond the stop level in the
// firing direction the order fills at open (path position 0). For the
// open-equals-stop case both legs return 0 simultaneously and the dual-stop
// arbitration breaks the tie in favour of the long leg — this matches TV's
// broker emulator on probe 83.
bool entry_stop_first_touch(const Bar& bar, double stop_level,
                                   bool is_long, double* out_pos) {
    return entry_stop_first_touch(bar, bar_path_uses_high_first(bar),
                                  stop_level, is_long, out_pos);
}

bool entry_stop_first_touch(const Bar& bar, bool high_first, double stop_level,
                            bool is_long, double* out_pos) {
    if (std::isnan(stop_level) || out_pos == nullptr) return false;

    if (is_long) {
        if (!(bar.high >= stop_level)) return false;
        if (bar.open >= stop_level) {
            *out_pos = 0.0;
            return true;
        }
    } else {
        if (!(bar.low <= stop_level)) return false;
        if (bar.open <= stop_level) {
            *out_pos = 0.0;
            return true;
        }
    }

    double path[4];
    fill_bar_path_points_ordered(bar, high_first, path);

    for (int i = 1; i < 4; ++i) {
        double prev = path[i - 1];
        double curr = path[i];
        if (is_long) {
            if (curr <= prev) continue;
        } else {
            if (curr >= prev) continue;
        }
        double lo = std::min(prev, curr);
        double hi = std::max(prev, curr);
        if (stop_level < lo || stop_level > hi) continue;

        double pos = static_cast<double>(i - 1);
        double denom = curr - prev;
        if (std::abs(denom) > kSegmentDenomEps) {
            pos += (stop_level - prev) / denom;
        }
        *out_pos = pos;
        return true;
    }

    *out_pos = 0.0;
    return true;
}

}  // namespace internal
}  // namespace pineforge
