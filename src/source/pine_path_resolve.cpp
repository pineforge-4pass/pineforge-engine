/*
 * pine_path_resolve.cpp — the TradingView half of the modeled OHLC path.
 *
 * Moved verbatim out of src/engine_path_resolve.cpp (R5 lane L12, 2.ii g):
 * priced-entry activation, the cross-event list, tick-quantized trail
 * activation and the whole exit-path resolver are source-layer semantics with
 * no kernel caller.  Only the Pine adapter and the source-bound tests reach
 * them.  Bodies, comments and file-internal order are unchanged; the generic
 * helpers they call (`bar_path_uses_high_first`, `fill_bar_path_points_ordered`,
 * `first_touch_position`) stay in the kernel TU and are reached through the
 * same engine_internal.hpp declarations as before.
 */

#include "../engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
namespace internal {

// Returns: -1 = stop hit first, +1 = limit hit first, 0 = neither
// Walks a 4-waypoint intra-bar price path to determine fill priority.
int price_path_priority(const Bar& bar, double stop_level, double limit_level) {
    // Build the 4-waypoint price path
    double path[4];
    if (bar_path_uses_high_first(bar)) {
        // Open nearer high: O -> H -> L -> C
        path[0] = bar.open; path[1] = bar.high; path[2] = bar.low; path[3] = bar.close;
    } else {
        // Open nearer low (or tied): O -> L -> H -> C
        path[0] = bar.open; path[1] = bar.low; path[2] = bar.high; path[3] = bar.close;
    }

    bool has_stop = !std::isnan(stop_level);
    bool has_limit = !std::isnan(limit_level);

    // Walk the path and check which level is crossed first
    double prev = path[0];
    for (int i = 1; i < 4; i++) {
        double curr = path[i];
        double lo = std::min(prev, curr);
        double hi = std::max(prev, curr);

        bool stop_in_range = has_stop && stop_level >= lo && stop_level <= hi;
        bool limit_in_range = has_limit && limit_level >= lo && limit_level <= hi;

        if (stop_in_range && limit_in_range) {
            // First crossing along prev→curr: smaller parametric t hits first.
            double denom = curr - prev;
            if (std::abs(denom) > kSegmentDenomEps) {
                double t_stop = (stop_level - prev) / denom;
                double t_limit = (limit_level - prev) / denom;
                const double eps = kPathPosEps;
                if (t_stop < t_limit - eps) return -1;
                if (t_limit < t_stop - eps) return 1;
            }
            return -1;  // tie or degenerate segment
        }
        if (stop_in_range) return -1;
        if (limit_in_range) return 1;

        prev = curr;
    }
    return 0;  // neither hit
}


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


// PathCrossKind, PathCrossEvent, ExitPathFill, DualEntryStopPathWinner
// are defined in engine_internal.hpp.

void fill_bar_path_points(const Bar& bar, double path[4]) {
    if (bar_path_uses_high_first(bar)) {
        // Open nearer high: O -> H -> L -> C
        path[0] = bar.open;
        path[1] = bar.high;
        path[2] = bar.low;
        path[3] = bar.close;
    } else {
        // Open nearer low (or tied): O -> L -> H -> C
        path[0] = bar.open;
        path[1] = bar.low;
        path[2] = bar.high;
        path[3] = bar.close;
    }
}


int path_cross_kind_priority(PathCrossKind kind) {
    switch (kind) {
        case PathCrossKind::STOP:
            return 0;
        case PathCrossKind::TRAIL:
            return 1;
        case PathCrossKind::LIMIT:
            return 2;
    }
    return 3;
}


void append_cross_event(CrossEventList* events,
                               double from_price,
                               double to_price,
                               double level,
                               PathCrossKind kind) {
    if (events == nullptr || std::isnan(level)) return;

    double lo = std::min(from_price, to_price);
    double hi = std::max(from_price, to_price);
    if (level < lo || level > hi) return;

    double pos = 0.0;
    double denom = to_price - from_price;
    if (std::abs(denom) > kSegmentDenomEps) {
        pos = (level - from_price) / denom;
    }
    pos = std::clamp(pos, 0.0, 1.0);
    if (events->n < 3) events->ev[events->n++] = {level, pos, kind};
}


namespace {

void sort_cross_events(CrossEventList& events) {
    // Insertion sort over <=3 elements with the same comparator the previous
    // std::sort used. The three kinds are pairwise distinct, so the comparator
    // is a strict total order here and any correct sort yields the identical
    // sequence.
    auto before = [](const PathCrossEvent& a, const PathCrossEvent& b) {
        const double eps = kPathPosEps;
        if (a.path_pos < b.path_pos - eps) return true;
        if (b.path_pos < a.path_pos - eps) return false;
        return path_cross_kind_priority(a.kind) < path_cross_kind_priority(b.kind);
    };
    for (int i = 1; i < events.n; ++i) {
        PathCrossEvent key = events.ev[i];
        int j = i - 1;
        while (j >= 0 && before(key, events.ev[j])) { events.ev[j + 1] = events.ev[j]; --j; }
        events.ev[j + 1] = key;
    }
}

}  // namespace


CrossEventList collect_cross_events(double from_price,
                                                        double to_price,
                                                        double stop_level,
                                                        double limit_level,
                                                        double trail_level) {
    CrossEventList events;
    append_cross_event(&events, from_price, to_price, stop_level, PathCrossKind::STOP);
    append_cross_event(&events, from_price, to_price, limit_level, PathCrossKind::LIMIT);
    append_cross_event(&events, from_price, to_price, trail_level, PathCrossKind::TRAIL);
    sort_cross_events(events);
    return events;
}


CrossEventList collect_cross_events_split(double from_price,
                                          double to_price,
                                          double tick_from_price,
                                          double tick_to_price,
                                          double stop_level,
                                          double limit_level,
                                          double trail_level,
                                          double trail_activation_level) {
    CrossEventList events;
    append_cross_event(&events, tick_from_price, tick_to_price, stop_level, PathCrossKind::STOP);
    append_cross_event(&events, tick_from_price, tick_to_price, limit_level, PathCrossKind::LIMIT);
    // An ACTIVE trail's level (running raw best -/+ offset) is crossed on the
    // raw path; a dormant exit-at-activation trail's ACTIVATION is reached on
    // the tick-quantized path (design-trail-activation-tick-bar, below). The
    // two are exclusive per segment (select_exit_segment_levels), so the
    // list still holds at most one TRAIL event.
    if (!std::isnan(trail_level)) {
        append_cross_event(&events, from_price, to_price, trail_level, PathCrossKind::TRAIL);
    } else {
        append_cross_event(&events, tick_from_price, tick_to_price,
                           trail_activation_level, PathCrossKind::TRAIL);
    }
    sort_cross_events(events);
    return events;
}


bool resolve_entry_stop_limit_fill(const Bar& bar,
                                          bool is_long,
                                          double stop_price,
                                          double limit_price,
                                          double* fill_price,
                                          bool* activated,
                                          bool* fill_at_bar_point) {
    if (fill_at_bar_point != nullptr) *fill_at_bar_point = false;
    if (fill_price == nullptr || activated == nullptr
        || std::isnan(stop_price) || std::isnan(limit_price)) {
        return false;
    }
    auto fill_at = [&](double price, bool is_bar_point) {
        *fill_price = price;
        if (fill_at_bar_point != nullptr) *fill_at_bar_point = is_bar_point;
        return true;
    };

    double path[4];
    fill_bar_path_points(bar, path);

    bool active = *activated
        || (is_long ? (path[0] >= stop_price) : (path[0] <= stop_price));
    *activated = active;

    auto limit_is_marketable = [&](double price) {
        return is_long ? (price <= limit_price) : (price >= limit_price);
    };

    if (active && limit_is_marketable(path[0])) {
        return fill_at(path[0], /*is_bar_point=*/true);
    }

    for (int seg_idx = 1; seg_idx < 4; ++seg_idx) {
        double from_price = path[seg_idx - 1];
        bool from_is_bar_point = true;
        double to_price = path[seg_idx];

        if (!active) {
            bool activates = is_long
                ? (from_price < stop_price && to_price >= stop_price)
                : (from_price > stop_price && to_price <= stop_price);
            if (!activates) {
                continue;
            }

            active = true;
            *activated = true;
            from_price = stop_price;
            from_is_bar_point = false;
            if (limit_is_marketable(from_price)) {
                return fill_at(from_price, /*is_bar_point=*/false);
            }
        }

        if (is_long) {
            if (from_price <= limit_price) {
                return fill_at(from_price, from_is_bar_point);
            }
            if (to_price <= limit_price) {
                return fill_at(limit_price, /*is_bar_point=*/false);
            }
        } else {
            if (from_price >= limit_price) {
                return fill_at(from_price, from_is_bar_point);
            }
            if (to_price >= limit_price) {
                return fill_at(limit_price, /*is_bar_point=*/false);
            }
        }
    }

    return false;
}


namespace {

// design-trail-activation-tick-bar (round 7 family K): TradingView tests a
// trailing stop's ACTIVATION against the bar's OHLC quantized to the tick
// (nearest, floor(p / mintick + 0.5), the round-6 stop / limit trigger rule)
// while the trail's running best stays the raw print. Pinned with `lab tv`
// on NYSE:F 15m (scratchpad/r7/pins/f15-trail-0415-{reissue,fixed760,
// fixed754}, 2025-04-10..18, fixed 100 shares): a short entered 04-15
// 14:45Z @9.49 with strategy.exit(trail_points = close * 0.008 /
// syminfo.mintick, trail_offset = 0) re-issued every bar — or a fixed 7.60 /
// 7.54 ticks, all ceil to 8 -> activation 9.41 — exits on the 15:45Z bar
// @9.41 in every tape. That bar's raw low is 9.415 (> 9.41: the engine's
// no-activate, which slid the exit to the 16:45Z low 9.41) and its
// tick-quantized low is 9.41 (9.415 is 9.41499.. in binary -> floor(941.499
// + 0.5) = 941), which reaches the activation and, with offset 0, exits
// one-shot at the level. The boztilkiserhan wma-adx / wma-rsi probes' 1-4
// bar exit slides (04-15, 06-20, 2026-01-21) and their exitP90 residual are
// this. The trail's peak / trough path itself stays raw (round 5; the
// round-6 stopround-xt-L-trail tape: a quantized 14.04 best would have
// filled 02-20 @14.01 where TradingView exits 02-23 at the open), and so
// does the open-gap test (unpinned). Same quantizer as
// BacktestEngine::tick_grid_price, spelled k / (1 / mintick) for decimal
// ticks so an on-grid level compares equal bit for bit.
double tick_quantized_price(double price, double mintick) {
    if (std::isnan(price) || !(mintick > 0.0)) return price;
    const double k = std::floor(price / mintick + 0.5);
    const double inv = 1.0 / mintick;
    const double inv_int = std::floor(inv + 0.5);
    if (inv_int > 0.0 && std::abs(inv - inv_int) <= 1e-6 * inv_int) {
        return k / inv_int;
    }
    return k * mintick;
}

// Per-bar trail state used while walking the synthesized OHLC path for an EXIT.
// activation_level is the absolute price at which the trail arms;
// trail_offset_price is the distance the stop trails the running best after
// activation; best_price tracks the high (long) / low (short) seen so far on
// this bar's path; exits_at_activation flags TV's special behaviour where a
// trail with no offset fires the exit at the activation level itself.
struct ExitTrailState {
    double activation_level = std::numeric_limits<double>::quiet_NaN();
    double trail_offset_price = std::numeric_limits<double>::quiet_NaN();
    double best_price = std::numeric_limits<double>::quiet_NaN();
    double mintick = std::numeric_limits<double>::quiet_NaN();
    bool has_trail = false;
    bool trail_active = false;
    bool exits_at_activation = false;
    // An EXPLICIT zero-tick trail_offset (0, or a sub-tick value that
    // truncates to 0): once armed it is a zero-distance trailing stop on the
    // raw running best (round 10 family AC), see compute_exit_trail_state.
    bool zero_offset_rides = false;
};

// Initialize per-bar trail state from the strategy.exit parameters.
// trail_points is interpreted as ticks (TV ceils to next whole tick before
// applying — with the kTrailPointsCeilEps tolerance, see engine_internal.hpp),
// so the activation level lands on a mintick boundary AWAY from entry. The
// engine previously kept the raw float and rounded the activation level to
// nearest mintick at fill time, which produced a 1-tick-toward-entry bias
// on roughly 40% of community/scalping-strategy trades. The level itself
// is materialized on the tick grid (snap_trail_level_to_tick_grid) so a bar
// extreme sitting exactly on it counts as reached.
ExitTrailState compute_exit_trail_state(bool is_long, double trail_points,
                                        double trail_price,
                                        double trail_offset, double entry_price,
                                        double trail_best_start,
                                        double syminfo_mintick) {
    ExitTrailState s;
    // A trail is armed either by trail_points (activation offset in ticks from
    // the entry price) OR by trail_price (an absolute activation price level).
    // Pine's strategy.exit accepts both; they describe the same activation
    // level two different ways. trail_points takes precedence if both are set.
    s.has_trail = !std::isnan(trail_points) || !std::isnan(trail_price);
    s.best_price = trail_best_start;
    s.mintick = syminfo_mintick;
    if (!s.has_trail) {
        return s;
    }
    if (!std::isnan(trail_points)) {
        const double trail_ticks = trail_points_to_ticks(trail_points);
        s.activation_level = snap_trail_level_to_tick_grid(
            is_long ? (entry_price + trail_ticks * syminfo_mintick)
                    : (entry_price - trail_ticks * syminfo_mintick),
            syminfo_mintick);
    } else {
        // Absolute activation price level (no entry-relative tick rounding).
        s.activation_level = trail_price;
    }
    // A fractional trail_offset (ticks) is TRUNCATED to whole ticks by
    // TradingView, not rounded up: the trailing level sits floor(offset)
    // ticks behind the running extreme. Measured on every non-gap trailing
    // exit whose level could be recovered from the TV tape (level = TV fill
    // + slippage ticks): nils123456-orb-strat on BINANCE:ETHUSDT.P
    // (trail_offset = price / mintick, slippage 0) 11/11 and
    // legalrice2697-nse-elite-strategy-v6-full-system on OANDA:EURUSD
    // (atr * 4 / mintick, slippage 2) 58/62 sat exactly ONE tick nearer the
    // extreme than the ceil() level, for offsets with a fractional part both
    // below and above .5 (never zero for the .5+ half, which rules out
    // round-to-nearest). Whole-tick offsets are unchanged.
    //
    // The truncation comes FIRST, and an offset that truncates to ZERO ticks
    // -- an explicit 0 or any sub-tick value in (0, 1) -- is TV's
    // explicit-zero trail (the zero_offset_rides rule below): an activation
    // first reached intrabar is the one-shot fill AT the activation; once
    // armed it is a zero-distance trailing stop on the raw running best. Its
    // offset price stays NaN so the activation-only route serves the
    // dormant state; positive whole-tick offsets retain the ordinary
    // best-price-minus/plus-offset trailing behaviour. Pinned with `lab tv`
    // on OANDA:EURUSD 15m (2025-04-01 -> 05-01): the tapes for
    // strategy.exit("x", "L", trail_points=3, trail_offset=0),
    // trail_offset=0.5 and trail_offset=0.9 are byte-identical (190 rows,
    // sha256 36aa80ac...). The engine used to keep floor(0.6) = 0 as a
    // FINITE zero distance that armed at the bar's extreme and filled there
    // even when the activation was first reached intrabar: winthetrade
    // ema-9-vwap ATR trail (trail_offset = atr*2 passed as ticks) on
    // OANDA:EURUSD 15m, short 2025-03-31 03:45Z @1.08330, atr*2 ~ 0.0006
    // "ticks" -> activation ceil -> 1 tick = 1.08329, offset floor -> 0;
    // exit bar O 1.08330 L 1.08314: TV 1.08329 (the activation), engine
    // 1.08314 (the low).
    const double trail_offset_ticks = trail_offset_to_ticks(trail_offset);  // NaN stays NaN
    const bool zero_tick_offset = (trail_offset_ticks == 0.0);   // explicit [0, 1)
    if (!std::isnan(trail_offset_ticks) && !zero_tick_offset) {
        s.trail_offset_price = trail_offset_ticks * syminfo_mintick;
    }
    s.exits_at_activation = std::isnan(s.trail_offset_price);
    s.zero_offset_rides = zero_tick_offset;
    // A trail whose activation the position's running extreme has ALREADY
    // reached starts the bar armed. The carried best is the raw running
    // extreme; the activation test reads it tick-quantized
    // (design-trail-activation-tick-bar), as the broker read the bars that
    // produced it.
    //
    // An OMITTED trail_offset arms this way and keeps the exit-at-activation
    // FILL rule: TV treats its activation as durable order state measured
    // against the position's running extreme. Discriminator (corpus
    // bracket-exit-stop-limit-trail-same-bar-01, 2025-08-30 08:45): entry
    // 4392.08, activation 4392.26 from trail_points=atr with NO offset
    // argument, entry-bar high 4396.01 crossed the level before the order's
    // first live bar, whose open 4392.25 sits one tick BELOW the level — TV
    // fills at that open, which only a carried armed state can produce.
    // Offset>0 trails likewise keep the reconstruction: activation is
    // durable for every trail that keeps running after it activates.
    //
    // An EXPLICIT trail_offset=0 (or a sub-tick offset that truncates to 0
    // ticks, see above) arms from the carried best too, and once armed it
    // RIDES: the level is the raw running best itself (zero distance), an
    // open at or past it fills at the open print, a with-direction leg
    // raises it, an against-direction leg fills at it (directional level
    // snap). An open BEYOND the best (or past a dormant activation) is
    // decided by its print against the directionally snapped open
    // (try_exit_open_gap_fill): a print on the stop side fills at the open,
    // one beyond it rides. Only an activation FIRST reached intrabar, by a path leg, is
    // the one-shot fill at the activation level (the trail_activation_level
    // route in select_exit_segment_levels). Round 10 family AC, pinned with
    // 14 `lab tv` tapes on NYSE:F 15m (scratchpad/r10/famAC/pins,
    // 2026-09-06; boztilkiserhan-serhan1-wma-rsi-trailing-scalp, which
    // re-issues strategy.exit(trail_points = close * 1.5% / mintick,
    // trail_offset = 0) every bar):
    //   long 2025-10-24 13:45Z @13.26, exit placed at that bar's close
    //     13.485 with trail_points 21 (activation 13.47 <= the close):
    //     14:00Z O 13.485 H 13.55 L 13.41 C 13.545 -> TV 13.49 = the open
    //     PRINT (nearest tick; the floored open-level 13.48 is what the
    //     one-shot open-gap shortcut printed, the probe's exitP90 and the
    //     seed of its all-in equity knock-on); trail_points 23 -> 13.49,
    //     24 -> 13.50, 26 -> 13.52 (= the entry bar's high: the extreme
    //     restarts at the issuing close, so the high before the order
    //     existed does not arm it), 27 -> 13.53: not reached by the
    //     placement close, the activation fires one-shot where the 14:00Z
    //     path reaches it;
    //   long 10-20 19:45Z @11.99, placement close 12.005, trail_points 1
    //     (activation 12.00 <= close): 10-21 13:30Z O 12.255 H 12.32
    //     L 12.075 C 12.265 (high-first) -> TV 12.32 = the HIGH: the open
    //     gaps ABOVE the level, the trail rides O->H and fills on H->L;
    //     trail_points 5 (activation 12.04 between the close and the open):
    //     12.32 again — the open ARMS it (observe_exit_trail_open) and it
    //     rides; trail_points 27 (activation 12.26 vs the raw open 12.255,
    //     tick-quantized 12.26): 12.26 — the open arming test is RAW, the
    //     H leg reaches 12.26 and it fires one-shot there;
    //   short 03-20 19:45Z @10.04, placement close 10.015, trail_points 5
    //     (activation 9.99): 03-21 13:30Z O 9.915 H 9.98 L 9.86 C 9.975
    //     (low-first) -> TV 9.86 = the LOW: open-armed, rides O->L, fills
    //     on L->H (the shortcut printed ceil(9.915) = 9.92);
    //   short 10-24 19:30Z @13.95, placement close 13.915, trail_points 2 /
    //     3 (activation 13.93 / 13.92 >= close): 19:45Z O 13.915 -> TV
    //     13.91 = nearest(13.915), the open breaching the placement-armed
    //     level is an open PRINT (a directional ceil would print 13.92);
    //   the NASDAQ:AAPL 04-22 13:30Z 196.135 -> 196.13 pin of
    //     test_trail_fill_snap (open-armed, low-first bar) still holds: the
    //     first leg crosses the open-level immediately, a level fill.
    // The retro-arming defect this shape used to guard against (#148: an
    // activation refreshed from a newer close dropping under an older,
    // higher peak) is closed at the source since round 9 family Z: a fresh
    // or moved trail request restarts trail_best_price_ at the issuing
    // close (engine_strategy_commands.cpp), so the carried best IS the
    // placement close on the order's first live bar.
    if (!std::isnan(s.best_price)) {
        const double tick_best = tick_quantized_price(s.best_price, syminfo_mintick);
        s.trail_active = is_long ? (tick_best >= s.activation_level)
                                 : (tick_best <= s.activation_level);
    }
    return s;
}

// Current trail-stop level for the active leg; NaN when trail is not yet armed.
double active_exit_trail_level(const ExitTrailState& s, bool is_long) {
    if (!s.has_trail || !s.trail_active) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (std::isnan(s.trail_offset_price)) {
        // An armed explicit-zero trail rides the raw running best (round 10
        // family AC); the omitted-offset shape keeps its activation level.
        return s.zero_offset_rides ? s.best_price : s.activation_level;
    }
    // best -/+ K ticks is a tick count too: 9.89 + 1 tick must equal the
    // 9.90 high that touches it (NYSE:F 15m 2025-04-03 14:00Z, `lab tv`
    // trail-eq-S-off1: TV fills @9.90 on that bar).
    return snap_trail_level_to_tick_grid(
        is_long ? (s.best_price - s.trail_offset_price)
                : (s.best_price + s.trail_offset_price),
        s.mintick);
}

// Open-bar gap shortcut for non-entry bars: if bar.open already breaches an
// active trail level, the activation-as-exit level, or the stop / limit in the
// firing direction, the exit fills at bar.open. Order matters — trail first,
// then activation-as-exit, then stop, then limit (matches the original cascade).
// design-stop-tick-rounding: the stop / limit legs test `tick_open` (the
// tick-quantized open), the trail legs the raw open.
bool try_exit_open_gap_fill(const Bar& bar, double tick_open, bool is_long,
                            bool has_stop, double stop_price,
                            bool has_limit, double limit_price,
                            const ExitTrailState& trail,
                            ExitPathFill* out_fill) {
    const double trail_level = active_exit_trail_level(trail, is_long);
    auto fill_at_open = [&](bool is_limit, bool trail_level_at_open = false) {
        out_fill->should_fill = true;
        out_fill->fill_price = bar.open;
        out_fill->is_limit = is_limit;
        out_fill->at_bar_open = true;
        out_fill->open_is_trail_level = trail_level_at_open;
        return true;
    };
    // An OMITTED-offset exit-at-activation trail whose activation the open
    // already sits past (in the favourable direction) ARMS at the open with
    // best = open and fires at open -/+ 0: that price is the trail's LEVEL,
    // not a print the order gapped through, so it is flagged for the
    // directional level snap (sub-tick open 196.135 -> 196.13 for a long
    // exit; the raw-print nearest rounding printed 196.14). A trail the open
    // gaps through in the ADVERSE direction (first test) keeps the raw-print
    // booking.
    //
    // An EXPLICIT zero-offset trail rides the raw running best once armed
    // (round 10 family AC), so an open BEYOND the best does not gap through
    // anything — it raises the best to itself (arming the trail if it was
    // dormant and the open sits past the activation). TradingView then places
    // the fresh stop at that open snapped DIRECTIONALLY to the tick grid (long
    // floor, short ceil) and tests it at once against the open's PRINT — the
    // nearest tick (tick_quantized_price: floor(open / mintick + 0.5), the
    // rounding bar_fill_price books and the production tick_open). The print
    // sits AT the stop exactly when it rounds toward the stop side (long:
    // print <= open, short: print >= open; an on-grid open is the trivial
    // case, print == open == stop): the exit fills at the open as a LEVEL
    // fill (open_is_trail_level, the consumer's directional snap books the
    // stop = the print). When the print rounds away from the stop it is one
    // tick beyond it, nothing is touched at the open, and the trail rides the
    // path from best = the raw open (observe_exit_trail_open). Pinned by 76
    // `lab tv` tapes (scratchpad/r10/famAC/pins..pins5) that separate on this
    // one variable and on no other — not the bar's path, its candle, the gap
    // size, the symbol, the side, nor whether the trail was dormant:
    //   on-grid   NASDAQ:AAPL 05-12 13:30Z O 211.05 -> 211.05 (not the 211.26
    //             high the ride booked), armed-before-the-bar 05-16 212.31,
    //             07-23 215.00, 09-05 239.96; NYSE:F 05-20 10.80, 07-25 11.33,
    //             03-16 11.84, short 03-31 9.58, 03-12 11.96, 03-23 11.89
    //   sub-tick, print toward the stop (x / 0.01 fraction below .5 for a
    //             long, at/above .5 for a short in doubles): long 04-28
    //             272.335 -> 272.33, 12-19 271.835 -> 271.83; short 04-29
    //             208.955 -> 208.96, 02-04 227.125 -> 227.13, 03-06 234.445
    //             -> 234.45, 03-31 217.005 -> 217.01, 05-23 193.665 -> 193.67,
    //             03-11 221.025 -> 221.03, 04-10 189.945 -> 189.95; NYSE:F
    //             short 03-06 9.515 -> 9.52, 01-09 14.335 -> 14.34
    //   sub-tick, print away from the stop: long NYSE:F 10-21 12.255 -> the
    //             12.32 high, 09-29 12.075 -> 12.11 (the 12.115 high floored),
    //             AAPL 05-29 203.575 -> 203.78, 07-30 211.895 -> 212.39, 09-25
    //             253.205 -> 254.32, 08-22 226.185 -> 226.95; short NYSE:F
    //             03-21 9.915 -> the 9.86 low, 03-13 9.575 -> 9.51
    const bool zero_offset_open_arms =
        trail.has_trail && trail.zero_offset_rides
        && (trail.trail_active
                ? (is_long ? bar.open > trail.best_price : bar.open < trail.best_price)
                : (is_long ? bar.open >= trail.activation_level
                           : bar.open <= trail.activation_level));
    const double open_print = tick_quantized_price(bar.open, trail.mintick);
    const bool zero_offset_print_at_level =
        zero_offset_open_arms
        && (is_long ? open_print <= bar.open : open_print >= bar.open);
    if (is_long) {
        if (!std::isnan(trail_level) && bar.open <= trail_level) return fill_at_open(false);
        if (zero_offset_print_at_level) {
            return fill_at_open(false, /*trail_level_at_open=*/true);
        }
        if (trail.exits_at_activation && !trail.zero_offset_rides
            && bar.open >= trail.activation_level) {
            return fill_at_open(false, /*trail_level_at_open=*/true);
        }
        if (has_stop && tick_open <= stop_price) return fill_at_open(false);
        if (has_limit && tick_open >= limit_price) return fill_at_open(true);
    } else {
        if (!std::isnan(trail_level) && bar.open >= trail_level) return fill_at_open(false);
        if (zero_offset_print_at_level) {
            return fill_at_open(false, /*trail_level_at_open=*/true);
        }
        if (trail.exits_at_activation && !trail.zero_offset_rides
            && bar.open <= trail.activation_level) {
            return fill_at_open(false, /*trail_level_at_open=*/true);
        }
        if (has_stop && tick_open >= stop_price) return fill_at_open(false);
        if (has_limit && tick_open <= limit_price) return fill_at_open(true);
    }
    return false;
}

// Trigger levels for one OHLC-path segment. Stops fire on against-direction
// segments (long stops on falling, short stops on rising); limits fire on
// with-direction segments. The trail level is the active one on stop-firing
// segments (crossed on the raw path); on limit-firing segments a dormant
// exit-at-activation trail can still fire if the segment reaches its
// activation level, which is reported separately because it is tested on
// the tick-quantized path (design-trail-activation-tick-bar).
void select_exit_segment_levels(bool is_long, bool rising, bool falling,
                                double stop_price, double limit_price,
                                const ExitTrailState& trail,
                                double* stop_level, double* limit_level,
                                double* trail_level,
                                double* trail_activation_level) {
    *stop_level = std::numeric_limits<double>::quiet_NaN();
    *limit_level = std::numeric_limits<double>::quiet_NaN();
    *trail_level = std::numeric_limits<double>::quiet_NaN();
    *trail_activation_level = std::numeric_limits<double>::quiet_NaN();
    const bool stop_seg = is_long ? falling : rising;
    const bool limit_seg = is_long ? rising : falling;
    if (stop_seg) {
        *stop_level = stop_price;
        *trail_level = active_exit_trail_level(trail, is_long);
    } else if (limit_seg) {
        *limit_level = limit_price;
        if (trail.exits_at_activation && !trail.trail_active) {
            *trail_activation_level = trail.activation_level;
        }
    }
}

// After a segment is walked without a fill, advance trail's best price (only
// on with-direction segments, the RAW segment end) and arm the trail once the
// tick-quantized segment end reaches the activation
// (design-trail-activation-tick-bar).
void update_exit_trail_state(bool is_long, bool rising, bool falling,
                             double to_price, double tick_to_price,
                             ExitTrailState* trail) {
    if (!trail->has_trail) return;
    if (is_long && rising) {
        if (std::isnan(trail->best_price) || to_price > trail->best_price) {
            trail->best_price = to_price;
        }
        if (!trail->trail_active && tick_to_price >= trail->activation_level) {
            trail->trail_active = true;
        }
    } else if (!is_long && falling) {
        if (std::isnan(trail->best_price) || to_price < trail->best_price) {
            trail->best_price = to_price;
        }
        if (!trail->trail_active && tick_to_price <= trail->activation_level) {
            trail->trail_active = true;
        }
    }
}

// Fold the bar's OPEN into the trail state before the intrabar path walk.
// TradingView's broker emulator sees the open as the FIRST traded price of
// the bar: a trail whose activation level the open already sits past arms AT
// the open with best = open, so the trailing level on the first leg is
// open -/+ offset. The engine used to observe prices only at the END of each
// path segment (update_exit_trail_state), so on an adverse-leg-first bar
// (|O-L| < |H-O| for a long) the trail stayed dormant through the retrace,
// armed only at the favourable extreme and filled at extreme -/+ offset --
// the worst-case price TV never prints.
//
// Evidence (winthetrade-ema-9-vwap-strategy-with-atr-trailing-stop family,
// trail_points = trail_offset = atr*2 passed as ticks; the entries are POOC
// close fills, so the carried best is the entry price itself):
//   OANDA:XAUUSD 15m long 2025-04-04 10:45Z @3110.31; 11:00Z bar O 3110.40
//     H 3136.775 L 3109.24 C 3134.46, 15 ticks / 15 ticks (mintick 0.001):
//     TV "Long Exit" 3110.385 = open - 0.015 (PnL 24.47); engine 3136.76 =
//     high - 0.015 (PnL 8631). 194 of the 195 differing XAUUSD exits fit
//     TV = open -/+ k ticks vs engine = extreme -/+ the same k, always on an
//     adverse-leg-first path with the open past the activation level.
//   NASDAQ:AAPL 15m short 2025-04-02 18:45Z @222.93; 04-03 13:30Z bar
//     O 205.54 L 202.52: TV 205.55 (open + 1 tick), engine 202.53 (low + 1).
//   NYSE:F 1D long 2025-04-21 @9.47; 04-22 bar O 9.55 H 9.72: TV 9.54,
//     engine 9.71.
//
// Only the OPEN itself arms here -- the carried best was folded in by
// compute_exit_trail_state. An omitted-offset exit-at-activation trail whose
// open already sits past its activation has been filled by
// try_exit_open_gap_fill before this runs, so for it this is a pure
// best-price observation; an explicit zero-offset trail whose open print
// did not already touch the level it creates (try_exit_open_gap_fill) is
// ARMED here by such an open (best = open) and rides the path from it
// (round 10 family AC: NYSE:F 10-21 13:30Z opens 12.255 past a 12.04
// activation and TV fills at the bar's 12.32 high, 03-21 13:30Z short opens
// 9.915 past 9.99 and fills at the 9.86 low).
void observe_exit_trail_open(bool is_long, double open, ExitTrailState* trail) {
    if (!trail->has_trail) return;
    if (is_long) {
        if (std::isnan(trail->best_price) || open > trail->best_price) {
            trail->best_price = open;
        }
        if (!trail->trail_active && open >= trail->activation_level) {
            trail->trail_active = true;
        }
    } else {
        if (std::isnan(trail->best_price) || open < trail->best_price) {
            trail->best_price = open;
        }
        if (!trail->trail_active && open <= trail->activation_level) {
            trail->trail_active = true;
        }
    }
}


// Open-bar gap shortcut for the no-trail metric: returns true when bar.open
// already breaches stop or limit in the firing direction.
bool no_trail_exit_gaps_at_open(const Bar& bar, bool is_long,
                                double stop_price, double limit_price) {
    const bool has_stop = !std::isnan(stop_price);
    const bool has_limit = !std::isnan(limit_price);
    if (is_long) {
        if (has_stop && bar.open <= stop_price) return true;
        if (has_limit && bar.open >= limit_price) return true;
    } else {
        if (has_stop && bar.open >= stop_price) return true;
        if (has_limit && bar.open <= limit_price) return true;
    }
    return false;
}

// Trigger levels for one OHLC-path segment in the trail-less metric path.
// Mirrors select_exit_segment_levels minus the trail handling.
void select_no_trail_exit_segment_levels(bool is_long, bool rising, bool falling,
                                         double stop_price, double limit_price,
                                         double* stop_level, double* limit_level) {
    *stop_level = std::numeric_limits<double>::quiet_NaN();
    *limit_level = std::numeric_limits<double>::quiet_NaN();
    const bool stop_seg = is_long ? falling : rising;
    const bool limit_seg = is_long ? rising : falling;
    if (stop_seg) {
        *stop_level = stop_price;
    } else if (limit_seg) {
        *limit_level = limit_price;
    }
}

}  // namespace


ExitPathFill resolve_exit_path_fill(const Bar& bar,
                                           PositionSide position_side,
                                           double stop_price,
                                           double limit_price,
                                           double trail_points,
                                           double trail_price,
                                           double trail_offset,
                                           double position_entry_price,
                                           double trail_best_start,
                                           bool is_entry_bar,
                                           bool magnifier_active,
                                           double syminfo_mintick,
                                           bool cascade_wp_gap,
                                           double path_start_position) {
    return resolve_exit_path_fill(bar, bar, position_side, stop_price, limit_price,
                                  trail_points, trail_price, trail_offset,
                                  position_entry_price, trail_best_start,
                                  is_entry_bar, magnifier_active, syminfo_mintick,
                                  cascade_wp_gap, path_start_position);
}


ExitPathFill resolve_exit_path_fill(const Bar& bar,
                                           const Bar& tick_bar,
                                           PositionSide position_side,
                                           double stop_price,
                                           double limit_price,
                                           double trail_points,
                                           double trail_price,
                                           double trail_offset,
                                           double position_entry_price,
                                           double trail_best_start,
                                           bool is_entry_bar,
                                           bool magnifier_active,
                                           double syminfo_mintick,
                                           bool cascade_wp_gap,
                                           double path_start_position) {
    ExitPathFill fill;
    if (position_side == PositionSide::FLAT) return fill;

    const bool is_long = (position_side == PositionSide::LONG);
    const bool has_stop = !std::isnan(stop_price);
    const bool has_limit = !std::isnan(limit_price);

    ExitTrailState trail = compute_exit_trail_state(
        is_long, trail_points, trail_price, trail_offset, position_entry_price,
        trail_best_start, syminfo_mintick);

    // Open-gap shortcut. The legacy code only ran this on non-entry bars,
    // because on the entry bar bar.open == position_entry_price and a stop /
    // limit on the wrong side would gap-fill at $0 PnL. With magnifier ON,
    // however, TV's broker emulator does treat each lower-TF sub-bar's
    // open as a fresh gap event and DOES fill wrong-side exits at the
    // entry bar's open (verified across magnifier-dist-probe-01..08b — 340
    // of 871 trades on probe-01 are wrong-side gap fills). Allow the gap
    // shortcut on the entry bar in magnifier mode only; the wrong-side
    // eligibility skip in classify_order_eligibility still gates the non-
    // magnifier path against bogus na-arithmetic stops.
    // cascade_wp_gap is the KI-67 exit-cascade in-flight leg-end waypoint POINT
    // (a degenerate O=H=L=C=W bar). The Model S gate has already established that
    // the exit level lies in the in-flight remainder in the trigger direction, so
    // the order gap-fills at that waypoint price — force the open-gap shortcut on
    // even though entry and exit share this bar (is_entry_bar).
    const double path_cursor = std::clamp(path_start_position, 0.0, 3.0);
    if (path_cursor <= kPathPosEps
        && (!is_entry_bar || magnifier_active || cascade_wp_gap)) {
        if (try_exit_open_gap_fill(bar, tick_bar.open, is_long, has_stop, stop_price,
                                   has_limit, limit_price, trail, &fill)) {
            // A gap fill happens at the bar's open — path position 0.
            fill.path_position = 0.0;
            return fill;
        }
        // No gap fill: the open is still the first price this bar the
        // resting order saw. Fold it into the trail's running best (and arm
        // the trail if the open is already past its activation) before the
        // segment walk, exactly as TV's emulator does -- see
        // observe_exit_trail_open for the tapes. Same liveness condition as
        // the gap shortcut: a mid-path cursor or a non-magnifier entry bar
        // means the order did not exist at the open.
        observe_exit_trail_open(is_long, bar.open, &trail);
    }

    // design-stop-tick-rounding: the tick-quantized twin walks the raw bar's
    // leg order, so both paths share segment indices and the cursor; stop /
    // limit crossings are taken on the tick path, trail crossings (and the
    // trail's running best) on the raw path.
    const bool high_first = bar_path_uses_high_first(bar);
    double path[4];
    double tick_path[4];
    fill_bar_path_points_ordered(bar, high_first, path);
    fill_bar_path_points_ordered(tick_bar, high_first, tick_path);

    for (int seg_idx = 1; seg_idx < 4; ++seg_idx) {
        // A priced parent entry can activate a resting from_entry bracket in
        // the middle of this path. Skip every elapsed segment and truncate
        // the containing segment to the unconsumed suffix. A waypoint cursor
        // resumes on the following segment under that segment's normal
        // direction-specific stop/limit rules.
        if (path_cursor >= static_cast<double>(seg_idx) - kPathPosEps) {
            continue;
        }
        double from_price = path[seg_idx - 1];
        const double to_price = path[seg_idx];
        double tick_from_price = tick_path[seg_idx - 1];
        const double tick_to_price = tick_path[seg_idx];
        const double seg_start = static_cast<double>(seg_idx - 1);
        if (path_cursor > seg_start + kPathPosEps) {
            const double t = std::clamp(path_cursor - seg_start, 0.0, 1.0);
            from_price += (to_price - from_price) * t;
            tick_from_price += (tick_to_price - tick_from_price) * t;
        }
        const bool rising = to_price > from_price;
        const bool falling = to_price < from_price;

        double stop_level;
        double limit_level;
        double trail_level;
        double trail_activation_level;
        select_exit_segment_levels(is_long, rising, falling,
                                   stop_price, limit_price, trail,
                                   &stop_level, &limit_level, &trail_level,
                                   &trail_activation_level);

        CrossEventList events = collect_cross_events_split(
            from_price, to_price, tick_from_price, tick_to_price,
            stop_level, limit_level, trail_level, trail_activation_level);
        if (events.n != 0) {
            fill.should_fill = true;
            fill.fill_price = events.ev[0].price;
            fill.is_trail = (events.ev[0].kind == PathCrossKind::TRAIL);
            fill.is_limit = (events.ev[0].kind == PathCrossKind::LIMIT);
            // Chronology of the fill itself, in first_touch_position units.
            // Interpolate against the FULL segment (path[seg_idx-1] ->
            // path[seg_idx]) even when a mid-path cursor truncated it, so
            // the scale matches first_touch_position's exactly. A stop /
            // limit crossing — and a dormant trail's activation reach — is
            // placed on the tick path it was found on; an active trail's
            // level crossing on the raw path.
            const bool on_tick_path = !fill.is_trail || std::isnan(trail_level);
            const double seg_origin = on_tick_path ? tick_path[seg_idx - 1]
                                                   : path[seg_idx - 1];
            const double seg_end = on_tick_path ? tick_to_price : to_price;
            const double seg_denom = seg_end - seg_origin;
            double fill_pos = seg_start;
            if (std::abs(seg_denom) > kSegmentDenomEps) {
                fill_pos += (fill.fill_price - seg_origin) / seg_denom;
            }
            fill.path_position = fill_pos;
            return fill;
        }

        update_exit_trail_state(is_long, rising, falling, to_price,
                                tick_to_price, &trail);
    }

    return fill;
}


bool cascade_exit_inflight_fires(const Bar& bar, double ap, int seg_i,
                                 PositionSide position_side,
                                 double stop_price, double limit_price) {
    // A terminal (seg_i == 2 -> C) or off-path (seg_i < 0) in-flight leg never
    // gap-fills same-bar; only the two extreme-ending legs (O->W1, W1->W2) do.
    if (seg_i < 0 || seg_i >= 2) return false;

    double path[4];
    fill_bar_path_points(bar, path);

    const bool is_long = (position_side == PositionSide::LONG);
    // W0 is the in-flight leg-end waypoint; the in-flight remainder is ap -> W0
    // (a fill AT the leg's start waypoint makes ap == path[seg_i], sweeping the
    // whole leg). A limit fires on the with-cursor-direction leg (better than the
    // level); a stop would need the opposite direction, so it can only trigger on
    // a later (reversed) leg — the general form is kept for brackets.
    const double W0 = path[seg_i + 1];
    const bool goes_up = W0 >= ap;
    const bool has_stop = !std::isnan(stop_price);
    const bool has_limit = !std::isnan(limit_price);
    bool trig = false;
    if (has_stop) {
        trig = trig
            || (is_long && !goes_up && W0 <= stop_price && stop_price <= ap)
            || (!is_long && goes_up && ap <= stop_price && stop_price <= W0);
    }
    if (has_limit) {
        trig = trig
            || (is_long && goes_up && ap < limit_price && limit_price <= W0)
            || (!is_long && !goes_up && W0 <= limit_price && limit_price < ap);
    }
    return trig;
}


}  // namespace internal
}  // namespace pineforge
