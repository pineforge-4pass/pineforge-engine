#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/compat/pine/exit_lifecycle.hpp>
/*
 * engine_orders.cpp — execute_market_* and partial-exit fill mechanics
 */

#include "../engine_internal.hpp"
#include <pineforge/order_action.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pineforge {
using namespace source;
using namespace internal;

namespace {
// Existing source FIFO endpoint policy; never a native quantity tolerance.
// Keep the R2 stop/whole-lot interpretation at 1e-10 in this adapter.
constexpr double kSourceFifoEndpointEpsilon = kQtyEpsilon;

std::optional<execution::SelectedOpeningSet> source_fifo_prefix_membership(
        const std::vector<PyramidEntry>& lots, double qty_limit,
        int64_t cycle) {
    if (cycle <= 0 || !std::isfinite(qty_limit) || qty_limit <= 0.0)
        return std::nullopt;

    double qty_closed = 0.0;
    size_t prefix_size = 0;
    for (const auto& lot : lots) {
        // Match the source's original accumulation and endpoint ordering.
        // Once at the endpoint, even a tiny next sibling stays unselected.
        if (qty_closed >= qty_limit - kSourceFifoEndpointEpsilon) break;
        if (!std::isfinite(lot.qty) || lot.qty <= 0.0) return std::nullopt;
        const double close_qty = std::min(lot.qty, qty_limit - qty_closed);
        const double keep_qty = lot.qty - close_qty;
        if (keep_qty > kSourceFifoEndpointEpsilon) return std::nullopt;
        ++prefix_size;
        qty_closed += close_qty;
    }
    if (prefix_size == 0 || prefix_size == lots.size()) return std::nullopt;

    execution::SelectedOpeningSet selection{cycle, {}};
    std::unordered_set<uint64_t> included;
    double selected_qty = 0.0;
    for (size_t index = 0; index < prefix_size; ++index) {
        const auto& lot = lots[index];
        if (lot.entry_incarnation == 0) return std::nullopt;
        if (included.insert(lot.entry_incarnation).second)
            selection.incarnations.push_back(lot.entry_incarnation);
        selected_qty += lot.qty;
        if (!std::isfinite(selected_qty)) return std::nullopt;
    }
    // An opening identity may have multiple physical fragments, but all of
    // its live fragments must belong to this prefix. Otherwise use Reduce.
    for (size_t index = prefix_size; index < lots.size(); ++index) {
        if (included.count(lots[index].entry_incarnation) != 0)
            return std::nullopt;
    }
    return selection;
}

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

double source::PineStrategyHost::calc_qty_for_type(double fill_price, double qty_value, int qty_type) const {
    if (std::isnan(qty_value)) {
        return calc_qty(fill_price);
    }
    const double equity = qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        ? percent_commission_live_equity(round_to_mintick(current_bar_.close)) : 0.0;
    return calc_qty_for_type_from_equity(fill_price, qty_value, qty_type, equity);
}

double source::PineStrategyHost::calc_default_qty_from_equity(double fill_price, double equity) const {
    const double basis = round_to_mintick(fill_price);
    switch (default_qty_type_) {
        case QtyType::FIXED:
            return apply_qty_step(default_qty_value_);
        case QtyType::PERCENT_OF_EQUITY: {
            // Source money precision and commission reservation stay exactly
            // here for both live-equity and post-close projection callers.
            if (tv_money_lot_sizing()) equity = tv_money_round(equity);
            if (!std::isfinite(equity)) return 0.0;
            const double cash = reserve_percent_commission(
                equity * (default_qty_value_ / 100.0)) / active_account_currency_fx();
            if (!(std::isfinite(basis) && basis > 0.0)) return 0.0;
            if (tv_money_lot_sizing())
                return tv_money_floor_lot(cash / (basis * syminfo_.pointvalue), qty_step_);
            return apply_qty_step(cash / (basis * syminfo_.pointvalue));
        }
        case QtyType::CASH:
            return (std::isfinite(basis) && basis > 0.0)
                ? apply_qty_step((default_qty_value_ / active_account_currency_fx())
                                  / (basis * syminfo_.pointvalue)) : 0.0;
    }
    return apply_qty_step(default_qty_value_);
}

double source::PineStrategyHost::calc_qty_for_type_from_equity(
        double fill_price, double qty_value, int qty_type, double equity) const {
    if (std::isnan(qty_value)) return calc_default_qty_from_equity(fill_price, equity);
    // qty_step_ lot-size flooring applies uniformly regardless of how the
    // caller's qty was derived — including this FIXED branch, which is the
    // common ``strategy.entry(qty=someComputedExpr)`` shape (e.g. a DCA base/
    // safety-order qty = orderSizeUsd/close). See apply_qty_step's doc
    // comment (engine.hpp) for the verified TV behavior this mirrors.
    if (qty_type < 0 || qty_type == static_cast<int>(QtyType::FIXED)) {
        return apply_qty_step(qty_value);
    }
    // The explicit percent_of_equity / cash branches below run on the SAME
    // on-tick basis as calc_qty (engine.hpp): the open lot is marked and the
    // budget is divided at round_to_mintick of the price, never at a raw
    // sub-tick print, so the two sizing consumers cannot diverge on a
    // sub-penny feed. Reachability note: order.qty_type is set only from
    // strategy_entry's per-call qty_type parameter (default -1); the corpus'
    // generated code only ever sets default_qty_type_ and pineforge.h exposes
    // no per-call qty_type, so this path is reached by direct C++ callers
    // only — it is kept on the rounded basis for consistency, not because a
    // tape pinned it (calc_qty carries the F / AAPL census).
    const double basis = round_to_mintick(fill_price);
    if (qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)) {
        if (!std::isfinite(equity)) return 0.0;
        double cash = reserve_percent_commission(equity * (qty_value / 100.0));
        // Reject (qty 0) on a non-finite / non-positive fill price — a degenerate
        // $0/NaN print must NOT size as the raw % number (silent wrong-qty bug).
        // One contract's currency exposure is basis × pointvalue (1.0 for
        // crypto/equity — legacy math unchanged; futures divide the budget by
        // the full per-contract notional). cash is account-currency (equity is);
        // convert to quote currency via account_currency_fx_ before dividing by
        // the quote-currency basis — same convention as calc_qty() in
        // engine.hpp. fx=1.0 is a no-op.
        return (std::isfinite(basis) && basis > 0.0)
            ? apply_qty_step((cash / active_account_currency_fx())
                             / (basis * syminfo_.pointvalue)) : 0.0;
    }
    if (qty_type == static_cast<int>(QtyType::CASH)) {
        return (std::isfinite(basis) && basis > 0.0)
            ? apply_qty_step((qty_value / active_account_currency_fx())
                             / (basis * syminfo_.pointvalue)) : 0.0;
    }
    return apply_qty_step(qty_value);
}

double source::PineStrategyHost::source_reversal_qty(
        double fill_price, double explicit_qty, int explicit_qty_type,
        bool prequantized) const {
    if (prequantized) return explicit_qty;
    const bool needs_equity = std::isnan(explicit_qty)
        ? default_qty_type_ == QtyType::PERCENT_OF_EQUITY
        : explicit_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    if (!needs_equity)
        return calc_qty_for_type_from_equity(fill_price, explicit_qty, explicit_qty_type, 0.0);
    const auto projection = project_native_settlement_v1(
        execution::Flatten{}, execution::Fill{fill_price, {}, {}, 0});
    if (projection.status != execution::Status::Applied
        && projection.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved reversal sizing projection");
    return calc_qty_for_type_from_equity(
        fill_price, explicit_qty, explicit_qty_type, projection.realized_balance);
}

void source::PineStrategyHost::execute_market_entry(const std::string& id, bool is_long, double fill_price,
                                          double explicit_qty, int explicit_qty_type,
                                          PositionSide created_position_side,
                                          bool close_only_opposite,
                                          bool is_priced_entry,
                                          double tv_carry_qty,
                                          int created_bar,
                                          bool later_same_tick_entry,
                                          bool paired_flat_market_transaction,
                                          bool explicit_qty_prequantized,
                                          uint64_t entry_incarnation) {
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
        execute_market_exit(fill_price);
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
                                   (explicit_qty_prequantized
                                    || paired_flat_market_transaction),
                               entry_incarnation);
        return;
    }

    if (position_side_ == requested) {
        add_to_pyramid_market_with_qty_provenance(
            id, is_long, fill_price, explicit_qty, explicit_qty_type,
            created_position_side, is_priced_entry, explicit_qty_prequantized,
            entry_incarnation);
        return;
    }

    if (created_position_side == PositionSide::FLAT && close_only_opposite) {
        // fill_price is already resolved by apply_fill_slippage above.
        close_opposite_then_enter(
            id, is_long, fill_price, explicit_qty, explicit_qty_type,
            /*purge_pending_exits=*/!paired_flat_market_transaction,
            /*explicit_qty_prequantized=*/
                (explicit_qty_prequantized
                 || paired_flat_market_transaction),
            entry_incarnation);
        return;
    }

    if (later_same_tick_entry) {
        sequential_same_tick_reversal_fill_with_qty_provenance(
            id, is_long, fill_price, explicit_qty, explicit_qty_type,
            explicit_qty_prequantized, entry_incarnation);
        return;
    }

    // ``close_only_opposite`` reaches here for a created_position_side != FLAT
    // reduce-only flip (the FLAT bracket case returned above via
    // close_opposite_then_enter). This is either a deferred-flip carry that
    // reverses a later position cycle, or the equality-only same-cycle frozen
    // transaction whose whole broker movement is consumed by the close. Both
    // close the live opposite position without opening their own leg.
    flip_market_position_to(id, is_long, fill_price, explicit_qty, explicit_qty_type,
                            explicit_qty_prequantized,
                            /*close_only=*/close_only_opposite,
                            entry_incarnation);
}

void source::PineStrategyHost::execute_market_exit(double fill_price) {
    if (position_side_ == PositionSide::FLAT) {
        return;
    }

    // Apply slippage: closing long = sell (lower), closing short = buy
    // (higher). LIMIT-triggered exits (TP brackets) take the unslipped
    // limit-or-better path via apply_fill_slippage.
    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_fill_slippage(fill_price, is_buy);
    const auto result = settle_resolved_execution(
        execution::Flatten{}, execution::Fill{fill_price, {}, {}, 0});
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved full-position settlement");
}

void source::PineStrategyHost::record_range_end_close_trades() {
    range_end_trades_.clear();
    if (stream_warmup_mode_) return;
    if (position_side_ == PositionSide::FLAT) return;
    if (equity_curve_.empty()) return;          // no script bar was dispatched
    if (!std::isfinite(current_bar_.close)) return;

    const bool was_long = (position_side_ == PositionSide::LONG);
    const double fill_price = bar_fill_price(current_bar_.close);
    // Date the exit leg on the script bar's label, never a magnifier
    // sub-bar; the bar itself is restored afterwards.
    const int64_t bar_ts = current_bar_.timestamp;
    current_bar_.timestamp = equity_curve_.back().time_ms;
    double range_end_pnl = 0.0;
    for (const auto& pe : pyramid_entries_) {
        Trade row = build_close_trade(pe, pe.qty, fill_price, was_long);
        row.open_at_end = true;              // exit_id / exit_comment stay empty
        range_end_pnl += row.pnl;
        range_end_trades_.push_back(std::move(row));
    }
    current_bar_.timestamp = bar_ts;

    pf_equity_point_t& last = equity_curve_.back();
    last.open_profit = 0.0;
    last.equity = initial_capital_ + net_profit_sum_ + range_end_pnl;
    // Re-fold the scalar extremes from the curve so they read the re-marked
    // last point exactly as the compute_equity_stats walk will.
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;
    max_drawdown_ = 0.0;
    max_runup_ = 0.0;
    for (const auto& p : equity_curve_) fold_equity_extreme(p.equity);
}

void source::PineStrategyHost::execute_partial_exit_qty(
        double fill_price, double qty_to_close, PositionReductionCause cause) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;
    const double held = position_side_ == PositionSide::LONG
        ? position_qty_ : -position_qty_;
    const auto reduction = order_action::plan(held, order_action::Reduce{qty_to_close});
    if (!reduction) return;
    qty_to_close = reduction->close_units();
    if (qty_to_close <= kQtyEpsilon) return;

    bool is_buy = (position_side_ == PositionSide::SHORT);
    fill_price = apply_fill_slippage(fill_price, is_buy);
    const int pre_count = position_entry_count_;
    execution::Action action = order_action::Reduce{qty_to_close};
    std::optional<execution::SelectedOpeningSet> prefix;
    if (std::abs(held) - qty_to_close <= kQtyEpsilon) action = execution::Flatten{};
    else prefix = source_fifo_prefix_membership(
        pyramid_entries_, qty_to_close, position_cycle_seq_);
    const execution::Fill fill{fill_price, {}, {}, 0};
    const auto result = prefix
        ? settle_execution_selected_with_lifecycle(execution::Flatten{}, fill, {}, *prefix)
        : settle_resolved_execution(action, fill);
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved partial-close settlement");
    if (result.status == execution::Status::Applied)
        restore_source_partial_exit_slots(pre_count, cause);
}

void source::PineStrategyHost::execute_partial_exit(double fill_price, double qty_percent,
                                          PositionReductionCause cause) {
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
    execute_partial_exit_qty(fill_price, qty_to_close, cause);
}

void source::PineStrategyHost::execute_partial_exit_by_entry(double fill_price,
                                                   const std::string& from_entry,
                                                   PositionReductionCause cause) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;

    const auto incarnations = source_opening_membership(pyramid_entries_,
        [&](const PyramidEntry& lot) { return lot.entry_id == from_entry; });
    if (incarnations.empty()) return;
    const execution::SelectedOpeningSet selection{position_cycle_seq_, incarnations};
    fill_price = apply_fill_slippage(fill_price, position_side_ == PositionSide::SHORT);
    const int pre_count = position_entry_count_;
    const auto result = settle_execution_selected_with_lifecycle(
        execution::Flatten{}, execution::Fill{fill_price, {}, {}, 0}, {}, selection);
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved bound-close settlement");
    if (result.status == execution::Status::Applied)
        restore_source_partial_exit_slots(pre_count, cause);
}

void source::PineStrategyHost::execute_partial_exit_by_entry_qty(
        double fill_price, const std::string& from_entry, double qty_to_close,
        PositionReductionCause cause) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;
    if (!std::isfinite(qty_to_close) || qty_to_close <= kQtyEpsilon) return;

    const auto incarnations = source_opening_membership(pyramid_entries_,
        [&](const PyramidEntry& lot) { return lot.entry_id == from_entry; });
    if (incarnations.empty()) return;
    double selected_qty = 0.0;
    bool has_unselected_lots = false;
    for (const auto& lot : pyramid_entries_) {
        if (lot.entry_id == from_entry) selected_qty += lot.qty;
        else has_unselected_lots = true;
    }
    const execution::SelectedOpeningSet selection{position_cycle_seq_, incarnations};
    execution::Action action = order_action::Reduce{qty_to_close};
    if (!has_unselected_lots && selected_qty - qty_to_close <= kQtyEpsilon)
        action = execution::Flatten{};
    fill_price = apply_fill_slippage(fill_price, position_side_ == PositionSide::SHORT);
    const int pre_count = position_entry_count_;
    const auto result = settle_execution_selected_with_lifecycle(
        action, execution::Fill{fill_price, {}, {}, 0}, {}, selection);
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved bound-close settlement");
    if (result.status == execution::Status::Applied)
        restore_source_partial_exit_slots(pre_count, cause);
}

void source::PineStrategyHost::execute_partial_exit_by_entry_percent(double fill_price,
                                                           const std::string& from_entry,
                                                           double qty_percent,
                                                           PositionReductionCause cause) {
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;

    double matched_qty = 0.0;
    for (const auto& pe : pyramid_entries_) {
        if (pe.entry_id == from_entry) matched_qty += pe.qty;
    }
    if (matched_qty <= kQtyEpsilon) return;

    double pct = std::clamp(qty_percent, 0.0, 100.0);
    double qty_to_close = matched_qty * (pct / 100.0);
    if (qty_to_close <= kQtyEpsilon) return;

    execute_partial_exit_by_entry_qty(fill_price, from_entry, qty_to_close, cause);
}

double source::PineStrategyHost::cover_samebar_market_adds_on_exit(const source::PendingOrder& order,
                                                         double fill_price,
                                                         PositionReductionCause cause) {
    if (order.from_entry.empty()) return 0.0;
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return 0.0;
    // Scope to a PRICED bracket (stop/limit/trail). A plain market close /
    // close_all already flattens the whole position through its own path.
    bool priced_bracket = !std::isnan(order.legs.prices().stop_price)
        || !std::isnan(order.legs.prices().limit_price)
        || !std::isnan(order.legs.prices().trail_points)
        || !std::isnan(order.legs.prices().trail_price);
    if (!priced_bracket) return 0.0;

    const auto incarnations = source_opening_membership(pyramid_entries_,
        [&](const PyramidEntry& lot) {
            return lot.market_pyramid_add && lot.entry_bar_index == bar_index_
                && lot.entry_id == order.from_entry;
        });
    if (incarnations.empty()) return 0.0;
    const execution::SelectedOpeningSet selection{position_cycle_seq_, incarnations};
    const double slipped = apply_fill_slippage(fill_price, position_side_ == PositionSide::SHORT);
    // This is a second Fill after primary restoration and R20, so capture its
    // own current source slot count rather than reusing the primary snapshot.
    const int pre_count = position_entry_count_;
    const auto result = settle_execution_selected_with_lifecycle(
        execution::Flatten{}, execution::Fill{slipped, order.id, order.comment, order.incarnation},
        {}, selection);
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved bound-close settlement");
    if (result.status == execution::Status::Applied)
        restore_source_partial_exit_slots(pre_count, cause);
    return result.closed_units;
}

void source::PineStrategyHost::cancel_oca_group(std::string oca_name, std::string exclude_id) {
    // Direct callers may borrow both strings from the vector being erased.
    // Value parameters keep membership/exclusion stable throughout remove_if.
    if (oca_name.empty()) return;
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const source::PendingOrder& o) {
                return o.oca_name == oca_name && o.id != exclude_id;
            }),
        pending_orders_.end());
}

void source::PineStrategyHost::reduce_oca_group(std::string oca_name,
                                      std::string exclude_id,
                                      double filled_qty) {
    if (oca_name.empty()) return;
    if (!(filled_qty > 0.0)) return;  // nothing to subtract
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](source::PendingOrder& o) {
                if (o.oca_name != oca_name || o.id == exclude_id) return false;
                if (std::isnan(o.qty)) return true;  // default-sized: cancel
                o.qty -= filled_qty;
                return o.qty <= kOcaQtyEpsilon;
            }),
        pending_orders_.end());
}

void source::PineStrategyHost::purge_exit_orders(bool retain_for_pending_entries) {
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
                [&](const source::PendingOrder& o) {
                    return o.type == OrderType::EXIT
                        && !(!o.from_entry.empty()
                             && pending_entry_ids.count(o.from_entry));
                }),
            pending_orders_.end());
        return;
    }
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [](const source::PendingOrder& o) { return o.type == OrderType::EXIT; }),
        pending_orders_.end());
}

Trade source::PineStrategyHost::build_close_trade(const PyramidEntry& pe, double close_qty,
                                        double fill_price, bool was_long) const {
    execution::PhysicalExecutionContext context;
    context.effective_time_ms = current_bar_.timestamp;
    context.interval_index = bar_index_;
    context.preceding_exit_path_prefix = fold_exit_path_extremes_;
    if (!std::isnan(fold_exit_trail_peak_)) {
        context.preceding_exit_trail_peak = fold_exit_trail_peak_;
    }
    return build_close_trade_with_costs(pe, close_qty, fill_price, was_long,
        allocated_entry_commission(pe, close_qty), calc_commission(fill_price, close_qty),
        context);
}

void source::PineStrategyHost::emit_close_trade(const PyramidEntry& pe, double close_qty,
                                      double fill_price, bool was_long) {
    record_close_trade(build_close_trade(pe, close_qty, fill_price, was_long));
}

void source::PineStrategyHost::restore_source_partial_exit_slots(
        int pre_count, PositionReductionCause cause) {
    // Settlement owns quantities, average price, cycles, and physical dust.
    // This adapter step restores only the source's occupied-slot policy.
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return;
    if (cause == PositionReductionCause::BRACKET_EXIT) {
        position_entry_count_ = std::max(pre_count, static_cast<int>(pyramid_entries_.size()));
    }
}

exit_legs::Frame source::PineStrategyHost::next_leg_event(exit_legs::Phase phase) {
    const auto frame = preview_next_leg_event(phase);
    ++exit_leg_event_seq_;
    return frame;
}

void source::PineStrategyHost::apply_leg_action(source::PendingOrder& order, exit_legs::Operation operation,
                                      std::optional<exit_legs::Frame> supplied) {
    // Rebinding can require a later receipt event at this same hook. Preserve
    // its phase when replacing that receipt; an after-margin completion must
    // not be recorded as processed during the earlier observation phase.
    const auto result = transition_exit_leg(
        order.legs, order.incarnation, std::move(operation), supplied,
        exit_leg_event_seq_, position_cycle_seq_);
    switch (result) {
    case ExitLegTransitionResult::Applied:
    case ExitLegTransitionResult::Replay:
        return;
    case ExitLegTransitionResult::Exhausted:
        throw std::overflow_error("exit lifecycle event exhausted");
    case ExitLegTransitionResult::RevisionExhausted:
        throw std::overflow_error("exit lifecycle revision exhausted");
    case ExitLegTransitionResult::StaleIdentity:
        throw std::logic_error("stale exit lifecycle instruction");
    case ExitLegTransitionResult::BindRefused:
        throw std::logic_error("exit lifecycle owner bind refused");
    case ExitLegTransitionResult::ActionRefused:
        throw std::logic_error("exit lifecycle action refused");
    }
    throw std::logic_error("exit lifecycle action refused");
}

void source::PineStrategyHost::bind_exit_activation(source::PendingOrder& order) {
    if (order.type != OrderType::EXIT) return;
    if (!order.legs.target().incarnation) order.legs.attach(order.incarnation, position_cycle_seq_);
    if (order.legs.target().owner != position_cycle_seq_)
        apply_leg_action(order, exit_legs::BindOwner{position_cycle_seq_});
    if (position_side_ == PositionSide::FLAT || position_cycle_seq_ <= 0) {
        order.leg_activation.unbind();
        return;
    }
    order.leg_activation.bind(order.pine_exit_activation.resolve(
        position_cycle_seq_, position_open_bar_));
}

void source::PineStrategyHost::bind_retained_exit_activations() {
    for (auto& order : pending_orders_) bind_exit_activation(order);
}

void source::PineStrategyHost::unbind_exit_activations() {
    for (auto& order : pending_orders_) {
        if (order.type == OrderType::EXIT) {
            order.leg_activation.unbind();
            if (!order.legs.target().incarnation) order.legs.attach(order.incarnation, position_cycle_seq_);
            const exit_legs::Action action{order.legs.target(), order.legs.revision(), next_leg_event(), exit_legs::BindOwner{0}};
            // A prearmed lifecycle may not have acquired the physical cycle.
            // Unbind its actual owner while preserving exact order identity.
            if (order.legs.apply({order.incarnation, order.legs.target().owner}, action)
                    != exit_legs::Result::Applied)
                throw std::logic_error("exit lifecycle flat unbind refused");
        }
    }
}

void source::PineStrategyHost::open_fresh_position(PositionSide requested, double fill_price,
                                         double qty, const std::string& id,
                                         uint64_t entry_incarnation) {
    if (position_side_ != PositionSide::FLAT)
        throw std::runtime_error("invalid resolved fresh opening: position not flat");
    settle_source_opening(requested, fill_price, qty, id, {}, entry_incarnation);
}

execution::Result source::PineStrategyHost::settle_source_opening(
        PositionSide requested, double fill_price, double qty,
        const std::string& id, const std::string& comment, uint64_t incarnation) {
    if ((requested != PositionSide::LONG && requested != PositionSide::SHORT)
        || (position_side_ != PositionSide::FLAT && position_side_ != requested)
        || !std::isfinite(qty) || qty < 0.0)
        throw std::runtime_error("invalid resolved source opening");
    const std::size_t before_lots = pyramid_entries_.size();
    const auto result = settle_resolved_execution(
        order_action::Transact{requested == PositionSide::LONG ? qty : -qty},
        execution::Fill{fill_price, id, comment, incarnation});
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved source opening settlement");
    if (result.status == execution::Status::Applied
        && (result.closed_units != 0.0 || result.opened_units == 0.0
            || pyramid_entries_.size() != before_lots + 1
            || pyramid_entries_.back().entry_incarnation != incarnation
            || pyramid_entries_.back().qty != std::abs(result.opened_units)))
        throw std::runtime_error("invalid resolved source opening lot provenance");
    return result;
}

void source::PineStrategyHost::consume_tv_carry_from_siblings(const std::string& id,
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

void source::PineStrategyHost::enter_market_from_flat(const std::string& id, bool is_long,
                                            double fill_price, double explicit_qty,
                                            int explicit_qty_type,
                                            PositionSide created_position_side,
                                            bool is_priced_entry, double tv_carry_qty,
                                            int created_bar,
                                            bool explicit_qty_prequantized,
                                            uint64_t entry_incarnation) {
    const bool carry_was_long =
        created_position_side == PositionSide::LONG;
    const bool tv_deferred_flip =
        is_priced_entry
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
    open_fresh_position(requested, fill_price, qty, id, entry_incarnation);
}

void source::PineStrategyHost::add_to_pyramid_market(const std::string& id, bool is_long,
                                           double fill_price, double explicit_qty,
                                           int explicit_qty_type,
                                           PositionSide created_position_side,
                                           bool is_priced_entry,
                                           uint64_t entry_incarnation) {
    add_to_pyramid_market_with_qty_provenance(
        id, is_long, fill_price, explicit_qty, explicit_qty_type,
        created_position_side, is_priced_entry, false, entry_incarnation);
}

void source::PineStrategyHost::add_to_pyramid_market_with_qty_provenance(
        const std::string& id, bool is_long, double fill_price, double explicit_qty,
        int explicit_qty_type, PositionSide created_position_side,
        bool is_priced_entry, bool explicit_qty_prequantized,
        uint64_t entry_incarnation) {
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
    const double new_qty = explicit_qty_prequantized
        ? explicit_qty : calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    // Zero-lot add safety net. The fill kernel (apply_filled_order_to_state's
    // zero-lot decline) consumes such an order before it reaches here; should
    // any path bypass that gate, never materialize a qty-0 pyramid lot nor
    // spend a pyramiding slot on it — TV does not place the order at all.
    if (!(new_qty > kQtyEpsilon)) return;
    const auto result = settle_source_opening(
        requested, fill_price, new_qty, id, {}, entry_incarnation);
    // KI-62: only a same-direction MARKET add is scratched by a same-bar
    // from_entry bracket exit; a priced pyramid add is not this collision.
    if (result.status == execution::Status::Applied && result.opened_units != 0.0)
        pyramid_entries_.back().market_pyramid_add = !is_priced_entry;
}

void source::PineStrategyHost::close_opposite_then_enter(const std::string& id, bool is_long,
                                               double fill_price, double explicit_qty,
                                               int explicit_qty_type,
                                               bool purge_pending_exits,
                                               bool explicit_qty_prequantized,
                                               uint64_t entry_incarnation) {
    execution::LifecycleEffects lifecycle;
    if (purge_pending_exits) lifecycle.removals = snapshot_exit_pending_removals();
    apply_resolved_close_opposite_then_enter(
        id, is_long, fill_price, explicit_qty, explicit_qty_type,
        explicit_qty_prequantized, entry_incarnation, std::move(lifecycle));
}

std::vector<execution::PendingRemoval>
source::PineStrategyHost::snapshot_exit_pending_removals() const {
    std::vector<execution::PendingRemoval> removals;
    for (const auto& order : pending_orders_) {
        if (order.type == OrderType::EXIT) {
            removals.push_back({order.incarnation, order.created_seq,
                                order.legs.target(), order.legs.revision()});
        }
    }
    return removals;
}

void source::PineStrategyHost::apply_resolved_close_opposite_then_enter(
        const std::string& id, bool is_long, double fill_price,
        double explicit_qty, int explicit_qty_type,
        bool explicit_qty_prequantized, uint64_t entry_incarnation,
        execution::LifecycleEffects lifecycle) {
    const double tx_qty = explicit_qty_prequantized
        ? explicit_qty
        : calc_qty_for_type(fill_price, explicit_qty, explicit_qty_type);
    if (!std::isfinite(tx_qty) || tx_qty < 0.0)
        throw std::invalid_argument("invalid close-opposite transaction quantity");
    if (tx_qty == 0.0) return;
    const double signed_units = is_long ? tx_qty : -tx_qty;
    double held = 0.0;
    for (const auto& lot : pyramid_entries_) held += lot.qty;
    const double signed_held = position_side_ == PositionSide::SHORT ? -held
        : position_side_ == PositionSide::LONG ? held : 0.0;
    const auto planned = order_action::plan(
        signed_held, order_action::Transact{signed_units});
    if (!planned)
        throw std::runtime_error("unrepresentable close-opposite transaction");
    if (planned->no_effect()) return;

    execution::Action action = order_action::Transact{signed_units};
    const double remainder = std::abs(planned->open_units());
    if (remainder <= kQtyEpsilon) {
        if (!pyramid_entries_.empty() && planned->close_units() >= held)
            action = execution::Flatten{};
        else
            action = order_action::Reduce{planned->close_units()};
    }

    const auto result = settle_execution_with_lifecycle(
        action, execution::Fill{fill_price, id, {}, entry_incarnation},
        lifecycle);
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved close-opposite settlement");
}

void source::PineStrategyHost::flip_market_position_to(const std::string& id, bool is_long,
                                             double fill_price, double explicit_qty,
                                             int explicit_qty_type,
                                             bool explicit_qty_prequantized,
                                             bool close_only,
                                             uint64_t entry_incarnation) {
    // The incoming direction is also the closing direction. The caller has
    // already resolved this one price; no un-slip/re-slip or second ticket.
    const execution::Fill fill{fill_price, id, {}, entry_incarnation};
    double incoming = 0.0;
    if (!close_only) {
        incoming = source_reversal_qty(
            fill_price, explicit_qty, explicit_qty_type, explicit_qty_prequantized);
        if (!std::isfinite(incoming) || incoming < 0.0)
            throw std::runtime_error("invalid resolved flip quantity");
    }
    const auto result = incoming > 0.0
        ? settle_reversal_with_lifecycle_v1(
            execution::ReverseTo{is_long ? incoming : -incoming}, fill, {})
        : settle_resolved_execution(execution::Flatten{}, fill);
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved flip settlement");
}

void source::PineStrategyHost::sequential_same_tick_reversal_fill(const std::string& id,
                                                        bool is_long,
                                                        double fill_price,
                                                        double explicit_qty,
                                                        int explicit_qty_type,
                                                        uint64_t entry_incarnation) {
    sequential_same_tick_reversal_fill_with_qty_provenance(
        id, is_long, fill_price, explicit_qty, explicit_qty_type, false, entry_incarnation);
}

void source::PineStrategyHost::sequential_same_tick_reversal_fill_with_qty_provenance(
        const std::string& id, bool is_long, double fill_price, double explicit_qty,
        int explicit_qty_type, bool explicit_qty_prequantized, uint64_t entry_incarnation) {
    double held = 0.0;
    for (const auto& lot : pyramid_entries_) held += lot.qty;
    const double transaction = source_reversal_qty(
        fill_price, explicit_qty, explicit_qty_type, explicit_qty_prequantized);
    if (!std::isfinite(transaction) || transaction < 0.0)
        throw std::runtime_error("invalid resolved sequential quantity");
    execution::Action action = execution::Flatten{};
    if (transaction - held > kQtyEpsilon)
        action = order_action::Transact{is_long ? transaction : -transaction};
    // Class B closes the entire old book here; its later sibling remains a
    // separate Fill. Empty lifecycle preserves the active pending iteration.
    const auto result = settle_resolved_execution(
        action, execution::Fill{fill_price, id, {}, entry_incarnation});
    if (result.status != execution::Status::Applied
        && result.status != execution::Status::NoEffect)
        throw std::runtime_error("invalid resolved sequential settlement");
}

} // namespace pineforge
