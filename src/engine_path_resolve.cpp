/*
 * engine_path_resolve.cpp — path-resolution helpers for fill-priority on the synthesized OHLC path
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
namespace internal {


bool bar_path_uses_high_first(const Bar& bar) {
    // TradingView's broker emulator chooses the first intrabar leg from
    // the open's proximity to high vs low, not from candle color.
    return std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
}


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
            if (std::abs(denom) > 1e-15) {
                double t_stop = (stop_level - prev) / denom;
                double t_limit = (limit_level - prev) / denom;
                const double eps = 1e-12;
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


// Return earliest path position (segment index + [0..1] interpolation) where
// price level is crossed on OHLC path. Returns false if never crossed.
bool first_touch_position(const Bar& bar, double level, double* out_pos) {
    if (std::isnan(level) || out_pos == nullptr) return false;

    double path[4];
    if (bar_path_uses_high_first(bar)) {
        // Open nearer high: O -> H -> L -> C
        path[0] = bar.open; path[1] = bar.high; path[2] = bar.low; path[3] = bar.close;
    } else {
        // Open nearer low (or tied): O -> L -> H -> C
        path[0] = bar.open; path[1] = bar.low; path[2] = bar.high; path[3] = bar.close;
    }

    for (int i = 1; i < 4; ++i) {
        double prev = path[i - 1];
        double curr = path[i];
        double lo = std::min(prev, curr);
        double hi = std::max(prev, curr);
        if (level < lo || level > hi) continue;

        double pos = static_cast<double>(i - 1);
        double denom = curr - prev;
        if (std::abs(denom) > 1e-15) {
            pos += (level - prev) / denom;
        }
        *out_pos = pos;
        return true;
    }
    return false;
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
    if (bar_path_uses_high_first(bar)) {
        path[0] = bar.open; path[1] = bar.high; path[2] = bar.low; path[3] = bar.close;
    } else {
        path[0] = bar.open; path[1] = bar.low; path[2] = bar.high; path[3] = bar.close;
    }

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
        if (std::abs(denom) > 1e-15) {
            pos += (stop_level - prev) / denom;
        }
        *out_pos = pos;
        return true;
    }

    *out_pos = 0.0;
    return true;
}


// For flat-position opposing stop entries (long stop vs short stop), return
// true if any opposite stop is touched earlier on the bar path than `current`.
bool opposing_stop_entry_hits_first(const Bar& bar,
                                           const std::vector<PendingOrder>& orders,
                                           std::size_t current_idx) {
    if (current_idx >= orders.size()) return false;
    const PendingOrder& current = orders[current_idx];
    if (current.type != OrderType::ENTRY) return false;
    if (std::isnan(current.stop_price) || !std::isnan(current.limit_price)) return false;

    bool current_touched = current.is_long ? (bar.high >= current.stop_price)
                                           : (bar.low <= current.stop_price);
    if (!current_touched) return false;

    double cur_pos = 0.0;
    if (!entry_stop_first_touch(bar, current.stop_price, current.is_long, &cur_pos))
        return false;

    const double eps = 1e-12;
    for (std::size_t j = 0; j < orders.size(); ++j) {
        if (j == current_idx) continue;
        const PendingOrder& other = orders[j];
        if (other.type != OrderType::ENTRY) continue;
        if (other.is_long == current.is_long) continue;
        if (std::isnan(other.stop_price) || !std::isnan(other.limit_price)) continue;

        bool other_touched = other.is_long ? (bar.high >= other.stop_price)
                                           : (bar.low <= other.stop_price);
        if (!other_touched) continue;

        double other_pos = 0.0;
        if (!entry_stop_first_touch(bar, other.stop_price, other.is_long, &other_pos))
            continue;
        if (other_pos < cur_pos - eps) return true;
        // Path-tied opposing pair: prefer the long entry. Defer the short.
        if (std::abs(other_pos - cur_pos) <= eps && !current.is_long && other.is_long) {
            return true;
        }
    }
    return false;
}


DualEntryStopPathWinner dual_entry_stop_path_winner(const Bar& bar,
                                                          const std::vector<PendingOrder>& orders) {
    const PendingOrder* long_ord = nullptr;
    const PendingOrder* short_ord = nullptr;
    for (const PendingOrder& o : orders) {
        if (o.type != OrderType::ENTRY) continue;
        if (!std::isnan(o.limit_price)) continue;
        if (std::isnan(o.stop_price)) continue;
        if (o.is_long) {
            if (long_ord != nullptr) {
                return DualEntryStopPathWinner::None;
            }
            long_ord = &o;
        } else {
            if (short_ord != nullptr) {
                return DualEntryStopPathWinner::None;
            }
            short_ord = &o;
        }
    }
    if (long_ord == nullptr || short_ord == nullptr) {
        return DualEntryStopPathWinner::None;
    }
    bool lt = bar.high >= long_ord->stop_price;
    bool st = bar.low <= short_ord->stop_price;
    if (!lt || !st) {
        return DualEntryStopPathWinner::None;
    }
    double lp = 0.0;
    double sp = 0.0;
    if (!entry_stop_first_touch(bar, long_ord->stop_price, true, &lp))
        return DualEntryStopPathWinner::None;
    if (!entry_stop_first_touch(bar, short_ord->stop_price, false, &sp))
        return DualEntryStopPathWinner::None;
    const double eps = 1e-12;
    if (lp < sp - eps) {
        return DualEntryStopPathWinner::LongFirst;
    }
    if (sp < lp - eps) {
        return DualEntryStopPathWinner::ShortFirst;
    }
    // Direction-aware first-touch only ties when neither side has a clear
    // up- or down-leg (e.g. a degenerate flat bar). TradingView's broker
    // resolves the ambiguity by preferring the long stop.
    return DualEntryStopPathWinner::LongFirst;
}


// For OCA exit siblings (e.g., separate TP and SL strategy.order calls),
// compute first-touch position on OHLC path for a single-priced order.
bool exit_order_touch_position(const Bar& bar,
                                      const PendingOrder& order,
                                      PositionSide pos,
                                      double* out_pos) {
    if (out_pos == nullptr || pos == PositionSide::FLAT) return false;

    bool has_stop = !std::isnan(order.stop_price);
    bool has_limit = !std::isnan(order.limit_price);
    if (has_stop == has_limit) return false;  // only pure stop OR pure limit

    if (pos == PositionSide::LONG) {
        if (has_stop) {
            if (!(bar.low <= order.stop_price)) return false;
            if (bar.open <= order.stop_price) {
                *out_pos = 0.0;  // gap-through at bar open
                return true;
            }
            return first_touch_position(bar, order.stop_price, out_pos);
        }
        if (!(bar.high >= order.limit_price)) return false;
        if (bar.open >= order.limit_price) {
            *out_pos = 0.0;
            return true;
        }
        return first_touch_position(bar, order.limit_price, out_pos);
    }

    // SHORT position
    if (has_stop) {
        if (!(bar.high >= order.stop_price)) return false;
        if (bar.open >= order.stop_price) {
            *out_pos = 0.0;
            return true;
        }
        return first_touch_position(bar, order.stop_price, out_pos);
    }
    if (!(bar.low <= order.limit_price)) return false;
    if (bar.open <= order.limit_price) {
        *out_pos = 0.0;
        return true;
    }
    return first_touch_position(bar, order.limit_price, out_pos);
}


bool oca_exit_sibling_hits_first(const Bar& bar,
                                        const std::vector<PendingOrder>& orders,
                                        std::size_t current_idx,
                                        PositionSide pos) {
    if (current_idx >= orders.size() || pos == PositionSide::FLAT) return false;
    const PendingOrder& current = orders[current_idx];
    if (current.type != OrderType::RAW_ORDER) return false;
    if (current.oca_name.empty() || (current.oca_type != 1 && current.oca_type != 2)) return false;

    bool current_exit_style = (pos == PositionSide::LONG) ? !current.is_long : current.is_long;
    if (!current_exit_style) return false;

    double cur_pos = 0.0;
    if (!exit_order_touch_position(bar, current, pos, &cur_pos)) return false;

    const double eps = 1e-12;
    for (std::size_t j = 0; j < orders.size(); ++j) {
        if (j == current_idx) continue;
        const PendingOrder& other = orders[j];
        if (other.type != OrderType::RAW_ORDER) continue;
        if (other.oca_name != current.oca_name) continue;
        bool other_exit_style = (pos == PositionSide::LONG) ? !other.is_long : other.is_long;
        if (!other_exit_style) continue;

        double other_pos = 0.0;
        if (!exit_order_touch_position(bar, other, pos, &other_pos)) continue;
        if (other_pos < cur_pos - eps) return true;
    }
    return false;
}


// strategy.exit → OrderType::EXIT; strategy.order → RAW_ORDER. When a raw order's
// direction opposes the open position, stop/limit/trail behave like closing orders,
// not entries (fixes wrong fill prices for bracket TP/SL from strategy.order).
bool order_is_exit_style(const PendingOrder& o, PositionSide pos) {
    if (o.type == OrderType::EXIT) return true;
    if (o.type != OrderType::RAW_ORDER || pos == PositionSide::FLAT) return false;
    if (pos == PositionSide::LONG && !o.is_long) return true;
    if (pos == PositionSide::SHORT && o.is_long) return true;
    return false;
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


void append_cross_event(std::vector<PathCrossEvent>* events,
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
    if (std::abs(denom) > 1e-15) {
        pos = (level - from_price) / denom;
    }
    pos = std::clamp(pos, 0.0, 1.0);
    events->push_back({level, pos, kind});
}


std::vector<PathCrossEvent> collect_cross_events(double from_price,
                                                        double to_price,
                                                        double stop_level,
                                                        double limit_level,
                                                        double trail_level) {
    std::vector<PathCrossEvent> events;
    append_cross_event(&events, from_price, to_price, stop_level, PathCrossKind::STOP);
    append_cross_event(&events, from_price, to_price, limit_level, PathCrossKind::LIMIT);
    append_cross_event(&events, from_price, to_price, trail_level, PathCrossKind::TRAIL);

    std::sort(events.begin(), events.end(),
              [](const PathCrossEvent& a, const PathCrossEvent& b) {
                  const double eps = 1e-12;
                  if (a.path_pos < b.path_pos - eps) return true;
                  if (b.path_pos < a.path_pos - eps) return false;
                  return path_cross_kind_priority(a.kind) < path_cross_kind_priority(b.kind);
              });
    return events;
}


bool resolve_entry_stop_limit_fill(const Bar& bar,
                                          bool is_long,
                                          double stop_price,
                                          double limit_price,
                                          double* fill_price) {
    if (fill_price == nullptr || std::isnan(stop_price) || std::isnan(limit_price)) {
        return false;
    }

    double path[4];
    fill_bar_path_points(bar, path);

    bool active = is_long ? (path[0] >= stop_price) : (path[0] <= stop_price);

    auto limit_is_marketable = [&](double price) {
        return is_long ? (price <= limit_price) : (price >= limit_price);
    };

    if (active && limit_is_marketable(path[0])) {
        *fill_price = path[0];
        return true;
    }

    for (int seg_idx = 1; seg_idx < 4; ++seg_idx) {
        double from_price = path[seg_idx - 1];
        double to_price = path[seg_idx];

        if (!active) {
            bool activates = is_long
                ? (from_price < stop_price && to_price >= stop_price)
                : (from_price > stop_price && to_price <= stop_price);
            if (!activates) {
                continue;
            }

            active = true;
            from_price = stop_price;
            if (limit_is_marketable(from_price)) {
                *fill_price = from_price;
                return true;
            }
        }

        if (is_long) {
            if (from_price <= limit_price) {
                *fill_price = from_price;
                return true;
            }
            if (to_price <= limit_price) {
                *fill_price = limit_price;
                return true;
            }
        } else {
            if (from_price >= limit_price) {
                *fill_price = from_price;
                return true;
            }
            if (to_price >= limit_price) {
                *fill_price = limit_price;
                return true;
            }
        }
    }

    return false;
}


namespace {

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
    bool has_trail = false;
    bool trail_active = false;
    bool exits_at_activation = false;
};

// Initialize per-bar trail state from the strategy.exit parameters.
// trail_points is interpreted as ticks (TV ceils to next whole tick before
// applying), so the activation level lands on a mintick boundary AWAY from
// entry. The engine previously kept the raw float and rounded the activation
// level to nearest mintick at fill time, which produced a 1-tick-toward-entry
// bias on roughly 40% of community/scalping-strategy trades.
ExitTrailState compute_exit_trail_state(bool is_long, double trail_points,
                                        double trail_offset, double entry_price,
                                        double trail_best_start,
                                        double syminfo_mintick) {
    ExitTrailState s;
    s.has_trail = !std::isnan(trail_points);
    s.best_price = trail_best_start;
    if (!s.has_trail) {
        return s;
    }
    const double trail_ticks = std::ceil(trail_points);
    s.activation_level = is_long
        ? (entry_price + trail_ticks * syminfo_mintick)
        : (entry_price - trail_ticks * syminfo_mintick);
    if (!std::isnan(trail_offset)) {
        s.trail_offset_price = std::ceil(trail_offset) * syminfo_mintick;
    }
    if (!std::isnan(s.best_price)) {
        s.trail_active = is_long ? (s.best_price >= s.activation_level)
                                 : (s.best_price <= s.activation_level);
    }
    s.exits_at_activation = std::isnan(s.trail_offset_price);
    return s;
}

// Current trail-stop level for the active leg; NaN when trail is not yet armed.
double active_exit_trail_level(const ExitTrailState& s, bool is_long) {
    if (!s.has_trail || !s.trail_active) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (std::isnan(s.trail_offset_price)) {
        return s.activation_level;
    }
    return is_long ? (s.best_price - s.trail_offset_price)
                   : (s.best_price + s.trail_offset_price);
}

// Open-bar gap shortcut for non-entry bars: if bar.open already breaches an
// active trail level, the activation-as-exit level, or the stop / limit in the
// firing direction, the exit fills at bar.open. Order matters — trail first,
// then activation-as-exit, then stop, then limit (matches the original cascade).
bool try_exit_open_gap_fill(const Bar& bar, bool is_long,
                            bool has_stop, double stop_price,
                            bool has_limit, double limit_price,
                            const ExitTrailState& trail,
                            ExitPathFill* out_fill) {
    const double trail_level = active_exit_trail_level(trail, is_long);
    auto fill_at_open = [&]() {
        out_fill->should_fill = true;
        out_fill->fill_price = bar.open;
        return true;
    };
    if (is_long) {
        if (!std::isnan(trail_level) && bar.open <= trail_level) return fill_at_open();
        if (trail.exits_at_activation && bar.open >= trail.activation_level) return fill_at_open();
        if (has_stop && bar.open <= stop_price) return fill_at_open();
        if (has_limit && bar.open >= limit_price) return fill_at_open();
    } else {
        if (!std::isnan(trail_level) && bar.open >= trail_level) return fill_at_open();
        if (trail.exits_at_activation && bar.open <= trail.activation_level) return fill_at_open();
        if (has_stop && bar.open >= stop_price) return fill_at_open();
        if (has_limit && bar.open <= limit_price) return fill_at_open();
    }
    return false;
}

// Trigger levels for one OHLC-path segment. Stops fire on against-direction
// segments (long stops on falling, short stops on rising); limits fire on
// with-direction segments. The trail level is the active one on stop-firing
// segments; on limit-firing segments it can still arm if the segment crosses
// up/down through the activation level for an exit-at-activation trail.
void select_exit_segment_levels(bool is_long, bool rising, bool falling,
                                double stop_price, double limit_price,
                                const ExitTrailState& trail,
                                double* stop_level, double* limit_level,
                                double* trail_level) {
    *stop_level = std::numeric_limits<double>::quiet_NaN();
    *limit_level = std::numeric_limits<double>::quiet_NaN();
    *trail_level = std::numeric_limits<double>::quiet_NaN();
    const bool stop_seg = is_long ? falling : rising;
    const bool limit_seg = is_long ? rising : falling;
    if (stop_seg) {
        *stop_level = stop_price;
        *trail_level = active_exit_trail_level(trail, is_long);
    } else if (limit_seg) {
        *limit_level = limit_price;
        if (trail.exits_at_activation && !trail.trail_active) {
            *trail_level = trail.activation_level;
        }
    }
}

// After a segment is walked without a fill, advance trail's best price (only
// on with-direction segments) and arm the trail once best crosses activation.
void update_exit_trail_state(bool is_long, bool rising, bool falling,
                             double to_price, ExitTrailState* trail) {
    if (!trail->has_trail) return;
    if (is_long && rising) {
        if (std::isnan(trail->best_price) || to_price > trail->best_price) {
            trail->best_price = to_price;
        }
        if (!trail->trail_active && trail->best_price >= trail->activation_level) {
            trail->trail_active = true;
        }
    } else if (!is_long && falling) {
        if (std::isnan(trail->best_price) || to_price < trail->best_price) {
            trail->best_price = to_price;
        }
        if (!trail->trail_active && trail->best_price <= trail->activation_level) {
            trail->trail_active = true;
        }
    }
}

// On the entry bar, an EXIT order whose stop/limit lies on the wrong side of
// entry would have triggered before the position opened — block it.
bool entry_bar_blocks_no_trail_exit(bool is_long,
                                    double stop_price, double limit_price,
                                    double entry_price) {
    const bool has_stop = !std::isnan(stop_price);
    const bool has_limit = !std::isnan(limit_price);
    if (is_long) {
        if (has_stop && stop_price > entry_price) return true;
        if (has_limit && limit_price < entry_price) return true;
    } else {
        if (has_stop && stop_price < entry_price) return true;
        if (has_limit && limit_price > entry_price) return true;
    }
    return false;
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


// Earliest intra-bar path coordinate [0, 3) where this EXIT's stop/limit would
// first fill, ignoring trail. Orders sibling strategy.exit() calls with the same
// from_entry by TradingView OHLC path (e.g. partial TP vs full bracket).
// Returns +inf if no fill this bar or if the order uses trail (caller falls back
// to full-before-partial).
double exit_order_earliest_path_metric_no_trail(
    const Bar& bar,
    const PendingOrder& order,
    PositionSide position_side,
    bool is_entry_bar,
    double position_entry_price) {
    if (order.type != OrderType::EXIT) {
        return std::numeric_limits<double>::infinity();
    }
    if (!std::isnan(order.trail_points)) {
        return std::numeric_limits<double>::infinity();
    }

    const bool is_long = (position_side == PositionSide::LONG);
    const double stop_price = order.stop_price;
    const double limit_price = order.limit_price;
    if (std::isnan(stop_price) && std::isnan(limit_price)) {
        return std::numeric_limits<double>::infinity();
    }

    if (is_entry_bar) {
        if (entry_bar_blocks_no_trail_exit(is_long, stop_price, limit_price,
                                           position_entry_price)) {
            return std::numeric_limits<double>::infinity();
        }
    } else if (no_trail_exit_gaps_at_open(bar, is_long, stop_price, limit_price)) {
        return 0.0;
    }

    double path[4];
    fill_bar_path_points(bar, path);

    for (int seg_idx = 1; seg_idx < 4; ++seg_idx) {
        const double from_price = path[seg_idx - 1];
        const double to_price = path[seg_idx];
        const bool rising = to_price > from_price;
        const bool falling = to_price < from_price;

        double stop_level;
        double limit_level;
        const double trail_level = std::numeric_limits<double>::quiet_NaN();
        select_no_trail_exit_segment_levels(is_long, rising, falling,
                                            stop_price, limit_price,
                                            &stop_level, &limit_level);

        std::vector<PathCrossEvent> events =
            collect_cross_events(from_price, to_price, stop_level, limit_level, trail_level);
        if (!events.empty()) {
            const double eps = 1e-15;
            return (seg_idx - 1) + events[0].path_pos - eps;
        }
    }

    return std::numeric_limits<double>::infinity();
}


ExitPathFill resolve_exit_path_fill(const Bar& bar,
                                           PositionSide position_side,
                                           double stop_price,
                                           double limit_price,
                                           double trail_points,
                                           double trail_offset,
                                           double position_entry_price,
                                           double trail_best_start,
                                           bool is_entry_bar,
                                           bool magnifier_active,
                                           double syminfo_mintick) {
    ExitPathFill fill;
    if (position_side == PositionSide::FLAT) return fill;

    const bool is_long = (position_side == PositionSide::LONG);
    const bool has_stop = !std::isnan(stop_price);
    const bool has_limit = !std::isnan(limit_price);

    ExitTrailState trail = compute_exit_trail_state(
        is_long, trail_points, trail_offset, position_entry_price,
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
    if (!is_entry_bar || magnifier_active) {
        if (try_exit_open_gap_fill(bar, is_long, has_stop, stop_price,
                                   has_limit, limit_price, trail, &fill)) {
            return fill;
        }
    }

    double path[4];
    fill_bar_path_points(bar, path);

    for (int seg_idx = 1; seg_idx < 4; ++seg_idx) {
        const double from_price = path[seg_idx - 1];
        const double to_price = path[seg_idx];
        const bool rising = to_price > from_price;
        const bool falling = to_price < from_price;

        double stop_level;
        double limit_level;
        double trail_level;
        select_exit_segment_levels(is_long, rising, falling,
                                   stop_price, limit_price, trail,
                                   &stop_level, &limit_level, &trail_level);

        std::vector<PathCrossEvent> events =
            collect_cross_events(from_price, to_price, stop_level, limit_level, trail_level);
        if (!events.empty()) {
            fill.should_fill = true;
            fill.fill_price = events.front().price;
            fill.is_trail = (events.front().kind == PathCrossKind::TRAIL);
            return fill;
        }

        update_exit_trail_state(is_long, rising, falling, to_price, &trail);
    }

    return fill;
}


}  // namespace internal
}  // namespace pineforge
