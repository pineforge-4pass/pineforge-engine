/*
 * engine_run.cpp — public run() entrypoints + run_magnified_bar + get_input_*
 */

#include "engine_internal.hpp"

#include <pineforge/ta.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;

namespace {
// A callback owns its origin, not an engine clone. Nesting and exceptions restore
// the previous owner; copying an engine cannot inherit a live callback token.
thread_local const BacktestEngine* birth_context_owner = nullptr;
thread_local std::optional<OrderBirth> birth_context;
class ScopedBirthContext {
public:
    ScopedBirthContext(const BacktestEngine* owner, const OrderBirth& birth)
        : previous_owner_(birth_context_owner), previous_(birth_context) {
        birth_context_owner = owner; birth_context = birth;
    }
    ~ScopedBirthContext() {
        birth_context_owner = previous_owner_; birth_context = previous_;
    }
private:
    const BacktestEngine* previous_owner_;
    std::optional<OrderBirth> previous_;
};
[[noreturn]] void reject_chart_bar(int index, const char* rule) {
    throw std::invalid_argument("chart bar[" + std::to_string(index) + "]." + rule);
}

// Structural admission only: no price-domain, grid, calendar or financial
// arithmetic policy. Scan the entire supplied array before any run mutation.
// Each public run() calls this once; run_tf_impl receives validated input.
void validate_chart_bars(const Bar* bars, int n) {
    if (n < 0) throw std::invalid_argument("chart bar count must be non-negative");
    if (n > 0 && bars == nullptr)
        throw std::invalid_argument("chart bars must be non-null for a nonempty array");
    for (int i = 0; i < n; ++i) {
        const Bar& bar = bars[i];
        if (!std::isfinite(bar.open)) reject_chart_bar(i, "open must be finite");
        if (!std::isfinite(bar.high)) reject_chart_bar(i, "high must be finite");
        if (!std::isfinite(bar.low)) reject_chart_bar(i, "low must be finite");
        if (!std::isfinite(bar.close)) reject_chart_bar(i, "close must be finite");
        if (bar.low > std::min(bar.open, bar.close))
            reject_chart_bar(i, "low must not exceed open or close");
        if (bar.high < std::max(bar.open, bar.close))
            reject_chart_bar(i, "high must not be below open or close");
        // NaN is unavailable activity, distinct from a known zero total.
        if (!std::isnan(bar.volume) && (!std::isfinite(bar.volume) || bar.volume < 0))
            reject_chart_bar(i, "volume must be non-negative finite or NaN (unavailable)");
        if (i > 0) {
            const int64_t previous = bars[i - 1].timestamp;
            if (bar.timestamp <= previous)
                reject_chart_bar(i, "timestamp must be strictly increasing");
            // With increasing signed values, a difference can overflow only
            // when the previous timestamp is negative. This addition is safe;
            // do not subtract the timestamps before checking representability.
            if (previous < 0 && bar.timestamp > std::numeric_limits<int64_t>::max() + previous)
                reject_chart_bar(i, "timestamp delta exceeds int64 range");
        }
    }
}

// ABI v4 live-runtime surface (task 4): installs this run's forced path
// order as the thread-local internal::bar_path_uses_high_first override for
// exactly the duration of the scope, restoring whatever override value was
// in effect before it (not unconditionally AUTO) on every exit path --
// normal return or an exception unwinding through a `try`. Restoring the
// PRIOR value rather than hardcoding 0 is future-proofed against a caller
// ever nesting two overridden runs on the same thread; today there is no
// such nesting (each public run() entrypoint reaches exactly one of the two
// installation sites below, see the single-TF run() and run_tf_impl), so in
// practice the prior value is always AUTO (0). One file-scope definition
// shared by both installation sites instead of a duplicated local struct.
struct PathOrderScope {
    int prev;
    explicit PathOrderScope(int mode) : prev(internal::path_order_override()) {
        internal::set_path_order_override(mode);
    }
    ~PathOrderScope() { internal::set_path_order_override(prev); }
};
}  // namespace

bool BacktestEngine::set_account_currency_fx_series(
        const int64_t* timestamps_ms, const double* rates, int n) {
    guard_native_mutation("set_account_currency_fx_series");
    // Timestamped FX is not route-complete for the realtime scheduler. Reject
    // late installation as well as stream_begin-with-series so callers cannot
    // bypass fail-closed behavior by changing configuration after warmup.
    if (stream_phase_ == StreamPhase::REALTIME || stream_warmup_mode_) {
        return false;
    }
    if (n < 0 || (n > 0 && (!timestamps_ms || !rates))) return false;
    if (n == 0) {
        account_currency_fx_timestamps_.clear();
        account_currency_fx_rates_.clear();
        return true;
    }

    std::vector<int64_t> next_timestamps;
    std::vector<double> next_rates;
    next_timestamps.reserve(static_cast<std::size_t>(n));
    next_rates.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if ((i > 0 && timestamps_ms[i] <= timestamps_ms[i - 1])
            || !std::isfinite(rates[i]) || rates[i] <= 0.0) {
            return false;
        }
        next_timestamps.push_back(timestamps_ms[i]);
        next_rates.push_back(rates[i]);
    }
    account_currency_fx_timestamps_ = std::move(next_timestamps);
    account_currency_fx_rates_ = std::move(next_rates);
    return true;
}

double BacktestEngine::account_currency_fx_at(int64_t timestamp_ms) const {
    if (account_currency_fx_timestamps_.empty()) {
        return account_currency_fx_;
    }
    const auto it = std::upper_bound(
        account_currency_fx_timestamps_.begin(),
        account_currency_fx_timestamps_.end(), timestamp_ms);
    if (it == account_currency_fx_timestamps_.begin()) {
        return account_currency_fx_;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(account_currency_fx_timestamps_.begin(), it) - 1);
    return account_currency_fx_rates_[index];
}

double BacktestEngine::active_account_currency_fx() const {
    return account_currency_fx_at(current_bar_.timestamp);
}


// open_trade_* accessors moved to engine_trade_accessors.cpp.

// Invoke the generated chart strategy body under its own EMA warmup mode.
// The selector is thread-local because multiple engines can run concurrently;
// restoring the previous value (also during stack unwinding) prevents both
// cross-engine contamination and leakage between chart and request.security
// evaluation. The latter installs its own scope around every security
// evaluator dispatch and restores the prior thread-local value on return.
OrderBirth BacktestEngine::capture_order_birth() const {
    if (birth_context_owner == this && birth_context) return *birth_context;
    return OrderBirth::direct_command(bar_index_, current_bar_.timestamp);
}

void BacktestEngine::invoke_chart_on_bar(const Bar& bar) {
    const OrderBirth origin = birth_context_owner == this && birth_context
        ? *birth_context : OrderBirth::chart_evaluation(bar_index_, bar.timestamp);
    ScopedBirthContext origin_scope(this, origin);
    process_short_margin_before_script(bar);
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

    // Bar-addressed window state (ta::bar_context()): the chart context's TA
    // members address their rings by the Pine bar_index the script sees, and
    // warm up from the feed's first bar (pine index bar_index_offset_).
    // Every tick of one script bar (compute() then recompute() under the bar
    // magnifier) shares the index, so they rewrite the same slot.
    ta::BarContextScope bar_scope(pine_bar_index(), bar_index_offset_);

    named_entry_cancelled_incarnation_in_current_eval_.clear();
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
    // ABI v4 live-runtime surface (task 4): reset the per-bar dual-entry-stop
    // arbitration snapshot once per bar, before anything else -- including
    // the COOF early return below, so a calc_on_order_fills_ bar (which never
    // writes this snapshot) correctly reads None instead of a stale value
    // left by an earlier standard-path bar. See last_bar_dual_entry_decision_
    // (engine.hpp) and its write site (engine_fills.cpp).
    last_bar_dual_entry_decision_ = internal::DualEntryStopPathWinner::None;
    if (calc_on_order_fills_) {
        dispatch_bar_calc_on_order_fills();
        return;
    }
    // strategy.risk.max_intraday_loss: the day-start equity is the first
    // tick of the chart-tz day, before any fill at it.
    intraday_loss_begin_bar(current_bar_);

    // A confirmed timestamped FX point is consumed at the broker boundary,
    // before any resting order or the close-time script body can observe the
    // position.  Restrict current_bar_ to the opening point while the broker
    // emits the forced exit so the trade cannot inherit future high/low state
    // from the script bar.
    {
        const Bar script_bar = current_bar_;
        current_bar_ = Bar{script_bar.open, script_bar.open, script_bar.open,
                           script_bar.open, 0.0, script_bar.timestamp};
        try {
            process_carried_position_fx_rollover(script_bar);
            // finding-430: a carried leveraged position already in margin
            // deficit at the open is sliced here, at the open price, before
            // any resting order sees the bar. The survivor's adverse-extreme
            // check (pre-exit hook / end-of-bar process_margin_call) is
            // unchanged and may book TV's second same-bar slice.
            margin_call_slice_at_bar_open(script_bar);
            if (process_orders_on_close_ && slippage_ > 0) {
                tv_money_long_margin_call(script_bar,
                    /*carried_pooc_pre_close=*/true, /*opening_only=*/true);
            }
        } catch (...) {
            current_bar_ = script_bar;
            throw;
        }
        current_bar_ = script_bar;
    }

    // A C-factor inheritance is same-ordinary-bar state. A candidate erased
    // by replacement/OCA/cancel never reaches the fill kernel, so discard any
    // stale identity before starting the next broker batch.
    max_intraday_filled_orders_.ordinary_open(bar_index_);

    // Opt-in POOC intraday-cap candidate: the position left by the prior
    // close's quota-triggering MARKET attempt owns a flatten due at this
    // next broker boundary. Consume it before resting orders and path sampling,
    // even if quota renews today; a replacement cycle cannot inherit it.
    if (const auto request = position_close_obligation_.take_at_open(bar_index_, position_cycle_seq_)) {
        if (position_side_ != PositionSide::FLAT) {
            const size_t trades_before = trades_.size();
            const PositionSide side_before = position_side_;
            const double qty_before = position_qty_;
            execute_market_exit(bar_fill_price(current_bar_.open));
            if (position_side_ != side_before
                || std::abs(position_qty_ - qty_before) > kQtyEpsilon
                || trades_.size() != trades_before) {
                ++broker_fill_event_seq_;
            }
            for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
                trades_[ti].exit_comment = request->comment;
                trades_[ti].exit_id = "";
            }
        }
    }

    if (probe_suppress_tail_logic_ && is_tail_bar_) {
        // Live probe (spec §3.2): the forming bar runs only the pre-on_bar
        // steps, so the run's last-bar fills are the settled book's fills
        // against the forming bar and the post-run book is the in-force book.
        _push_source_series();
        process_pending_orders(current_bar_);
        evaluate_max_intraday_loss_over_path(current_bar_);
        update_per_trade_extremes();
        return;
    }

    // Advance native source-series history before strategy logic so
    // get_input_source()'s returned series is current for this bar. Covers
    // the simple run() loop, run_simple_bar_loop, and the no-magnifier
    // aggregation path (all route through dispatch_bar). The magnifier path
    // inlines its own on_bar call and pushes there instead.
    _push_source_series();
    if (process_orders_on_close_) {
        const bool no_pending_broker_orders = pending_orders_.empty();
        const uint64_t fills_before_pending = broker_fill_event_seq_;
        process_pending_orders(current_bar_, /*before_pooc_script=*/true); // step 1: old stop/limit
        evaluate_max_intraday_loss_over_path(current_bar_);
        // Round 13 D: the carried 1x-long rounded-money event belongs before
        // the close-time script. TV's full/30% close pins read the already
        // reduced position here; an end-of-bar check would see the script's
        // flattened/reduced state instead. The helper refuses pending-order
        // interactions and every fresh entry, so it cannot replay a close
        // fill's past path or move an existing broker fill across the event.
        if (no_pending_broker_orders
            && broker_fill_event_seq_ == fills_before_pending) {
            tv_money_long_margin_call(current_bar_, /*carried_pooc_pre_close=*/true);
        }
        if (broker_fill_event_seq_ == fills_before_pending) {
            process_carried_pooc_short_margin_before_script(current_bar_);
        }
        update_per_trade_extremes();             // step 2: update before strategy reads
        invoke_chart_on_bar(current_bar_);       // step 3: strategy logic
        flush_same_bar_close();                  // step 3b: surviving strategy.close fill
        process_pending_orders(current_bar_);    // step 4: new market orders
        max_intraday_filled_orders_.source_batch_end();
    } else {
        process_pending_orders(current_bar_);
        evaluate_max_intraday_loss_over_path(current_bar_);
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
        settle_dormant_bracket_reissues(exit_legs::Domain::Ordinary);
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
    coof_checkpoint_prev_chart_close_ = prev_chart_close_;   // issue #178
    coof_checkpoint_last_chart_close_ = last_chart_close_;
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
    prev_chart_close_ = coof_checkpoint_prev_chart_close_;   // issue #178
    last_chart_close_ = coof_checkpoint_last_chart_close_;
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
    coof_checkpoint_prev_chart_close_ = prev_chart_close_;   // issue #178
    coof_checkpoint_last_chart_close_ = last_chart_close_;
    commit_script_state();
    coof_checkpoint_contains_current_bar_ = true;
}

uint64_t BacktestEngine::execute_coof_script_body(
        const Bar& script_bar,
        double broker_cursor_price,
        bool cursor_is_bar_point,
        const OrderBirth& evaluation_origin,
        uint64_t direct_fill_event_budget,
        bool opening_money_prefix) {
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
    if (opening_money_prefix) {
        // The new broker event and its direct-close callbacks are still at O.
        // Pine sees the complete historical bar; these physical exits cannot
        // inherit that bar's future extremes before its path has advanced.
        current_bar_.high = current_bar_.low = current_bar_.close = script_bar.open;
        update_per_trade_extremes();
        current_bar_ = script_bar;
    } else {
        update_per_trade_extremes();
    }

    coof_scheduler_active_ = true;
    coof_fill_recalc_active_ = evaluation_origin.from_fill();
    coof_cursor_is_bar_close_ = evaluation_origin.from_fill()
        ? evaluation_origin.cursor().terminal_point() : true;
    // KI-67: only the first fill event at O owns "bar-open" provenance and
    // places standard orders. A later fill at the same O, like a fill at any
    // segment/extreme/close point, is mid-bar and places cascade orders.
    coof_recalc_at_bar_open_ = compat::pine::first_open_fill_evaluation(evaluation_origin);
    coof_cursor_price_ = broker_cursor_price;
    coof_cursor_is_bar_point_ = cursor_is_bar_point;
    coof_direct_fill_events_remaining_ = direct_fill_event_budget;
    const uint64_t before = broker_fill_event_seq_;
    ScopedBirthContext origin_scope(this, evaluation_origin);
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
    coof_market_entry_recalc_incarnation_ = 0;
    coof_market_entry_recalc_fill_seq_ = 0;
    coof_direct_fill_events_remaining_ = 0;
    return broker_fill_event_seq_ - before;
}

uint64_t BacktestEngine::run_coof_recalc_chain(
        const Bar& script_bar, double broker_cursor_price,
        bool cursor_is_bar_point, BirthCursor cursor,
        uint64_t& evaluation_ordinal, uint64_t triggering_events,
        uint64_t max_events, uint64_t events_already,
        bool grouped_stop_recalc, uint64_t market_entry_incarnation,
        bool opening_money_prefix) {
    // Bind callbacks to the actual simulator events that scheduled them. Direct
    // fills append their exact sequence interval to this FIFO; a later callback
    // never borrows the newest global sequence as its alleged triggering fill.
    using Interval = std::pair<uint64_t, uint64_t>;
    std::deque<Interval> pending;
    auto append_events = [&](uint64_t first, uint64_t last, bool grouped) {
        if (first == 0 || last < first) throw std::logic_error("invalid callback fill interval");
        if (grouped) pending.emplace_back(first, last);
        else for (uint64_t seq = first;; ++seq) {
            pending.emplace_back(seq, seq);
            if (seq == last) break;
        }
    };
    if (triggering_events > broker_fill_event_seq_)
        throw std::logic_error("callback interval exceeds committed fills");
    if (triggering_events > 0)
        append_events(broker_fill_event_seq_ - triggering_events + 1,
                      broker_fill_event_seq_, grouped_stop_recalc);
    uint64_t total_events = triggering_events;
    uint64_t handled = 0;
    while (!pending.empty() && events_already + handled < max_events) {
        const auto trigger = pending.front(); pending.pop_front(); ++handled;
        const auto origin = OrderBirth::fill_evaluation(
            bar_index_, script_bar.timestamp, cursor, broker_cursor_price,
            trigger.first, trigger.second, ++evaluation_ordinal);
        const uint64_t used = events_already + total_events;
        const uint64_t direct_budget = used < max_events ? max_events - used : 0;
        coof_recalc_after_first_open_fill_ = cursor.first_point()
            && !compat::pine::first_open_fill_evaluation(origin);
        coof_market_entry_recalc_incarnation_ = handled == 1 ? market_entry_incarnation : 0;
        coof_market_entry_recalc_fill_seq_ = broker_fill_event_seq_;
        const uint64_t before = broker_fill_event_seq_;
        const uint64_t direct = execute_coof_script_body(
            script_bar, broker_cursor_price, cursor_is_bar_point,
            origin, direct_budget, opening_money_prefix);
        total_events += direct;
        if (direct > 0) append_events(before + 1, broker_fill_event_seq_, false);
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
    intraday_loss_begin_bar(script_bar);
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
    uint64_t evaluation_ordinal = 0;
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
    coof_market_entry_recalc_incarnation_ = 0;
    coof_market_entry_recalc_fill_seq_ = 0;

    double path[4];
    fill_bar_path_points(script_bar, path);
    double cursor = path[0];
    // strategy.risk.max_intraday_loss at the open tick, before its fills
    // (a no-op on the day's first bar, whose open is the day-start mark).
    if (evaluate_max_intraday_loss(path[0], 0.0)) {
        finish_intraday_loss_cancel();
    }
    // finding-446: a strategy.close booked at the cursor is a raw-bar-price
    // fill only while the cursor sits on an OHLC path point; a fill-price
    // cursor is already in its booked shape (see coof_cursor_is_bar_point_).
    bool cursor_is_bar_point = true;
    int next_waypoint = 1;
    bool evaluate_current_point = true;

    // A carried positive-slip POOC market lot checks rounded money at O,
    // before any pending fill. The helper owns the one broker event and its
    // consumed-bar stamp. Recalc valuation stays at raw O: each actual exit
    // pays its own slippage, and later orders advance on the unchanged path.
    if (process_orders_on_close_ && slippage_ > 0) {
        current_bar_ = coof_point_bar(script_bar, cursor);
        coof_hist_path_index_ = 0;
        const uint64_t before = broker_fill_event_seq_;
        if (tv_money_long_margin_call(script_bar,
                /*carried_pooc_pre_close=*/true, /*opening_only=*/true)) {
            coof_cascade_recalc_leg_ = 0;
            fill_events += run_coof_recalc_chain(
                script_bar, cursor, /*cursor_is_bar_point=*/true,
                BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 4),
                evaluation_ordinal, broker_fill_event_seq_ - before, kNoFillEventBudget, 0,
                /*grouped_stop_recalc=*/false, /*market_entry_incarnation=*/0,
                /*opening_money_prefix=*/true);
            // The ordinary O exception permits just the first follow-up
            // fill at O. A direct survivor close already consumed that slot.
            evaluate_current_point = fill_events == 1;
        }
    }

    auto consume_fill = [&](const CoofFillResult& fill,
                            BirthCursor birth_cursor,
                            bool filled_at_bar_open_point) {
        const uint64_t before = fill_events;
        const bool chart_tick_touch = std::isfinite(fill.chart_waypoint_price);
        cursor = chart_tick_touch ? fill.chart_waypoint_price : fill.fill_price;
        cursor_is_bar_point = chart_tick_touch;
        // The recalc chain receives O-point provenance, but only its first fill
        // event is classified as bar-open. A later fill at the same O is a
        // leg-0 cascade (the Pine historical cascade permission).
        fill_events += run_coof_recalc_chain(
            script_bar, fill.fill_price, /*cursor_is_bar_point=*/false,
            birth_cursor, evaluation_ordinal, fill.fill_events, kNoFillEventBudget, fill_events,
            fill.grouped_stop_recalc, fill.market_entry_incarnation);
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
                exit_closed_was_long, &script_bar);
            if (fill.filled) {
                // A fill at this POINT (cursor == path[next_waypoint-1]) puts the
                // in-flight leg at path[next_waypoint-1] -> path[next_waypoint],
                // i.e. leg (next_waypoint-1) — the leg the loop traverses next.
                coof_cascade_recalc_leg_ = next_waypoint - 1;
                consume_fill(
                    fill, BirthCursor::point(BirthCursorDomain::HistoricalPath, next_waypoint - 1, 4),
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
            // A fill mid-leg leaves the in-flight leg at (next_waypoint-1); a fill
            // that reaches the leg-end waypoint (path[next_waypoint]) advances to
            // the NEXT leg (next_waypoint) — the loop's ++next_waypoint below.
            coof_cascade_recalc_leg_ =
                reached_target ? next_waypoint : (next_waypoint - 1);
            consume_fill(
                fill, reached_target
                    ? BirthCursor::point(BirthCursorDomain::HistoricalPath, next_waypoint, 4)
                    : BirthCursor::segment(BirthCursorDomain::HistoricalPath, next_waypoint - 1, 4),
                /*filled_at_bar_open_point=*/false);
            // H/L/C itself has been consumed by this priced fill. Only O has
            // the same-point two-fill exception; a market order born in the
            // recalc must wait for the next historical waypoint.
            if (reached_target) ++next_waypoint;
            continue;
        }

        cursor = target;
        cursor_is_bar_point = true;
        // strategy.risk.max_intraday_loss at the waypoint the broker reached
        // without a fill on the leg: the position marked at the extreme /
        // close (t6: the held short closed at the high 71751.33).
        if (evaluate_max_intraday_loss(target, 0.0)) {
            finish_intraday_loss_cancel();
        }
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
    cursor_is_bar_point = true;
    uint64_t direct = execute_coof_script_body(
        script_bar, cursor, cursor_is_bar_point,
        OrderBirth::chart_evaluation(bar_index_, script_bar.timestamp),
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
    settle_dormant_bracket_reissues(exit_legs::Domain::Coof);
    if (trades_.size() != trades_before_mc) {
        refresh_frozen_default_sizing_after_margin_call();
    }
    const uint64_t margin_events = broker_fill_event_seq_ - fill_seq_before_mc;
    if (margin_events > 0) {
        fill_events += run_coof_recalc_chain(
            script_bar, cursor, cursor_is_bar_point,
            BirthCursor::point(BirthCursorDomain::HistoricalPath, 3, 4),
            evaluation_ordinal, margin_events,
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
    coof_market_entry_recalc_incarnation_ = 0;
    coof_market_entry_recalc_fill_seq_ = 0;
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
    range_end_trades_.clear();
    net_profit_sum_ = 0.0;
    net_profit_roundoff_bound_ = 0.0;
    net_profit_roundoff_value_ = 0.0;
    gross_profit_sum_ = 0.0;
    gross_loss_sum_ = 0.0;
    win_trades_count_ = 0;
    loss_trades_count_ = 0;
    eventrades_count_ = 0;

    // Open position + pending orders.
    reset_position_state_to_flat();   // position_side_/qty/price/time/count,
                                      // pyramid_entries_, trail, partial ids
    // Cycle ownership is scoped to this run, like order incarnations below.
    // A flat transition within a run must keep advancing it; only a new run
    // returns the allocator to its constructor value.
    next_position_cycle_seq_ = 1;
    pending_orders_.clear();
    // PendingOrder incarnations are report provenance scoped to one run.
    // Resetting keeps a reused handle byte/identity-equivalent to a fresh
    // handle while preserving the invariant that zero means unavailable.
    next_order_incarnation_ = 1;
    exit_leg_event_seq_ = 0;
    next_order_seq_ = 1;
    market_admission_journal_.reset();
    named_entry_cancelled_incarnation_in_current_eval_.clear();
    pending_close_qty_in_bar_ = 0.0;
    pos_view_freeze_bar_ = -1;   // KI-64: fresh run starts with no frozen view
    pos_view_frozen_side_ = PositionSide::FLAT;
    pos_view_frozen_qty_ = 0.0;
    pos_view_frozen_entry_qty_.clear();
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
    callsite_close_bar_ = -1;
    callsite_close_queue_seq_ = 0;
    callsite_close_callsites_.clear();
    callsite_close_admitted_total_ = 0.0;
    callsite_close_reserved_qty_.clear();
    callsite_close_two_call_first_qty_.clear();
    fold_exit_path_extremes_ = false;
    fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
    last_exit_fill_was_trail_ = false;
    trail_best_before_bar_ = std::numeric_limits<double>::quiet_NaN();
    trail_best_before_bar_index_ = -1;
    trail_best_before_bar_position_cycle_ = 0;
    trail_best_before_bar_fill_seq_ = 0;
    priced_entry_activity_bar_ = -1;
    priced_entry_filled_this_bar_ = false;
    open_margin_slice_bar_ = -1;

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
    broker_state_hashes_.clear();    // ABI v4 task 6: retain capacity like equity_curve_

    // Risk halt latch + day trackers (one-way halt must not survive a rerun).
    risk_halted_ = false;
    cons_loss_day_count_ = 0;
    last_loss_day_ = -1;
    intraday_pnl_ = 0.0;
    intraday_pnl_day_ = -1;
    intraday_loss_day_start_equity_ = std::numeric_limits<double>::quiet_NaN();
    intraday_loss_day_ = -1;
    intraday_loss_block_day_ = -1;
    intraday_loss_evaluating_ = false;
    intraday_loss_cancel_pending_ = false;
    max_intraday_filled_orders_.reset_run();
    position_close_obligation_ = {};
    broker_fill_event_seq_ = 0;
    last_margin_call_event_bar_ = -1;        // finding-308: bar-keyed one-shot
    intrabar_exit_margin_call_bar_ = -1;     // markers must not survive a rerun
    coof_scheduler_active_ = false;
    coof_fill_recalc_active_ = false;
    coof_recalc_at_bar_open_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_market_entry_recalc_incarnation_ = 0;
    coof_market_entry_recalc_fill_seq_ = 0;
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
    // ABI v4 task 4 fix (final review F6): a run that dispatches zero
    // script bars never reaches dispatch_bar()'s own per-bar reset (top of
    // dispatch_bar(), engine_run.cpp), which would otherwise leave a reused
    // handle's last_bar_dual_entry_decision_ (also hashed by
    // engine_state_hash.cpp) reading the PREVIOUS run's value.
    last_bar_dual_entry_decision_ = internal::DualEntryStopPathWinner::None;
    trail_close_restart_bar_ = -1;
    prev_bar_timestamp_ = 0;
    // The chart's native daily partition is rebuilt per run by the
    // multi-timeframe run() (prepare_chart_day_partition); a run that never
    // builds one must not report a stale one.
    chart_day_partition_ = NativeDayPartition{};
    account_currency_fx_broker_epoch_initialized_ = false;
    account_currency_fx_broker_epoch_ = 0;
    account_currency_fx_broker_rate_ = account_currency_fx_;
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
    stream_input_mode_ = StreamInputMode::UNSET;
    stream_observe_actions_ = false;
    stream_action_sequence_ = 0;
    stream_order_actions_.clear();

    // Native source-series history (input.source(...) ring buffers). Must list
    // EVERY _src_*_ member declared in engine.hpp — a missing one leaks history
    // into a reused handle (see test_handle_reuse_reset all-series coverage).
    _src_open_.clear();
    _src_high_.clear();
    _src_low_.clear();
    _src_close_.clear();
    _src_volume_.clear();
    prev_chart_close_ = std::numeric_limits<double>::quiet_NaN();  // issue #178
    last_chart_close_ = std::numeric_limits<double>::quiet_NaN();
    coof_checkpoint_prev_chart_close_ = std::numeric_limits<double>::quiet_NaN();
    coof_checkpoint_last_chart_close_ = std::numeric_limits<double>::quiet_NaN();
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


void BacktestEngine::legacy_run_simple(const Bar* bars, int n) {
    last_error_.clear();
    last_run_status_ = 0;
    abort_requested_.store(false, std::memory_order_relaxed);
    try {
    validate_chart_bars(bars, n);
    if (n > 0 && bars != nullptr) {
        last_bar_time_ = bars[n - 1].timestamp;
        last_bar_index_ = n - 1;
    } else {
        last_bar_time_ = 0;
        last_bar_index_ = 0;
    }
    // ABI v4 live-runtime surface (task 4): install this run's forced path
    // order for exactly the duration of this call (see the file-scope
    // PathOrderScope above).
    PathOrderScope path_order_scope(path_order_mode_);
    if (!account_currency_fx_timestamps_.empty() && calc_on_order_fills_) {
        throw std::runtime_error(
            "timestamped account-currency FX does not support calc_on_order_fills");
    }
    reset_run_state();
    prepare_script_run(bars, n, !stream_warmup_mode_);
    equity_curve_.reserve((size_t)std::max(n, 0));

    std::string detected_tf = "";
    if (n >= 2 && bars != nullptr) {
        detected_tf = detect_timeframe(bars, n);
    }
    input_tf_ = detected_tf;
    script_tf_ = detected_tf;
    script_tf_seconds_ = tf_to_seconds(script_tf_);
    // Single-TF path: bars IS the script-bar array (input_tf == script_tf
    // trivially, no aggregation), so the exact/extrapolate-from-last rule
    // applies.
    apply_realtime_tail_horizon(bars, n, /*script_bar_geometry=*/true);

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
        state.ta_bar_index = -1;
    }

    for (int i = 0; i < n; i++) {
        check_abort();
        current_bar_ = bars[i];
        bar_index_ = i;
        is_tail_bar_ = (i == n - 1);
        is_first_tick_ = true;
        is_last_tick_ = true;
        barstate_islast_ = !stream_warmup_mode_ && !realtime_tail_ && (i == n - 1);
        diag_script_bars_processed_++;
        // Reset per-bar pending-close accumulator. Each on_bar call
        // captures fresh ``strategy.close*`` qty for the same-bar
        // close-then-entry source-order rule (see engine.hpp).
        pending_close_qty_in_bar_ = 0.0;
        dispatch_bar();
        update_equity_extremes();
        record_equity_point(current_bar_.timestamp);  // ts not mutated on this path
        if (broker_state_hash_recording_) broker_state_hashes_.push_back(broker_state_hash());
        prev_bar_timestamp_ = current_bar_.timestamp;
    }
    // TradingView's range-end accounting: a position still open after the
    // last bar is reported as a closed trade at that bar's close
    // (record_range_end_close_trades, engine_orders.cpp). Report-only:
    // the live position is untouched. Skipped under the live-runtime tail
    // (spec §3.1): the last bar is still forming, so it never gets a
    // synthetic range-end close row.
    if (!realtime_tail_) record_range_end_close_trades();
    } catch (const AbortRequested&) {
        last_run_status_ = 1;
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}


// Live-runtime tail (spec §3.1): once script_tf_seconds_ is known for this
// run, freeze pine_last_bar_index()/last_bar_time_ at the horizon bar
// instead of the fed array's actual last index/timestamp. No-op unless
// realtime_tail_ is on and a positive horizon was configured.
//
// The `bars` array passed in is script-bar geometry only when the caller
// says so (script_bar_geometry == true): the single-TF run(bars, n) path,
// and run_tf_impl's !needs_aggregation call where input_tf == script_tf
// makes input bars the same as script bars. There, last_bar_time_ is the
// EXACT timestamp of bars[horizon_bars - 1] when that bar exists in the fed
// array (horizon_bars <= n); otherwise it is extrapolated from the array's
// actual final bar (bars[n - 1]), not the first one -- a feed with any gap
// (session/weekend boundary, a missing bar, a calendar TF) makes an
// extrapolation from bars[0] wrong even when the exact timestamp was
// available.
//
// Under aggregation (input_tf < script_tf, script_bar_geometry == false)
// `bars` is the *input* array, so a script-bar horizon does not index it
// correctly (final-rereview.md N1): last_bar_time_ is instead extrapolated
// from the first input bar's timestamp, one script-TF step per horizon bar
// -- the formula this function used unconditionally before the exact/
// extrapolate-from-last-bar fix, restored here for this path only.
void BacktestEngine::apply_realtime_tail_horizon(const Bar* bars, int n,
                                                  bool script_bar_geometry) {
    if (!realtime_tail_ || realtime_tail_horizon_bars_ <= 0 || n <= 0 || bars == nullptr) return;
    const int horizon = realtime_tail_horizon_bars_;
    last_bar_index_ = horizon - 1;
    const int64_t script_tf_ms =
        static_cast<int64_t>(script_tf_seconds_ > 0 ? script_tf_seconds_ : 0) * 1000;
    if (script_bar_geometry) {
        if (horizon <= n) {
            last_bar_time_ = bars[horizon - 1].timestamp;
        } else {
            last_bar_time_ = bars[n - 1].timestamp
                + static_cast<int64_t>(horizon - n) * script_tf_ms;
        }
    } else {
        last_bar_time_ = bars[0].timestamp
            + static_cast<int64_t>(horizon - 1) * script_tf_ms;
    }
}


// --- run_magnified_bar ---
void BacktestEngine::run_magnified_bar(
        const std::vector<Bar>& sub_bars, int64_t script_bar_ts,
        bool caller_completed_on_boundary) {
    if (sub_bars.empty()) return;
    if (calc_on_order_fills_) {
        run_magnified_bar_calc_on_order_fills(
            sub_bars, script_bar_ts, caller_completed_on_boundary);
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
    // The input bar after this group, set by the caller; each sub-bar's
    // successor inside the group is known here.
    const int64_t after_group_ms = security_next_input_ms_;

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

    // finding-430: the script bar's open is the first point of every
    // sub-bar path. A carried leveraged position already in deficit there is
    // sliced at the open before the first sub-bar's samples are walked.
    {
        const Bar open_point{bar_open, bar_open, bar_open, bar_open, 0.0,
                             timestamp};
        current_bar_ = open_point;
        margin_call_slice_at_bar_open(open_point);
    }

    for (int si = 0; si < total_sub; ++si) {
        const Bar& sb = sub_bars[si];
        cumulative_vol += sb.volume;
        timestamp = sb.timestamp;

        security_next_input_ms_ = (si + 1 < total_sub)
            ? sub_bars[static_cast<std::size_t>(si + 1)].timestamp
            : after_group_ms;
        // Feed security evaluators with each sub-bar
        for (auto& state : security_eval_states_) {
            if (caller_completed_on_boundary
                && state.publish_gate_tf_seconds > 0
                && si == total_sub - 1) {
                // This retained input belongs to the next caller. The outer
                // loop feeds it after the completed chart body dispatches.
                continue;
            }
            feed_security_eval_state(
                state, sb,
                caller_completed_on_boundary
                    ? si == total_sub - 2
                    : si == total_sub - 1);
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
        settle_dormant_bracket_reissues(exit_legs::Domain::Magnifier);
        if (trades_.size() != trades_before_mc) {
            refresh_frozen_default_sizing_after_margin_call();
        }
    }
    finalize_bar();
}

void BacktestEngine::run_magnified_bar_calc_on_order_fills(
        const std::vector<Bar>& sub_bars,
        int64_t script_bar_ts,
        bool caller_completed_on_boundary) {
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
    const int64_t after_group_ms = security_next_input_ms_;
    for (int si = 0; si < total_sub; ++si) {
        const Bar& sb = sub_bars[static_cast<std::size_t>(si)];
        security_next_input_ms_ = (si + 1 < total_sub)
            ? sub_bars[static_cast<std::size_t>(si + 1)].timestamp
            : after_group_ms;
        // Historical script executions see the completed security state for
        // the script bar. Feeding all committed lower-TF bars before taking
        // the script-state checkpoint mirrors the standard path, where
        // security evaluators are fed before dispatch_bar.
        for (auto& state : security_eval_states_) {
            if (caller_completed_on_boundary
                && state.publish_gate_tf_seconds > 0
                && si == total_sub - 1) {
                continue;
            }
            feed_security_eval_state(
                state, sb,
                caller_completed_on_boundary
                    ? si == total_sub - 2
                    : si == total_sub - 1);
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
    uint64_t evaluation_ordinal = 0;
    int exit_closed_from_bar = -1;
    uint64_t exit_closed_from_incarnation = 0;
    bool exit_closed_was_long = false;
    snapshot_coof_script_state();
    coof_scheduler_active_ = true;
    coof_cursor_is_bar_close_ = false;
    coof_evaluating_path_segment_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_market_entry_recalc_incarnation_ = 0;
    coof_market_entry_recalc_fill_seq_ = 0;

    double cursor = ticks.front().price;
    bool cursor_is_bar_point = true;  // finding-446, see the simple loop
    int64_t cursor_ts = ticks.front().timestamp;
    std::size_t next_tick = 1;
    bool evaluate_current_point = true;

    auto consume_fill = [&](const CoofFillResult& fill,
                            BirthCursor birth_cursor,
                            bool filled_at_first_tick) {
        const uint64_t before = fill_events;
        cursor = fill.fill_price;
        cursor_is_bar_point = false;
        // Magnifier path: historical cascade permission is inert here (the cascade gate is
        // guarded by !bar_magnifier_enabled_), but keep provenance consistent —
        // a first-tick fill is the magnifier analogue of a bar-open recalc.
        fill_events += run_coof_recalc_chain(
            script_bar, cursor, cursor_is_bar_point, birth_cursor,
            evaluation_ordinal, fill.fill_events, max_fill_events, fill_events);
        evaluate_current_point = filled_at_first_tick
            && before == 0 && fill_events == 1;
    };

    while (fill_events < max_fill_events) {
        if (evaluate_current_point) {
            Bar point = coof_point_bar(script_bar, cursor);
            point.timestamp = cursor_ts;
            current_bar_ = point;
            CoofFillResult fill = process_next_pending_order(
                point, /*allow_market_orders=*/true,
                exit_closed_from_bar, exit_closed_from_incarnation,
                exit_closed_was_long);
            if (fill.filled) {
                consume_fill(
                    fill, BirthCursor::point(BirthCursorDomain::MagnifierTicks,
                        static_cast<int>(next_tick) - 1, static_cast<int>(ticks.size())),
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
            cursor_is_bar_point = true;
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
            consume_fill(
                fill, reached_target
                    ? BirthCursor::point(BirthCursorDomain::MagnifierTicks,
                        static_cast<int>(next_tick), static_cast<int>(ticks.size()))
                    : BirthCursor::segment(BirthCursorDomain::MagnifierTicks,
                        static_cast<int>(next_tick) - 1, static_cast<int>(ticks.size())),
                /*filled_at_first_tick=*/false);
            // The real lower-TF endpoint is already consumed. Do not replay
            // a market-enabled point at the same H/L/C tick; O remains the
            // sole intentional same-tick exception.
            if (reached_target) ++next_tick;
            continue;
        }

        cursor = target.price;
        cursor_is_bar_point = true;
        cursor_ts = target.timestamp;
        ++next_tick;
        evaluate_current_point = true;
    }

    cursor = ticks.back().price;
    cursor_is_bar_point = true;
    uint64_t direct = execute_coof_script_body(
        script_bar, cursor, cursor_is_bar_point,
        OrderBirth::chart_evaluation(bar_index_, script_bar.timestamp),
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
    settle_dormant_bracket_reissues(exit_legs::Domain::MagnifierCoof);
    if (trades_.size() != trades_before_mc) {
        refresh_frozen_default_sizing_after_margin_call();
    }
    const uint64_t margin_events = broker_fill_event_seq_ - fill_seq_before_mc;
    if (margin_events > 0 && fill_events < max_fill_events) {
        fill_events += run_coof_recalc_chain(
            script_bar, cursor, cursor_is_bar_point,
            BirthCursor::point(BirthCursorDomain::MagnifierTicks,
                               static_cast<int>(ticks.size()) - 1, static_cast<int>(ticks.size())),
            evaluation_ordinal, margin_events,
            max_fill_events, fill_events);
    }

    restore_coof_script_state();
    coof_scheduler_active_ = false;
    coof_fill_recalc_active_ = false;
    coof_recalc_at_bar_open_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_market_entry_recalc_incarnation_ = 0;
    coof_market_entry_recalc_fill_seq_ = 0;
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
// Public entry: clears last_error_/last_run_status_/abort_requested_ exactly
// once, then hands off to run_tf_impl, which owns the actual work and must
// not clear any of those itself (see run_tf_impl's doc comment in
// engine.hpp -- the SymInfo/overrides overload below calls run_tf_impl
// directly for the same reason).
void BacktestEngine::legacy_run_tf(const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) {
    last_error_.clear();
    last_run_status_ = 0;
    abort_requested_.store(false, std::memory_order_relaxed);
    try {
    validate_chart_bars(input_bars, n_input);
    run_tf_impl(input_bars, n_input, input_tf, script_tf, bar_magnifier,
                magnifier_samples, magnifier_dist);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}

void BacktestEngine::run_tf_impl(const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) {
    if (n_input > 0 && input_bars != nullptr) {
        last_bar_time_ = input_bars[n_input - 1].timestamp;
    } else {
        last_bar_time_ = 0;
    }
    // ABI v4 live-runtime surface (task 4): this is the TF-aware path's own
    // installation of the same file-scope PathOrderScope guard, so every
    // run's actual work (this function) installs and clears the override
    // exactly once, however it was reached (the thin TF-aware run()
    // wrapper, the syminfo/overrides overload, or stream_begin's warmup,
    // which all delegate here).
    PathOrderScope path_order_scope(path_order_mode_);
    try {
    if (!account_currency_fx_timestamps_.empty()
        && (calc_on_order_fills_ || bar_magnifier)) {
        throw std::runtime_error(
            "timestamped account-currency FX supports ordinary historical dispatch only; "
            "calc_on_order_fills and bar magnifier are unsupported");
    }
    // Auto-detect input_tf from bar timestamps if not provided
    std::string effective_input_tf = input_tf;
    if (effective_input_tf.empty() && n_input >= 2) {
        effective_input_tf = detect_timeframe(input_bars, n_input);
    }
    // script_tf defaults to input_tf if not provided (strategy runs on the data's timeframe)
    std::string effective_script_tf = script_tf.empty() ? effective_input_tf : script_tf;

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) {
        if (bar_magnifier) {
            throw std::runtime_error(
                "auxiliary request.security feed cannot share the bar-magnifier path");
        }
        if (effective_input_tf.empty() || effective_script_tf.empty()
            || effective_input_tf != effective_script_tf) {
            throw std::runtime_error(
                "auxiliary request.security feed requires native chart input_tf == script_tf");
        }
        security_input_tf_ = aux_security_input_tf_;
    } else {
        security_input_tf_ = effective_input_tf;
    }
#else
    security_input_tf_ = effective_input_tf;
#endif

    // Store parameters
    input_tf_ = effective_input_tf;
    script_tf_ = effective_script_tf;
    script_tf_seconds_ = tf_to_seconds(script_tf_);
    bar_magnifier_enabled_ = bar_magnifier;
    magnifier_samples_ = magnifier_samples;
    magnifier_dist_ = magnifier_dist;

    // Runtime diagnostics baseline
    diag_input_bars_processed_ = n_input;
    diag_script_bars_processed_ = 0;
    diag_magnifier_sub_bars_processed_ = 0;
    diag_magnifier_sample_ticks_processed_ = 0;

    reset_run_state();
    // Match the generated wrapper's original static/dynamic eligibility from
    // the caller's arguments, before auto-detection filled effective TFs.
    // A new stream always computes its warmup dynamically so later ticks do
    // not inherit a finite historical precalculation cache.
    prepare_script_run(input_bars, n_input,
        !stream_warmup_mode_ && !bar_magnifier
        && input_tf.empty() && script_tf.empty());
    configure_security_evaluators();

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
        script_tf_agg_ = TimeframeAggregator(effective_script_tf, effective_input_tf,
                                             syminfo_.timezone, syminfo_.session);
    } else {
        script_tf_agg_ = TimeframeAggregator();  // passthrough
    }

    int expected_script_bars =
        count_expected_script_bars(input_bars, n_input, needs_aggregation);
    last_bar_index_ = expected_script_bars - 1;
    // Live-runtime tail (spec §3.1): freeze last_bar_index_/last_bar_time_ at
    // the horizon bar. Must run AFTER the expected_script_bars assignment
    // above, which would otherwise clobber it. `input_bars` is the
    // script-bar array only when !needs_aggregation (input_tf ==
    // script_tf); under aggregation it is the finer *input* array, so
    // last_bar_time_ must fall back to the pre-fix first-bar extrapolation
    // instead of indexing input bars by a script-bar horizon (N1).
    apply_realtime_tail_horizon(input_bars, n_input,
                                 /*script_bar_geometry=*/!needs_aggregation);
    // reset_run_state() already ran above — reserve AFTER it so the capacity
    // hint isn't wiped (clear() retains capacity but order still matters for
    // any future reset that releases).
    equity_curve_.reserve((size_t)std::max(expected_script_bars, 0));

    validate_security_timeframes(security_input_tf_);

    // The run's first chart bar: the default range-start cut of every
    // coarser-than-chart / chart-timeframe request.security aggregation
    // (security_input_precedes_range_start). Cleared with the run so the
    // stream path and a later run start from their own first bar.
    security_first_chart_bar_ms_ = (n_input > 0) ? input_bars[0].timestamp : 0;

    init_security_eval_states_for_run(security_input_tf_);
    prepare_native_security_feeds(input_bars, n_input);
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) {
        prepare_aux_security_chart_ranges(input_bars, n_input,
                                          effective_script_tf);
    }
#endif
    // The historical lookahead projection is built from the chart bars on
    // both feed paths: a coarser-than-chart lookahead_on request leaks its
    // period's FINAL values from the period's first chart bar whether the
    // evaluator is fed the chart bars themselves or the auxiliary finer
    // slice (round 7, family I: hungpixi's "W" f_count on the BTC / XAUUSD
    // 1D lanes, which run split-feed for their "30" requests, read a
    // progressive partial week while TradingView reads the week's final).
    prepare_historical_security_lookahead_projections(
        input_bars, n_input, effective_input_tf);

    // The chart symbol's native daily partition (a "D" feed on an intraday
    // chart) keys the chart-level D consumers for exactly the bar loop:
    // time("D"), timeframe.change("1D"), ta.change(time("D")) and ta.vwap's
    // anchor read TradingView's trade-date daily bars, the request.security
    // evaluators their own partitions installed above. Empty -> nothing
    // installed, every rule nominal (prepare_chart_day_partition).
    prepare_chart_day_partition(input_bars, n_input);
    {
        NativeDayPartitionScope chart_day_partition(
            chart_day_partition_.empty() ? nullptr : &chart_day_partition_);
        if (!needs_aggregation && !bar_magnifier) {
            run_simple_bar_loop(input_bars, n_input);
        } else {
            run_aggregation_bar_loop(input_bars, n_input, bar_magnifier,
                                     expected_script_bars);
        }
    }
    // TradingView's range-end accounting: a position still open after the
    // last script bar is reported as a closed trade at that bar's close
    // (record_range_end_close_trades, engine_orders.cpp). Report-only: the
    // live position is untouched, and the stream warmup replay, whose bars
    // are not a range end, is skipped. Also skipped under the live-runtime
    // tail (spec §3.1): the last bar is still forming.
    if (!realtime_tail_) record_range_end_close_trades();
    clear_historical_security_lookahead_projections();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    clear_aux_security_chart_ranges();
#endif
    } catch (const AbortRequested&) {
        last_run_status_ = 1;
        clear_historical_security_lookahead_projections();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        clear_aux_security_chart_ranges();
#endif
    } catch (const std::exception& e) {
        clear_historical_security_lookahead_projections();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        clear_aux_security_chart_ranges();
#endif
        last_error_ = e.what();
    } catch (...) {
        clear_historical_security_lookahead_projections();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        clear_aux_security_chart_ranges();
#endif
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
    security_next_input_ms_ = 0;
    security_calling_close_ms_ = 0;
    for (auto& state : security_eval_states_) {
        state.feed_count = 0;
        state.eval_complete_count = 0;
        state.eval_partial_count = 0;
        state.current_bar = Bar{};
        state.current_sub_bar_count = 0;
        state.lower_tf_sub_bar_index = 0;
        state.lower_tf_input_buffer.clear();
        state.first_bucket_published = false;
        state.deferred_aux.clear();
        state.slice_open_label = 0;
        state.last_published_label = 0;
        state.historical_projections.clear();
        state.historical_projection_cursor = 0;
        state.historical_projection_dispatched = false;
        state.native_feed_index = -1;
        state.native_bars_by_label.clear();
        state.aggregator = TimeframeAggregator();
        if (state.lower_tf_emulation || state.lower_tf_use_input) {
            continue;
        }
        int req_ratio = tf_ratio(effective_input_tf, state.tf);
        if (req_ratio > 1) {
            state.aggregator = TimeframeAggregator(state.tf, effective_input_tf,
                syminfo_.timezone, syminfo_.session);
        } else if (req_ratio == -1) {
            state.aggregator = TimeframeAggregator(state.tf, effective_input_tf,
                syminfo_.timezone, syminfo_.session);
        }
        // The symbol kind decides whether a shortened session's last chart
        // bar completes a D/W/M bucket (exchange calendars) or the period
        // waits for its nominal close (OTC streams: OANDA cfd / forex).
        state.aggregator.set_early_close_completes(
            session_template_knows_early_close());
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

    historical_security_lookahead_projection_active_ = true;
    const int64_t input_ms = static_cast<int64_t>(input_seconds) * 1000;

    for (auto& state : security_eval_states_) {
        const int requested_seconds = tf_to_seconds(state.tf);
        // A calendar month has no fixed second count (tf_to_seconds -1) but
        // is always coarser than an intraday / daily script and buckets
        // through the calendar-aware tf_change below exactly as "W" and "D"
        // do. Without it a "M" lookahead_on site fell through to the
        // progressive partial peeks, whereas TradingView leaks the month's
        // FINAL values from its first chart bar (lab tv wm-m-f15-jul,
        // 2026-09-05: August's o/h/l/c from 08-01 09:30).
        const bool calendar_month = requested_seconds == -1
            && calendar_period_for(state.tf) == CalendarPeriod::MONTH;
        const bool eligible = !state.lower_tf_requested
            && !state.lower_tf_emulation
            && !state.lower_tf_use_input
            && state.lookahead_on
            && !state.gaps_on
            && !state.heikinashi
            && (calendar_month || requested_seconds > script_seconds);
        if (!eligible) {
            continue;
        }

        // Range-start warmup drops, PER EVALUATOR, every input bar whose HTF
        // bucket opened before the range start (feed_security_eval_state).
        // Build this evaluator's projection from that exact same retained
        // suffix and store child indexes relative to it: its feed cursor
        // likewise starts at zero on the first retained child because the
        // early-return path never increments it. The cut differs between
        // evaluators (a "W" series loses the whole straddling week, a "60"
        // series only the straddling hour), so it cannot be hoisted. This
        // composes the two independently opt-in historical semantics without
        // exposing a pre-range aggregate or shifting the first projected
        // bucket. An evaluator with no retained input gets no projection and
        // falls through to its (equally empty) progressive path.
        // The instant a chart child is fed to this evaluator: the child
        // itself on the single-feed path, its first auxiliary bar on the
        // split-feed path -- the range-start cut and the dispatch key must
        // see the same instant the feed will (an OANDA daily stamp sits at
        // the 17:00 ET break, its slice starts at the 18:00 session open).
        auto child_instant_ms = [&](int child) -> int64_t {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
            if (aux_security_feed_enabled()) {
                const std::size_t idx = static_cast<std::size_t>(child);
                if (idx < aux_security_chart_begin_.size()
                        && aux_security_chart_begin_[idx]
                               < aux_security_bars_.size()) {
                    return aux_security_bars_[aux_security_chart_begin_[idx]]
                        .timestamp;
                }
            }
#endif
            return input_bars[child].timestamp;
        };
        int projection_begin = 0;
        // The shared predicate also covers the single-feed OTC daily cut.
        // It is false for excluded evaluators, so no separate mode guard may
        // let the producer retain children the consumer will discard.
        while (projection_begin < n_input
                && security_input_precedes_range_start(
                       state, child_instant_ms(projection_begin))) {
            ++projection_begin;
        }
        if (projection_begin >= n_input) {
            continue;
        }
        const int projection_count = n_input - projection_begin;

        const int expected_children = std::max(
            1, (calendar_month ? 31 * 86400 : requested_seconds) / input_seconds);
        state.historical_projections.reserve(static_cast<std::size_t>(
            projection_count / expected_children + 1));
        state.historical_projection_cursor = 0;
        state.historical_projection_dispatched = false;

        auto crosses_requested_boundary = [&](int64_t from_ms,
                                               int64_t to_ms) {
            // tf_change treats epoch zero as an uninitialized sentinel. Keep
            // tests/synthetic feeds beginning at zero correct via the fixed-TF
            // bucket fallback; real feeds take the calendar-aware path.
            if (from_ms != 0 && to_ms != 0) {
                // A native feed's own period partition (TradingView's daily
                // stamps: a CME holiday session merged into the next trade
                // date's bar) bounds the projected buckets exactly as it
                // bounds the completion path, so the leaked bar is the
                // merged one from the holiday session's first chart bar.
                if (state.aggregator.has_native_periods()) {
                    return state.aggregator.period_changes(from_ms, to_ms);
                }
                return tf_change(from_ms, to_ms, state.tf,
                                 syminfo_.timezone, syminfo_.session);
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
                    aggregate, child_instant_ms(begin), is_complete});
        };

        int group_begin = projection_begin;
        Bar aggregate = input_bars[projection_begin];
        for (int i = projection_begin + 1; i < n_input; ++i) {
            if (crosses_requested_boundary(aggregate.timestamp,
                                           input_bars[i].timestamp)) {
                // A later bucket proves this group is historical/confirmed,
                // even when sparse input omitted its natural final child.
                publish_group(group_begin, aggregate, true);
                group_begin = i;
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
        state.historical_projection_dispatched = false;
    }
}


bool BacktestEngine::chart_bar_ismarket(int64_t bar_ms) const {
    return pineforge::pine_session_ismarket(syminfo_.session, syminfo_.timezone,
                                            bar_ms, script_tf_);
}

void BacktestEngine::set_session_bar_state(bool in_session,
                                           bool intraday_islastbar) {
    session_ismarket_ = in_session;
    if (tf_is_daily_or_higher(script_tf_)) {
        // A daily-or-higher chart bar covers its whole session day(s): it is
        // the session's first bar and its last bar at once (TradingView's
        // "first / last bar of the day's session", read on a bar that IS the
        // day), so both predicates hold on every bar of such a chart.
        session_isfirstbar_ = in_session;
        session_islastbar_ = in_session;
        return;
    }
    session_isfirstbar_ = in_session && !prev_in_session_;
    session_islastbar_ = intraday_islastbar;
}


// Bar pump for the no-aggregation, no-magnifier case: every input bar is a
// script bar, fed straight to on_bar with the standard
// pending-orders / per-trade-extremes / on_bar / equity-update sequence
// (with the process_orders_on_close TV variant when configured).
void BacktestEngine::run_simple_bar_loop(const Bar* input_bars, int n_input) {
    for (int i = 0; i < n_input; ++i) {
        check_abort();
        current_bar_ = input_bars[i];
        bar_index_ = i;
        is_tail_bar_ = (i == n_input - 1);
        is_first_tick_ = true;
        is_last_tick_ = true;
        barstate_islast_ = !stream_warmup_mode_ && !realtime_tail_ && (i == n_input - 1);
        diag_script_bars_processed_++;
        // Reset per-bar pending-close accumulator. Each on_bar call captures
        // fresh ``strategy.close*`` qty for the same-bar close-then-entry
        // source-order rule (see engine.hpp). Without the reset the
        // accumulator monotonically grows and starves every subsequent
        // priced-entry's tv_carry_qty (validation/52, 63, 72, 93, 95, 96
        // pre-fix: per-leg PnL drifts because the deferred-flip carry
        // chain is wiped after the first fire).
        pending_close_qty_in_bar_ = 0.0;

        // Feed security evaluators. On the split-feed path only the finer
        // auxiliary slice advances request.security; the native chart bar is
        // never passed to a security evaluator. The next input bar's
        // timestamp lets a calendar bucket complete on the period's actual
        // last chart bar (security_next_input_ms_).
        security_next_input_ms_ =
            (i + 1 < n_input) ? input_bars[i + 1].timestamp : 0;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        if (aux_security_feed_enabled()) {
            feed_aux_security_for_chart_bar(i);
        } else
#endif
        {
            for (auto& state : security_eval_states_) {
                feed_security_eval_state(state, input_bars[i]);
            }
        }

        // Update session predicates for session.ismarket / isfirstbar / islastbar.
        // Intraday islastbar: fire when this bar is in-session but the NEXT bar
        // won't be (lookahead: peek at the next bar's timestamp if available,
        // else fire on the last bar).
        {
            const bool in_session = chart_bar_ismarket(current_bar_.timestamp);
            bool next_in_session = false;
            if (in_session && i + 1 < n_input) {
                next_in_session = chart_bar_ismarket(input_bars[i + 1].timestamp);
            } else if (in_session && realtime_tail_ && script_tf_seconds_ > 0) {
                // Live tail: no i+1 exists; use the bucket calendar (the rule
                // engine_stream.cpp applies to a forming bar).
                next_in_session = chart_bar_ismarket(
                    current_bar_.timestamp
                    + static_cast<int64_t>(script_tf_seconds_) * 1000);
            } else if (in_session && realtime_tail_) {
                // Live tail with an unparseable/degenerate script_tf_seconds_
                // (no bucket width to advance by): a forming bar is never the
                // session's last bar, matching engine_stream.cpp's fallback
                // for the same degenerate case.
                next_in_session = true;
            }
            set_session_bar_state(in_session, in_session && !next_in_session);
        }

        dispatch_bar();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        // The rest of a first-bucket-latched evaluator's slice (its bars
        // after the first published bucket): TradingView's lookahead_on
        // read of a finer request is the calling bar's first intrabar, so
        // the body above read that, and the TA state now catches up on the
        // remaining sub-bars before the next chart bar's slice.
        if (aux_security_feed_enabled()) {
            feed_deferred_aux_security_for_chart_bar(i);
        }
#endif
        prev_in_session_ = session_ismarket_;
        update_equity_extremes();
        record_equity_point(current_bar_.timestamp);  // ts not mutated on this path
        if (broker_state_hash_recording_) broker_state_hashes_.push_back(broker_state_hash());
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
        check_abort();
        // The next input bar's timestamp for the security evaluators fed
        // below (directly, by run_magnified_bar's sub-bar walk, or by the
        // boundary re-feed): a calendar bucket completes on the period's
        // actual last chart bar (security_next_input_ms_).
        security_next_input_ms_ =
            (i + 1 < n_input) ? input_bars[i + 1].timestamp : 0;
        // Finer lookahead_on publication needs the chart aggregator's real
        // completion event. Eager completion feeds the current child normally;
        // boundary fallback replays the completed caller before the retained
        // next-caller child is evaluated.
        AggregatedBar ab = script_tf_agg_.feed(input_bars[i]);
        const bool completed_on_boundary = ab.is_complete
            && tf_change(ab.bar.timestamp, input_bars[i].timestamp, script_tf_,
                         syminfo_.timezone, syminfo_.session);
        if (!bar_magnifier) {
            for (auto& state : security_eval_states_) {
                if (completed_on_boundary
                    && state.publish_gate_tf_seconds > 0) {
                    publish_security_eval_state_at_calling_boundary(state);
                } else {
                    feed_security_eval_state(
                        state, input_bars[i], ab.is_complete);
                }
            }
        }

        if (bar_magnifier) {
            group_sub_bars.push_back(input_bars[i]);
        }

        if (ab.is_complete) {
            // Script-bar label for the equity curve: ab.bar.timestamp — the
            // aggregator's bucket label of the COMPLETED bucket (its grid /
            // session-day open, see TimeframeAggregator::bar_label_ms).
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
            // ABI v4 live-runtime surface (task 4): the bar magnifier's
            // run_magnified_bar never reaches dispatch_bar() (its own
            // top-of-function reset), so this emitted-script-bar boundary is
            // the per-bar reset site for it. Redundant-but-harmless on the
            // non-magnifier branch below, which also calls dispatch_bar().
            last_bar_dual_entry_decision_ = internal::DualEntryStopPathWinner::None;
            is_tail_bar_ = (i == n_input - 1);
            emitted_script_bars++;
            barstate_islast_ = !stream_warmup_mode_ && !realtime_tail_
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
                // Intraday islastbar is not deterministic here without lookahead.
                set_session_bar_state(
                    chart_bar_ismarket(group_sub_bars.front().timestamp),
                    /*intraday_islastbar=*/false);
                run_magnified_bar(
                    group_sub_bars, script_bar_ts, completed_on_boundary);
                prev_in_session_ = session_ismarket_;
                group_sub_bars.clear();
            } else {
                // No magnifier: use aggregated bar directly.
                //
                // ab.bar.timestamp is the bucket's LABEL — its open on the
                // symbol-clock grid (TimeframeAggregator::bar_label_ms), not
                // the first-present sub-bar's ts. When a feed gap eats the
                // bucket-opening sub-bar(s) — OANDA's 1m tape prints nothing
                // for the first minutes of every 17:00 ET forex session — the
                // first-present label drifted forward (17:04 where TV dates
                // the chart bar 17:00) and every trade booked on that bar
                // missed exact closed-trade identity by four minutes even
                // though price and PnL matched (finding 473). The label is
                // the session-anchored grid open, so US-equity 4h buckets
                // (09:30-anchored, not UTC-aligned) label correctly too.
                current_bar_ = ab.bar;
                // Update session predicates.
                {
                    const bool in_session = chart_bar_ismarket(current_bar_.timestamp);
                    set_session_bar_state(in_session, in_session && barstate_islast_);
                }
                dispatch_bar();
                prev_in_session_ = session_ismarket_;
            }
            update_equity_extremes();
            record_equity_point(script_bar_ts);
            if (broker_state_hash_recording_) broker_state_hashes_.push_back(broker_state_hash());
            prev_bar_timestamp_ = current_bar_.timestamp;
        }
        if (completed_on_boundary) {
            // The boundary-triggering input was retained by the chart
            // aggregator for the next caller. Feed it only after the completed
            // caller's chart body, and only to the finer lookahead_on states
            // that were replayed/deferred above. Other security states kept
            // their established feed order and cadence.
            for (auto& state : security_eval_states_) {
                if (state.publish_gate_tf_seconds > 0) {
                    feed_security_eval_state(
                        state, input_bars[i], /*calling_bar_complete=*/false);
                }
            }
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
void BacktestEngine::legacy_run_rich(const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          const std::unordered_map<std::string, std::string>& inputs,
                          const SymInfo& syminfo,
                          const StrategyOverrides* overrides,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) {
    last_error_.clear();
    last_run_status_ = 0;
    // Clears once, here, at the earliest point of this public entry --
    // before the syminfo/inputs/overrides setup below runs. Delegating to
    // run_tf_impl (not the public TF-aware run() overload, which would
    // clear a second time) means nothing after this line can wipe a
    // request_abort() that arrives from another thread during that setup:
    // the flag survives untouched until run_tf_impl's own check_abort()
    // calls consume it once the bar loop actually starts.
    abort_requested_.store(false, std::memory_order_relaxed);
    try {
    validate_chart_bars(input_bars, n_input);
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

    // Delegate to the TF-aware run's actual work directly (run_tf_impl, not
    // the public run() overload above) so the flag this overload just
    // cleared is not cleared a second time.
    run_tf_impl(input_bars, n_input, input_tf, script_tf, bar_magnifier, magnifier_samples, magnifier_dist);
    // Defensive: nothing in this overload's own body calls check_abort(), and
    // run_tf_impl above already converts AbortRequested to last_run_status_
    // == 1 internally, so this clause cannot fire today. Kept for symmetry
    // with the other two overloads and as a guard if that ever changes.
    } catch (const AbortRequested&) {
        last_run_status_ = 1;
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}

}  // namespace pineforge
