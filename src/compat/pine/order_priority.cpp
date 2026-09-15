#include <pineforge/compat/pine/order_priority.hpp>

#include <limits>

namespace pineforge::compat::pine {
namespace {

bool finite(double value) noexcept { return std::isfinite(value); }
bool absent(double value) noexcept { return std::isnan(value); }

} // namespace

std::optional<OrderPriorityDecision> OrderPriority::select(
        const OrderPriorityContext& ctx,
        const std::vector<OrderPriorityCandidate>& candidates) const {
    if (!attached_ || !retained_parent_first_ || !ctx.broker_flat
        || !ctx.process_orders_on_close || ctx.calc_on_order_fills
        || ctx.coof_scheduler_active || ctx.bar_magnifier_enabled
        || ctx.stream_warmup_mode || !ctx.stream_idle || candidates.size() != 2) {
        return std::nullopt;
    }

    const OrderPriorityCandidate* parent = nullptr;
    const OrderPriorityCandidate* child = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.kind == OrderPriorityKind::Entry) parent = &candidate;
        else if (candidate.kind == OrderPriorityKind::Exit) child = &candidate;
    }
    if (!parent || !child) return std::nullopt;

    const bool exact_parent = parent->created_flat && parent->predecessor == 0
        && parent->recreated_after_named_cancelled != 0
        && parent->recreated_after_named_cancelled < parent->handle.incarnation
        && parent->recreated_after_named_cancelled != child->handle.incarnation
        && parent->named_cancel_surviving_exit > parent->recreated_after_named_cancelled
        && parent->named_cancel_surviving_exit < parent->handle.incarnation
        && parent->created_bar == ctx.bar_index - 1
        && parent->default_quantity && !parent->birth_from_fill
        && !parent->prior_close && !parent->at_entry_capacity
        && !parent->stop_limit_activated && finite(parent->stop) && absent(parent->limit)
        && absent(parent->trail_points) && absent(parent->trail_price)
        && absent(parent->trail_offset) && parent->oca_name.empty() && parent->oca_type == 0;
    const double child_percent = absent(child->qty_percent) ? 100.0 : child->qty_percent;
    const bool exact_child = !child->from_entry.empty()
        && child->predecessor == parent->named_cancel_surviving_exit
        && child->created_flat && child->created_bar == ctx.bar_index - 1
        && !child->birth_from_fill && !child->prior_close && !child->at_entry_capacity
        && absent(child->requested_qty) && child_percent >= 100.0 - 1e-9
        && finite(child->stop) && finite(child->limit)
        && absent(child->profit_ticks) && absent(child->loss_ticks)
        && absent(child->trail_points) && absent(child->trail_price)
        && absent(child->trail_offset) && child->oca_name.empty() && child->oca_type == 0;
    if (!exact_parent || !exact_child || child->from_entry != parent->id
        || child->source_sequence >= parent->source_sequence
        || parent->handle.incarnation == std::numeric_limits<std::uint64_t>::max()
        || child->handle.incarnation != parent->handle.incarnation + 1) {
        return std::nullopt;
    }
    return OrderPriorityDecision{parent->handle, child->handle};
}

} // namespace pineforge::compat::pine
