#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace pineforge {
namespace {
template<class T>
void reserve_effects(std::vector<T>& values, size_t extra) {
    if (extra > values.max_size() - values.size())
        throw std::length_error("execution effect capacity exhausted");
    const size_t required = values.size() + extra;
    if (required <= values.capacity()) return;
    const size_t grown = values.capacity() > values.max_size() / 2
        ? values.max_size() : values.capacity() * 2;
    values.reserve(std::max(required, grown));
}
} // namespace

execution::Result BacktestEngine::settle_resolved_execution(
        const execution::Action& action, const execution::Fill& fill) {
    return settle_execution_with_lifecycle(action, fill, {});
}

execution::Result BacktestEngine::settle_execution_with_lifecycle(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle) {
    using execution::Status;
    if (!std::isfinite(fill.price)) return {Status::InvalidPrice};
    if (fill.commission_account && !std::isfinite(*fill.commission_account))
        return {Status::InvalidAccounting};
    const auto no_effect = [&]() -> execution::Result {
        return {fill.commission_account && *fill.commission_account != 0.0
            ? Status::InvalidAccounting : Status::NoEffect};
    };

    const bool flatten = std::holds_alternative<execution::Flatten>(action);
    const auto* reduce = std::get_if<order_action::Reduce>(&action);
    const auto* transact = std::get_if<order_action::Transact>(&action);
    const double requested = reduce ? reduce->units
        : transact ? std::abs(transact->signed_units) : 0.0;
    if (!std::isfinite(requested) || (reduce && requested < 0.0))
        return {Status::InvalidQuantity};

    if (position_side_ != PositionSide::FLAT
        && position_side_ != PositionSide::LONG
        && position_side_ != PositionSide::SHORT)
        return {Status::InvalidBook};
    if ((position_side_ == PositionSide::FLAT) != pyramid_entries_.empty())
        return {Status::InvalidBook};

    double held = 0.0;
    for (const auto& lot : pyramid_entries_) {
        if (!std::isfinite(lot.qty) || lot.qty <= 0.0 || !std::isfinite(lot.price))
            return {Status::InvalidBook};
        held += lot.qty;
        if (!std::isfinite(held)) return {Status::InvalidBook};
    }
    if (auto invalid = validate_lifecycle_effects(lifecycle))
        return {*invalid};
    const double signed_held = position_side_ == PositionSide::SHORT ? -held : held;
    if (!flatten && requested == 0.0) return no_effect();
    if ((flatten || reduce) && pyramid_entries_.empty()) return no_effect();

    // The scalar planner validates the request's representability. Physical
    // FIFO allocation below is authoritative for the surviving position: it
    // must not reject a valid decimal lot merely because a differently grouped
    // scalar addition has a one-ulp difference.
    if (reduce && !order_action::plan(signed_held, *reduce))
        return {Status::UnrepresentableQuantity};
    if (transact && !order_action::plan(signed_held, *transact))
        return {Status::UnrepresentableQuantity};

    const PositionSide incoming = transact && transact->signed_units < 0.0
        ? PositionSide::SHORT : PositionSide::LONG;
    const bool opposite = transact && position_side_ != PositionSide::FLAT
        && position_side_ != incoming;
    const bool closes = flatten || reduce || opposite;
    const bool was_long = position_side_ == PositionSide::LONG;

    // Stage all physical splits and financial rows before changing the book.
    std::vector<PyramidEntry> survivors;
    std::vector<size_t> closing_indices;
    std::vector<double> closing_quantities;
    if (!flatten) survivors.reserve(pyramid_entries_.size());
    closing_indices.reserve(closes ? pyramid_entries_.size() : 0);
    closing_quantities.reserve(closes ? pyramid_entries_.size() : 0);
    double closed = 0.0;
    double remaining = requested;
    for (size_t index = 0; index < pyramid_entries_.size(); ++index) {
        const auto& lot = pyramid_entries_[index];
        const double amount = !closes ? 0.0
            : flatten ? lot.qty : std::min(lot.qty, remaining);
        if (amount == 0.0) {
            survivors.push_back(lot);
            continue;
        }
        const double kept = lot.qty - amount;
        const double next_closed = closed + amount;
        if (!std::isfinite(next_closed) || kept == lot.qty
            || (!flatten && closed != 0.0
                && (next_closed == closed || next_closed == amount)))
            return {Status::UnrepresentableQuantity};
        if (!flatten) {
            const double next_remaining = requested - next_closed;
            if (next_remaining < 0.0 || next_remaining == remaining)
                return {Status::UnrepresentableQuantity};
            remaining = next_remaining;
        }
        closed = next_closed;
        closing_indices.push_back(index);
        closing_quantities.push_back(amount);
        if (kept > 0.0) {
            auto survivor = lot;
            const double scale = kept / lot.qty;
            survivor.qty = kept;
            survivor.max_runup *= scale;
            survivor.max_drawdown *= scale;
            survivor.entry_commission_account = open_entry_commission(lot)
                - allocated_entry_commission(lot, amount);
            survivors.push_back(std::move(survivor));
        }
    }

    const double opening = !transact ? 0.0 : opposite ? remaining : requested;
    if (opening > 0.0 && opposite && !survivors.empty())
        return {Status::UnrepresentableQuantity};
    const auto current_costs = quote_execution_commissions(closing_quantities, opening, fill);
    for (double cost : current_costs)
        if (!std::isfinite(cost)) return {Status::InvalidAccounting};
    const double opening_commission = current_costs.back();
    std::vector<Trade> closed_trades;
    closed_trades.reserve(closing_indices.size());
    for (size_t i = 0; i < closing_indices.size(); ++i) {
        const auto& lot = pyramid_entries_[closing_indices[i]];
        auto trade = build_close_trade_with_costs(lot, closing_quantities[i],
            fill.price, was_long, allocated_entry_commission(lot, closing_quantities[i]),
            current_costs[i]);
        trade.exit_id = fill.id;
        trade.exit_comment = fill.comment;
        closed_trades.push_back(std::move(trade));
    }
    if (opening > 0.0 && (position_side_ == PositionSide::FLAT || survivors.empty())
        && (next_position_cycle_seq_ <= 0
            || next_position_cycle_seq_ == std::numeric_limits<int64_t>::max()))
        throw std::overflow_error("position cycle sequence exhausted");
    double after_qty = 0.0;
    double weighted = 0.0;
    for (const auto& lot : survivors) {
        after_qty += lot.qty;
        weighted += lot.price * lot.qty;
    }
    if (opening > 0.0) {
        const double next = after_qty + opening;
        if (!std::isfinite(next) || (after_qty > 0.0
            && (next == after_qty || next == opening)))
            return {Status::UnrepresentableQuantity};
        after_qty = next;
        weighted += fill.price * opening;
    }
    const size_t after_lots = survivors.size() + (opening > 0.0 ? 1 : 0);
    const double after_price = after_lots == 1
        ? (opening > 0.0 ? fill.price : survivors.front().price)
        : after_qty > 0.0 ? weighted / after_qty : 0.0;
    if (!std::isfinite(after_qty) || !std::isfinite(after_price))
        return {Status::InvalidAccounting};

    // A financial sum must remain representable; reporting-only percentages
    // or excursion projections do not decide whether an action may settle.
    double next_profit = net_profit_sum_;
    double next_gross_profit = gross_profit_sum_;
    double next_gross_loss = gross_loss_sum_;
    double next_intraday = intraday_pnl_;
    for (const auto& trade : closed_trades) {
        if (!std::isfinite(trade.pnl) || !std::isfinite(trade.commission))
            return {Status::InvalidAccounting};
        next_profit += trade.pnl;
        next_intraday += trade.pnl;
        if (trade.pnl > 0.0) next_gross_profit += trade.pnl;
        if (trade.pnl < 0.0) next_gross_loss += trade.pnl;
    }
    if (!std::isfinite(next_profit) || !std::isfinite(next_gross_profit)
        || !std::isfinite(next_gross_loss) || !std::isfinite(next_intraday))
        return {Status::InvalidAccounting};
    validate_close_trade_counters(closed_trades.data(), closed_trades.size());
    if (opening > 0.0 && !survivors.empty()
        && position_entry_count_ == std::numeric_limits<int>::max())
        throw std::overflow_error("position entry counter exhausted");

    const bool will_reset = closed > 0.0 && survivors.empty();
    const bool will_open_quoted = opening > 0.0
        && (position_side_ == PositionSide::FLAT || survivors.empty());
    if (auto invalid = preflight_settlement_lifecycle(
            lifecycle, will_reset, will_open_quoted))
        return {*invalid};

    const size_t first_trade = trades_.size();
    const size_t first_action = stream_order_actions_.size();
    const size_t events = closed_trades.size() + (opening > 0.0 ? 1 : 0);
    if (stream_observe_actions_) {
        if (events > std::numeric_limits<uint64_t>::max() - stream_action_sequence_)
            throw std::overflow_error("stream action sequence overflow");
        reserve_effects(stream_order_actions_, events);
    }
    reserve_effects(trades_, closed_trades.size());

    // Commit through the existing accounting/observation sinks. Allocation or
    // lifecycle exceptions still abort the owning engine run; this internal
    // synchronous kernel does not promise recovery/replay of a failed commit.
    // Order: authorized pre-close events, close observations and old-cycle
    // unbind, authorized pending removals, then quoted opening bind.
    if (lifecycle.pre_close) apply_pre_close_lifecycle_batch(*lifecycle.pre_close);
    for (auto& trade : closed_trades) record_close_trade(std::move(trade));
    if (closed > 0.0) {
        if (survivors.empty()) {
            reset_position_state_to_flat();
        } else {
            pyramid_entries_ = std::move(survivors);
            position_qty_ = after_qty;
            position_entry_price_ = after_price;
            position_entry_count_ = static_cast<int>(pyramid_entries_.size());
        }
    }
    apply_authorized_pending_removals(lifecycle.removals);
    if (opening > 0.0) {
        PyramidEntry lot{fill.price, current_bar_.timestamp, opening, fill.id, bar_index_};
        lot.entry_incarnation = fill.incarnation;
        lot.entry_comment = fill.comment;
        lot.entry_commission_account = opening_commission;
        if (position_side_ == PositionSide::FLAT) {
            open_quoted_position(incoming, std::move(lot));
        } else {
            append_quoted_lot(std::move(lot), after_qty, after_price);
        }
    }
    if (stream_observe_actions_) stream_refresh_action_metadata(first_action, first_trade);
    return {Status::Applied, closed,
            incoming == PositionSide::SHORT ? -opening : opening};
}

std::vector<double> BacktestEngine::quote_execution_commissions(
        const std::vector<double>& closed_units, double opening_units,
        const execution::Fill& fill) const {
    // Unit/notional schedules are quoted at each physical allocation. A flat
    // ticket charge is shared across the entire execution, including a flip's
    // opening remainder. Row count must not multiply a per-order fee.
    std::vector<double> costs(closed_units.size() + 1, 0.0);
    const auto native_modeled_commission = [&](double quantity) {
        // Native resolved executions charge a positive percentage against
        // absolute resolved notional. Keep calc_commission's signed-price
        // behavior unchanged for all legacy Pine paths.
        if (commission_type_ == CommissionType::PERCENT) {
            return std::abs(fill.price) * quantity * syminfo_.pointvalue
                * active_account_currency_fx() * (commission_value_ / 100.0);
        }
        return calc_commission(fill.price, quantity);
    };
    if (!fill.commission_account && commission_type_ != CommissionType::CASH_PER_ORDER) {
        for (size_t i = 0; i < closed_units.size(); ++i)
            costs[i] = native_modeled_commission(closed_units[i]);
        costs.back() = opening_units > 0.0 ? native_modeled_commission(opening_units) : 0.0;
        return costs;
    }
    double units = opening_units;
    for (double quantity : closed_units) units += quantity;
    if (units == 0.0) return costs;
    if (!std::isfinite(units)) {
        costs.back() = std::numeric_limits<double>::quiet_NaN();
        return costs;
    }
    const double ticket = fill.commission_account
        ? *fill.commission_account : calc_commission(fill.price, units);
    if (!std::isfinite(ticket)) {
        costs.back() = std::numeric_limits<double>::quiet_NaN();
        return costs;
    }
    double remaining_fee = ticket;
    for (size_t i = 0; i < closed_units.size(); ++i) {
        double share = opening_units == 0.0 && i + 1 == closed_units.size()
            ? remaining_fee : ticket * (closed_units[i] / units);
        // Floating allocation must not invent a negative charge from a
        // positive ticket (or vice versa); the final effect owns the residue.
        share = ticket >= 0.0 ? std::clamp(share, 0.0, remaining_fee)
                              : std::clamp(share, remaining_fee, 0.0);
        remaining_fee -= share;
        costs[i] = share;
    }
    costs.back() = opening_units > 0.0 ? remaining_fee : 0.0;
    return costs;
}

double BacktestEngine::marked_equity(double price) const {
    if (!std::isfinite(price)
        || (position_side_ != PositionSide::FLAT && position_side_ != PositionSide::LONG
            && position_side_ != PositionSide::SHORT)
        || (position_side_ == PositionSide::FLAT) != pyramid_entries_.empty())
        return std::numeric_limits<double>::quiet_NaN();
    double equity = initial_capital_ + net_profit_sum_;
    const double direction = position_side_ == PositionSide::SHORT ? -1.0 : 1.0;
    for (const auto& lot : pyramid_entries_) {
        if (!std::isfinite(lot.qty) || lot.qty <= 0.0 || !std::isfinite(lot.price))
            return std::numeric_limits<double>::quiet_NaN();
        equity += direction * (price - lot.price) * lot.qty * syminfo_.pointvalue
            * active_account_currency_fx() - open_entry_commission(lot);
    }
    return equity;
}

} // namespace pineforge
