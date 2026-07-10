/* Deterministic command/order lifecycle diagnostics. */

#include "engine_internal.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace pineforge {
namespace {

template <typename UInt>
void hash_unsigned(uint64_t& hash, UInt value) {
    static_assert(std::is_unsigned<UInt>::value, "unsigned hash input");
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        hash ^= static_cast<uint8_t>((value >> (i * 8)) & 0xffU);
        hash *= 1099511628211ULL;
    }
}

void hash_double(uint64_t& hash, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_unsigned(hash, bits);
}

void hash_string(uint64_t& hash, const std::string& value) {
    hash_unsigned(hash, static_cast<uint64_t>(value.size()));
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
}

double trail_activation_price(const PendingOrder& order,
                              PositionSide position_side,
                              double entry_price, double mintick) {
    if (!std::isnan(order.trail_points)) {
        const double direction = position_side == PositionSide::SHORT ? -1.0 : 1.0;
        return entry_price + direction * std::ceil(order.trail_points) * mintick;
    }
    return order.trail_price;
}

}  // namespace

OrderLifecycleState BacktestEngine::lifecycle_state(
        const PendingOrder& order) const {
    const bool has_trail = !std::isnan(order.trail_points)
        || !std::isnan(order.trail_price);
    if (has_trail) {
        return order.realtime_trail_activated
            ? OrderLifecycleState::ACTIVE_TRAIL
            : OrderLifecycleState::PENDING_TRAIL_ACTIVATION;
    }
    const bool has_stop = !std::isnan(order.stop_price);
    const bool has_limit = !std::isnan(order.limit_price);
    if (has_stop && has_limit) {
        return order.stop_limit_activated
            ? OrderLifecycleState::ACTIVE_STOP_LIMIT
            : OrderLifecycleState::PENDING_STOP_LIMIT;
    }
    if (has_stop) return OrderLifecycleState::PENDING_STOP;
    if (has_limit) return OrderLifecycleState::PENDING_LIMIT;
    return OrderLifecycleState::PENDING_MARKET;
}

void BacktestEngine::record_order_transition(
        const PendingOrder& order, OrderLifecycleState before,
        OrderLifecycleState after, OrderTransition transition,
        OrderTransitionReason reason, double filled_quantity,
        double fill_price, double position_before, double equity_before) {
    if (!order_event_recording_enabled_) return;
    OrderLifecycleEvent event;
    pf_order_event_t& v = event.value;
    v.transition_sequence = next_transition_sequence_++;
    v.command_revision_id = order.command_revision_id;
    v.order_leg_id = order.order_leg_id;
    v.priority_sequence = order.priority_sequence;
    v.fill_id = order.terminal_fill_id;
    v.entry_lot_id = 0;
    if (!pyramid_entries_.empty()
        && (order.type == OrderType::MARKET
            || order.type == OrderType::ENTRY
            || order.type == OrderType::RAW_ORDER)) {
        v.entry_lot_id = pyramid_entries_.back().entry_lot_id;
    }
    const double position_after = signed_position_size();
    if (std::isfinite(position_before)
        && std::abs(position_before) <= internal::kQtyEpsilon
        && std::abs(position_after) > internal::kQtyEpsilon) {
        ++position_episode_id_;
    }
    v.position_episode_id = position_episode_id_;
    v.event_timestamp = current_bar_.timestamp;
    v.event_sequence = stream_phase_ == StreamPhase::REALTIME
        ? stream_last_sequence_ : 0;
    v.input_bar_index = std::max<int64_t>(
        0, static_cast<int64_t>(diag_input_bars_processed_) - 1);
    v.script_bar_index = bar_index_;
    v.command_kind = static_cast<int32_t>(order.type) + 1;
    const bool has_trail = !std::isnan(order.trail_points)
        || !std::isnan(order.trail_price);
    const bool has_stop = !std::isnan(order.stop_price);
    const bool has_limit = !std::isnan(order.limit_price);
    v.leg_kind = has_trail ? 5 : (has_stop && has_limit ? 4
        : (has_stop ? 3 : (has_limit ? 2 : 1)));
    v.state_before = static_cast<int32_t>(before);
    v.state_after = static_cast<int32_t>(after);
    v.transition = static_cast<int32_t>(transition);
    v.reason = static_cast<int32_t>(reason);
    v.side = order.is_long ? 1 : -1;
    v.oca_type = order.oca_type;
    v.requested_quantity = order.qty;
    v.remaining_quantity = transition == OrderTransition::FILLED ? 0.0 : order.qty;
    v.filled_quantity = filled_quantity;
    v.observed_price = current_bar_.close;
    v.stop_price = order.stop_price;
    v.limit_price = order.limit_price;
    v.trail_activation_price = trail_activation_price(
        order, position_side_, position_entry_price_, syminfo_mintick_);
    v.trail_watermark = order.realtime_trail_best_price;
    v.fill_price = fill_price;
    v.position_size_before = position_before;
    v.position_size_after = position_after;
    v.equity_before = equity_before;
    v.equity_after = current_equity() + open_profit(current_bar_.close);
    event.id = order.id;
    event.from_entry = order.from_entry;
    event.oca_name = order.oca_name;

    uint64_t& hash = order_event_hash_;
#define PF_HASH_U(field) hash_unsigned(hash, static_cast<uint64_t>(v.field))
#define PF_HASH_D(field) hash_double(hash, v.field)
    PF_HASH_U(transition_sequence); PF_HASH_U(command_revision_id);
    PF_HASH_U(order_leg_id); PF_HASH_U(priority_sequence); PF_HASH_U(fill_id);
    PF_HASH_U(entry_lot_id); PF_HASH_U(position_episode_id);
    PF_HASH_U(event_timestamp); PF_HASH_U(event_sequence);
    PF_HASH_U(input_bar_index); PF_HASH_U(script_bar_index);
    PF_HASH_U(command_kind); PF_HASH_U(leg_kind); PF_HASH_U(state_before);
    PF_HASH_U(state_after); PF_HASH_U(transition); PF_HASH_U(reason);
    PF_HASH_U(side); PF_HASH_U(oca_type);
    PF_HASH_D(requested_quantity); PF_HASH_D(remaining_quantity);
    PF_HASH_D(filled_quantity); PF_HASH_D(observed_price); PF_HASH_D(stop_price);
    PF_HASH_D(limit_price); PF_HASH_D(trail_activation_price);
    PF_HASH_D(trail_watermark); PF_HASH_D(fill_price);
    PF_HASH_D(position_size_before); PF_HASH_D(position_size_after);
    PF_HASH_D(equity_before); PF_HASH_D(equity_after);
#undef PF_HASH_D
#undef PF_HASH_U
    hash_string(hash, event.id);
    hash_string(hash, event.from_entry);
    hash_string(hash, event.oca_name);

    ++order_event_count_;
    if (order_events_.size() < kOrderEventRetentionCapacity) {
        order_events_.push_back(std::move(event));
    } else {
        ++order_event_dropped_;
    }
}

}  // namespace pineforge
