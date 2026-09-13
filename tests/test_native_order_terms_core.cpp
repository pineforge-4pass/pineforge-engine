// Native execution-terms core witnesses. This intentionally exercises only
// WorkingRequestCore values and prepared tokens: settlement/matcher behavior
// is owned by the phase-2 consumer tests.
#include <pineforge/native_order.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

static_assert(std::variant_size_v<no::OrderIntent> == 5);
static_assert(std::variant_size_v<no::Remaining> == 4);
static_assert(std::variant_size_v<no::RemainingProjection> == 4);
static_assert(std::variant_size_v<no::Allowance> == 4);
static_assert(std::variant_size_v<no::CommandEvent> == 17);
static_assert(std::variant_size_v<no::ExecutionPlan> == 4);
static_assert(std::variant_size_v<no::ExecutionScope> == 3);
static_assert(std::variant_size_v<no::TriggerState> == 9);
static_assert(std::is_same_v<decltype(no::to_execution_plan(ex::Action{ex::Flatten{}})),
                             no::ExecutionPlan>);

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

#define REQUIRE(condition) do { \
    const bool required = static_cast<bool>(condition); \
    CHECK(required); \
    if (!required) throw std::runtime_error("test prerequisite failed"); \
} while (0)

uint64_t bits(double value) {
    uint64_t result = 0;
    static_assert(sizeof result == sizeof value, "binary64 witness");
    std::memcpy(&result, &value, sizeof result);
    return result;
}

no::TargetObservation flat_book() {
    no::TargetObservation target;
    target.current_position = no::PositionFlat{};
    return target;
}

no::TargetObservation nonflat_book(int64_t cycle = 1,
                                   no::Side side = no::Side::Long) {
    no::TargetObservation target;
    target.current_position = no::PositionNonflat{cycle, side};
    return target;
}

no::EvaluationContext point(uint64_t ordinal) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = ordinal;
    context.cursor.point.effective_time_ms = 1000;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

no::TermsResolvedInput terms(double price, std::optional<double> units,
                             no::OpeningShape shape = no::OpeningShape::Transact) {
    no::TermsResolvedInput input;
    input.price_kind = no::NativeCandidatePriceKind::PointPrice;
    input.raw_price = price;
    input.default_resolved_price = price;
    input.terms.resolved_price = price;
    input.terms.units = units;
    input.terms.shape = shape;
    return input;
}

struct Fixture {
    no::RunIdentity run;
    no::WorkingRequestCore core;
    uint64_t incarnation = 1;
    uint64_t ordinal = 1;

    explicit Fixture(const char* name)
        : run{std::string("native-order-terms-core-") + name, 1}, core(run) {}

    no::SubmitResult submit(no::Request request, no::CommandContext context = {}) {
        context.decision_time_ms = context.decision_time_ms == 0 ? 1000 : context.decision_time_ms;
        auto prepared = core.prepare_submit(request, context, incarnation, ordinal);
        const no::SubmitResult predicted = prepared.predicted();
        auto installed = core.install_submit(std::move(prepared));
        REQUIRE(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed));
        const auto& result = std::get<no::CommandInstalled<no::SubmitResult>>(installed);
        ordinal += result.events.count;
        if (predicted.status == no::SubmitStatus::Accepted) {
            REQUIRE(predicted.handle.has_value());
            incarnation = predicted.handle->incarnation + 1;
        }
        return predicted;
    }

    no::EvaluationContext evaluate(const no::RequestHandle& target,
                                   const no::TargetObservation& observation,
                                   uint64_t at = 0) {
        const no::EvaluationContext context = point(at == 0 ? ordinal : at);
        auto prepared = core.prepare_evaluation(target, context, observation, ordinal);
        if (auto* mutation = std::get_if<no::PreparedMutation>(&prepared)) {
            auto installed = core.install_mutation(std::move(*mutation));
            REQUIRE(std::holds_alternative<no::Installed>(installed));
            ordinal += std::get<no::Installed>(installed).events.count;
        } else {
            REQUIRE(std::holds_alternative<no::NoChange>(prepared));
        }
        return context;
    }

    no::Installed install_mutation(no::Preparation<no::PreparedMutation>&& prepared) {
        REQUIRE(std::holds_alternative<no::PreparedMutation>(prepared));
        auto installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(prepared)));
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        const no::Installed result = std::get<no::Installed>(installed);
        ordinal += result.events.count;
        return result;
    }

    no::ExecutionAppliedEvent apply(const no::RequestHandle& target,
                                    const no::ExecutionProposal& proposal,
                                    no::TargetObservation after = nonflat_book()) {
        auto prepared = core.prepare_execution(target, proposal, ordinal);
        REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
        no::CommittedExecutionFacts facts;
        facts.result.status = ex::Status::Applied;
        facts.result.closed_units = proposal.inspected_closed_units;
        facts.result.opened_units = proposal.inspected_opened_units;
        facts.result.current_ticket = proposal.inspected_current_ticket;
        facts.result.first_trade_index = 7;
        facts.result.closed_trade_count = proposal.inspected_closed_units == 0.0 ? 0 : 1;
        facts.cycle_before = 1;
        facts.cycle_after = std::holds_alternative<no::PositionFlat>(after.current_position) ? 0 : 2;
        facts.post_target = std::move(after);
        facts.committed_action = proposal.physical_action;
        auto installed = core.install_execution(std::get<no::PreparedExecution>(std::move(prepared)), facts);
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ordinal += std::get<no::Installed>(installed).events.count;
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(core.history().back()));
        return std::get<no::ExecutionAppliedEvent>(core.history().back());
    }
};

template <class T>
void check_error(const no::Preparation<T>& result, no::CoreFailure code) {
    REQUIRE(std::holds_alternative<no::PreparationError>(result));
    CHECK(std::get<no::PreparationError>(result).code == code);
}

no::ExecutionProposal proposal_at(const no::EvaluationContext& context,
                                  no::ExecutionPlan plan,
                                  double closed,
                                  double opened,
                                  no::PositionIdentity before = no::PositionFlat{}) {
    no::ExecutionProposal proposal;
    proposal.cursor = context.cursor;
    proposal.raw_price = 100.0;
    proposal.resolved_price = 100.0;
    proposal.physical_action = std::move(plan);
    proposal.scope = ex::Book{};
    proposal.pre_fill = std::move(before);
    proposal.inspected_closed_units = closed;
    proposal.inspected_opened_units = opened;
    proposal.inspected_current_ticket = 3.0;
    return proposal;
}

void submission_rules() {
    Fixture fixture("submission");
    no::CommandContext market_only;
    market_only.surface = no::CommandSurface::MarketOnly;

    no::Request missing_side{no::HostSized{no::HostSizedKind::Open, std::nullopt}, "", ""};
    CHECK(fixture.submit(missing_side).reason == no::RequestRejectReason::InvalidQuantity);

    no::Request close_with_side{no::HostSized{no::HostSizedKind::Close, no::Side::Long}, "", ""};
    CHECK(fixture.submit(close_with_side).reason == no::RequestRejectReason::InvalidQuantity);

    no::Request market_host{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "", ""};
    CHECK(fixture.submit(market_host, market_only).reason == no::RequestRejectReason::InvalidQuantity);

    no::Request waiting_close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "", ""};
    waiting_close.owner = no::WaitForApplied{{fixture.run, 99}};
    CHECK(fixture.submit(waiting_close).reason == no::RequestRejectReason::InvalidOwner);

    no::Request bound_open{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "", ""};
    bound_open.owner = no::BindOpening{{fixture.run, 99}, 7};
    CHECK(fixture.submit(bound_open).reason == no::RequestRejectReason::InvalidOwner);

    no::Request sized_open{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "", ""};
    sized_open.trigger = no::Stop{101.0};
    sized_open.capacity = no::PointBudget{2.0};
    sized_open.group = no::Member{9, 1, no::GroupEffect::Reduce};
    const auto accepted_open = fixture.submit(sized_open);
    CHECK(accepted_open.status == no::SubmitStatus::Accepted);

    no::Request sized_close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "", ""};
    sized_close.trigger = no::Limit{99.0};
    sized_close.group = no::Member{9, 2, no::GroupEffect::Reduce};
    CHECK(fixture.submit(sized_close).status == no::SubmitStatus::Accepted);

    no::Request bound_close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "", ""};
    const no::RequestHandle opening{fixture.run, 777};
    bound_close.owner = no::BindOpening{opening, 7};
    no::CommandContext bound_context;
    bound_context.opening = no::OpeningObservation{
        opening, 7, no::PositionNonflat{7, no::Side::Long}, true};
    CHECK(fixture.submit(bound_close, bound_context).status == no::SubmitStatus::Accepted);

    no::Request reverse_market{no::ReverseTo{1.0}, "", ""};
    CHECK(fixture.submit(reverse_market, market_only).reason == no::RequestRejectReason::InvalidQuantity);

    no::Request replaceable{no::Transact{1.0}, "replaceable", ""};
    const auto replaceable_result = fixture.submit(replaceable);
    REQUIRE(replaceable_result.handle);
    auto prepared_replace = fixture.core.prepare_replace(*replaceable_result.handle, reverse_market,
                                                          market_only, fixture.incarnation,
                                                          fixture.ordinal);
    const auto predicted_replace = prepared_replace.predicted();
    auto installed_replace = fixture.core.install_replace(std::move(prepared_replace));
    REQUIRE(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(installed_replace));
    fixture.ordinal += std::get<no::CommandInstalled<no::ReplaceResult>>(installed_replace).events.count;
    CHECK(predicted_replace.status == no::ReplaceStatus::ReplaceRejected);
    CHECK(predicted_replace.reason == no::RequestRejectReason::InvalidQuantity);

    no::Request reverse_zero{no::ReverseTo{0.0}, "", ""};
    CHECK(fixture.submit(reverse_zero).reason == no::RequestRejectReason::InvalidQuantity);

    no::CommandContext half_grid;
    half_grid.quantity_grid = 0.5;
    no::Request reverse_off_grid{no::ReverseTo{0.3}, "", ""};
    CHECK(fixture.submit(reverse_off_grid, half_grid).reason == no::RequestRejectReason::OffGrid);

    no::Request reverse_group{no::ReverseTo{1.0}, "", ""};
    reverse_group.group = no::Member{10, 1, no::GroupEffect::Reduce};
    CHECK(fixture.submit(reverse_group).reason == no::RequestRejectReason::InvalidGroup);

    no::Request reverse_budget{no::ReverseTo{1.0}, "", ""};
    reverse_budget.capacity = no::PointBudget{1.0};
    CHECK(fixture.submit(reverse_budget).reason == no::RequestRejectReason::InvalidCapacity);

    no::Request reverse_bound{no::ReverseTo{1.0}, "", ""};
    reverse_bound.owner = no::BindOpening{{fixture.run, 1}, 1};
    CHECK(fixture.submit(reverse_bound).reason == no::RequestRejectReason::InvalidOwner);

    no::Request reverse{no::ReverseTo{-1.0}, "", ""};
    reverse.trigger = no::Trail{1.0, std::nullopt};
    CHECK(fixture.submit(reverse).status == no::SubmitStatus::Accepted);
}

void deferred_allowance_and_binding() {
    Fixture host("deferred-allowance");
    no::Request request{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "host", ""};
    request.capacity = no::PointBudget{2.0};
    const auto submitted = host.submit(request);
    REQUIRE(submitted.handle);
    const auto eval = host.evaluate(*submitted.handle, flat_book());
    const auto* deferred = std::get_if<no::AllowanceDeferred>(
        &host.core.find_live(*submitted.handle)->allowance);
    REQUIRE(deferred);
    CHECK(deferred->point_ordinal == eval.cursor.point.ordinal);
    const auto expected = no::WorkingRequestCore::evaluated_allowance(
        *host.core.find_live(*submitted.handle), eval.cursor.point.ordinal);
    CHECK(expected.index() == host.core.find_live(*submitted.handle)->allowance.index());
    CHECK(std::get<no::AllowanceDeferred>(expected).point_ordinal == deferred->point_ordinal);

    auto premature = proposal_at(eval, no::ExecutionPlan{no::Transact{1.0}}, 0.0, 1.0);
    check_error(host.core.prepare_execution(*submitted.handle, premature, host.ordinal),
                no::CoreFailure::InvalidProposal);

    auto duplicate = host.core.prepare_evaluation(*submitted.handle, eval, flat_book(), host.ordinal);
    REQUIRE(std::holds_alternative<no::NoChange>(duplicate));
    CHECK(std::get<no::NoChange>(duplicate).reason == no::NoChangeReason::NoTransition);

    const auto history_before = host.core.history().size();
    auto binding = host.core.prepare_terms(*submitted.handle, eval, terms(100.0, 5.0), host.ordinal);
    const auto installed = host.install_mutation(std::move(binding));
    CHECK(installed.events.count == 1);
    CHECK(host.core.history().size() == history_before + 1);
    const auto& receipt = std::get<no::TermsResolvedEvent>(host.core.history().back());
    CHECK(std::holds_alternative<no::RemainingProjectionDeferred>(receipt.remaining_before));
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(receipt.remaining_after));
    CHECK(std::get<no::RemainingProjectionUnits>(receipt.remaining_after).q == 5.0);
    const auto* bound = host.core.find_live(*submitted.handle);
    REQUIRE(bound);
    REQUIRE(std::holds_alternative<no::RemainingUnits>(bound->remaining));
    CHECK(std::get<no::RemainingUnits>(bound->remaining).q == 5.0);
    const auto* host_allowance = std::get_if<no::AllowanceUnits>(&bound->allowance);
    REQUIRE(host_allowance);
    CHECK(host_allowance->initial == 2.0 && host_allowance->left == 2.0);

    Fixture explicit_request("explicit-allowance");
    no::Request explicit_open{no::Transact{5.0}, "explicit", ""};
    explicit_open.capacity = no::PointBudget{2.0};
    const auto explicit_submitted = explicit_request.submit(explicit_open);
    REQUIRE(explicit_submitted.handle);
    const auto explicit_eval = explicit_request.evaluate(*explicit_submitted.handle, flat_book(),
                                                         eval.cursor.point.ordinal);
    (void) explicit_eval;
    const auto* explicit_allowance = std::get_if<no::AllowanceUnits>(
        &explicit_request.core.find_live(*explicit_submitted.handle)->allowance);
    REQUIRE(explicit_allowance);
    CHECK(host_allowance->point_ordinal == explicit_allowance->point_ordinal);
    CHECK(host_allowance->initial == explicit_allowance->initial);
    CHECK(host_allowance->left == explicit_allowance->left);

    auto resized = host.core.prepare_terms(*submitted.handle, eval, terms(101.0, 1.0), host.ordinal);
    check_error(resized, no::CoreFailure::InvalidProposal);
    auto price_only = host.core.prepare_terms(*submitted.handle, eval, terms(101.0, std::nullopt),
                                              host.ordinal);
    host.install_mutation(std::move(price_only));
    const auto* after_price_only = host.core.find_live(*submitted.handle);
    REQUIRE(after_price_only);
    CHECK(std::get<no::RemainingUnits>(after_price_only->remaining).q == 5.0);
    CHECK(std::get<no::AllowanceUnits>(after_price_only->allowance).left == 2.0);
}

void zero_paths_and_rejections() {
    for (const auto grid : {std::optional<double>{}, std::optional<double>{0.25}}) {
        Fixture fixture(grid ? "zero-grid" : "zero-no-grid");
        no::Request request{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "zero", ""};
        no::CommandContext context;
        context.quantity_grid = grid;
        const auto submitted = fixture.submit(request, context);
        REQUIRE(submitted.handle);
        const auto eval = fixture.evaluate(*submitted.handle, flat_book());
        const double negative_zero = -0.0;
        auto prepared = fixture.core.prepare_terms(
            *submitted.handle, eval, terms(100.0, negative_zero, no::OpeningShape::CloseOpposite),
            fixture.ordinal);
        const auto installed = fixture.install_mutation(std::move(prepared));
        CHECK(installed.events.count == 2);
        REQUIRE(std::holds_alternative<no::TermsResolvedEvent>(
            fixture.core.history()[fixture.core.history().size() - 2]));
        const auto& receipt = std::get<no::TermsResolvedEvent>(
            fixture.core.history()[fixture.core.history().size() - 2]);
        REQUIRE(receipt.input.terms.units);
        CHECK(bits(*receipt.input.terms.units) == bits(negative_zero));
        CHECK(std::holds_alternative<no::NoEffectEvent>(fixture.core.history().back()));
        CHECK(!fixture.core.find_live(*submitted.handle));
    }

    Fixture fixture("rejected-attempted-terms");
    const auto submitted = fixture.submit(no::Request{no::Transact{1.0}, "explicit", ""});
    REQUIRE(submitted.handle);
    const auto eval = fixture.evaluate(*submitted.handle, flat_book());
    const no::ExecutionTerms attempted{99.0, 1.0, no::OpeningShape::ReverseTo};
    auto rejected = fixture.core.prepare_match_rejected(*submitted.handle, eval,
        no::MatchRejectReason::InvalidTerms, attempted, fixture.ordinal);
    fixture.install_mutation(std::move(rejected));
    const auto& event = std::get<no::MatchRejectedEvent>(fixture.core.history().back());
    REQUIRE(event.attempted_terms);
    CHECK(event.attempted_terms->resolved_price == attempted.resolved_price);
    REQUIRE(event.attempted_terms->units);
    CHECK(*event.attempted_terms->units == *attempted.units);
    CHECK(event.attempted_terms->shape == attempted.shape);
}

struct PendingGroup {
    Fixture fixture{"pending-group"};
    no::RequestHandle a;
    no::RequestHandle b;
    no::EvaluationContext b_context;

    PendingGroup() : a{}, b{} {
        no::Request first{no::Transact{0.1}, "a", ""};
        first.group = no::Member{44, 1, no::GroupEffect::Reduce};
        no::Request second{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "b", ""};
        second.group = no::Member{44, 2, no::GroupEffect::Reduce};
        const auto a_result = fixture.submit(first);
        const auto b_result = fixture.submit(second);
        REQUIRE(a_result.handle && b_result.handle);
        a = *a_result.handle;
        b = *b_result.handle;

        const auto a_context = fixture.evaluate(a, flat_book());
        auto applied_proposal = proposal_at(a_context, no::ExecutionPlan{no::Transact{0.1}}, 0.0, 0.1);
        const auto applied = fixture.apply(a, applied_proposal, nonflat_book(1, no::Side::Long));
        CHECK(applied.filled_working == 0.1);
        const auto* a_member = std::get_if<no::Member>(&applied.request().group);
        REQUIRE(a_member);
        CHECK(a_member->group == 44 && a_member->cohort == 1
              && a_member->effect == no::GroupEffect::Reduce);
        const no::EventId applied_id{fixture.run, applied.ordinal};
        auto deferred = fixture.core.prepare_group_effect(applied_id, b, fixture.ordinal);
        fixture.install_mutation(std::move(deferred));

        const auto* live = fixture.core.find_live(b);
        REQUIRE(live);
        const auto* pending = std::get_if<no::PendingDeferred>(&live->pending);
        REQUIRE(pending);
        CHECK(pending->total == 0.1);
        CHECK(pending->count == 1);
        const auto* deferred_event = fixture.core.event_at(pending->tail_receipt);
        REQUIRE(deferred_event);
        REQUIRE(std::holds_alternative<no::DeferredGroupAdjustmentEvent>(*deferred_event));
        const auto& authentic = std::get<no::DeferredGroupAdjustmentEvent>(*deferred_event);
        CHECK(authentic.recipient == b);
        CHECK(authentic.effect == no::GroupEffect::Reduce);
        const auto* b_member = std::get_if<no::Member>(&live->request().group);
        REQUIRE(b_member);
        CHECK(b_member->group == 44 && b_member->cohort == 2
              && b_member->effect == no::GroupEffect::Reduce);
        b_context = fixture.evaluate(b, flat_book());
        REQUIRE(std::holds_alternative<no::AllowanceDeferred>(
            fixture.core.find_live(b)->allowance));
    }
};

void authenticated_deduction_and_terminals() {
    PendingGroup partial;
    const auto* before = partial.fixture.core.find_live(partial.b);
    REQUIRE(before);
    double deduction = -1.0;
    double after = -1.0;
    bool exhausted = true;
    CHECK(no::WorkingRequestCore::effective_host_units(before->pending, 0.25,
                                                       &deduction, &after, &exhausted));
    CHECK(deduction == 0.1 && after == 0.15 && !exhausted);

    auto prepared = partial.fixture.core.prepare_terms(
        partial.b, partial.b_context, terms(100.0, 0.25), partial.fixture.ordinal);
    partial.fixture.install_mutation(std::move(prepared));
    const auto& receipt = std::get<no::TermsResolvedEvent>(partial.fixture.core.history().back());
    CHECK(receipt.pending_total == 0.1);
    CHECK(receipt.effective_deduction == 0.1);
    CHECK(receipt.prior_adjustment_ids.size() == 1);
    const auto* bound = partial.fixture.core.find_live(partial.b);
    REQUIRE(bound);
    REQUIRE(std::holds_alternative<no::RemainingUnits>(bound->remaining));
    CHECK(std::get<no::RemainingUnits>(bound->remaining).q == 0.15);
    CHECK(std::holds_alternative<no::PendingNone>(bound->pending));

    PendingGroup exhausted_group;
    const auto history_before = exhausted_group.fixture.core.history().size();
    auto prepared_exhausted = exhausted_group.fixture.core.prepare_terms(
        exhausted_group.b, exhausted_group.b_context, terms(100.0, 0.1),
        exhausted_group.fixture.ordinal);
    const auto installed = exhausted_group.fixture.install_mutation(std::move(prepared_exhausted));
    CHECK(installed.events.count == 2);
    CHECK(exhausted_group.fixture.core.history().size() == history_before + 2);
    const auto& receipt_event = std::get<no::TermsResolvedEvent>(
        exhausted_group.fixture.core.history()[history_before]);
    const auto& cancelled = std::get<no::CancelledEvent>(
        exhausted_group.fixture.core.history()[history_before + 1]);
    CHECK(cancelled.reason == no::CancelReason::Group);
    REQUIRE(cancelled.cause);
    CHECK(cancelled.cause->ordinal == receipt_event.ordinal);
    CHECK(!exhausted_group.fixture.core.find_live(exhausted_group.b));
}

void numeric_failure_and_chain_authentication() {
    PendingGroup unrepresentable;
    const auto* live = unrepresentable.fixture.core.find_live(unrepresentable.b);
    REQUIRE(live);
    double deduction = 17.0;
    double after = 19.0;
    bool exhausted = true;
    CHECK(!no::WorkingRequestCore::effective_host_units(live->pending, 1e16,
                                                        &deduction, &after, &exhausted));
    CHECK(deduction == 17.0 && after == 19.0 && exhausted);
    CHECK(no::WorkingRequestCore::effective_host_units(live->pending, 0.0,
                                                       &deduction, &after, &exhausted));
    CHECK(deduction == 0.0 && after == 0.0 && exhausted);
    CHECK(no::WorkingRequestCore::effective_host_units(no::PendingNone{}, 0.5,
                                                       &deduction, &after, &exhausted));
    CHECK(deduction == 0.0 && after == 0.5 && !exhausted);

    const auto history_size = unrepresentable.fixture.core.history().size();
    const auto ordinal = unrepresentable.fixture.ordinal;
    const auto pending = live->pending;
    const auto remaining = live->remaining;
    const auto allowance = live->allowance;
    const auto failed = unrepresentable.fixture.core.prepare_terms(
        unrepresentable.b, unrepresentable.b_context, terms(100.0, 1e16),
        unrepresentable.fixture.ordinal);
    check_error(failed, no::CoreFailure::UnrepresentableReservation);
    const auto& error = std::get<no::PreparationError>(failed);
    CHECK(error.cause.run == unrepresentable.fixture.run && error.cause.ordinal == 0);
    CHECK(unrepresentable.fixture.core.history().size() == history_size);
    CHECK(unrepresentable.fixture.ordinal == ordinal);
    const auto* after_failure = unrepresentable.fixture.core.find_live(unrepresentable.b);
    REQUIRE(after_failure);
    CHECK(after_failure->pending.index() == pending.index());
    CHECK(after_failure->remaining.index() == remaining.index());
    CHECK(after_failure->allowance.index() == allowance.index());

    const auto invalid = unrepresentable.fixture.core.prepare_terms(
        unrepresentable.b, unrepresentable.b_context,
        terms(100.0, std::numeric_limits<double>::quiet_NaN()), unrepresentable.fixture.ordinal);
    check_error(invalid, no::CoreFailure::InvalidProposal);

    PendingGroup corrupt;
    auto* forged = const_cast<no::LiveRequest*>(corrupt.fixture.core.find_live(corrupt.b));
    REQUIRE(forged);
    auto& pending_deferred = std::get<no::PendingDeferred>(forged->pending);
    pending_deferred.tail_receipt.ordinal += 1000;
    const auto corrupt_history = corrupt.fixture.core.history().size();
    const auto conflict = corrupt.fixture.core.prepare_terms(
        corrupt.b, corrupt.b_context, terms(100.0, 0.25), corrupt.fixture.ordinal);
    check_error(conflict, no::CoreFailure::ConflictingReceipt);
    CHECK(corrupt.fixture.core.history().size() == corrupt_history);
}

void plan_keyed_accounting_and_shape_validation() {
    Fixture reverse_fixture("reverse");
    const auto reverse = reverse_fixture.submit(no::Request{no::ReverseTo{1.0}, "reverse", ""});
    REQUIRE(reverse.handle);
    const auto reverse_context = reverse_fixture.evaluate(*reverse.handle, nonflat_book(1, no::Side::Short));
    auto reversal = proposal_at(reverse_context, no::ExecutionPlan{ex::ReverseTo{1.0}}, 10.0, 1.0,
                                no::PositionNonflat{1, no::Side::Short});
    const auto applied = reverse_fixture.apply(*reverse.handle, reversal,
                                               nonflat_book(2, no::Side::Long));
    CHECK(applied.filled_working == 11.0);
    CHECK(applied.filled_working > std::get<no::RemainingProjectionUnits>(applied.remaining_before).q);
    CHECK(applied.terminal_reason == no::AppliedTerminalReason::WorkingUnitsSatisfied);
    REQUIRE(std::holds_alternative<no::AllowanceUnits>(applied.allowance_after));
    const auto& allowance_after = std::get<no::AllowanceUnits>(applied.allowance_after);
    CHECK(allowance_after.point_ordinal == reverse_context.cursor.point.ordinal);
    CHECK(allowance_after.initial == 1.0);
    CHECK(allowance_after.left == 0.0);
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(applied.remaining_after));
    CHECK(std::get<no::RemainingProjectionUnits>(applied.remaining_after).q == 0.0);

    Fixture reverse_partial("reverse-partial");
    const auto partial = reverse_partial.submit(no::Request{no::ReverseTo{1.0}, "reverse", ""});
    REQUIRE(partial.handle);
    const auto partial_context = reverse_partial.evaluate(*partial.handle, nonflat_book(1, no::Side::Short));
    auto partial_plan = proposal_at(partial_context, no::ExecutionPlan{ex::ReverseTo{0.5}}, 10.0, 0.5,
                                    no::PositionNonflat{1, no::Side::Short});
    check_error(reverse_partial.core.prepare_execution(*partial.handle, partial_plan,
                                                       reverse_partial.ordinal),
                no::CoreFailure::InvalidProposal);

    Fixture absorbed_reverse("absorbed-reverse");
    const auto tiny = absorbed_reverse.submit(no::Request{no::ReverseTo{-0.1}, "tiny", ""});
    REQUIRE(tiny.handle);
    const auto tiny_context = absorbed_reverse.evaluate(*tiny.handle, nonflat_book(1, no::Side::Long));
    auto tiny_plan = proposal_at(tiny_context, no::ExecutionPlan{ex::ReverseTo{-0.1}}, 1e16, -0.1,
                                 no::PositionNonflat{1, no::Side::Long});
    const auto tiny_applied = absorbed_reverse.apply(*tiny.handle, tiny_plan,
                                                      nonflat_book(2, no::Side::Short));
    CHECK(tiny_applied.filled_working == 1e16);
    CHECK(bits(tiny_applied.opened_units) == bits(-0.1));
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(tiny_applied.remaining_after));
    CHECK(std::get<no::RemainingProjectionUnits>(tiny_applied.remaining_after).q == 0.0);

    Fixture transact_reject("transact-overflow");
    const auto transaction = transact_reject.submit(no::Request{no::Transact{0.1}, "tx", ""});
    REQUIRE(transaction.handle);
    const auto transaction_context = transact_reject.evaluate(*transaction.handle,
                                                               nonflat_book(1, no::Side::Short));
    auto overflow = proposal_at(transaction_context, no::ExecutionPlan{no::Transact{0.1}}, 1e16, 0.1,
                                no::PositionNonflat{1, no::Side::Short});
    check_error(transact_reject.core.prepare_execution(*transaction.handle, overflow,
                                                        transact_reject.ordinal),
                no::CoreFailure::NonrepresentableQuantity);

    Fixture reduce_reject("reduce-over-cap");
    const auto reduction = reduce_reject.submit(
        no::Request{no::Reduce{no::ExplicitUnits{1.0}}, "reduce", ""});
    REQUIRE(reduction.handle);
    const auto reduction_context = reduce_reject.evaluate(*reduction.handle,
                                                           nonflat_book(1, no::Side::Long));
    auto excessive_reduce = proposal_at(reduction_context,
                                        no::ExecutionPlan{pineforge::order_action::Reduce{1.0}},
                                        2.0, 0.0, no::PositionNonflat{1, no::Side::Long});
    check_error(reduce_reject.core.prepare_execution(*reduction.handle, excessive_reduce,
                                                      reduce_reject.ordinal),
                no::CoreFailure::InvalidProposal);

    Fixture whole_close("whole-close");
    no::Request sized{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "whole", ""};
    const auto host = whole_close.submit(sized);
    REQUIRE(host.handle);
    const auto host_context = whole_close.evaluate(*host.handle, flat_book());
    whole_close.install_mutation(whole_close.core.prepare_terms(
        *host.handle, host_context, terms(100.0, 1.0, no::OpeningShape::CloseOpposite),
        whole_close.ordinal));
    const auto* bound = whole_close.core.find_live(*host.handle);
    REQUIRE(bound);
    auto flatten = proposal_at(host_context, no::ExecutionPlan{ex::Flatten{}}, 10.0, 0.0,
                               no::PositionNonflat{1, no::Side::Short});
    const auto flattened = whole_close.apply(*host.handle, flatten, flat_book());
    CHECK(flattened.filled_working == 10.0);
    CHECK(flattened.filled_working
          > std::get<no::RemainingProjectionUnits>(flattened.remaining_before).q);
    CHECK(flattened.terminal_reason == no::AppliedTerminalReason::WorkingUnitsSatisfied);
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(flattened.remaining_after));
    CHECK(std::get<no::RemainingProjectionUnits>(flattened.remaining_after).q == 0.0);

    Fixture budget("budget-shape");
    no::Request budget_request{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "budget", ""};
    budget_request.capacity = no::PointBudget{2.0};
    const auto budget_host = budget.submit(budget_request);
    REQUIRE(budget_host.handle);
    const auto budget_context = budget.evaluate(*budget_host.handle, flat_book());
    check_error(budget.core.prepare_terms(
                    *budget_host.handle, budget_context,
                    terms(100.0, 3.0, no::OpeningShape::ReverseTo), budget.ordinal),
                no::CoreFailure::InvalidProposal);
    budget.install_mutation(budget.core.prepare_terms(
        *budget_host.handle, budget_context, terms(100.0, 3.0), budget.ordinal));
    auto budget_transact = proposal_at(budget_context, no::ExecutionPlan{no::Transact{2.0}}, 0.0, 2.0);
    REQUIRE(std::holds_alternative<no::PreparedExecution>(
        budget.core.prepare_execution(*budget_host.handle, budget_transact, budget.ordinal)));
    auto budget_reverse = proposal_at(budget_context, no::ExecutionPlan{ex::ReverseTo{3.0}}, 3.0, 3.0,
                                      no::PositionNonflat{1, no::Side::Short});
    check_error(budget.core.prepare_execution(*budget_host.handle, budget_reverse, budget.ordinal),
                no::CoreFailure::InvalidProposal);
    auto budget_flatten = proposal_at(budget_context, no::ExecutionPlan{ex::Flatten{}}, 3.0, 0.0,
                                      no::PositionNonflat{1, no::Side::Short});
    check_error(budget.core.prepare_execution(*budget_host.handle, budget_flatten, budget.ordinal),
                no::CoreFailure::InvalidProposal);

    Fixture close_shape("close-shape");
    const auto close = close_shape.submit(
        no::Request{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "close", ""});
    REQUIRE(close.handle);
    const auto close_context = close_shape.evaluate(*close.handle, nonflat_book(1, no::Side::Long));
    check_error(close_shape.core.prepare_terms(*close.handle, close_context,
                                               terms(100.0, 1.0, no::OpeningShape::ReverseTo),
                                               close_shape.ordinal),
                no::CoreFailure::InvalidProposal);

    Fixture explicit_shape("explicit-shape");
    const auto explicit_target = explicit_shape.submit(no::Request{no::Transact{1.0}, "tx", ""});
    REQUIRE(explicit_target.handle);
    const auto explicit_context = explicit_shape.evaluate(*explicit_target.handle, flat_book());
    check_error(explicit_shape.core.prepare_terms(*explicit_target.handle, explicit_context,
                                                  terms(100.0, std::nullopt,
                                                        no::OpeningShape::CloseOpposite),
                                                  explicit_shape.ordinal),
                no::CoreFailure::InvalidProposal);
}

}  // namespace

int main() {
    try {
        submission_rules();
        deferred_allowance_and_binding();
        zero_paths_and_rejections();
        authenticated_deduction_and_terminals();
        numeric_failure_and_chain_authentication();
        plan_keyed_accounting_and_shape_validation();
    } catch (const std::exception& error) {
        ++failures;
        std::printf("FAIL exception: %s\n", error.what());
    }
    std::printf("%s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
