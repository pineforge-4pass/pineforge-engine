/*
 * engine_orders.cpp — execute_market_* and partial-exit fill mechanics
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pineforge {
using namespace internal;


// Risk management + per-trade extreme tracking moved to engine_risk.cpp.

double BacktestEngine::calc_qty_for_type(double fill_price, double qty_value, int qty_type) const {
    if (std::isnan(qty_value)) {
        return calc_qty(fill_price);
    }
    if (qty_type < 0 || qty_type == static_cast<int>(QtyType::FIXED)) {
        return qty_value;
    }
    if (qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)) {
        double equity = current_equity() + open_profit(current_bar_.close);
        double cash = equity * (qty_value / 100.0);
        // Reject (qty 0) on a non-finite / non-positive fill price — a degenerate
        // $0/NaN print must NOT size as the raw % number (silent wrong-qty bug).
        // One contract's currency exposure is fill_price × pointvalue (1.0 for
        // crypto/equity — legacy math unchanged; futures divide the budget by
        // the full per-contract notional).
        return (std::isfinite(fill_price) && fill_price > 0.0)
            ? (cash / (fill_price * syminfo_.pointvalue)) : 0.0;
    }
    if (qty_type == static_cast<int>(QtyType::CASH)) {
        return (std::isfinite(fill_price) && fill_price > 0.0)
            ? (qty_value / (fill_price * syminfo_.pointvalue)) : 0.0;
    }
    return qty_value;
}


// Internal helper: execute a market entry (handles reverse-and-open).
//
// Dispatches to one of four case-helpers based on the current position
// state and the entry's close_only_opposite flag:
//   1. enter_market_from_flat       — position FLAT
//   2. add_to_pyramid_market        — position is same direction as requested
//   3. close_opposite_then_enter    — close-only-opposite branch
//   4. flip_market_position_to      — opposite direction (close-and-flip)
void BacktestEngine::execute_market_entry(const std::string& id, bool is_long, double fill_price,
                                          double explicit_qty, int explicit_qty_type,
                                          PositionSide created_position_side,
                                          bool close_only_opposite,
                                          bool is_priced_entry,
                                          double tv_carry_qty,
                                          int created_bar) {
    // Degenerate-bar guard: never open a position at a non-finite fill price
    // (e.g. a NaN/Inf print). Dropping the fill keeps trade output finite and
    // a single bad tick from poisoning the backtest. Clean feeds never hit this.
    if (!std::isfinite(fill_price)) return;
    PositionSide requested = is_long ? PositionSide::LONG : PositionSide::SHORT;
    bool is_opposite_entry = position_side_ != PositionSide::FLAT && position_side_ != requested;
    bool direction_blocked =
        (risk_direction_ == RiskDirection::LONG_ONLY && !is_long)
        || (risk_direction_ == RiskDirection::SHORT_ONLY && is_long);

    if (is_opposite_entry && direction_blocked) {
        double exit_fill = apply_slippage(fill_price, position_side_ == PositionSide::SHORT);
        execute_market_exit(exit_fill);
        purge_exit_orders();
        return;
    }

    // Check risk rules before allowing entry
    if (!check_risk_allow_entry(is_long)) return;

    // Apply slippage: buy fills higher, sell fills lower
    fill_price = apply_slippage(fill_price, is_long);

    if (position_side_ == PositionSide::FLAT) {
        enter_market_from_flat(id, is_long, fill_price, explicit_qty, explicit_qty_type,
                               created_position_side, is_priced_entry, tv_carry_qty,
                               created_bar);
        return;
    }

    if (position_side_ == requested) {
        add_to_pyramid_market(id, is_long, fill_price, explicit_qty, explicit_qty_type,
                              created_position_side, is_priced_entry);
        return;
    }

    if (created_position_side == PositionSide::FLAT && close_only_opposite) {
        close_opposite_then_enter(id, is_long, fill_price, explicit_qty, explicit_qty_type);
        return;
    }

    flip_market_position_to(id, is_long, fill_price, explicit_qty, explicit_qty_type);
}


// Internal helper: execute a market exit (close position at fill price)
void BacktestEngine::execute_market_exit(double fill_price) {
    if (position_side_ == PositionSide::FLAT) {
        return;
    }

    // Apply slippage: closing long = sell (lower), closing short = buy (higher)
    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_slippage(fill_price, is_buy);
    bool was_long = (position_side_ == PositionSide::LONG);

    // Emit one Trade per pyramid entry (matches TradingView reporting)
    for (auto& pe : pyramid_entries_) {
        emit_close_trade(pe, pe.qty, fill_price, was_long);
    }

    reset_position_state_to_flat();
}


// FIFO-drain up to qty_limit from pyramid_entries_, optionally restricted to a
// single from_entry id. See engine.hpp for the contract. Mirrors TradingView's
// per-pyramid trade reporting: one Trade per drained slice. Returns total qty
// drained so callers can assert / log if needed.
double BacktestEngine::fifo_drain(const std::string* from_entry, double qty_limit,
                                  double fill_price, bool was_long) {
    double qty_closed = 0.0;
    std::vector<PyramidEntry> remaining;
    for (auto& pe : pyramid_entries_) {
        bool eligible = (from_entry == nullptr) || (pe.entry_id == *from_entry);
        if (!eligible || qty_closed >= qty_limit - kQtyEpsilon) {
            remaining.push_back(pe);
            continue;
        }
        double close_qty = std::min(pe.qty, qty_limit - qty_closed);
        double keep_qty = pe.qty - close_qty;
        qty_closed += close_qty;

        emit_close_trade(pe, close_qty, fill_price, was_long);

        if (keep_qty > kQtyEpsilon) {
            // Scale the accumulated USD excursion to the kept slice so the
            // remaining entry's extremes stay consistent with its reduced
            // qty (update_per_trade_extremes accumulates (diff) * pe.qty).
            double keep_scale = keep_qty / pe.qty;
            remaining.push_back({pe.price, pe.time, keep_qty, pe.entry_id,
                                 pe.entry_bar_index, pe.entry_comment,
                                 pe.max_runup * keep_scale,
                                 pe.max_drawdown * keep_scale,
                                 pe.skip_entry_bar_high,
                                 pe.skip_entry_bar_low});
        }
    }

    pyramid_entries_ = std::move(remaining);
    position_qty_ -= qty_closed;
    return qty_closed;
}


// Internal helper: execute a partial exit (reduce position by qty, create trade records)
// TradingView creates individual trade records for each partial exit.
void BacktestEngine::execute_partial_exit_qty(double fill_price, double qty_to_close) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;
    qty_to_close = std::clamp(qty_to_close, 0.0, position_qty_);
    if (qty_to_close <= kQtyEpsilon) return;

    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_slippage(fill_price, is_buy);
    bool was_long = (position_side_ == PositionSide::LONG);

    // Close FIFO across all pyramid entries, creating trade records.
    fifo_drain(/*from_entry=*/nullptr, qty_to_close, fill_price, was_long);
    settle_position_after_partial_exit();
}


void BacktestEngine::execute_partial_exit(double fill_price, double qty_percent) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;

    double pct = std::clamp(qty_percent, 0.0, 100.0);
    execute_partial_exit_qty(fill_price, position_qty_ * (pct / 100.0));
}


// Internal helper: close only entries matching from_entry (close_entries_rule="ANY")
void BacktestEngine::execute_partial_exit_by_entry(double fill_price, const std::string& from_entry) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;

    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_slippage(fill_price, is_buy);
    bool was_long = (position_side_ == PositionSide::LONG);

    std::vector<PyramidEntry> remaining;
    for (auto& pe : pyramid_entries_) {
        if (pe.entry_id == from_entry) {
            emit_close_trade(pe, pe.qty, fill_price, was_long);
            position_qty_ -= pe.qty;
        } else {
            remaining.push_back(pe);
        }
    }

    pyramid_entries_ = std::move(remaining);
    settle_position_after_partial_exit();
}


// Internal helper: partially close entries matching from_entry by qty_percent.
void BacktestEngine::execute_partial_exit_by_entry_percent(double fill_price,
                                                           const std::string& from_entry,
                                                           double qty_percent) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;

    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_slippage(fill_price, is_buy);
    bool was_long = (position_side_ == PositionSide::LONG);

    double matched_qty = 0.0;
    for (const auto& pe : pyramid_entries_) {
        if (pe.entry_id == from_entry) matched_qty += pe.qty;
    }
    if (matched_qty <= kQtyEpsilon) return;

    double pct = std::clamp(qty_percent, 0.0, 100.0);
    double qty_to_close = matched_qty * (pct / 100.0);
    if (qty_to_close <= kQtyEpsilon) return;

    // Close FIFO, but only from entries matching from_entry.
    fifo_drain(&from_entry, qty_to_close, fill_price, was_long);
    settle_position_after_partial_exit();
}


// Internal helper: cancel OCA group members (except the one that just filled)
void BacktestEngine::cancel_oca_group(const std::string& oca_name, const std::string& exclude_id) {
    if (oca_name.empty()) return;
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) {
                return o.oca_name == oca_name && o.id != exclude_id;
            }),
        pending_orders_.end());
}

// Pine v6 strategy.oca.reduce: when one sibling fills qty Q, every other
// sibling's remaining qty is reduced by Q. Siblings whose remaining qty
// reaches <= 0 are cancelled outright (matches TradingView behaviour and
// degenerates to oca.cancel when the filling order's qty >= sibling qty).
// Siblings using default sizing (qty == NaN) cannot have a meaningful
// per-order qty applied at place time, so we conservatively cancel them
// (this matches the prior, blanket-cancel behaviour for that subset).
void BacktestEngine::reduce_oca_group(const std::string& oca_name,
                                      const std::string& exclude_id,
                                      double filled_qty) {
    if (oca_name.empty()) return;
    if (!(filled_qty > 0.0)) return;  // nothing to subtract
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](PendingOrder& o) {
                if (o.oca_name != oca_name || o.id == exclude_id) return false;
                if (std::isnan(o.qty)) return true;  // default-sized: cancel
                o.qty -= filled_qty;
                return o.qty <= kOcaQtyEpsilon;
            }),
        pending_orders_.end());
}


void BacktestEngine::purge_exit_orders() {
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [](const PendingOrder& o) { return o.type == OrderType::EXIT; }),
        pending_orders_.end());
}


// ────────────────────────────────────────────────────────────────────
// Shared close-side / position-state helpers
// ────────────────────────────────────────────────────────────────────

// Emit one Trade record for closing close_qty of pyramid entry pe at
// fill_price (already slippage-adjusted). Updates trades_, profit/loss
// aggregates, intraday PnL, and consecutive-loss-day tracking exactly the
// same way every closing path does — the body was previously inlined in
// execute_market_exit, execute_partial_exit_qty, execute_partial_exit_by_entry,
// execute_partial_exit_by_entry_percent, and the flip branch of
// execute_market_entry. Mirrors TradingView's per-pyramid trade reporting.
void BacktestEngine::emit_close_trade(const PyramidEntry& pe, double close_qty,
                                      double fill_price, bool was_long) {
    // Realized PnL scales by the instrument point value ($ per point per
    // contract). Crypto/equity (pointvalue=1) is unchanged; futures (e.g. ES=50)
    // multiply the price-difference PnL. pnl_pct is a price-return % and is
    // point-value-invariant. Commission is in account currency: cash-per-* is
    // already absolute, and percent commission scales the notional by
    // pointvalue inside calc_commission.
    const double pv = syminfo_.pointvalue;
    double pnl = (was_long ? (fill_price - pe.price) : (pe.price - fill_price))
                 * close_qty * pv;
    double pnl_pct = was_long
        ? (fill_price / pe.price - 1.0) * 100.0
        : (pe.price / fill_price - 1.0) * 100.0;
    const double entry_commission = calc_commission(pe.price, close_qty);
    const double exit_commission  = calc_commission(fill_price, close_qty);
    pnl -= entry_commission + exit_commission;

    Trade trade;
    trade.entry_time = pe.time;
    trade.exit_time = current_bar_.timestamp;
    trade.entry_price = pe.price;
    trade.exit_price = fill_price;
    trade.qty = close_qty;
    trade.pnl = pnl;
    trade.pnl_pct = pnl_pct;
    trade.is_long = was_long;
    trade.entry_bar_index = pe.entry_bar_index;
    trade.exit_bar_index = bar_index_;
    trade.entry_id = pe.entry_id;
    trade.entry_comment = pe.entry_comment;
    trade.commission = entry_commission + exit_commission;
    // Excursions: TV's per-trade excursion includes the exit fill itself —
    // a stop-out's adverse excursion is at least the loss at the SL fill and
    // a take-profit's favorable excursion includes the move to the TP fill.
    // The per-bar sampler (update_per_trade_extremes) cannot see this: exit
    // fills happen inside process_pending_orders and the pyramid entry is
    // removed before the next sample, so same-bar entry+exit trades would
    // otherwise report 0/0. Fold the fill price in here. The carried
    // per-entry extreme is scaled to the closed slice (close_qty/pe.qty) so
    // a partial close reports the slice's USD excursion, matching TV's
    // per-trade-record qty. Both fields stay >= 0 (Pine accessor convention);
    // the TV-export sign flip happens only in the CSV writer.
    double slice = (pe.qty > kQtyEpsilon) ? (close_qty / pe.qty) : 1.0;
    double fill_fav = (was_long ? (fill_price - pe.price) : (pe.price - fill_price))
                      * close_qty;
    double runup = std::max(pe.max_runup * slice, fill_fav);
    double drawdown = std::max(pe.max_drawdown * slice, -fill_fav);
    // Priced (stop/limit/trail) exits fill mid-bar: the bar-path extremes the
    // assumed OHLC path reaches BEFORE the exit fill belong to this trade's
    // excursion, but per-bar sampling never sees them (the entry is removed
    // before the next update_per_trade_extremes). Fold them in here, honoring
    // the entry-side masks when the trade opened on this same bar (an extreme
    // that precedes the ENTRY fill is not part of the trade either).
    // TRAIL fills: the peak that armed the trail (fill +/- offset) is a
    // pre-fill favorable excursion no bar-boundary sample sees (TV reports
    // MFE == peak for trail exits).
    if (!std::isnan(fold_exit_trail_peak_)) {
        double peak_fav = (was_long ? (fold_exit_trail_peak_ - pe.price)
                                    : (pe.price - fold_exit_trail_peak_))
                          * close_qty;
        runup = std::max(runup, peak_fav);
    }
    if (fold_exit_path_extremes_) {
        double fill_pos = 0.0;
        if (internal::first_touch_position(current_bar_, fill_price, &fill_pos)) {
            const bool high_first = internal::bar_path_uses_high_first(current_bar_);
            const double high_pos = high_first ? 1.0 : 2.0;
            const double low_pos  = high_first ? 2.0 : 1.0;
            const bool same_bar = (pe.entry_bar_index == bar_index_);
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
    // TV reports excursions on the NET OPEN-PROFIT basis: the entry-leg
    // commission is deducted from the favorable/adverse extremes (verified
    // numerically on pyramid-cash-fractional-commission-01 — TV's exported
    // excursions differ from the gross price excursion by exactly
    // qty * cash_per_contract on every trade, both columns). Favorable is
    // floored at 0 (TV never exports a negative favorable excursion —
    // confirmed across all 757k corpus rows); adverse grows by the entry
    // commission (open profit at the entry tick is already -commission).
    // Both fields remain >= 0 here (Pine positive-drawdown convention).
    trade.max_runup = std::max(0.0, runup * pv - entry_commission);
    trade.max_drawdown = drawdown * pv + entry_commission;
    const double trade_pnl = trade.pnl;
    trades_.push_back(std::move(trade));
    net_profit_sum_ += trade_pnl;
    if (trade_pnl > 0) { gross_profit_sum_ += trade_pnl; win_trades_count_++; }
    else if (trade_pnl < 0) { gross_loss_sum_ += trade_pnl; loss_trades_count_++; }
    else { ++eventrades_count_; }  // strategy.eventrades: exact zero P&L (TV uses == 0)

    // Update risk state: intraday PnL and consecutive loss day tracking
    intraday_pnl_ += pnl;
    if (pnl < 0.0) {
        BarTime bt = _decompose_bar_time_chart_tz();
        int cur_day = bt.dayofmonth * 100 + bt.month;
        if (cur_day != last_loss_day_) {
            last_loss_day_ = cur_day;
            cons_loss_day_count_++;
        }
    } else if (pnl > 0.0) {
        cons_loss_day_count_ = 0;
    }
}


// Reset all per-position state after the position is fully closed. Used by
// every full-close path (execute_market_exit) and by partial-exit settlement
// when the FIFO loop drained the position.
void BacktestEngine::reset_position_state_to_flat() {
    position_side_ = PositionSide::FLAT;
    position_entry_price_ = 0.0;
    position_entry_time_ = 0;
    position_qty_ = 0.0;
    position_entry_count_ = 0;
    position_open_bar_ = -1;
    trail_best_price_ = std::numeric_limits<double>::quiet_NaN();
    pyramid_entries_.clear();
    consumed_partial_exit_ids_.clear();
}


// After a partial exit potentially empties pyramid_entries_, either reset
// position state to FLAT (no entries left or qty effectively zero) or
// recompute volume-weighted average entry price across surviving entries.
// Body was previously inlined identically at the end of every partial-exit
// path.
void BacktestEngine::settle_position_after_partial_exit() {
    if (position_qty_ <= kQtyEpsilon || pyramid_entries_.empty()) {
        reset_position_state_to_flat();
    } else {
        double total_qty = 0, weighted_sum = 0;
        for (auto& pe : pyramid_entries_) {
            weighted_sum += pe.price * pe.qty;
            total_qty += pe.qty;
        }
        position_entry_price_ = weighted_sum / total_qty;
        position_entry_count_ = (int)pyramid_entries_.size();
    }
}


// Establish a fresh position at fill_price/qty after a transition from FLAT
// or a same-bar close. Resets all per-position state and seeds the first
// pyramid entry. Used by every entry path that opens a brand-new position
// (FLAT entry, close-only-opposite remainder, opposite flip).
void BacktestEngine::open_fresh_position(PositionSide requested, double fill_price,
                                         double qty, const std::string& id) {
    position_side_ = requested;
    position_entry_price_ = fill_price;
    position_entry_time_ = current_bar_.timestamp;
    position_qty_ = qty;
    position_entry_count_ = 1;
    position_open_bar_ = bar_index_;
    trail_best_price_ = fill_price;
    pyramid_entries_.clear();
    consumed_partial_exit_ids_.clear();
    pyramid_entries_.push_back({fill_price, current_bar_.timestamp, qty, id, bar_index_});
}


// ────────────────────────────────────────────────────────────────────
// execute_market_entry case helpers
// ────────────────────────────────────────────────────────────────────

// Drop the TV deferred-flip carry on every other pending priced entry that
// was placed during the same source position cycle. TV consumes the carry
// from the now-closed source position exactly once, so siblings (matching
// ``created_position_side``) must fire later with their own explicit qty
// rather than re-applying the same carry growth.
//
// Probe 93 (pyramiding=2, two opposite-direction stops armed during a long
// cycle): the first sibling grows by |old|+qty=2, the second sibling fires
// fresh at qty=1. Without this, both siblings would grow and the engine
// emits an extra-qty pyramid add (or, after the cleanup-loop wipes the
// survivor, an entire extra round-trip the next day).
//
// Cycle scoping: TV consumes carry only when the triggering sibling fires
// from FLAT. That means a sibling armed in a LATER position cycle (whose
// own ``tv_carry_qty`` was captured from a different source position
// later than this firing entry's ``created_bar``) must keep its carry
// independent — it will apply its own carry when it later fires from
// flat. Without this scoping, consuming a future-cycle sibling's carry
// drops one cycle's worth of qty (validation/52, 63, 72, 92, 93, 95, 96
// pre-fix: row count + qty match TV exactly, but per-leg PnL drifts
// because the chain qty schedule shifts by one cycle when a sibling is
// pre-emptively wiped by an earlier-cycle fire).
void BacktestEngine::consume_tv_carry_from_siblings(const std::string& id,
                                                    PositionSide created_position_side,
                                                    int created_bar) {
    for (auto& other : pending_orders_) {
        if (other.id == id) continue;
        if (other.created_position_side != created_position_side) continue;
        if (other.tv_carry_qty <= 0.0) continue;
        // Cycle-scope: only consume siblings placed no later than the
        // firing order's own placement. Siblings placed in a LATER bar
        // captured carry from a DIFFERENT source position cycle and own
        // their carry — TV does not pre-emptively wipe them.
        if (other.created_bar > created_bar) continue;
        other.tv_carry_qty = 0.0;
    }
}


// Open a new position from FLAT.
//
// TradingView's deferred-flip growth rule (probes 52, 63, 72, 92): a priced
// (stop/limit) entry that was placed while the strategy held an
// OPPOSITE-direction position carries that position's qty forward. If the
// original position is later closed (by strategy.close, close_all, or any
// exit) and the priced entry now fires from FLAT, the new position opens at
// ``qty + tv_carry_qty`` rather than just ``qty`` — as if it had been a
// true flip of the original (now-closed) position.
//
// The carry persists across bars: in probe 92 the daily cleanup
// (``strategy.close_all``) closes the long at chart 12:15 and the SE stop
// fires hours later at 21:30, still applying the carry. So this helper
// reads ``tv_carry_qty`` from the pending order itself (snapshotted at
// placement, see PendingOrder struct in engine.hpp) rather than a per-bar
// transient state.
//
// Conditions:
//   (a) script_has_strategy_close_   -- compile-time AST scan (gates the
//                                       rule to strategies that can
//                                       actually trigger the deferred-flip
//                                       pattern)
//   (b) is_priced_entry              -- stop/limit, not market
//   (c) tv_carry_qty > 0             -- order was placed while a position
//                                       was open
//   (d) requested != created direction
//                                    -- new entry is opposite to the carry
//                                       position
//
// After applying the carry, ``consume_tv_carry_from_siblings`` zeroes the
// same-source-cycle siblings so probe 93 doesn't double-grow.
//
// Final guard: TradingView margin check. required_margin = qty * fill_price
// * margin_pct / 100. If required_margin > available equity, TV silently
// rejects the fill (the order simply does not appear in the trade list).
// With default margin_long_/margin_short_ = 100 (1x leverage) this is just
// "position value <= equity". Reproduces the IES/VCP/ies-probe-08 entry-skip
// behaviour where dynamic-qty strategies over-leverage on low-ATR bars and
// TV silently drops the entry while the engine fires it (matched-trade qty
// ratio in probe 08 was empirically equal to engine_equity / TV_equity,
// proving the math is right but the gate was missing).
void BacktestEngine::enter_market_from_flat(const std::string& id, bool is_long,
                                            double fill_price, double explicit_qty,
                                            int explicit_qty_type,
                                            PositionSide created_position_side,
                                            bool is_priced_entry, double tv_carry_qty,
                                            int created_bar) {
    bool carry_was_long = (created_position_side == PositionSide::LONG);
    bool tv_deferred_flip =
        script_has_strategy_close_
        && is_priced_entry
        && tv_carry_qty > 0.0
        && (carry_was_long ? !is_long : is_long);
    double base_qty = calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    double qty = tv_deferred_flip ? (tv_carry_qty + base_qty) : base_qty;
    if (tv_deferred_flip) {
        consume_tv_carry_from_siblings(id, created_position_side, created_bar);
    }
    // NOTE: margin check is performed at SIGNAL time inside
    // strategy_entry / queue_deferred_close_order, NOT here at fill
    // time. This matches TV's broker emulator, which rejects entries
    // whose qty * SIGNAL_BAR_CLOSE exceeds equity. By the time we
    // reach this fill-side helper the order has already been admitted
    // (or rejected) at signal time, and the next-bar slippage between
    // signal close and fill open should NOT be allowed to flip a TV-
    // accepted entry into a reject. See parity-probe-{03..06} +
    // ies-probe-08 for the empirical justification.
    PositionSide requested = is_long ? PositionSide::LONG : PositionSide::SHORT;
    open_fresh_position(requested, fill_price, qty, id);
}


// Add to an existing same-direction position (pyramiding).
//
// Pyramiding limit applies at fill time, EXCEPT for priced (stop/limit)
// entries that were placed while the position was either FLAT or holding
// the OPPOSITE direction. Such entries were "armed" pre-position (probe
// 80's morning short stop firing on top of the afternoon's short entry
// confirms the flat-armed case) or placed as a flip-prep stop during a
// previous opposite-direction cycle that has since closed (probe 72's S2
// placed while LONG'ing via L2 and TV emits the second-sibling short trade
// despite pyramiding=1). Market entries — and entries placed while already
// in the SAME direction — still respect the limit (probe 54's two same-bar
// same-direction market entries with pyramiding=1 must keep only the first
// one).
void BacktestEngine::add_to_pyramid_market(const std::string& id, bool is_long,
                                           double fill_price, double explicit_qty,
                                           int explicit_qty_type,
                                           PositionSide created_position_side,
                                           bool is_priced_entry) {
    PositionSide requested = is_long ? PositionSide::LONG : PositionSide::SHORT;
    bool flat_armed_priced =
        is_priced_entry && created_position_side == PositionSide::FLAT;
    bool pre_armed_opposite_priced =
        is_priced_entry
        && created_position_side != PositionSide::FLAT
        && created_position_side != requested;
    if (!flat_armed_priced && !pre_armed_opposite_priced
        && position_entry_count_ >= pyramiding_) {
        return;
    }
    double new_qty = calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    double total_qty = position_qty_ + new_qty;
    position_entry_price_ = (position_entry_price_ * position_qty_ + fill_price * new_qty) / total_qty;
    position_qty_ = total_qty;
    position_entry_count_++;
    trail_best_price_ = fill_price;
    pyramid_entries_.push_back({fill_price, current_bar_.timestamp, new_qty, id, bar_index_});
}


// close_only_opposite branch: TV semantic for opposite-direction entries
// where the strategy.entry call was placed with ``close_only_opposite=true``
// — close part of the existing opposite position by tx_qty, then open the
// requested-direction remainder if any.
void BacktestEngine::close_opposite_then_enter(const std::string& id, bool is_long,
                                               double fill_price, double explicit_qty,
                                               int explicit_qty_type) {
    double tx_qty = calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    double close_qty = std::min(tx_qty, position_qty_);
    // execute_partial_exit_qty applies slippage internally (mirrors its other
    // callers, e.g. execute_partial_exit_by_percent). Pass the RAW fill_price —
    // pre-slipping here would double-slip the close leg (issue #27).
    execute_partial_exit_qty(fill_price, close_qty);
    purge_exit_orders();
    double remainder = tx_qty - close_qty;
    if (remainder <= kQtyEpsilon) {
        return;
    }
    fill_price = apply_slippage(fill_price, is_long);
    PositionSide requested = is_long ? PositionSide::LONG : PositionSide::SHORT;
    open_fresh_position(requested, fill_price, remainder, id);
}


// Opposite direction: close current position trade-by-pyramid then open new
// position in requested direction at the entry-slipped fill_price.
//
// Standard Pine semantic: an in-position flip (``strategy.entry`` while
// holding the opposite side) closes the existing position and opens a fresh
// position with ``qty`` from ``strategy.entry``'s ``qty`` parameter — not
// ``|old| + qty``. Verified empirically with probe 92: TV produces 328
// qty=1 in-position flips (SE stop firing while still long) and only 20
// qty=2 flips that all happen AFTER a same-day cleanup closed the long.
// The qty=2 cases are handled by the paired-close growth rule in
// ``enter_market_from_flat``; this branch keeps the standard
// ``new_size = qty`` contract.
//
// We deliberately do NOT purge exit orders here. Mutating pending_orders_
// mid-iteration of process_pending_orders shifts indices and corrupts the
// filled_indices accounting. Stale exits targeting the old entry id get
// cleaned up on the next bar by the "from_entry doesn't match any pyramid
// entry" check in process_pending_orders. Newly-placed exits that target
// the incoming entry id stay and evaluate correctly on the current bar's
// remaining iterations.
void BacktestEngine::flip_market_position_to(const std::string& id, bool is_long,
                                             double fill_price, double explicit_qty,
                                             int explicit_qty_type) {
    // For the close we need exit slippage based on closing direction.
    // Closing a long = sell (price - slip); closing a short = buy (price + slip).
    // fill_price already has entry slippage applied; un-slip it before
    // re-applying with the exit direction.
    double raw_price = fill_price;
    if (slippage_ != 0) {
        double slip = slippage_ * syminfo_mintick_;
        raw_price = is_long ? (fill_price - slip) : (fill_price + slip);
    }
    double exit_fill = apply_slippage(raw_price, position_side_ == PositionSide::SHORT);
    bool was_long = (position_side_ == PositionSide::LONG);

    // Emit one Trade per pyramid entry (matches TradingView reporting)
    for (auto& pe : pyramid_entries_) {
        emit_close_trade(pe, pe.qty, exit_fill, was_long);
    }

    double new_qty = calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    PositionSide requested = is_long ? PositionSide::LONG : PositionSide::SHORT;
    open_fresh_position(requested, fill_price, new_qty, id);
}


}  // namespace pineforge
