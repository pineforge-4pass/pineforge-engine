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
    // qty_step_ lot-size flooring applies uniformly regardless of how the
    // caller's qty was derived — including this FIXED branch, which is the
    // common ``strategy.entry(qty=someComputedExpr)`` shape (e.g. a DCA base/
    // safety-order qty = orderSizeUsd/close). See apply_qty_step's doc
    // comment (engine.hpp) for the verified TV behavior this mirrors.
    if (qty_type < 0 || qty_type == static_cast<int>(QtyType::FIXED)) {
        return apply_qty_step(qty_value);
    }
    if (qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)) {
        double equity = current_equity() + open_profit(current_bar_.close);
        double cash = reserve_percent_commission(equity * (qty_value / 100.0));
        // Reject (qty 0) on a non-finite / non-positive fill price — a degenerate
        // $0/NaN print must NOT size as the raw % number (silent wrong-qty bug).
        // One contract's currency exposure is fill_price × pointvalue (1.0 for
        // crypto/equity — legacy math unchanged; futures divide the budget by
        // the full per-contract notional). cash is account-currency (equity is);
        // convert to quote currency via account_currency_fx_ before dividing by
        // the quote-currency fill_price — same convention as calc_qty() in
        // engine.hpp. fx=1.0 is a no-op.
        return (std::isfinite(fill_price) && fill_price > 0.0)
            ? apply_qty_step((cash / account_currency_fx_) / (fill_price * syminfo_.pointvalue)) : 0.0;
    }
    if (qty_type == static_cast<int>(QtyType::CASH)) {
        return (std::isfinite(fill_price) && fill_price > 0.0)
            ? apply_qty_step((qty_value / account_currency_fx_) / (fill_price * syminfo_.pointvalue)) : 0.0;
    }
    return apply_qty_step(qty_value);
}


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
void BacktestEngine::execute_market_entry(const std::string& id, bool is_long, double fill_price,
                                          double explicit_qty, int explicit_qty_type,
                                          PositionSide created_position_side,
                                          bool close_only_opposite,
                                          bool is_priced_entry,
                                          double tv_carry_qty,
                                          int created_bar,
                                          bool later_same_tick_entry,
                                          bool paired_flat_market_transaction) {
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
        if (!paired_flat_market_transaction) purge_exit_orders();
        return;
    }

    // Check risk rules before allowing entry
    if (!check_risk_allow_entry(is_long)) return;

    // Apply slippage: buy fills higher, sell fills lower. LIMIT-triggered
    // fills (current_fill_is_limit_) take the unslipped limit-or-better
    // path instead — TV does not slip limit fills.
    fill_price = apply_fill_slippage(fill_price, is_long);

    if (position_side_ == PositionSide::FLAT) {
        enter_market_from_flat(id, is_long, fill_price, explicit_qty, explicit_qty_type,
                               created_position_side, is_priced_entry, tv_carry_qty,
                               created_bar,
                               /*explicit_qty_prequantized=*/
                                   paired_flat_market_transaction);
        return;
    }

    if (position_side_ == requested) {
        add_to_pyramid_market(id, is_long, fill_price, explicit_qty, explicit_qty_type,
                              created_position_side, is_priced_entry);
        return;
    }

    if (created_position_side == PositionSide::FLAT && close_only_opposite) {
        close_opposite_then_enter(
            id, is_long, fill_price, explicit_qty, explicit_qty_type,
            /*purge_pending_exits=*/!paired_flat_market_transaction,
            /*explicit_qty_prequantized=*/paired_flat_market_transaction);
        return;
    }

    if (later_same_tick_entry) {
        sequential_same_tick_reversal_fill(id, is_long, fill_price, explicit_qty,
                                           explicit_qty_type);
        return;
    }

    // ``close_only_opposite`` reaches here for a created_position_side != FLAT
    // reduce-only flip (the FLAT bracket case returned above via
    // close_opposite_then_enter). This is either a deferred-flip carry that
    // reverses a later position cycle, or the equality-only same-cycle frozen
    // transaction whose whole broker movement is consumed by the close. Both
    // close the live opposite position without opening their own leg.
    flip_market_position_to(id, is_long, fill_price, explicit_qty, explicit_qty_type,
                            /*close_only=*/close_only_opposite);
}


// Internal helper: execute a market exit (close position at fill price)
void BacktestEngine::execute_market_exit(double fill_price) {
    if (position_side_ == PositionSide::FLAT) {
        return;
    }

    // Apply slippage: closing long = sell (lower), closing short = buy
    // (higher). LIMIT-triggered exits (TP brackets) take the unslipped
    // limit-or-better path via apply_fill_slippage.
    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_fill_slippage(fill_price, is_buy);
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
                                 pe.skip_entry_bar_low,
                                 pe.market_pyramid_add});
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
    fill_price = apply_fill_slippage(fill_price, is_buy);
    bool was_long = (position_side_ == PositionSide::LONG);

    // Close FIFO across all pyramid entries, creating trade records.
    fifo_drain(/*from_entry=*/nullptr, qty_to_close, fill_price, was_long);
    settle_position_after_partial_exit();
}


void BacktestEngine::execute_partial_exit(double fill_price, double qty_percent) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;

    double pct = std::clamp(qty_percent, 0.0, 100.0);
    double qty_to_close = position_qty_ * (pct / 100.0);
    // Percent-derived partial exit resolved at FILL time (an exit placed
    // while still FLAT carries no reserved qty): floor the lot to the
    // instrument qty step exactly like the placement-time path in
    // compute_exit_reserved_qty — see apply_exit_qty_step for the TV
    // dust-remainder evidence. Full exits (pct == 100%) stay exact.
    if (pct < 100.0 - kFullPercentEps) {
        qty_to_close = apply_exit_qty_step(qty_to_close);
    }
    execute_partial_exit_qty(fill_price, qty_to_close);
}


// Internal helper: close only entries matching from_entry (close_entries_rule="ANY")
void BacktestEngine::execute_partial_exit_by_entry(double fill_price, const std::string& from_entry) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;

    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_fill_slippage(fill_price, is_buy);
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
    fill_price = apply_fill_slippage(fill_price, is_buy);
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


// KI-62: after a from_entry PRICED bracket exit fills, scratch (close dur-0)
// any same-bar same-id MARKET pyramid-add slice still open — it filled earlier
// this bar, ahead of the exit in TV's open-tick fill sequence, so TV's exit
// covers it. Targets ONLY flagged same-bar (entry_bar_index == bar_index_)
// market-add slices of this from_entry: the frozen pre-add lot was already
// drained by the normal close, and prior-bar slices (entry_bar_index <
// bar_index_) are never touched — so multi-bar pyramids stay untouched. A
// strict no-op when no such slice exists (the KEEP cell fills the exit first,
// so the add is not yet open; non-collision shapes flag no add). Emits each
// covered slice as its own dur-0 trade (entry at the add's fill price, exit at
// this exit's fill price), matching TV's per-pyramid scratch reporting.
double BacktestEngine::cover_samebar_market_adds_on_exit(const PendingOrder& order,
                                                         double fill_price) {
    if (order.from_entry.empty()) return 0.0;
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return 0.0;
    // Scope to a PRICED bracket (stop/limit/trail). A plain market close /
    // close_all already flattens the whole position through its own path.
    bool priced_bracket = !std::isnan(order.stop_price)
        || !std::isnan(order.limit_price)
        || !std::isnan(order.trail_points)
        || !std::isnan(order.trail_price);
    if (!priced_bracket) return 0.0;

    bool is_buy = (position_side_ == PositionSide::SHORT);
    double slipped = apply_fill_slippage(fill_price, is_buy);
    bool was_long = (position_side_ == PositionSide::LONG);

    std::vector<PyramidEntry> remaining;
    remaining.reserve(pyramid_entries_.size());
    double closed = 0.0;
    for (auto& pe : pyramid_entries_) {
        if (pe.market_pyramid_add
            && pe.entry_bar_index == bar_index_
            && pe.entry_id == order.from_entry) {
            emit_close_trade(pe, pe.qty, slipped, was_long);
            closed += pe.qty;
        } else {
            remaining.push_back(pe);
        }
    }
    if (closed <= kQtyEpsilon) return 0.0;   // nothing covered
    pyramid_entries_ = std::move(remaining);
    position_qty_ -= closed;
    settle_position_after_partial_exit();
    return closed;
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


void BacktestEngine::purge_exit_orders(bool retain_for_pending_entries) {
    if (retain_for_pending_entries) {
        // End-of-bar flat-purge: the position is flat, but a from_entry-bound
        // EXIT bracket whose parent ENTRY is still a PENDING order (e.g. a limit
        // entry that could not fill on its creation bar) must be RETAINED — once
        // the entry fills on a later bar the bracket fires, matching TV. Only
        // brackets with no live/pending parent entry are stale and dropped.
        std::unordered_set<std::string> pending_entry_ids;
        for (const auto& o : pending_orders_) {
            if (o.type == OrderType::ENTRY || o.type == OrderType::MARKET) {
                pending_entry_ids.insert(o.id);
            }
        }
        pending_orders_.erase(
            std::remove_if(pending_orders_.begin(), pending_orders_.end(),
                [&](const PendingOrder& o) {
                    return o.type == OrderType::EXIT
                        && !(!o.from_entry.empty()
                             && pending_entry_ids.count(o.from_entry));
                }),
            pending_orders_.end());
        return;
    }
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
    // multiply the price-difference PnL. The price-difference component is in
    // the symbol's QUOTE currency; multiply by account_currency_fx_ (default
    // 1.0, no-op for the corpus) to report in the strategy's ACCOUNT currency
    // — same conversion the margin gate (engine_strategy_commands.cpp) already
    // applies. Commission is already account-currency-native (calc_commission
    // applies the same fx factor internally for its PERCENT case; cash-per-*
    // is account-currency by construction), so it is NOT scaled again here.
    const double pv = syminfo_.pointvalue;
    double pnl = (was_long ? (fill_price - pe.price) : (pe.price - fill_price))
                 * close_qty * pv * account_currency_fx_;
    const double entry_commission = calc_commission(pe.price, close_qty);
    const double exit_commission  = calc_commission(fill_price, close_qty);
    pnl -= entry_commission + exit_commission;
    // TV "Net P&L %" convention (arbitrated 2026-06-12 vs TV export,
    // trade #258 short: 102.44 USD on 2276.66 entry => 4.50%): NET pnl
    // as a percent of entry cost (entry_price * qty * pointvalue, same
    // account_currency_fx_ conversion as pnl above so the ratio is
    // currency-invariant). Long/no-commission degenerates to the old
    // (exit/entry-1) form; shorts diverge on large moves ((entry/exit-1)
    // was wrong). Computed AFTER the commission subtraction above — order
    // matters.
    const double entry_cost = pe.price * close_qty * pv * account_currency_fx_;
    double pnl_pct = (entry_cost > 0.0) ? (pnl / entry_cost) * 100.0 : 0.0;

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
    // runup/drawdown are quote-currency (price-diff × qty); convert to
    // account currency via account_currency_fx_ (default 1.0, no-op) before
    // combining with entry_commission, which is already account-currency
    // (see calc_commission) — same convention as pnl above.
    trade.max_runup = std::max(0.0, runup * pv * account_currency_fx_ - entry_commission);
    trade.max_drawdown = drawdown * pv * account_currency_fx_ + entry_commission;
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
    position_cycle_seq_ = 0;
    position_entry_price_ = 0.0;
    opening_affordability_pending_ = false;
    opening_affordability_eligible_ = false;
    opening_affordability_raw_fill_base_ =
        std::numeric_limits<double>::quiet_NaN();
    position_entry_time_ = 0;
    position_qty_ = 0.0;
    position_entry_count_ = 0;
    position_open_bar_ = -1;
    trail_best_price_ = std::numeric_limits<double>::quiet_NaN();
    pyramid_entries_.clear();
    id_unclosed_qty_.clear();
    close_reserved_qty_.clear();
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
    position_cycle_seq_ = next_position_cycle_seq_++;
    position_entry_price_ = fill_price;
    // The shared post-dispatch lifecycle hook queues the new fill's event.
    // Clear prior-cycle provenance now so reversals cannot expose it even
    // transiently.
    opening_affordability_pending_ = false;
    opening_affordability_eligible_ = false;
    opening_affordability_raw_fill_base_ =
        std::numeric_limits<double>::quiet_NaN();
    position_entry_time_ = current_bar_.timestamp;
    position_qty_ = qty;
    position_entry_count_ = 1;
    position_open_bar_ = bar_index_;
    trail_best_price_ = fill_price;
    pyramid_entries_.clear();
    id_unclosed_qty_.clear();
    close_reserved_qty_.clear();
    consumed_partial_exit_ids_.clear();
    pyramid_entries_.push_back({fill_price, current_bar_.timestamp, qty, id, bar_index_});
    id_unclosed_qty_[id] += qty;
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
                                            int created_bar,
                                            bool explicit_qty_prequantized) {
    bool carry_was_long = (created_position_side == PositionSide::LONG);
    bool tv_deferred_flip =
        script_has_strategy_close_
        && is_priced_entry
        && tv_carry_qty > 0.0
        && (carry_was_long ? !is_long : is_long);
    double base_qty = explicit_qty_prequantized
        ? explicit_qty
        : calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    double qty = tv_deferred_flip ? (tv_carry_qty + base_qty) : base_qty;
    if (tv_deferred_flip) {
        consume_tv_carry_from_siblings(id, created_position_side, created_bar);
    }
    // NOTE: for EXPLICIT-qty market entries the margin check is performed at
    // SIGNAL time inside strategy_entry / queue_deferred_close_order, NOT here
    // at fill time. This matches TV's broker emulator, which rejects entries
    // whose qty * SIGNAL_BAR_CLOSE exceeds equity. By the time we reach this
    // fill-side helper such an order has already been admitted (or rejected)
    // at signal time, and the next-bar slippage between signal close and fill
    // open should NOT flip a TV-accepted entry into a reject. The empirical
    // base — parity-probe-{03..06} + ies-probe-08 — is entirely explicit-qty /
    // pct<100 / headroom sizing, so the claim is scoped to it. The one FROZEN
    // default-sized carve-out that TV DOES re-check and drop at fill (a
    // percent==100, zero-commission, true-flat above-lot gap) is handled by
    // the gap-reject gate in apply_filled_order_to_state, upstream of this
    // helper — a dropped order never reaches enter_market_from_flat.
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
    // KI-62: only a same-direction MARKET add is scratched by a same-bar
    // from_entry bracket exit; a priced pyramid add is not this collision.
    pyramid_entries_.back().market_pyramid_add = !is_priced_entry;
    id_unclosed_qty_[id] += new_qty;
}


// close_only_opposite branch: TV semantic for opposite-direction entries
// where the strategy.entry call was placed with ``close_only_opposite=true``
// — close part of the existing opposite position by tx_qty, then open the
// requested-direction remainder if any.
void BacktestEngine::close_opposite_then_enter(const std::string& id, bool is_long,
                                               double fill_price, double explicit_qty,
                                               int explicit_qty_type,
                                               bool purge_pending_exits,
                                               bool explicit_qty_prequantized) {
    double tx_qty = explicit_qty_prequantized
        ? explicit_qty
        : calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    double close_qty = std::min(tx_qty, position_qty_);
    // execute_partial_exit_qty applies slippage internally (mirrors its other
    // callers, e.g. execute_partial_exit_by_percent). Pass the RAW fill_price —
    // pre-slipping here would double-slip the close leg (issue #27).
    execute_partial_exit_qty(fill_price, close_qty);
    // The ordinary close-only-opposite path historically purges stale exits.
    // A confirmed same-source flat MARKET pair is different: both transaction
    // legs execute inside process_pending_orders, where erasing the vector
    // would invalidate the active order reference and filled-index ledger.
    // Its stale exits follow flip_market_position_to's safe next-pass cleanup.
    if (purge_pending_exits) purge_exit_orders();
    double remainder = tx_qty - close_qty;
    if (remainder <= kQtyEpsilon) {
        return;
    }
    fill_price = apply_fill_slippage(fill_price, is_long);
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
                                             int explicit_qty_type,
                                             bool close_only) {
    // For the close we need exit slippage based on closing direction.
    // Closing a long = sell (price - slip); closing a short = buy (price + slip).
    // fill_price already has entry slippage applied; un-slip it before
    // re-applying with the exit direction.
    double raw_price = fill_price;
    if (slippage_ != 0 && !current_fill_is_limit_) {
        // LIMIT-triggered entry fills were never slipped (limit-or-better
        // path), so there is no entry slip to back out for the close leg.
        double slip = slippage_ * syminfo_mintick_;
        raw_price = is_long ? (fill_price - slip) : (fill_price + slip);
    }
    // Flag-aware close leg: a limit-triggered flip's close leg follows the
    // entry leg's limit semantics (unslipped, limit-or-better) for internal
    // consistency with the sibling close_opposite_then_enter path — both
    // legs land at the identical unslipped snapped price. No direct TV
    // evidence yet (needs a limit-flip slippage>0 export) — corpus provably
    // indifferent at slippage=0.
    double exit_fill = apply_fill_slippage(raw_price, position_side_ == PositionSide::SHORT);
    bool was_long = (position_side_ == PositionSide::LONG);

    // Emit one Trade per pyramid entry (matches TradingView reporting)
    for (auto& pe : pyramid_entries_) {
        emit_close_trade(pe, pe.qty, exit_fill, was_long);
    }

    if (close_only) {
        // Priced-entry reduce-only cases: either this order was armed during a
        // prior cycle and flips a later opposite position, or its same-cycle
        // frozen transaction exactly equals the grown live opposite position.
        // In both cases close the whole position and stay flat; do NOT open.
        // See apply_entry_order_fill's close_only_opposite predicates.
        reset_position_state_to_flat();
        return;
    }

    double new_qty = calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    PositionSide requested = is_long ? PositionSide::LONG : PositionSide::SHORT;
    open_fresh_position(requested, fill_price, new_qty, id);
}


// TradingView same-tick multi-entry sequential-fill semantics (audit rule
// R*, jevondijefferson-big-breakout-strategy, 2026-07-02 tv-ceiling audit —
// validated 26/26 against every in-window race in the TV export):
//
//   Same-tick entries fill SEQUENTIALLY in script-call order, each at
//   plain (non-augmented) qty; the reversal augmentation — close the
//   opposite position, then enter — attaches ONLY to the LAST
//   same-direction entry of the tick; the fill that crosses zero / opens
//   from flat owns the entry ID.
//
// This helper handles a market entry filling against an OPPOSITE position
// when ANOTHER same-direction market entry (distinct id, placed on the
// same on_bar) fills later at this same processing point:
//
//   - tx_qty > |pos|  ("class C"): the plain fill itself crosses zero —
//     the whole opposite position closes (exit rows tagged with THIS
//     order's id by the caller) and the REMAINDER (tx_qty - |pos|) opens
//     under THIS id. The later sibling then executes against the
//     sequentially-updated same-direction position and is rejected by
//     the pyramiding gate. TV example (2025-06-17 15:15 UTC race): old
//     long 7.6834 closes with exit signal "Short", the new short is the
//     0.2262 remainder — NOT a full q_plain lot.
//
//   - tx_qty <= |pos| ("class B"): the plain fill only reduces the
//     opposite position, so the position is still opposite when the LAST
//     entry executes — the augmentation attaches there: the remaining
//     position closes and a full plain lot opens under the LAST id.
//     TV's trade list reports the old lot's close as ONE row at the
//     shared fill price attributed to THIS (first) order's signal
//     (TV#29: short 7.8542 exits with signal "Long"; TV#30: the new long
//     7.6441 enters as "Wyckoff Swing Long"). To reproduce those rows —
//     both fills land at the same price, so the PnL split is invisible —
//     the whole position closes HERE tagged with this order's id, and
//     the sibling then opens its own plain lot from flat, owning the
//     entry ID exactly as R* requires.
//
// Without this rule the first fill took flip_market_position_to: the new
// lot opened at full plain qty under the FIRST id, binding the WRONG
// from_entry bracket (class B) or over-opening q_plain instead of the
// remainder (class C) — seeding the engine's multi-day tiny-qty
// stale-remainder desync chains (~10 race chains in the jevondijefferson
// diff). See tests/test_same_tick_multi_entry_race.cpp.
void BacktestEngine::sequential_same_tick_reversal_fill(const std::string& id,
                                                        bool is_long,
                                                        double fill_price,
                                                        double explicit_qty,
                                                        int explicit_qty_type) {
    // fill_price arrives entry-slipped (execute_market_entry applied entry
    // slippage). The close leg needs EXIT slippage for the closing
    // direction — un-slip, then re-apply, mirroring flip_market_position_to.
    double raw_price = fill_price;
    if (slippage_ != 0 && !current_fill_is_limit_) {
        double slip = slippage_ * syminfo_mintick_;
        raw_price = is_long ? (fill_price - slip) : (fill_price + slip);
    }
    double exit_fill = apply_fill_slippage(raw_price, position_side_ == PositionSide::SHORT);
    bool was_long = (position_side_ == PositionSide::LONG);
    double old_qty = position_qty_;

    // Close the ENTIRE opposite position — one row per pyramid lot, all
    // tagged with THIS order's id by apply_filled_order_to_state (matches
    // TV's single-row exit attribution to the first closing signal).
    for (auto& pe : pyramid_entries_) {
        emit_close_trade(pe, pe.qty, exit_fill, was_long);
    }
    reset_position_state_to_flat();

    // Plain (non-augmented) sizing, computed after the close so
    // percent-of-equity sees the realized PnL — the same equity basis
    // flip_market_position_to uses. Only the portion that crosses zero
    // opens a position; if the plain qty doesn't reach past the old
    // position, the LAST same-tick entry (filling next from flat) owns
    // the new position instead.
    double tx_qty = calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    double remainder = tx_qty - old_qty;
    if (remainder > kQtyEpsilon) {
        PositionSide requested = is_long ? PositionSide::LONG : PositionSide::SHORT;
        open_fresh_position(requested, fill_price, remainder, id);
    }
}


}  // namespace pineforge
