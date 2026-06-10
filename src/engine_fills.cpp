/*
 * engine_fills.cpp — process_pending_orders — the bar-pump fill loop
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;


// strategy_entry / strategy_close / strategy_close_all / strategy_exit
// moved to engine_strategy_commands.cpp.
void BacktestEngine::process_pending_orders(const Bar& bar) {
    // Update risk state
    update_risk_state();

    double trail_best_path_state = trail_best_price_;
    update_trail_best_for_bar_open(bar);
    sort_exit_siblings_by_path_fill(bar);

    sort_orders_by_fill_phase(bar);

    if (priced_entry_activity_bar_ != bar_index_) {
        priced_entry_activity_bar_ = bar_index_;
        priced_entry_filled_this_bar_ = false;
    }

    int exit_closed_from_bar = -1;   // created_bar of the last full-close exit
    bool exit_closed_was_long = false;  // direction of the closed position

    // Reusable member scratchpad (capacity persists across calls; avoids a
    // heap allocation per process_pending_orders call). Must start empty.
    std::unordered_set<std::string>& pass0_opposing_skip_ids = scratch_skip_ids_;
    pass0_opposing_skip_ids.clear();
    DualEntryStopPathWinner dual_entry_path_ = DualEntryStopPathWinner::None;
    if (position_side_ == PositionSide::FLAT) {
        dual_entry_path_ = dual_entry_stop_path_winner(bar, pending_orders_);
    }

    for (int opposing_pass = 0; opposing_pass < 2; ++opposing_pass) {
    std::vector<size_t> filled_indices;
    filled_indices.reserve(pending_orders_.size());
    // TV cancels pending SAME-DIRECTION entries placed on a prior on_bar when
    // a full strategy.exit closes the position on this bar. Opposite-direction
    // (reversal) entries still fire. Entries placed on the SAME on_bar as the
    // fired exit also still fire (close + entry reversal placed together).

    for (size_t i = 0; i < pending_orders_.size(); i++) {
        PendingOrder& order = pending_orders_[i];
        auto eligibility = classify_order_eligibility(
            order, opposing_pass, dual_entry_path_, pass0_opposing_skip_ids,
            exit_closed_from_bar, exit_closed_was_long);
        if (eligibility == OrderEligibility::Remove) {
            filled_indices.push_back(i);
            continue;
        }
        if (eligibility == OrderEligibility::Skip) {
            continue;
        }

        auto fill = evaluate_fill_price(
            order, i, bar, opposing_pass, trail_best_path_state,
            pass0_opposing_skip_ids);
        if (fill.kind != FillEvaluation::Kind::Fill) {
            continue;
        }

        apply_filled_order_to_state(
            order, i, fill.fill_price, bar,
            trail_best_path_state,
            exit_closed_from_bar, exit_closed_was_long,
            filled_indices);
    }
    compact_filled_pending_orders(filled_indices, exit_closed_from_bar, exit_closed_was_long);
    }  // opposing_pass

    // If position is flat after processing, purge any remaining exit orders
    if (position_side_ == PositionSide::FLAT) {
        purge_exit_orders();
    }
}

// ────────────────────────────────────────────────────────────────────
// process_pending_orders helpers
// ────────────────────────────────────────────────────────────────────

// Update trailing stop best price for the current bar's open / high / low.
// Called once per bar before any intra-bar fill evaluation.
void BacktestEngine::update_trail_best_for_bar_open(const Bar& bar) {
    if (position_side_ == PositionSide::LONG) {
        if (std::isnan(trail_best_price_) || bar.high > trail_best_price_)
            trail_best_price_ = bar.high;
    } else if (position_side_ == PositionSide::SHORT) {
        if (std::isnan(trail_best_price_) || bar.low < trail_best_price_)
            trail_best_price_ = bar.low;
    }
}

// Order sibling EXIT orders (sharing the same from_entry id): by earliest
// intra-bar OHLC path trigger when neither uses trail; otherwise full
// (100%) before partial. Stable so PineScript source order is preserved
// for ties.
//
// PERF NOTE (P3): this stable_sort and the following sort_orders_by_fill_phase
// stable_sort are intentionally kept as two passes. They CANNOT be merged into
// one combined comparator without risking a change in fill order:
//   - This pass orders exit siblings by a path-fill metric (or full-before-
//     partial) that the fill-phase comparator has no knowledge of.
//   - The fill-phase pass breaks final ties by created_seq, NOT by current
//     array position, so it does not preserve this pass's path-fill ordering
//     for orders that tie on fill phase. Folding the path-fill metric into the
//     fill-phase comparator would re-rank those ties and alter which sibling
//     fills first.
// Correctness over perf: leave as two sequential stable_sorts.
void BacktestEngine::sort_exit_siblings_by_path_fill(const Bar& bar) {
    std::stable_sort(pending_orders_.begin(), pending_orders_.end(),
        [&](const PendingOrder& a, const PendingOrder& b) {
            if (a.type != OrderType::EXIT || b.type != OrderType::EXIT
                || a.from_entry != b.from_entry || a.from_entry.empty()) {
                return false;
            }
            auto qp = [](const PendingOrder& o) {
                double q = std::isnan(o.qty_percent) ? 100.0 : std::clamp(o.qty_percent, 0.0, 100.0);
                return q;
            };
            bool a_full = qp(a) >= 100.0 - kFullPercentEps;
            bool b_full = qp(b) >= 100.0 - kFullPercentEps;
            const bool a_trail = !std::isnan(a.trail_points);
            const bool b_trail = !std::isnan(b.trail_points);
            if (a_trail || b_trail) {
                if (a_full != b_full) {
                    return a_full;
                }
                return false;
            }
            bool is_ent_bar = (position_open_bar_ == bar_index_);
            double ma = exit_order_earliest_path_metric_no_trail(
                bar, a, position_side_, is_ent_bar, position_entry_price_);
            double mb = exit_order_earliest_path_metric_no_trail(
                bar, b, position_side_, is_ent_bar, position_entry_price_);
            const double inf = std::numeric_limits<double>::infinity();
            const double eps = kPathPosEps;
            if (ma < inf && mb < inf) {
                if (ma < mb - eps) {
                    return true;
                }
                if (mb < ma - eps) {
                    return false;
                }
            }
            if (ma < inf && mb >= inf) {
                return true;
            }
            if (mb < inf && ma >= inf) {
                return false;
            }
            if (a_full != b_full) {
                return a_full;
            }
            return false;
        });
}

// Sort by the first possible fill point, then by PineScript source order.
// Market orders fill at bar open. Priced orders that gap through at open
// share that same fill point; other priced orders evaluate later on the
// synthetic OHLC path. This avoids broad type-based reordering.
void BacktestEngine::sort_orders_by_fill_phase(const Bar& bar) {
    std::stable_sort(pending_orders_.begin(), pending_orders_.end(),
        [&](const PendingOrder& a, const PendingOrder& b) {
            auto fill_phase = [&](const PendingOrder& o) {
                bool has_stop = !std::isnan(o.stop_price);
                bool has_limit = !std::isnan(o.limit_price);
                bool has_trail = !std::isnan(o.trail_points);
                bool exit_style = order_is_exit_style(o, position_side_);

                if (o.type == OrderType::MARKET
                    || (!has_stop && !has_limit && !has_trail)) {
                    return 0;
                }

                if (exit_style) {
                    if (position_side_ == PositionSide::LONG) {
                        if (has_stop && bar.open <= o.stop_price) return 0;
                        if (has_limit && bar.open >= o.limit_price) return 0;
                    } else if (position_side_ == PositionSide::SHORT) {
                        if (has_stop && bar.open >= o.stop_price) return 0;
                        if (has_limit && bar.open <= o.limit_price) return 0;
                    }
                    return 1;
                }

                if (o.is_long) {
                    if (has_stop && bar.open >= o.stop_price) return 0;
                    if (has_limit && bar.open <= o.limit_price) return 0;
                } else {
                    if (has_stop && bar.open <= o.stop_price) return 0;
                    if (has_limit && bar.open >= o.limit_price) return 0;
                }
                return 1;
            };

            int pa = fill_phase(a);
            int pb = fill_phase(b);
            if (pa != pb) return pa < pb;
            auto is_entry_same_as_current_position = [&](const PendingOrder& o) {
                return (o.type == OrderType::MARKET || o.type == OrderType::ENTRY)
                    && ((position_side_ == PositionSide::LONG && o.is_long)
                        || (position_side_ == PositionSide::SHORT && !o.is_long));
            };
            bool a_exit_style = order_is_exit_style(a, position_side_);
            bool b_exit_style = order_is_exit_style(b, position_side_);
            bool a_entry_same = is_entry_same_as_current_position(a);
            bool b_entry_same = is_entry_same_as_current_position(b);
            if (a_exit_style && b_entry_same) return true;
            if (b_exit_style && a_entry_same) return false;
            // TradingView empirically processes a same-bar full market
            // exit BEFORE an opposite-direction priced (stop/limit) entry,
            // even when the priced entry gaps through the open and would
            // otherwise share the entry's same fill phase. Verified by
            // ``test_market_close_fills_before_same_bar_opposite_stop_entry``
            // (close-then-fresh-stop) and probes 52, 63, 72, 92 (close-
            // then-deferred-flip-stop). Without this rule the priced entry
            // would flip the still-open position at the open, eating the
            // close-driven exit's deferred-flip carry.
            auto is_full_market_exit = [&](const PendingOrder& o) {
                if (o.type != OrderType::EXIT) return false;
                bool has_stop = !std::isnan(o.stop_price);
                bool has_limit = !std::isnan(o.limit_price);
                bool has_trail = !std::isnan(o.trail_points);
                if (has_stop || has_limit || has_trail) return false;
                double qp = std::isnan(o.qty_percent) ? 100.0 : o.qty_percent;
                return qp >= 100.0 - kFullPercentEps;
            };
            auto is_opposite_priced_entry = [&](const PendingOrder& o) {
                if (o.type != OrderType::ENTRY) return false;
                if (position_side_ == PositionSide::FLAT) return false;
                bool entry_long = o.is_long;
                bool pos_long = (position_side_ == PositionSide::LONG);
                return entry_long != pos_long;
            };
            bool a_full_close = is_full_market_exit(a);
            bool b_full_close = is_full_market_exit(b);
            bool a_opp_priced = is_opposite_priced_entry(a);
            bool b_opp_priced = is_opposite_priced_entry(b);
            if (a_full_close && b_opp_priced) return true;
            if (b_full_close && a_opp_priced) return false;
            return a.created_seq < b.created_seq;
        });
}

// Remove filled orders in O(n) single pass and mirror the in-loop wipe
// predicate: only stale entries that were ADDED to the just-closed
// position (created_position_side matches the closed direction) get
// cleaned out. Opposite-direction-prep stops armed during a previous
// position cycle survive (probe 93).
void BacktestEngine::compact_filled_pending_orders(
        const std::vector<size_t>& filled_indices,
        int exit_closed_from_bar,
        bool exit_closed_was_long) {
    if (filled_indices.empty()) return;
    // filled_indices is built by push_back(i) with i strictly increasing over
    // the inner fill loop (at most one push per iteration), so it is already
    // sorted ascending with no duplicates. A binary search over the vector
    // replaces a per-call hash-table build for the membership test below.
    auto is_filled = [&](size_t idx) {
        return std::binary_search(filled_indices.begin(), filled_indices.end(), idx);
    };
    PositionSide closed_side =
        exit_closed_was_long ? PositionSide::LONG : PositionSide::SHORT;
    size_t write = 0;
    for (size_t read = 0; read < pending_orders_.size(); ++read) {
        bool stale_same_direction_entry_after_exit =
            exit_closed_from_bar >= 0
            && (pending_orders_[read].type == OrderType::ENTRY
                || pending_orders_[read].type == OrderType::MARKET)
            && pending_orders_[read].is_long == exit_closed_was_long
            && pending_orders_[read].created_position_side == closed_side;
        if (!is_filled(read)
            && !stale_same_direction_entry_after_exit) {
            if (write != read) pending_orders_[write] = std::move(pending_orders_[read]);
            ++write;
        }
    }
    pending_orders_.resize(write);
}


// Apply a successfully matched fill to engine state. Dispatches by
// order.type to the appropriate execute_* method, updates trailing-stop
// best price, handles risk gating + intraday-fill caps + OCA group
// cancellation, and tracks the same-direction-after-exit cleanup that
// the post-loop compaction needs to mirror.
void BacktestEngine::apply_filled_order_to_state(
        PendingOrder& order,
        size_t order_index,
        double fill_price,
        const Bar& bar,
        double& trail_best_path_state,
        int& exit_closed_from_bar,
        bool& exit_closed_was_long,
        std::vector<size_t>& filled_indices) {
    if (order.type == OrderType::MARKET || order.type == OrderType::ENTRY) {
        PositionSide requested = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        bool is_opposite_entry =
            position_side_ != PositionSide::FLAT && position_side_ != requested;
        if (!is_opposite_entry && !check_risk_allow_entry(order.is_long)) {
            filled_indices.push_back(order_index);
            return;
        }
    }

    // Check max_intraday_filled_orders limit.
    //
    // TV's broker emulator (LATCH-TILL-DAY-ROLLOVER semantics):
    //   1. Track fills on the current chart-day. When the Nth fill
    //      (== max_intraday_filled_orders) lands and the resulting
    //      position is non-flat, TV synthesises a full close at the
    //      SAME BAR / SAME FILL PRICE tagged
    //      "Close Position (Max number of filled orders in one day)".
    //   2. After the synthetic close fires, a LATCH (intraday_cap_hit_)
    //      is set. ALL subsequent fills on that chart-day are silently
    //      rejected — TV emits at most one cap-close per chart-day.
    //   3. The latch (and the counter) reset only at chart-day rollover.
    //
    // Verified empirically against validation probe 97b's tv_trades.csv:
    //   - 382 cap-close exits across 13 months of data (~one per
    //     chart-day where the cap fires). NOT multiple per day.
    //   - cap-trigger entry + synthetic close share the same timestamp
    //     and price (close trade carries pnl == 0)
    //
    // Two prior bugs:
    //   - First impl just early-returned when the cap was hit, leaving
    //     the position carried open across day boundaries (382 cap-
    //     close exits in TV, 0 in engine).
    //   - Second impl recharged the counter after each cap-cycle so
    //     multiple cap-closes fired per chart-day (3459 engine vs
    //     1957 TV trades on 97b — 43% over-count).
    bool will_trigger_cap = false;
    if (max_intraday_filled_orders_ > 0) {
        BarTime bt = _decompose_bar_time_chart_tz();
        int cur_day = bt.dayofmonth * 100 + bt.month;
        if (cur_day != intraday_day_) {
            intraday_day_ = cur_day;
            intraday_fill_count_ = 0;
            intraday_cap_hit_ = false;  // RESET LATCH on chart-day rollover
        }
        if (intraday_cap_hit_) {
            // Latched: drop this pending order and skip dispatch.
            // Removing from pending_orders_ matches TV's behaviour of
            // silently consuming/rejecting fills past the daily cap.
            filled_indices.push_back(order_index);
            return;
        }
        intraday_fill_count_++;
        will_trigger_cap =
            (intraday_fill_count_ >= max_intraday_filled_orders_);
    }

    filled_indices.push_back(order_index);

    // Track trades before fill to set exit_comment/exit_id on new trades
    size_t trades_before = trades_.size();

    // Snapshot signed position before the fill so we can compute the
    // filled qty for OCA-reduce semantics. Long = +qty, Short = -qty.
    auto signed_pos = [&]() {
        if (position_side_ == PositionSide::LONG)  return  position_qty_;
        if (position_side_ == PositionSide::SHORT) return -position_qty_;
        return 0.0;
    };
    double signed_pos_before = signed_pos();

    // Priced (stop/limit) fills happen mid-bar: any trade they close must
    // fold the pre-fill portion of the bar's path into its excursion
    // (emit_close_trade reads this flag). Market fills land at the bar
    // boundary (open / close) where the boundary sampling already covers
    // the trade's bars, so the flag stays false for them.
    fold_exit_path_extremes_ =
        !std::isnan(order.stop_price) || !std::isnan(order.limit_price)
        || !std::isnan(order.trail_points) || !std::isnan(order.trail_offset);
    if (last_exit_fill_was_trail_) {
        // TRAIL fills retrace exactly trail_offset from the armed peak, so
        // peak = fill +/- offset — a pre-fill favorable excursion of the
        // closing trade that no bar-boundary sample ever sees.
        double off = std::isnan(order.trail_offset)
                         ? 0.0
                         : std::ceil(order.trail_offset) * syminfo_mintick_;
        fold_exit_trail_peak_ = (position_side_ == PositionSide::LONG)
                                    ? fill_price + off
                                    : fill_price - off;
    }
    if (order.type == OrderType::MARKET) {
        apply_market_order_fill(order, fill_price, bar, trail_best_path_state);
    } else if (order.type == OrderType::ENTRY) {
        apply_entry_order_fill(order, fill_price, bar, trail_best_path_state);
    } else if (order.type == OrderType::EXIT) {
        apply_exit_order_fill(order, fill_price, exit_closed_from_bar, exit_closed_was_long);
    } else if (order.type == OrderType::RAW_ORDER) {
        apply_raw_order_fill(order, fill_price, trail_best_path_state,
                             exit_closed_from_bar, exit_closed_was_long);
    }
    fold_exit_path_extremes_ = false;
    fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();

    double signed_pos_after = signed_pos();
    double filled_qty = std::abs(signed_pos_after - signed_pos_before);

    if (position_side_ == PositionSide::FLAT) {
        trail_best_path_state = trail_best_price_;
    }

    // Set exit_comment and exit_id on any trades created by this fill
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = order.comment;
        trades_[ti].exit_id = order.id;
    }

    // Handle OCA groups: cancel (type 1) cancels all siblings; reduce
    // (type 2, Pine v6 strategy.oca.reduce) reduces siblings' remaining
    // qty by the qty just filled — only siblings whose qty drops to 0
    // are removed. See TradingView Pine v6 docs strategy.oca.reduce.
    //
    // OCA-cancel full-fill gate (validation_oca/oca-three-way-probe-02):
    // TV cancels CANCEL-group siblings only after the originating order
    // is FULLY filled, not after the first contract fills. With qty=4
    // long + qty=2 sibling A_TP: A_TP fills qty=2, position=2 remaining,
    // A_SL stays alive until the second sibling fires. We compare the
    // qty actually transacted (``filled_qty``) against the order's
    // explicit qty. If the request was default-sized (qty == NaN), we
    // can't compute a residual so we conservatively cancel siblings on
    // any fill (matches the prior, blanket-cancel behaviour for that
    // subset). The OCA group name scoping inside cancel_oca_group /
    // reduce_oca_group already isolates groups from each other.
    if (!order.oca_name.empty()) {
        bool fully_filled = std::isnan(order.qty)
            || filled_qty + kOcaQtyEpsilon >= order.qty;
        if (order.oca_type == 1 && fully_filled) {
            cancel_oca_group(order.oca_name, order.id);
        } else if (order.oca_type == 2) {
            reduce_oca_group(order.oca_name, order.id, filled_qty);
        }
    }
    // When an exit fill causes position to go flat, subsequent EXIT
    // orders in this iteration are naturally skipped by the flat guard
    // earlier in the inner loop body.

    // max_intraday_filled_orders auto-close: if this fill was the
    // cap-triggering one and the position is still non-flat after
    // dispatch (entries leave a position open; exits that flatten
    // already need no synthetic close), emit TV's synthetic
    // "Close Position (Max number of filled orders in one day)" exit at
    // the same fill price, then LATCH so all subsequent fills on this
    // chart-day are silently rejected. TV emits at most one cap-close
    // per chart-day (probe 97b: 382 cap-closes across 13 months,
    // ~one per chart-day where the cap fires). The latch is reset
    // only on chart-day rollover (see top of this function).
    if (will_trigger_cap) {
        if (position_side_ != PositionSide::FLAT) {
            // TV cap-close exit price empirics (probe 97 stop-entry +
            // cap composition):
            //
            //   When the cap-triggering fill is a STOP entry that fired
            //   INTRA-bar (stop > bar.open for long, stop < bar.open
            //   for short), TV's synthetic "Close Position (Max number
            //   of filled orders in one day)" exit emits at the bar's
            //   FAVORABLE extreme — bar.high for a long, bar.low for a
            //   short — not at the entry's stop trigger price. The
            //   model: TV's broker traces the bar path past the stop
            //   trigger to the next extreme (continuation through the
            //   stop direction is the "worst case" assumption Pine uses
            //   for path resolution), and the cap-close fires at that
            //   reached extreme. Verified against 152 cap-close trades
            //   in probe 97: long stop-entry fills with stop > open
            //   close at bar.high; short stop-entry fills with
            //   stop < open close at bar.low.
            //
            //   When the entry filled AT bar.open (gap-fill: long stop
            //   <= open, short stop >= open, or a market entry — no
            //   intra-bar travel was needed to reach the trigger), TV's
            //   cap-close emits at fill_price = bar.open. Probe 97b
            //   (market entries only, no stops) confirms 382/382 cap-
            //   closes at fill_price = entry_price = bar.open.
            //
            //   This ONLY applies to ENTRY/MARKET fills that opened the
            //   position. Other fill types (RAW_ORDER bracket exits,
            //   EXIT close-deferred orders) reach this path only when
            //   they themselves flatten — but a flatten leaves
            //   position_side_ FLAT, so the outer guard already skips
            //   the synthetic close emit. So we only need the bar-
            //   extreme adjustment for the entry-fill cases.
            double cap_close_price = fill_price;
            const bool entry_kind = (order.type == OrderType::ENTRY ||
                                     order.type == OrderType::MARKET);
            if (entry_kind) {
                if (position_side_ == PositionSide::LONG && fill_price > bar.open) {
                    cap_close_price = bar.high;
                } else if (position_side_ == PositionSide::SHORT && fill_price < bar.open) {
                    cap_close_price = bar.low;
                }
            }
            size_t close_trades_before = trades_.size();
            execute_market_exit(cap_close_price);
            for (size_t ti = close_trades_before; ti < trades_.size(); ++ti) {
                trades_[ti].exit_comment =
                    "Close Position (Max number of filled orders in one day)";
                trades_[ti].exit_id = "";
            }
        }
        intraday_cap_hit_ = true;  // latch — block further fills until day rollover
    }
}


// ── Per-OrderType fill kernels (called from apply_filled_order_to_state) ──

// Compute the pre-fill excursion masks for a priced (stop/limit) entry that
// filled intrabar: on the assumed OHLC path, an extreme that occurs BEFORE
// the fill position is not part of the new trade's excursion, so
// update_per_trade_extremes must skip it on the fill bar (TV convention —
// TV starts excursion tracking at the fill, the engine otherwise samples
// the full bar range including the pre-fill leg). Open/gap fills resolve to
// path position 0 and leave both masks false.
static void set_entry_fill_excursion_masks(PyramidEntry& pe, const Bar& bar,
                                           double fill_price) {
    double fill_pos = 0.0;
    if (!internal::first_touch_position(bar, fill_price, &fill_pos)) return;
    const bool high_first = internal::bar_path_uses_high_first(bar);
    const double high_pos = high_first ? 1.0 : 2.0;
    const double low_pos  = high_first ? 2.0 : 1.0;
    pe.skip_entry_bar_high = (high_pos < fill_pos);
    pe.skip_entry_bar_low  = (low_pos < fill_pos);
}

void BacktestEngine::apply_market_order_fill(PendingOrder& order, double fill_price,
                                             const Bar& bar,
                                             double& trail_best_path_state) {
    execute_market_entry(order.id, order.is_long, fill_price, order.qty, order.qty_type,
                         order.created_position_side, /*close_only_opposite=*/false,
                         /*is_priced_entry=*/false, /*tv_carry_qty=*/0.0,
                         order.created_bar);
    double trail_best_after_fill = trail_best_price_;
    // Set entry comment on the just-created pyramid entry
    if (!pyramid_entries_.empty()) pyramid_entries_.back().entry_comment = order.comment;
    // Update trail_best_price_ with intra-bar extremes for same-bar exit eval
    if (position_side_ == PositionSide::LONG)
        trail_best_price_ = std::max(trail_best_price_, bar.high);
    else if (position_side_ == PositionSide::SHORT)
        trail_best_price_ = std::min(trail_best_price_, bar.low);
    trail_best_path_state = trail_best_after_fill;
}

void BacktestEngine::apply_entry_order_fill(PendingOrder& order, double fill_price,
                                            const Bar& bar,
                                            double& trail_best_path_state) {
    PositionSide side_before = position_side_;
    double qty_before = position_qty_;
    int count_before = position_entry_count_;
    size_t trades_before_entry = trades_.size();

    // Flat-issued pending ENTRY stops/limits act as a bracket: when one
    // side opens the position, a touch of the opposite pending ENTRY
    // (still flat-issued) closes at the touch price (TradingView's
    // List of trades shows an exit tied to the opposite bracket),
    // not a full reversal-and-new-position. The bracket persists
    // across bars: probes 80-87 confirm TV closes the position on a
    // later bar when only one leg fired earlier and the opposite
    // leg's stop is touched subsequently.
    PositionSide entry_req = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
    bool close_only_opposite =
        order.created_position_side == PositionSide::FLAT
        && position_side_ != PositionSide::FLAT
        && entry_req != position_side_;
    execute_market_entry(order.id, order.is_long, fill_price, order.qty, order.qty_type,
                         order.created_position_side, close_only_opposite,
                         /*is_priced_entry=*/true,
                         order.tv_carry_qty,
                         order.created_bar);

    bool did_execute =
        (position_side_ != side_before)
        || (std::abs(position_qty_ - qty_before) > 1e-12)
        || (position_entry_count_ != count_before)
        || (trades_.size() != trades_before_entry);

    bool was_priced_entry = !std::isnan(order.stop_price) || !std::isnan(order.limit_price);
    if (did_execute) {
        double trail_best_after_fill = trail_best_price_;
        if (!pyramid_entries_.empty()) pyramid_entries_.back().entry_comment = order.comment;
        if (position_side_ == PositionSide::LONG)
            trail_best_price_ = std::max(trail_best_price_, bar.high);
        else if (position_side_ == PositionSide::SHORT)
            trail_best_price_ = std::min(trail_best_price_, bar.low);
        if (was_priced_entry) {
            priced_entry_filled_this_bar_ = true;
            // Mask pre-fill bar extremes for the entry this fill created
            // (guard: back() really is this order's same-bar entry — a
            // close-only-opposite fill creates no new entry).
            if (!pyramid_entries_.empty()
                && pyramid_entries_.back().entry_bar_index == bar_index_
                && pyramid_entries_.back().entry_id == order.id) {
                set_entry_fill_excursion_masks(pyramid_entries_.back(), bar,
                                               pyramid_entries_.back().price);
            }
        }
        trail_best_path_state = trail_best_after_fill;
    }
}

void BacktestEngine::apply_exit_order_fill(PendingOrder& order, double fill_price,
                                           int& exit_closed_from_bar,
                                           bool& exit_closed_was_long) {
    double qp = std::isnan(order.qty_percent) ? 100.0 : std::clamp(order.qty_percent, 0.0, 100.0);
    bool has_explicit_qty_to_close = !std::isnan(order.qty);
    double qty_before_exit = position_qty_;
    bool is_partial = has_explicit_qty_to_close
        ? order.qty < qty_before_exit - 1e-9
        : qp < 100.0 - kFullPercentEps;
    size_t trades_before_exit = trades_.size();
    PositionSide side_before_exit = position_side_;

    if (close_entries_rule_any_ && !order.from_entry.empty()) {
        // close_entries_rule="ANY": close only matching entries
        if (is_partial) {
            execute_partial_exit_by_entry_percent(fill_price, order.from_entry, qp);
        } else {
            execute_partial_exit_by_entry(fill_price, order.from_entry);
        }
    } else {
        if (has_explicit_qty_to_close) {
            execute_partial_exit_qty(fill_price, order.qty);
        } else if (is_partial) {
            execute_partial_exit(fill_price, qp);
        } else {
            execute_market_exit(fill_price);
        }
    }

    if (order.requested_partial && trades_.size() > trades_before_exit) {
        consumed_partial_exit_ids_.insert(order.id);
    }

    // Full exit that closed the position: pending SAME-direction entries
    // placed on a different on_bar are cancelled for the rest of this
    // bar (TV's same-direction cancellation rule).
    if (!is_partial && position_side_ == PositionSide::FLAT) {
        exit_closed_from_bar = order.created_bar;
        exit_closed_was_long = (side_before_exit == PositionSide::LONG);
    }
}

void BacktestEngine::apply_raw_order_fill(PendingOrder& order, double fill_price,
                                          double& trail_best_path_state,
                                          int& exit_closed_from_bar,
                                          bool& exit_closed_was_long) {
    if (position_side_ == PositionSide::FLAT) {
        fill_price = apply_slippage(fill_price, order.is_long);
        double qty = std::isnan(order.qty) ? calc_qty(fill_price) : order.qty;
        position_side_ = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        position_entry_price_ = fill_price;
        position_entry_time_ = current_bar_.timestamp;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = bar_index_;
        trail_best_price_ = fill_price;
        pyramid_entries_.clear();
        pyramid_entries_.push_back({fill_price, current_bar_.timestamp, qty, order.id, bar_index_});
        if (!std::isnan(order.stop_price) || !std::isnan(order.limit_price)) {
            set_entry_fill_excursion_masks(pyramid_entries_.back(), current_bar_, fill_price);
        }
        trail_best_path_state = trail_best_price_;
    } else {
        PositionSide side_before_raw = position_side_;
        PositionSide requested = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (position_side_ == requested) {
            // Same-direction RAW_ORDER fill = pyramid-add. Most commonly,
            // this fires when an OCA-reduce bracket placed during a PRIOR
            // opposite-direction position survives a same-bar flip and
            // gap-fills at the next bar's open as a leftover same-direction
            // entry. TV's broker emulator gap-fills these as a real
            // pyramid-add; previously we silently dropped them.
            //
            // Probe 97a reference: short→long MA-cross flip leaves the
            // pre-existing buy-stop bracket alive; the bracket's
            // ``created_position_side`` is SHORT but the live position is
            // now LONG — the ``pre_armed_opposite_priced`` semantic in
            // ``add_to_pyramid_market`` admits the add even when the
            // pyramiding limit would otherwise reject it.
            //
            // We mirror that semantic here directly (rather than calling
            // ``add_to_pyramid_market``) because the strategy.order path
            // does not carry an explicit qty_type and lacks the
            // execute_market_entry preamble (carry consumption, risk
            // gating, etc.) that the high-level helper assumes.
            bool is_priced_entry = !std::isnan(order.limit_price)
                                   || !std::isnan(order.stop_price);
            bool flat_armed_priced =
                is_priced_entry && order.created_position_side == PositionSide::FLAT;
            bool pre_armed_opposite_priced =
                is_priced_entry
                && order.created_position_side != PositionSide::FLAT
                && order.created_position_side != requested;
            if (!flat_armed_priced && !pre_armed_opposite_priced
                && position_entry_count_ >= pyramiding_) {
                return;
            }
            fill_price = apply_slippage(fill_price, order.is_long);
            double new_qty = std::isnan(order.qty) ? calc_qty(fill_price) : order.qty;
            double total_qty = position_qty_ + new_qty;
            position_entry_price_ =
                (position_entry_price_ * position_qty_ + fill_price * new_qty) / total_qty;
            position_qty_ = total_qty;
            position_entry_count_++;
            trail_best_price_ = fill_price;
            pyramid_entries_.push_back({fill_price, current_bar_.timestamp, new_qty, order.id, bar_index_});
            if (is_priced_entry) {
                set_entry_fill_excursion_masks(pyramid_entries_.back(), current_bar_, fill_price);
            }
        } else {
            execute_market_exit(fill_price);
            if (position_side_ == PositionSide::FLAT) {
                exit_closed_from_bar = order.created_bar;
                exit_closed_was_long = (side_before_raw == PositionSide::LONG);
            }
        }
    }
}


// ── Inner-loop phase 1: order eligibility ─────────────────────────────
// Returns whether the given pending order should be processed this
// iteration. Walks the chain of TV-empirical "skip" / "cancel" rules
// in source order; the first rule to fire dictates the verdict.
BacktestEngine::OrderEligibility BacktestEngine::classify_order_eligibility(
        PendingOrder& order, int opposing_pass,
        internal::DualEntryStopPathWinner dual_entry_path,
        const std::unordered_set<std::string>& pass0_opposing_skip_ids,
        int exit_closed_from_bar, bool exit_closed_was_long) {
    using internal::DualEntryStopPathWinner;
    if (opposing_pass == 1) {
        if (!pass0_opposing_skip_ids.count(order.id)) {
            return OrderEligibility::Skip;
        }
        // Pass 0 deferred this leg as the path loser. TradingView only applies
        // a same-bar second touch as a bracket exit when the buy-stop leads on
        // the path; if the sell-stop leads, the later buy touch is discarded.
        if (dual_entry_path == DualEntryStopPathWinner::ShortFirst && order.is_long) {
            return OrderEligibility::Remove;
        }
        if (!(dual_entry_path == DualEntryStopPathWinner::LongFirst && !order.is_long)) {
            if (dual_entry_path != DualEntryStopPathWinner::None
                && dual_entry_path != DualEntryStopPathWinner::Tie) {
                return OrderEligibility::Remove;
            }
        }
    }

    bool exit_style = order_is_exit_style(order, position_side_);

    bool stale_close_order_for_new_position =
        order.type == OrderType::EXIT
        && order.created_while_in_position
        && order.id.rfind("__close__", 0) == 0
        && position_side_ != PositionSide::FLAT
        && position_open_bar_ > order.created_bar;
    if (stale_close_order_for_new_position) {
        return OrderEligibility::Remove;
    }

    // When flat, cancel stale exit orders that were created while a position
    // was open. This prevents old strategy.exit brackets from leaking into
    // future positions after a market close/reversal.
    if (order.type == OrderType::EXIT && position_side_ == PositionSide::FLAT) {
        return order.created_while_in_position
            ? OrderEligibility::Remove
            : OrderEligibility::Skip;
    }

    // TradingView throttles priced (stop/limit) entry fills to one per bar,
    // EXCEPT for flat-issued priced entries that resolve a bracket pair on
    // the same bar (close the side just opened), pyramid an existing
    // position with another flat-armed leg (probe 80 has the morning short
    // stop firing on the same bar as the afternoon short stop, both
    // flat-issued), or pre-armed-opposite siblings whose carry-source
    // position has since closed (probe 72/93: S placed during L and S2
    // placed during L2 both fire on the same bar when their stops are
    // touched together — TV emits both as separate trades).
    if (priced_entry_filled_this_bar_ && order.type == OrderType::ENTRY) {
        PositionSide requested = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        bool flat_armed = order.created_position_side == PositionSide::FLAT
                          && position_side_ != PositionSide::FLAT;
        bool flat_armed_opposite_close = flat_armed
            && position_side_ != requested
            && position_open_bar_ == bar_index_;
        bool flat_armed_same_dir_pyramid = flat_armed
            && position_side_ == requested;
        bool pre_armed_opposite_sibling =
            order.created_position_side != PositionSide::FLAT
            && order.created_position_side != requested;
        if (!flat_armed_opposite_close && !flat_armed_same_dir_pyramid
            && !pre_armed_opposite_sibling) {
            return OrderEligibility::Skip;
        }
    }

    // Cancel pending SAME-DIRECTION entry orders placed on a prior on_bar
    // when a full strategy.exit has fired on this bar. Opposite-direction
    // entries (reversal via stop/limit-then-new-signal) still fire.
    // Restrict the wipe to entries actually ADDED to the just-closed
    // position (created_position_side matches the closed direction).
    PositionSide closed_side =
        exit_closed_was_long ? PositionSide::LONG : PositionSide::SHORT;
    if (exit_closed_from_bar >= 0
        && (order.type == OrderType::MARKET || order.type == OrderType::ENTRY)
        && order.is_long == exit_closed_was_long
        && order.created_position_side == closed_side) {
        return OrderEligibility::Remove;
    }

    // With process_orders_on_close, ALL priced orders (stop/limit/trail)
    // placed this bar should only be evaluated from the next bar.
    if (process_orders_on_close_ && order.created_bar == bar_index_) {
        bool has_price_condition = !std::isnan(order.stop_price)
                                   || !std::isnan(order.limit_price)
                                   || !std::isnan(order.trail_points);
        if (has_price_condition) {
            return OrderEligibility::Skip;
        }
    }

    // Skip exit orders whose from_entry doesn't match any active entry id.
    if (order.type == OrderType::EXIT && !order.from_entry.empty()) {
        bool has_match = false;
        for (const auto& pe : pyramid_entries_) {
            if (pe.entry_id == order.from_entry) {
                has_match = true;
                break;
            }
        }
        if (!has_match) {
            // Cancel stale from_entry-bound exits so they cannot fire later
            // against future positions with the same id.
            return OrderEligibility::Remove;
        }
    }

    // Same-bar exit handling: TradingView evaluates priced exits (stop/limit/
    // trail) on the entry bar itself (entry fills at open, then intra-bar
    // data evaluates exits). But skip if the exit has garbage values
    // (computed when position was flat).
    //
    // The wrong-side eligibility skip (stop > entry for long, etc.) gates
    // out spurious orders whose stop/limit was derived from
    // ``strategy.position_avg_price`` while flat: Pine returns ``na`` and
    // arithmetic propagates ``na`` (so the order becomes a no-op), but the
    // engine returns ``0.0`` and arithmetic produces a numerically-valid
    // but semantically-wrong-side level (negative or near-zero). Without
    // the skip those orders gap-fill at the entry bar's open (every bar a
    // signal fires would close at $0 PnL).
    //
    // The magnifier corpus (probe-01..08b) places exits with USER-COMPUTED
    // valid wrong-side stops (e.g. ``open + (high-open)*0.5`` is between
    // open and high, then becomes wrong-side once the next bar's open lands
    // below it). TV's broker emulator fires these at the entry bar's open
    // because each magnifier sub-bar opens fresh and triggers the gap
    // predicate. The bypass below lets bar_magnifier_enabled_ runs fall
    // through to resolve_exit_path_fill / try_exit_open_gap_fill (now also
    // active on entry bars in magnifier mode) so legitimate wrong-side
    // exits fire at entry price as TV reports them.
    bool is_entry_bar = (exit_style && position_open_bar_ == bar_index_);
    if (is_entry_bar) {
        bool has_price = !std::isnan(order.stop_price) || !std::isnan(order.limit_price)
                         || !std::isnan(order.trail_points);
        if (!has_price) {
            return OrderEligibility::Skip;  // skip market exits on entry bar
        }
        if (!bar_magnifier_enabled_) {
            double ep = position_entry_price_;
            if (position_side_ == PositionSide::LONG) {
                if (!std::isnan(order.stop_price) && order.stop_price > ep) return OrderEligibility::Skip;
                if (!std::isnan(order.limit_price) && order.limit_price < ep) return OrderEligibility::Skip;
            } else if (position_side_ == PositionSide::SHORT) {
                if (!std::isnan(order.stop_price) && order.stop_price < ep) return OrderEligibility::Skip;
                if (!std::isnan(order.limit_price) && order.limit_price > ep) return OrderEligibility::Skip;
            }
        }
    }

    return OrderEligibility::Proceed;
}

// ── Inner-loop phase 2: fill-price evaluation ─────────────────────────
// Computes the fill price (if any) for an eligible order. May insert
// into pass0_opposing_skip_ids when an opposing entry-stop is touched
// first on the path; the inner loop's second pass picks it up.
BacktestEngine::FillEvaluation BacktestEngine::evaluate_fill_price(
        PendingOrder& order, size_t order_index, const Bar& bar,
        int opposing_pass, double trail_best_path_state,
        std::unordered_set<std::string>& pass0_opposing_skip_ids) {
    bool exit_style = order_is_exit_style(order, position_side_);
    bool has_stop = !std::isnan(order.stop_price);
    bool has_limit = !std::isnan(order.limit_price);
    bool has_trail = !std::isnan(order.trail_points);

    last_exit_fill_was_trail_ = false;

    if (order.type == OrderType::RAW_ORDER && exit_style
        && oca_exit_sibling_hits_first(bar, pending_orders_, order_index, position_side_)) {
        return {FillEvaluation::Kind::NoFill, 0.0};
    }

    bool is_entry_bar = (exit_style && position_open_bar_ == bar_index_);
    double fill_price = 0.0;
    bool should_fill = false;

    if (exit_style && (has_stop || has_limit || has_trail)) {
        ExitPathFill exit_fill = resolve_exit_path_fill(
            bar,
            position_side_,
            order.stop_price,
            order.limit_price,
            order.trail_points,
            order.trail_offset,
            position_entry_price_,
            trail_best_path_state,
            is_entry_bar,
            bar_magnifier_enabled_,
            syminfo_mintick_);
        if (exit_fill.should_fill) {
            fill_price = exit_fill.fill_price;
            should_fill = true;
            last_exit_fill_was_trail_ = exit_fill.is_trail;
        }
    } else if (order.type == OrderType::MARKET ||
               (!has_stop && !has_limit && !has_trail)) {
        fill_price = process_orders_on_close_ ? bar.close : bar.open;
        should_fill = true;
    } else if (has_stop && has_limit) {
        // Entry stop-limit semantics: the stop activates the limit order,
        // and the limit can only fill after activation along the OHLC path.
        should_fill = resolve_entry_stop_limit_fill(
            bar,
            order.is_long,
            order.stop_price,
            order.limit_price,
            &fill_price);
    } else if (has_stop) {
        // Entry stop order
        if (position_side_ == PositionSide::FLAT && opposing_pass == 0 &&
            opposing_stop_entry_hits_first(bar, pending_orders_, order_index)) {
            pass0_opposing_skip_ids.insert(order.id);
            return {FillEvaluation::Kind::DeferredToOpposingPass, 0.0};
        }
        if (order.is_long) {
            if (bar.high >= order.stop_price) {
                fill_price = std::max(bar.open, order.stop_price);
                // TradingView snaps stop entry fills to mintick in the
                // conservative direction (long stop -> ceil).
                if (fill_price > bar.open) {
                    fill_price = round_to_mintick_directional(fill_price, true);
                }
                should_fill = true;
            }
        } else {
            if (bar.low <= order.stop_price) {
                fill_price = std::min(bar.open, order.stop_price);
                if (fill_price < bar.open) {
                    fill_price = round_to_mintick_directional(fill_price, false);
                }
                should_fill = true;
            }
        }
    } else if (has_limit) {
        // Entry limit order
        if (order.is_long) {
            if (bar.low <= order.limit_price) {
                fill_price = std::min(bar.open, order.limit_price);
                should_fill = true;
            }
        } else {
            if (bar.high >= order.limit_price) {
                fill_price = std::max(bar.open, order.limit_price);
                should_fill = true;
            }
        }
    }

    return {should_fill ? FillEvaluation::Kind::Fill : FillEvaluation::Kind::NoFill,
            fill_price};
}

}  // namespace pineforge
