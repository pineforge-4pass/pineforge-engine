/*
 * engine_strategy_commands.cpp — the strategy.* command surface.
 *
 * Carved out of the original monolithic engine source during the v0.1
 * file-split (phase 6) — the BacktestEngine implementation now spans the
 * src/engine_*.cpp family. Each function is a
 * direct translation of one PineScript strategy.* call into a pending
 * order placement (and possibly an immediate fill when
 * `process_orders_on_close` or `immediately=true` is set).
 *
 *   strategy_entry       - place a pending entry order (market/limit/stop)
 *   strategy_close       - close a position (full or partial, by id or all)
 *   strategy_close_all   - thin wrapper over strategy_close("")
 *   strategy_exit        - place a take-profit / stop-loss / trail
 *   strategy_cancel      - drop one pending order by id
 *   strategy_cancel_all  - drop all pending orders
 *   strategy_order       - low-level RAW_ORDER placement
 *
 * Order matching itself lives in process_pending_orders
 * (engine_fills.cpp); the actual fill mechanics are in
 * execute_market_entry / execute_market_exit / execute_partial_exit
 * (engine_orders.cpp).
 */

#include "engine_internal.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pineforge {

using internal::kFullPercentEps;
using internal::kFullQtyEps;
using internal::kQtyEpsilon;

namespace {
// TradingView replays the strategy continuously from the FIRST OHLCV bar.
// The validator's ``trade_start_time`` gate intentionally suppresses
// strategy commands during the warmup span so TA / var accumulators
// converge without polluting comparison output. But the gate is set to
// ``TV first entry - input bar`` (see _trade_start_time_ms_from_tv in
// the validator) which, on a 15m strategy fed 1m OHLCV, lands one
// MINUTE before the first TV trade — far inside the script bar that
// CONTAINS the first TV trade, but BEFORE the script bar that
// PRECEDES it.
//
// A stop/limit placed on the immediately-preceding script bar (where
// TV's first trade originates) is therefore dropped under the strict
// gate, and the engine's first in-window trade fires several bars
// later from a different placement entirely. Validation/62-same-id-
// stop-cross-before-modify is the canonical victim: TV's 03-31 03:30
// long entry was longFirst's 03:15 stop firing — pre-fix the 03:15
// strategy.entry was gated, longModify at 03:30 armed a different
// stop that fired hours later, and the validator's price-fallback
// alignment then misaligned 64 in-window trades.
//
// Subtracting one script TF interval from the gate restores the
// previous script bar's strategy commands (just enough to let
// pre-window placements fire on the first in-window bar) without
// re-introducing the 411 extra pre-window trades that an
// unconditional bypass produces in continuously-firing strategies
// like basic/volty-expan. The buffer matches TV's chart-bar
// resolution rather than the validator-chosen input-bar resolution.
inline bool trading_is_active(int64_t current_ms, int64_t start_ms,
                              int script_tf_seconds) {
    if (start_ms == std::numeric_limits<int64_t>::min()) {
        return true;
    }
    int64_t buffer_ms = (script_tf_seconds > 0)
                           ? static_cast<int64_t>(script_tf_seconds) * 1000
                           : 0;
    return current_ms >= start_ms - buffer_ms;
}
}

void BacktestEngine::strategy_entry(const std::string& id, bool is_long,
                                     double limit_price, double stop_price, double qty,
                                     const std::string& comment,
                                     const std::string& oca_name, int oca_type,
                                     int qty_type) {
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, script_tf_seconds_)) return;

    // TradingView intraday-cap freeze gate (Pine docs:
    // ``strategy.risk.max_intraday_filled_orders``):
    //   "If the limit is reached during the day, the strategy is closed
    //    at the close of the next bar of the day, and all subsequent
    //    orders are blocked until the start of the next trading day."
    //
    // The fill-time gate in apply_filled_order_to_state already drops
    // FILLS during the latched window. But TV blocks ORDER PLACEMENT
    // too — a strategy.entry call inside a latched bar must not enter
    // the pending queue at all, otherwise the order survives until the
    // next chart-day rollover and fires a phantom entry on the first
    // new-day bar at a price TV never reports. Probe 97 trade #22
    // (UTC 04-07 00:00 long @ 1581.99) is the canonical victim — the
    // residual exit-price drift after the 97a/97b composition fixes
    // was driven by these phantom new-day entries (long-stop placed on
    // bar 04-06 23:45 with arm_long=true while the cap had already
    // latched on 04-06 07:00, then carrying to fire on the new chart-
    // day before the script's else-branch could cancel it).
    if (_intraday_cap_currently_latched()) return;

    // KI-65 MARKET/MARKET follow-up. Every explicit MARKET call first receives
    // the established OWN-qty admission below. Eligible own-admitted calls are
    // merely snapshotted here; the broker pair is finalized at the next fill
    // boundary, after the complete same-source-bar call set is known. Deferring
    // prevents an alternating three-call set from prematurely gross-rejecting
    // call 2 before call 3 exists.
    const bool explicit_fixed_market_call =
        std::isfinite(qty) && qty > kQtyEpsilon
        && std::isnan(limit_price) && std::isnan(stop_price)
        && oca_name.empty()
        && (qty_type < 0 || qty_type == static_cast<int>(QtyType::FIXED));
    const double paired_flat_market_own_qty = explicit_fixed_market_call
        ? std::abs(apply_qty_step(qty))
        : std::numeric_limits<double>::quiet_NaN();
    const bool paired_flat_market_candidate =
        explicit_fixed_market_call
        && paired_flat_market_own_qty > kQtyEpsilon
        && pending_flat_market_pair_scope_is_live()
        && position_side_ == PositionSide::FLAT;

    // TradingView broker rule: market-entry orders are admitted only
    // when ``qty * <signal-bar close> * margin_pct/100 <= equity``.
    // The check happens HERE (at signal time, with current_bar_.close
    // — the close of the bar where ``strategy.entry`` was just called),
    // NOT later in apply_market_order_fill where fill_price is the
    // NEXT bar's open. For ``qty = strategy.equity / close`` sizing
    // patterns (parity-anomalies/equity-mirror, community/IES) this
    // distinction is load-bearing: the close-vs-open slippage routinely
    // inflates overshoot from ~zero (at signal) to ~$20 (at fill), and
    // pre-fix engine rejected those at fill time while TV accepted
    // them at signal time — accumulating into community/IES's PnL
    // drift. Verified empirically by parity-probe-04..06 (all 57/57
    // matched) and the equity-mirror anomaly (full-equity sizing right at the
    // 1× boundary, where TV's behaviour is itself non-deterministic — see
    // corpus/validation/anomaly-equity-mirror-strategy-equity-01).
    // Only applied to MARKET entries (limit/stop entries have their
    // own price baked into the order itself).
    //
    // Scope: this signal-time gate covers EXPLICIT-qty market entries only
    // (the empirical base above — parity-probe-04..06 — is all explicit
    // ``qty = <expr>`` sizing, all with headroom at the boundary). DEFAULT-
    // sized (qty=na) market entries never reach it (qty is NaN here); their
    // quantity is frozen at this bar's close further below (see
    // frozen_default_market_qty), AFTER this gate, precisely so the freeze
    // cannot accidentally activate a gate whose equity basis (realized-only
    // current_equity()) was never validated for default sizing. Those frozen
    // default-sized entries instead get their own, much narrower fill-time
    // re-check: the percent==100 zero-commission true-flat above-lot gap-reject
    // in apply_filled_order_to_state (design-cntvxiao-gap-reject). It does NOT
    // contradict the explicit-qty "signal-time only" rule above — different
    // sizing regime, different TV ground truth.
    if (!std::isnan(qty) && std::isnan(limit_price) && std::isnan(stop_price)) {
        double margin_pct = is_long ? margin_long_ : margin_short_;
        if (margin_pct > 0.0 && !std::isnan(current_bar_.close)) {
            // Position value in account currency includes the futures
            // point-value multiplier (1.0 for crypto/equity — unchanged).
            // The notional is in the symbol's quote currency; convert it to the
            // account currency (FX 1.0 unless the script declared a differing
            // currency=) so it is comparable to equity, which is denominated in
            // the account currency. Default 1.0 leaves the corpus untouched.
            double required_margin = std::abs(qty) * current_bar_.close
                                     * syminfo_.pointvalue
                                     * account_currency_fx_
                                     * (margin_pct / 100.0);
            double available_equity = current_equity();
            double epsilon = std::max(1e-9, std::abs(available_equity) * 1e-12);
            if (required_margin > available_equity + epsilon) {
                return;
            }
        }
    }
    int64_t preserved_seq = 0;
    for (const auto& o : pending_orders_) {
        if (o.id == id) {
            preserved_seq = o.created_seq;
            break;
        }
    }

    // Remove existing pending order with same id
    for (const PendingOrder& pending : pending_orders_) {
        if (pending.id == id) {
            const bool entry_like =
                pending.type == OrderType::MARKET
                || pending.type == OrderType::ENTRY
                || pending.type == OrderType::RAW_ORDER;
            // A candidate call that replaces any resting entry-like order is
            // not an exact two-call source-bar set, even when the replaced
            // order came from an earlier bar. Taint this call's bar as well as
            // preserving the old-bar tombstone below.
            if (paired_flat_market_candidate && entry_like) {
                pending_flat_market_pair_disqualified_bars_.insert(bar_index_);
            }
            if (entry_like
                && pending.created_position_side == PositionSide::FLAT) {
                pending_flat_market_pair_disqualified_bars_.insert(
                    pending.created_bar);
            }
            invalidate_pending_flat_market_pair(pending.created_seq);
        }
    }
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) { return o.id == id; }),
        pending_orders_.end());

    // On the ordinary non-POOC path, TradingView rejects a same-direction
    // priced strategy.entry call when the live position is already at the
    // pyramiding cap. This is a placement-time admission rule, not merely a
    // fill-time check: a rejected stop/limit must not survive a later reversal
    // and fire against the new opposite position. Same-id replacement happens
    // first, so an over-cap reissue also removes the older pending order without
    // admitting the replacement. Market entries keep their fill-time role-
    // change semantics, and POOC entry+close co-queues remain governed by the
    // same-close-pass rules. Ground truth:
    // order-entry-overcap-priced-admission-01 phases A/B.
    bool over_pyramiding_cap =
        position_side_ != PositionSide::FLAT
        && position_side_ == (is_long ? PositionSide::LONG : PositionSide::SHORT)
        && position_entry_count_ >= pyramiding_;
    bool is_priced_entry = !std::isnan(limit_price) || !std::isnan(stop_price);
    if (is_priced_entry && !process_orders_on_close_ && over_pyramiding_cap) return;

    PendingOrder order;
    order.id = id;
    order.from_entry = "";
    order.is_long = is_long;
    order.trail_points = std::numeric_limits<double>::quiet_NaN();
    order.trail_price = std::numeric_limits<double>::quiet_NaN();
    order.trail_offset = std::numeric_limits<double>::quiet_NaN();
    order.qty = qty;
    order.qty_type = qty_type;
    order.qty_percent = 100.0;
    order.oca_name = oca_name;
    order.oca_type = oca_type;
    order.created_bar = bar_index_;
    order.created_seq = preserved_seq > 0 ? preserved_seq : next_order_seq_++;
    order.incarnation = next_order_incarnation_++;
    order.created_during_coof_recalc = coof_fill_recalc_active_;
    order.coof_born_at_close_recalc =
        coof_fill_recalc_active_ && coof_cursor_is_bar_close_;
    // KI-67: a fill recalc that was NOT triggered at the bar-open tick is a
    // mid-bar recalc; orders it places are cascade orders (eligible only at the
    // remaining extreme waypoints of the historical 4-tick path).
    order.coof_born_mid_bar =
        coof_fill_recalc_active_ && !coof_recalc_at_bar_open_;
    order.created_position_side = position_side_;
    order.created_position_cycle_seq = position_cycle_seq_;
    order.created_after_position_close_in_bar =
        pending_close_qty_in_bar_ > kQtyEpsilon;
    // Market orders and the POOC priced path retain the placement snapshot for
    // the downstream full-close compaction and role-change rules. Ordinary
    // non-POOC priced orders that are over cap returned above.
    order.over_pyramiding_cap_at_placement = over_pyramiding_cap;
    // TradingView empirical rule (probe 52 trade 113): the deferred-flip
    // carry is the position size at THIS placement, not the original.
    // ``strategy.entry`` with the same id replaces the pending order
    // entirely on each call — including a fresh capture of position_qty_.
    // If the LE/SE was placed during a non-zero position and that
    // position closes before the LE/SE fires, subsequent re-placements
    // (now from flat) capture carry=0 and the order fires fresh on
    // qty=1 — exactly TV's "chain reset" behaviour at 04-26 16:30 UTC.
    //
    // Probe 93 cycle B refinement: a ``strategy.close`` call earlier in
    // the SAME on_bar must be subtracted off because TV evaluates calls
    // in source order. ``pending_close_qty_in_bar_`` accumulates qty of
    // strategy.close* calls during the current on_bar and resets at the
    // top of each bar. When close is called BEFORE entry, the entry
    // captures the post-close position size; when entry is called BEFORE
    // close, ``pending_close_qty_in_bar_`` is still 0 and the carry
    // equals the open position.
    //
    // Side-gate: ``position_qty_`` is undefined whenever ``position_side_``
    // is FLAT — the engine's default ``position_qty_ = 1.0`` would leak
    // into ``tv_carry_qty`` for the FIRST priced ``strategy.entry`` call
    // of any session that has never opened a position before, fabricating
    // a phantom carry. Probe 62 manifests this: the longModify stop
    // placed at 03:30 (after the warmup gate skipped longFirst at 03:15)
    // captured carry=1 from the default qty, then fired later with
    // qty=2 instead of 1, breaking parity from trade #1 onward. Reading
    // the position size through the canonical ``signed_position_size``
    // path returns 0 when FLAT regardless of the underlying default.
    double live_pos_qty = (position_side_ == PositionSide::FLAT)
                              ? 0.0
                              : position_qty_;
    double effective_pos = std::max(0.0, live_pos_qty - pending_close_qty_in_bar_);
    order.tv_carry_qty = effective_pos;
    order.comment = comment;

    bool has_limit = !std::isnan(limit_price);
    bool has_stop = !std::isnan(stop_price);

    if (!has_limit && !has_stop) {
        order.type = OrderType::MARKET;
        order.limit_price = std::numeric_limits<double>::quiet_NaN();
        order.stop_price = std::numeric_limits<double>::quiet_NaN();
        if (paired_flat_market_candidate) {
            order.paired_flat_market_candidate = true;
            order.paired_flat_market_own_qty = paired_flat_market_own_qty;
            order.paired_flat_market_signal_close = current_bar_.close;
            order.paired_flat_market_signal_equity = current_equity();
            order.paired_flat_market_signal_margin_pct =
                is_long ? margin_long_ : margin_short_;
            order.paired_flat_market_signal_pointvalue = syminfo_.pointvalue;
            order.paired_flat_market_signal_fx = account_currency_fx_;
        }
        // TradingView freezes DEFAULT (qty=na) percent_of_equity / cash
        // market-order sizing at THIS (signal) bar's close — see
        // frozen_default_market_qty (engine.hpp) for the rule and the
        // empirical basis. current_bar_.close is close(S) right here, so
        // placement is the one point where the frozen computation is
        // naturally correct (no double count, no fill-bar look-ahead).
        // FIXED default sizing needs no freeze: its fill-time value is
        // identical. The frozen quantity goes in frozen_default_qty, NOT in
        // order.qty — order.qty must stay NaN so every isnan(order.qty)-keyed
        // "was this default-sized?" branch (OCA cancel, reversal binding,
        // OCA fully-filled, partial-exit classification) keeps its meaning.
        if (std::isnan(qty)
            && (default_qty_type_ == QtyType::PERCENT_OF_EQUITY
                || default_qty_type_ == QtyType::CASH)
            && !std::isnan(current_bar_.close)) {
            order.frozen_default_qty = frozen_default_market_qty(/*is_buy=*/is_long);
            // KI-54: persist the sizing basis for the fill-time TV margin
            // admission re-check (see PendingOrder::sizing_equity in
            // engine.hpp and the gate in apply_filled_order_to_state).
            order.sizing_price = frozen_sizing_price(/*is_buy=*/is_long);
            order.sizing_equity =
                current_equity() + open_profit(current_bar_.close);
            order.sizing_mark = current_bar_.close;
            // Direction-neutral: two fill-time consumers read this flag.
            //   1. KI-61 long entry-bar affordability trim
            //      (engine_fills.cpp): re-checks order.is_long and margin_long
            //      via long_full_margin_after_fill, so its long-only exemption
            //      remains invariant.
            //   2. gap-reject (design-cntvxiao-gap-reject, engine_fills.cpp):
            //      direction-symmetric — drops a true-flat all-in zero-comm
            //      entry whose gapped fill notional exceeds equity by >1 lot,
            //      on EITHER side.
            // The margin term is the direction-appropriate one so a short at
            // margin_short==100 qualifies exactly as a long at margin_long==100.
            const double affordability_margin =
                is_long ? margin_long_ : margin_short_;
            order.opening_affordability_exemption_candidate =
                order.created_position_side == PositionSide::FLAT
                && !order.created_after_position_close_in_bar
                && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
                && std::abs(default_qty_value_ - 100.0) < 1e-12
                && std::isfinite(affordability_margin)
                && std::abs(affordability_margin / 100.0 - 1.0) < 1e-12
                && std::isfinite(order.frozen_default_qty)
                && std::isfinite(order.sizing_equity)
                && std::isfinite(order.sizing_price)
                && std::isfinite(order.sizing_mark);
        }
        // design-explicit-qty-fill-admission: capture the fill-time admission
        // snapshot for an EXPLICIT-qty (the caller passed a finite qty) MARKET
        // entry created TRUE-FLAT. The signal-time gate above (~:139-158)
        // already rejected orders whose notional at THIS bar's close overshoots
        // equity; this snapshot lets the fill-time re-check drop the entry when
        // the next-bar fill gaps adversely enough that |qty|*slipped_fill
        // exceeds the placement equity — TV's pinned behavior for the all-in
        // floor idiom (probe-68 / mdfe3757). Disjoint from the frozen default-
        // sizing snapshot above (that path requires isnan(qty)); priced
        // (limit/stop) entries never reach here (else-branch) and RAW
        // strategy.order builds its order elsewhere, so neither carries the
        // candidate flag. Commission is EXCLUDED from the fill predicate.
        if (!std::isnan(qty) && !std::isnan(current_bar_.close)) {
            const double explicit_margin = is_long ? margin_long_ : margin_short_;
            // Equity basis matches the frozen path (KI-54): realized equity plus
            // open profit marked at the signal close. == current_equity() when
            // flat, which every candidate is (created_position_side == FLAT).
            const double placement_equity =
                current_equity() + open_profit(current_bar_.close);
            const double slipped_signal_close =
                frozen_sizing_price(/*is_buy=*/is_long);
            order.explicit_flat_admission_candidate =
                order.created_position_side == PositionSide::FLAT
                && !order.created_after_position_close_in_bar
                && std::isfinite(explicit_margin) && explicit_margin > 0.0
                && std::isfinite(placement_equity)
                && std::isfinite(slipped_signal_close);
            if (order.explicit_flat_admission_candidate) {
                order.explicit_placement_equity = placement_equity;
                order.explicit_slipped_signal_close = slipped_signal_close;
            }
        }
    } else {
        order.type = OrderType::ENTRY;
        order.limit_price = limit_price;
        order.stop_price = stop_price;
        // KI-65 placement-time pending-market awareness (probe pf-probe-ki65-
        // dual-entry-precedence): a priced entry placed from FLAT that will
        // reverse a position an EARLIER same-on_bar OPPOSITE-direction MARKET
        // entry opens must FULLY reverse (open its own leg), not collapse to
        // close-only-flat. TV runs no arbitration on same-bar opposite entries —
        // both execute; the second call sells own + the pending opposite MARKET
        // qty. A pending STOP contributes 0 (require type==MARKET), and a
        // placement-rejected market never reaches pending_orders_ (the signal-
        // time margin gate above `return`s before push_back), so it too
        // contributes 0. created_seq scopes it to a market placed BEFORE this
        // entry (the E1-first, E2-second probe framing). Deferred-flip carry is
        // excluded (created_position_side != FLAT).
        if (order.created_position_side == PositionSide::FLAT) {
            for (const auto& sib : pending_orders_) {
                if (sib.type == OrderType::MARKET
                    && sib.is_long != is_long
                    && sib.created_bar == order.created_bar
                    && sib.created_seq < order.created_seq) {
                    order.reverses_same_bar_market_from_flat = true;
                    break;
                }
            }
        }
    }

    pending_orders_.push_back(std::move(order));
    invalidate_unsafe_pooc_global_full_exit_dynamic_qty();
}

void BacktestEngine::strategy_close(const std::string& id, const std::string& comment,
                                    double qty, double qty_percent, bool immediately) {
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, script_tf_seconds_)) return;
    if (position_side_ == PositionSide::FLAT) {
        return;
    }

    // TradingView one-close-fill-per-bar rule: under process_orders_on_close,
    // default-FIFO ``strategy.close(id)`` calls (no explicit qty/qty_percent,
    // FIFO close-entries rule) issued on the SAME bar collapse into a single
    // surviving market close order that fills at the bar close. Route those
    // calls through the same-bar batch; the fill executes at the end-of-bar
    // order-processing point (dispatch_bar step 4 / magnifier last tick) via
    // flush_same_bar_close(). Everything else (ANY rule, explicit qty,
    // close_all, immediately=true, non-POC deferred closes) keeps the
    // existing paths.
    const bool pooc_can_fill_at_this_cursor =
        process_orders_on_close_
        && (!coof_scheduler_active_ || coof_cursor_is_bar_close_)
        // A fill recalculation at C occurs after that broker point has been
        // consumed. Only an explicit immediately=true close may execute at
        // the current cursor; ordinary POOC closes are materialized as
        // pending instructions and expire if no ordinary pass reissues them.
        && !(coof_scheduler_active_ && coof_fill_recalc_active_);
    if (pooc_can_fill_at_this_cursor && !immediately
        && !close_entries_rule_any_ && !id.empty()
        && std::isnan(qty) && std::isnan(qty_percent)) {
        enqueue_same_bar_close(id, comment);
        return;
    }

    double matching_qty = 0.0;
    double qty_to_close = 0.0;
    bool all_entries_match = false;
    if (!compute_close_target_qty(id, qty, qty_percent,
                                  matching_qty, qty_to_close, all_entries_match)) {
        return;
    }

    const double eps = kQtyEpsilon;
    bool closes_full_position = false;
    if (id.empty()) {
        closes_full_position = qty_to_close >= position_qty_ - eps;
    } else if (close_entries_rule_any_) {
        closes_full_position = all_entries_match && qty_to_close >= position_qty_ - eps;
    } else {
        closes_full_position = qty_to_close >= position_qty_ - eps;
    }

    // Track this close's qty for the same-bar source-order carry rule.
    // A subsequent ``strategy.entry`` on the same on_bar will see the
    // post-close position size when capturing its tv_carry_qty.
    pending_close_qty_in_bar_ += qty_to_close;
    bool closes_fifo_qty = !close_entries_rule_any_ && !closes_full_position;
    bool closes_any_qty = close_entries_rule_any_ && !closes_full_position;

    if (closes_full_position) {
        bool closing_long = (position_side_ == PositionSide::LONG);
        cancel_orders_for_full_close(id, closing_long);
    }

    if ((pooc_can_fill_at_this_cursor || immediately)
        && !(coof_scheduler_active_
             && coof_direct_fill_events_remaining_ == 0)) {
        // KI-64: for an ORDINARY POOC close (not immediately=true, which is
        // defined to reflect its fill at once) freeze the script-visible
        // position BEFORE execute_immediate_close mutates it, so a later
        // strategy.position_size gate in THIS bar still sees the pre-close
        // position. Broker/order side effects below are unchanged.
        if (process_orders_on_close_ && !immediately) {
            freeze_script_position_view();
        }
        const bool ordinary_pooc_close_all =
            pooc_can_fill_at_this_cursor && !immediately && id.empty()
            && !coof_scheduler_active_;
        execute_immediate_close(id, comment, qty_to_close, matching_qty,
                                closes_full_position, closes_fifo_qty, closes_any_qty,
                                ordinary_pooc_close_all);
        return;
    }

    // design-declined-reversal-close-leg: compute_close_target_qty's default-
    // FIFO branch (below condition) debited id_unclosed_qty_[id] by
    // qty_to_close. Record it on the deferred close so a later reversal-decline
    // suppression can re-credit exactly that amount — UNLESS the POOC recalc
    // block below re-credits it immediately (guard against a double-credit).
    const bool default_fifo_close = !close_entries_rule_any_ && !id.empty()
        && std::isnan(qty) && std::isnan(qty_percent);
    const bool immediate_ledger_recredit = coof_scheduler_active_
        && coof_fill_recalc_active_ && coof_cursor_is_bar_close_
        && process_orders_on_close_ && default_fifo_close;
    const double consumed_ledger_qty =
        (default_fifo_close && !immediate_ledger_recredit)
            ? qty_to_close : std::numeric_limits<double>::quiet_NaN();
    const uint64_t deferred_close_incarnation = queue_deferred_close_order(
        id, comment, qty_to_close, matching_qty,
        closes_full_position, closes_any_qty,
        consumed_ledger_qty);

    // TradingView keeps one narrowly identifiable prior-bar broker order
    // across an ordinary deferred close_all: a pure STOP strategy.entry that
    // reuses the id of a physically-live lot on the held side and was within
    // the pyramiding cap at placement. Snapshot physical pyramid_entries_
    // here, before the close later drains them; id_unclosed_qty_ is purposely
    // not used because default-FIFO close(id) can make that logical ledger
    // disagree with the actually-live lot roster. Pair the snapshot with the
    // newly queued close_all's fresh incarnation: created_seq intentionally
    // survives same-id replacement for ordering, so it is not an identity.
    // Sharing the call bar alone is also insufficient when an earlier RAW or
    // ANY close(id) flattens first. No later full-close call globally clears
    // this stamp: a coexisting earlier close_all still owns it, while a
    // cancelled/replaced close_all can never be impersonated because its
    // incarnation is never reused.
    if (closes_full_position && id.empty() && !process_orders_on_close_) {
        const bool closing_long = position_side_ == PositionSide::LONG;
        for (PendingOrder& pending : pending_orders_) {
            const bool pure_stop_entry =
                pending.type == OrderType::ENTRY
                && std::isfinite(pending.stop_price)
                && std::isnan(pending.limit_price)
                && std::isnan(pending.trail_points)
                && std::isnan(pending.trail_price)
                && std::isnan(pending.trail_offset)
                && !pending.stop_limit_activated;
            if (!pure_stop_entry
                || pending.created_bar >= bar_index_
                || pending.is_long != closing_long
                || pending.created_position_side != position_side_
                || pending.over_pyramiding_cap_at_placement) {
                continue;
            }
            const bool has_physically_live_same_id_lot =
                std::any_of(
                    pyramid_entries_.begin(), pyramid_entries_.end(),
                    [&](const PyramidEntry& lot) {
                        return lot.entry_id == pending.id
                            && lot.qty > kQtyEpsilon;
                    });
            if (has_physically_live_same_id_lot) {
                pending.same_id_stop_deferred_close_all_bar = bar_index_;
                pending.same_id_stop_deferred_close_all_incarnation =
                    deferred_close_incarnation;
            }
        }
    }
    // A default-FIFO close consumes id_unclosed_qty_ while resolving its
    // target above. When the command was born after an already-consumed POOC
    // close, its market order expires without a broker tick; keep the logical
    // entry ledger available so a later ordinary-close execution can reissue
    // and actually fill the close (Delta's next-bar lifecycle).
    if (immediate_ledger_recredit) {
        id_unclosed_qty_[id] += qty_to_close;
    }
}

void BacktestEngine::strategy_close_all() {
    strategy_close("");
}

// Qty the pending same-bar close batch will fill at flush time (0 when no
// batch is pending). Lets same-bar order sizing (strategy.exit) see the
// post-close position before the flush actually executes.
double BacktestEngine::pending_same_bar_close_target() const {
    if (!sb_close_active_) return 0.0;
    auto it = id_unclosed_qty_.find(sb_close_id_);
    double unclosed = (it != id_unclosed_qty_.end()) ? it->second : 0.0;
    double avail = std::max(0.0, position_qty_ - close_reserved_other_qty(sb_close_id_));
    return std::min(unclosed, avail);
}

// Sum of same-bar-close reservations held by OTHER ids. A surviving
// multi-close fill leaves its unconsumed ledger amount reserved against
// the position; other ids' close targets are capped by what remains.
double BacktestEngine::close_reserved_other_qty(const std::string& id) const {
    double sum = 0.0;
    for (const auto& kv : close_reserved_qty_) {
        if (kv.first != id) sum += kv.second;
    }
    return sum;
}

// Admit a default-FIFO strategy.close(id) call into the same-bar batch
// (TV one-fill-per-bar rule — see the close_reserved_qty_ field block in
// engine.hpp). Order cancels tied to a full-position close keep their
// CALL-time timing (identical to the previous immediate path); only the
// FILL is deferred to flush_same_bar_close().
void BacktestEngine::enqueue_same_bar_close(const std::string& id,
                                            const std::string& comment) {
    const double eps = kQtyEpsilon;
    auto it = id_unclosed_qty_.find(id);
    double unclosed = (it != id_unclosed_qty_.end()) ? it->second : 0.0;
    double avail = std::max(0.0, position_qty_ - close_reserved_other_qty(id));
    double target = std::min(unclosed, avail);
    if (target <= eps) {
        return;  // zero-target call: no-op, cannot become the survivor
    }

    // Same-bar source-order carry (see strategy_entry's tv_carry_qty): a
    // subsequent strategy.entry on this on_bar sees the post-close size.
    pending_close_qty_in_bar_ += target;

    bool closes_full_position = target >= position_qty_ - eps;
    if (closes_full_position) {
        bool closing_long = (position_side_ == PositionSide::LONG);
        cancel_orders_for_full_close(id, closing_long);
        purge_exit_orders();
    }

    if (!sb_close_active_ || sb_close_bar_ != bar_index_) {
        sb_close_active_ = true;
        sb_close_bar_ = bar_index_;
        sb_close_calls_ = 1;
        sb_close_first_id_ = id;
        sb_close_id_ = id;
        sb_close_comment_ = comment;
        return;
    }
    if (id == sb_close_id_) {
        // Re-issued close for the id already pending: replaces the same
        // order in place (comment refresh; not a new call).
        sb_close_comment_ = comment;
        return;
    }
    sb_close_calls_ += 1;
    if (sb_close_calls_ == 2) {
        // The FIRST replaced call's ledger is consumed silently: no fill,
        // no trade rows, reservation released. Intermediate replaced
        // calls (3rd+) keep their ledgers intact.
        id_unclosed_qty_.erase(sb_close_first_id_);
        close_reserved_qty_.erase(sb_close_first_id_);
    }
    sb_close_id_ = id;
    sb_close_comment_ = comment;
}

// Execute the surviving same-bar close at the bar's close price. Called
// from the end-of-bar order-processing points (dispatch_bar step 4 and
// the magnifier's last tick), i.e. the same bar and price the immediate
// path used, after the strategy's on_bar has fully run.
void BacktestEngine::flush_same_bar_close() {
    // KI-64: on_bar has returned — release any POOC script-visible position
    // freeze so the flush below, step-4 order processing, and post-run reads
    // all observe the real (post-close) position. Runs on every POOC bar
    // (flush is the first call after each POOC on_bar), before the early
    // return so a bar with an in-line close but no enqueued survivor still
    // clears. Broker reads here use position_side_/position_qty_ directly.
    clear_script_position_view();
    if (!sb_close_active_) return;
    const std::string id = sb_close_id_;
    const std::string comment = sb_close_comment_;
    const bool sole_call = (sb_close_calls_ == 1);
    sb_close_active_ = false;
    sb_close_bar_ = -1;
    sb_close_calls_ = 0;
    sb_close_first_id_.clear();
    sb_close_id_.clear();
    sb_close_comment_.clear();
    if (position_side_ == PositionSide::FLAT) return;

    const double eps = kQtyEpsilon;
    auto it = id_unclosed_qty_.find(id);
    double unclosed = (it != id_unclosed_qty_.end()) ? it->second : 0.0;
    double avail = std::max(0.0, position_qty_ - close_reserved_other_qty(id));
    double target = std::min(unclosed, avail);
    if (target <= eps) return;

    bool closes_full_position = target >= position_qty_ - eps;
    if (coof_scheduler_active_
        && ((coof_fill_recalc_active_ && coof_cursor_is_bar_close_)
            || !coof_cursor_is_bar_close_
            || coof_direct_fill_events_remaining_ == 0)) {
        // The script execution still occurs after the last allowed fill, but
        // its close cannot manufacture an extra historical broker event.
        // Materialize the command so same-bar broker/order side effects remain
        // explicit; as a post-C POOC market instruction it expires unless a
        // later ordinary-close execution reissues it.
        queue_deferred_close_order(
            id, comment, target, target, closes_full_position,
            /*closes_any_qty=*/false);
        return;
    }

    if (sole_call) {
        // Single close call this bar: classic semantics — consume the
        // ledger and release any prior reservation for this id.
        it->second -= target;
        if (it->second <= eps) {
            id_unclosed_qty_.erase(it);
        }
        close_reserved_qty_.erase(id);
    }

    size_t trades_before = trades_.size();
    PositionSide side_before = position_side_;
    double qty_before = position_qty_;
    const double broker_price =
        coof_scheduler_active_ && std::isfinite(coof_cursor_price_)
            ? coof_cursor_price_ : current_bar_.close;
    if (closes_full_position) {
        const bool closed_long = (position_side_ == PositionSide::LONG);
        // Exit-order cancel/purge already ran at CALL time in
        // enqueue_same_bar_close — orders armed after the close call
        // must survive exactly as they did under the immediate path.
        execute_market_exit(broker_price);
        if (position_side_ == PositionSide::FLAT) {
            cancel_same_bar_market_reentries_after_full_close(
                closed_long, /*preserve_undercap_entries=*/false);
        }
    } else {
        execute_partial_exit_qty(broker_price, target);
        if (position_side_ == PositionSide::FLAT) {
            // Retain from_entry brackets whose parent entry is still a
            // pending order (it fills right after this flush).
            purge_exit_orders(/*retain_for_pending_entries=*/true);
        }
    }
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = comment;
        trades_[ti].exit_id = "__close__" + id;
    }
    if (position_side_ != side_before
        || std::abs(position_qty_ - qty_before) > eps
        || trades_.size() != trades_before) {
        ++broker_fill_event_seq_;
        if (coof_scheduler_active_ && coof_direct_fill_events_remaining_ > 0) {
            --coof_direct_fill_events_remaining_;
        }
    }
    if (!sole_call && position_side_ != PositionSide::FLAT) {
        // Surviving multi-call close: ledger NOT consumed (TV leaves the
        // id closable again later); the filled amount stays reserved
        // against the position for other ids' close targets.
        close_reserved_qty_[id] = target;
    }
}

void BacktestEngine::strategy_exit(const std::string& id, const std::string& from_entry,
                                    double limit_price, double stop_price,
                                    double trail_points, double trail_offset,
                                    double trail_price, double qty_percent,
                                    const std::string& comment,
                                    double qty, const std::string& oca_name,
                                    double profit_ticks, double loss_ticks) {
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, script_tf_seconds_)) return;
    const bool has_actionable_exit = !std::isnan(limit_price)
        || !std::isnan(stop_price)
        || !std::isnan(profit_ticks)
        || !std::isnan(loss_ticks)
        || !std::isnan(trail_points)
        || !std::isnan(trail_price);
    if (!has_actionable_exit) {
        // TV probe N0/NR: an all-actionable-NaN strategy.exit is inert, not a
        // market close. A same-id call still cancels its prior bracket, so the
        // return follows matching-EXIT removal but precedes all sizing and
        // reservation work. trail_offset alone is intentionally insufficient.
        int64_t discarded_seq = 0;
        double discarded_reserved_qty = std::numeric_limits<double>::quiet_NaN();
        clear_existing_exit_order(id, from_entry, /*has_trail_request=*/false,
                                  discarded_seq, discarded_reserved_qty);
        return;
    }
    bool has_explicit_qty = !std::isnan(qty);
    double qp = std::isnan(qty_percent) ? 100.0 : std::clamp(qty_percent, 0.0, 100.0);
    // A default-FIFO strategy.close batched earlier on this SAME bar has not
    // filled yet (it fills at the end-of-bar flush) but its qty is already
    // committed. An exit armed after that close call must size against the
    // post-close position — exactly what it saw when the immediate path
    // executed the close mid-bar. Without this, a close + reversal-entry +
    // exit sequence freezes the OLD side's size into the bracket's reserved
    // qty (wayward-bison: the Long SL stop filled only the stale short-sized
    // 3.9629 of an 8.0672 long, fragmenting one TV exit into two rows).
    double sb_pending_close = (sb_close_active_ && sb_close_bar_ == bar_index_)
                                  ? pending_same_bar_close_target()
                                  : 0.0;
    double live_pos_qty = (position_side_ == PositionSide::FLAT)
                              ? 0.0
                              : std::max(0.0, position_qty_ - sb_pending_close);
    bool effectively_flat = live_pos_qty <= kQtyEpsilon;
    // If an explicit qty is given, derive an effective qp from the current
    // position size so downstream FIFO accounting (compute_exit_reserved_qty,
    // already-reserved tally, etc.) sees a consistent fraction. The order
    // itself stores the absolute qty so the per-fill execution path
    // honours the literal request.
    if (has_explicit_qty && live_pos_qty > kQtyEpsilon) {
        double clamped_qty = std::min(qty, live_pos_qty);
        qp = (clamped_qty / live_pos_qty) * 100.0;
    }
    bool is_partial = qp < 100.0 - kFullPercentEps;
    bool has_trail_request = !std::isnan(trail_points) || !std::isnan(trail_price);

    // Re-issued explicitly partial exits with the same id are one-shot for a live position.
    if (is_partial && !effectively_flat
        && consumed_partial_exit_ids_.find(id) != consumed_partial_exit_ids_.end()) {
        return;
    }

    int64_t preserved_seq = 0;
    double preserved_reserved_qty = std::numeric_limits<double>::quiet_NaN();
    clear_existing_exit_order(id, from_entry, has_trail_request,
                              preserved_seq, preserved_reserved_qty);

    double reserved_qty = std::numeric_limits<double>::quiet_NaN();
    bool bind_global_full_exit_dynamic_qty = false;
    std::vector<std::size_t> pooc_global_full_exit_bound_add_indices;
    if (has_explicit_qty) {
        // Honour the explicit qty literally (clamped to the live position
        // and subject to the same already-reserved accounting). This is
        // the path Pine's ``strategy.exit(... qty=N)`` follows when N is
        // strictly smaller than the open position size — required for
        // multi-bracket per-position exits (validation_oca/oca-three-way-
        // probe-02 has two qty=1 brackets attached to a qty=2 entry).
        if (effectively_flat) {
            // Defer placement; FIFO accounting will recompute when a
            // position eventually exists.
            reserved_qty = std::min(qty, std::numeric_limits<double>::infinity());
        } else {
            double already_reserved = 0.0;
            for (const auto& o : pending_orders_) {
                if (o.type != OrderType::EXIT || o.from_entry != from_entry) continue;
                if (!std::isnan(o.qty)) {
                    already_reserved += o.qty;
                } else {
                    double oqp = std::isnan(o.qty_percent) ? 100.0
                        : std::clamp(o.qty_percent, 0.0, 100.0);
                    already_reserved += live_pos_qty * (oqp / 100.0);
                }
            }
            // Reservation capacity: by default the live position net of any
            // same-bar batched close (legacy behaviour, post-close sizing).
            // But when a PENDING entry order with
            // id == from_entry exists, TV binds the bracket to THAT entry's
            // eventual fills, not to the unrelated live position: a reversal
            // bar places qty=1 brackets for the about-to-fill opposite qty=2
            // entry while the old position (1 lot the other way) is still
            // open. Clamping to the live position dropped every bracket
            // after the first (thulashimohanr-prev-day-week-levels probe:
            // RevShortT2's stop order never existed, so the engine sailed
            // through TV's overnight stop-out and desynced for days).
            // Capacity then = open fills already tagged from_entry (same-id
            // pyramiding remainder) + the pending entry's qty (unbounded
            // when the entry's qty only resolves at fill time).
            double capacity = live_pos_qty;
            bool entry_pending = false;
            double pending_entry_qty = 0.0;
            for (const auto& o : pending_orders_) {
                if (o.id != from_entry) continue;
                if (o.type != OrderType::MARKET && o.type != OrderType::ENTRY
                    && o.type != OrderType::RAW_ORDER) continue;
                entry_pending = true;
                if (std::isnan(o.qty)) {
                    pending_entry_qty = std::numeric_limits<double>::infinity();
                } else {
                    pending_entry_qty += o.qty;
                }
            }
            if (entry_pending) {
                double open_from_entry = 0.0;
                for (const auto& pe : pyramid_entries_) {
                    if (pe.entry_id == from_entry) open_from_entry += pe.qty;
                }
                capacity = open_from_entry + pending_entry_qty;
            }
            double available = std::max(0.0, capacity - already_reserved);
            reserved_qty = std::min(qty, available);
            if (reserved_qty <= kQtyEpsilon) return;
        }
        is_partial = reserved_qty < live_pos_qty - kFullQtyEps;
    } else {
        // Default-sized (percent) bracket armed while its from_entry ENTRY is
        // still a PENDING order in the OPPOSITE direction of the live
        // position (the reversal-bar shape: strategy.entry(X) +
        // strategy.exit(from_entry=X) issued together while the old opposite
        // position is still open). TV binds the bracket to X's eventual
        // fill — the bracket closes 100% of the lot the entry actually
        // opens. Freezing reserved_qty at the CURRENT position size (the
        // old, about-to-be-replaced side) under-sizes the bracket whenever
        // the fresh percent-of-equity lot exceeds the old position, leaving
        // a dust remainder (q_plain - |old|) open when the bracket fires —
        // the seed of jevondijefferson's multi-day tiny-qty desync chains
        // (2025-05-23 12:00, 2025-10-04 15:30, 2026-02-13 15:15,
        // 2026-02-22 13:45 UTC: e.g. 10-04 bracket froze at 4.3847 against
        // the new 4.5089 short, stranding 0.1242). Defer the reservation
        // (qty = NaN): the fill-side path then executes a FULL exit against
        // the live position, exactly like a bracket placed while flat.
        // Mirrors the explicit-qty path's pending-entry capacity rule above
        // (thulashimohanr fix); entries with an explicit qty keep the
        // legacy reservation math.
        bool bind_to_pending_reversal_entry = false;
        if (!from_entry.empty() && !effectively_flat
            && qp >= 100.0 - kFullPercentEps) {
            for (const auto& o : pending_orders_) {
                if (o.id != from_entry) continue;
                if (o.type != OrderType::MARKET && o.type != OrderType::ENTRY
                    && o.type != OrderType::RAW_ORDER) continue;
                PositionSide entry_dir = o.is_long ? PositionSide::LONG
                                                   : PositionSide::SHORT;
                if (entry_dir != position_side_ && std::isnan(o.qty)) {
                    bind_to_pending_reversal_entry = true;
                }
                break;  // entry ids are unique in pending_orders_
            }
        }

        // POOC global-full-exit reservation: when the complete pending
        // entry-like queue consists only of ordinary same-direction high-level
        // MARKET adds created on this bar, each under the pyramiding cap, those
        // adds fill at C before this later-created priced exit can trigger. A
        // global (omitted from_entry) 100% bracket covers that post-add
        // position on TradingView. Keep the normal finite reservation for
        // sibling accounting, then mark this one order so the fill path closes
        // the full live position after the adds have joined it.
        //
        // This is deliberately narrower than the reversal binding above:
        // POOC only; every pending MARKET/ENTRY/RAW order must be an ordinary
        // same-bar high-level MARKET entry on the same side at placement and
        // now, under the pyramiding cap; at least one such add must exist;
        // global full-percent default sizing only. Explicit exit qty uses the
        // separate branch, while from_entry, partial, RAW_ORDER, priced,
        // opposite, prior-bar, COOF-recalc, over-cap, and mixed-queue shapes
        // retain the established frozen reservation.
        bool eligible_global_full_exit_dynamic_qty = false;
        if (from_entry.empty()
            && process_orders_on_close_
            && !effectively_flat
            && qp >= 100.0 - kFullPercentEps) {
            bool found_qualifying_add = false;
            bool entry_queue_is_bounded = true;
            for (std::size_t i = 0; i < pending_orders_.size(); ++i) {
                const PendingOrder& o = pending_orders_[i];
                if (o.type != OrderType::MARKET
                    && o.type != OrderType::ENTRY
                    && o.type != OrderType::RAW_ORDER) {
                    continue;
                }
                const PositionSide entry_dir = o.is_long
                    ? PositionSide::LONG : PositionSide::SHORT;
                const bool qualifies = o.created_bar == bar_index_
                    && o.type == OrderType::MARKET
                    && !o.created_during_coof_recalc
                    && !o.over_pyramiding_cap_at_placement
                    && entry_dir == position_side_
                    && o.created_position_side == position_side_;
                if (!qualifies) {
                    entry_queue_is_bounded = false;
                    break;
                }
                found_qualifying_add = true;
                pooc_global_full_exit_bound_add_indices.push_back(i);
            }
            eligible_global_full_exit_dynamic_qty =
                found_qualifying_add
                && entry_queue_is_bounded;
        }

        if (!bind_to_pending_reversal_entry) {
            if (!compute_exit_reserved_qty(
                    from_entry, preserved_reserved_qty, live_pos_qty,
                    qp, is_partial, reserved_qty)) {
                return;
            }
            eligible_global_full_exit_dynamic_qty =
                eligible_global_full_exit_dynamic_qty
                && !is_partial
                && std::isfinite(reserved_qty)
                && reserved_qty >= live_pos_qty - kFullQtyEps;
        }

        // The pending order stores this below after the common construction.
        bind_global_full_exit_dynamic_qty =
            eligible_global_full_exit_dynamic_qty;
    }

    PendingOrder order;
    order.id = id;
    order.from_entry = from_entry;
    order.type = OrderType::EXIT;
    order.is_long = false;
    order.limit_price = limit_price;
    order.stop_price = stop_price;
    order.trail_points = trail_points;
    order.trail_price = trail_price;
    order.trail_offset = trail_offset;
    order.profit_ticks = profit_ticks;
    order.loss_ticks = loss_ticks;
    order.qty = reserved_qty;
    order.qty_type = -1;
    order.qty_percent = qp;
    order.requested_partial = is_partial;
    order.pooc_global_full_exit_dynamic_qty =
        bind_global_full_exit_dynamic_qty;
    order.pooc_global_full_exit_tracks_bound_adds =
        bind_global_full_exit_dynamic_qty;
    if (bind_global_full_exit_dynamic_qty) {
        for (std::size_t index : pooc_global_full_exit_bound_add_indices) {
            pending_orders_[index].pooc_global_full_exit_bound_add = true;
        }
    }
    // OCA-name plumbing: ``strategy.exit`` supports oca_name (Pine v6) so
    // siblings in different OCA groups can fire independently. The cancel
    // sweep predicate (engine_fills.cpp::apply_filled_order_to_state →
    // cancel_oca_group) already isolates groups by name; without this
    // assignment all strategy.exit-issued orders shared an empty name and
    // the first bracket's TP would silently leave the other bracket's
    // legs intact (probe oca-three-way-02 lost ~42% of its trades).
    order.oca_name = oca_name;
    order.oca_type = oca_name.empty() ? 0 : 1;  // strategy.exit semantics: cancel
    order.created_bar = bar_index_;
    order.created_seq = preserved_seq > 0 ? preserved_seq : next_order_seq_++;
    order.incarnation = next_order_incarnation_++;
    order.created_during_coof_recalc = coof_fill_recalc_active_;
    order.coof_born_at_close_recalc =
        coof_fill_recalc_active_ && coof_cursor_is_bar_close_;
    // KI-67: a fill recalc that was NOT triggered at the bar-open tick is a
    // mid-bar recalc; orders it places are cascade orders (eligible only at the
    // remaining extreme waypoints of the historical 4-tick path).
    order.coof_born_mid_bar =
        coof_fill_recalc_active_ && !coof_recalc_at_bar_open_;
    if (coof_fill_recalc_active_ && coof_scheduler_active_
        && std::isfinite(coof_cursor_price_)
        && position_side_ != PositionSide::FLAT
        && position_open_bar_ == bar_index_) {
        const bool closing_long = position_side_ == PositionSide::LONG;
        const bool stop_marketable = !std::isnan(stop_price)
            && (closing_long ? coof_cursor_price_ <= stop_price
                             : coof_cursor_price_ >= stop_price);
        const bool limit_marketable = !std::isnan(limit_price)
            && (closing_long ? coof_cursor_price_ >= limit_price
                             : coof_cursor_price_ <= limit_price);
        order.coof_suppress_stop_on_entry_bar = stop_marketable;
        order.coof_suppress_limit_on_entry_bar = limit_marketable;
    }
    // KI-67 exit cascade (Model S). Record this mid-bar cascade exit's in-flight
    // leg so the historical dispatch gate can hold it on that leg's remainder,
    // exact-fill it on subsequent legs, and gap-fill it at the in-flight leg-end
    // waypoint. seg_i is the loop's REAL in-flight leg at this recalc — not
    // re-derived from the recalc price, which is ambiguous when the triggering
    // fill lands exactly on a waypoint ("a fill AT a waypoint starts the NEXT
    // leg"). current_bar_ is the full script bar during a fill recalc; the
    // magnifier path owns its own tick model and is scoped out.
    if (order.coof_born_mid_bar && !bar_magnifier_enabled_
        && coof_scheduler_active_ && std::isfinite(coof_cursor_price_)
        && position_side_ != PositionSide::FLAT
        && (!std::isnan(order.stop_price) || !std::isnan(order.limit_price))
        && std::isnan(order.trail_points) && std::isnan(order.trail_price)) {
        const int si = coof_cascade_recalc_leg_;
        order.coof_cascade_seg_i =
            (si >= 0 && si <= 2) ? static_cast<int8_t>(si)
                                 : static_cast<int8_t>(-1);
        order.coof_cascade_inflight_fires = internal::cascade_exit_inflight_fires(
            current_bar_, coof_cursor_price_, si, position_side_,
            order.stop_price, order.limit_price);
    }
    // Position-derived captures use the post-batched-close view (see
    // live_pos_qty above) so an exit armed after a same-bar strategy.close
    // records the same state it did when the close executed mid-bar.
    order.created_position_side = effectively_flat ? PositionSide::FLAT : position_side_;
    order.tv_carry_qty = live_pos_qty;
    order.comment = comment;
    order.created_while_in_position = !effectively_flat;

    pending_orders_.push_back(std::move(order));
}

void BacktestEngine::strategy_cancel(const std::string& id) {
    for (const PendingOrder& order : pending_orders_) {
        if (order.id == id) {
            if ((order.type == OrderType::MARKET
                 || order.type == OrderType::ENTRY
                 || order.type == OrderType::RAW_ORDER)
                && order.created_position_side == PositionSide::FLAT) {
                pending_flat_market_pair_disqualified_bars_.insert(
                    order.created_bar);
            }
            invalidate_pending_flat_market_pair(order.created_seq);
        }
    }
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) { return o.id == id; }),
        pending_orders_.end());
}

void BacktestEngine::strategy_cancel_all() {
    for (const PendingOrder& order : pending_orders_) {
        if ((order.type == OrderType::MARKET
             || order.type == OrderType::ENTRY
             || order.type == OrderType::RAW_ORDER)
            && order.created_position_side == PositionSide::FLAT) {
            pending_flat_market_pair_disqualified_bars_.insert(
                order.created_bar);
        }
    }
    pending_orders_.clear();
}

void BacktestEngine::strategy_order(const std::string& id, bool is_long, double qty,
                                     double limit_price, double stop_price,
                                     const std::string& oca_name, int oca_type) {
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, script_tf_seconds_)) return;
    int64_t preserved_seq = 0;
    for (const auto& o : pending_orders_) {
        if (o.id == id) {
            preserved_seq = o.created_seq;
            break;
        }
    }

    // Remove existing pending order with same id
    for (const PendingOrder& pending : pending_orders_) {
        if (pending.id == id) {
            if ((pending.type == OrderType::MARKET
                 || pending.type == OrderType::ENTRY
                 || pending.type == OrderType::RAW_ORDER)
                && pending.created_position_side == PositionSide::FLAT) {
                pending_flat_market_pair_disqualified_bars_.insert(
                    pending.created_bar);
            }
            invalidate_pending_flat_market_pair(pending.created_seq);
        }
    }
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) { return o.id == id; }),
        pending_orders_.end());

    PendingOrder order;
    order.id = id;
    order.from_entry = "";
    order.is_long = is_long;
    order.trail_points = std::numeric_limits<double>::quiet_NaN();
    order.trail_price = std::numeric_limits<double>::quiet_NaN();
    order.trail_offset = std::numeric_limits<double>::quiet_NaN();
    order.qty = qty;
    order.qty_type = -1;
    order.qty_percent = 100.0;
    order.oca_name = oca_name;
    order.oca_type = oca_type;
    order.created_bar = bar_index_;
    order.created_seq = preserved_seq > 0 ? preserved_seq : next_order_seq_++;
    order.incarnation = next_order_incarnation_++;
    order.created_during_coof_recalc = coof_fill_recalc_active_;
    order.coof_born_at_close_recalc =
        coof_fill_recalc_active_ && coof_cursor_is_bar_close_;
    // KI-67: a fill recalc that was NOT triggered at the bar-open tick is a
    // mid-bar recalc; orders it places are cascade orders (eligible only at the
    // remaining extreme waypoints of the historical 4-tick path).
    order.coof_born_mid_bar =
        coof_fill_recalc_active_ && !coof_recalc_at_bar_open_;
    order.created_position_side = position_side_;
    order.created_after_position_close_in_bar =
        pending_close_qty_in_bar_ > kQtyEpsilon;
    // Same placement-time over-cap snapshot as strategy_entry, mirroring the
    // strategy.order add gate (engine_fills.cpp same-direction RAW add). A
    // strategy.order market/priced order is a RAW_ORDER and is not currently a
    // target of the same-direction post-full-close wipe (which keys on
    // MARKET/ENTRY), so this flag is inert for the RAW path today; it is
    // captured here for consistency so the provenance stays correct if the
    // wipe is ever widened to RAW_ORDER adds.
    order.over_pyramiding_cap_at_placement =
        position_side_ != PositionSide::FLAT
        && position_side_ == (is_long ? PositionSide::LONG : PositionSide::SHORT)
        && position_entry_count_ >= pyramiding_;
    order.tv_carry_qty = position_qty_;

    bool has_limit = !std::isnan(limit_price);
    bool has_stop = !std::isnan(stop_price);

    if (!has_limit && !has_stop) {
        order.type = OrderType::RAW_ORDER;
        order.limit_price = std::numeric_limits<double>::quiet_NaN();
        order.stop_price = std::numeric_limits<double>::quiet_NaN();
        // Same signal-time freeze as strategy_entry's MARKET branch: a
        // default-sized strategy.order market order runs through the same
        // TV default-sizing engine, so its quantity is frozen at this
        // (signal) bar's close too. Stored off to the side (order.qty stays
        // NaN) for the same reason as in strategy_entry.
        if (std::isnan(qty)
            && (default_qty_type_ == QtyType::PERCENT_OF_EQUITY
                || default_qty_type_ == QtyType::CASH)
            && !std::isnan(current_bar_.close)) {
            order.frozen_default_qty = frozen_default_market_qty(/*is_buy=*/is_long);
            // KI-54: same admission snapshot as strategy_entry's MARKET
            // branch. The fill-time gate skips opposite-direction RAW fills
            // (they only close the position) — see
            // apply_filled_order_to_state.
            order.sizing_price = frozen_sizing_price(/*is_buy=*/is_long);
            order.sizing_equity =
                current_equity() + open_profit(current_bar_.close);
            order.sizing_mark = current_bar_.close;
        }
    } else {
        order.type = OrderType::RAW_ORDER;
        order.limit_price = limit_price;
        order.stop_price = stop_price;
    }

    pending_orders_.push_back(std::move(order));
    invalidate_unsafe_pooc_global_full_exit_dynamic_qty();
}

void BacktestEngine::invalidate_unsafe_pooc_global_full_exit_dynamic_qty() {
    // This helper is called only after a later entry-like placement was
    // successfully admitted. The oracle covers adds already pending BEFORE
    // the global exit, not any order emitted after it—even another same-side
    // MARKET add. Clear every still-live marker, including candidates carried
    // into a later bar; their finite ``qty`` remains the conservative
    // reservation automatically.
    for (auto& o : pending_orders_) {
        if (o.type == OrderType::EXIT) {
            o.pooc_global_full_exit_dynamic_qty = false;
        }
    }
}

// ────────────────────────────────────────────────────────────────────
// strategy_close / strategy_exit helpers
// ────────────────────────────────────────────────────────────────────

// Validate a strategy.close request against the live position and
// pyramid roster, returning the qty to actually close. Sets
// matching_qty / qty_to_close / all_entries_match. Returns false when
// the id specifies an unknown entry or the resolved qty rounds to
// zero, signalling the caller to early-return.
bool BacktestEngine::compute_close_target_qty(const std::string& id,
                                              double qty,
                                              double qty_percent,
                                              double& matching_qty_out,
                                              double& qty_to_close_out,
                                              bool& all_entries_match_out) {
    const double eps0 = kQtyEpsilon;
    // Default FIFO close-entries rule: strategy.close(id) with no explicit
    // qty/qty_percent closes the UNCLOSED quantity tagged `id`
    // (id_unclosed_qty_) and FIFO-attributes the resulting trade records to
    // the OLDEST open entries (handled downstream by the plain FIFO drain).
    //
    // This is what TradingView does: close(id) closes the quantity entered
    // under `id` and not yet targeted by a prior close(id) — it does NOT
    // re-sum the physical open lots carrying that id. The two agree when each
    // id maps to a single open lot. They diverge for grid bots that re-use
    // one entry id across sequential buy/sell cycles: the FIFO trade-record
    // drain removes the oldest lot (often a DIFFERENT id), so the id-tagged
    // lot stays physically open even though a prior close(id) already
    // accounted for it. Summing physical lots then double-closes it (engine
    // over-closes 2x), while a TP whose id-lot was drained away by an earlier
    // close would find no physical match and be skipped (engine under-closes)
    // — both fixed here by consulting the logical ledger instead.
    //
    // The ANY rule keeps the physical id-matched path (closes_any_qty).
    if (!close_entries_rule_any_ && !id.empty()
        && std::isnan(qty) && std::isnan(qty_percent)) {
        auto it = id_unclosed_qty_.find(id);
        double unclosed = (it != id_unclosed_qty_.end()) ? it->second : 0.0;
        double target = std::min(unclosed, position_qty_);
        all_entries_match_out = false;  // FIFO drain may span lots of other ids
        if (target <= eps0) {
            return false;
        }
        matching_qty_out = target;
        qty_to_close_out = target;
        // Consume the id's unclosed ledger now that the close commits.
        it->second -= target;
        if (it->second <= eps0) {
            id_unclosed_qty_.erase(it);
        }
        return true;
    }

    bool has_matching_entry = id.empty();
    all_entries_match_out = id.empty() ? true : !pyramid_entries_.empty();
    matching_qty_out = id.empty() ? position_qty_ : 0.0;
    if (!id.empty()) {
        has_matching_entry = false;
        for (const auto& pe : pyramid_entries_) {
            if (pe.entry_id == id) {
                has_matching_entry = true;
                matching_qty_out += pe.qty;
            } else {
                all_entries_match_out = false;
            }
        }
    }

    if (!id.empty() && !has_matching_entry) {
        return false;
    }

    const double eps = kQtyEpsilon;
    qty_to_close_out = matching_qty_out;
    if (!std::isnan(qty)) {
        qty_to_close_out = std::min(std::max(qty, 0.0), matching_qty_out);
    } else if (!std::isnan(qty_percent)) {
        double pct = std::clamp(qty_percent, 0.0, 100.0);
        qty_to_close_out = matching_qty_out * (pct / 100.0);
    }
    if (qty_to_close_out <= eps) {
        return false;
    }
    return true;
}

// Wipe pending orders that should not survive a full strategy.close:
//
//   Pending strategy.exit orders bound to the same entry id are wiped
//   (community/IES regression: a partial TP1 limit and the queued
//   market close were both firing on the next bar's open, producing
//   two trade rows for the same logical close).
//
// Pending strategy.entry / market orders are LEFT ALONE. Per
// TradingView's documented broker semantics, only cancel()/cancel_all()
// cancel pending orders; close()/close_all() only closes the open
// position — a pending same-direction entry (e.g. a stale add-on stop
// placed while the position was open) survives and can still fire on a
// later bar, "ghost-refilling" into a new position. This mirrors DCA/
// grid-bot bots (3commas-style: N independent price-level orders,
// closing one level must not silently cancel another level's still-
// pending order) — verified against 3commas-3commas-pullback-sniper-
// strategy, where the previous same-direction wipe was itself the bug
// (closed-form count-delta 2.88% -> <0.5%).
void BacktestEngine::cancel_orders_for_full_close(const std::string& id, bool /*closing_long*/) {
    pending_orders_.erase(
        std::remove_if(
            pending_orders_.begin(),
            pending_orders_.end(),
            [&](const PendingOrder& o) {
                if (o.type != OrderType::EXIT) {
                    return false;
                }
                if (id.empty()) {
                    return o.from_entry.empty();
                }
                return o.from_entry == id;
            }),
        pending_orders_.end());
}

void BacktestEngine::cancel_same_bar_market_reentries_after_full_close(
        bool closed_long, bool preserve_undercap_entries) {
    const PositionSide closed_side = closed_long ? PositionSide::LONG : PositionSide::SHORT;
    pending_orders_.erase(
        std::remove_if(
            pending_orders_.begin(),
            pending_orders_.end(),
            [&](const PendingOrder& o) {
                // Deferred full exits already remove same-direction market
                // entries through process_pending_orders' exit_closed_from_bar
                // machinery. POOC/immediate closes execute outside that loop,
                // so mirror only the market-reentry cleanup here. An ordinary
                // POOC close_all preserves an entry created before it when the
                // entry was under the pyramiding cap at placement;
                // over-cap entries still drop. Explicit immediately=true and
                // flush-time strategy.close(id) keep blanket cancellation.
                // Priced entries intentionally survive, and opposite-direction
                // market entries remain valid reversals.
                return o.type == OrderType::MARKET
                    && o.created_bar == bar_index_
                    && o.created_position_side == closed_side
                    && o.is_long == closed_long
                    && (!preserve_undercap_entries
                        || o.over_pyramiding_cap_at_placement);
            }),
        pending_orders_.end());
}

// Run the close at the current bar's close price (the
// process_orders_on_close / strategy.close(immediately=true) path).
// Dispatches between full, FIFO-partial, and by-entry-percent partial
// exit primitives, then tags the new trade rows with comment + exit_id.
void BacktestEngine::execute_immediate_close(const std::string& id,
                                             const std::string& comment,
                                             double qty_to_close,
                                             double matching_qty,
                                             bool closes_full_position,
                                             bool closes_fifo_qty,
                                             bool closes_any_qty,
                                             bool preserve_undercap_entries) {
    const double eps = kQtyEpsilon;
    size_t trades_before = trades_.size();
    PositionSide side_before = position_side_;
    double qty_before = position_qty_;
    const double broker_price =
        coof_scheduler_active_ && std::isfinite(coof_cursor_price_)
            ? coof_cursor_price_ : current_bar_.close;
    if (closes_full_position) {
        const bool closed_long = (position_side_ == PositionSide::LONG);
        execute_market_exit(broker_price);
        purge_exit_orders();
        if (position_side_ == PositionSide::FLAT) {
            cancel_same_bar_market_reentries_after_full_close(
                closed_long, preserve_undercap_entries);
        }
    } else if (closes_fifo_qty) {
        execute_partial_exit_qty(broker_price, qty_to_close);
        if (position_side_ == PositionSide::FLAT) {
            purge_exit_orders();
        }
    } else if (closes_any_qty) {
        double pct = matching_qty > eps ? (qty_to_close / matching_qty) * 100.0 : 100.0;
        execute_partial_exit_by_entry_percent(broker_price, id, pct);
        if (position_side_ == PositionSide::FLAT) {
            purge_exit_orders();
        }
    }
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = comment;
        trades_[ti].exit_id = "__close__" + id;
    }
    if (position_side_ != side_before
        || std::abs(position_qty_ - qty_before) > eps
        || trades_.size() != trades_before) {
        ++broker_fill_event_seq_;
        if (coof_scheduler_active_ && coof_direct_fill_events_remaining_ > 0) {
            --coof_direct_fill_events_remaining_;
        }
    }
}

// Build the deferred EXIT pending order representing this close, to
// be matched at the next bar's open by process_pending_orders. Mirrors
// the qty / qty_percent shape that the partial-exit dispatch in
// execute_immediate_close would have produced for the same flags.
uint64_t BacktestEngine::queue_deferred_close_order(
        const std::string& id,
        const std::string& comment,
        double qty_to_close,
        double matching_qty,
        bool closes_full_position,
        bool closes_any_qty,
        double consumed_ledger_qty) {
    const double eps = kQtyEpsilon;
    PendingOrder order;
    order.id = "__close__" + id;
    order.from_entry = close_entries_rule_any_ ? id : "";
    order.type = OrderType::EXIT;
    order.is_long = false;
    order.limit_price = std::numeric_limits<double>::quiet_NaN();
    order.stop_price = std::numeric_limits<double>::quiet_NaN();
    order.trail_points = std::numeric_limits<double>::quiet_NaN();
    order.trail_price = std::numeric_limits<double>::quiet_NaN();
    order.trail_offset = std::numeric_limits<double>::quiet_NaN();
    if (closes_any_qty) {
        order.qty = std::numeric_limits<double>::quiet_NaN();
        order.qty_type = -1;
        order.qty_percent = matching_qty > eps ? (qty_to_close / matching_qty) * 100.0 : 100.0;
    } else {
        order.qty = closes_full_position ? std::numeric_limits<double>::quiet_NaN() : qty_to_close;
        order.qty_type = -1;
        order.qty_percent = closes_full_position ? 100.0
            : (position_qty_ > eps ? (qty_to_close / position_qty_) * 100.0 : 100.0);
    }
    order.oca_name = "";
    order.oca_type = 0;
    order.created_bar = bar_index_;
    order.created_seq = next_order_seq_++;
    order.incarnation = next_order_incarnation_++;
    order.created_during_coof_recalc = coof_fill_recalc_active_;
    order.coof_born_at_close_recalc =
        coof_fill_recalc_active_ && coof_cursor_is_bar_close_;
    // KI-67: a fill recalc that was NOT triggered at the bar-open tick is a
    // mid-bar recalc; orders it places are cascade orders (eligible only at the
    // remaining extreme waypoints of the historical 4-tick path).
    order.coof_born_mid_bar =
        coof_fill_recalc_active_ && !coof_recalc_at_bar_open_;
    order.created_position_side = position_side_;
    order.tv_carry_qty = position_qty_;
    order.comment = comment;
    order.created_while_in_position = true;
    // design-declined-reversal-close-leg: the qty this close debited from
    // id_unclosed_qty_ at CALL time (default-FIFO branch), so a later
    // suppression can re-credit exactly that amount. NaN when nothing was
    // debited (ANY rule / explicit qty / close_all).
    order.suppressed_close_consumed_ledger_qty = consumed_ledger_qty;

    const uint64_t incarnation = order.incarnation;
    pending_orders_.push_back(std::move(order));
    return incarnation;
}

// Capture seq + reserved qty of an existing pending exit with the
// same (id, from_entry), reset the trail high-water mark when starting
// a fresh trail (no prior order, in-position), and erase the matching
// pending EXIT order so the caller can push a freshly built
// replacement.
void BacktestEngine::clear_existing_exit_order(const std::string& id,
                                               const std::string& from_entry,
                                               bool has_trail_request,
                                               int64_t& preserved_seq_out,
                                               double& preserved_reserved_qty_out) {
    bool had_existing_order = false;
    preserved_seq_out = 0;
    preserved_reserved_qty_out = std::numeric_limits<double>::quiet_NaN();
    for (const auto& o : pending_orders_) {
        if (o.type == OrderType::EXIT && o.id == id && o.from_entry == from_entry) {
            had_existing_order = true;
            preserved_seq_out = o.created_seq;
            if (!std::isnan(o.qty)) {
                preserved_reserved_qty_out = o.qty;
            }
            break;
        }
    }

    if (has_trail_request && !had_existing_order && position_side_ != PositionSide::FLAT) {
        trail_best_price_ = current_bar_.close;
    }

    // Erase only the matching prior EXIT order — mirror the gated lookup
    // above. TradingView keeps entry-order ids and exit-order ids in
    // independent namespaces: a strategy.exit replacing its prior order
    // must never clobber a same-bar pending strategy.entry that happens to
    // reuse the id string, nor a sibling exit attached to a different
    // from_entry. A bare ``o.id == id`` predicate deleted the still-pending
    // entry, so the strategy never opened a position (zero trades).
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) {
                return o.type == OrderType::EXIT
                    && o.id == id
                    && o.from_entry == from_entry;
            }),
        pending_orders_.end());
}

// Compute the qty this strategy.exit can reserve against the current
// position, accounting for already-pending sibling exits (same
// from_entry). Updates qp / is_partial to reflect the actual reserved
// fraction. Also enforces "ignore additional partial exits while a
// full exit is already pending for this from_entry". Returns false
// (caller should abort) when the available qty is zero or a blocking
// full exit is queued.
bool BacktestEngine::compute_exit_reserved_qty(const std::string& from_entry,
                                               double preserved_reserved_qty,
                                               double live_pos_qty,
                                               double& qp_io,
                                               bool& is_partial_io,
                                               double& reserved_qty_out) {
    // live_pos_qty: the position size this exit may size against — the raw
    // position_qty_ minus any same-bar batched strategy.close target that is
    // committed but not yet flushed (see strategy_exit). <= eps behaves like
    // FLAT: defer, recompute when a position exists.
    reserved_qty_out = std::numeric_limits<double>::quiet_NaN();
    if (position_side_ == PositionSide::FLAT || live_pos_qty <= kQtyEpsilon) {
        return true;
    }

    double already_reserved = 0.0;
    for (const auto& o : pending_orders_) {
        if (o.type != OrderType::EXIT || o.from_entry != from_entry) continue;
        if (!std::isnan(o.qty)) {
            already_reserved += o.qty;
        } else {
            double oqp = std::isnan(o.qty_percent) ? 100.0 : std::clamp(o.qty_percent, 0.0, 100.0);
            already_reserved += live_pos_qty * (oqp / 100.0);
        }
    }

    double available_qty = std::max(0.0, live_pos_qty - already_reserved);
    // Only carry the preserved (frozen) reserved qty for genuine PARTIAL
    // re-issues (qp < 100%). A full-position exit (qp == 100%) re-issued
    // every bar while the position keeps GROWING via pyramiding/DCA must
    // re-expand to 100% of the now-larger position rather than stay frozen
    // at the size captured when it was first placed; otherwise the TP touch
    // closes only the first FIFO lot at the true limit and the residual lots
    // exit one bar late at a re-priced limit/next-bar-open (one logical exit
    // fragmenting across two bars). For partial re-issues the carry is kept
    // to avoid double-reserving against the same from_entry.
    if (!std::isnan(preserved_reserved_qty) && qp_io < 100.0 - kFullPercentEps) {
        reserved_qty_out = std::min(preserved_reserved_qty, live_pos_qty);
    } else {
        double requested_qty = live_pos_qty * (qp_io / 100.0);
        // TV floors each percent-derived PARTIAL exit lot to the
        // instrument lot step at placement (apply_exit_qty_step doc has
        // the row-level evidence); the sub-step remainder stays open as
        // a dust position instead of being closed by the final bracket
        // leg. A dust-sized request that floors to zero is simply not
        // placed (the kQtyEpsilon check below aborts), which is also
        // TV's behaviour: dust positions carry no TP bracket and only
        // ever close via reversal/close_all/margin call. Full-position
        // exits (qp == 100%) are left exact so they always flatten.
        if (qp_io < 100.0 - kFullPercentEps) {
            requested_qty = apply_exit_qty_step(requested_qty);
        }
        reserved_qty_out = std::min(requested_qty, available_qty);
    }
    if (reserved_qty_out <= kQtyEpsilon) {
        return false;
    }
    qp_io = (live_pos_qty > kQtyEpsilon) ? (reserved_qty_out / live_pos_qty) * 100.0 : qp_io;
    is_partial_io = reserved_qty_out < live_pos_qty - kFullQtyEps;

    // If there is already a full exit pending for this from_entry, ignore
    // additional partial exits until that full exit is consumed/cancelled.
    if (is_partial_io) {
        for (const auto& o : pending_orders_) {
            if (o.type != OrderType::EXIT) continue;
            if (o.from_entry != from_entry) continue;
            double oqp = std::isnan(o.qty_percent) ? 100.0 : std::clamp(o.qty_percent, 0.0, 100.0);
            if (oqp >= 100.0 - kFullPercentEps) {
                return false;
            }
        }
    }
    return true;
}

} // namespace pineforge
