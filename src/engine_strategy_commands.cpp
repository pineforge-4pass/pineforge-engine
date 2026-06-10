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
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, tf_to_seconds(script_tf_))) return;

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
    // matched) and parity-anomalies/equity-mirror (full-equity sizing
    // right at the 1× boundary, where TV's behaviour is itself
    // non-deterministic — see corpus/parity-anomalies/README.md).
    // Only applied to MARKET entries (limit/stop entries have their
    // own price baked into the order itself).
    if (!std::isnan(qty) && std::isnan(limit_price) && std::isnan(stop_price)) {
        double margin_pct = is_long ? margin_long_ : margin_short_;
        if (margin_pct > 0.0 && !std::isnan(current_bar_.close)) {
            // Position value in account currency includes the futures
            // point-value multiplier (1.0 for crypto/equity — unchanged).
            double required_margin = std::abs(qty) * current_bar_.close
                                     * syminfo_.pointvalue
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
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) { return o.id == id; }),
        pending_orders_.end());

    PendingOrder order;
    order.id = id;
    order.from_entry = "";
    order.is_long = is_long;
    order.trail_points = std::numeric_limits<double>::quiet_NaN();
    order.trail_offset = std::numeric_limits<double>::quiet_NaN();
    order.qty = qty;
    order.qty_type = qty_type;
    order.qty_percent = 100.0;
    order.oca_name = oca_name;
    order.oca_type = oca_type;
    order.created_bar = bar_index_;
    order.created_seq = preserved_seq > 0 ? preserved_seq : next_order_seq_++;
    order.created_position_side = position_side_;
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
        if (process_orders_on_close_) {
            // With process_orders_on_close, market entries fill immediately at bar close
            // so that strategy.position_avg_price is correct for subsequent strategy.exit() calls
            double fill = current_bar_.close;
            execute_market_entry(id, is_long, fill, qty, qty_type, position_side_);
            // Set entry comment on the just-created pyramid entry
            if (!pyramid_entries_.empty()) pyramid_entries_.back().entry_comment = comment;
            return;
        }
        order.type = OrderType::MARKET;
        order.limit_price = std::numeric_limits<double>::quiet_NaN();
        order.stop_price = std::numeric_limits<double>::quiet_NaN();
    } else {
        order.type = OrderType::ENTRY;
        order.limit_price = limit_price;
        order.stop_price = stop_price;
    }

    pending_orders_.push_back(order);
}

void BacktestEngine::strategy_close(const std::string& id, const std::string& comment,
                                    double qty, double qty_percent, bool immediately) {
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, tf_to_seconds(script_tf_))) return;
    if (position_side_ == PositionSide::FLAT) {
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

    if (process_orders_on_close_ || immediately) {
        execute_immediate_close(id, comment, qty_to_close, matching_qty,
                                closes_full_position, closes_fifo_qty, closes_any_qty);
        return;
    }

    queue_deferred_close_order(id, comment, qty_to_close, matching_qty,
                               closes_full_position, closes_any_qty);
}

void BacktestEngine::strategy_close_all() {
    strategy_close("");
}

void BacktestEngine::strategy_exit(const std::string& id, const std::string& from_entry,
                                    double limit_price, double stop_price,
                                    double trail_points, double trail_offset,
                                    double trail_price, double qty_percent,
                                    const std::string& comment,
                                    double qty, const std::string& oca_name) {
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, tf_to_seconds(script_tf_))) return;
    bool has_explicit_qty = !std::isnan(qty);
    double qp = std::isnan(qty_percent) ? 100.0 : std::clamp(qty_percent, 0.0, 100.0);
    // If an explicit qty is given, derive an effective qp from the current
    // position size so downstream FIFO accounting (compute_exit_reserved_qty,
    // already-reserved tally, etc.) sees a consistent fraction. The order
    // itself stores the absolute qty so the per-fill execution path
    // honours the literal request.
    if (has_explicit_qty && position_qty_ > kQtyEpsilon) {
        double clamped_qty = std::min(qty, position_qty_);
        qp = (clamped_qty / position_qty_) * 100.0;
    }
    bool is_partial = qp < 100.0 - kFullPercentEps;
    bool has_trail_request = !std::isnan(trail_points);

    // Re-issued explicitly partial exits with the same id are one-shot for a live position.
    if (is_partial && position_side_ != PositionSide::FLAT
        && consumed_partial_exit_ids_.find(id) != consumed_partial_exit_ids_.end()) {
        return;
    }

    int64_t preserved_seq = 0;
    double preserved_reserved_qty = std::numeric_limits<double>::quiet_NaN();
    clear_existing_exit_order(id, from_entry, has_trail_request,
                              preserved_seq, preserved_reserved_qty);

    double reserved_qty = std::numeric_limits<double>::quiet_NaN();
    if (has_explicit_qty) {
        // Honour the explicit qty literally (clamped to the live position
        // and subject to the same already-reserved accounting). This is
        // the path Pine's ``strategy.exit(... qty=N)`` follows when N is
        // strictly smaller than the open position size — required for
        // multi-bracket per-position exits (validation_oca/oca-three-way-
        // probe-02 has two qty=1 brackets attached to a qty=2 entry).
        if (position_side_ == PositionSide::FLAT) {
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
                    already_reserved += position_qty_ * (oqp / 100.0);
                }
            }
            double available = std::max(0.0, position_qty_ - already_reserved);
            reserved_qty = std::min(qty, available);
            if (reserved_qty <= kQtyEpsilon) return;
        }
        is_partial = reserved_qty < position_qty_ - kFullQtyEps;
    } else {
        if (!compute_exit_reserved_qty(from_entry, preserved_reserved_qty,
                                       qp, is_partial, reserved_qty)) {
            return;
        }
    }

    PendingOrder order;
    order.id = id;
    order.from_entry = from_entry;
    order.type = OrderType::EXIT;
    order.is_long = false;
    order.limit_price = limit_price;
    order.stop_price = stop_price;
    order.trail_points = trail_points;
    order.trail_offset = trail_offset;
    order.qty = reserved_qty;
    order.qty_type = -1;
    order.qty_percent = qp;
    order.requested_partial = is_partial;
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
    order.created_position_side = position_side_;
    order.tv_carry_qty = position_qty_;
    order.comment = comment;
    order.created_while_in_position = (position_side_ != PositionSide::FLAT);

    pending_orders_.push_back(order);
}

void BacktestEngine::strategy_cancel(const std::string& id) {
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) { return o.id == id; }),
        pending_orders_.end());
}

void BacktestEngine::strategy_cancel_all() {
    pending_orders_.clear();
}

void BacktestEngine::strategy_order(const std::string& id, bool is_long, double qty,
                                     double limit_price, double stop_price,
                                     const std::string& oca_name, int oca_type) {
    if (!trading_is_active(current_bar_.timestamp, trade_start_time_, tf_to_seconds(script_tf_))) return;
    int64_t preserved_seq = 0;
    for (const auto& o : pending_orders_) {
        if (o.id == id) {
            preserved_seq = o.created_seq;
            break;
        }
    }

    // Remove existing pending order with same id
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) { return o.id == id; }),
        pending_orders_.end());

    PendingOrder order;
    order.id = id;
    order.from_entry = "";
    order.is_long = is_long;
    order.trail_points = std::numeric_limits<double>::quiet_NaN();
    order.trail_offset = std::numeric_limits<double>::quiet_NaN();
    order.qty = qty;
    order.qty_type = -1;
    order.qty_percent = 100.0;
    order.oca_name = oca_name;
    order.oca_type = oca_type;
    order.created_bar = bar_index_;
    order.created_seq = preserved_seq > 0 ? preserved_seq : next_order_seq_++;
    order.created_position_side = position_side_;
    order.tv_carry_qty = position_qty_;

    bool has_limit = !std::isnan(limit_price);
    bool has_stop = !std::isnan(stop_price);

    if (!has_limit && !has_stop) {
        order.type = OrderType::RAW_ORDER;
        order.limit_price = std::numeric_limits<double>::quiet_NaN();
        order.stop_price = std::numeric_limits<double>::quiet_NaN();
    } else {
        order.type = OrderType::RAW_ORDER;
        order.limit_price = limit_price;
        order.stop_price = stop_price;
    }

    pending_orders_.push_back(order);
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
//   (1) Pending strategy.exit orders bound to the same entry id are
//       wiped (community/IES regression: a partial TP1 limit and the
//       queued market close were both firing on the next bar's open,
//       producing two trade rows for the same logical close).
//
//   (2) Pending strategy.entry / market orders that were ADDED TO
//       THE NOW-CLOSING POSITION are wiped (probe in
//       tests/test_integration.cpp: a stale long-add stop placed
//       while already long must not re-open the position after the
//       long is closed).
//
// Crucially, pending entries placed FOR THE OPPOSITE DIRECTION
// (flip preparation — e.g., a short-stop entry placed while still
// long, intended to fire after the long closes) must NOT be
// cancelled here. The previous predicate `o.is_long == closing_long`
// wiped same-direction entries regardless of the position they were
// placed in, which broke validation/93-flip-stop-pyramiding-2: the
// 0:45 short-stop placed while in the morning long was cancelled
// by 12:15 close_all on the still-pending leg even though the
// engine had since flipped to a short position via the OTHER stop,
// dropping a short the strategy intended to keep alive overnight.
void BacktestEngine::cancel_orders_for_full_close(const std::string& id, bool closing_long) {
    pending_orders_.erase(
        std::remove_if(
            pending_orders_.begin(),
            pending_orders_.end(),
            [&](const PendingOrder& o) {
                if (o.type == OrderType::EXIT) {
                    if (id.empty()) {
                        return o.from_entry.empty();
                    }
                    return o.from_entry == id;
                }
                if (o.type != OrderType::ENTRY && o.type != OrderType::MARKET) {
                    return false;
                }
                if (o.created_bar >= bar_index_) {
                    // Same-on_bar reversal entry: keep it (handles
                    // strategy.close + strategy.entry(opposite) in the
                    // same Pine block).
                    return false;
                }
                // Only wipe entries that were a same-direction ADD to
                // the position being closed.
                bool added_to_long = closing_long
                    && o.created_position_side == PositionSide::LONG
                    && o.is_long;
                bool added_to_short = !closing_long
                    && o.created_position_side == PositionSide::SHORT
                    && !o.is_long;
                return added_to_long || added_to_short;
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
                                             bool closes_any_qty) {
    const double eps = kQtyEpsilon;
    size_t trades_before = trades_.size();
    if (closes_full_position) {
        execute_market_exit(current_bar_.close);
        purge_exit_orders();
    } else if (closes_fifo_qty) {
        execute_partial_exit_qty(current_bar_.close, qty_to_close);
        if (position_side_ == PositionSide::FLAT) {
            purge_exit_orders();
        }
    } else if (closes_any_qty) {
        double pct = matching_qty > eps ? (qty_to_close / matching_qty) * 100.0 : 100.0;
        execute_partial_exit_by_entry_percent(current_bar_.close, id, pct);
        if (position_side_ == PositionSide::FLAT) {
            purge_exit_orders();
        }
    }
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = comment;
        trades_[ti].exit_id = "__close__" + id;
    }
}

// Build the deferred EXIT pending order representing this close, to
// be matched at the next bar's open by process_pending_orders. Mirrors
// the qty / qty_percent shape that the partial-exit dispatch in
// execute_immediate_close would have produced for the same flags.
void BacktestEngine::queue_deferred_close_order(const std::string& id,
                                                const std::string& comment,
                                                double qty_to_close,
                                                double matching_qty,
                                                bool closes_full_position,
                                                bool closes_any_qty) {
    const double eps = kQtyEpsilon;
    PendingOrder order;
    order.id = "__close__" + id;
    order.from_entry = close_entries_rule_any_ ? id : "";
    order.type = OrderType::EXIT;
    order.is_long = false;
    order.limit_price = std::numeric_limits<double>::quiet_NaN();
    order.stop_price = std::numeric_limits<double>::quiet_NaN();
    order.trail_points = std::numeric_limits<double>::quiet_NaN();
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
    order.created_position_side = position_side_;
    order.tv_carry_qty = position_qty_;
    order.comment = comment;
    order.created_while_in_position = true;

    pending_orders_.push_back(order);
}

// Capture seq + reserved qty of an existing pending exit with the
// same (id, from_entry), reset the trail high-water mark when starting
// a fresh trail (no prior order, in-position), and erase any pending
// order with this id so the caller can push a freshly built
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

    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& o) { return o.id == id; }),
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
                                               double& qp_io,
                                               bool& is_partial_io,
                                               double& reserved_qty_out) {
    reserved_qty_out = std::numeric_limits<double>::quiet_NaN();
    if (position_side_ == PositionSide::FLAT) {
        return true;
    }

    double already_reserved = 0.0;
    for (const auto& o : pending_orders_) {
        if (o.type != OrderType::EXIT || o.from_entry != from_entry) continue;
        if (!std::isnan(o.qty)) {
            already_reserved += o.qty;
        } else {
            double oqp = std::isnan(o.qty_percent) ? 100.0 : std::clamp(o.qty_percent, 0.0, 100.0);
            already_reserved += position_qty_ * (oqp / 100.0);
        }
    }

    double available_qty = std::max(0.0, position_qty_ - already_reserved);
    if (!std::isnan(preserved_reserved_qty)) {
        reserved_qty_out = std::min(preserved_reserved_qty, position_qty_);
    } else {
        double requested_qty = position_qty_ * (qp_io / 100.0);
        reserved_qty_out = std::min(requested_qty, available_qty);
    }
    if (reserved_qty_out <= kQtyEpsilon) {
        return false;
    }
    qp_io = (position_qty_ > kQtyEpsilon) ? (reserved_qty_out / position_qty_) * 100.0 : qp_io;
    is_partial_io = reserved_qty_out < position_qty_ - kFullQtyEps;

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
