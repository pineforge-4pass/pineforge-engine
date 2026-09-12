#include <pineforge/native_order.hpp>

#include <cstdio>
#include <string>
#include <variant>

using pineforge::execution::Flatten;
using pineforge::native_order::CommandEvent;
using pineforge::native_order::CommittedExecutionFacts;
using pineforge::native_order::DriverEligibilityClass;
using pineforge::native_order::EvaluationContext;
using pineforge::native_order::ExecutionAppliedEvent;
using pineforge::native_order::ExecutionProposal;
using pineforge::native_order::Installed;
using pineforge::native_order::MatchRejectedEvent;
using pineforge::native_order::MatchRejectReason;
using pineforge::native_order::NoChange;
using pineforge::native_order::NoEffectEvent;
using pineforge::native_order::PositionFlat;
using pineforge::native_order::PositionNonflat;
using pineforge::native_order::PreparedExecution;
using pineforge::native_order::PreparedMutation;
using pineforge::native_order::Request;
using pineforge::native_order::RunIdentity;
using pineforge::native_order::Side;
using pineforge::native_order::TargetObservation;
using pineforge::native_order::WorkingRequestCore;
using pineforge::order_action::Transact;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)

EvaluationContext ctx_at(uint64_t ordinal, int64_t time_ms, bool matching = true) {
    EvaluationContext context;
    context.cursor.point.ordinal = ordinal;
    context.cursor.point.effective_time_ms = time_ms;
    context.driver_class = DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = matching;
    return context;
}

void advance(uint64_t& ordinal, const Installed& installed) {
    ordinal += installed.events.count == 0 ? 0 : installed.events.count;
    if (installed.events.count == 0) ++ordinal;  // unused; callers pass next
}
}  // namespace

int main() {
    RunIdentity id{"terminal-session", 1};
    WorkingRequestCore core(id);
    uint64_t incarnation = 1;
    uint64_t ordinal = 1;
    auto accepted = core.submit(Request{Transact{1.0}, "buy", ""}, 60000, incarnation, ordinal);
    CHECK(accepted.status == pineforge::native_order::SubmitStatus::Accepted);
    CHECK(core.live().size() == 1);
    CHECK(core.history().size() == 1);

    auto none_prep = core.prepare_no_effect(*accepted.handle, ctx_at(2, 60000), ordinal);
    CHECK(std::holds_alternative<PreparedMutation>(none_prep));
    auto none_installed = core.install_mutation(
            std::get<PreparedMutation>(std::move(none_prep)));
    CHECK(std::holds_alternative<Installed>(none_installed));
    ordinal += std::get<Installed>(none_installed).events.count;
    CHECK(core.live().empty());
    CHECK(core.history().size() == 2);
    CHECK(std::holds_alternative<NoEffectEvent>(core.history().back()));

    auto second = core.submit(Request{Transact{-1.0}, "sell", ""}, 120000, incarnation, ordinal);
    CHECK(second.status == pineforge::native_order::SubmitStatus::Accepted);
    auto reject_prep = core.prepare_match_rejected(
            *second.handle, ctx_at(ordinal + 1, 120000), MatchRejectReason::MaxAbsUnits, ordinal);
    CHECK(std::holds_alternative<PreparedMutation>(reject_prep));
    auto reject_installed = core.install_mutation(
            std::get<PreparedMutation>(std::move(reject_prep)));
    CHECK(std::holds_alternative<Installed>(reject_installed));
    ordinal += std::get<Installed>(reject_installed).events.count;
    CHECK(core.live().empty());
    CHECK(std::holds_alternative<MatchRejectedEvent>(core.history().back()));
    CHECK(std::get<MatchRejectedEvent>(core.history().back()).reason
          == MatchRejectReason::MaxAbsUnits);

    auto third = core.submit(Request{Flatten{}, "flat", ""}, 180000, incarnation, ordinal);
    CHECK(third.status == pineforge::native_order::SubmitStatus::Accepted);
    TargetObservation long_book;
    long_book.current_position = PositionNonflat{1, Side::Long};
    auto eval = core.prepare_evaluation(*third.handle, ctx_at(ordinal + 1, 180000), long_book,
                                        ordinal);
    CHECK(std::holds_alternative<PreparedMutation>(eval));
    auto eval_installed = core.install_mutation(std::get<PreparedMutation>(std::move(eval)));
    CHECK(std::holds_alternative<Installed>(eval_installed));
    ordinal += std::get<Installed>(eval_installed).events.count;

    ExecutionProposal proposal;
    proposal.cursor.point.ordinal = ordinal;  // overwritten below from live allowance point
    proposal.cursor = ctx_at(core.live()[0].allowance.index() == 0 ? 1 : 1, 180000).cursor;
    // Use the same point as evaluation initialized.
    proposal.cursor.point.ordinal = 5;  // evaluation used usable ordinal then next point
    proposal.cursor.point.effective_time_ms = 180000;
    // Re-read live allowance point.
    if (const auto* units = std::get_if<pineforge::native_order::AllowanceAllScope>(
                &core.live()[0].allowance)) {
        proposal.cursor.point.ordinal = units->point_ordinal;
    }
    proposal.raw_price = 104;
    proposal.resolved_price = 104;
    proposal.physical_action = Flatten{};
    proposal.scope = pineforge::execution::Book{};
    proposal.pre_fill = PositionNonflat{1, Side::Long};
    proposal.inspected_closed_units = 1.0;
    proposal.inspected_opened_units = 0.0;
    proposal.inspected_current_ticket = 6;
    auto exec_prep = core.prepare_execution(*third.handle, proposal, ordinal);
    CHECK(std::holds_alternative<PreparedExecution>(exec_prep));
    CommittedExecutionFacts facts;
    facts.result.status = pineforge::execution::Status::Applied;
    facts.result.closed_units = 1.0;
    facts.result.opened_units = 0.0;
    facts.result.current_ticket = 6;
    facts.result.first_trade_index = 0;
    facts.result.closed_trade_count = 1;
    facts.cycle_before = 1;
    facts.cycle_after = 0;
    facts.post_target.current_position = PositionFlat{};
    facts.committed_action = Flatten{};
    auto exec_installed = core.install_execution(
            std::get<PreparedExecution>(std::move(exec_prep)), facts);
    CHECK(std::holds_alternative<Installed>(exec_installed));
    CHECK(core.live().empty());
    CHECK(std::holds_alternative<ExecutionAppliedEvent>(core.history().back()));
    CHECK(std::get<ExecutionAppliedEvent>(core.history().back()).current_ticket == 6);
    CHECK(std::get<ExecutionAppliedEvent>(core.history().back()).terminal);
    (void)advance;

    std::printf("%s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
