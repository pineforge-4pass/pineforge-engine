#include "pine_path_resolve_internal.hpp"

#include <cmath>
#include <limits>

namespace pineforge {
namespace internal {

// For flat-position opposing stop entries (long stop vs short stop), return
// true if any opposite stop is touched earlier on the bar path than `current`.
bool opposing_stop_entry_hits_first(const Bar& bar,
                                           const std::vector<source::PendingOrder>& orders,
                                           std::size_t current_idx,
                                           int current_bar_index) {
    return opposing_stop_entry_hits_first(bar, bar_path_uses_high_first(bar),
                                          orders, current_idx, current_bar_index);
}

bool opposing_stop_entry_hits_first(const Bar& bar, bool high_first,
                                    const std::vector<source::PendingOrder>& orders,
                                    std::size_t current_idx,
                                    int current_bar_index) {
    if (current_idx >= orders.size()) return false;
    const source::PendingOrder& current = orders[current_idx];
    auto deferred_at_consumed_close = [&](const source::PendingOrder& order) {
        return current_bar_index >= 0
            && order.birth.at_terminal_fill()
            && order.created_bar == current_bar_index;
    };
    if (deferred_at_consumed_close(current)) return false;
    if (current.type != OrderType::ENTRY) return false;
    if (std::isnan(current.legs.prices().stop_price) || !std::isnan(current.legs.prices().limit_price)) return false;

    bool current_touched = current.is_long ? (bar.high >= current.legs.prices().stop_price)
                                           : (bar.low <= current.legs.prices().stop_price);
    if (!current_touched) return false;

    double cur_pos = 0.0;
    if (!entry_stop_first_touch(bar, high_first, current.legs.prices().stop_price,
                                current.is_long, &cur_pos))
        return false;

    const double eps = kPathPosEps;
    for (std::size_t j = 0; j < orders.size(); ++j) {
        if (j == current_idx) continue;
        const source::PendingOrder& other = orders[j];
        if (deferred_at_consumed_close(other)) continue;
        if (other.type != OrderType::ENTRY) continue;
        if (other.is_long == current.is_long) continue;
        if (std::isnan(other.legs.prices().stop_price) || !std::isnan(other.legs.prices().limit_price)) continue;

        bool other_touched = other.is_long ? (bar.high >= other.legs.prices().stop_price)
                                           : (bar.low <= other.legs.prices().stop_price);
        if (!other_touched) continue;

        double other_pos = 0.0;
        if (!entry_stop_first_touch(bar, high_first, other.legs.prices().stop_price,
                                    other.is_long, &other_pos))
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
                                                          const std::vector<source::PendingOrder>& orders,
                                                          int current_bar_index) {
    return dual_entry_stop_path_winner(bar, bar_path_uses_high_first(bar),
                                       orders, current_bar_index);
}

DualEntryStopPathWinner dual_entry_stop_path_winner(const Bar& bar, bool high_first,
                                                     const std::vector<source::PendingOrder>& orders,
                                                     int current_bar_index) {
    const source::PendingOrder* long_ord = nullptr;
    const source::PendingOrder* short_ord = nullptr;
    for (const source::PendingOrder& o : orders) {
        if (current_bar_index >= 0
            && o.birth.at_terminal_fill()
            && o.created_bar == current_bar_index) {
            continue;
        }
        if (o.type != OrderType::ENTRY) continue;
        if (!std::isnan(o.legs.prices().limit_price)) continue;
        if (std::isnan(o.legs.prices().stop_price)) continue;
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
    bool lt = bar.high >= long_ord->legs.prices().stop_price;
    bool st = bar.low <= short_ord->legs.prices().stop_price;
    if (!lt || !st) {
        return DualEntryStopPathWinner::None;
    }
    double lp = 0.0;
    double sp = 0.0;
    if (!entry_stop_first_touch(bar, high_first, long_ord->legs.prices().stop_price, true, &lp))
        return DualEntryStopPathWinner::None;
    if (!entry_stop_first_touch(bar, high_first, short_ord->legs.prices().stop_price, false, &sp))
        return DualEntryStopPathWinner::None;
    const double eps = kPathPosEps;
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
                                      const source::PendingOrder& order,
                                      PositionSide pos,
                                      double* out_pos) {
    return exit_order_touch_position(bar, bar_path_uses_high_first(bar),
                                     order, pos, out_pos);
}

bool exit_order_touch_position(const Bar& bar, bool high_first,
                               const source::PendingOrder& order,
                               PositionSide pos,
                               double* out_pos) {
    if (out_pos == nullptr || pos == PositionSide::FLAT) return false;

    bool has_stop = !std::isnan(order.legs.prices().stop_price);
    bool has_limit = !std::isnan(order.legs.prices().limit_price);
    if (has_stop == has_limit) return false;  // only pure stop OR pure limit

    if (pos == PositionSide::LONG) {
        if (has_stop) {
            if (!(bar.low <= order.legs.prices().stop_price)) return false;
            if (bar.open <= order.legs.prices().stop_price) {
                *out_pos = 0.0;  // gap-through at bar open
                return true;
            }
            return first_touch_position(bar, high_first, order.legs.prices().stop_price, out_pos);
        }
        if (!(bar.high >= order.legs.prices().limit_price)) return false;
        if (bar.open >= order.legs.prices().limit_price) {
            *out_pos = 0.0;
            return true;
        }
        return first_touch_position(bar, high_first, order.legs.prices().limit_price, out_pos);
    }

    // SHORT position
    if (has_stop) {
        if (!(bar.high >= order.legs.prices().stop_price)) return false;
        if (bar.open >= order.legs.prices().stop_price) {
            *out_pos = 0.0;
            return true;
        }
        return first_touch_position(bar, high_first, order.legs.prices().stop_price, out_pos);
    }
    if (!(bar.low <= order.legs.prices().limit_price)) return false;
    if (bar.open <= order.legs.prices().limit_price) {
        *out_pos = 0.0;
        return true;
    }
    return first_touch_position(bar, high_first, order.legs.prices().limit_price, out_pos);
}


bool oca_exit_sibling_hits_first(const Bar& bar,
                                        const std::vector<source::PendingOrder>& orders,
                                        std::size_t current_idx,
                                        PositionSide pos) {
    return oca_exit_sibling_hits_first(bar, bar_path_uses_high_first(bar),
                                       orders, current_idx, pos);
}

bool oca_exit_sibling_hits_first(const Bar& bar, bool high_first,
                                 const std::vector<source::PendingOrder>& orders,
                                 std::size_t current_idx,
                                 PositionSide pos) {
    if (current_idx >= orders.size() || pos == PositionSide::FLAT) return false;
    const source::PendingOrder& current = orders[current_idx];
    if (current.type != OrderType::RAW_ORDER) return false;
    if (current.oca_name.empty() || (current.oca_type != 1 && current.oca_type != 2)) return false;

    bool current_exit_style = (pos == PositionSide::LONG) ? !current.is_long : current.is_long;
    if (!current_exit_style) return false;

    double cur_pos = 0.0;
    if (!exit_order_touch_position(bar, high_first, current, pos, &cur_pos)) return false;

    const double eps = kPathPosEps;
    for (std::size_t j = 0; j < orders.size(); ++j) {
        if (j == current_idx) continue;
        const source::PendingOrder& other = orders[j];
        if (other.type != OrderType::RAW_ORDER) continue;
        if (other.oca_name != current.oca_name) continue;
        bool other_exit_style = (pos == PositionSide::LONG) ? !other.is_long : other.is_long;
        if (!other_exit_style) continue;

        double other_pos = 0.0;
        if (!exit_order_touch_position(bar, high_first, other, pos, &other_pos)) continue;
        if (other_pos < cur_pos - eps) return true;
    }
    return false;
}


// strategy.exit → OrderType::EXIT; strategy.order → RAW_ORDER. When a raw order's
// direction opposes the open position, stop/limit/trail behave like closing orders,
// not entries (fixes wrong fill prices for bracket TP/SL from strategy.order).
bool order_is_exit_style(const source::PendingOrder& o, PositionSide pos) {
    if (o.type == OrderType::EXIT) return true;
    if (o.type != OrderType::RAW_ORDER || pos == PositionSide::FLAT) return false;
    if (pos == PositionSide::LONG && !o.is_long) return true;
    if (pos == PositionSide::SHORT && o.is_long) return true;
    return false;
}

namespace {
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
    const source::PendingOrder& order,
    PositionSide position_side,
    bool is_entry_bar,
    double position_entry_price, int64_t position_cycle, int64_t bar_index) {
    return exit_order_earliest_path_metric_no_trail(
        bar, bar_path_uses_high_first(bar), order, position_side,
        is_entry_bar, position_entry_price, position_cycle, bar_index);
}

double exit_order_earliest_path_metric_no_trail(
    const Bar& bar,
    bool high_first,
    const source::PendingOrder& order,
    PositionSide position_side,
    bool is_entry_bar,
    double position_entry_price, int64_t position_cycle, int64_t bar_index) {
    if (order.type != OrderType::EXIT) {
        return std::numeric_limits<double>::infinity();
    }
    if (!std::isnan(order.legs.prices().trail_points) || !std::isnan(order.legs.prices().trail_price)) {
        return std::numeric_limits<double>::infinity();
    }

    const bool is_long = (position_side == PositionSide::LONG);
    // The owner transition resolved each leg's lower-bound coordinate. An
    // unavailable leg cannot hide its independently ready sibling's path.
    const double stop_price =
        (!order.leg_activation.stop_ready(position_cycle, bar_index)
         || !order.legs.available(exit_legs::Leg::Stop, bar_index))
            ? std::numeric_limits<double>::quiet_NaN()
            : order.legs.prices().stop_price;
    const double limit_price =
        (!order.leg_activation.limit_ready(position_cycle, bar_index)
         || !order.legs.available(exit_legs::Leg::Limit, bar_index))
            ? std::numeric_limits<double>::quiet_NaN()
            : order.legs.prices().limit_price;
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
    fill_bar_path_points_ordered(bar, high_first, path);

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

        CrossEventList events =
            collect_cross_events(from_price, to_price, stop_level, limit_level, trail_level);
        if (events.n != 0) {
            const double eps = 1e-15;
            return (seg_idx - 1) + events.ev[0].path_pos - eps;
        }
    }

    return std::numeric_limits<double>::infinity();
}

}  // namespace internal
}  // namespace pineforge
