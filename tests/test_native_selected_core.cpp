// WorkingRequestCore selected authorization tests. Physical observations and
// commit receipts are literal inputs; real lot/fee/hash witnesses live in the
// companion host tests. This core owns no physical roster or hash implementation.
#include <pineforge/native_order.hpp>

// Counts the install's heap allocations: every replaceable form, one allocator.
#include "global_allocation_replacement.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

using namespace pineforge::native_order;
namespace ex = pineforge::execution;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define REQUIRE(x) do { const bool ok = bool(x); CHECK(ok); \
    if (!ok) throw std::runtime_error("test prerequisite failed"); } while (0)
const RunIdentity run{"selected-core-provenance-with-a-non-small-session-key", 1};
RequestHandle handle(uint64_t incarnation) { return {run, incarnation}; }

TargetObservation observed(std::initializer_list<uint64_t> live,
                           std::initializer_list<uint64_t> cohort = {10, 20},
                           PositionIdentity position = PositionNonflat{7, Side::Long},
                           int64_t queried_cycle = 7) {
    TargetObservation result;
    result.current_position = position;
    for (const auto member : cohort) {
        const bool alive = std::find(live.begin(), live.end(), member) != live.end();
        result.openings.push_back({handle(member), queried_cycle, position, alive});
    }
    return result;
}
CommandContext command(const TargetObservation& target) {
    CommandContext context;
    context.decision_time_ms = 1000;
    context.openings = target.openings;
    return context;
}
Request selected(double units = 9, std::vector<RequestHandle> cohort = {handle(20), handle(10)}) {
    Request request{Reduce{ExplicitUnits{units}}, "same-label", "fixed cohort"};
    request.owner = BindOpenings{std::move(cohort), 7};
    return request;
}
EvaluationContext point(uint64_t ordinal, bool current = false) {
    EvaluationContext context;
    context.cursor.point.ordinal = ordinal;
    context.cursor.point.effective_time_ms = 1000;
    context.cursor.point.provenance = current ? pineforge::NativePriceProvenance::CurrentExecution
                                            : pineforge::NativePriceProvenance::ObservedPrint;
    context.driver_class = current ? DriverEligibilityClass::CurrentExecution
                                   : DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

struct Fixture {
    WorkingRequestCore core{run};
    uint64_t incarnation = 100;
    uint64_t ordinal = 1;

    SubmitResult submit(const Request& request, const CommandContext& context) {
        auto prepared = core.prepare_submit(request, context, incarnation, ordinal);
        const auto predicted = prepared.predicted();
        const auto installed = core.install_submit(std::move(prepared));
        REQUIRE(std::holds_alternative<CommandInstalled<SubmitResult>>(installed));
        ordinal += std::get<CommandInstalled<SubmitResult>>(installed).events.count;
        if (predicted.handle) incarnation = predicted.handle->incarnation + 1;
        return predicted;
    }
    RequestHandle put(const Request& request, const TargetObservation& target = observed({10, 20})) {
        auto result = submit(request, command(target));
        REQUIRE(result.status == SubmitStatus::Accepted);
        REQUIRE(result.handle);
        return *result.handle;
    }
    void mutation(Preparation<PreparedMutation> prepared) {
        REQUIRE(std::holds_alternative<PreparedMutation>(prepared));
        auto result = core.install_mutation(std::get<PreparedMutation>(std::move(prepared)));
        REQUIRE(std::holds_alternative<Installed>(result));
        ordinal += std::get<Installed>(result).events.count;
    }
    EvaluationContext evaluate(const RequestHandle& target, const TargetObservation& observation,
                                bool current = false) {
        const auto context = point(ordinal++, current);
        auto prepared = core.prepare_evaluation(target, context, observation, ordinal);
        if (std::holds_alternative<PreparedMutation>(prepared)) mutation(std::move(prepared));
        else REQUIRE(std::holds_alternative<NoChange>(prepared));
        return context;
    }
    ExecutionProposal proposal(const EvaluationContext& context, const TargetObservation& before,
                               double closed, std::vector<uint64_t> scope = {10, 20},
                               bool flatten = false) {
        ExecutionProposal result;
        result.cursor = context.cursor;
        result.raw_price = 110;
        result.resolved_price = 110;
        result.physical_action = flatten
            ? pineforge::native_order::ExecutionPlan{ex::Flatten{}}
            : pineforge::native_order::ExecutionPlan{pineforge::order_action::Reduce{closed}};
        result.scope = SelectedExposure{7, std::move(scope)};
        result.pre_fill = before.current_position;
        result.pre_target = before;
        result.inspected_closed_units = closed;
        result.inspected_current_ticket = 6;
        return result;
    }
    ExecutionAppliedEvent fill(const RequestHandle& target, const ExecutionProposal& proposal,
                               const TargetObservation& after, std::size_t rows = 1) {
        auto prepared = core.prepare_execution(target, proposal, ordinal);
        REQUIRE(std::holds_alternative<PreparedExecution>(prepared));
        CommittedExecutionFacts facts;
        facts.result.status = ex::Status::Applied;
        facts.result.closed_units = proposal.inspected_closed_units;
        facts.result.opened_units = proposal.inspected_opened_units;
        facts.result.current_ticket = proposal.inspected_current_ticket;
        facts.result.first_trade_index = 5;
        facts.result.closed_trade_count = rows;
        facts.cycle_before = 7;
        const auto* position = std::get_if<PositionNonflat>(&after.current_position);
        facts.cycle_after = position ? position->cycle : 0;
        facts.post_target = after;
        facts.committed_action = proposal.physical_action;
        const auto before_allocations = global_allocation::allocations;
        auto installed = core.install_execution(std::get<PreparedExecution>(std::move(prepared)), facts);
        const auto after_allocations = global_allocation::allocations;
        CHECK(before_allocations == after_allocations);
        REQUIRE(std::holds_alternative<Installed>(installed));
        CHECK(std::get<Installed>(installed).events.count == 1);
        ordinal += std::get<Installed>(installed).events.count;
        REQUIRE(std::holds_alternative<ExecutionAppliedEvent>(core.history().back()));
        return std::get<ExecutionAppliedEvent>(core.history().back());
    }
};

template <class T>
void error_is(const Preparation<T>& result, CoreFailure expected) {
    REQUIRE(std::holds_alternative<PreparationError>(result));
    CHECK(std::get<PreparationError>(result).code == expected);
}

void canonical_admission_and_invalid_attempts() {
    Fixture fixture;
    auto request = selected();
    auto context = command(observed({10, 20}, {20, 10}));
    auto accepted = fixture.submit(request, context);
    REQUIRE(accepted.handle);
    const auto* live = fixture.core.find_live(*accepted.handle);
    REQUIRE(live);
    CHECK(std::get<BindOpenings>(request.owner).openings.front() == handle(20));
    const std::vector<RequestHandle> canonical{handle(10), handle(20)};
    CHECK(std::get<BindOpenings>(live->request().owner).openings == canonical);
    CHECK(std::get<OpeningsClose>(live->authority).openings == canonical);
    CHECK(std::get<OpeningsClose>(live->authority).side == Side::Long);
    CHECK(std::get<EnrollmentFromCommand>(std::get<OpeningsClose>(live->authority).enrollment)
              .accepted.ordinal == accepted.event_ordinal);
    CHECK(fixture.core.bound_close_handles() == std::vector<RequestHandle>{*accepted.handle});
    // Neither opening request is working. Physical provenance, not parent status,
    // is the authority supplied by these observations.
    CHECK(!fixture.core.find_live(handle(10)));
    CHECK(!fixture.core.find_live(handle(20)));
    CHECK(!has_market_defaults(request));

    auto reject = [&](Request attempt, CommandContext observations, RequestRejectReason reason) {
        const auto before_incarnation = fixture.incarnation;
        const auto before_history = fixture.core.history().size();
        const auto before_live = fixture.core.live().size();
        auto result = fixture.submit(attempt, observations);
        CHECK(result.status == SubmitStatus::Rejected);
        CHECK(result.reason == reason);
        CHECK(!result.handle);
        CHECK(fixture.incarnation == before_incarnation);
        CHECK(fixture.core.live().size() == before_live);
        CHECK(fixture.core.history().size() == before_history + 1);
        const auto& rejected = std::get<RejectedEvent>(fixture.core.history().back());
        if (const auto* attempted = std::get_if<BindOpenings>(&attempt.owner)) {
            const auto& retained = std::get<BindOpenings>(rejected.request.owner);
            CHECK(retained.openings == attempted->openings);
            CHECK(retained.cycle == attempted->cycle);
        }
    };
    reject(selected(9, {}), context, RequestRejectReason::InvalidOwner);
    reject(selected(9, {handle(20), handle(20)}), context, RequestRejectReason::InvalidOwner);
    reject(selected(9, {handle(20), handle(0)}), context, RequestRejectReason::InvalidOwner);
    auto foreign = handle(10); foreign.run.run_number++;
    reject(selected(9, {handle(20), foreign}), context, RequestRejectReason::InvalidOwner);
    foreign = handle(10); foreign.run.session_key += "foreign";
    reject(selected(9, {handle(20), foreign}), context, RequestRejectReason::InvalidOwner);
    auto invalid = request; std::get<BindOpenings>(invalid.owner).cycle = 0;
    reject(invalid, context, RequestRejectReason::InvalidOwner);
    std::get<BindOpenings>(invalid.owner).cycle = 8;
    reject(invalid, context, RequestRejectReason::InvalidOwner);
    reject(request, command(observed({10})), RequestRejectReason::InvalidOwner);
    reject(request, command(observed({10}, {10})), RequestRejectReason::InvalidOwner);
    auto malformed = context; malformed.openings[1] = malformed.openings[0];
    reject(request, malformed, RequestRejectReason::InvalidOwner);
    malformed = context; malformed.openings[1].queried_cycle = 8;
    reject(request, malformed, RequestRejectReason::InvalidOwner);
    malformed = context; malformed.openings[1].current_position = PositionNonflat{7, Side::Short};
    reject(request, malformed, RequestRejectReason::InvalidOwner);
    reject(request, command(observed({}, {10, 20}, PositionFlat{})), RequestRejectReason::InvalidOwner);
    invalid = request; invalid.intent = Transact{3};
    reject(invalid, context, RequestRejectReason::InvalidOwner);
    invalid.intent = Reduce{OwnerOpenedUnits{}};
    reject(invalid, context, RequestRejectReason::InvalidQuantityBasis);
    invalid = request; invalid.intent = Flatten{}; invalid.capacity = PointBudget{1};
    reject(invalid, context, RequestRejectReason::InvalidCapacity);
    malformed = context; malformed.surface = CommandSurface::MarketOnly;
    reject(request, malformed, RequestRejectReason::InvalidOwner);

    Fixture singleton;
    auto single = singleton.put(selected(2, {handle(10)}), observed({10}, {10}));
    CHECK(std::get<OpeningsClose>(singleton.core.find_live(single)->authority).openings.size() == 1);
    Fixture short_side;
    auto short_handle = short_side.put(selected(), observed({10, 20}, {10, 20},
                                                           PositionNonflat{7, Side::Short}));
    CHECK(short_side.core.working_is_buy(*short_side.core.find_live(short_handle)));
    CHECK(short_side.core.eligibility_facts(*short_side.core.find_live(short_handle), point(50))
              .position_side == Side::Short);
}

void exact_observations_and_scope() {
    Fixture fixture;
    const auto target = fixture.put(selected());
    const auto before = observed({10, 20});
    const auto context = fixture.evaluate(target, before);
    const auto good = fixture.proposal(context, before, 1);
    const auto history_size = fixture.core.history().size();
    auto invalid = good; invalid.pre_target.openings.clear();
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::MissingObservation);
    invalid = good; invalid.pre_target.openings[1] = invalid.pre_target.openings[0];
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::ObservationMismatch);
    invalid = good; invalid.pre_target.openings[0].queried_opening = handle(30);
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::ObservationMismatch);
    invalid = good; invalid.pre_target.openings[0].queried_cycle = 8;
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::ObservationMismatch);
    invalid = good; invalid.pre_target.openings[0].current_position = PositionFlat{};
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::ObservationMismatch);
    invalid = good; invalid.pre_target.openings.push_back(before.openings[0]);
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::ObservationMismatch);
    invalid = good; invalid.pre_fill = PositionNonflat{8, Side::Long};
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::ObservationMismatch);
    invalid = good; invalid.scope = ex::Book{};
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::InvalidScope);
    invalid = good; invalid.scope = ex::OpeningExposure{10, 7};
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::InvalidScope);
    for (const auto& incarnations : std::vector<std::vector<uint64_t>>{{10}, {10, 30}, {10, 10}, {}, {10, 20, 30}}) {
        invalid = good; invalid.scope = SelectedExposure{7, incarnations};
        error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::InvalidScope);
    }
    invalid = good; invalid.scope = SelectedExposure{8, {10, 20}};
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::InvalidScope);
    invalid = good; invalid.inspected_opened_units = 1;
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::InvalidProposal);
    CHECK(fixture.core.history().size() == history_size);
    CHECK(std::get<RemainingUnits>(fixture.core.find_live(target)->remaining).q == 9);

    // Even with an allowance already initialized at this point, malformed
    // refreshes are errors and a newly retired member must leave the scope.
    auto missing = before; missing.openings.pop_back();
    error_is(fixture.core.prepare_evaluation(target, context, missing, fixture.ordinal),
             CoreFailure::MissingObservation);
    const auto retired = observed({20});
    auto fresh = fixture.proposal(context, retired, 1, {20});
    invalid = fresh; invalid.scope = SelectedExposure{7, {10, 20}};
    error_is(fixture.core.prepare_execution(target, invalid, fixture.ordinal), CoreFailure::InvalidScope);
    const auto applied = fixture.fill(target, fresh, retired);
    CHECK(std::get<SelectedExposure>(applied.scope).incarnations == std::vector<uint64_t>{20});
    CHECK(std::get<BindOpenings>(applied.request().owner).openings.size() == 2);
    CHECK(std::get<OpeningsClose>(fixture.core.find_live(target)->authority).openings.size() == 2);
}

void partial_budgets_fragments_and_exhaustion() {
    Fixture fixture;
    auto request = selected(); request.capacity = PointBudget{2};
    const auto target = fixture.put(request);
    auto before = observed({10, 20});
    auto context = fixture.evaluate(target, before);
    auto applied = fixture.fill(target, fixture.proposal(context, before, 2, {20, 10}), before, 2);
    CHECK(!applied.terminal);
    CHECK(applied.closed_units == 2 && applied.filled_working == 2);
    CHECK(applied.current_ticket == 6 && applied.closed_trade_count == 2);
    CHECK(std::get<SelectedExposure>(applied.scope).incarnations == (std::vector<uint64_t>{10, 20}));
    CHECK(std::get<RemainingUnits>(fixture.core.find_live(target)->remaining).q == 7);
    CHECK(std::get<AllowanceUnits>(fixture.core.find_live(target)->allowance).left == 0);
    CHECK(std::holds_alternative<NoChange>(fixture.core.prepare_execution(
        target, fixture.proposal(context, before, 1), fixture.ordinal)));

    // All fragments of 10 retire, while 20 keeps the cohort alive.
    context = fixture.evaluate(target, before);
    auto only20 = observed({20});
    applied = fixture.fill(target, fixture.proposal(context, before, 2), only20);
    CHECK(!applied.terminal);
    CHECK(std::get<RemainingUnits>(fixture.core.find_live(target)->remaining).q == 5);
    CHECK(std::holds_alternative<NoChange>(fixture.core.prepare_bound_expiry(
        {run, applied.ordinal}, target, only20, fixture.ordinal)));

    // A continued fragment under the original 10 provenance is authorized
    // again; no permanent per-member tombstone is introduced.
    context = fixture.evaluate(target, before);
    applied = fixture.fill(target, fixture.proposal(context, before, 2), only20);
    CHECK(!applied.terminal);
    CHECK(std::get<RemainingUnits>(fixture.core.find_live(target)->remaining).q == 3);
    context = fixture.evaluate(target, only20);
    applied = fixture.fill(target, fixture.proposal(context, only20, 1, {20}), observed({}));
    CHECK(applied.terminal);
    CHECK(applied.terminal_reason == AppliedTerminalReason::TargetExhausted);
    CHECK(std::get<RemainingProjectionUnits>(applied.remaining_after).q == 2);
    CHECK(!fixture.core.find_live(target));
    CHECK(std::holds_alternative<NoChange>(fixture.core.prepare_evaluation(
        target, point(fixture.ordinal++), before, fixture.ordinal)));

    Fixture flatten;
    auto all = selected(); all.intent = Flatten{};
    const auto flat_target = flatten.put(all);
    context = flatten.evaluate(flat_target, before);
    applied = flatten.fill(flat_target, flatten.proposal(context, before, 3, {10, 20}, true), observed({}), 3);
    CHECK(applied.terminal_reason == AppliedTerminalReason::Flattened);
    CHECK(applied.cycle_after == 7);  // Unrelated book exposure remains.
    CHECK(applied.closed_trade_count == 3);  // Several fragments, one event/ticket.
    CHECK(applied.filled_working == 3 && applied.current_ticket == 6);

    Fixture satisfied;
    auto cap = satisfied.put(selected(1));
    context = satisfied.evaluate(cap, before);
    applied = satisfied.fill(cap, satisfied.proposal(context, before, 1), before);
    CHECK(applied.terminal_reason == AppliedTerminalReason::WorkingUnitsSatisfied);
}

void expiry_and_post_observation_validation() {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture fixture;
        const auto target = fixture.put(selected());
        auto observation = observed({});
        if (mode == 1) observation = observed({}, {10, 20}, PositionNonflat{8, Side::Long});
        if (mode == 2) observation = observed({}, {10, 20}, PositionNonflat{7, Side::Short});
        auto missing = observation; missing.openings.clear();
        error_is(fixture.core.prepare_bound_expiry({run, 1}, target, missing, fixture.ordinal),
                 CoreFailure::MissingObservation);
        auto inconsistent = observation; inconsistent.openings[0].queried_cycle = 99;
        error_is(fixture.core.prepare_bound_expiry({run, 1}, target, inconsistent, fixture.ordinal),
                 CoreFailure::ObservationMismatch);
        if (mode == 0) {
            fixture.mutation(fixture.core.prepare_bound_expiry({run, 1}, target, observation, fixture.ordinal));
        } else {
            fixture.evaluate(target, observation);
        }
        CHECK(!fixture.core.find_live(target));
        REQUIRE(std::holds_alternative<CancelledEvent>(fixture.core.history().back()));
        CHECK(std::get<CancelledEvent>(fixture.core.history().back()).reason == CancelReason::OwnerGone);
        CHECK(fixture.core.history().size() == 2);  // Accepted + cancelled only.
    }

    Fixture fixture;
    const auto target = fixture.put(selected());
    const auto before = observed({10, 20});
    const auto context = fixture.evaluate(target, before);
    auto proposal = fixture.proposal(context, before, 1);
    auto prepared = fixture.core.prepare_execution(target, proposal, fixture.ordinal);
    REQUIRE(std::holds_alternative<PreparedExecution>(prepared));
    CommittedExecutionFacts facts;
    facts.result = {ex::Status::Applied, 1, 0, 6};
    facts.post_target.current_position = before.current_position;
    const auto count = fixture.core.history().size();
    auto installed = fixture.core.install_execution(std::get<PreparedExecution>(std::move(prepared)), facts);
    REQUIRE(std::holds_alternative<InstallError>(installed));
    CHECK(std::get<InstallError>(installed) == InstallError::WrongCoreOrRun);
    CHECK(fixture.core.history().size() == count);
    CHECK(fixture.core.find_live(target));
}

void replacement_and_parent_lifetime() {
    Fixture fixture;
    auto request = selected(); request.trigger = Stop{105}; request.capacity = PointBudget{2};
    request.group = Member{9, 2, GroupEffect::Reduce};
    const auto target = fixture.put(request);
    const auto before = observed({10, 20});
    const auto context = fixture.evaluate(target, before);
    fixture.mutation(fixture.core.prepare_trigger(target, ActivateStop{context.cursor, 104},
                                                  context.driver_class, fixture.ordinal));
    fixture.fill(target, fixture.proposal(context, before, 2), before);
    const auto definition = fixture.core.find_live(target)->definition;
    const auto predecessor_incarnation = fixture.incarnation;
    auto invalid = selected(5, {handle(20), handle(30)});
    auto prepared = fixture.core.prepare_replace(target, invalid, command(observed({10, 20})),
                                                 fixture.incarnation, fixture.ordinal);
    CHECK(prepared.predicted().status == ReplaceStatus::ReplaceRejected);
    auto installed = fixture.core.install_replace(std::move(prepared));
    REQUIRE(std::holds_alternative<CommandInstalled<ReplaceResult>>(installed));
    fixture.ordinal += std::get<CommandInstalled<ReplaceResult>>(installed).events.count;
    CHECK(fixture.incarnation == predecessor_incarnation);
    CHECK(fixture.core.find_live(target)->definition == definition);
    CHECK(std::holds_alternative<StopActive>(fixture.core.find_live(target)->trigger_state));
    CHECK(std::get<AllowanceUnits>(fixture.core.find_live(target)->allowance).left == 0);
    CHECK(std::get<RemainingUnits>(fixture.core.find_live(target)->remaining).q == 7);
    CHECK(std::get<Member>(fixture.core.find_live(target)->request().group).group == 9);
    CHECK(std::get<BindOpenings>(std::get<ReplaceRejectedEvent>(fixture.core.history().back())
                                  .attempted.owner).openings == std::get<BindOpenings>(invalid.owner).openings);

    auto successor_request = selected(5);
    auto successor_context = command(before);
    prepared = fixture.core.prepare_replace(target, successor_request, successor_context,
                                            fixture.incarnation, fixture.ordinal);
    REQUIRE(prepared.predicted().successor);
    const auto successor = *prepared.predicted().successor;
    installed = fixture.core.install_replace(std::move(prepared));
    REQUIRE(std::holds_alternative<CommandInstalled<ReplaceResult>>(installed));
    fixture.ordinal += std::get<CommandInstalled<ReplaceResult>>(installed).events.count;
    fixture.incarnation = successor.incarnation + 1;
    CHECK(!fixture.core.find_live(target));
    const auto* live = fixture.core.find_live(successor);
    REQUIRE(live);
    CHECK(live->predecessor() == target);
    CHECK(live->birth().acceptance_ordinal > definition->birth.acceptance_ordinal);
    CHECK(std::holds_alternative<MarketReady>(live->trigger_state));
    CHECK(std::holds_alternative<AllowanceUnset>(live->allowance));
    CHECK(std::get<RemainingUnits>(live->remaining).q == 5);
    CHECK(std::get<OpeningsClose>(live->authority).openings ==
          (std::vector<RequestHandle>{handle(10), handle(20)}));
    CHECK(std::holds_alternative<NoGroup>(live->request().group));
    CHECK(std::holds_alternative<NoChange>(fixture.core.prepare_parent_terminal(
        {run, fixture.ordinal - 1}, successor, fixture.ordinal)));

    // A real opening request remains working after a partial fragment; its
    // successor may close its predecessor's already-open physical exposure.
    Fixture parent;
    Request opening{Transact{8}, "same-label", ""};
    opening.capacity = PointBudget{2};
    const auto parent_handle = parent.put(opening);
    const auto parent_context = parent.evaluate(parent_handle, before);
    auto parent_proposal = parent.proposal(parent_context, before, 0);
    parent_proposal.physical_action = Transact{2};
    parent_proposal.scope = ex::Book{};
    parent_proposal.inspected_opened_units = 2;
    parent.fill(parent_handle, parent_proposal, before, 0);
    auto bound_parent = selected(5, {parent_handle, handle(10)});
    const auto parent_observation = observed({10, parent_handle.incarnation}, {parent_handle.incarnation, 10});
    auto replacement = parent.core.prepare_replace(parent_handle, bound_parent, command(parent_observation),
                                                    parent.incarnation, parent.ordinal);
    REQUIRE(replacement.predicted().successor);
    const auto replacement_handle = *replacement.predicted().successor;
    REQUIRE(std::holds_alternative<CommandInstalled<ReplaceResult>>(
        parent.core.install_replace(std::move(replacement))));
    CHECK(!parent.core.find_live(parent_handle));
    CHECK(std::get<OpeningsClose>(parent.core.find_live(replacement_handle)->authority).openings ==
          (std::vector<RequestHandle>{handle(10), parent_handle}));
}

void aggregate_groups() {
    for (const auto effect : {GroupEffect::Reduce, GroupEffect::Cancel}) {
        Fixture fixture;
        auto filler_request = selected(10); filler_request.capacity = PointBudget{3};
        filler_request.group = Member{1, 1, effect};
        const auto filler = fixture.put(filler_request);
        auto recipient_request = selected(8); recipient_request.group = Member{1, 2, effect};
        const auto recipient = fixture.put(recipient_request);
        auto same_cohort = recipient_request; same_cohort.group = Member{1, 1, effect};
        const auto excluded = fixture.put(same_cohort);
        auto flatten_request = recipient_request; flatten_request.intent = Flatten{};
        const auto flatten = fixture.put(flatten_request);
        auto wait_request = Request{Reduce{OwnerOpenedUnits{}}, "wait", ""};
        wait_request.owner = WaitForApplied{filler};
        const auto waiting = fixture.put(wait_request);
        const auto before = observed({10, 20});
        const auto context = fixture.evaluate(filler, before);
        auto event = fixture.fill(filler, fixture.proposal(context, before, 3), before, 3);
        const EventId cause{run, event.ordinal};
        CHECK(event.filled_working == 3);
        auto recipients = fixture.core.group_recipients(cause);
        if (effect == GroupEffect::Cancel) {
            CHECK(recipients.empty());  // Nonterminal fill cannot cancel.
        } else {
            CHECK(recipients == (std::vector<RequestHandle>{recipient, flatten}));
            fixture.mutation(fixture.core.prepare_group_effect(cause, recipient, fixture.ordinal));
            CHECK(std::get<RemainingUnits>(fixture.core.find_live(recipient)->remaining).q == 5);
            const auto& reduction = std::get<ReservationReducedEvent>(fixture.core.history().back());
            CHECK(reduction.requested_delta == 3 && reduction.actual_deduction == 3);
            CHECK(std::get<NoChange>(fixture.core.prepare_group_effect(cause, recipient, fixture.ordinal))
                      .reason == NoChangeReason::AlreadyApplied);
            fixture.mutation(fixture.core.prepare_group_effect(cause, flatten, fixture.ordinal));
            CHECK(!fixture.core.find_live(flatten));
        }
        CHECK(std::get<RemainingUnits>(fixture.core.find_live(excluded)->remaining).q == 8);
        CHECK(std::holds_alternative<Wait>(fixture.core.find_live(waiting)->authority));
        const auto next = fixture.evaluate(filler, before);
        event = fixture.fill(filler, fixture.proposal(next, before, 2), observed({}), 2);
        CHECK(event.terminal_reason == AppliedTerminalReason::TargetExhausted);
        const EventId terminal{run, event.ordinal};
        if (effect == GroupEffect::Cancel) {
            recipients = fixture.core.group_recipients(terminal);
            CHECK(recipients == (std::vector<RequestHandle>{recipient, flatten}));
            for (const auto& member : recipients) {
                fixture.mutation(fixture.core.prepare_group_effect(terminal, member, fixture.ordinal));
            }
            CHECK(!fixture.core.find_live(recipient));
        }
        // Only explicit terminal dependency processing affects this waiter.
        CHECK(fixture.core.find_live(waiting));
        fixture.mutation(fixture.core.prepare_parent_terminal(terminal, waiting, fixture.ordinal));
        CHECK(!fixture.core.find_live(waiting));
    }

    Fixture fixture;
    auto source = selected(3); source.group = Member{2, 1, GroupEffect::Reduce};
    const auto filler = fixture.put(source);
    auto small = selected(2); small.group = Member{2, 2, GroupEffect::Reduce};
    const auto recipient = fixture.put(small);
    const auto before = observed({10, 20});
    const auto context = fixture.evaluate(filler, before);
    const auto event = fixture.fill(filler, fixture.proposal(context, before, 3), before, 3);
    const EventId cause{run, event.ordinal};
    // A new recipient after the cause is outside its birth cutoff.
    const auto later = fixture.put(small);
    CHECK(fixture.core.group_recipients(cause) == std::vector<RequestHandle>{recipient});
    const auto history_before = fixture.core.history().size();
    fixture.mutation(fixture.core.prepare_group_effect(cause, recipient, fixture.ordinal));
    CHECK(fixture.core.history().size() == history_before + 2);
    const auto& reduction = std::get<ReservationReducedEvent>(fixture.core.history()[history_before]);
    CHECK(reduction.requested_delta == 3 && reduction.actual_deduction == 2);
    CHECK(!fixture.core.find_live(recipient));
    CHECK(fixture.core.find_live(later));
    CHECK(std::get<NoChange>(fixture.core.prepare_group_effect(cause, recipient, fixture.ordinal))
              .reason == NoChangeReason::AlreadyApplied);
}

void current_guard_and_price_domain() {
    Fixture fixture;
    const auto target = fixture.put(selected());
    const auto before = observed({10, 20});
    auto context = point(fixture.ordinal + 1, true);
    const auto& live = *fixture.core.find_live(target);
    CHECK(fixture.core.evaluation_eligible(live, context));
    context.existing_matching_bit = false;
    CHECK(!fixture.core.evaluation_eligible(live, context));
    context.existing_matching_bit = true;
    context.driver_class = DriverEligibilityClass::ObservedPrint;
    CHECK(!fixture.core.evaluation_eligible(live, context));
    context = fixture.evaluate(target, before, true);
    auto proposal = fixture.proposal(context, before, 1);
    for (const double price : {0.0, -1.0}) {
        proposal.resolved_price = price;
        CHECK(std::holds_alternative<PreparedExecution>(fixture.core.prepare_execution(
            target, proposal, fixture.ordinal)));
        proposal.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
        error_is(fixture.core.prepare_execution(target, proposal, fixture.ordinal), CoreFailure::InvalidProposal);
        proposal.cursor = context.cursor;
    }
    proposal.resolved_price = std::numeric_limits<double>::infinity();
    error_is(fixture.core.prepare_execution(target, proposal, fixture.ordinal), CoreFailure::InvalidProposal);
    proposal.resolved_price = -1;
    const auto event = fixture.fill(target, proposal, observed({}));
    CHECK(event.resolved_price == -1);
    CHECK(event.provenance() == static_cast<uint8_t>(pineforge::NativePriceProvenance::CurrentExecution));
    CHECK(event.terminal_reason == AppliedTerminalReason::TargetExhausted);

    for (int mode = 0; mode < 3; ++mode) {
        Fixture rejected;
        auto request = selected();
        if (mode == 0) request.capacity = PointBudget{1};
        if (mode == 1) request.trigger = Limit{110};
        if (mode == 2) request.trigger = Stop{105};
        const auto unsupported = rejected.put(request);
        CHECK(!rejected.core.evaluation_eligible(*rejected.core.find_live(unsupported), point(99, true)));
    }
    CHECK(!fixture.core.trigger_permits_driver(Market{}, MarketReady{},
            static_cast<DriverEligibilityClass>(255), true));
}

void permutations_and_immutable_receipts() {
    std::vector<uint64_t> order{10, 20, 30};
    do {
        Fixture fixture;
        std::vector<RequestHandle> members;
        for (auto member : order) members.push_back(handle(member));
        const auto request = selected(9, members);
        auto target_observation = observed({10, 20, 30}, {30, 10, 20});
        const auto target = fixture.put(request, target_observation);
        const auto& accepted = std::get<AcceptedEvent>(fixture.core.history().front());
        const auto definition = accepted.definition;
        CHECK(std::get<BindOpenings>(definition->request.owner).openings ==
              (std::vector<RequestHandle>{handle(10), handle(20), handle(30)}));
        const auto context = fixture.evaluate(target, target_observation);
        // Every authorized live member is represented even when only the first
        // physical contributor produces a quantity-limited row.
        const auto applied = fixture.fill(target, fixture.proposal(context, target_observation, 1, order),
                                          target_observation, 1);
        CHECK(std::get<SelectedExposure>(applied.scope).incarnations == (std::vector<uint64_t>{10, 20, 30}));
        CHECK(applied.closed_trade_count == 1);
        CHECK(applied.definition == definition);
        CHECK(std::get<BindOpenings>(request.owner).openings == members);
        const auto cancellation = fixture.core.cancel(target, fixture.ordinal);
        CHECK(cancellation.status == CancelStatus::Cancelled);
        CHECK(std::get<BindOpenings>(applied.request().owner).openings.size() == 3);
        CHECK(std::get<OpeningsClose>(std::get<CancelledEvent>(fixture.core.history().back())
                                      .prior_authority).openings.size() == 3);
    } while (std::next_permutation(order.begin(), order.end()));

    // Cohort cardinality is determined by supplied provenance, with no 64 cap.
    Fixture large;
    std::vector<RequestHandle> members;
    std::vector<uint64_t> incarnations;
    auto observation = observed({}, {});
    for (uint64_t i = 1000; i < 1129; ++i) {
        members.push_back(handle(i));
        incarnations.push_back(i);
        observation.openings.push_back({handle(i), 7, observation.current_position, true});
    }
    std::reverse(members.begin(), members.end());
    const auto target = large.put(selected(1, members), observation);
    const auto context = large.evaluate(target, observation);
    const auto event = large.fill(target, large.proposal(context, observation, 1, incarnations), observation);
    CHECK(std::get<SelectedExposure>(event.scope).incarnations == incarnations);
    CHECK(std::get<BindOpenings>(event.request().owner).openings.size() == 129);
}
}  // namespace

int main() {
    static_assert(std::variant_size_v<Owner> == 5);
    static_assert(std::variant_size_v<Authority> == 8);
    static_assert(std::variant_size_v<ExecutionScope> == 3);
    static_assert(std::variant_size_v<ex::Action> == 3);
    static_assert(std::variant_size_v<ex::CloseScope> == 2);
    static_assert(static_cast<uint8_t>(DriverEligibilityClass::CurrentExecution) == 6);
    try {
        canonical_admission_and_invalid_attempts();
        exact_observations_and_scope();
        partial_budgets_fragments_and_exhaustion();
        expiry_and_post_observation_validation();
        replacement_and_parent_lifetime();
        aggregate_groups();
        current_guard_and_price_domain();
        permutations_and_immutable_receipts();
    } catch (const std::exception& error) {
        ++failures;
        std::printf("FAIL exception: %s\n", error.what());
    }
    std::printf("native selected core: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
