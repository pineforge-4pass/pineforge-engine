#include "engine_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge {
namespace {

bool valid_lifecycle_phase(exit_legs::Phase phase) {
    return static_cast<unsigned>(phase)
        <= static_cast<unsigned>(exit_legs::Phase::AfterMargin);
}

bool same_target(exit_legs::Target a, exit_legs::Target b) {
    return a.incarnation == b.incarnation && a.owner == b.owner;
}

struct PendingLegCopy {
    uint64_t incarnation = 0;
    int64_t created_seq = 0;
    OrderType type = OrderType::EXIT;
    exit_legs::Lifecycle legs;
    bool removed = false;
};

PendingLegCopy* find_leg_copy(std::vector<PendingLegCopy>& copies,
                              uint64_t incarnation, int64_t created_seq) {
    PendingLegCopy* found = nullptr;
    for (auto& copy : copies) {
        if (copy.removed) continue;
        if (copy.incarnation != incarnation || copy.created_seq != created_seq)
            continue;
        if (found) return nullptr;
        found = &copy;
    }
    return found;
}

struct IdentityKey {
    uint64_t incarnation = 0;
    int64_t created_seq = 0;
    bool operator==(const IdentityKey& other) const {
        return incarnation == other.incarnation && created_seq == other.created_seq;
    }
};

struct IdentityKeyHash {
    size_t operator()(const IdentityKey& key) const {
        return std::hash<uint64_t>{}(key.incarnation)
            ^ (std::hash<int64_t>{}(key.created_seq) << 1);
    }
};

} // namespace

exit_legs::Domain BacktestEngine::current_exit_leg_domain() const {
    if (stream_phase_ != StreamPhase::IDLE) return exit_legs::Domain::RawTicks;
    if (bar_magnifier_enabled_) {
        return coof_scheduler_active_ ? exit_legs::Domain::MagnifierCoof
                                      : exit_legs::Domain::Magnifier;
    }
    return coof_scheduler_active_ ? exit_legs::Domain::Coof
                                  : exit_legs::Domain::Ordinary;
}

exit_legs::Frame BacktestEngine::preview_next_leg_event(exit_legs::Phase phase) const {
    if (exit_leg_event_seq_ == UINT64_MAX)
        throw std::overflow_error("exit lifecycle event exhausted");
    return {exit_leg_event_seq_ + 1, bar_index_, current_exit_leg_domain(), phase};
}

const PendingOrder* BacktestEngine::find_unique_pending(
        uint64_t incarnation, int64_t created_seq) const {
    const PendingOrder* found = nullptr;
    for (const auto& order : pending_orders_) {
        if (order.incarnation != incarnation || order.created_seq != created_seq)
            continue;
        if (found) return nullptr;
        found = &order;
    }
    return found;
}

PendingOrder* BacktestEngine::find_unique_pending(
        uint64_t incarnation, int64_t created_seq) {
    return const_cast<PendingOrder*>(
        static_cast<const BacktestEngine*>(this)->find_unique_pending(
            incarnation, created_seq));
}

BacktestEngine::ExitLegTransitionResult BacktestEngine::transition_exit_leg(
        exit_legs::Lifecycle& legs, uint64_t order_incarnation,
        exit_legs::Operation operation, std::optional<exit_legs::Frame> supplied,
        uint64_t& event_seq, int64_t position_cycle) const {
    const auto receipt_phase = supplied ? supplied->phase
                                       : exit_legs::Phase::Observation;
    const auto mint = [&](exit_legs::Phase phase) -> std::optional<exit_legs::Frame> {
        if (event_seq == UINT64_MAX) return std::nullopt;
        return exit_legs::Frame{
            ++event_seq, bar_index_, current_exit_leg_domain(), phase};
    };
    if (!legs.target().incarnation)
        legs.attach(order_incarnation, position_cycle);
    if (legs.last_action()) {
        event_seq = std::max(event_seq, legs.last_action()->cause.event);
        if (supplied && supplied->event <= legs.last_action()->cause.event)
            supplied.reset();
    }
    if (legs.target().incarnation != order_incarnation)
        return ExitLegTransitionResult::StaleIdentity;
    if (legs.target().owner != position_cycle) {
        const auto bind_cause = mint(receipt_phase);
        if (!bind_cause) return ExitLegTransitionResult::Exhausted;
        const exit_legs::Action bind{legs.target(), legs.revision(), *bind_cause,
                                    exit_legs::BindOwner{position_cycle}};
        if (legs.apply(legs.target(), bind) != exit_legs::Result::Applied)
            return ExitLegTransitionResult::BindRefused;
        supplied.reset();
    }
    const auto cause = supplied ? supplied : mint(receipt_phase);
    if (!cause) return ExitLegTransitionResult::Exhausted;
    const exit_legs::Action action{
        legs.target(), legs.revision(), *cause, std::move(operation)};
    const auto result = legs.apply({order_incarnation, position_cycle}, action);
    if (result == exit_legs::Result::Applied) return ExitLegTransitionResult::Applied;
    if (result == exit_legs::Result::Replay) return ExitLegTransitionResult::Replay;
    if (result == exit_legs::Result::Exhausted)
        return ExitLegTransitionResult::RevisionExhausted;
    return ExitLegTransitionResult::ActionRefused;
}

std::optional<execution::Status> BacktestEngine::validate_lifecycle_effects(
        const execution::LifecycleEffects& lifecycle) const {
    if (!lifecycle.pre_close && lifecycle.removals.empty()) return std::nullopt;
    std::unordered_set<IdentityKey, IdentityKeyHash> seen_intents;
    if (lifecycle.pre_close) {
        if (!valid_lifecycle_phase(lifecycle.pre_close->phase))
            return execution::Status::InvalidLifecycle;
        for (const auto& intent : lifecycle.pre_close->operations) {
            const IdentityKey key{intent.order_incarnation, intent.created_seq};
            if (!seen_intents.insert(key).second)
                return execution::Status::InvalidLifecycle;
            if (const auto* bind = std::get_if<exit_legs::BindOwner>(&intent.operation)) {
                if (bind->owner != position_cycle_seq_)
                    return execution::Status::InvalidLifecycle;
            }
            const PendingOrder* order = find_unique_pending(
                intent.order_incarnation, intent.created_seq);
            if (!order) return execution::Status::InvalidLifecycle;
            if (!same_target(order->legs.target(), intent.target)
                || order->legs.revision() != intent.expected_revision)
                return execution::Status::InvalidLifecycle;
        }
    }
    std::unordered_set<IdentityKey, IdentityKeyHash> seen_removals;
    for (const auto& removal : lifecycle.removals) {
        if (removal.incarnation == 0) return execution::Status::InvalidLifecycle;
        const IdentityKey key{removal.incarnation, removal.created_seq};
        if (!seen_removals.insert(key).second)
            return execution::Status::InvalidLifecycle;
        const PendingOrder* order = find_unique_pending(
            removal.incarnation, removal.created_seq);
        if (!order || order->type != OrderType::EXIT)
            return execution::Status::InvalidLifecycle;
        if (!same_target(order->legs.target(), removal.target)
            || order->legs.revision() != removal.expected_revision)
            return execution::Status::InvalidLifecycle;
    }
    return std::nullopt;
}

std::optional<execution::Status> BacktestEngine::preflight_settlement_lifecycle(
        const execution::LifecycleEffects& lifecycle,
        bool will_reset_to_flat, bool will_open_quoted) {
    if (!lifecycle.pre_close && !will_reset_to_flat && !will_open_quoted)
        return std::nullopt;

    std::vector<PendingLegCopy> copies;
    copies.reserve(pending_orders_.size());
    for (const auto& order : pending_orders_) {
        copies.push_back({order.incarnation, order.created_seq, order.type,
                          order.legs, false});
    }
    uint64_t seq = exit_leg_event_seq_;
    const int64_t old_cycle = position_cycle_seq_;

    if (lifecycle.pre_close) {
        if (seq == UINT64_MAX)
            throw std::overflow_error("exit lifecycle event exhausted");
        const exit_legs::Frame cause{
            ++seq, bar_index_, current_exit_leg_domain(), lifecycle.pre_close->phase};
        for (const auto& intent : lifecycle.pre_close->operations) {
            auto* copy = find_leg_copy(
                copies, intent.order_incarnation, intent.created_seq);
            if (!copy) return execution::Status::InvalidLifecycle;
            const auto result = transition_exit_leg(
                copy->legs, copy->incarnation, intent.operation, cause,
                seq, old_cycle);
            if (result == ExitLegTransitionResult::Exhausted)
                throw std::overflow_error("exit lifecycle event exhausted");
            if (result == ExitLegTransitionResult::RevisionExhausted)
                throw std::overflow_error("exit lifecycle revision exhausted");
            if (result != ExitLegTransitionResult::Applied
                && result != ExitLegTransitionResult::Replay)
                return execution::Status::InvalidLifecycle;
        }
    }

    if (will_reset_to_flat) {
        for (auto& copy : copies) {
            if (copy.removed || copy.type != OrderType::EXIT) continue;
            if (!copy.legs.target().incarnation)
                copy.legs.attach(copy.incarnation, old_cycle);
            if (seq == UINT64_MAX)
                throw std::overflow_error("exit lifecycle event exhausted");
            const exit_legs::Frame cause{
                ++seq, bar_index_, current_exit_leg_domain(),
                exit_legs::Phase::Observation};
            const exit_legs::Action action{
                copy.legs.target(), copy.legs.revision(), cause,
                exit_legs::BindOwner{0}};
            // Flat cleanup unbinds the lifecycle's stored owner, which may
            // still be zero for a prearmed exit. The pending instruction's
            // incarnation must nevertheless match exactly.
            const auto applied = copy.legs.apply(
                {copy.incarnation, copy.legs.target().owner}, action);
            if (applied == exit_legs::Result::Exhausted)
                throw std::overflow_error("exit lifecycle revision exhausted");
            if (applied != exit_legs::Result::Applied) {
                if (lifecycle.pre_close)
                    return execution::Status::InvalidLifecycle;
                throw std::logic_error("exit lifecycle flat unbind refused");
            }
        }
    }

    for (const auto& removal : lifecycle.removals) {
        auto* copy = find_leg_copy(
            copies, removal.incarnation, removal.created_seq);
        if (!copy || copy->type != OrderType::EXIT)
            return execution::Status::InvalidLifecycle;
        copy->removed = true;
    }

    if (will_open_quoted) {
        const int64_t new_cycle = next_position_cycle_seq_;
        for (auto& copy : copies) {
            if (copy.removed || copy.type != OrderType::EXIT) continue;
            if (!copy.legs.target().incarnation)
                copy.legs.attach(copy.incarnation, new_cycle);
            if (copy.legs.target().owner == new_cycle) continue;
            const auto result = transition_exit_leg(
                copy.legs, copy.incarnation, exit_legs::BindOwner{new_cycle},
                std::nullopt, seq, new_cycle);
            if (result == ExitLegTransitionResult::Exhausted)
                throw std::overflow_error("exit lifecycle event exhausted");
            if (result == ExitLegTransitionResult::RevisionExhausted)
                throw std::overflow_error("exit lifecycle revision exhausted");
            if (result != ExitLegTransitionResult::Applied
                && result != ExitLegTransitionResult::Replay)
                throw std::logic_error("exit lifecycle action refused");
        }
    }
    return std::nullopt;
}

void BacktestEngine::apply_pre_close_lifecycle_batch(
        const execution::LifecycleBatch& batch) {
    const auto cause = next_leg_event(batch.phase);
    for (const auto& intent : batch.operations) {
        PendingOrder* order = find_unique_pending(
            intent.order_incarnation, intent.created_seq);
        if (!order) throw std::logic_error("exit lifecycle pre-close target missing");
        const auto result = transition_exit_leg(
            order->legs, order->incarnation, intent.operation, cause,
            exit_leg_event_seq_, position_cycle_seq_);
        if (result == ExitLegTransitionResult::Exhausted)
            throw std::overflow_error("exit lifecycle event exhausted");
        if (result == ExitLegTransitionResult::RevisionExhausted)
            throw std::overflow_error("exit lifecycle revision exhausted");
        if (result == ExitLegTransitionResult::StaleIdentity)
            throw std::logic_error("stale exit lifecycle instruction");
        if (result == ExitLegTransitionResult::BindRefused)
            throw std::logic_error("exit lifecycle owner bind refused");
        if (result != ExitLegTransitionResult::Applied
            && result != ExitLegTransitionResult::Replay)
            throw std::logic_error("exit lifecycle action refused");
    }
}

void BacktestEngine::apply_authorized_pending_removals(
        const std::vector<execution::PendingRemoval>& removals) {
    if (removals.empty()) return;
    std::vector<execution::PendingRemoval> remaining = removals;
    pending_orders_.erase(
        std::remove_if(pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& order) {
                if (order.type != OrderType::EXIT) return false;
                for (auto it = remaining.begin(); it != remaining.end(); ++it) {
                    if (it->incarnation == order.incarnation
                        && it->created_seq == order.created_seq) {
                        remaining.erase(it);
                        return true;
                    }
                }
                return false;
            }),
        pending_orders_.end());
    if (!remaining.empty())
        throw std::logic_error("exit lifecycle removal target missing");
}

} // namespace pineforge
