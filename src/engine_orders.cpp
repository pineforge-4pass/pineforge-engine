/*
 * engine_orders.cpp — execute_market_* and partial-exit fill mechanics
 */

#include "engine_internal.hpp"
#include <pineforge/order_action.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pineforge {
using namespace internal;

namespace {
// Source predicates are resolved here, never retained by native settlement.
// Every fragment of an opening must agree with the selected source predicate.
template<class Predicate>
std::vector<uint64_t> source_opening_membership(
        const std::vector<PyramidEntry>& lots, Predicate selected) {
    std::unordered_map<uint64_t, bool> membership;
    std::vector<uint64_t> incarnations;
    for (const auto& lot : lots) {
        const bool matches = selected(lot);
        if (lot.entry_incarnation == 0) {
            if (matches)
                throw std::runtime_error("invalid resolved bound-close settlement: unowned opening");
            continue;
        }
        const auto [it, inserted] = membership.emplace(lot.entry_incarnation, matches);
        if (!inserted && it->second != matches)
            throw std::runtime_error("invalid resolved bound-close settlement: heterogeneous opening");
        if (inserted && matches) incarnations.push_back(lot.entry_incarnation);
    }
    return incarnations;
}
} // namespace


// Risk management + per-trade extreme tracking moved to engine_risk.cpp.










// Internal helper: execute a market entry (handles reverse-and-open).
//
// Dispatches to one of five case-helpers based on the current position
// state and the entry's close_only_opposite / later_same_tick_entry flags:
//   1. enter_market_from_flat       — position FLAT
//   2. add_to_pyramid_market        — position is same direction as requested
//   3. close_opposite_then_enter    — close-only-opposite branch
//   4. sequential_same_tick_reversal_fill — opposite direction, another
//      same-direction market entry fills later this same tick (TV rule R*)
//   5. flip_market_position_to      — opposite direction (close-and-flip)



// Internal helper: execute a market exit (close position at fill price)



// Range-end accounting for a position still open after the final bar.
//
// TradingView's deep-backtest report — the ws-report-v1 tape the campaign
// grades against — does not leave the last position open. It reports it as
// a CLOSED trade whose exit leg sits on the range's last bar at that bar's
// close, with an empty exit Signal, and counts it in closedTrades. The
// orb-lite probe on NYSE:F 1D is the canonical row: Entry short 2026-03-16
// @ 11.82, Exit 2026-04-30 @ 12.08 — 12.08 is the last close of the range,
// the Signal cell is empty, metrics say closedTrades:1 (the browser export
// writes the same row with Signal "Open", which the verifier already
// recognises; the ws tape gives it no marker at all). On the f-1d spark
// set 8/10 engine runs hold the same position to the last bar (the
// engine's bars-in-market is TV's Duration + 1, the entry bar counted) and
// 7/10 carry a mark-to-market open_pl equal to TV's row to the cent — the
// row is exactly the engine's open position marked at the last close.
//
// The engine exported closed trades only: trades_ grows in emit_close_trade
// alone, and neither run loop flattened at end of feed, so every such probe
// was one trade short against the tape and the verifier scored the missing
// row as an unmatched TV trade. This is the operator-decided (2026-09-02)
// emulation of TradingView's range-end accounting: after the last script
// bar has been dispatched and its equity point recorded, the rows that a
// close of the open position at the last bar's close would record — one
// per pyramid slice, as every other full close, through the same
// build_close_trade arithmetic — are written to range_end_trades_, which
// fill_trades_section merges behind the script's own closed trades.
//
// Reporting only. The live position, the pending orders, trades_ and the
// realized sums are left exactly as the bar loop left them: a stream's
// realtime bars continue the warmup position (stream_begin), the Pine
// accessors never see the row (it is dated after the last on_bar), and the
// broker-mechanics tests keep inspecting the open lot the run ended with.
// The report is where TradingView's accounting is emulated.
//
// Pricing. The row is a mark at the close, not an order the broker
// emulator fills: it takes the raw close rounded to the NEAREST tick
// (bar_fill_price, finding-446 — the tape's 12.08 is the on-tick close; a
// sub-tick print such as 9.565 books 9.56 like any close-priced fill) and
// NO slippage ticks, because slippage models the fill uncertainty of a
// market order and TV prices this row at the bar close itself. The exit
// commission follows the strategy's rules like any close; the one tape in
// hand has commission 0, so that part is the operator's call rather than a
// measured one. The exit is dated on the script bar's label (the equity
// curve's time_ms — magnifier-invariant, the same instant a
// process_orders_on_close fill on that bar reports).
//
// Accounting. The report's closed-trade count, net profit, win/loss
// tallies and commission paid include the rows (TV counts them in
// closedTrades, and its netProfit is net of the row's exit commission). The
// last equity point is re-marked to the flat account — open_profit 0,
// equity = capital + net profit including the rows — so the documented
// identity equity == initial_capital + net_profit + open_profit holds on
// the last bar (test_metrics pins it), as it does on TradingView's own
// curve, whose last point is the account after that close. The bar loop
// had already folded that point's GROSS mark into the scalar drawdown /
// run-up extremes (update_equity_extremes), and the compute_equity_stats
// curve walk reproduces those scalars only while the curve holds exactly
// the values that were folded: a first cut of this change re-marked the
// point and left the scalars alone, which silently broke that identity on
// every commissioned run whose last bar was the trough or the peak (the
// fold had seen the gross mark, the walk saw the net one — review finding,
// 2026-09-02). The fold is one per script bar, paired with the curve
// point, so the scalars are re-folded from the curve here (fold_equity_
// extreme over every point, the re-marked last one included): the walk
// and the scalars agree again, the extremes now read the range-end close
// the way the curve does, and every earlier point is untouched. The
// in-market bar count is the loop's (the last bar was in market). A
// strategy that is flat after the final bar records nothing. The stream
// warmup replay is not a range end (the realtime bars continue it) and is
// skipped.



// Internal helper: execute a partial exit (reduce position by qty, create trade records)
// TradingView creates individual trade records for each partial exit.



// Settle an already resolved same-side fill as a distinct physical lot.
// Admission, sizing, side selection and source interpretation precede this
// seam. It neither consults a pyramiding cap nor invents a source-policy bit.
// The caller supplies the lot's immutable label, identity and fill metadata;
// accounting and stream observations still use the engine's sole lot ledger.
// Test-only compatibility construction seam; no production caller. Live
// source entries use settle_source_opening and the shared settlement owner.
void BacktestEngine::append_same_side_fill(PyramidEntry lot) {
    snapshot_entry_commission(lot);
    const double total_qty = position_qty_ + lot.qty;
    const double average_price =
        (position_entry_price_ * position_qty_ + lot.price * lot.qty) / total_qty;
    append_quoted_lot(std::move(lot), total_qty, average_price);
}

void BacktestEngine::append_quoted_lot(PyramidEntry lot, double total_qty,
                                      double average_price) {
    if (position_entry_count_ == std::numeric_limits<int>::max())
        throw std::overflow_error("position entry counter exhausted");
    position_entry_price_ = average_price;
    position_qty_ = total_qty;
    ++position_entry_count_;
    trail_best_price_ = lot.price;
    pyramid_entries_.push_back(std::move(lot));
    if (stream_observe_actions_) stream_observe_entry(pyramid_entries_.back());
}





// Internal helper: close only entries matching from_entry (close_entries_rule="ANY")



// Internal helper: close an exact quantity only from entries matching
// from_entry. Live-position strategy.exit calls freeze their percent-derived
// reservations into request record::qty; when layered siblings fill on one bar,
// that absolute reservation must survive earlier reductions of the position.



// Internal helper: resolve a genuinely deferred percentage at fill time, then
// close that quantity only from entries matching from_entry.

// Sibling reduction on a group fill is the request's own
// native_order::GroupEffect (Cancel or Reduce), resolved by the execution
// consumer; this file keeps no group helper (ADR-0001, detached comment
// residue).

// ────────────────────────────────────────────────────────────────────
// Shared close-side / position-state helpers
// ────────────────────────────────────────────────────────────────────

// Build a close row using the current source context and resolved price.
// This does not book cash, emit a live trade or update source-day observations;
// range-end reporting also uses this non-mutating row builder.


Trade BacktestEngine::build_close_trade_with_costs(const PyramidEntry& pe, double close_qty,
        double fill_price, bool was_long, double entry_commission,
        double exit_commission,
        const execution::PhysicalExecutionContext& context) const {
    // Realized PnL scales by the instrument point value ($ per point per
    // contract). Crypto/equity (pointvalue=1) is unchanged; futures (e.g. ES=50)
    // multiply the price-difference PnL. The price-difference component is in
    // the symbol's QUOTE currency; multiply by account_currency_fx_ (default
    // 1.0, no-op for the corpus) to report in the strategy's ACCOUNT currency
    // — same conversion the margin gate (engine_strategy_commands.cpp) already
    // applies. Commission is already account-currency-native (calc_commission
    // applies the same fx factor internally for its PERCENT case; cash-per-*
    // is account-currency by construction), so it is NOT scaled again here.
    const double pv = syminfo_.pointvalue;
    double pnl = (was_long ? (fill_price - pe.price) : (pe.price - fill_price))
                 * close_qty * pv * active_account_currency_fx();
    pnl -= entry_commission + exit_commission;
    // TV "Net P&L %" convention (arbitrated 2026-06-12 vs TV export,
    // trade #258 short: 102.44 USD on 2276.66 entry => 4.50%): NET pnl
    // as a percent of entry cost (entry_price * qty * pointvalue, same
    // account_currency_fx_ conversion as pnl above so the ratio is
    // currency-invariant). Long/no-commission degenerates to the old
    // (exit/entry-1) form; shorts diverge on large moves ((entry/exit-1)
    // was wrong). Computed AFTER the commission subtraction above — order
    // matters.
    const double entry_cost = pe.price * close_qty * pv
                              * active_account_currency_fx();
    double pnl_pct = (entry_cost > 0.0) ? (pnl / entry_cost) * 100.0 : 0.0;

    Trade trade;
    trade.entry_time = pe.time;
    trade.exit_time = context.effective_time_ms;
    trade.entry_price = pe.price;
    trade.exit_price = fill_price;
    trade.qty = close_qty;
    trade.pnl = pnl;
    trade.pnl_pct = pnl_pct;
    trade.is_long = was_long;
    trade.entry_bar_index = pe.entry_bar_index;
    trade.exit_bar_index = context.interval_index;
    trade.entry_id = pe.entry_id;
    trade.entry_incarnation = pe.entry_incarnation;
    trade.entry_comment = pe.entry_comment;
    trade.commission = entry_commission + exit_commission;
    // Excursions: TV's per-trade excursion includes the exit fill itself —
    // a stop-out's adverse excursion is at least the loss at the SL fill and
    // a take-profit's favorable excursion includes the move to the TP fill.
    // The per-bar sampler (update_per_trade_extremes) cannot see this: exit
    // fills happen inside request matching and the pyramid entry is
    // removed before the next sample, so same-bar entry+exit trades would
    // otherwise report 0/0. Fold the fill price in here. The carried
    // per-entry extreme is scaled to the closed slice (close_qty/pe.qty) so
    // a partial close reports the slice's USD excursion, matching TV's
    // per-trade-record qty. Both fields stay >= 0 (Pine accessor convention);
    // the TV-export sign flip happens only in the CSV writer.
    const double slice = (pe.qty > 0.0) ? (close_qty / pe.qty) : 1.0;
    const double fill_fav = (was_long ? (fill_price - pe.price) : (pe.price - fill_price))
                      * close_qty;
    double runup = 0.0;
    double drawdown = 0.0;
    if (lot_excursion_hook_) {
        // The host owns this lot's excursion (RULING A48): it sampled the
        // lot's path itself and returns the two magnitudes for the closing
        // row. The kernel contributes nothing beyond the booking facts.
        ClosedLotExcursionFacts facts;
        facts.entry_incarnation = pe.entry_incarnation;
        facts.entry_time_ms = pe.time;
        facts.entry_price = pe.price;
        facts.lot_qty = pe.qty;
        facts.closed_qty = close_qty;
        facts.fill_price = fill_price;
        facts.carried_favorable = pe.max_runup;
        facts.carried_adverse = pe.max_drawdown;
        facts.is_long = was_long;
        facts.entry_bar_index = pe.entry_bar_index;
        facts.exit_bar_index = context.interval_index;
        facts.entry_bar_high_masked = pe.skip_entry_bar_high;
        facts.entry_bar_low_masked = pe.skip_entry_bar_low;
        const ClosedLotExcursion owned = lot_excursion_hook_(facts);
        runup = owned.favorable;
        drawdown = owned.adverse;
    } else {
    runup = std::max(pe.max_runup * slice, fill_fav);
    drawdown = std::max(pe.max_drawdown * slice, -fill_fav);
    // Priced (stop/limit/trail) exits fill mid-bar: the bar-path extremes the
    // assumed OHLC path reaches BEFORE the exit fill belong to this trade's
    // excursion, but per-bar sampling never sees them (the entry is removed
    // before the next update_per_trade_extremes). Fold them in here, honoring
    // the entry-side masks when the trade opened on this same bar (an extreme
    // that precedes the ENTRY fill is not part of the trade either).
    // TRAIL fills: the peak that armed the trail (fill +/- offset) is a
    // pre-fill favorable excursion no bar-boundary sample sees (TV reports
    // MFE == peak for trail exits).
    if (context.preceding_exit_trail_peak) {
        const double peak = *context.preceding_exit_trail_peak;
        double peak_fav = (was_long ? (peak - pe.price) : (pe.price - peak)) * close_qty;
        runup = std::max(runup, peak_fav);
    }
    if (context.preceding_exit_path_prefix && *context.preceding_exit_path_prefix) {
        double fill_pos = 0.0;
        if (internal::first_touch_position(current_bar_, fill_price, &fill_pos)) {
            const bool high_first = internal::bar_path_uses_high_first(current_bar_);
            const double high_pos = high_first ? 1.0 : 2.0;
            const double low_pos  = high_first ? 2.0 : 1.0;
            const bool same_bar = (pe.entry_bar_index == context.interval_index);
            if (high_pos < fill_pos && !(same_bar && pe.skip_entry_bar_high)) {
                double hi_fav = (was_long ? (current_bar_.high - pe.price)
                                          : (pe.price - current_bar_.high)) * close_qty;
                runup = std::max(runup, hi_fav);
                drawdown = std::max(drawdown, -hi_fav);
            }
            if (low_pos < fill_pos && !(same_bar && pe.skip_entry_bar_low)) {
                double lo_fav = (was_long ? (current_bar_.low - pe.price)
                                          : (pe.price - current_bar_.low)) * close_qty;
                runup = std::max(runup, lo_fav);
                drawdown = std::max(drawdown, -lo_fav);
            }
        }
    }
    }
    // TV reports excursions on the NET OPEN-PROFIT basis: the entry-leg
    // commission is deducted from the favorable/adverse extremes (verified
    // numerically on pyramid-cash-fractional-commission-01 — TV's exported
    // excursions differ from the gross price excursion by exactly
    // qty * cash_per_contract on every trade, both columns). Favorable is
    // floored at 0 (TV never exports a negative favorable excursion —
    // confirmed across all 757k corpus rows); adverse grows by the entry
    // commission (open profit at the entry tick is already -commission).
    // Both fields remain >= 0 here (Pine positive-drawdown convention).
    // runup/drawdown are quote-currency (price-diff × qty); convert to
    // account currency via account_currency_fx_ (default 1.0, no-op) before
    // combining with entry_commission, which is already account-currency
    // (see calc_commission) — same convention as pnl above.
    trade.max_runup = std::max(
        0.0, runup * pv * active_account_currency_fx() - entry_commission);
    // Excursion columns are nonnegative magnitudes even when an observed
    // entry rebate is negative. Keep positive-fee behavior unchanged while
    // preventing a rebate from producing an impossible negative drawdown.
    trade.max_drawdown = std::max(
        0.0, drawdown * pv * active_account_currency_fx()
                 + entry_commission);
    return trade;
}




void BacktestEngine::record_close_trade(Trade trade) {
    validate_close_trade_counters(&trade, 1);
    const double trade_pnl = trade.pnl;
    trades_.push_back(std::move(trade));
    if (stream_observe_actions_) stream_observe_exit(trades_.size() - 1);
    const double previous_net_profit = net_profit_sum_;
    net_profit_sum_ += trade_pnl;
    // Knuth TwoSum recovers this addition's exact binary64 residual without
    // changing the authoritative cached sum. Accumulate absolute residuals
    // upward so vanished intermediate profits cannot disappear from the
    // uncertainty used by a later rounded-money margin comparison.
    if (previous_net_profit == net_profit_roundoff_value_
        && std::isfinite(previous_net_profit) && std::isfinite(trade_pnl)
        && std::isfinite(net_profit_sum_)
        && std::isfinite(net_profit_roundoff_bound_)) {
        const double virtual_pnl = net_profit_sum_ - previous_net_profit;
        const double residual =
            (previous_net_profit - (net_profit_sum_ - virtual_pnl))
            + (trade_pnl - virtual_pnl);
        if (std::isfinite(residual)) {
            if (residual != 0.0) {
                net_profit_roundoff_bound_ = std::nextafter(
                    net_profit_roundoff_bound_ + std::abs(residual),
                    std::numeric_limits<double>::infinity());
            }
        } else {
            net_profit_roundoff_bound_ = std::numeric_limits<double>::infinity();
        }
    } else {
        net_profit_roundoff_bound_ = std::numeric_limits<double>::infinity();
    }
    net_profit_roundoff_value_ = net_profit_sum_;
    if (trade_pnl > 0) { gross_profit_sum_ += trade_pnl; win_trades_count_++; }
    else if (trade_pnl < 0) { gross_loss_sum_ += trade_pnl; loss_trades_count_++; }
    else { ++eventrades_count_; }  // strategy.eventrades: exact zero P&L (TV uses == 0)
}

void BacktestEngine::validate_close_trade_counters(const Trade* rows, size_t count) const {
    const int maximum = std::numeric_limits<int>::max();
    // In an ordinary run each counter can advance at most once per row.
    // Only an exhausted/near-exhausted counter needs the exact financial walk.
    if (count <= static_cast<size_t>(maximum)) {
        const int room = maximum - static_cast<int>(count);
        if (win_trades_count_ <= room && loss_trades_count_ <= room
            && eventrades_count_ <= room)
            return;
    }
    int64_t wins = win_trades_count_, losses = loss_trades_count_;
    int64_t evens = eventrades_count_;
    for (size_t i = 0; i < count; ++i) {
        const double pnl = rows[i].pnl;
        if (pnl > 0.0) {
            ++wins;
        } else if (pnl < 0.0) {
            ++losses;
        } else {
            ++evens;
        }
        if (wins > maximum || losses > maximum || evens > maximum)
            throw std::overflow_error("closed trade counter exhausted");
    }
}


// Reset all per-position state after the position is fully closed. Used by
// every full-close path (execute_market_exit) and by partial-exit settlement
// when the FIFO loop drained the position.
void BacktestEngine::reset_position_state_to_flat() {
    position_side_ = PositionSide::FLAT;
    position_cycle_seq_ = 0;
    position_entry_price_ = 0.0;
    opening_obligations_.invalidate();
    position_entry_time_ = 0;
    position_qty_ = 0.0;
    position_entry_count_ = 0;
    position_open_bar_ = -1;
    trail_best_price_ = std::numeric_limits<double>::quiet_NaN();
    pyramid_entries_.clear();
}



// After a partial exit potentially empties pyramid_entries_, either reset
// position state to FLAT (no entries left or qty effectively zero) or
// recompute volume-weighted average entry price across surviving entries.
// Body was previously inlined identically at the end of every partial-exit
// path.
void BacktestEngine::settle_position_after_partial_exit(
        PositionReductionCause cause) {
    if (position_qty_ <= kQtyEpsilon || pyramid_entries_.empty()) {
        reset_position_state_to_flat();
    } else {
        double total_qty = 0, weighted_sum = 0;
        for (auto& pe : pyramid_entries_) {
            weighted_sum += pe.price * pe.qty;
            total_qty += pe.qty;
        }
        position_entry_price_ = weighted_sum / total_qty;
        // TV returns a pyramid slot when the entry is retired by a close-path
        // order — the grid-bot family depends on it (3commas-ena: 1021 fills
        // over 64 reused ids, 776 entries between flats under a cap of 200,
        // never more than 50 CONCURRENT entries). TV does NOT return the slot
        // when a strategy.exit bracket drains another logical slot by FIFO
        // (thulashimohanr 2026-03-29: the 03-26 entry was fully retired by two
        // T1 fills and TV still refused the third entry). The narrowly proven
        // unique-owner retirement in apply_exit_order_fill can release a slot
        // after this conservative settlement; a prior foreign-bracket slice
        // or ambiguous same-ID ownership remains pinned.
        if (cause == PositionReductionCause::BRACKET_EXIT) {
            position_entry_count_ =
                std::max(position_entry_count_, (int)pyramid_entries_.size());
        } else {
            position_entry_count_ = (int)pyramid_entries_.size();
        }

    }
}


// Exposure transitions resolve activation once. Matchers never refresh a
// deadline from whichever position happens to be current at read time.







// Establish a fresh position at fill_price/qty after a transition from FLAT
// or a same-bar close. Resets all per-position state and seeds the first
// pyramid entry. Used by every entry path that opens a brand-new position
// (FLAT entry, close-only-opposite remainder, opposite flip).




void BacktestEngine::open_quoted_position(PositionSide requested, PyramidEntry lot) {
    if (next_position_cycle_seq_ <= 0
        || next_position_cycle_seq_ == std::numeric_limits<int64_t>::max())
        throw std::overflow_error("position cycle sequence exhausted");
    position_side_ = requested;
    position_cycle_seq_ = next_position_cycle_seq_++;
    position_entry_price_ = lot.price;
    // The shared post-dispatch lifecycle hook queues the new fill's event.
    // Clear prior-cycle provenance now so reversals cannot expose it even
    // transiently.
    opening_obligations_.invalidate();
    position_entry_time_ = lot.time;
    position_qty_ = lot.qty;
    position_entry_count_ = 1;
    position_open_bar_ = lot.entry_bar_index;
    trail_best_price_ = lot.price;
    pyramid_entries_.clear();
    pyramid_entries_.push_back(std::move(lot));
    if (stream_observe_actions_) stream_observe_entry(pyramid_entries_.back());
}

// The execute_market_entry case helpers are gone: growth on a deferred flip
// survives only as a frozen pending-row field name, the opening gate is
// NativeMarginModel::initial_long / initial_short, and the order of two fills
// at one point is the consumer's acceptance and incarnation ordinals
// (ADR-0001, detached comment residue).

}  // namespace pineforge
