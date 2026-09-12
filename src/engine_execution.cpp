#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
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

struct CloseScopeInspection {
    execution::Status status = execution::Status::Applied;
    double held = 0.0;
};

// Called only after ordinary price/quantity/book validation. This value is
// local to the synchronous call and grants no saved allocation authority.
CloseScopeInspection inspect_close_scope(
        const execution::CloseScope& scope, const execution::Action& action,
        int64_t current_cycle, const std::vector<PyramidEntry>& lots,
        double whole_book_held) {
    if (std::holds_alternative<execution::Book>(scope))
        return {execution::Status::Applied, whole_book_held};
    const auto* exposure = std::get_if<execution::OpeningExposure>(&scope);
    if (!exposure || exposure->incarnation == 0 || exposure->cycle <= 0
        || exposure->cycle != current_cycle
        || (!std::holds_alternative<execution::Flatten>(action)
            && !std::holds_alternative<order_action::Reduce>(action)))
        return {execution::Status::InvalidCloseTarget};
    double held = 0.0;
    for (const auto& lot : lots) {
        if (lot.entry_incarnation == exposure->incarnation) held += lot.qty;
    }
    if (held == 0.0) return {execution::Status::InvalidCloseTarget};
    if (!std::isfinite(held)) return {execution::Status::InvalidBook};
    return {execution::Status::Applied, held};
}

CloseScopeInspection inspect_selected_opening_set(
        const execution::SelectedOpeningSet& selection,
        const execution::Action& action,
        int64_t current_cycle, const std::vector<PyramidEntry>& lots,
        std::unordered_set<std::uint64_t>& ids_out) {
    ids_out.clear();
    if (std::holds_alternative<order_action::Transact>(action)
        || selection.cycle <= 0 || selection.cycle != current_cycle
        || selection.incarnations.empty())
        return {execution::Status::InvalidCloseTarget};
    std::unordered_set<std::uint64_t> live;
    live.reserve(lots.size());
    for (const auto& lot : lots) live.insert(lot.entry_incarnation);
    ids_out.reserve(selection.incarnations.size());
    for (const auto id : selection.incarnations) {
        if (id == 0 || !ids_out.insert(id).second
            || live.find(id) == live.end()) {
            ids_out.clear();
            return {execution::Status::InvalidCloseTarget};
        }
    }
    double held = 0.0;
    for (const auto& lot : lots) {
        if (ids_out.count(lot.entry_incarnation) == 0) continue;
        held += lot.qty;
        if (!std::isfinite(held)) {
            ids_out.clear();
            return {execution::Status::InvalidBook};
        }
    }
    if (held == 0.0) {
        ids_out.clear();
        return {execution::Status::InvalidCloseTarget};
    }
    return {execution::Status::Applied, held};
}

bool selected_for_close(const execution::CloseScope& scope,
                        const PyramidEntry& lot) {
    if (std::holds_alternative<execution::Book>(scope)) return true;
    const auto* exposure = std::get_if<execution::OpeningExposure>(&scope);
    return exposure && lot.entry_incarnation == exposure->incarnation;
}

struct CloseSplit {
    execution::Status status = execution::Status::Applied;
    double amount = 0.0;
    double kept = 0.0;
};

// Identical FIFO arithmetic for inspection and commit. Book callers pass
// their original request unchanged; selected reductions pass the request
// capped to selected exposure, independently of unrelated physical lots.
CloseSplit next_close_split(const PyramidEntry& lot, bool closes, bool flatten,
                            double requested, double& closed, double& remaining) {
    const double amount = !closes ? 0.0
        : flatten ? lot.qty : std::min(lot.qty, remaining);
    if (amount == 0.0) return {execution::Status::Applied, 0.0, lot.qty};
    const double kept = lot.qty - amount;
    const double next_closed = closed + amount;
    if (!std::isfinite(next_closed) || kept == lot.qty
        || (!flatten && closed != 0.0
            && (next_closed == closed || next_closed == amount)))
        return {execution::Status::UnrepresentableQuantity};
    if (!flatten) {
        const double next_remaining = requested - next_closed;
        if (next_remaining < 0.0 || next_remaining == remaining)
            return {execution::Status::UnrepresentableQuantity};
        remaining = next_remaining;
    }
    closed = next_closed;
    return {execution::Status::Applied, amount, kept};
}

execution::AccountEffectProjection invalid_projection(execution::Status status) {
    execution::AccountEffectProjection out;
    out.status = status;
    return out;
}
} // namespace

struct BacktestEngine::NativeSettlementStage {
    enum class Phase { Invalid, NoEffect, Ready };
    Phase phase = Phase::Invalid;
    execution::Status status = execution::Status::NoEffect;
    bool flatten = false;
    bool scoped = false;
    bool opposite = false;
    bool closes = false;
    bool use_selected = false;
    PositionSide incoming = PositionSide::LONG;
    bool was_long = false;
    double requested = 0.0;
    double selected_held = 0.0;
    double allocation_requested = 0.0;
    double closed = 0.0;
    double opening = 0.0;
    double ticket = 0.0;
    double after_qty = 0.0;
    double after_price = 0.0;
    std::vector<std::size_t> closing_indices;
    std::vector<double> closing_quantities;
    std::vector<double> current_costs;
    std::vector<PyramidEntry> survivors;
    std::unordered_set<std::uint64_t> selected_ids;
};

void BacktestEngine::stage_native_settlement(
        NativeSettlementStage& stage,
        const execution::Action& action,
        const execution::Fill& fill,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected,
        const execution::LifecycleEffects* lifecycle) const {
    using execution::Status;
    stage = NativeSettlementStage{};
    auto fail = [&](Status status) {
        stage.phase = NativeSettlementStage::Phase::Invalid;
        stage.status = status;
    };
    auto no_effect = [&]() {
        stage.phase = NativeSettlementStage::Phase::NoEffect;
        stage.status = fill.commission_account && *fill.commission_account != 0.0
            ? Status::InvalidAccounting : Status::NoEffect;
    };

    if (!std::isfinite(fill.price)) { fail(Status::InvalidPrice); return; }
    if (fill.commission_account && !std::isfinite(*fill.commission_account)) {
        fail(Status::InvalidAccounting);
        return;
    }

    stage.flatten = std::holds_alternative<execution::Flatten>(action);
    const auto* reduce = std::get_if<order_action::Reduce>(&action);
    const auto* transact = std::get_if<order_action::Transact>(&action);
    stage.requested = reduce ? reduce->units
        : transact ? std::abs(transact->signed_units) : 0.0;
    if (!std::isfinite(stage.requested) || (reduce && stage.requested < 0.0)) {
        fail(Status::InvalidQuantity);
        return;
    }
    if (position_side_ != PositionSide::FLAT
        && position_side_ != PositionSide::LONG
        && position_side_ != PositionSide::SHORT) {
        fail(Status::InvalidBook);
        return;
    }
    if ((position_side_ == PositionSide::FLAT) != pyramid_entries_.empty()) {
        fail(Status::InvalidBook);
        return;
    }

    double held = 0.0;
    for (const auto& lot : pyramid_entries_) {
        if (!std::isfinite(lot.qty) || lot.qty <= 0.0 || !std::isfinite(lot.price)) {
            fail(Status::InvalidBook);
            return;
        }
        held += lot.qty;
        if (!std::isfinite(held)) {
            fail(Status::InvalidBook);
            return;
        }
    }
    if (lifecycle) {
        if (auto invalid = validate_lifecycle_effects(*lifecycle)) {
            fail(*invalid);
            return;
        }
    }

    CloseScopeInspection selection;
    if (selected) {
        selection = inspect_selected_opening_set(
            *selected, action, position_cycle_seq_, pyramid_entries_,
            stage.selected_ids);
        stage.use_selected = true;
        stage.scoped = true;
    } else {
        selection = inspect_close_scope(
            book_or_opening, action, position_cycle_seq_, pyramid_entries_, held);
        stage.scoped = std::holds_alternative<execution::OpeningExposure>(
            book_or_opening);
    }
    if (selection.status != Status::Applied) {
        fail(selection.status);
        return;
    }
    stage.selected_held = selection.held;
    stage.allocation_requested = stage.scoped && reduce
        ? std::min(stage.requested, selection.held) : stage.requested;
    const double signed_held = position_side_ == PositionSide::SHORT
        ? -selection.held : selection.held;
    if (!stage.flatten && stage.requested == 0.0) { no_effect(); return; }
    if ((stage.flatten || reduce) && pyramid_entries_.empty()) {
        no_effect();
        return;
    }
    if (reduce && !order_action::plan(signed_held,
            order_action::Reduce{stage.allocation_requested})) {
        fail(Status::UnrepresentableQuantity);
        return;
    }
    if (transact && !order_action::plan(signed_held, *transact)) {
        fail(Status::UnrepresentableQuantity);
        return;
    }

    stage.incoming = transact && transact->signed_units < 0.0
        ? PositionSide::SHORT : PositionSide::LONG;
    stage.opposite = transact && position_side_ != PositionSide::FLAT
        && position_side_ != stage.incoming;
    stage.closes = stage.flatten || reduce || stage.opposite;
    stage.was_long = position_side_ == PositionSide::LONG;

    if (!stage.flatten || stage.scoped)
        stage.survivors.reserve(pyramid_entries_.size());
    stage.closing_indices.reserve(stage.closes ? pyramid_entries_.size() : 0);
    stage.closing_quantities.reserve(stage.closes ? pyramid_entries_.size() : 0);
    double closed = 0.0;
    double remaining = stage.allocation_requested;
    for (size_t index = 0; index < pyramid_entries_.size(); ++index) {
        const auto& lot = pyramid_entries_[index];
        const bool member = stage.use_selected
            ? stage.selected_ids.count(lot.entry_incarnation) != 0
            : selected_for_close(book_or_opening, lot);
        const auto split = next_close_split(
            lot, stage.closes && member, stage.flatten,
            stage.allocation_requested, closed, remaining);
        if (split.status != Status::Applied) {
            fail(split.status);
            return;
        }
        if (split.amount == 0.0) {
            stage.survivors.push_back(lot);
            continue;
        }
        stage.closing_indices.push_back(index);
        stage.closing_quantities.push_back(split.amount);
        if (split.kept > 0.0) {
            auto survivor = lot;
            const double scale = split.kept / lot.qty;
            survivor.qty = split.kept;
            survivor.max_runup *= scale;
            survivor.max_drawdown *= scale;
            survivor.entry_commission_account = open_entry_commission(lot)
                - allocated_entry_commission(lot, split.amount);
            stage.survivors.push_back(std::move(survivor));
        }
    }
    stage.closed = closed;
    stage.opening = !transact ? 0.0 : stage.opposite ? remaining : stage.requested;
    if (stage.opening > 0.0 && stage.opposite && !stage.survivors.empty()) {
        fail(Status::UnrepresentableQuantity);
        return;
    }
    stage.current_costs = quote_execution_commissions(
        stage.closing_quantities, stage.opening, fill);
    stage.ticket = 0.0;
    for (double cost : stage.current_costs) {
        if (!std::isfinite(cost)) {
            fail(Status::InvalidAccounting);
            return;
        }
        stage.ticket += cost;
        if (!std::isfinite(stage.ticket)) {
            fail(Status::InvalidAccounting);
            return;
        }
    }
    double after_qty = 0.0;
    double weighted = 0.0;
    for (const auto& lot : stage.survivors) {
        after_qty += lot.qty;
        weighted += lot.price * lot.qty;
    }
    if (stage.opening > 0.0) {
        const double next = after_qty + stage.opening;
        if (!std::isfinite(next) || (after_qty > 0.0
            && (next == after_qty || next == stage.opening))) {
            fail(Status::UnrepresentableQuantity);
            return;
        }
        after_qty = next;
        weighted += fill.price * stage.opening;
    }
    const size_t after_lots = stage.survivors.size() + (stage.opening > 0.0 ? 1 : 0);
    stage.after_qty = after_qty;
    stage.after_price = after_lots == 1
        ? (stage.opening > 0.0 ? fill.price : stage.survivors.front().price)
        : after_qty > 0.0 ? weighted / after_qty : 0.0;
    stage.phase = NativeSettlementStage::Phase::Ready;
    stage.status = Status::Applied;
}

execution::Result BacktestEngine::settle_resolved_execution(
        const execution::Action& action, const execution::Fill& fill) {
    return settle_execution_with_lifecycle(action, fill, {});
}

execution::Result BacktestEngine::settle_execution_with_lifecycle(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle) {
    execution::PhysicalExecutionContext context;
    context.effective_time_ms = current_bar_.timestamp;
    context.interval_index = bar_index_;
    context.preceding_exit_path_prefix = fold_exit_path_extremes_;
    if (!std::isnan(fold_exit_trail_peak_)) {
        context.preceding_exit_trail_peak = fold_exit_trail_peak_;
    }
    return settle_with_context(action, fill, lifecycle, context);
}

execution::Result BacktestEngine::settle_native_execution_at(
        const execution::Action& action, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context) {
    return settle_native_execution_scoped_at(action, fill, context, execution::Book{});
}

execution::Result BacktestEngine::settle_native_execution_scoped_at(
        const execution::Action& action, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        execution::CloseScope scope) {
    return settle_with_context_scoped(action, fill, {}, context, scope);
}

execution::Result BacktestEngine::settle_with_context(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context) {
    return settle_with_context_scoped(action, fill, lifecycle, context, execution::Book{});
}

execution::Result BacktestEngine::settle_native_execution_selected_at(
        const execution::Action& action, const execution::Fill& fill,
        const execution::PhysicalExecutionContext& context,
        const execution::SelectedOpeningSet& selection) {
    return settle_with_context_selected(action, fill, {}, context, selection);
}

execution::Result BacktestEngine::settle_execution_selected_with_lifecycle(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::SelectedOpeningSet& selection) {
    execution::PhysicalExecutionContext context;
    context.effective_time_ms = current_bar_.timestamp;
    context.interval_index = bar_index_;
    context.preceding_exit_path_prefix = fold_exit_path_extremes_;
    if (!std::isnan(fold_exit_trail_peak_)) {
        context.preceding_exit_trail_peak = fold_exit_trail_peak_;
    }
    return settle_with_context_selected(action, fill, lifecycle, context, selection);
}

execution::Result BacktestEngine::settle_with_context_scoped(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context,
        execution::CloseScope scope) {
    return settle_with_membership(action, fill, lifecycle, context, scope, nullptr);
}

execution::Result BacktestEngine::settle_with_context_selected(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context,
        const execution::SelectedOpeningSet& selection) {
    return settle_with_membership(
        action, fill, lifecycle, context, execution::Book{}, &selection);
}

execution::Result BacktestEngine::settle_with_membership(
        const execution::Action& action, const execution::Fill& fill,
        const execution::LifecycleEffects& lifecycle,
        const execution::PhysicalExecutionContext& context,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected) {
    using execution::Status;
    NativeSettlementStage stage;
    stage_native_settlement(
        stage, action, fill, book_or_opening, selected, &lifecycle);
    if (stage.phase != NativeSettlementStage::Phase::Ready)
        return {stage.status};
    const auto no_effect = [&]() -> execution::Result {
        return {fill.commission_account && *fill.commission_account != 0.0
            ? Status::InvalidAccounting : Status::NoEffect};
    };
    if (stage.scoped && stage.closed == 0.0) return no_effect();

    std::vector<Trade> closed_trades;
    closed_trades.reserve(stage.closing_indices.size());
    for (size_t i = 0; i < stage.closing_indices.size(); ++i) {
        const auto& lot = pyramid_entries_[stage.closing_indices[i]];
        auto trade = build_close_trade_with_costs(lot, stage.closing_quantities[i],
            fill.price, stage.was_long,
            allocated_entry_commission(lot, stage.closing_quantities[i]),
            stage.current_costs[i], context);
        trade.exit_id = fill.id;
        trade.exit_comment = fill.comment;
        closed_trades.push_back(std::move(trade));
    }
    if (stage.opening > 0.0
        && (position_side_ == PositionSide::FLAT || stage.survivors.empty())
        && (next_position_cycle_seq_ <= 0
            || next_position_cycle_seq_ == std::numeric_limits<int64_t>::max()))
        throw std::overflow_error("position cycle sequence exhausted");
    if (!std::isfinite(stage.after_qty) || !std::isfinite(stage.after_price))
        return {Status::InvalidAccounting};

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
    if (stage.opening > 0.0 && !stage.survivors.empty()
        && position_entry_count_ == std::numeric_limits<int>::max())
        throw std::overflow_error("position entry counter exhausted");

    const bool will_reset = stage.closed > 0.0 && stage.survivors.empty();
    const bool will_open_quoted = stage.opening > 0.0
        && (position_side_ == PositionSide::FLAT || stage.survivors.empty());
    if (auto invalid = preflight_settlement_lifecycle(
            lifecycle, will_reset, will_open_quoted))
        return {*invalid};

    const size_t first_trade = trades_.size();
    const size_t first_action = stream_order_actions_.size();
    const size_t events = closed_trades.size() + (stage.opening > 0.0 ? 1 : 0);
    if (stream_observe_actions_) {
        if (events > std::numeric_limits<uint64_t>::max() - stream_action_sequence_)
            throw std::overflow_error("stream action sequence overflow");
        reserve_effects(stream_order_actions_, events);
    }
    reserve_effects(trades_, closed_trades.size());
    if (stage.opening > 0.0) reserve_effects(pyramid_entries_, 1);

    // Commit through the existing accounting/observation sinks. Allocation or
    // lifecycle exceptions still abort the owning engine run; this internal
    // synchronous kernel does not promise recovery/replay of a failed commit.
    // Order: authorized pre-close events, close observations and old-cycle
    // unbind, authorized pending removals, then quoted opening bind.
    if (lifecycle.pre_close) apply_pre_close_lifecycle_batch(*lifecycle.pre_close);
    for (auto& trade : closed_trades) record_close_trade(std::move(trade));
    if (stage.closed > 0.0) {
        if (stage.survivors.empty()) {
            reset_position_state_to_flat();
        } else {
            pyramid_entries_ = std::move(stage.survivors);
            position_qty_ = stage.after_qty;
            position_entry_price_ = stage.after_price;
            position_entry_count_ = static_cast<int>(pyramid_entries_.size());
        }
    }
    apply_authorized_pending_removals(lifecycle.removals);
    if (stage.opening > 0.0) {
        const double opening_commission = stage.current_costs.back();
        PyramidEntry lot{fill.price, context.effective_time_ms, stage.opening, fill.id,
                         context.interval_index};
        lot.entry_incarnation = fill.incarnation;
        lot.entry_comment = fill.comment;
        lot.entry_commission_account = opening_commission;
        if (position_side_ == PositionSide::FLAT) {
            open_quoted_position(stage.incoming, std::move(lot));
        } else {
            append_quoted_lot(std::move(lot), stage.after_qty, stage.after_price);
        }
    }
    if (stream_observe_actions_) stream_refresh_action_metadata(first_action, first_trade);
    execution::Result applied;
    applied.status = Status::Applied;
    applied.closed_units = stage.closed;
    applied.opened_units = stage.incoming == PositionSide::SHORT
        ? -stage.opening : stage.opening;
    applied.current_ticket = stage.ticket;
    applied.first_trade_index = first_trade;
    applied.closed_trade_count = closed_trades.size();
    applied.opened_lot_incarnation = stage.opening > 0.0 ? fill.incarnation : 0;
    return applied;
}

execution::SettlementInspection BacktestEngine::inspect_native_settlement(
        const execution::Action& action, const execution::Fill& fill) const {
    return inspect_native_settlement_scoped(action, fill, execution::Book{});
}

execution::SettlementInspection BacktestEngine::inspect_native_settlement_scoped(
        const execution::Action& action, const execution::Fill& fill,
        execution::CloseScope scope) const {
    return inspect_with_membership(action, fill, scope, nullptr);
}

execution::SettlementInspection BacktestEngine::inspect_native_settlement_selected(
        const execution::Action& action, const execution::Fill& fill,
        const execution::SelectedOpeningSet& selection) const {
    return inspect_with_membership(action, fill, execution::Book{}, &selection);
}

execution::SettlementInspection BacktestEngine::inspect_with_membership(
        const execution::Action& action, const execution::Fill& fill,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected) const {
    using execution::Status;
    NativeSettlementStage stage;
    stage_native_settlement(
        stage, action, fill, book_or_opening, selected, nullptr);
    execution::SettlementInspection out;
    if (stage.phase != NativeSettlementStage::Phase::Ready) {
        out.status = stage.status;
        return out;
    }
    const double fx = active_account_currency_fx();
    out.status = Status::Applied;
    out.closed_units = stage.closed;
    out.opened_units = stage.incoming == PositionSide::SHORT
        ? -stage.opening : stage.opening;
    out.resulting_abs_units = stage.after_qty;
    out.resulting_lot_count = stage.survivors.size() + (stage.opening > 0.0 ? 1 : 0);
    out.resulting_abs_notional = stage.after_qty * std::abs(fill.price)
        * syminfo_.pointvalue * fx;
    out.current_ticket = stage.ticket;
    out.would_open = stage.opening > 0.0;
    out.incoming_short = stage.incoming == PositionSide::SHORT;
    if (stage.closed == 0.0 && stage.opening == 0.0) {
        out.status = fill.commission_account && *fill.commission_account != 0.0
            ? Status::InvalidAccounting : Status::NoEffect;
        out.would_open = false;
    }
    return out;
}

execution::AccountEffectProjection BacktestEngine::project_native_settlement_v1(
        const execution::Action& action, const execution::Fill& fill) const {
    return project_native_settlement_scoped_v1(action, fill, execution::Book{});
}

execution::AccountEffectProjection BacktestEngine::project_native_settlement_scoped_v1(
        const execution::Action& action, const execution::Fill& fill,
        execution::CloseScope scope) const {
    return project_with_membership(action, fill, scope, nullptr);
}

execution::AccountEffectProjection BacktestEngine::project_native_settlement_selected_v1(
        const execution::Action& action, const execution::Fill& fill,
        const execution::SelectedOpeningSet& selection) const {
    return project_with_membership(action, fill, execution::Book{}, &selection);
}

execution::AccountEffectProjection BacktestEngine::project_with_membership(
        const execution::Action& action, const execution::Fill& fill,
        execution::CloseScope book_or_opening,
        const execution::SelectedOpeningSet* selected) const {
    using execution::Status;
    NativeSettlementStage stage;
    stage_native_settlement(
        stage, action, fill, book_or_opening, selected, nullptr);
    if (stage.phase == NativeSettlementStage::Phase::Invalid)
        return invalid_projection(stage.status);

    const auto unchanged_account = [&]() -> execution::AccountEffectProjection {
        execution::AccountEffectProjection out;
        out.status = Status::NoEffect;
        const double fx = active_account_currency_fx();
        const double pv = syminfo_.pointvalue;
        double abs_units = 0.0;
        for (const auto& lot : pyramid_entries_) abs_units += lot.qty;
        out.resulting_abs_units = abs_units;
        out.resulting_lot_count = pyramid_entries_.size();
        out.resulting_abs_notional =
            abs_units * std::abs(fill.price) * pv * fx;
        double remaining = 0.0;
        for (const auto& lot : pyramid_entries_)
            remaining += open_entry_commission(lot);
        out.realized_balance = initial_capital_ + net_profit_sum_;
        out.remaining_entry_cost = remaining;
        out.marked_equity = marked_equity(fill.price);
        out.cycle_after = pyramid_entries_.empty() ? 0 : position_cycle_seq_;
        out.signed_units_after = position_side_ == PositionSide::SHORT
            ? -abs_units
            : position_side_ == PositionSide::LONG ? abs_units : 0.0;
        if (!std::isfinite(out.resulting_abs_units)
            || !std::isfinite(out.resulting_abs_notional)
            || !std::isfinite(out.realized_balance)
            || !std::isfinite(out.remaining_entry_cost)
            || !std::isfinite(out.marked_equity)
            || !std::isfinite(out.signed_units_after))
            return invalid_projection(Status::InvalidAccounting);
        return out;
    };

    if (stage.phase == NativeSettlementStage::Phase::NoEffect) {
        if (stage.status != Status::NoEffect)
            return invalid_projection(stage.status);
        return unchanged_account();
    }
    if (stage.closed == 0.0 && stage.opening == 0.0) {
        if (fill.commission_account && *fill.commission_account != 0.0)
            return invalid_projection(Status::InvalidAccounting);
        return unchanged_account();
    }

    if (stage.opening > 0.0
        && (position_side_ == PositionSide::FLAT || stage.survivors.empty())
        && (next_position_cycle_seq_ <= 0
            || next_position_cycle_seq_ == std::numeric_limits<int64_t>::max()))
        throw std::overflow_error("position cycle sequence exhausted");

    execution::PhysicalExecutionContext context;
    std::vector<Trade> closed_trades;
    closed_trades.reserve(stage.closing_indices.size());
    for (size_t i = 0; i < stage.closing_indices.size(); ++i) {
        const auto& lot = pyramid_entries_[stage.closing_indices[i]];
        auto trade = build_close_trade_with_costs(lot, stage.closing_quantities[i],
            fill.price, stage.was_long,
            allocated_entry_commission(lot, stage.closing_quantities[i]),
            stage.current_costs[i], context);
        closed_trades.push_back(std::move(trade));
    }

    double realized = net_profit_sum_;
    for (const auto& trade : closed_trades) {
        if (!std::isfinite(trade.pnl) || !std::isfinite(trade.commission))
            return invalid_projection(Status::InvalidAccounting);
        realized += trade.pnl;
    }
    realized += initial_capital_;
    if (!std::isfinite(realized))
        return invalid_projection(Status::InvalidAccounting);

    const double fx = active_account_currency_fx();
    const double pv = syminfo_.pointvalue;
    const PositionSide resulting_side = !stage.survivors.empty()
        ? position_side_
        : stage.opening > 0.0 ? stage.incoming : PositionSide::FLAT;
    const double direction = resulting_side == PositionSide::SHORT ? -1.0 : 1.0;
    double remaining = 0.0;
    double equity = realized;
    for (const auto& lot : stage.survivors) {
        if (!std::isfinite(lot.qty) || lot.qty <= 0.0 || !std::isfinite(lot.price))
            return invalid_projection(Status::InvalidAccounting);
        const double paid = open_entry_commission(lot);
        remaining += paid;
        equity += direction * (fill.price - lot.price) * lot.qty * pv * fx - paid;
    }
    if (stage.opening > 0.0) {
        const double paid = stage.current_costs.back();
        remaining += paid;
        equity += direction * (fill.price - fill.price) * stage.opening * pv * fx
            - paid;
    }
    if (!std::isfinite(remaining) || !std::isfinite(equity)
        || !std::isfinite(stage.after_qty)
        || !std::isfinite(stage.ticket))
        return invalid_projection(Status::InvalidAccounting);

    execution::AccountEffectProjection out;
    out.status = Status::Applied;
    out.closed_units = stage.closed;
    out.opened_units = stage.incoming == PositionSide::SHORT
        ? -stage.opening : stage.opening;
    out.resulting_abs_units = stage.after_qty;
    out.resulting_lot_count = stage.survivors.size() + (stage.opening > 0.0 ? 1 : 0);
    out.resulting_abs_notional =
        stage.after_qty * std::abs(fill.price) * pv * fx;
    out.current_ticket = stage.ticket;
    out.would_open = stage.opening > 0.0;
    out.incoming_short = stage.incoming == PositionSide::SHORT;
    out.realized_balance = realized;
    out.remaining_entry_cost = remaining;
    out.marked_equity = equity;
    if (out.resulting_lot_count == 0) {
        out.cycle_after = 0;
        out.signed_units_after = 0.0;
    } else if (!stage.survivors.empty()) {
        out.cycle_after = position_cycle_seq_;
        out.signed_units_after = position_side_ == PositionSide::SHORT
            ? -stage.after_qty : stage.after_qty;
    } else {
        out.cycle_after = next_position_cycle_seq_;
        out.signed_units_after = stage.incoming == PositionSide::SHORT
            ? -stage.after_qty : stage.after_qty;
    }
    if (!std::isfinite(out.resulting_abs_notional)
        || !std::isfinite(out.signed_units_after))
        return invalid_projection(Status::InvalidAccounting);
    return out;
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
