#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include <limits>

namespace pineforge::compat::pine {

std::optional<broker::OrderPriorityDecision> OrderPriority::select(
        const OrderPriorityContext& ctx,
        const std::vector<source::PendingOrder>& book) const {
    if (!attached_ || !retained_parent_first_
        || !ctx.broker_flat || !ctx.process_orders_on_close
        || ctx.calc_on_order_fills || ctx.coof_scheduler_active
        || ctx.bar_magnifier_enabled || ctx.stream_warmup_mode
        || !ctx.stream_idle || book.size() != 2) return std::nullopt;

    const source::PendingOrder* parent = nullptr;
    const source::PendingOrder* child = nullptr;
    for (const source::PendingOrder& order : book) {
        if (order.type == OrderType::ENTRY) parent = &order;
        else if (order.type == OrderType::EXIT) child = &order;
    }
    if (!parent || !child) return std::nullopt;

    // Preserve every legacy exclusion. Incarnation adjacency and exact book
    // size are Pine evidence boundaries, not native dependency invariants.
    const uint64_t cancelled_incarnation =
        parent->recreated_after_named_cancelled_entry_incarnation;
    const uint64_t surviving_exit_incarnation =
        parent->named_cancel_surviving_exit_incarnation;
    const bool parent_is_exact_fresh_stop =
        parent->type == OrderType::ENTRY
        && parent->created_position_side == PositionSide::FLAT
        && (parent->replaced_order_incarnation == 0)
        && cancelled_incarnation != 0
        && cancelled_incarnation < parent->incarnation
        && cancelled_incarnation != child->incarnation
        && surviving_exit_incarnation > cancelled_incarnation
        && surviving_exit_incarnation < parent->incarnation
        && parent->created_bar == ctx.bar_index - 1
        && std::isnan(parent->qty)
        && !parent->birth.from_fill()
        && !placement_has_prior_close(*parent)
        && !placement_at_entry_capacity(*parent)
        && !parent->stop_limit_activated
        && std::isfinite(parent->legs.prices().stop_price)
        && std::isnan(parent->legs.prices().limit_price)
        && std::isnan(parent->legs.prices().trail_points)
        && std::isnan(parent->legs.prices().trail_price)
        && std::isnan(parent->legs.prices().trail_offset)
        && parent->oca_name.empty()
        && parent->oca_type == 0;
    const double child_qp = std::isnan(child->qty_percent)
        ? 100.0 : child->qty_percent;
    const bool child_is_exact_retained_bracket =
        child->type == OrderType::EXIT
        && !child->from_entry.empty()
        && (child->replaced_order_incarnation != 0)
        && child->replaced_order_incarnation
            == surviving_exit_incarnation
        && child->created_position_side == PositionSide::FLAT
        && child->created_bar == ctx.bar_index - 1
        && !child->birth.from_fill()
        && !placement_has_prior_close(*child)
        && !child->quantity_request.is_partial(1e-9, 1e-9)
        && std::isnan(child->qty)
        && child_qp >= 100.0 - 1e-9
        && std::isfinite(child->legs.prices().stop_price)
        && std::isfinite(child->legs.prices().limit_price)
        && std::isnan(child->legs.prices().profit_ticks)
        && std::isnan(child->legs.prices().loss_ticks)
        && std::isnan(child->legs.prices().trail_points)
        && std::isnan(child->legs.prices().trail_price)
        && std::isnan(child->legs.prices().trail_offset)
        && child->oca_name.empty()
        && child->oca_type == 0;
    const bool exact_pair = parent_is_exact_fresh_stop
        && child_is_exact_retained_bracket
        && child->from_entry == parent->id
        && child->created_seq < parent->created_seq
        && child->incarnation != 0
        && parent->incarnation != 0
        && parent->incarnation
            < std::numeric_limits<uint64_t>::max()
        && child->incarnation == parent->incarnation + 1;
    if (!exact_pair) return std::nullopt;
    return broker::OrderPriorityDecision{{{
        {parent->incarnation, child->created_seq},
        {child->incarnation, parent->created_seq},
    }}};
}

} // namespace pineforge::compat::pine
