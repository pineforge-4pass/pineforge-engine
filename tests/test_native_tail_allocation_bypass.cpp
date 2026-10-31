// Uses only the base public API: overlay this file on the frozen base and
// execute it BEFORE applying the implementation. The optional receipt-vector
// allocation used to throw bad_alloc; the one-record fallback must complete
// the identical group adjustment.
#include "global_allocation_replacement.hpp"
#include <pineforge/native_order.hpp>

#include <cstdio>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void allocation_bypass() {
    // SessionKey is already interned before the fault; copying it cannot
    // allocate. The short request text also needs no heap on supported hosts.
    // Cause/plan copies share immutable definitions.
    const no::RunIdentity run{"t", 1};
    no::WorkingRequestCore core(run);
    core.reserve(32); // reserve the one journal chunk before the fault
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    const auto submit = [&](no::Request request) {
        auto prepared = core.prepare_submit(request, {}, incarnation, ordinal);
        const auto predicted = prepared.predicted();
        require(predicted.handle.has_value(), "prepare accepted test request");
        auto installed = core.install_submit(std::move(prepared));
        require(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed),
                "install test request");
        ordinal += std::get<no::CommandInstalled<no::SubmitResult>>(installed).events.count;
        incarnation = predicted.handle->incarnation + 1;
        return *predicted.handle;
    };
    no::Request deferred{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "d", ""};
    deferred.group = no::Member{44, 2, no::GroupEffect::Reduce};
    const auto target = submit(deferred);
    no::Request request{no::Transact{0.25}, "s", ""};
    request.group = no::Member{44, 1, no::GroupEffect::Reduce};
    const auto source = submit(request);
    no::EvaluationContext context;
    context.cursor.point.ordinal = ordinal;
    context.cursor.point.effective_time_ms = 1000;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    no::TargetObservation flat;
    flat.current_position = no::PositionFlat{};
    auto evaluated = core.prepare_evaluation(source, context, flat, ordinal);
    if (auto* mutation = std::get_if<no::PreparedMutation>(&evaluated)) {
        const auto installed = core.install_mutation(std::move(*mutation));
        require(std::holds_alternative<no::Installed>(installed), "install source evaluation");
        ordinal += std::get<no::Installed>(installed).events.count;
    } else require(std::holds_alternative<no::NoChange>(evaluated), "source evaluation is valid");
    no::ExecutionProposal proposal;
    proposal.cursor = context.cursor;
    proposal.raw_price = proposal.resolved_price = 100.0;
    proposal.physical_action = no::Transact{0.25};
    proposal.scope = ex::Book{};
    proposal.pre_fill = no::PositionFlat{};
    proposal.inspected_opened_units = 0.25;
    proposal.inspected_current_ticket = 3.0;
    auto execution = core.prepare_execution(source, proposal, ordinal);
    require(std::holds_alternative<no::PreparedExecution>(execution), "prepare source execution");
    no::CommittedExecutionFacts facts;
    facts.result.status = ex::Status::Applied;
    facts.result.opened_units = 0.25;
    facts.result.current_ticket = 3.0;
    facts.cycle_before = 1;
    facts.cycle_after = 2;
    facts.post_target.current_position = no::PositionNonflat{2, no::Side::Long};
    facts.committed_action = proposal.physical_action;
    const auto installed = core.install_execution(
        std::get<no::PreparedExecution>(std::move(execution)), facts);
    require(std::holds_alternative<no::Installed>(installed), "install source execution");
    ordinal += std::get<no::Installed>(installed).events.count;
    const no::EventId cause{run, std::get<no::ExecutionAppliedEvent>(core.history().back()).ordinal};
    const auto before = core.history_end();
    const auto refused = global_allocation::refused;
    // ensure_receipt_store first allocates its mandatory owner, then its
    // optional tail. No prepared-token allocation happens until afterwards.
    global_allocation::fail_after = 1;
    auto prepared = [&] {
        try {
            return core.prepare_group_effect(cause, target, ordinal);
        } catch (const std::bad_alloc&) {
            global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
            throw std::runtime_error("optional receipt tail allocation must bypass");
        }
    }();
    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    require(global_allocation::refused == refused + 1, "the real tail allocation was refused once");
    require(std::holds_alternative<no::PreparedMutation>(prepared),
            "allocation bypass prepares the complete group effect");
    auto effect = core.install_mutation(std::get<no::PreparedMutation>(std::move(prepared)));
    require(std::holds_alternative<no::Installed>(effect), "allocation bypass installs group effect");
    ordinal += std::get<no::Installed>(effect).events.count;
    const auto& event = std::get<no::DeferredGroupAdjustmentEvent>(core.history_at(before));
    const auto& pending = std::get<no::PendingDeferred>(core.find_live(target)->pending);
    require(event.deferred_delta == 0.25 && pending.total == 0.25 && pending.count == 1,
            "allocation bypass preserves exact group arithmetic");
    require(core.group_effect_receipt_count() == 1, "allocation bypass retains the receipt");
    const auto receipt = core.group_effect_receipt(0);
    require(receipt.cause == cause && receipt.recipient == target
                && receipt.effect == no::GroupEffect::Reduce
                && receipt.outcome_ordinal == event.ordinal,
            "allocation bypass preserves the complete receipt identity");
}
} // namespace

int main() {
    try {
        allocation_bypass();
        std::puts("native tail allocation bypass passed");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        std::fprintf(stderr, "native tail allocation bypass: %s\n", error.what());
        return 1;
    }
}
