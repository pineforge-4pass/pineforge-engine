#pragma once

#include <pineforge/native_order.hpp>

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

namespace p2_kernel_order_test {
namespace no = pineforge::native_order;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline no::TargetObservation long_book() {
    no::TargetObservation target;
    target.current_position = no::PositionNonflat{7, no::Side::Long};
    return target;
}

struct Fixture {
    no::RunIdentity run;
    no::WorkingRequestCore core;
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;

    explicit Fixture(const char* name) : run{name, 1}, core(run) {}

    no::RequestHandle submit(const no::Request& request) {
        const auto result = core.submit(request, 0, incarnation, ordinal);
        require(result.status == no::SubmitStatus::Accepted && result.handle,
                "fixture request was not accepted");
        return *result.handle;
    }

    no::Installed mutation(no::Preparation<no::PreparedMutation> prepared) {
        require(std::holds_alternative<no::PreparedMutation>(prepared),
                "fixture mutation preparation failed");
        auto installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(prepared)));
        require(std::holds_alternative<no::Installed>(installed), "fixture mutation install failed");
        const auto result = std::get<no::Installed>(installed);
        ordinal += result.events.count;
        return result;
    }

    no::EvaluationContext evaluate(const no::RequestHandle& handle) {
        no::EvaluationContext context;
        context.cursor.point.ordinal = ordinal++;
        context.cursor.point.effective_time_ms = 1000;
        context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
        context.driver_class = no::DriverEligibilityClass::ObservedPrint;
        context.existing_matching_bit = true;
        auto prepared = core.prepare_evaluation(handle, context, long_book(), ordinal);
        if (std::holds_alternative<no::PreparedMutation>(prepared)) mutation(std::move(prepared));
        else require(std::holds_alternative<no::NoChange>(prepared), "fixture evaluation failed");
        return context;
    }

    no::EventId fill(const no::RequestHandle& handle, double units, double price = 100.0) {
        const auto context = evaluate(handle);
        no::ExecutionProposal proposal;
        proposal.cursor = context.cursor;
        proposal.raw_price = proposal.resolved_price = price;
        proposal.physical_action = no::Transact{units};
        proposal.scope = pineforge::execution::Book{};
        proposal.pre_fill = no::PositionNonflat{7, no::Side::Long};
        proposal.pre_target = long_book();
        proposal.inspected_opened_units = units;
        proposal.inspected_current_ticket = 1;
        auto prepared = core.prepare_execution(handle, proposal, ordinal);
        require(std::holds_alternative<no::PreparedExecution>(prepared),
                "fixture execution preparation failed");
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = units;
        facts.result.current_ticket = 1;
        facts.cycle_before = facts.cycle_after = 7;
        facts.post_target = long_book();
        facts.committed_action = proposal.physical_action;
        auto installed = core.install_execution(
            std::get<no::PreparedExecution>(std::move(prepared)), facts);
        require(std::holds_alternative<no::Installed>(installed), "fixture execution install failed");
        ordinal += std::get<no::Installed>(installed).events.count;
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
        require(applied && applied->handle() == handle && applied->terminal
                    && applied->filled_working == units,
                "fixture did not commit the terminal group cause");
        return no::EventId{run, applied->ordinal};
    }

    void reject_small() {
        require(core.submit(no::Request{no::Transact{0.0}, "advance", ""},
                            0, incarnation, ordinal).status == no::SubmitStatus::Rejected,
                "fixture journal advance failed");
    }
};
} // namespace p2_kernel_order_test
