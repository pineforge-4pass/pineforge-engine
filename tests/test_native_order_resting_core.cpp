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
    context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
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
                                    double opened = 0.0, double price = 100.0) {
    const auto* live = core.find_live(handle);
    CHECK(live);
    uint64_t point = 0;
    if (const auto* units = std::get_if<AllowanceUnits>(&live->allowance)) {
        point = units->point_ordinal;
    }
    ExecutionProposal proposal;
    proposal.cursor.point.ordinal = point;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = price;
    proposal.resolved_price = price;
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
    const auto* remaining_units =
            std::get_if<pineforge::native_order::RemainingProjectionUnits>(&applied.remaining_after);
    CHECK(remaining_units);
    CHECK(remaining_units->q == remaining_after);
    if (!terminal) {
        const auto* live_now = core.find_live(handle);
        CHECK(live_now);
        CHECK(std::get<RemainingUnits>(live_now->remaining).q == remaining_after);
    } else {
        CHECK(!core.find_live(handle));
    }
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

EventId fill_group_source(WorkingRequestCore& core, RequestHandle handle, double opened,
                          uint64_t& inc, uint64_t& ord) {
    (void)inc;
    auto eval = core.prepare_evaluation(handle, print_ctx(ord + 1), flat_book(), ord);
    commit_mutation(core, std::move(eval), ord);
    ExecutionProposal proposal;
    proposal.cursor.point.ordinal =
            std::get<AllowanceUnits>(core.find_live(handle)->allowance).point_ordinal;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    proposal.physical_action = Transact{opened};
    proposal.scope = pineforge::execution::Book{};
    proposal.inspected_closed_units = 0.0;
    proposal.inspected_opened_units = opened;
    proposal.inspected_current_ticket = 1;
    auto prep = core.prepare_execution(handle, proposal, ord);
    CHECK(std::holds_alternative<PreparedExecution>(prep));
    CommittedExecutionFacts facts;
    facts.result.status = pineforge::execution::Status::Applied;
    facts.result.opened_units = opened;
    facts.result.current_ticket = 1;
    facts.cycle_after = 1;
    facts.post_target = long_book(1);
    facts.committed_action = proposal.physical_action;
    auto installed = core.install_execution(std::get<PreparedExecution>(std::move(prep)), facts);
    CHECK(std::holds_alternative<Installed>(installed));
    ord += std::get<Installed>(installed).events.count;
    return EventId{kRun, std::get<ExecutionAppliedEvent>(core.history().back()).ordinal};
}

void checked_reservation_failure() {
    const double maxv = std::numeric_limits<double>::max();
    const double p60 = 0x1p60;
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        Request filler{Transact{maxv}, "f", ""};
        filler.group = Member{3, 1, GroupEffect::Reduce};
        const auto parent = core.submit(filler, 1, inc, ord);
        Request child{Reduce{OwnerOpenedUnits{}}, "c", ""};
        child.owner = WaitForApplied{*parent.handle};
        child.group = Member{3, 2, GroupEffect::Reduce};
        const auto waiting = core.submit(child, 1, inc, ord);
        const EventId cause = fill_group_source(core, *parent.handle, maxv, inc, ord);
        auto first = core.prepare_group_effect(cause, *waiting.handle, ord);
        commit_mutation(core, std::move(first), ord);
        CHECK(std::holds_alternative<RemainingUnbound>(core.find_live(*waiting.handle)->remaining));
        const auto* pending =
                std::get_if<pineforge::native_order::PendingDeferred>(&core.find_live(*waiting.handle)->pending);
        CHECK(pending);
        CHECK(pending->total == maxv);
        const std::size_t hist = core.history().size();
        Request filler2{Transact{maxv}, "f2", ""};
        filler2.group = Member{3, 1, GroupEffect::Reduce};
        const auto parent2 = core.submit(filler2, 1, inc, ord);
        const EventId cause2 = fill_group_source(core, *parent2.handle, maxv, inc, ord);
        auto second = core.prepare_group_effect(cause2, *waiting.handle, ord);
        CHECK(std::holds_alternative<PreparationError>(second));
        CHECK(std::get<PreparationError>(second).code
              == pineforge::native_order::CoreFailure::UnrepresentableReservation);
        CHECK(core.history().size() == hist + 2);
        CHECK(std::get<pineforge::native_order::PendingDeferred>(
                      core.find_live(*waiting.handle)->pending)
                      .total
              == maxv);
    }
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        Request filler{Transact{p60}, "f", ""};
        filler.group = Member{4, 1, GroupEffect::Reduce};
        const auto parent = core.submit(filler, 1, inc, ord);
        Request child{Reduce{OwnerOpenedUnits{}}, "c", ""};
        child.owner = WaitForApplied{*parent.handle};
        child.group = Member{4, 2, GroupEffect::Reduce};
        const auto waiting = core.submit(child, 1, inc, ord);
        const EventId cause = fill_group_source(core, *parent.handle, p60, inc, ord);
        commit_mutation(core, core.prepare_group_effect(cause, *waiting.handle, ord), ord);
        Request filler2{Transact{1.0}, "f2", ""};
        filler2.group = Member{4, 1, GroupEffect::Reduce};
        const auto parent2 = core.submit(filler2, 1, inc, ord);
        const EventId cause2 = fill_group_source(core, *parent2.handle, 1.0, inc, ord);
        auto second = core.prepare_group_effect(cause2, *waiting.handle, ord);
        CHECK(std::holds_alternative<PreparationError>(second));
        CHECK(std::get<PreparationError>(second).code
              == pineforge::native_order::CoreFailure::UnrepresentableReservation);
        CHECK(std::get<pineforge::native_order::PendingDeferred>(
                      core.find_live(*waiting.handle)->pending)
                      .total
              == p60);
    }
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        Request recipient{Reduce{ExplicitUnits{p60}}, "r", ""};
        recipient.group = Member{5, 2, GroupEffect::Reduce};
        const auto dest = core.submit(recipient, 1, inc, ord);
        auto bind = core.prepare_evaluation(*dest.handle, print_ctx(ord + 1), long_book(1), ord);
        commit_mutation(core, std::move(bind), ord);
        CHECK(std::get<RemainingUnits>(core.find_live(*dest.handle)->remaining).q == p60);
        Request filler{Transact{1.0}, "f", ""};
        filler.group = Member{5, 1, GroupEffect::Reduce};
        const auto parent = core.submit(filler, 1, inc, ord);
        const EventId cause = fill_group_source(core, *parent.handle, 1.0, inc, ord);
        const std::size_t hist = core.history().size();
        auto reduce = core.prepare_group_effect(cause, *dest.handle, ord);
        CHECK(std::holds_alternative<PreparationError>(reduce));
        CHECK(std::get<PreparationError>(reduce).code
              == pineforge::native_order::CoreFailure::UnrepresentableReservation);
        CHECK(core.history().size() == hist);
        CHECK(std::get<RemainingUnits>(core.find_live(*dest.handle)->remaining).q == p60);
    }
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
    auto trig = core.prepare_trigger(*accepted.handle, hit, DriverEligibilityClass::ObservedPrint, ord);
    commit_mutation(core, std::move(trig), ord);
    CHECK(std::holds_alternative<pineforge::native_order::StopActive>(
            core.find_live(*accepted.handle)->trigger_state));
    auto again = core.prepare_trigger(*accepted.handle, hit, DriverEligibilityClass::ObservedPrint, ord);
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

void review_failure_before_witnesses() {
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        const auto parent = core.submit(market_tx(2.0, "p"), 1, inc, ord);
        Request child{Reduce{OwnerOpenedUnits{}}, "c", ""};
        child.owner = WaitForApplied{*parent.handle};
        child.capacity = PointBudget{1};
        const auto accepted = core.submit(child, 1, inc, ord);
        CHECK(accepted.status == SubmitStatus::Accepted);
        CHECK(std::holds_alternative<RemainingUnbound>(core.find_live(*accepted.handle)->remaining));
    }
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        Request r{Transact{1.0}, "bad-group", ""};
        r.group = Member{1, 0, static_cast<GroupEffect>(9)};
        const auto rejected = core.submit(r, 1, inc, ord);
        CHECK(rejected.status == SubmitStatus::Rejected);
        CHECK(rejected.reason == RequestRejectReason::InvalidGroup);
        CHECK(rejected.event_ordinal != 0);
    }
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        RequestHandle owner{kRun, 44};
        Request r{Reduce{ExplicitUnits{1}}, "bind", ""};
        r.owner = BindOpening{owner, 7};
        CommandContext ctx;
        ctx.opening = OpeningObservation{owner, 7, PositionNonflat{8, Side::Long}, true};
        auto prepared = core.prepare_submit(r, ctx, inc, ord);
        CHECK(prepared.predicted().status == SubmitStatus::Rejected);
        CHECK(prepared.predicted().reason == RequestRejectReason::InvalidOwner);
        core.install_submit(std::move(prepared));
        CHECK(core.live().empty());
    }
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        Request parent_req{Transact{2.0}, "p", ""};
        parent_req.capacity = PointBudget{1.0};
        const auto parent = core.submit(parent_req, 1, inc, ord);
        auto eval = core.prepare_evaluation(*parent.handle, print_ctx(ord + 1), flat_book(), ord);
        commit_mutation(core, std::move(eval), ord);
        fill_transact(core, *parent.handle, 1.0, ord, 1.0, false, 0, 7, 1.0);
        const EventId cause{kRun, std::get<ExecutionAppliedEvent>(core.history().back()).ordinal};
        Request child{Transact{1.0}, "late", ""};
        child.owner = WaitForApplied{*parent.handle};
        const auto h = core.submit(child, 1, inc, ord);
        CHECK(h.handle->incarnation > 0);
        auto arm = core.prepare_owner_applied(cause, *h.handle, std::nullopt, ord);
        CHECK(std::holds_alternative<NoChange>(arm));
        CHECK(std::get<NoChange>(arm).reason == NoChangeReason::NoTransition);
        CHECK(std::holds_alternative<Wait>(core.find_live(*h.handle)->authority));
        CHECK(core.find_live(*h.handle)->birth().acceptance_ordinal > cause.ordinal);
    }
    {
        WorkingRequestCore a(kRun), b(kRun);
        uint64_t ai = 1, ao = 1, bi = 1, bo = 1;
        Request r{Transact{2.0}, "partial", ""};
        r.capacity = PointBudget{1.0};
        const auto ha = a.submit(r, 0, ai, ao);
        auto eval = a.prepare_evaluation(*ha.handle, print_ctx(ao + 1), flat_book(), ao);
        commit_mutation(a, std::move(eval), ao);
        ExecutionProposal proposal;
        proposal.cursor.point.ordinal =
                std::get<AllowanceUnits>(a.find_live(*ha.handle)->allowance).point_ordinal;
        proposal.cursor.point.effective_time_ms = 1000;
        proposal.raw_price = proposal.resolved_price = 100;
        proposal.physical_action = Transact{1.0};
        proposal.inspected_opened_units = 1.0;
        auto prep = a.prepare_execution(*ha.handle, proposal, ao);
        CHECK(std::holds_alternative<PreparedExecution>(prep));
        auto token = std::get<PreparedExecution>(std::move(prep));
        CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = 1.0;
        facts.cycle_after = 7;
        facts.post_target = long_book(7);
        facts.committed_action = Transact{1.0};
        g_allocs.store(0);
        auto wrong = b.install_execution(std::move(token), facts);
        CHECK(g_allocs.load() == 0);
        CHECK(std::holds_alternative<InstallError>(wrong));
        CHECK(std::get<InstallError>(wrong) == InstallError::WrongCoreOrRun);
        CHECK(a.live().size() == 1);
        CHECK(a.find_live(*ha.handle)->definition);
        g_allocs.store(0);
        auto retry = a.install_execution(std::move(token), facts);
        CHECK(g_allocs.load() == 0);
        CHECK(std::holds_alternative<Installed>(retry));
        CHECK(std::get_if<ExecutionAppliedEvent>(&a.history().back())->definition);
        auto consumed = a.install_execution(std::move(token), facts);
        CHECK(std::holds_alternative<InstallError>(consumed));
        CHECK(std::get<InstallError>(consumed) == InstallError::AlreadyConsumed);
        (void)bi;
        (void)bo;
    }
    {
        WorkingRequestCore src(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        Request r{Transact{2.0}, "m", ""};
        r.capacity = PointBudget{1.0};
        const auto h = src.submit(r, 0, inc, ord);
        auto eval = src.prepare_evaluation(*h.handle, print_ctx(ord + 1), flat_book(), ord);
        commit_mutation(src, std::move(eval), ord);
        ExecutionProposal proposal;
        proposal.cursor.point.ordinal =
                std::get<AllowanceUnits>(src.find_live(*h.handle)->allowance).point_ordinal;
        proposal.cursor.point.effective_time_ms = 1000;
        proposal.raw_price = proposal.resolved_price = 100;
        proposal.physical_action = Transact{1.0};
        proposal.inspected_opened_units = 1.0;
        auto prep = src.prepare_execution(*h.handle, proposal, ord);
        auto token = std::get<PreparedExecution>(std::move(prep));
        WorkingRequestCore dst = std::move(src);
        CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = 1.0;
        facts.cycle_after = 7;
        facts.post_target = long_book(7);
        facts.committed_action = Transact{1.0};
        auto moved = dst.install_execution(std::move(token), facts);
        CHECK(std::holds_alternative<InstallError>(moved));
        CHECK(std::get<InstallError>(moved) == InstallError::WrongCoreOrRun);
        CHECK(dst.find_live(*h.handle));
    }
}
void chronological_pending_receipts() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1, ord = 1;
    Request parent_request{Transact{1.0}, "late-opening", ""};
    parent_request.trigger = Limit{50.0};
    const auto parent = core.submit(parent_request, 0, inc, ord);
    Request child_request{Reduce{OwnerOpenedUnits{}}, "decimal-pending", ""};
    child_request.owner = WaitForApplied{*parent.handle};
    child_request.group = Member{7, 2, GroupEffect::Reduce};
    const auto child = core.submit(child_request, 0, inc, ord);
    std::vector<EventId> expected_receipts;
    for (double quantity : {0.1, 0.2, 0.3}) {
        Request source_request{Transact{quantity}, "adjustment", ""};
        source_request.group = Member{7, 1, GroupEffect::Reduce};
        const auto source = core.submit(source_request, 0, inc, ord);
        const uint64_t point = ord++;
        commit_mutation(core, core.prepare_evaluation(*source.handle, print_ctx(point),
                                                     long_book(7), ord), ord);
        const auto applied = fill_transact(core, *source.handle, quantity, ord, 0.0,
                                           true, 7, 7, quantity);
        const EventId cause{kRun, applied.ordinal};
        commit_mutation(core, core.prepare_group_effect(cause, *child.handle, ord), ord);
        const auto& receipt = std::get<DeferredGroupAdjustmentEvent>(core.history().back());
        CHECK(receipt.cause == cause);
        CHECK(receipt.deferred_delta == quantity);
        expected_receipts.push_back(EventId{kRun, receipt.ordinal});
    }
    const auto& pending = std::get<pineforge::native_order::PendingDeferred>(
            core.find_live(*child.handle)->pending);
    CHECK(pending.total == 0x1.3333333333334p-1);
    CHECK(pending.count == 3);
    const uint64_t point = ord++;
    commit_mutation(core, core.prepare_evaluation(*parent.handle, print_ctx(point),
                                                 long_book(7), ord), ord);
    const auto applied = fill_transact(core, *parent.handle, 1.0, ord, 0.0, true, 7, 7, 1.0, 50.0);
    const OpeningObservation opening{*parent.handle, 7, PositionNonflat{7, Side::Long}, true};
    commit_mutation(core, core.prepare_owner_applied(EventId{kRun, applied.ordinal},
                                                    *child.handle, opening, ord), ord);
    const auto& bound = std::get<QuantityBoundEvent>(core.history()[core.history().size() - 2]);
    CHECK(bound.prior_adjustment_ids == expected_receipts);
    CHECK(bound.pending_total == 0x1.3333333333334p-1);
    CHECK(bound.source_units == 1.0);
    CHECK(bound.effective_deduction == 0x1.3333333333334p-1);
    CHECK(std::get<RemainingUnits>(core.find_live(*child.handle)->remaining).q
          == 0x1.9999999999998p-2);
}

void reduce_to_flatten_receipt_replay() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1, ord = 1;
    Request source_request{Transact{1.0}, "reduce-source", ""};
    source_request.group = Member{7, 1, GroupEffect::Reduce};
    const auto source = core.submit(source_request, 0, inc, ord);
    Request target_request{Flatten{}, "all-scope", ""};
    target_request.group = Member{7, 2, GroupEffect::Cancel};
    const auto target = core.submit(target_request, 0, inc, ord);
    const uint64_t point = ord++;
    commit_mutation(core, core.prepare_evaluation(*source.handle, print_ctx(point),
                                                 long_book(7), ord), ord);
    const auto applied = fill_transact(core, *source.handle, 1.0, ord, 0.0, true, 7, 7, 1.0);
    const EventId cause{kRun, applied.ordinal};
    commit_mutation(core, core.prepare_group_effect(cause, *target.handle, ord), ord);
    const auto& cancelled = std::get<CancelledEvent>(core.history().back());
    CHECK(cancelled.reason == CancelReason::Group);
    CHECK(cancelled.cause == cause);
    CHECK(std::holds_alternative<pineforge::native_order::RemainingProjectionFlattenAll>(
            cancelled.unexecuted));
    CHECK(!core.find_live(*target.handle));
    const std::size_t history_size = core.history().size();
    const uint64_t next = ord;
    auto replay = core.prepare_group_effect(cause, *target.handle, ord);
    CHECK(std::holds_alternative<NoChange>(replay));
    CHECK(std::get<NoChange>(replay).reason == NoChangeReason::AlreadyApplied);
    CHECK(core.history().size() == history_size);
    CHECK(ord == next);
}

void explicit_trigger_driver_and_favorable_arm() {
    using pineforge::NativeCompletionKind;
    using pineforge::NativePriceProvenance;
    using pineforge::NativePathPhase;
    for (auto completion : {NativeCompletionKind::Confirmed, NativeCompletionKind::PartialFinalized}) {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1, ord = 1;
        Request request{Transact{1.0}, "tick-aftercalc", ""};
        request.trigger = Stop{100.0};
        const auto accepted = core.submit(request, 0, inc, ord);
        auto context = print_ctx(ord++);
        context.cursor.point.provenance = NativePriceProvenance::AfterCalculationClose;
        context.cursor.point.path_phase = NativePathPhase::Close;
        context.cursor.point.completion = completion;
        context.driver_class = DriverEligibilityClass::TickAfterCalculation;
        commit_mutation(core, core.prepare_evaluation(*accepted.handle, context, flat_book(), ord), ord);
        const auto size = core.history().size();
        const ActivateStop valid_hit{context.cursor, 101.0};
        auto bar_aftercalc = core.prepare_trigger(*accepted.handle, valid_hit,
                DriverEligibilityClass::ConfirmedAfterCalculationClose, ord);
        CHECK(std::holds_alternative<NoChange>(bar_aftercalc));
        CHECK(std::get<NoChange>(bar_aftercalc).reason == NoChangeReason::NotEligible);
        auto contradictory = core.prepare_trigger(*accepted.handle, valid_hit,
                DriverEligibilityClass::ObservedPrint, ord);
        CHECK(std::holds_alternative<NoChange>(contradictory));
        auto carried_hit = valid_hit;
        carried_hit.cursor.point.provenance = NativePriceProvenance::CarriedOpen;
        auto carried = core.prepare_trigger(*accepted.handle, carried_hit,
                DriverEligibilityClass::CarriedOpen, ord);
        CHECK(std::holds_alternative<NoChange>(carried));
        auto disguised_carried = core.prepare_trigger(*accepted.handle, carried_hit,
                DriverEligibilityClass::TickAfterCalculation, ord);
        CHECK(std::holds_alternative<NoChange>(disguised_carried));
        auto before_birth = valid_hit;
        before_birth.cursor.point.ordinal = accepted.event_ordinal;
        auto early = core.prepare_trigger(*accepted.handle, before_birth,
                DriverEligibilityClass::TickAfterCalculation, ord);
        CHECK(std::holds_alternative<NoChange>(early));
        auto below_stop = valid_hit;
        below_stop.reached_price = 99.0;
        auto not_reached = core.prepare_trigger(*accepted.handle, below_stop,
                DriverEligibilityClass::TickAfterCalculation, ord);
        CHECK(std::holds_alternative<PreparationError>(not_reached));
        CHECK(core.history().size() == size);
        commit_mutation(core, core.prepare_trigger(*accepted.handle, valid_hit,
                DriverEligibilityClass::TickAfterCalculation, ord), ord);
        CHECK(std::holds_alternative<pineforge::native_order::StopActive>(
                core.find_live(*accepted.handle)->trigger_state));
    }
    for (double sign : {-1.0, 1.0}) {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1, ord = 1;
        Request request{Transact{sign}, "trail-arm", ""};
        request.trigger = pineforge::native_order::Trail{2.0, sign < 0.0 ? 105.0 : 95.0};
        const auto accepted = core.submit(request, 0, inc, ord);
        auto context = print_ctx(ord++);
        commit_mutation(core, core.prepare_evaluation(*accepted.handle, context, flat_book(), ord), ord);
        const double wrong_side = sign < 0.0 ? 104.0 : 96.0;
        auto refused = core.prepare_trigger(*accepted.handle,
                pineforge::native_order::BeginTrailTracking{context.cursor, wrong_side},
                DriverEligibilityClass::ObservedPrint, ord);
        CHECK(std::holds_alternative<PreparationError>(refused));
        const double favorable = sign < 0.0 ? 106.0 : 94.0;
        commit_mutation(core, core.prepare_trigger(*accepted.handle,
                pineforge::native_order::BeginTrailTracking{context.cursor, favorable},
                DriverEligibilityClass::ObservedPrint, ord), ord);
        CHECK(std::get<pineforge::native_order::TrailTrack>(
                core.find_live(*accepted.handle)->trigger_state).best == favorable);
    }
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
    review_failure_before_witnesses();
    chronological_pending_receipts();
    reduce_to_flatten_receipt_replay();
    explicit_trigger_driver_and_favorable_arm();
    std::printf("native order resting core: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
