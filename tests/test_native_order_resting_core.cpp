#include <pineforge/native_order.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <variant>

using pineforge::execution::Flatten;
using pineforge::native_order::ActivateStop;
using pineforge::native_order::AllowanceUnits;
using pineforge::native_order::AppliedTerminalReason;
using pineforge::native_order::ArmedEvent;
using pineforge::native_order::ArmedTransaction;
using pineforge::native_order::BindOpening;
using pineforge::native_order::BookClose;
using pineforge::native_order::CancelReason;
using pineforge::native_order::CancelledEvent;
using pineforge::native_order::CloseBoundEvent;
using pineforge::native_order::CommandContext;
using pineforge::native_order::CommandEvent;
using pineforge::native_order::CommandInstalled;
using pineforge::native_order::CommandSurface;
using pineforge::native_order::CommittedExecutionFacts;
using pineforge::native_order::DeferredGroupAdjustmentEvent;
using pineforge::native_order::DriverEligibilityClass;
using pineforge::native_order::EvaluationContext;
using pineforge::native_order::EventId;
using pineforge::native_order::ExecutionAppliedEvent;
using pineforge::native_order::ExecutionProposal;
using pineforge::native_order::ExplicitUnits;
using pineforge::native_order::GroupEffect;
using pineforge::native_order::Independent;
using pineforge::native_order::Installed;
using pineforge::native_order::InstallError;
using pineforge::native_order::Limit;
using pineforge::native_order::Market;
using pineforge::native_order::Member;
using pineforge::native_order::NoChange;
using pineforge::native_order::NoChangeReason;
using pineforge::native_order::NoEffectEvent;
using pineforge::native_order::OpeningClose;
using pineforge::native_order::OpeningObservation;
using pineforge::native_order::OwnerOpenedUnits;
using pineforge::native_order::PointBudget;
using pineforge::native_order::Preparation;
using pineforge::native_order::PositionFlat;
using pineforge::native_order::PositionNonflat;
using pineforge::native_order::PreparedExecution;
using pineforge::native_order::PreparedMutation;
using pineforge::native_order::PreparedSubmit;
using pineforge::native_order::RejectedEvent;
using pineforge::native_order::ReplaceRejectedEvent;
using pineforge::native_order::ReplaceStatus;
using pineforge::native_order::PreparationError;
using pineforge::native_order::QuantityBoundEvent;
using pineforge::native_order::Reduce;
using pineforge::native_order::RemainingUnbound;
using pineforge::native_order::RemainingUnits;
using pineforge::native_order::Request;
using pineforge::native_order::RequestHandle;
using pineforge::native_order::RequestRejectReason;
using pineforge::native_order::ReservationReducedEvent;
using pineforge::native_order::RunIdentity;
using pineforge::native_order::Side;
using pineforge::native_order::Stop;
using pineforge::native_order::SubmitStatus;
using pineforge::native_order::TargetObservation;
using pineforge::native_order::Transact;
using pineforge::native_order::Wait;
using pineforge::native_order::WaitForApplied;
using pineforge::native_order::WorkingRequestCore;
using pineforge::native_order::has_market_defaults;
using pineforge::native_order::market_request;

std::atomic<int> g_allocs{0};

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* operator new(std::size_t n, std::align_val_t a) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<size_t>(a), n ? n : 1) != 0) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

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

const RunIdentity kRun{"resting", 1};

EvaluationContext print_ctx(uint64_t point, int64_t time_ms = 1000, bool matching = true) {
    EvaluationContext context;
    context.cursor.point.ordinal = point;
    context.cursor.point.effective_time_ms = time_ms;
    context.driver_class = DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = matching;
    return context;
}

TargetObservation long_book(int64_t cycle = 7) {
    TargetObservation observation;
    observation.current_position = PositionNonflat{cycle, Side::Long};
    return observation;
}

TargetObservation flat_book() {
    TargetObservation observation;
    observation.current_position = PositionFlat{};
    return observation;
}

Request market_tx(double q, std::string label = {}) {
    return Request{Transact{q}, std::move(label), {}};
}

uint64_t commit_mutation(WorkingRequestCore& core, Preparation<PreparedMutation> prep,
                         uint64_t& ordinal) {
    CHECK(std::holds_alternative<PreparedMutation>(prep));
    auto installed = core.install_mutation(std::get<PreparedMutation>(std::move(prep)));
    CHECK(std::holds_alternative<Installed>(installed));
    const auto count = std::get<Installed>(installed).events.count;
    ordinal += count;
    return count;
}

ExecutionAppliedEvent fill_transact(WorkingRequestCore& core, const RequestHandle& handle,
                                    double filled, uint64_t& ordinal, double remaining_after,
                                    bool terminal, int64_t cycle_before, int64_t cycle_after,
                                    double opened = 0.0) {
    const auto* live = core.find_live(handle);
    CHECK(live);
    uint64_t point = 0;
    if (const auto* units = std::get_if<AllowanceUnits>(&live->allowance)) {
        point = units->point_ordinal;
    }
    ExecutionProposal proposal;
    proposal.cursor.point.ordinal = point;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    const auto* tx = std::get_if<Transact>(&live->request().intent);
    const double signed_fill = (tx && tx->signed_units < 0.0) ? -filled : filled;
    proposal.physical_action = Transact{signed_fill};
    proposal.scope = pineforge::execution::Book{};
    if (cycle_before == 0) proposal.pre_fill = PositionFlat{};
    else proposal.pre_fill = PositionNonflat{cycle_before, Side::Long};
    proposal.inspected_closed_units = opened != 0.0 ? std::abs(filled - std::abs(opened)) : 0.0;
    if (opened == 0.0 && cycle_after == 0 && cycle_before != 0) {
        proposal.inspected_closed_units = filled;
    } else if (opened != 0.0) {
        proposal.inspected_closed_units = std::max(0.0, filled - std::abs(opened));
        proposal.inspected_opened_units = opened;
    } else {
        proposal.inspected_opened_units = filled;
        proposal.inspected_closed_units = 0.0;
    }
    proposal.inspected_current_ticket = 1;
    auto prep = core.prepare_execution(handle, proposal, ordinal);
    CHECK(std::holds_alternative<PreparedExecution>(prep));
    CommittedExecutionFacts facts;
    facts.result.status = pineforge::execution::Status::Applied;
    facts.result.closed_units = proposal.inspected_closed_units;
    facts.result.opened_units = proposal.inspected_opened_units;
    facts.result.current_ticket = 1;
    facts.cycle_before = cycle_before;
    facts.cycle_after = cycle_after;
    facts.post_target = cycle_after == 0 ? flat_book() : long_book(cycle_after);
    facts.committed_action = proposal.physical_action;
    auto installed = core.install_execution(std::get<PreparedExecution>(std::move(prep)), facts);
    CHECK(std::holds_alternative<Installed>(installed));
    ordinal += std::get<Installed>(installed).events.count;
    CHECK(std::holds_alternative<ExecutionAppliedEvent>(core.history().back()));
    const auto& applied = std::get<ExecutionAppliedEvent>(core.history().back());
    CHECK(applied.terminal == terminal);
    (void)remaining_after;
    return applied;
}

void validation_matrix_and_market_defaults() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    CHECK(has_market_defaults(market_tx(1.0, "m")));
    CHECK(core.submit(market_request(pineforge::order_action::Transact{2.0}, "phys"), 1, inc, ord)
                  .status
          == SubmitStatus::Accepted);

    Request bad_basis{Reduce{OwnerOpenedUnits{}}, "oo", "", Market{},
                      pineforge::native_order::ImmediateRemaining{}, Independent{}};
    const auto rejected = core.submit(bad_basis, 1, inc, ord);
    CHECK(rejected.status == SubmitStatus::Rejected);
    CHECK(rejected.reason == RequestRejectReason::InvalidQuantityBasis);

    Request wait_tx{Transact{1.0}, "wtx", "", Market{}, pineforge::native_order::ImmediateRemaining{},
                    WaitForApplied{core.live().front().handle()}};
    wait_tx.intent = Transact{1.0};
    wait_tx.owner = WaitForApplied{core.live().front().handle()};
    Request wait_oo = wait_tx;
    wait_oo.intent = Reduce{OwnerOpenedUnits{}};
    wait_oo.owner = WaitForApplied{core.live().front().handle()};
    const auto child = core.submit(wait_oo, 1, inc, ord);
    CHECK(child.status == SubmitStatus::Accepted);
    CHECK(std::holds_alternative<RemainingUnbound>(core.find_live(*child.handle)->remaining));
    CHECK(std::holds_alternative<Wait>(core.find_live(*child.handle)->authority));

    Request wait_tx_oo{Transact{1.0}, "bad", ""};
    wait_tx_oo.owner = WaitForApplied{*child.handle};
    wait_tx_oo.intent = Transact{1.0};
    // OwnerOpened is only on Reduce; Transact cannot carry it. InvalidQuantityBasis via
    // Independent+OwnerOpened already covered. Wait+Transact is accepted.
    Request flatten_budget{Flatten{}, "fb", ""};
    flatten_budget.capacity = PointBudget{1.0};
    CHECK(core.submit(flatten_budget, 1, inc, ord).reason == RequestRejectReason::InvalidCapacity);

    Request bad_trig{Transact{1.0}, "bt", ""};
    bad_trig.trigger = Stop{0.0};
    CHECK(core.submit(bad_trig, 1, inc, ord).reason == RequestRejectReason::InvalidTrigger);

    Request bad_group{Transact{1.0}, "bg", ""};
    bad_group.group = Member{0, 1, GroupEffect::Cancel};
    CHECK(core.submit(bad_group, 1, inc, ord).reason == RequestRejectReason::InvalidGroup);

    Request bind_tx{Transact{1.0}, "bx", ""};
    bind_tx.owner = BindOpening{core.live().front().handle(), 7};
    CHECK(core.submit(bind_tx, 1, inc, ord).reason == RequestRejectReason::InvalidOwner);
}

void bind_opening_and_replace_predecessor_wait() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto parent = core.submit(market_tx(1.0, "p"), 1, inc, ord);
    OpeningObservation opening;
    opening.queried_opening = *parent.handle;
    opening.queried_cycle = 7;
    opening.current_position = PositionNonflat{7, Side::Long};
    opening.has_live_matching_lot = true;
    Request child{Reduce{ExplicitUnits{1.0}}, "c", ""};
    child.owner = BindOpening{*parent.handle, 7};
    auto prepared = core.prepare_submit(child, pineforge::native_order::CommandContext{10, std::nullopt, opening},
                                        inc, ord);
    CHECK(prepared.predicted().status == SubmitStatus::Accepted);
    auto installed = core.install_submit(std::move(prepared));
    CHECK(std::holds_alternative<pineforge::native_order::CommandInstalled<
                  pineforge::native_order::SubmitResult>>(installed));
    ++ord;
    ++inc;
    CHECK(std::holds_alternative<OpeningClose>(core.live().back().authority));

    Request wait_on_self{Transact{-1.0}, "ws", ""};
    wait_on_self.owner = WaitForApplied{*parent.handle};
    const auto replaced = core.replace(*parent.handle, wait_on_self, 20, inc, ord);
    CHECK(replaced.status == pineforge::native_order::ReplaceStatus::ReplaceRejected);
    CHECK(replaced.reason == RequestRejectReason::InvalidOwner);
    CHECK(core.find_live(*parent.handle));
}

void c9_eligibility_does_not_mutate() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto tx = core.submit(market_tx(1.0, "t"), 1, inc, ord);
    const auto rd = core.submit(Request{Reduce{ExplicitUnits{1.0}}, "r", ""}, 1, inc, ord);
    const std::size_t live_n = core.live().size();
    const std::size_t hist_n = core.history().size();
    const auto ctx = print_ctx(3, 1, true);
    CHECK(core.evaluation_eligible(*core.find_live(*tx.handle), ctx));
    CHECK(core.evaluation_eligible(*core.find_live(*rd.handle), ctx));
    CHECK(core.live().size() == live_n);
    CHECK(core.history().size() == hist_n);
    CHECK(std::holds_alternative<pineforge::native_order::UnboundBookClose>(
            core.find_live(*rd.handle)->authority));
}

void flat_close_noeffect_and_bind() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto close = core.submit(Request{Reduce{ExplicitUnits{1.0}}, "c", ""}, 1, inc, ord);
    auto none = core.prepare_evaluation(*close.handle, print_ctx(2), flat_book(), ord);
    commit_mutation(core, std::move(none), ord);
    CHECK(core.live().empty());
    CHECK(std::holds_alternative<NoEffectEvent>(core.history().back()));

    const auto close2 = core.submit(Request{Reduce{ExplicitUnits{2.0}}, "c2", ""}, 1, inc, ord);
    auto bound = core.prepare_evaluation(*close2.handle, print_ctx(ord + 1), long_book(), ord);
    commit_mutation(core, std::move(bound), ord);
    CHECK(std::holds_alternative<CloseBoundEvent>(core.history().back()));
    CHECK(std::holds_alternative<BookClose>(core.find_live(*close2.handle)->authority));
}

void partial_retain_and_target_exhaust() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    Request req{Transact{5.0}, "p", ""};
    req.capacity = PointBudget{2.0};
    const auto accepted = core.submit(req, 1, inc, ord);
    auto eval = core.prepare_evaluation(*accepted.handle, print_ctx(ord + 1), flat_book(), ord);
    commit_mutation(core, std::move(eval), ord);
    const auto first = fill_transact(core, *accepted.handle, 2.0, ord, 3.0, false, 0, 7, 2.0);
    CHECK(!first.terminal);
    CHECK(core.find_live(*accepted.handle));
    CHECK(std::holds_alternative<RemainingUnits>(core.find_live(*accepted.handle)->remaining));
    CHECK(std::get<RemainingUnits>(core.find_live(*accepted.handle)->remaining).q == 3.0);

    Request reduce{Reduce{ExplicitUnits{5.0}}, "red", ""};
    const auto closer = core.submit(reduce, 1, inc, ord);
    auto bind = core.prepare_evaluation(*closer.handle, print_ctx(ord + 1), long_book(7), ord);
    commit_mutation(core, std::move(bind), ord);
    ExecutionProposal proposal;
    const auto* live = core.find_live(*closer.handle);
    proposal.cursor.point.ordinal = std::get<AllowanceUnits>(live->allowance).point_ordinal;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    proposal.physical_action = pineforge::order_action::Reduce{5.0};
    proposal.scope = pineforge::execution::Book{};
    proposal.pre_fill = PositionNonflat{7, Side::Long};
    proposal.inspected_closed_units = 1.0;
    proposal.inspected_opened_units = 0.0;
    proposal.inspected_current_ticket = 2;
    auto prep = core.prepare_execution(*closer.handle, proposal, ord);
    CHECK(std::holds_alternative<PreparedExecution>(prep));
    CommittedExecutionFacts facts;
    facts.result.status = pineforge::execution::Status::Applied;
    facts.result.closed_units = 1.0;
    facts.result.current_ticket = 2;
    facts.cycle_before = 7;
    facts.cycle_after = 0;
    facts.post_target = flat_book();
    facts.committed_action = proposal.physical_action;
    auto installed = core.install_execution(std::get<PreparedExecution>(std::move(prep)), facts);
    CHECK(std::holds_alternative<Installed>(installed));
    CHECK(!core.find_live(*closer.handle));
    const auto& applied = std::get<ExecutionAppliedEvent>(core.history().back());
    CHECK(applied.terminal);
    CHECK(applied.terminal_reason == AppliedTerminalReason::TargetExhausted);
}

void group_reduce_defer_and_quantity_bound() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    Request filler{Transact{2.0}, "f", ""};
    filler.group = Member{7, 1, GroupEffect::Reduce};
    const auto parent = core.submit(filler, 1, inc, ord);
    Request child{Reduce{OwnerOpenedUnits{}}, "ch", ""};
    child.owner = WaitForApplied{*parent.handle};
    child.group = Member{7, 2, GroupEffect::Cancel};
    const auto waiting = core.submit(child, 1, inc, ord);
    CHECK(std::holds_alternative<RemainingUnbound>(core.find_live(*waiting.handle)->remaining));

    auto eval = core.prepare_evaluation(*parent.handle, print_ctx(ord + 1), flat_book(), ord);
    commit_mutation(core, std::move(eval), ord);
    const auto applied = fill_transact(core, *parent.handle, 2.0, ord, 0.0, true, 0, 8, 2.0);
    CHECK(applied.terminal);
    const EventId cause{kRun, applied.ordinal};
    auto recipients = core.group_recipients(cause);
    CHECK(recipients.size() == 1);
    CHECK(recipients[0] == *waiting.handle);
    auto deferred = core.prepare_group_effect(cause, *waiting.handle, ord);
    commit_mutation(core, std::move(deferred), ord);
    CHECK(std::holds_alternative<DeferredGroupAdjustmentEvent>(core.history().back()));
    CHECK(core.find_live(*waiting.handle));
    CHECK(std::holds_alternative<RemainingUnbound>(core.find_live(*waiting.handle)->remaining));
    auto replay = core.prepare_group_effect(cause, *waiting.handle, ord);
    CHECK(std::holds_alternative<NoChange>(replay));
    CHECK(std::get<NoChange>(replay).reason == NoChangeReason::AlreadyApplied);

    OpeningObservation opening;
    opening.queried_opening = *parent.handle;
    opening.queried_cycle = 8;
    opening.current_position = PositionNonflat{8, Side::Long};
    opening.has_live_matching_lot = true;
    auto armed = core.prepare_owner_applied(cause, *waiting.handle, opening, ord);
    CHECK(std::holds_alternative<PreparedMutation>(armed));
    commit_mutation(core, std::move(armed), ord);
    CHECK(std::holds_alternative<QuantityBoundEvent>(core.history()[core.history().size() - 2]));
    const auto& bound = std::get<QuantityBoundEvent>(core.history()[core.history().size() - 2]);
    CHECK(bound.source_units == 2.0);
    CHECK(bound.effective_deduction == 2.0);
    CHECK(std::holds_alternative<CancelledEvent>(core.history().back()));
    CHECK(!core.find_live(*waiting.handle));
}

void owner_applied_wait_through_close_only() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    Request parent_req{Transact{3.0}, "p", ""};
    parent_req.capacity = PointBudget{1.0};
    const auto parent = core.submit(parent_req, 1, inc, ord);
    Request child{Reduce{OwnerOpenedUnits{}}, "c", ""};
    child.owner = WaitForApplied{*parent.handle};
    child.trigger = Limit{100.0};
    const auto waiting = core.submit(child, 1, inc, ord);
    auto eval = core.prepare_evaluation(*parent.handle, print_ctx(ord + 1),
                                        TargetObservation{PositionNonflat{7, Side::Short}, std::nullopt},
                                        ord);
    commit_mutation(core, std::move(eval), ord);

    ExecutionProposal proposal;
    proposal.cursor.point.ordinal =
            std::get<AllowanceUnits>(core.find_live(*parent.handle)->allowance).point_ordinal;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    proposal.physical_action = Transact{1.0};
    proposal.scope = pineforge::execution::Book{};
    proposal.pre_fill = PositionNonflat{7, Side::Short};
    proposal.inspected_closed_units = 1.0;
    proposal.inspected_opened_units = 0.0;
    proposal.inspected_current_ticket = 1;
    auto prep = core.prepare_execution(*parent.handle, proposal, ord);
    CHECK(std::holds_alternative<PreparedExecution>(prep));
    CommittedExecutionFacts facts;
    facts.result.status = pineforge::execution::Status::Applied;
    facts.result.closed_units = 1.0;
    facts.result.current_ticket = 1;
    facts.cycle_before = 7;
    facts.cycle_after = 7;
    facts.post_target.current_position = PositionNonflat{7, Side::Short};
    facts.committed_action = proposal.physical_action;
    auto installed = core.install_execution(std::get<PreparedExecution>(std::move(prep)), facts);
    CHECK(std::holds_alternative<Installed>(installed));
    ord += std::get<Installed>(installed).events.count;
    const EventId f1{kRun, std::get<ExecutionAppliedEvent>(core.history().back()).ordinal};
    auto still = core.prepare_owner_applied(f1, *waiting.handle, std::nullopt, ord);
    CHECK(std::holds_alternative<NoChange>(still));
    CHECK(std::get<NoChange>(still).reason == NoChangeReason::StillWaiting);
    CHECK(std::holds_alternative<Wait>(core.find_live(*waiting.handle)->authority));
    CHECK(std::holds_alternative<RemainingUnbound>(core.find_live(*waiting.handle)->remaining));
}

void checked_reservation_failure() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    Request filler{Transact{std::numeric_limits<double>::max()}, "f", ""};
    filler.group = Member{3, 1, GroupEffect::Reduce};
    const auto parent = core.submit(filler, 1, inc, ord);
    Request child{Reduce{OwnerOpenedUnits{}}, "c", ""};
    child.owner = WaitForApplied{*parent.handle};
    child.group = Member{3, 2, GroupEffect::Reduce};
    const auto waiting = core.submit(child, 1, inc, ord);
    auto eval = core.prepare_evaluation(*parent.handle, print_ctx(ord + 1), flat_book(), ord);
    commit_mutation(core, std::move(eval), ord);
    ExecutionProposal proposal;
    proposal.cursor.point.ordinal =
            std::get<AllowanceUnits>(core.find_live(*parent.handle)->allowance).point_ordinal;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    proposal.physical_action = Transact{std::numeric_limits<double>::max()};
    proposal.scope = pineforge::execution::Book{};
    proposal.inspected_closed_units = 0.0;
    proposal.inspected_opened_units = std::numeric_limits<double>::max();
    proposal.inspected_current_ticket = 1;
    auto prep = core.prepare_execution(*parent.handle, proposal, ord);
    if (std::holds_alternative<PreparationError>(prep)) {
        CHECK(std::get<PreparationError>(prep).code
              == pineforge::native_order::CoreFailure::NonrepresentableQuantity);
        return;
    }
    CHECK(std::holds_alternative<PreparedExecution>(prep));
    CommittedExecutionFacts facts;
    facts.result.status = pineforge::execution::Status::Applied;
    facts.result.opened_units = std::numeric_limits<double>::max();
    facts.result.current_ticket = 1;
    facts.cycle_after = 1;
    facts.post_target = long_book(1);
    facts.committed_action = proposal.physical_action;
    auto installed = core.install_execution(std::get<PreparedExecution>(std::move(prep)), facts);
    CHECK(std::holds_alternative<Installed>(installed));
    ord += std::get<Installed>(installed).events.count;
    const EventId cause{kRun, std::get<ExecutionAppliedEvent>(core.history().back()).ordinal};
    auto first = core.prepare_group_effect(cause, *waiting.handle, ord);
    commit_mutation(core, std::move(first), ord);
    Request filler2{Transact{std::numeric_limits<double>::max()}, "f2", ""};
    filler2.group = Member{3, 1, GroupEffect::Reduce};
    const auto parent2 = core.submit(filler2, 1, inc, ord);
    (void)parent2;
}

void stale_prepare_and_parent_terminal() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto parent = core.submit(market_tx(1.0, "p"), 1, inc, ord);
    Request child{Reduce{ExplicitUnits{1.0}}, "c", ""};
    child.owner = WaitForApplied{*parent.handle};
    const auto waiting = core.submit(child, 1, inc, ord);
    auto first = core.prepare_cancel(*parent.handle, ord);
    auto second = core.prepare_cancel(*parent.handle, ord);
    auto stale = core.install_cancel(std::move(first));
    CHECK(std::holds_alternative<InstallError>(stale));
    auto ok = core.install_cancel(std::move(second));
    CHECK(std::holds_alternative<pineforge::native_order::CommandInstalled<
                  pineforge::native_order::CancelResult>>(ok));
    ++ord;
    CHECK(!core.find_live(*parent.handle));
    const EventId cause{kRun, std::get<CancelledEvent>(core.history().back()).ordinal};
    auto kids = core.waiting_children(*parent.handle);
    CHECK(kids.size() == 1);
    auto term = core.prepare_parent_terminal(cause, kids.front(), ord);
    commit_mutation(core, std::move(term), ord);
    CHECK(!core.find_live(*waiting.handle));
    CHECK(std::get<CancelledEvent>(core.history().back()).reason == CancelReason::OwnerGone);
}

void stop_activation_and_receipt_dedup() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    Request req{Transact{1.0}, "s", ""};
    req.trigger = Stop{101.0};
    const auto accepted = core.submit(req, 1, inc, ord);
    auto eval = core.prepare_evaluation(*accepted.handle, print_ctx(ord + 1), flat_book(), ord);
    commit_mutation(core, std::move(eval), ord);
    ActivateStop hit;
    hit.cursor = print_ctx(ord).cursor;
    hit.reached_price = 101;
    auto trig = core.prepare_trigger(*accepted.handle, hit, ord);
    commit_mutation(core, std::move(trig), ord);
    CHECK(std::holds_alternative<pineforge::native_order::StopActive>(
            core.find_live(*accepted.handle)->trigger_state));
    auto again = core.prepare_trigger(*accepted.handle, hit, ord);
    CHECK(std::holds_alternative<NoChange>(again));
}

void wrong_core_token_and_move_invalidation() {
    const RunIdentity same{"same-logical-run-with-a-long-key", 1};
    WorkingRequestCore a(same), b(same);
    uint64_t ai = 1, ao = 1, bi = 1, bo = 1;
    auto left = a.prepare_submit(Request{Transact{1}, "A", ""}, CommandContext{}, ai, ao);
    auto right = b.prepare_submit(Request{Transact{9}, "B", ""}, CommandContext{}, bi, bo);
    auto cross = b.install_submit(std::move(left));
    CHECK(std::holds_alternative<InstallError>(cross));
    CHECK(std::get<InstallError>(cross) == InstallError::WrongCoreOrRun);
    CHECK(b.live().empty());
    CHECK(b.history().empty());
    auto ok = b.install_submit(std::move(right));
    CHECK(std::holds_alternative<CommandInstalled<pineforge::native_order::SubmitResult>>(ok));
    ++bo;
    ++bi;

    WorkingRequestCore src(same);
    uint64_t si = 1, so = 1;
    auto token = src.prepare_submit(Request{Transact{1}, "m", ""}, CommandContext{}, si, so);
    WorkingRequestCore dst = std::move(src);
    auto moved = dst.install_submit(std::move(token));
    CHECK(std::holds_alternative<InstallError>(moved));
    CHECK(std::get<InstallError>(moved) == InstallError::WrongCoreOrRun);
    CHECK(dst.live().empty());
}

void market_only_committed_rejection() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto live = core.submit(market_tx(1.0, "keep"), 1, inc, ord);
    Request extra{Transact{1.0}, "x", ""};
    extra.trigger = Limit{100.0};
    extra.owner = WaitForApplied{*live.handle};
    CommandContext market;
    market.surface = CommandSurface::MarketOnly;
    auto prepared = core.prepare_submit(extra, market, inc, ord);
    CHECK(prepared.predicted().status == SubmitStatus::Rejected);
    CHECK(prepared.predicted().reason == RequestRejectReason::InvalidTrigger);
    CHECK(prepared.predicted().event_ordinal != 0);
    auto installed = core.install_submit(std::move(prepared));
    CHECK(std::holds_alternative<CommandInstalled<pineforge::native_order::SubmitResult>>(installed));
    ++ord;
    const auto* rejected = std::get_if<RejectedEvent>(&core.history().back());
    CHECK(rejected);
    CHECK(rejected->reason == RequestRejectReason::InvalidTrigger);
    CHECK(rejected->surface == CommandSurface::MarketOnly);
    CHECK(rejected->ordinal != 0);
    CHECK(std::holds_alternative<Limit>(rejected->request.trigger));
    CHECK(std::holds_alternative<WaitForApplied>(rejected->request.owner));

    Request group_extra{Transact{1.0}, "g", ""};
    group_extra.group = Member{3, 1, GroupEffect::Cancel};
    auto gprep = core.prepare_submit(group_extra, market, inc, ord);
    CHECK(gprep.predicted().reason == RequestRejectReason::InvalidGroup);
    core.install_submit(std::move(gprep));
    ++ord;

    Request cap_extra{Transact{1.0}, "c", ""};
    cap_extra.capacity = PointBudget{1.0};
    auto cprep = core.prepare_submit(cap_extra, market, inc, ord);
    CHECK(cprep.predicted().reason == RequestRejectReason::InvalidCapacity);
    core.install_submit(std::move(cprep));
    ++ord;

    Request own_extra{Transact{1.0}, "o", ""};
    own_extra.owner = WaitForApplied{*live.handle};
    auto oprep = core.prepare_submit(own_extra, market, inc, ord);
    CHECK(oprep.predicted().reason == RequestRejectReason::InvalidOwner);
    core.install_submit(std::move(oprep));
    ++ord;

    Request ghost{Transact{1.0}, "ghost", ""};
    ghost.trigger = Stop{50.0};
    RequestHandle absent{kRun, 99};
    auto rprep = core.prepare_replace(absent, ghost, market, inc, ord);
    CHECK(rprep.predicted().status == ReplaceStatus::NotWorking);
    core.install_replace(std::move(rprep));
    ++ord;
    CHECK(std::holds_alternative<pineforge::native_order::NotWorkingEvent>(core.history().back()));

    auto live_replace = core.prepare_replace(*live.handle, ghost, market, inc, ord);
    CHECK(live_replace.predicted().status == ReplaceStatus::ReplaceRejected);
    CHECK(live_replace.predicted().reason == RequestRejectReason::InvalidTrigger);
    core.install_replace(std::move(live_replace));
    ++ord;
    const auto* rr = std::get_if<ReplaceRejectedEvent>(&core.history().back());
    CHECK(rr);
    CHECK(rr->surface == CommandSurface::MarketOnly);
    CHECK(rr->reason == RequestRejectReason::InvalidTrigger);
    CHECK(core.find_live(*live.handle));
}

void long_key_install_and_eligibility_no_alloc() {
    const RunIdentity long_run{std::string(80, 'K'), 1};
    WorkingRequestCore core(long_run);
    uint64_t inc = 1;
    uint64_t ord = 1;
    auto prepared = core.prepare_submit(Request{Transact{1.0}, "long", ""}, CommandContext{}, inc, ord);
    g_allocs.store(0);
    auto installed = core.install_submit(std::move(prepared));
    CHECK(g_allocs.load() == 0);
    CHECK(std::holds_alternative<CommandInstalled<pineforge::native_order::SubmitResult>>(installed));
    auto result = std::move(std::get<CommandInstalled<pineforge::native_order::SubmitResult>>(installed).result);
    CHECK(result.handle->run.session_key.size() == 80);
    ++inc;
    ++ord;

    Request child{Reduce{pineforge::native_order::ExplicitUnits{1.0}}, "w", ""};
    child.owner = WaitForApplied{*result.handle};
    auto child_prep = core.prepare_submit(child, CommandContext{}, inc, ord);
    g_allocs.store(0);
    auto child_inst = core.install_submit(std::move(child_prep));
    CHECK(g_allocs.load() == 0);
    ++inc;
    ++ord;
    const auto& waiting = core.live().back();
    g_allocs.store(0);
    auto facts = core.eligibility_facts(waiting, print_ctx(3));
    CHECK(g_allocs.load() == 0);
    CHECK(facts.waiting);
    CHECK(facts.authority != nullptr);

    g_allocs.store(0);
    const auto conv = core.submit(Request{Transact{2.0}, "conv", ""}, 1, inc, ord);
    CHECK(conv.status == SubmitStatus::Accepted);
    CHECK(conv.handle->run.session_key.size() == 80);
}
}  // namespace

int main() {
    validation_matrix_and_market_defaults();
    bind_opening_and_replace_predecessor_wait();
    c9_eligibility_does_not_mutate();
    flat_close_noeffect_and_bind();
    partial_retain_and_target_exhaust();
    group_reduce_defer_and_quantity_bound();
    owner_applied_wait_through_close_only();
    checked_reservation_failure();
    stale_prepare_and_parent_terminal();
    stop_activation_and_receipt_dedup();
    wrong_core_token_and_move_invalidation();
    market_only_committed_rejection();
    long_key_install_and_eligibility_no_alloc();
    std::printf("native order resting core: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
