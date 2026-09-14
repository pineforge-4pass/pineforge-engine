#include <pineforge/source/pine_strategy_host.hpp>
#include "../engine_internal.hpp"

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
using namespace source;
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

exit_legs::Domain source::PineStrategyHost::current_exit_leg_domain() const {
    if (stream_phase_ != StreamPhase::IDLE) return exit_legs::Domain::RawTicks;
    if (bar_magnifier_enabled_) {
        return coof_scheduler_active_ ? exit_legs::Domain::MagnifierCoof
                                      : exit_legs::Domain::Magnifier;
    }
    return coof_scheduler_active_ ? exit_legs::Domain::Coof
                                  : exit_legs::Domain::Ordinary;
}

exit_legs::Frame source::PineStrategyHost::preview_next_leg_event(exit_legs::Phase phase) const {
    if (exit_leg_event_seq_ == UINT64_MAX)
        throw std::overflow_error("exit lifecycle event exhausted");
    return {exit_leg_event_seq_ + 1, bar_index_, current_exit_leg_domain(), phase};
}

const PendingOrder* source::PineStrategyHost::find_unique_pending(
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

PendingOrder* source::PineStrategyHost::find_unique_pending(
        uint64_t incarnation, int64_t created_seq) {
    return const_cast<PendingOrder*>(
        static_cast<const PineStrategyHost*>(this)->find_unique_pending(
            incarnation, created_seq));
}

BacktestEngine::ExitLegTransitionResult source::PineStrategyHost::transition_exit_leg(
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

} // namespace pineforge
