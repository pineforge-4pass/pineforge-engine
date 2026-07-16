/*
 * engine_run.cpp — public run() entrypoints + run_magnified_bar + get_input_*
 */

#include "engine_internal.hpp"

#include <pineforge/ta.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;


// open_trade_* accessors moved to engine_trade_accessors.cpp.

// Invoke the generated chart strategy body under its own EMA warmup mode.
// The selector is thread-local because multiple engines can run concurrently;
// restoring the previous value (also during stack unwinding) prevents both
// cross-engine contamination and leakage between chart and request.security
// evaluation. The latter installs its own scope around every security
// evaluator dispatch and restores the prior thread-local value on return.
void BacktestEngine::invoke_chart_on_bar(const Bar& bar) {
    struct ChartEmaNaWarmupScope {
        bool previous;
        explicit ChartEmaNaWarmupScope(bool enabled)
            : previous(ta::ema_na_warmup_flag()) {
            ta::ema_na_warmup_flag() = enabled;
        }
        ~ChartEmaNaWarmupScope() {
            ta::ema_na_warmup_flag() = previous;
        }
    } scope(chart_ema_na_warmup_);

    on_bar(bar);
}

// Standard per-script-bar dispatch sequence, shared by the simple run() loop,
// run_simple_bar_loop, and the no-magnifier aggregation path. Operates on
// current_bar_ (already set by the caller).
//
// TradingView process_orders_on_close semantics:
//   1. Evaluate existing stop/limit orders from previous bars
//   2. Update per-trade extremes so on_bar reads current values
//   3. Strategy logic runs at bar close (creates new orders)
//   4. New market orders fill at bar.close; new stop/limit wait for next bar
// When process_orders_on_close_ is false, only steps 1-3 run.
void BacktestEngine::dispatch_bar() {
    if (calc_on_order_fills_) {
        dispatch_bar_calc_on_order_fills();
        return;
    }

    // A C-factor inheritance is same-ordinary-bar state. A candidate erased
    // by replacement/OCA/cancel never reaches the fill kernel, so discard any
    // stale identity before starting the next broker batch.
    intraday_cap_pooc_close_inheritor_incarnation_ = 0;

    // Opt-in POOC intraday-cap candidate: the cap-triggering MARKET entry
    // filled at the prior signal close, while the broker-generated flatten is
    // due at this next broker boundary.  Run it before resting orders and
    // before the current bar's path is sampled so the exit is exactly at open.
    if (intraday_cap_deferred_close_pending_) {
        intraday_cap_deferred_close_pending_ = false;
        if (position_side_ != PositionSide::FLAT) {
            const size_t trades_before = trades_.size();
            const PositionSide side_before = position_side_;
            const double qty_before = position_qty_;
            execute_market_exit(current_bar_.open);
            if (position_side_ != side_before
                || std::abs(position_qty_ - qty_before) > kQtyEpsilon
                || trades_.size() != trades_before) {
                ++broker_fill_event_seq_;
            }
            for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
                trades_[ti].exit_comment =
                    "Close Position (Max number of filled orders in one day)";
                trades_[ti].exit_id = "";
            }
        }
    }

    // Advance native source-series history before strategy logic so
    // get_input_source()'s returned series is current for this bar. Covers
    // the simple run() loop, run_simple_bar_loop, and the no-magnifier
    // aggregation path (all route through dispatch_bar). The magnifier path
    // inlines its own on_bar call and pushes there instead.
    _push_source_series();
    if (process_orders_on_close_) {
        process_pending_orders(current_bar_);   // step 1: old stop/limit
        update_per_trade_extremes();             // step 2: update before strategy reads
        invoke_chart_on_bar(current_bar_);       // step 3: strategy logic
        flush_same_bar_close();                  // step 3b: surviving strategy.close fill
        process_pending_orders(current_bar_);    // step 4: new market orders
        intraday_cap_pooc_close_inheritor_incarnation_ = 0;
    } else {
        process_pending_orders(current_bar_);
        update_per_trade_extremes();
        invoke_chart_on_bar(current_bar_);
    }
    // TradingView forced-liquidation check, once per script bar after all order
    // processing, using this bar's full adverse extreme (high/low).
    //
    // TV liquidates INTRABAR — before the close-time script body — so any
    // default-sized market order frozen by this bar's on_bar was sized on
    // pre-liquidation equity. When (and only when) the margin call actually
    // liquidated something, re-freeze those orders on the post-liquidation
    // state (see refresh_frozen_default_sizing_after_margin_call).
    {
        size_t trades_before_mc = trades_.size();
        process_margin_call(current_bar_);
        if (trades_.size() != trades_before_mc) {
            refresh_frozen_default_sizing_after_margin_call();
        }
    }
}

void BacktestEngine::snapshot_coof_script_state() {
    if (_src_series_active_) {
        coof_checkpoint_src_open_ = _src_open_;
        coof_checkpoint_src_high_ = _src_high_;
        coof_checkpoint_src_low_ = _src_low_;
        coof_checkpoint_src_close_ = _src_close_;
        coof_checkpoint_src_volume_ = _src_volume_;
        coof_checkpoint_src_hl2_ = _src_hl2_;
        coof_checkpoint_src_hlc3_ = _src_hlc3_;
        coof_checkpoint_src_ohlc4_ = _src_ohlc4_;
        coof_checkpoint_src_hlcc4_ = _src_hlcc4_;
    }
    snapshot_script_state();
    coof_checkpoint_contains_current_bar_ = false;
}

void BacktestEngine::restore_coof_script_state() {
    if (_src_series_active_) {
        _src_open_ = coof_checkpoint_src_open_;
        _src_high_ = coof_checkpoint_src_high_;
        _src_low_ = coof_checkpoint_src_low_;
        _src_close_ = coof_checkpoint_src_close_;
        _src_volume_ = coof_checkpoint_src_volume_;
        _src_hl2_ = coof_checkpoint_src_hl2_;
        _src_hlc3_ = coof_checkpoint_src_hlc3_;
        _src_ohlc4_ = coof_checkpoint_src_ohlc4_;
        _src_hlcc4_ = coof_checkpoint_src_hlcc4_;
    }
    restore_script_state();
}

void BacktestEngine::commit_coof_script_state() {
    if (_src_series_active_) {
        coof_checkpoint_src_open_ = _src_open_;
        coof_checkpoint_src_high_ = _src_high_;
        coof_checkpoint_src_low_ = _src_low_;
        coof_checkpoint_src_close_ = _src_close_;
        coof_checkpoint_src_volume_ = _src_volume_;
        coof_checkpoint_src_hl2_ = _src_hl2_;
        coof_checkpoint_src_hlc3_ = _src_hlc3_;
        coof_checkpoint_src_ohlc4_ = _src_ohlc4_;
        coof_checkpoint_src_hlcc4_ = _src_hlcc4_;
    }
    commit_script_state();
    coof_checkpoint_contains_current_bar_ = true;
}

uint64_t BacktestEngine::execute_coof_script_body(
        const Bar& script_bar,
        double broker_cursor_price,
        bool is_fill_recalc,
        bool cursor_is_bar_close,
        bool recalc_at_bar_open,
        uint64_t direct_fill_event_budget) {
    restore_coof_script_state();
    current_bar_ = script_bar;
    // TradingView historical fill recalculations are both new and confirmed.
    // History advancement is a separate axis: after the completed ordinary
    // close execution has been committed, a post-C recalc recomputes that
    // current-bar slot instead of pushing a duplicate bar.
    is_first_tick_ = true;
    is_last_tick_ = true;
    history_slot_is_new_ = !coof_checkpoint_contains_current_bar_;
    pending_close_qty_in_bar_ = 0.0;
    pos_view_freeze_bar_ = -1;   // KI-64: recompute re-arms the freeze fresh
    _push_source_series();
    update_per_trade_extremes();

    coof_scheduler_active_ = true;
    coof_fill_recalc_active_ = is_fill_recalc;
    coof_cursor_is_bar_close_ = cursor_is_bar_close;
    // KI-67: only the first fill event at O owns "bar-open" provenance and
    // places standard orders. A later fill at the same O, like a fill at any
    // segment/extreme/close point, is mid-bar and places cascade orders.
    coof_recalc_at_bar_open_ = is_fill_recalc && recalc_at_bar_open;
    coof_cursor_price_ = broker_cursor_price;
    coof_direct_fill_events_remaining_ = direct_fill_event_budget;
    const uint64_t before = broker_fill_event_seq_;
    invoke_chart_on_bar(current_bar_);
    if (process_orders_on_close_) {
        // A same-bar close batch is a broker fill at the current monotonic
        // cursor. At the ordinary close execution that cursor is C; during a
        // fill recalc it is the fill point that triggered the execution.
        flush_same_bar_close();
    }
    coof_fill_recalc_active_ = false;
    coof_recalc_at_bar_open_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_direct_fill_events_remaining_ = 0;
    return broker_fill_event_seq_ - before;
}

uint64_t BacktestEngine::run_coof_recalc_chain(
        const Bar& script_bar,
        double broker_cursor_price,
        bool cursor_is_bar_close,
        bool recalc_at_bar_open,
        uint64_t triggering_events,
        uint64_t max_events,
        uint64_t events_already) {
    uint64_t total_events = triggering_events;
    uint64_t pending_recalcs = triggering_events;
    uint64_t handled = 0;
    while (pending_recalcs > 0 && events_already + handled < max_events) {
        --pending_recalcs;
        ++handled;
        const uint64_t used = events_already + total_events;
        const uint64_t direct_budget =
            used < max_events ? max_events - used : 0;
        // Only the first fill event at O owns bar-open provenance. A direct or
        // separately-dispatched later fill at that same O is a KI-67 cascade
        // recalc whose remaining path starts on leg 0 (O->W1).
        const bool first_open_fill_recalc =
            recalc_at_bar_open && events_already == 0 && handled == 1;
        coof_recalc_after_first_open_fill_ =
            recalc_at_bar_open && !first_open_fill_recalc;
        const uint64_t direct = execute_coof_script_body(
            script_bar, broker_cursor_price, /*is_fill_recalc=*/true,
            cursor_is_bar_close, first_open_fill_recalc,
            direct_budget);
        total_events += direct;
        pending_recalcs += direct;
    }
    return total_events;
}

namespace {

Bar coof_point_bar(const Bar& script_bar, double price) {
    Bar out = script_bar;
    out.open = price;
    out.high = price;
    out.low = price;
    out.close = price;
    return out;
}

Bar coof_segment_bar(const Bar& script_bar, double from, double to) {
    Bar out = script_bar;
    out.open = from;
    out.high = std::max(from, to);
    out.low = std::min(from, to);
    out.close = to;
    return out;
}

}  // namespace

void BacktestEngine::dispatch_bar_calc_on_order_fills() {
    const Bar script_bar = current_bar_;
    // KI-67: TradingView applies NO per-bar fill-event budget. The old fixed
    // cap of 4 produced the right ~2-cycle depth by accident but the wrong
    // reach (it exact-level-filled cascade brackets on the W2->C segment AND
    // truncated legitimate busy-bar resting-order fills). The natural depth cap
    // now comes from cascade eligibility: mid-bar cascade orders may fill only
    // at the two remaining extreme waypoints, so a bar terminates on its own.
    // kNoFillEventBudget disables the direct-fill deferral that the old
    // "budget == 0" test used; kCoofLoopGuard is a pure infinite-loop backstop
    // (never reached in correct operation — the monotonic waypoint advance plus
    // finite fillable-order set guarantee termination), NOT a semantic budget.
    constexpr uint64_t kNoFillEventBudget = std::numeric_limits<uint64_t>::max();
    constexpr int kCoofLoopGuard = 1 << 20;
    uint64_t fill_events = 0;
    int exit_closed_from_bar = -1;
    uint64_t exit_closed_from_incarnation = 0;
    bool exit_closed_was_long = false;

    snapshot_coof_script_state();
    coof_scheduler_active_ = true;
    coof_cursor_is_bar_close_ = false;
    coof_evaluating_path_segment_ = false;
    coof_at_extreme_waypoint_ = false;
    coof_hist_is_segment_ = false;
    coof_hist_path_index_ = -1;
    coof_cascade_recalc_leg_ = -1;
    coof_cascade_force_wp_gap_ = false;
    coof_recalc_after_first_open_fill_ = false;

    double path[4];
    fill_bar_path_points(script_bar, path);
    double cursor = path[0];
    int next_waypoint = 1;
    bool evaluate_current_point = true;

    auto consume_fill = [&](const CoofFillResult& fill,
                            bool cursor_is_close,
                            bool filled_at_bar_open_point) {
        const uint64_t before = fill_events;
        cursor = fill.fill_price;
        // The recalc chain receives O-point provenance, but only its first fill
        // event is classified as bar-open. A later fill at the same O is a
        // leg-0 cascade (PendingOrder::coof_born_mid_bar).
        fill_events += run_coof_recalc_chain(
            script_bar, cursor, cursor_is_close, filled_at_bar_open_point,
            fill.fill_events, kNoFillEventBudget, fill_events);
        // The carried order's open fill triggers one execution at O, and the
        // order born in that first execution may also fill at O. Every later
        // fill—including the first fill when it occurs inside a path segment—
        // advances monotonically toward the next historical waypoint.
        evaluate_current_point =
            filled_at_bar_open_point && before == 0 && fill_events == 1;
    };

    int loop_guard = 0;
    while (++loop_guard <= kCoofLoopGuard) {
        if (evaluate_current_point) {
            const bool cursor_is_close = next_waypoint >= 4;
            // Cascade orders fill only AT an extreme waypoint (W1 = next_waypoint
            // 2, W2 = next_waypoint 3); the O point (1) and the C point (>=4) do
            // not admit them.
            coof_at_extreme_waypoint_ =
                (next_waypoint == 2 || next_waypoint == 3);
            // KI-67 exit cascade: publish this POINT's path index (cursor ==
            // path[next_waypoint-1]) for the strategy.exit cascade gate.
            coof_hist_is_segment_ = false;
            coof_hist_path_index_ = next_waypoint - 1;
            const Bar point = coof_point_bar(script_bar, cursor);
            current_bar_ = point;
            CoofFillResult fill = process_next_pending_order(
                point, /*allow_market_orders=*/true,
                exit_closed_from_bar, exit_closed_from_incarnation,
                exit_closed_was_long);
            if (fill.filled) {
                // A fill at this POINT (cursor == path[next_waypoint-1]) puts the
                // in-flight leg at path[next_waypoint-1] -> path[next_waypoint],
                // i.e. leg (next_waypoint-1) — the leg the loop traverses next.
                coof_cascade_recalc_leg_ = next_waypoint - 1;
                consume_fill(
                    fill, cursor_is_close,
                    /*filled_at_bar_open_point=*/next_waypoint == 1);
                continue;
            }
            evaluate_current_point = false;
        }

        if (next_waypoint >= 4) break;

        const double target = path[next_waypoint];
        const Bar segment = coof_segment_bar(script_bar, cursor, target);
        current_bar_ = segment;
        coof_evaluating_path_segment_ = true;
        // No intra-segment exact-level fills for ENTRY cascade orders. EXIT
        // cascade orders exact-fill on SUBSEQUENT legs (leg index > seg_i); the
        // gate uses the published leg index below to distinguish them.
        coof_at_extreme_waypoint_ = false;
        // KI-67 exit cascade: publish this SEGMENT's leg index
        // (path[next_waypoint-1] -> path[next_waypoint]).
        coof_hist_is_segment_ = true;
        coof_hist_path_index_ = next_waypoint - 1;
        CoofFillResult fill = process_next_pending_order(
            segment, /*allow_market_orders=*/false,
            exit_closed_from_bar, exit_closed_from_incarnation,
            exit_closed_was_long);
        coof_evaluating_path_segment_ = false;
        if (fill.filled) {
            const bool reached_target =
                std::abs(fill.fill_price - target) <= kSegmentDenomEps;
            const bool cursor_is_close = next_waypoint == 3
                && reached_target;
            // A fill mid-leg leaves the in-flight leg at (next_waypoint-1); a fill
            // that reaches the leg-end waypoint (path[next_waypoint]) advances to
            // the NEXT leg (next_waypoint) — the loop's ++next_waypoint below.
            coof_cascade_recalc_leg_ =
                reached_target ? next_waypoint : (next_waypoint - 1);
            consume_fill(
                fill, cursor_is_close,
                /*filled_at_bar_open_point=*/false);
            // H/L/C itself has been consumed by this priced fill. Only O has
            // the same-point two-fill exception; a market order born in the
            // recalc must wait for the next historical waypoint.
            if (reached_target) ++next_waypoint;
            continue;
        }

        cursor = target;
        ++next_waypoint;
        evaluate_current_point = true;
    }

    // Past the extreme waypoints: neither the ordinary close execution nor the
    // POOC-C / margin passes admit cascade orders (they hold to the next bar).
    // Publishing the C waypoint (index 3) also holds EXIT cascade orders there:
    // a terminal in-flight leg never gap-fills, and no leg is "subsequent" to C.
    coof_at_extreme_waypoint_ = false;
    coof_hist_is_segment_ = false;
    coof_hist_path_index_ = 3;
    // No in-flight leg remains: any exit placed by the ordinary-close / POOC-C /
    // margin recalcs is terminal and rolls.
    coof_cascade_recalc_leg_ = -1;

    // The regular historical close execution is still required after all
    // fill-triggered executions. It starts from the prior committed checkpoint
    // and becomes this bar's committed Pine state.
    cursor = path[3];
    uint64_t direct = execute_coof_script_body(
        script_bar, cursor, /*is_fill_recalc=*/false,
        /*cursor_is_bar_close=*/true, /*recalc_at_bar_open=*/false,
        kNoFillEventBudget);
    // C is the terminal historical tick. Direct fills produced by this
    // ordinary-close execution are real broker fills, but do not trigger
    // another script body after the bar has ended.
    commit_coof_script_state();
    fill_events += direct;

    // POOC's close-time market/priced orders share C and must never replay the
    // already-consumed high/low. Process every ordinary-C sibling at that same
    // broker epoch, without a fill-triggered body between siblings, until no
    // eligible order remains.
    if (process_orders_on_close_) {
        const Bar close_point = coof_point_bar(script_bar, cursor);
        // The COOF terminal-C loop bypasses process_pending_orders(), so apply
        // the exact two-call explicit reversal gross-admission fence once,
        // after the ordinary close body has emitted the complete sibling book
        // and before either sibling can fill.
        apply_pooc_coof_explicit_flat_market_gross_admission();
        int c_guard = 0;
        while (++c_guard <= kCoofLoopGuard) {
            current_bar_ = close_point;
            CoofFillResult fill = process_next_pending_order(
                close_point, /*allow_market_orders=*/true,
                exit_closed_from_bar, exit_closed_from_incarnation,
                exit_closed_was_long);
            if (!fill.filled) break;
            fill_events += fill.fill_events;
        }
    }

    // Preserve the existing once-per-script-bar liquidation placement. A
    // liquidation is itself a broker fill and therefore triggers a C-point
    // historical recalc.
    current_bar_ = script_bar;
    const size_t trades_before_mc = trades_.size();
    const uint64_t fill_seq_before_mc = broker_fill_event_seq_;
    process_margin_call(current_bar_);
    if (trades_.size() != trades_before_mc) {
        refresh_frozen_default_sizing_after_margin_call();
    }
    const uint64_t margin_events = broker_fill_event_seq_ - fill_seq_before_mc;
    if (margin_events > 0) {
        fill_events += run_coof_recalc_chain(
            script_bar, cursor, /*cursor_is_bar_close=*/true,
            /*recalc_at_bar_open=*/false, margin_events,
            kNoFillEventBudget, fill_events);
    }

    // Broker fills and eligible priced GTC orders persist. A margin-call
    // recalculation remains speculative and cannot replace the completed
    // ordinary-close checkpoint.
    restore_coof_script_state();
    coof_scheduler_active_ = false;
    coof_fill_recalc_active_ = false;
    coof_recalc_at_bar_open_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_cursor_is_bar_close_ = false;
    coof_evaluating_path_segment_ = false;
    coof_at_extreme_waypoint_ = false;
    coof_hist_is_segment_ = false;
    coof_hist_path_index_ = -1;
    coof_cascade_recalc_leg_ = -1;
    coof_cascade_force_wp_gap_ = false;
    coof_direct_fill_events_remaining_ = 0;
    coof_checkpoint_contains_current_bar_ = false;
    history_slot_is_new_ = true;
    coof_cursor_price_ = std::numeric_limits<double>::quiet_NaN();
    current_bar_ = script_bar;
    is_first_tick_ = true;
    is_last_tick_ = true;
}


// Reset all per-run STATE (not configuration) so a reused handle's run N is
// bit-identical to a fresh handle's run 1. See header doc + tests/
// test_handle_reuse_reset.cpp. Configuration fields (initial_capital_,
// pyramiding_, slippage_, commission_*, default_qty_*, syminfo_, inputs_, risk
// thresholds) are intentionally NOT touched — they are set before run().
void BacktestEngine::reset_run_state() {
    // Closed-trade list + cached P&L / count accumulators.
    trades_.clear();
    trades_.reserve(256);
    net_profit_sum_ = 0.0;
    gross_profit_sum_ = 0.0;
    gross_loss_sum_ = 0.0;
    win_trades_count_ = 0;
    loss_trades_count_ = 0;
    eventrades_count_ = 0;

    // Open position + pending orders.
    reset_position_state_to_flat();   // position_side_/qty/price/time/count,
                                      // pyramid_entries_, trail, partial ids
    pending_orders_.clear();
    pending_flat_market_pair_disqualified_bars_.clear();
    default_flat_market_gross_disqualified_bars_.clear();
    pending_close_qty_in_bar_ = 0.0;
    pos_view_freeze_bar_ = -1;   // KI-64: fresh run starts with no frozen view
    sb_close_active_ = false;
    sb_close_bar_ = -1;
    sb_close_calls_ = 0;
    sb_close_first_id_.clear();
    sb_close_first_target_ = 0.0;
    sb_close_first_carry_valid_ = false;
    sb_close_first_carry_qty_ = 0.0;
    sb_close_id_.clear();
    sb_close_comment_.clear();
    close_reserved_qty_.clear();
    close_two_call_first_qty_.clear();
    fold_exit_path_extremes_ = false;
    fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
    last_exit_fill_was_trail_ = false;

    // Equity + position-size extremes.
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;
    max_drawdown_ = 0.0;
    max_runup_ = 0.0;
    max_contracts_held_all_ = 0.0;
    max_contracts_held_long_ = 0.0;
    max_contracts_held_short_ = 0.0;
    equity_curve_.clear();           // retain capacity (handle-reuse sweep pattern)
    bars_in_market_ = 0;
    first_bar_open_ = std::numeric_limits<double>::quiet_NaN();

    // Risk halt latch + day trackers (one-way halt must not survive a rerun).
    risk_halted_ = false;
    cons_loss_day_count_ = 0;
    last_loss_day_ = -1;
    intraday_pnl_ = 0.0;
    intraday_pnl_day_ = -1;
    intraday_day_ = -1;
    intraday_cap_hit_ = false;
    intraday_fill_count_ = 0;
    intraday_cap_deferred_close_pending_ = false;
    intraday_cap_pooc_close_inheritor_incarnation_ = 0;
    broker_fill_event_seq_ = 0;
    coof_scheduler_active_ = false;
    coof_fill_recalc_active_ = false;
    coof_recalc_at_bar_open_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_cursor_is_bar_close_ = false;
    coof_evaluating_path_segment_ = false;
    coof_at_extreme_waypoint_ = false;
    coof_hist_is_segment_ = false;
    coof_hist_path_index_ = -1;
    coof_cascade_recalc_leg_ = -1;
    coof_cascade_force_wp_gap_ = false;
    coof_cursor_price_ = std::numeric_limits<double>::quiet_NaN();
    coof_direct_fill_events_remaining_ = 0;
    coof_checkpoint_contains_current_bar_ = false;
    history_slot_is_new_ = true;

    // Per-bar cursor + session-predicate state.
    bar_index_ = 0;
    prev_bar_timestamp_ = 0;
    prev_in_session_ = false;
    session_ismarket_ = false;
    session_isfirstbar_ = false;
    session_islastbar_ = false;

    // A normal run starts a new lifecycle. stream_warmup_mode_ is deliberately
    // preserved: stream_begin sets it before delegating to run() so historical
    // bars are not mislabeled as the rightmost realtime bar.
    stream_phase_ = StreamPhase::IDLE;
    stream_input_tf_ms_ = 0;
    stream_next_input_open_ms_ = 0;
    stream_clock_ms_ = 0;
    stream_last_tick_ms_ = 0;
    stream_last_sequence_ = 0;
    stream_seen_sequence_ = false;
    stream_has_input_bar_ = false;
    stream_input_bar_ = Bar{};
    stream_last_price_ = 0.0;
    stream_has_last_price_ = false;
    stream_next_script_bar_index_ = 0;
    stream_script_bar_had_tick_ = false;
    stream_script_tick_seen_ = false;

    // Native source-series history (input.source(...) ring buffers). Must list
    // EVERY _src_*_ member declared in engine.hpp — a missing one leaks history
    // into a reused handle (see test_handle_reuse_reset all-series coverage).
    _src_open_.clear();
    _src_high_.clear();
    _src_low_.clear();
    _src_close_.clear();
    _src_volume_.clear();
    _src_hl2_.clear();
    _src_hlc3_.clear();
    _src_ohlc4_.clear();
    _src_hlcc4_.clear();

    // Per-bar trace/diagnostic buffers (trace_enabled_ is config — preserved).
    if (trace_enabled_) {
        trace_buffer_.clear();                       // keep capacity for the next traced run
    } else {
        std::vector<TraceEntryC>().swap(trace_buffer_);  // release retained capacity
    }
    trace_names_.clear();
    trace_name_index_.clear();
}


void BacktestEngine::run(const Bar* bars, int n) {
    last_error_.clear();
    if (n > 0 && bars != nullptr) {
        last_bar_time_ = bars[n - 1].timestamp;
        last_bar_index_ = n - 1;
    } else {
        last_bar_time_ = 0;
        last_bar_index_ = 0;
    }
    try {
    reset_run_state();
    equity_curve_.reserve((size_t)std::max(n, 0));

    std::string detected_tf = "";
    if (n >= 2 && bars != nullptr) {
        detected_tf = detect_timeframe(bars, n);
    }
    input_tf_ = detected_tf;
    script_tf_ = detected_tf;
    script_tf_seconds_ = tf_to_seconds(script_tf_);

    // Runtime diagnostics (single-timeframe path)
    diag_input_bars_processed_ = n;
    diag_script_bars_processed_ = 0;
    diag_magnifier_sub_bars_processed_ = 0;
    diag_magnifier_sample_ticks_processed_ = 0;
    diag_script_tf_ratio_ = 1;
    diag_needs_aggregation_ = false;
    bar_magnifier_enabled_ = false;
    for (auto& state : security_eval_states_) {
        state.feed_count = 0;
        state.eval_complete_count = 0;
        state.eval_partial_count = 0;
        state.current_bar = Bar{};
        state.current_sub_bar_count = 0;
    }

    for (int i = 0; i < n; i++) {
        current_bar_ = bars[i];
        bar_index_ = i;
        is_first_tick_ = true;
        is_last_tick_ = true;
        barstate_islast_ = !stream_warmup_mode_ && (i == n - 1);
        diag_script_bars_processed_++;
        // Reset per-bar pending-close accumulator. Each on_bar call
        // captures fresh ``strategy.close*`` qty for the same-bar
        // close-then-entry source-order rule (see engine.hpp).
        pending_close_qty_in_bar_ = 0.0;
        dispatch_bar();
        update_equity_extremes();
        record_equity_point(current_bar_.timestamp);  // ts not mutated on this path
        prev_bar_timestamp_ = current_bar_.timestamp;
    }
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}


// --- run_magnified_bar ---
void BacktestEngine::run_magnified_bar(const std::vector<Bar>& sub_bars, int64_t script_bar_ts) {
    if (sub_bars.empty()) return;
    if (calc_on_order_fills_) {
        run_magnified_bar_calc_on_order_fills(sub_bars, script_bar_ts);
        return;
    }

    double bar_open = sub_bars.front().open;
    double running_high = sub_bars.front().open;
    double running_low = sub_bars.front().open;
    double cumulative_vol = 0.0;
    int64_t timestamp = sub_bars.front().timestamp;

    // Hoisted out of the sub-bar loops below; cleared/refilled each iteration
    // via the out-param sample_price_path overloads so the buffer's capacity
    // is reused instead of heap-allocating a fresh vector per sub-bar.
    std::vector<double> samples;

    int total_sub = (int)sub_bars.size();
    diag_magnifier_sub_bars_processed_ += total_sub;

    // Real-bar magnifier mode: when we have multiple input sub-bars per script
    // bar (i.e. input_tf < script_tf and the validator/caller fed real lower-TF
    // OHLCV), each sub-bar's OHLC already encodes real intra-bar movement.
    // Walking each real sub-bar at its natural ENDPOINTS (O,H,L,C) reproduces
    // TradingView's broker emulator exactly — TV uses ENDPOINTS only and steps
    // through the lower-TF bars one at a time. Synthetic distributions
    // (UNIFORM/COSINE/TRIANGLE/etc.) interpolate spurious mid-points inside a
    // 1m bar that don't correspond to any real tick, adding noise. With real
    // sub-bars in hand we therefore force ENDPOINTS+4 regardless of the
    // user-requested distribution, and skip volume-weighted upsampling: extra
    // ticks beyond the four real OHLC corners cannot recover information that
    // wasn't in the input feed.
    const bool real_bar_magnifier_mode = (total_sub > 1);

    // Precompute per-script-bar mean volume so volume-weighted sampling can
    // scale each sub-bar's tick count relative to the local average.
    double mean_vol = 0.0;
    if (magnifier_volume_weighted_ && total_sub > 0 && !real_bar_magnifier_mode) {
        double sum_vol = 0.0;
        for (const Bar& sb : sub_bars) sum_vol += sb.volume;
        mean_vol = sum_vol / total_sub;
    }

    for (int si = 0; si < total_sub; ++si) {
        const Bar& sb = sub_bars[si];
        cumulative_vol += sb.volume;
        timestamp = sb.timestamp;

        // Feed security evaluators with each sub-bar
        for (auto& state : security_eval_states_) {
            feed_security_eval_state(state, sb);
        }

        if (real_bar_magnifier_mode) {
            // Each real sub-bar's OHLC turning points are the ticks. Always 4
            // samples = [O, H, L, C] in TV-style path order.
            sample_price_path(sb, 4, MagnifierDistribution::ENDPOINTS, samples);
        } else if (magnifier_volume_weighted_) {
            sample_price_path_volume_weighted(
                sb, magnifier_samples_, mean_vol,
                /*min_samples=*/2,
                /*max_samples=*/std::max(magnifier_samples_ * 4, 8),
                magnifier_dist_, samples);
        } else {
            sample_price_path(sb, magnifier_samples_, magnifier_dist_, samples);
        }
        int n_samples = (int)samples.size();
        diag_magnifier_sample_ticks_processed_ += n_samples;

        for (int pi = 0; pi < n_samples; ++pi) {
            double price = samples[pi];
            running_high = std::max(running_high, price);
            running_low = std::min(running_low, price);

            current_bar_.open = bar_open;
            current_bar_.high = running_high;
            current_bar_.low = running_low;
            current_bar_.close = price;
            current_bar_.volume = cumulative_vol;
            current_bar_.timestamp = timestamp;

            is_first_tick_ = (si == 0 && pi == 0);
            is_last_tick_ = (si == total_sub - 1 && pi == n_samples - 1);

            if (process_orders_on_close_) {
                process_pending_orders(current_bar_);
                update_per_trade_extremes();
                if (is_last_tick_) {
                    // Force is_first_tick_ true so that on_bar advances the series history.
                    is_first_tick_ = true;
                    // The strategy body and its time-of-day builtins
                    // (hour/minute/dayofmonth, intraday session gates) must see
                    // the SCRIPT bar's canonical open timestamp, not the final
                    // sub-bar's ts — else exact-time gates ("lock IB at 10:30")
                    // never fire. Intrabar fills above already used the real
                    // sub-bar timestamps. No-op when total_sub==1 (synthesized
                    // magnifier: the single sub-bar IS the script bar).
                    current_bar_.timestamp = script_bar_ts;
                    _push_source_series();
                    invoke_chart_on_bar(current_bar_);
                    flush_same_bar_close();  // surviving strategy.close fill
                    process_pending_orders(current_bar_);
                }
            } else {
                process_pending_orders(current_bar_);
                update_per_trade_extremes();
                if (is_last_tick_) {
                    // Force is_first_tick_ true so that on_bar advances the series history.
                    is_first_tick_ = true;
                    // See note above: strategy body sees the script-bar open ts,
                    // not the final sub-bar ts.
                    current_bar_.timestamp = script_bar_ts;
                    _push_source_series();
                    invoke_chart_on_bar(current_bar_);
                }
            }
        }
    }
    // TradingView forced-liquidation check, once per script bar. By the final
    // sub-bar current_bar_.high/.low hold the full script-bar adverse extreme,
    // and current_bar_.timestamp was restored to the script-bar open ts above.
    // Same post-liquidation re-freeze as the non-magnifier path (dispatch_bar).
    {
        size_t trades_before_mc = trades_.size();
        process_margin_call(current_bar_);
        if (trades_.size() != trades_before_mc) {
            refresh_frozen_default_sizing_after_margin_call();
        }
    }
    finalize_bar();
}

void BacktestEngine::run_magnified_bar_calc_on_order_fills(
        const std::vector<Bar>& sub_bars,
        int64_t script_bar_ts) {
    if (sub_bars.empty()) return;

    struct BrokerTick {
        double price;
        int64_t timestamp;
        // A real lower-timeframe bar starts a fresh broker epoch at its open.
        // The jump from the prior sub-bar's close to this price is a gap, not
        // a continuously traversed segment.
        bool starts_subbar;
    };

    Bar script_bar{};
    script_bar.open = sub_bars.front().open;
    script_bar.high = sub_bars.front().high;
    script_bar.low = sub_bars.front().low;
    script_bar.close = sub_bars.back().close;
    script_bar.volume = 0.0;
    script_bar.timestamp = script_bar_ts;
    for (const Bar& sb : sub_bars) {
        script_bar.high = std::max(script_bar.high, sb.high);
        script_bar.low = std::min(script_bar.low, sb.low);
        script_bar.volume += sb.volume;
    }

    const int total_sub = static_cast<int>(sub_bars.size());
    const bool real_lower_tf = total_sub > 1;
    diag_magnifier_sub_bars_processed_ += total_sub;

    double mean_vol = 0.0;
    if (magnifier_volume_weighted_ && !real_lower_tf) {
        for (const Bar& sb : sub_bars) mean_vol += sb.volume;
        mean_vol /= static_cast<double>(total_sub);
    }

    std::vector<BrokerTick> ticks;
    std::vector<double> samples;
    for (const Bar& sb : sub_bars) {
        // Historical script executions see the completed security state for
        // the script bar. Feeding all committed lower-TF bars before taking
        // the script-state checkpoint mirrors the standard path, where
        // security evaluators are fed before dispatch_bar.
        for (auto& state : security_eval_states_) {
            feed_security_eval_state(state, sb);
        }

        if (real_lower_tf) {
            sample_price_path(sb, 4, MagnifierDistribution::ENDPOINTS, samples);
        } else if (magnifier_volume_weighted_) {
            sample_price_path_volume_weighted(
                sb, magnifier_samples_, mean_vol,
                /*min_samples=*/2,
                /*max_samples=*/std::max(magnifier_samples_ * 4, 8),
                magnifier_dist_, samples);
        } else {
            sample_price_path(sb, magnifier_samples_, magnifier_dist_, samples);
        }
        diag_magnifier_sample_ticks_processed_ +=
            static_cast<int64_t>(samples.size());
        for (std::size_t sample_idx = 0; sample_idx < samples.size();
             ++sample_idx) {
            ticks.push_back({
                samples[sample_idx], sb.timestamp,
                real_lower_tf && sample_idx == 0,
            });
        }
    }
    if (ticks.empty()) return;

    // Unlike a fixed arbitrary loop guard, termination is derived from the
    // actual lower-timeframe broker ticks supplied by the magnifier.
    const uint64_t max_fill_events = static_cast<uint64_t>(ticks.size());
    uint64_t fill_events = 0;
    int exit_closed_from_bar = -1;
    uint64_t exit_closed_from_incarnation = 0;
    bool exit_closed_was_long = false;
    snapshot_coof_script_state();
    coof_scheduler_active_ = true;
    coof_cursor_is_bar_close_ = false;
    coof_evaluating_path_segment_ = false;
    coof_recalc_after_first_open_fill_ = false;

    double cursor = ticks.front().price;
    int64_t cursor_ts = ticks.front().timestamp;
    std::size_t next_tick = 1;
    bool evaluate_current_point = true;

    auto consume_fill = [&](const CoofFillResult& fill,
                            bool cursor_is_close,
                            bool filled_at_first_tick) {
        const uint64_t before = fill_events;
        cursor = fill.fill_price;
        // Magnifier path: coof_born_mid_bar is inert here (the cascade gate is
        // guarded by !bar_magnifier_enabled_), but keep provenance consistent —
        // a first-tick fill is the magnifier analogue of a bar-open recalc.
        fill_events += run_coof_recalc_chain(
            script_bar, cursor, cursor_is_close, filled_at_first_tick,
            fill.fill_events, max_fill_events, fill_events);
        evaluate_current_point = filled_at_first_tick
            && before == 0 && fill_events == 1;
    };

    while (fill_events < max_fill_events) {
        if (evaluate_current_point) {
            const bool cursor_is_close = next_tick >= ticks.size();
            Bar point = coof_point_bar(script_bar, cursor);
            point.timestamp = cursor_ts;
            current_bar_ = point;
            CoofFillResult fill = process_next_pending_order(
                point, /*allow_market_orders=*/true,
                exit_closed_from_bar, exit_closed_from_incarnation,
                exit_closed_was_long);
            if (fill.filled) {
                consume_fill(
                    fill, cursor_is_close,
                    /*filled_at_first_tick=*/next_tick == 1);
                continue;
            }
            evaluate_current_point = false;
        }

        if (next_tick >= ticks.size()) break;

        const BrokerTick target = ticks[next_tick];
        if (target.starts_subbar) {
            // Every real magnifier sub-bar opens fresh.  Resting priced orders
            // evaluate the new open as a point (and therefore use gap-fill
            // pricing); they must never interpolate a touch through the
            // previous close -> new open discontinuity.
            cursor = target.price;
            cursor_ts = target.timestamp;
            ++next_tick;
            evaluate_current_point = true;
            continue;
        }
        Bar segment = coof_segment_bar(script_bar, cursor, target.price);
        segment.timestamp = target.timestamp;
        current_bar_ = segment;
        coof_evaluating_path_segment_ = true;
        CoofFillResult fill = process_next_pending_order(
            segment, /*allow_market_orders=*/false,
            exit_closed_from_bar, exit_closed_from_incarnation,
            exit_closed_was_long);
        coof_evaluating_path_segment_ = false;
        if (fill.filled) {
            cursor_ts = target.timestamp;
            const bool reached_target =
                std::abs(fill.fill_price - target.price) <= kSegmentDenomEps;
            const bool cursor_is_close = next_tick + 1 == ticks.size()
                && reached_target;
            consume_fill(
                fill, cursor_is_close,
                /*filled_at_first_tick=*/false);
            // The real lower-TF endpoint is already consumed. Do not replay
            // a market-enabled point at the same H/L/C tick; O remains the
            // sole intentional same-tick exception.
            if (reached_target) ++next_tick;
            continue;
        }

        cursor = target.price;
        cursor_ts = target.timestamp;
        ++next_tick;
        evaluate_current_point = true;
    }

    cursor = ticks.back().price;
    uint64_t direct = execute_coof_script_body(
        script_bar, cursor, /*is_fill_recalc=*/false,
        /*cursor_is_bar_close=*/true, /*recalc_at_bar_open=*/false,
        fill_events < max_fill_events ? max_fill_events - fill_events : 0);
    commit_coof_script_state();
    // The last real lower-TF close is also terminal: count direct fills but do
    // not execute another script body after that completed broker tick.
    fill_events += direct;

    if (process_orders_on_close_) {
        Bar close_point = coof_point_bar(script_bar, cursor);
        close_point.timestamp = ticks.back().timestamp;
        while (fill_events < max_fill_events) {
            current_bar_ = close_point;
            CoofFillResult fill = process_next_pending_order(
                close_point, /*allow_market_orders=*/true,
                exit_closed_from_bar, exit_closed_from_incarnation,
                exit_closed_was_long);
            if (!fill.filled) break;
            fill_events += fill.fill_events;
        }
    }

    current_bar_ = script_bar;
    const size_t trades_before_mc = trades_.size();
    const uint64_t fill_seq_before_mc = broker_fill_event_seq_;
    process_margin_call(current_bar_);
    if (trades_.size() != trades_before_mc) {
        refresh_frozen_default_sizing_after_margin_call();
    }
    const uint64_t margin_events = broker_fill_event_seq_ - fill_seq_before_mc;
    if (margin_events > 0 && fill_events < max_fill_events) {
        fill_events += run_coof_recalc_chain(
            script_bar, cursor, /*cursor_is_bar_close=*/true,
            /*recalc_at_bar_open=*/false, margin_events,
            max_fill_events, fill_events);
    }

    restore_coof_script_state();
    coof_scheduler_active_ = false;
    coof_fill_recalc_active_ = false;
    coof_recalc_at_bar_open_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_cursor_is_bar_close_ = false;
    coof_evaluating_path_segment_ = false;
    coof_at_extreme_waypoint_ = false;
    coof_hist_is_segment_ = false;
    coof_hist_path_index_ = -1;
    coof_cascade_recalc_leg_ = -1;
    coof_cascade_force_wp_gap_ = false;
    coof_direct_fill_events_remaining_ = 0;
    coof_checkpoint_contains_current_bar_ = false;
    history_slot_is_new_ = true;
    coof_cursor_price_ = std::numeric_limits<double>::quiet_NaN();
    current_bar_ = script_bar;
    is_first_tick_ = true;
    is_last_tick_ = true;
    finalize_bar();
}


// --- New run() overload with full parameter set ---
void BacktestEngine::run(const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) {
    last_error_.clear();
    if (n_input > 0 && input_bars != nullptr) {
        last_bar_time_ = input_bars[n_input - 1].timestamp;
    } else {
        last_bar_time_ = 0;
    }
    try {
    // Auto-detect input_tf from bar timestamps if not provided
    std::string effective_input_tf = input_tf;
    if (effective_input_tf.empty() && n_input >= 2) {
        effective_input_tf = detect_timeframe(input_bars, n_input);
    }
    // script_tf defaults to input_tf if not provided (strategy runs on the data's timeframe)
    std::string effective_script_tf = script_tf.empty() ? effective_input_tf : script_tf;

    // Store parameters
    input_tf_ = effective_input_tf;
    script_tf_ = effective_script_tf;
    script_tf_seconds_ = tf_to_seconds(script_tf_);
    bar_magnifier_enabled_ = bar_magnifier;
    magnifier_samples_ = magnifier_samples;
    magnifier_dist_ = magnifier_dist;

    configure_security_evaluators();

    // Runtime diagnostics baseline
    diag_input_bars_processed_ = n_input;
    diag_script_bars_processed_ = 0;
    diag_magnifier_sub_bars_processed_ = 0;
    diag_magnifier_sample_ticks_processed_ = 0;

    reset_run_state();

    // Determine aggregation ratio for script TF
    int ratio = tf_ratio(effective_input_tf, effective_script_tf);
    if (ratio == -2 && !effective_input_tf.empty() && !effective_script_tf.empty()) {
        throw std::runtime_error(
            "script timeframe must be coarser than or equal to input timeframe: requested script_tf "
            + effective_script_tf + " from input timeframe " + effective_input_tf
        );
    }
    bool needs_aggregation = (ratio > 1 || ratio == -1);
    diag_script_tf_ratio_ = ratio;
    diag_needs_aggregation_ = needs_aggregation;

    // Initialize script TF aggregator
    if (needs_aggregation) {
        // Use a single timeframe-constructor path so script timeframe boundaries
        // follow the same wall-clock/calendar semantics as request.security.
        script_tf_agg_ = TimeframeAggregator(effective_script_tf, effective_input_tf);
    } else {
        script_tf_agg_ = TimeframeAggregator();  // passthrough
    }

    int expected_script_bars =
        count_expected_script_bars(input_bars, n_input, needs_aggregation);
    last_bar_index_ = expected_script_bars - 1;
    // reset_run_state() already ran above — reserve AFTER it so the capacity
    // hint isn't wiped (clear() retains capacity but order still matters for
    // any future reset that releases).
    equity_curve_.reserve((size_t)std::max(expected_script_bars, 0));

    validate_security_timeframes(effective_input_tf);

    init_security_eval_states_for_run(effective_input_tf);
    prepare_historical_security_lookahead_projections(
        input_bars, n_input, effective_input_tf);

    if (!needs_aggregation && !bar_magnifier) {
        run_simple_bar_loop(input_bars, n_input);
    } else {
        run_aggregation_bar_loop(input_bars, n_input, bar_magnifier,
                                 expected_script_bars);
    }
    clear_historical_security_lookahead_projections();
    } catch (const std::exception& e) {
        clear_historical_security_lookahead_projections();
        last_error_ = e.what();
    } catch (...) {
        clear_historical_security_lookahead_projections();
        last_error_ = "unknown error during BacktestEngine::run";
    }
}


// Preview pass: when aggregating to a coarser script TF, count how many
// completed script-TF bars the input feed will produce so the simple-loop /
// aggregation-loop can flag the final bar with barstate.islast at the right
// moment. Returns n_input verbatim when no aggregation is needed.
int BacktestEngine::count_expected_script_bars(const Bar* input_bars, int n_input,
                                                bool needs_aggregation) const {
    if (!needs_aggregation) return n_input;
    TimeframeAggregator preview_agg(script_tf_, input_tf_);
    int count = 0;
    for (int i = 0; i < n_input; ++i) {
        AggregatedBar preview = preview_agg.feed(input_bars[i]);
        if (preview.is_complete) {
            ++count;
        }
    }
    return count;
}


// Lazily reset per-run security evaluator state and (re)construct the
// aggregator each evaluator uses based on its requested TF vs. input_tf.
// Lower-TF emulation evaluators keep their default-constructed (passthrough)
// aggregator since their per-sub-bar synthesis is driven elsewhere.
void BacktestEngine::init_security_eval_states_for_run(
    const std::string& effective_input_tf) {
    for (auto& state : security_eval_states_) {
        state.feed_count = 0;
        state.eval_complete_count = 0;
        state.eval_partial_count = 0;
        state.current_bar = Bar{};
        state.current_sub_bar_count = 0;
        state.lower_tf_sub_bar_index = 0;
        state.lower_tf_input_buffer.clear();
        state.historical_projections.clear();
        state.historical_projection_cursor = 0;
        state.historical_projection_feed_index = 0;
        state.aggregator = TimeframeAggregator();
        if (state.lower_tf_emulation || state.lower_tf_use_input) {
            continue;
        }
        int req_ratio = tf_ratio(effective_input_tf, state.tf);
        if (req_ratio > 1) {
            state.aggregator = TimeframeAggregator(state.tf, effective_input_tf);
        } else if (req_ratio == -1) {
            state.aggregator = TimeframeAggregator(state.tf, effective_input_tf);
        }
    }
}


// Build the finite-batch oracle used by the opt-in historical lookahead
// candidate. Each eligible HTF bucket is aggregated from all input bars that
// are available in the batch and stored once with its first-child index. The
// evaluator is dispatched only at that index (engine_security.cpp), so the
// value is projected there and held for the rest of the bucket. A trailing
// bucket that has not reached its natural boundary is still projected from the
// available bars, but remains an incomplete evaluation so committed security
// history does not advance prematurely.
void BacktestEngine::prepare_historical_security_lookahead_projections(
        const Bar* input_bars, int n_input,
        const std::string& effective_input_tf) {
    clear_historical_security_lookahead_projections();

    const int input_seconds = tf_to_seconds(effective_input_tf);
    const int script_seconds = script_tf_seconds_;
    if (!historical_security_lookahead_projection_
            || stream_warmup_mode_
            // The finite-batch oracle is built from raw input bars. Until it
            // can consume script-TF aggregates, activating it across a
            // separate input->script aggregation stage would project the
            // wrong child indexes and values.
            || effective_input_tf != script_tf_
            || input_bars == nullptr || n_input <= 0
            || input_seconds <= 0 || script_seconds <= 0) {
        return;
    }

    // Range-start warmup drops every earlier input in
    // feed_security_eval_state(). Build the projection from that exact same
    // retained suffix and store child indexes relative to it: the per-state
    // feed cursor likewise starts at zero on the first retained child because
    // the early-return path never increments it. This composes the two
    // independently opt-in historical semantics without exposing a pre-range
    // aggregate or shifting the first projected bucket.
    int projection_begin = 0;
    if (security_range_start_na_warmup_) {
        while (projection_begin < n_input
                && input_bars[projection_begin].timestamp
                    < security_range_start_ms_) {
            ++projection_begin;
        }
        if (projection_begin >= n_input) {
            return;
        }
    }

    historical_security_lookahead_projection_active_ = true;
    const int64_t input_ms = static_cast<int64_t>(input_seconds) * 1000;
    const int projection_count = n_input - projection_begin;

    for (auto& state : security_eval_states_) {
        const int requested_seconds = tf_to_seconds(state.tf);
        const bool eligible = !state.lower_tf_requested
            && !state.lower_tf_emulation
            && !state.lower_tf_use_input
            && state.lookahead_on
            && !state.gaps_on
            && !state.heikinashi
            && requested_seconds > script_seconds;
        if (!eligible) {
            continue;
        }

        const int expected_children = std::max(
            1, requested_seconds / input_seconds);
        state.historical_projections.reserve(static_cast<std::size_t>(
            projection_count / expected_children + 1));
        state.historical_projection_cursor = 0;
        state.historical_projection_feed_index = 0;

        auto crosses_requested_boundary = [&](int64_t from_ms,
                                               int64_t to_ms) {
            // tf_change treats epoch zero as an uninitialized sentinel. Keep
            // tests/synthetic feeds beginning at zero correct via the fixed-TF
            // bucket fallback; real feeds take the calendar-aware path.
            if (from_ms != 0 && to_ms != 0) {
                return tf_change(from_ms, to_ms, state.tf);
            }
            const int64_t requested_ms =
                static_cast<int64_t>(requested_seconds) * 1000;
            return requested_ms > 0
                && from_ms / requested_ms != to_ms / requested_ms;
        };

        auto merge = [](Bar& aggregate, const Bar& child) {
            aggregate.high = std::max(aggregate.high, child.high);
            aggregate.low = std::min(aggregate.low, child.low);
            aggregate.close = child.close;
            aggregate.volume += child.volume;
        };

        auto publish_group = [&](int begin, const Bar& aggregate,
                                 bool is_complete) {
            state.historical_projections.push_back(
                HistoricalSecurityProjection{
                    aggregate, static_cast<std::size_t>(begin), is_complete});
        };

        int group_begin = 0;
        Bar aggregate = input_bars[projection_begin];
        for (int i = projection_begin + 1; i < n_input; ++i) {
            const int retained_child_index = i - projection_begin;
            if (crosses_requested_boundary(aggregate.timestamp,
                                           input_bars[i].timestamp)) {
                // A later bucket proves this group is historical/confirmed,
                // even when sparse input omitted its natural final child.
                publish_group(group_begin, aggregate, true);
                group_begin = retained_child_index;
                aggregate = input_bars[i];
            } else {
                merge(aggregate, input_bars[i]);
            }
        }

        bool final_complete = false;
        const int64_t last_timestamp = input_bars[n_input - 1].timestamp;
        if (last_timestamp <= std::numeric_limits<int64_t>::max() - input_ms) {
            final_complete = crosses_requested_boundary(
                last_timestamp, last_timestamp + input_ms);
        }
        publish_group(group_begin, aggregate, final_complete);
    }
}


void BacktestEngine::clear_historical_security_lookahead_projections() {
    historical_security_lookahead_projection_active_ = false;
    for (auto& state : security_eval_states_) {
        state.historical_projections.clear();
        state.historical_projection_cursor = 0;
        state.historical_projection_feed_index = 0;
    }
}


// Bar pump for the no-aggregation, no-magnifier case: every input bar is a
// script bar, fed straight to on_bar with the standard
// pending-orders / per-trade-extremes / on_bar / equity-update sequence
// (with the process_orders_on_close TV variant when configured).
void BacktestEngine::run_simple_bar_loop(const Bar* input_bars, int n_input) {
    for (int i = 0; i < n_input; ++i) {
        current_bar_ = input_bars[i];
        bar_index_ = i;
        is_first_tick_ = true;
        is_last_tick_ = true;
        barstate_islast_ = !stream_warmup_mode_ && (i == n_input - 1);
        diag_script_bars_processed_++;
        // Reset per-bar pending-close accumulator. Each on_bar call captures
        // fresh ``strategy.close*`` qty for the same-bar close-then-entry
        // source-order rule (see engine.hpp). Without the reset the
        // accumulator monotonically grows and starves every subsequent
        // priced-entry's tv_carry_qty (validation/52, 63, 72, 93, 95, 96
        // pre-fix: per-leg PnL drifts because the deferred-flip carry
        // chain is wiped after the first fire).
        pending_close_qty_in_bar_ = 0.0;

        // Feed security evaluators
        for (auto& state : security_eval_states_) {
            feed_security_eval_state(state, input_bars[i]);
        }

        // Update session predicates for session.ismarket / isfirstbar / islastbar.
        session_ismarket_  = pine_session_ismarket(syminfo_.session, syminfo_.timezone,
                                                    current_bar_.timestamp);
        session_isfirstbar_ = session_ismarket_ && !prev_in_session_;
        // islastbar: fire when this bar is in-session but the NEXT bar won't be
        // (lookahead: peek at next bar's timestamp if available, else fire on last bar).
        if (session_ismarket_) {
            bool next_in_session = false;
            if (i + 1 < n_input) {
                next_in_session = pine_session_ismarket(syminfo_.session, syminfo_.timezone,
                                                         input_bars[i + 1].timestamp);
            }
            session_islastbar_ = !next_in_session;
        } else {
            session_islastbar_ = false;
        }

        dispatch_bar();
        prev_in_session_ = session_ismarket_;
        update_equity_extremes();
        record_equity_point(current_bar_.timestamp);  // ts not mutated on this path
        prev_bar_timestamp_ = current_bar_.timestamp;
    }
}


// Bar pump for the aggregation path (with or without magnifier). Feeds each
// input bar through ``script_tf_agg_`` and either dispatches each completed
// script bar straight to on_bar (no magnifier) or hands the collected
// sub-bars to ``run_magnified_bar`` for price-path sampling. Security
// evaluators are fed per input bar in the non-magnifier case and deferred
// to ``run_magnified_bar`` in the magnifier case so each sub-bar runs
// exactly once before its sampled ticks.
void BacktestEngine::run_aggregation_bar_loop(const Bar* input_bars, int n_input,
                                                bool bar_magnifier,
                                                int expected_script_bars) {
    std::vector<Bar> group_sub_bars;
    // Each completed script bar collects up to `ratio` input sub-bars before
    // run_magnified_bar drains and clears the buffer. Reserve once so the
    // per-script-bar push_back churn reuses one allocation. diag_script_tf_ratio_
    // holds the input→script ratio set just before this loop; only a fixed
    // ratio (>1) gives a meaningful bound (variable/-1 left to grow naturally).
    if (bar_magnifier && diag_script_tf_ratio_ > 1) {
        group_sub_bars.reserve(static_cast<std::size_t>(diag_script_tf_ratio_));
    }
    int script_bar_index = 0;
    int emitted_script_bars = 0;

    for (int i = 0; i < n_input; ++i) {
        if (!bar_magnifier) {
            for (auto& state : security_eval_states_) {
                feed_security_eval_state(state, input_bars[i]);
            }
        }

        AggregatedBar ab = script_tf_agg_.feed(input_bars[i]);

        if (bar_magnifier) {
            group_sub_bars.push_back(input_bars[i]);
        }

        if (ab.is_complete) {
            // Script-bar label for the equity curve: ab.bar.timestamp — the
            // aggregator's first-present sub-bar ts of the COMPLETED bucket.
            // The aggregator is fed identically with magnifier on and off, so
            // this label is magnifier-invariant by construction. Captured
            // here because run_magnified_bar overwrites
            // current_bar_.timestamp with each sub-bar's ts.
            //
            // Deliberately NOT group_sub_bars.front().timestamp: when a
            // bucket completes via the boundary path (irregular/partial first
            // bucket), the boundary-triggering input bar is walked with the
            // PREVIOUS script bar's group but belongs to the new aggregator
            // bucket, so the group front lags ab.bar.timestamp by one input
            // bar and the on/off curves would disagree on that label.
            const int64_t script_bar_ts = ab.bar.timestamp;
            bar_index_ = script_bar_index++;
            emitted_script_bars++;
            barstate_islast_ = !stream_warmup_mode_
                && (emitted_script_bars == expected_script_bars);
            diag_script_bars_processed_++;
            // Reset per-bar pending-close accumulator. See run_simple_bar_loop
            // for the regression history; the aggregated path was missing the
            // same reset, which is why all 8 affected probes are scripts that
            // run with input_tf < script_tf (1m feeds, 15m strategies).
            pending_close_qty_in_bar_ = 0.0;

            if (bar_magnifier && !group_sub_bars.empty()) {
                // Magnifier mode: update session state using script-bar timestamp
                // (first sub-bar's timestamp represents the aggregated bar).
                session_ismarket_  = pine_session_ismarket(syminfo_.session, syminfo_.timezone,
                                                            group_sub_bars.front().timestamp);
                session_isfirstbar_ = session_ismarket_ && !prev_in_session_;
                session_islastbar_  = false;  // not deterministic in magnifier mode without lookahead
                run_magnified_bar(group_sub_bars, script_bar_ts);
                prev_in_session_ = session_ismarket_;
                group_sub_bars.clear();
            } else {
                // No magnifier: use aggregated bar directly.
                //
                // ab.bar.timestamp is the FIRST-PRESENT sub-bar's timestamp
                // (TimeframeAggregator keeps the opening sub-bar's ts through
                // the merge). When a feed gap eats the bucket-opening sub-bar(s)
                // — e.g. an exchange outage at HH:00 — this label drifts forward
                // (09:00 bucket whose first surviving bar is 09:03 → ts=09:03,
                // where TV would label it 09:00).
                //
                // We intentionally do NOT floor to the bucket boundary
                // ((ts/bucket_ms)*bucket_ms). That floor assumes UTC-epoch-aligned
                // buckets, which is only true for 24/7 instruments. Session-anchored
                // TFs (e.g. US-equity 4h anchored to the 09:30 session open) are
                // NOT UTC-aligned, so flooring would mislabel every bar there —
                // worse than the rare gap-at-open drift. First-present is also the
                // truthful timestamp: it is the real first bar that traded in the
                // bucket. The drift never moves PnL (cosmetic entry/exit time +
                // time-gated logic only); the sole risk is a strategy gating on an
                // exact bucket-open instant, which is not a pattern TV scripts use.
                current_bar_ = ab.bar;
                // Update session predicates.
                session_ismarket_  = pine_session_ismarket(syminfo_.session, syminfo_.timezone,
                                                            current_bar_.timestamp);
                session_isfirstbar_ = session_ismarket_ && !prev_in_session_;
                session_islastbar_  = session_ismarket_ && barstate_islast_;
                dispatch_bar();
                prev_in_session_ = session_ismarket_;
            }
            update_equity_extremes();
            record_equity_point(script_bar_ts);
            prev_bar_timestamp_ = current_bar_.timestamp;
        }
    }
}


// --- Input injection helpers ---
double BacktestEngine::get_input_double(const std::string& key, double default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        try { return std::stod(it->second); } catch (...) {}
    }
    return default_val;
}


int BacktestEngine::get_input_int(const std::string& key, int default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        try { return std::stoi(it->second); } catch (...) {}
    }
    return default_val;
}


int64_t BacktestEngine::get_input_int64(const std::string& key, int64_t default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        try { return std::stoll(it->second); } catch (...) {}
    }
    return default_val;
}


bool BacktestEngine::get_input_bool(const std::string& key, bool default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        const std::string& v = it->second;
        if (v == "true" || v == "1") return true;
        if (v == "false" || v == "0") return false;
    }
    return default_val;
}


std::string BacktestEngine::get_input_string(const std::string& key, const std::string& default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) return it->second;
    return default_val;
}


const Series<double>& BacktestEngine::get_input_source(
        const std::string& key, const Series<double>& default_series) const {
    auto it = inputs_.find(key);
    if (it == inputs_.end()) return default_series;
    const std::string& v = it->second;
    if (v == "open")   return _src_open_;
    if (v == "high")   return _src_high_;
    if (v == "low")    return _src_low_;
    if (v == "close")  return _src_close_;
    if (v == "volume") return _src_volume_;
    if (v == "hl2")    return _src_hl2_;
    if (v == "hlc3")   return _src_hlc3_;
    if (v == "ohlc4")  return _src_ohlc4_;
    if (v == "hlcc4")  return _src_hlcc4_;
    // Non-native override string (only reachable via an operator-supplied
    // input value; analyzer rejects non-native defvals). Fall back to the
    // codegen-resolved default rather than crash.
    return default_series;
}


// --- Full run() overload with SymInfo, StrategyOverrides, and input injection ---
void BacktestEngine::run(const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          const std::unordered_map<std::string, std::string>& inputs,
                          const SymInfo& syminfo,
                          const StrategyOverrides* overrides,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) {
    last_error_.clear();
    try {
    // Store syminfo and inputs
    syminfo_ = syminfo;
    syminfo_mintick_ = syminfo.mintick;
    // Forced-liquidation lot step (0 = disabled). On the codegen run(Bar*,n)
    // path this member is fed via set_syminfo_metadata("qty_step", …) and is
    // never reset; on this explicit-SymInfo path the struct is authoritative.
    if (std::isfinite(syminfo.qty_step) && syminfo.qty_step > 0.0)
        qty_step_ = syminfo.qty_step;
    inputs_ = inputs;

    // Apply overrides
    if (overrides) {
        if (!std::isnan(overrides->initial_capital))
            initial_capital_ = overrides->initial_capital;
        if (overrides->pyramiding >= 0)
            pyramiding_ = overrides->pyramiding;
        if (overrides->slippage >= 0)
            slippage_ = overrides->slippage;
        if (!std::isnan(overrides->commission_value))
            commission_value_ = overrides->commission_value;
        if (overrides->commission_type >= 0)
            commission_type_ = static_cast<CommissionType>(overrides->commission_type);
        if (!std::isnan(overrides->default_qty_value))
            default_qty_value_ = overrides->default_qty_value;
        if (overrides->default_qty_type >= 0)
            default_qty_type_ = static_cast<QtyType>(overrides->default_qty_type);
        if (overrides->process_orders_on_close >= 0)
            process_orders_on_close_ = (overrides->process_orders_on_close != 0);
        if (overrides->calc_on_order_fills >= 0)
            calc_on_order_fills_ = (overrides->calc_on_order_fills != 0);
        if (overrides->close_entries_rule >= 0)
            close_entries_rule_any_ = (overrides->close_entries_rule != 0);
    }

    // Delegate to the TF-aware run
    run(input_bars, n_input, input_tf, script_tf, bar_magnifier, magnifier_samples, magnifier_dist);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}

}  // namespace pineforge
