/*
 * engine_path_resolve.cpp — the generic half of the modeled OHLC path.
 *
 * A kernel TU needs exactly three things from the path: the forced path-order
 * override a replay/live host installs, the open-proximity rule that picks the
 * first intrabar leg, and the first position at which a level is touched on
 * that path (engine_orders.cpp).  Everything else the file used to hold is
 * TradingView exit/entry resolution and now lives in
 * src/source/pine_path_resolve.cpp; the two halves are the same
 * `pineforge::internal` namespace and the same declarations in
 * engine_internal.hpp, so callers did not move.
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>

namespace pineforge {
namespace internal {


namespace {
// ABI v4 live-runtime surface (task 4): thread-local forced path order.
// 0 AUTO, 1 HIGH_FIRST, 2 LOW_FIRST. thread_local is sufficient because a
// BacktestEngine handle is single-threaded per run; PathOrderScope
// (engine_run.cpp) installs this for exactly the duration of one run() and
// restores AUTO (0) on every exit path, so it can never leak into a later
// run on this thread that did not itself request a forced order.
thread_local int g_path_order_override = 0;
}  // namespace

void set_path_order_override(int mode) { g_path_order_override = mode; }

int path_order_override() { return g_path_order_override; }

bool bar_path_uses_high_first(const Bar& bar) {
    if (g_path_order_override == 1) return true;
    if (g_path_order_override == 2) return false;
    // TradingView's broker emulator chooses the first intrabar leg from
    // the open's proximity to high vs low, not from candle color.
    return std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
}


// Return earliest path position (segment index + [0..1] interpolation) where
// price level is crossed on OHLC path. Returns false if never crossed.
bool first_touch_position(const Bar& bar, double level, double* out_pos) {
    return first_touch_position(bar, bar_path_uses_high_first(bar), level, out_pos);
}

bool first_touch_position(const Bar& bar, bool high_first, double level,
                          double* out_pos) {
    if (std::isnan(level) || out_pos == nullptr) return false;

    double path[4];
    fill_bar_path_points_ordered(bar, high_first, path);

    for (int i = 1; i < 4; ++i) {
        double prev = path[i - 1];
        double curr = path[i];
        double lo = std::min(prev, curr);
        double hi = std::max(prev, curr);
        if (level < lo || level > hi) continue;

        double pos = static_cast<double>(i - 1);
        double denom = curr - prev;
        if (std::abs(denom) > kSegmentDenomEps) {
            pos += (level - prev) / denom;
        }
        *out_pos = pos;
        return true;
    }
    return false;
}



void fill_bar_path_points_ordered(const Bar& bar, bool high_first, double path[4]) {
    if (high_first) {
        path[0] = bar.open;
        path[1] = bar.high;
        path[2] = bar.low;
        path[3] = bar.close;
    } else {
        path[0] = bar.open;
        path[1] = bar.low;
        path[2] = bar.high;
        path[3] = bar.close;
    }
}


}  // namespace internal

// The entry-side half of the per-lot excursion capability (RULING A48). The
// owner of a lot's excursion knows one thing the kernel does not — whether
// its opening fill sat at a price the entry bar's path reaches, or after the
// whole path — and the kernel knows the rest: which leg the path walks first
// and where a price is first touched, the same two rules above that the
// matcher and the closing row already use. So the owner declares the fill
// point and the kernel derives the mask; no host writes a lot's flags.
void BacktestEngine::declare_opened_lot_entry_bar_mask(
        uint64_t entry_incarnation, const Bar& entry_bar,
        OpenedLotFillPoint fill_point) {
    for (auto& lot : pyramid_entries_) {
        if (lot.entry_incarnation != entry_incarnation) continue;
        if (fill_point == OpenedLotFillPoint::AfterPath) {
            // The fill is the bar's closing point: the whole path precedes it.
            lot.skip_entry_bar_high = true;
            lot.skip_entry_bar_low = true;
            continue;
        }
        double fill_pos = 0.0;
        if (!internal::first_touch_position(entry_bar, lot.price, &fill_pos)) continue;
        const bool high_first = internal::bar_path_uses_high_first(entry_bar);
        const double high_pos = high_first ? 1.0 : 2.0;
        const double low_pos = high_first ? 2.0 : 1.0;
        lot.skip_entry_bar_high = (high_pos < fill_pos);
        lot.skip_entry_bar_low = (low_pos < fill_pos);
    }
}

}  // namespace pineforge
