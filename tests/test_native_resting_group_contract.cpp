// Bounded R2 group/validity acceptance. Real host commands and observations.
// Core section is only internal duplicate delivery, which is not a public host
// operation. No injected lots, history mutation, or test-only production hooks.
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
int checks = 0, failures = 0, cases = 0;
const char* scenario = "setup";
struct StopCase {};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #x);                                \
        }                                                                                          \
    } while (0)
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        const bool ok_ = bool(x);                                                                  \
        CHECK(ok_);                                                                                \
        if (!ok_) throw StopCase{};                                                                \
    } while (0)

constexpr int64_t T = 1736121600000LL;

void near(double actual, double expected) {
    const bool ok = std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected));
    if (!ok) std::printf("  actual=%.17g expected=%.17g\n", actual, expected);
    CHECK(ok);
}

struct Host final : NativeStrategyHost {
    std::function<void(Host&)> beginning;
    std::function<void(Host&)> calculation;
    uint64_t sequence = 0;
    void on_native_run_begin() override {
        if (beginning) beginning(*this);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (calculation) calculation(*this);
    }
    void tick(int64_t offset, double price) {
        REQUIRE(stream_push_tick(TradeTick{T + offset + 1, ++sequence, price, 1}));
        REQUIRE(native_state().kind == NativeLifecycleKind::Running);
    }
};

NativeRunSpec configuration(const char* key, uint64_t run = 1, double fee = 0) {
    NativeRunSpec s;
    s.identity = {key, run};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "N";
    s.tickerid = "TEST:N";
    s.type = "crypto";
    s.currency = "USD";
    s.basecurrency = "USD";
    s.description = "R2 group acceptance";
    s.volumetype = "base";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000;
    s.point_value = 1;
    s.account_fx = 1;
    s.price_tick = .01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = fee;
    return s;
}

void start(Host& h, const char* key, uint64_t run = 1, double fee = 0, double first = 100) {
    REQUIRE(h.configure_native(configuration(key, run, fee)).status == NativeSetupStatus::Applied);
    auto beginning = std::exchange(h.beginning, {});
    const Bar warmup{first, first, first, first, 1, T - 60000};
    h.sequence = 0;
    REQUIRE(h.stream_begin(&warmup, 1, "1", "1"));
    REQUIRE(h.stream_push_tick(TradeTick{T, ++h.sequence, first, 0}));
    h.beginning = std::move(beginning);
    if (h.beginning) h.beginning(h);
    REQUIRE(h.native_state().kind == NativeLifecycleKind::Running);
}

void finish(Host& h) {
    REQUIRE(h.stream_end(false));
    CHECK(h.native_state().kind == NativeLifecycleKind::Completed);
}

no::Request tx(double q, const char* label = "") { return {no::Transact{q}, label, ""}; }
no::Request reduce(double q, const char* label = "") {
    return {no::Reduce{no::ExplicitUnits{q}}, label, ""};
}
no::Request event_reduce(const no::RequestHandle& parent, const char* label = "child") {
    no::Request r{no::Reduce{no::OwnerOpenedUnits{}}, label, ""};
    r.owner = no::WaitForApplied{parent};
    return r;
}
double far_open(double sign) { return sign > 0 ? 1.0 : 200.0; }
double far_close(double sign) { return sign > 0 ? 200.0 : 1.0; }

no::RequestHandle put(Host& h, const no::Request& request) {
    auto result = h.submit(request);
    REQUIRE(result.status == no::SubmitStatus::Accepted);
    REQUIRE(result.handle.has_value());
    return *result.handle;
}

template <class Event>
std::vector<Event> events(const Host& h) {
    std::vector<Event> out;
    for (const auto& row : h.native_events(0)) {
        if (row.command) {
            if (const auto* value = std::get_if<Event>(&*row.command)) out.push_back(*value);
        }
    }
    return out;
}

template <class Event>
std::vector<Event> events(const Host& h, const no::RequestHandle& handle) {
    std::vector<Event> out;
    for (const auto& event : events<Event>(h)) {
        REQUIRE(event.definition);
        if (event.definition->handle == handle) out.push_back(event);
    }
    return out;
}

std::vector<no::ExecutionAppliedEvent> fills(const Host& h, const no::RequestHandle& handle) {
    return events<no::ExecutionAppliedEvent>(h, handle);
}
std::vector<no::CancelledEvent> cancellations(const Host& h, const no::RequestHandle& handle) {
    return events<no::CancelledEvent>(h, handle);
}

void expect_reject(Host& h, const no::Request& request, no::RequestRejectReason reason) {
    const auto result = h.submit(request);
    CHECK(result.status == no::SubmitStatus::Rejected && !result.handle);
    CHECK(result.reason == reason);
    CHECK(result.event_ordinal > 0);
    const auto rejected = events<no::RejectedEvent>(h);
    REQUIRE(!rejected.empty());
    CHECK(rejected.back().reason == reason);
    CHECK(rejected.back().ordinal == result.event_ordinal);
}

static_assert(std::is_constructible_v<no::ReductionSize, no::ExplicitUnits>);
static_assert(std::is_constructible_v<no::ReductionSize, no::OwnerOpenedUnits>);
static_assert(!std::is_constructible_v<no::Transact, no::OwnerOpenedUnits>);
static_assert(!std::is_constructible_v<no::Flatten, no::OwnerOpenedUnits>);
static_assert(std::variant_size_v<no::ReductionSize> == 2);
static_assert(std::is_same_v<decltype(no::Transact::signed_units), double>);
static_assert(std::is_same_v<decltype(no::Reduce::size), no::ReductionSize>);
static_assert(std::is_empty_v<no::OwnerOpenedUnits>);
static_assert(std::is_empty_v<no::Flatten>);

// G1: partial Cancel Applied has no group effect; terminal Applied cancels the
// other cohort only. Labels/comments and cohort 0 stay inert/opaque.
void g1_partial_cancel_until_terminal(double sign) {
    Host h;
    start(h, sign > 0 ? "G1-partial-L" : "G1-partial-S");
    auto same = tx(sign, "alpha");
    same.comment = "same-cohort";
    same.trigger = no::Limit{far_open(sign)};
    same.group = no::Member{7, 1, no::GroupEffect::Cancel};
    const auto same_cohort = put(h, same);
    auto other = tx(sign, "alpha");
    other.comment = "other-cohort-zero";
    other.trigger = no::Limit{far_open(sign)};
    other.group = no::Member{7, 0, no::GroupEffect::Cancel};
    const auto other_cohort = put(h, other);
    auto filler = tx(sign * 3, "alpha");
    filler.comment = "emitter";
    filler.capacity = no::PointBudget{1};
    filler.group = no::Member{7, 1, no::GroupEffect::Cancel};
    const auto emit = put(h, filler);

    h.tick(0, 100);
    REQUIRE(fills(h, emit).size() == 1);
    CHECK(!fills(h, emit)[0].terminal);
    CHECK(cancellations(h, same_cohort).empty());
    CHECK(cancellations(h, other_cohort).empty());
    CHECK(fills(h, same_cohort).empty() && fills(h, other_cohort).empty());

    h.tick(1, 100);
    REQUIRE(fills(h, emit).size() == 2);
    CHECK(!fills(h, emit)[1].terminal);
    CHECK(cancellations(h, same_cohort).empty());
    CHECK(cancellations(h, other_cohort).empty());

    h.tick(2, 100);
    const auto ff = fills(h, emit);
    REQUIRE(ff.size() == 3);
    CHECK(ff[2].terminal);
    CHECK(ff[2].terminal_reason == no::AppliedTerminalReason::WorkingUnitsSatisfied);
    CHECK(cancellations(h, same_cohort).empty());
    const auto ce = cancellations(h, other_cohort);
    REQUIRE(ce.size() == 1);
    CHECK(ce[0].reason == no::CancelReason::Group);
    REQUIRE(ce[0].cause);
    CHECK(ce[0].cause->ordinal == ff[2].ordinal);
    CHECK(ce[0].cause->run == emit.run);

    h.tick(3, far_open(sign));
    CHECK(fills(h, same_cohort).size() == 1);
    CHECK(fills(h, other_cohort).empty());
    CHECK(cancellations(h, same_cohort).empty());
    near(h.physical_position().signed_units, sign * 4);
    finish(h);
}

// G1: clipped TargetExhausted terminal Cancel still emits.
void g1_target_exhausted_cancel_emits(double sign) {
    Host h;
    start(h, sign > 0 ? "G1-exhausted-L" : "G1-exhausted-S", 1, 2);
    put(h, tx(sign, "seed"));
    h.tick(0, 100);
    auto recipient = tx(sign, "resting");
    recipient.trigger = no::Limit{far_open(sign)};
    recipient.group = no::Member{7, 2, no::GroupEffect::Reduce};
    const auto other = put(h, recipient);
    auto closer = reduce(5, "clip");
    closer.group = no::Member{7, 1, no::GroupEffect::Cancel};
    const auto emit = put(h, closer);
    const int trades_before = h.trade_count();
    h.tick(1, 100);
    const auto ff = fills(h, emit);
    REQUIRE(ff.size() == 1);
    CHECK(ff[0].closed_units == 1 && ff[0].opened_units == 0 && ff[0].terminal);
    CHECK(ff[0].terminal_reason == no::AppliedTerminalReason::TargetExhausted);
    const auto* unused = std::get_if<no::RemainingProjectionUnits>(&ff[0].remaining_after);
    REQUIRE(unused);
    CHECK(unused->q == 4);
    CHECK(ff[0].current_ticket == 2);
    CHECK(h.trade_count() == trades_before + 1);
    const auto ce = cancellations(h, other);
    REQUIRE(ce.size() == 1);
    CHECK(ce[0].reason == no::CancelReason::Group);
    REQUIRE(ce[0].cause);
    CHECK(ce[0].cause->ordinal == ff[0].ordinal);
    CHECK(fills(h, other).empty());
    CHECK(h.cancel(other).status == no::CancelStatus::NotWorking);
    near(h.physical_position().signed_units, 0);
    finish(h);
}

// G3: parent's own Reduce adjusts the unbound child before quantity binding.
void g3_parent_reduce_unbinds_child(double sign) {
    Host h;
    start(h, sign > 0 ? "G3-L" : "G3-S", 1, 2);
    auto parent_req = tx(sign * 2, "parent");
    parent_req.group = no::Member{7, 1, no::GroupEffect::Reduce};
    const auto parent = put(h, parent_req);
    auto child_req = event_reduce(parent);
    child_req.trigger = no::Limit{far_close(sign)};
    child_req.group = no::Member{7, 2, no::GroupEffect::Cancel};
    const auto child = put(h, child_req);
    h.tick(0, 100);
    const auto pf = fills(h, parent);
    REQUIRE(pf.size() == 1);
    CHECK(pf[0].opened_units == sign * 2 && pf[0].closed_units == 0 && pf[0].terminal);
    CHECK(pf[0].filled_working == 2);
    CHECK(fills(h, child).empty());
    CHECK(events<no::ReservationReducedEvent>(h).empty());
    CHECK(events<no::ArmedEvent>(h, child).empty());
    const auto deferred = events<no::DeferredGroupAdjustmentEvent>(h);
    REQUIRE(deferred.size() == 1);
    CHECK(deferred[0].recipient == child);
    CHECK(deferred[0].cause.ordinal == pf[0].ordinal);
    CHECK(deferred[0].cause.run == parent.run);
    CHECK(deferred[0].effect == no::GroupEffect::Reduce);
    CHECK(deferred[0].deferred_delta == 2);
    CHECK(std::holds_alternative<no::PendingNone>(deferred[0].pending_before));
    CHECK(deferred[0].pending_after.total == 2 && deferred[0].pending_after.count == 1);
    const auto bound = events<no::QuantityBoundEvent>(h, child);
    REQUIRE(bound.size() == 1);
    CHECK(bound[0].source.ordinal == pf[0].ordinal);
    CHECK(bound[0].source_units == 2);
    CHECK(bound[0].pending_total == 2 && bound[0].effective_deduction == 2);
    REQUIRE(bound[0].prior_adjustment_ids.size() == 1);
    CHECK(bound[0].prior_adjustment_ids[0].ordinal == deferred[0].ordinal);
    const auto* remaining = std::get_if<no::RemainingProjectionUnits>(&bound[0].remaining);
    REQUIRE(remaining);
    CHECK(remaining->q == 0);
    const auto ce = cancellations(h, child);
    REQUIRE(ce.size() == 1);
    CHECK(ce[0].reason == no::CancelReason::Group);
    REQUIRE(ce[0].cause);
    CHECK(ce[0].cause->ordinal == pf[0].ordinal);
    CHECK(ce[0].ordinal == bound[0].ordinal + 1);
    CHECK(std::holds_alternative<no::RemainingProjectionUnbound>(ce[0].unexecuted));
    const auto* pending = std::get_if<no::PendingDeferred>(&ce[0].pending);
    REQUIRE(pending);
    CHECK(pending->total == 2 && pending->count == 1);
    CHECK(deferred[0].ordinal < bound[0].ordinal && bound[0].ordinal < ce[0].ordinal);
    CHECK(h.trade_count() == 0);
    CHECK(fills(h, child).empty());
    near(h.physical_position().signed_units, sign * 2);
    near(h.native_marked_equity(100), 9998);
    finish(h);
}

// G4: delta 5 against Units 2 stages ReservationReduced actual 2 then Cancelled.
void g4_delta_clips_units_and_cancels(double sign) {
    Host h;
    start(h, sign > 0 ? "G4-L" : "G4-S", 1, 2);
    auto waiting = tx(sign * 2, "units2");
    waiting.trigger = no::Limit{far_open(sign)};
    waiting.group = no::Member{7, 2, no::GroupEffect::Cancel};
    const auto recipient = put(h, waiting);
    auto emit = tx(sign * 5, "delta5");
    emit.group = no::Member{7, 1, no::GroupEffect::Reduce};
    const auto filler = put(h, emit);
    h.tick(0, 100);
    const auto ff = fills(h, filler);
    REQUIRE(ff.size() == 1);
    CHECK(ff[0].filled_working == 5 && ff[0].opened_units == sign * 5);
    CHECK(ff[0].current_ticket == 2);
    CHECK(fills(h, recipient).empty());
    const auto reduced = events<no::ReservationReducedEvent>(h);
    REQUIRE(reduced.size() == 1);
    CHECK(reduced[0].cause.ordinal == ff[0].ordinal);
    CHECK(reduced[0].recipient == recipient);
    CHECK(reduced[0].effect == no::GroupEffect::Reduce);
    CHECK(reduced[0].requested_delta == 5 && reduced[0].actual_deduction == 2);
    CHECK(reduced[0].before.q == 2);
    const auto* after = std::get_if<no::RemainingProjectionUnits>(&reduced[0].after);
    REQUIRE(after);
    CHECK(after->q == 0);
    const auto ce = cancellations(h, recipient);
    REQUIRE(ce.size() == 1);
    CHECK(ce[0].reason == no::CancelReason::Group);
    REQUIRE(ce[0].cause);
    CHECK(ce[0].cause->ordinal == ff[0].ordinal);
    CHECK(ce[0].ordinal == reduced[0].ordinal + 1);
    const auto* unexecuted = std::get_if<no::RemainingProjectionUnits>(&ce[0].unexecuted);
    REQUIRE(unexecuted);
    CHECK(unexecuted->q == 2);
    CHECK(h.trade_count() == 0);
    CHECK(h.cancel(recipient).status == no::CancelStatus::NotWorking);
    h.tick(1, far_open(sign));
    CHECK(fills(h, recipient).empty());
    CHECK(events<no::ReservationReducedEvent>(h).size() == 1);
    CHECK(cancellations(h, recipient).size() == 1);
    near(h.physical_position().signed_units, sign * 5);
    near(h.native_marked_equity(100), 9998);
    finish(h);
}

// G7 host: one cause, two recipients exactly once; later successor excluded.
void g7_two_recipients_and_later_successor(double sign) {
    Host h;
    start(h, sign > 0 ? "G7-L" : "G7-S");
    auto parent_req = tx(sign * 5, "parent");
    parent_req.trigger = no::Stop{100 + sign * 5};
    const auto parent = put(h, parent_req);
    auto c1 = event_reduce(parent, "first");
    c1.trigger = no::Limit{far_close(sign)};
    c1.group = no::Member{7, 2, no::GroupEffect::Reduce};
    const auto first = put(h, c1);
    auto c2 = event_reduce(parent, "second");
    c2.trigger = no::Limit{far_close(sign)};
    c2.group = no::Member{7, 3, no::GroupEffect::Reduce};
    const auto second = put(h, c2);
    auto f10 = tx(sign * 2, "F10");
    f10.group = no::Member{7, 1, no::GroupEffect::Reduce};
    const auto filler = put(h, f10);

    h.tick(0, 100);
    const auto ff = fills(h, filler);
    REQUIRE(ff.size() == 1);
    CHECK(fills(h, parent).empty());
    auto deferred = events<no::DeferredGroupAdjustmentEvent>(h);
    REQUIRE(deferred.size() == 2);
    CHECK(deferred[0].cause.ordinal == ff[0].ordinal && deferred[1].cause.ordinal == ff[0].ordinal);
    CHECK(deferred[0].recipient == first && deferred[1].recipient == second);
    CHECK(deferred[0].deferred_delta == 2 && deferred[1].deferred_delta == 2);
    CHECK(deferred[0].ordinal != deferred[1].ordinal);

    auto successor_req = event_reduce(parent, "second");
    successor_req.comment = "reused-text";
    successor_req.trigger = no::Limit{far_close(sign)};
    successor_req.group = no::Member{7, 3, no::GroupEffect::Reduce};
    const auto replaced = h.replace(second, successor_req);
    REQUIRE(replaced.status == no::ReplaceStatus::Replaced);
    REQUIRE(replaced.successor);
    const auto successor = *replaced.successor;
    CHECK(successor.incarnation != second.incarnation);
    CHECK(events<no::DeferredGroupAdjustmentEvent>(h).size() == 2);
    CHECK(cancellations(h, first).empty());

    h.tick(1, 100 + sign * 5);
    const auto pf = fills(h, parent);
    REQUIRE(pf.size() == 1);
    CHECK(pf[0].opened_units == sign * 5);
    CHECK(events<no::DeferredGroupAdjustmentEvent>(h).size() == 2);
    const auto first_bound = events<no::QuantityBoundEvent>(h, first);
    const auto successor_bound = events<no::QuantityBoundEvent>(h, successor);
    REQUIRE(first_bound.size() == 1);
    REQUIRE(successor_bound.size() == 1);
    CHECK(first_bound[0].source.ordinal == pf[0].ordinal);
    CHECK(first_bound[0].source_units == 5);
    CHECK(first_bound[0].pending_total == 2 && first_bound[0].effective_deduction == 2);
    REQUIRE(first_bound[0].prior_adjustment_ids.size() == 1);
    CHECK(first_bound[0].prior_adjustment_ids[0].ordinal == deferred[0].ordinal);
    const auto* first_left = std::get_if<no::RemainingProjectionUnits>(&first_bound[0].remaining);
    REQUIRE(first_left);
    CHECK(first_left->q == 3);
    CHECK(successor_bound[0].source.ordinal == pf[0].ordinal);
    CHECK(successor_bound[0].source_units == 5);
    CHECK(successor_bound[0].pending_total == 0 && successor_bound[0].effective_deduction == 0);
    CHECK(successor_bound[0].prior_adjustment_ids.empty());
    const auto* successor_left =
            std::get_if<no::RemainingProjectionUnits>(&successor_bound[0].remaining);
    REQUIRE(successor_left);
    CHECK(successor_left->q == 5);
    CHECK(events<no::QuantityBoundEvent>(h, second).empty());
    CHECK(fills(h, first).empty() && fills(h, successor).empty());
    CHECK(cancellations(h, first).empty() && cancellations(h, successor).empty());
    near(h.physical_position().signed_units, sign * 7);
    finish(h);
}

void v1_representable_owner_action_size() {
    Host h;
    start(h, "V1-matrix");
    auto independent_tx = tx(1, "I-Tx");
    independent_tx.trigger = no::Limit{far_open(1)};
    CHECK(put(h, independent_tx).incarnation != 0);
    auto independent_rd = reduce(1, "I-Rd");
    independent_rd.trigger = no::Limit{far_close(1)};
    CHECK(put(h, independent_rd).incarnation != 0);
    auto independent_fl = no::Request{no::Flatten{}, "I-Fl", ""};
    independent_fl.trigger = no::Limit{far_close(1)};
    CHECK(put(h, independent_fl).incarnation != 0);
    no::Request independent_oo{no::Reduce{no::OwnerOpenedUnits{}}, "I-Rd-OO", ""};
    expect_reject(h, independent_oo, no::RequestRejectReason::InvalidQuantityBasis);

    auto live_parent = tx(1, "wait-parent");
    live_parent.trigger = no::Limit{far_open(1)};
    const auto parent = put(h, live_parent);
    auto wait_tx = tx(1, "W-Tx");
    wait_tx.owner = no::WaitForApplied{parent};
    CHECK(put(h, wait_tx).incarnation != 0);
    auto wait_rd = reduce(1, "W-Rd");
    wait_rd.owner = no::WaitForApplied{parent};
    CHECK(put(h, wait_rd).incarnation != 0);
    CHECK(put(h, event_reduce(parent, "W-Rd-OO")).incarnation != 0);
    auto wait_fl = no::Request{no::Flatten{}, "W-Fl", ""};
    wait_fl.owner = no::WaitForApplied{parent};
    CHECK(put(h, wait_fl).incarnation != 0);

    auto flatten_budget = no::Request{no::Flatten{}, "budget-flat", ""};
    flatten_budget.capacity = no::PointBudget{1};
    expect_reject(h, flatten_budget, no::RequestRejectReason::InvalidCapacity);
    auto bad_group = tx(1, "g0");
    bad_group.group = no::Member{0, 1, no::GroupEffect::Cancel};
    expect_reject(h, bad_group, no::RequestRejectReason::InvalidGroup);
    auto bad_effect = tx(1, "ge");
    bad_effect.group = no::Member{3, 0, static_cast<no::GroupEffect>(9)};
    expect_reject(h, bad_effect, no::RequestRejectReason::InvalidGroup);
    auto zero_rd = reduce(0, "zero");
    expect_reject(h, zero_rd, no::RequestRejectReason::InvalidQuantity);
    auto zero_tx = tx(0, "zero-tx");
    expect_reject(h, zero_tx, no::RequestRejectReason::InvalidQuantity);
    finish(h);

    Host bind_host;
    start(bind_host, "V1-bind");
    const auto opener = put(bind_host, tx(1, "opener"));
    bind_host.tick(0, 100);
    const auto opened = fills(bind_host, opener);
    REQUIRE(opened.size() == 1);
    const int64_t cycle = opened[0].cycle_after;
    REQUIRE(cycle > 0);
    auto bind_rd = reduce(1, "B-Rd");
    bind_rd.owner = no::BindOpening{opener, cycle};
    CHECK(put(bind_host, bind_rd).incarnation != 0);
    auto bind_fl = no::Request{no::Flatten{}, "B-Fl", ""};
    bind_fl.owner = no::BindOpening{opener, cycle};
    CHECK(put(bind_host, bind_fl).incarnation != 0);
    auto bind_oo = no::Request{no::Reduce{no::OwnerOpenedUnits{}}, "B-Rd-OO", ""};
    bind_oo.owner = no::BindOpening{opener, cycle};
    expect_reject(bind_host, bind_oo, no::RequestRejectReason::InvalidQuantityBasis);
    auto bind_tx = tx(1, "B-Tx");
    bind_tx.owner = no::BindOpening{opener, cycle};
    expect_reject(bind_host, bind_tx, no::RequestRejectReason::InvalidOwner);
    finish(bind_host);

    Host grid;
    auto spec = configuration("V1-grid");
    spec.quantity_grid = 1.0;
    REQUIRE(grid.configure_native(spec).status == NativeSetupStatus::Applied);
    const Bar warmup{100, 100, 100, 100, 1, T - 60000};
    REQUIRE(grid.stream_begin(&warmup, 1, "1", "1"));
    REQUIRE(grid.stream_push_tick(TradeTick{T, ++grid.sequence, 100, 0}));
    expect_reject(grid, tx(1.5, "off-grid"), no::RequestRejectReason::OffGrid);
    finish(grid);
}

void v3_rejection_precedence() {
    Host h;
    start(h, "V3-precedence");
    const auto opener = put(h, tx(1, "lot"));
    h.tick(0, 100);
    const auto opened = fills(h, opener);
    REQUIRE(opened.size() == 1);
    const int64_t cycle = opened[0].cycle_after;

    auto qty_then_trig = reduce(0, "qty-first");
    qty_then_trig.trigger = no::Stop{0};
    expect_reject(h, qty_then_trig, no::RequestRejectReason::InvalidQuantity);

    auto trig_then_owner = tx(1, "trig-first");
    trig_then_owner.trigger = no::Stop{0};
    trig_then_owner.owner = no::WaitForApplied{no::RequestHandle{{"V3-precedence", 1}, 0}};
    expect_reject(h, trig_then_owner, no::RequestRejectReason::InvalidTrigger);

    auto owner_then_group = tx(1, "owner-first");
    owner_then_group.owner = no::WaitForApplied{no::RequestHandle{{"V3-precedence", 1}, 99}};
    owner_then_group.group = no::Member{0, 0, no::GroupEffect::Cancel};
    expect_reject(h, owner_then_group, no::RequestRejectReason::InvalidOwner);

    auto bind_tx = tx(1, "bind-tx");
    bind_tx.owner = no::BindOpening{opener, cycle};
    expect_reject(h, bind_tx, no::RequestRejectReason::InvalidOwner);

    auto bad_owner_oo = no::Request{no::Reduce{no::OwnerOpenedUnits{}}, "bind-oo-dead", ""};
    bad_owner_oo.owner = no::BindOpening{opener, 0};
    expect_reject(h, bad_owner_oo, no::RequestRejectReason::InvalidOwner);

    auto valid_owner_oo = no::Request{no::Reduce{no::OwnerOpenedUnits{}}, "bind-oo-live", ""};
    valid_owner_oo.owner = no::BindOpening{opener, cycle};
    expect_reject(h, valid_owner_oo, no::RequestRejectReason::InvalidQuantityBasis);

    auto nan_stop = tx(1, "nan");
    nan_stop.trigger = no::Stop{std::numeric_limits<double>::quiet_NaN()};
    expect_reject(h, nan_stop, no::RequestRejectReason::InvalidTrigger);
    const auto rejected = events<no::RejectedEvent>(h);
    REQUIRE(!rejected.empty());
    const auto* stop = std::get_if<no::Stop>(&rejected.back().request.trigger);
    REQUIRE(stop);
    CHECK(std::isnan(stop->price));
    finish(h);
}

void v4_self_wait_and_invalid_trigger_preserve_dependents() {
    Host h;
    start(h, "V4-self-wait");
    auto parent_req = tx(1, "parent");
    parent_req.trigger = no::Limit{far_open(1)};
    const auto parent = put(h, parent_req);
    auto child_req = tx(-1, "child");
    child_req.owner = no::WaitForApplied{parent};
    child_req.trigger = no::Limit{far_close(1)};
    const auto child = put(h, child_req);
    auto grandchild_req = reduce(1, "grandchild");
    grandchild_req.owner = no::WaitForApplied{child};
    const auto grandchild = put(h, grandchild_req);

    auto self_wait = tx(1, "self");
    self_wait.owner = no::WaitForApplied{parent};
    const auto self = h.replace(parent, self_wait);
    CHECK(self.status == no::ReplaceStatus::ReplaceRejected);
    CHECK(self.reason == no::RequestRejectReason::InvalidOwner);
    CHECK(!self.successor);
    const auto owner_reject = events<no::ReplaceRejectedEvent>(h);
    REQUIRE(!owner_reject.empty());
    CHECK(owner_reject.back().reason == no::RequestRejectReason::InvalidOwner);
    CHECK(owner_reject.back().target() == parent);

    auto bad_trig = tx(1, "bad-stop");
    bad_trig.trigger = no::Stop{0};
    const auto trig = h.replace(parent, bad_trig);
    CHECK(trig.status == no::ReplaceStatus::ReplaceRejected);
    CHECK(trig.reason == no::RequestRejectReason::InvalidTrigger);
    CHECK(!trig.successor);

    auto still_parent = tx(1, "still-live");
    still_parent.trigger = no::Stop{0};
    const auto parent_probe = h.replace(parent, still_parent);
    CHECK(parent_probe.status == no::ReplaceStatus::ReplaceRejected);
    CHECK(parent_probe.reason == no::RequestRejectReason::InvalidTrigger);
    auto still_child = tx(-1, "child-live");
    still_child.trigger = no::Stop{0};
    const auto child_probe = h.replace(child, still_child);
    CHECK(child_probe.status == no::ReplaceStatus::ReplaceRejected);
    CHECK(child_probe.reason == no::RequestRejectReason::InvalidTrigger);
    auto still_grand = reduce(1, "grand-live");
    still_grand.trigger = no::Stop{0};
    const auto grand_probe = h.replace(grandchild, still_grand);
    CHECK(grand_probe.status == no::ReplaceStatus::ReplaceRejected);
    CHECK(grand_probe.reason == no::RequestRejectReason::InvalidTrigger);
    CHECK(events<no::ReplacedEvent>(h).empty());
    CHECK(cancellations(h, parent).empty());
    CHECK(cancellations(h, child).empty());
    CHECK(cancellations(h, grandchild).empty());
    finish(h);
}

void v4_foreign_invalid_nonworking_beat_candidate() {
    Host h;
    start(h, "V4-target-first");
    auto live_req = tx(1, "live");
    live_req.trigger = no::Limit{far_open(1)};
    const auto live = put(h, live_req);
    auto bad = tx(1, "candidate");
    bad.trigger = no::Stop{0};

    no::RequestHandle foreign{{"other-run", 1}, live.incarnation};
    const auto foreign_result = h.replace(foreign, bad);
    CHECK(foreign_result.status == no::ReplaceStatus::InvalidHandle);
    CHECK(!foreign_result.reason);
    const auto invalid = events<no::InvalidHandleEvent>(h);
    REQUIRE(!invalid.empty());
    CHECK(invalid.back().target.run.session_key == "other-run");
    REQUIRE(invalid.back().attempted);
    CHECK(std::holds_alternative<no::Stop>(invalid.back().attempted->trigger));

    no::RequestHandle zero{{"V4-target-first", 1}, 0};
    const auto zero_result = h.replace(zero, bad);
    CHECK(zero_result.status == no::ReplaceStatus::InvalidHandle);

    const auto filled = put(h, tx(1, "fill-me"));
    h.tick(0, 100);
    REQUIRE(fills(h, filled).size() == 1);
    const auto dead = h.replace(filled, bad);
    CHECK(dead.status == no::ReplaceStatus::NotWorking);
    CHECK(!dead.reason);
    const auto not_working = events<no::NotWorkingEvent>(h);
    REQUIRE(!not_working.empty());
    CHECK(not_working.back().target == filled);
    REQUIRE(not_working.back().attempted);
    CHECK(std::holds_alternative<no::Stop>(not_working.back().attempted->trigger));

    const auto live_probe = h.replace(live, bad);
    CHECK(live_probe.status == no::ReplaceStatus::ReplaceRejected);
    CHECK(live_probe.reason == no::RequestRejectReason::InvalidTrigger);
    CHECK(events<no::ReplacedEvent>(h).empty());
    finish(h);
}

// Internal duplicate delivery is not a public host operation.
void core_duplicate_delivery_and_successor_exclusion() {
    const no::RunIdentity run{"group-core", 1};
    no::WorkingRequestCore core(run);
    uint64_t inc = 1;
    uint64_t ord = 1;
    auto parent_req = tx(5, "parent");
    parent_req.trigger = no::Limit{50};
    const auto parent = core.submit(parent_req, 0, inc, ord);
    REQUIRE(parent.status == no::SubmitStatus::Accepted);
    auto first_req = no::Request{no::Reduce{no::OwnerOpenedUnits{}}, "first", ""};
    first_req.owner = no::WaitForApplied{*parent.handle};
    first_req.group = no::Member{7, 2, no::GroupEffect::Reduce};
    const auto first = core.submit(first_req, 0, inc, ord);
    REQUIRE(first.status == no::SubmitStatus::Accepted);
    auto second_req = no::Request{no::Reduce{no::OwnerOpenedUnits{}}, "second", ""};
    second_req.owner = no::WaitForApplied{*parent.handle};
    second_req.group = no::Member{7, 3, no::GroupEffect::Reduce};
    const auto second = core.submit(second_req, 0, inc, ord);
    REQUIRE(second.status == no::SubmitStatus::Accepted);
    auto filler_req = tx(2, "F10");
    filler_req.group = no::Member{7, 1, no::GroupEffect::Reduce};
    const auto filler = core.submit(filler_req, 0, inc, ord);
    REQUIRE(filler.status == no::SubmitStatus::Accepted);

    auto eval = core.prepare_evaluation(*filler.handle,
                                        [] {
                                            no::EvaluationContext ctx;
                                            ctx.cursor.point.ordinal = 20;
                                            ctx.cursor.point.effective_time_ms = 1000;
                                            ctx.cursor.point.provenance =
                                                    NativePriceProvenance::ObservedPrint;
                                            ctx.driver_class = no::DriverEligibilityClass::ObservedPrint;
                                            ctx.existing_matching_bit = true;
                                            return ctx;
                                        }(),
                                        [] {
                                            no::TargetObservation obs;
                                            obs.current_position = no::PositionFlat{};
                                            return obs;
                                        }(),
                                        ord);
    REQUIRE(std::holds_alternative<no::PreparedMutation>(eval));
    auto eval_installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(eval)));
    REQUIRE(std::holds_alternative<no::Installed>(eval_installed));
    ord += std::get<no::Installed>(eval_installed).events.count;

    no::ExecutionProposal proposal;
    const auto* live = core.find_live(*filler.handle);
    REQUIRE(live);
    proposal.cursor.point.ordinal = std::get<no::AllowanceUnits>(live->allowance).point_ordinal;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 100;
    proposal.resolved_price = 100;
    proposal.physical_action = no::Transact{2};
    proposal.inspected_opened_units = 2;
    proposal.inspected_current_ticket = 1;
    auto prep = core.prepare_execution(*filler.handle, proposal, ord);
    REQUIRE(std::holds_alternative<no::PreparedExecution>(prep));
    no::CommittedExecutionFacts facts;
    facts.result.status = execution::Status::Applied;
    facts.result.opened_units = 2;
    facts.result.current_ticket = 1;
    facts.cycle_after = 1;
    facts.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
    facts.committed_action = no::Transact{2};
    auto installed = core.install_execution(std::get<no::PreparedExecution>(std::move(prep)), facts);
    REQUIRE(std::holds_alternative<no::Installed>(installed));
    ord += std::get<no::Installed>(installed).events.count;
    const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
    REQUIRE(applied);
    const no::EventId cause{run, applied->ordinal};

    auto recipients = core.group_recipients(cause);
    REQUIRE(recipients.size() == 2);
    CHECK(recipients[0] == *first.handle);
    CHECK(recipients[1] == *second.handle);

    auto d1 = core.prepare_group_effect(cause, *first.handle, ord);
    REQUIRE(std::holds_alternative<no::PreparedMutation>(d1));
    auto d1i = core.install_mutation(std::get<no::PreparedMutation>(std::move(d1)));
    REQUIRE(std::holds_alternative<no::Installed>(d1i));
    ord += std::get<no::Installed>(d1i).events.count;
    CHECK(std::holds_alternative<no::DeferredGroupAdjustmentEvent>(core.history().back()));
    auto d2 = core.prepare_group_effect(cause, *second.handle, ord);
    REQUIRE(std::holds_alternative<no::PreparedMutation>(d2));
    auto d2i = core.install_mutation(std::get<no::PreparedMutation>(std::move(d2)));
    REQUIRE(std::holds_alternative<no::Installed>(d2i));
    ord += std::get<no::Installed>(d2i).events.count;
    CHECK(std::holds_alternative<no::DeferredGroupAdjustmentEvent>(core.history().back()));

    const std::size_t after_both = core.history().size();
    const uint64_t ord_before_replay = ord;
    auto replay_first = core.prepare_group_effect(cause, *first.handle, ord);
    REQUIRE(std::holds_alternative<no::NoChange>(replay_first));
    CHECK(std::get<no::NoChange>(replay_first).reason == no::NoChangeReason::AlreadyApplied);
    auto replay_second = core.prepare_group_effect(cause, *second.handle, ord);
    REQUIRE(std::holds_alternative<no::NoChange>(replay_second));
    CHECK(std::get<no::NoChange>(replay_second).reason == no::NoChangeReason::AlreadyApplied);
    CHECK(core.history().size() == after_both);
    CHECK(ord == ord_before_replay);

    auto successor_req = no::Request{no::Reduce{no::OwnerOpenedUnits{}}, "second", ""};
    successor_req.owner = no::WaitForApplied{*parent.handle};
    successor_req.group = no::Member{7, 3, no::GroupEffect::Reduce};
    const auto successor = core.replace(*second.handle, successor_req, 0, inc, ord);
    REQUIRE(successor.status == no::ReplaceStatus::Replaced);
    REQUIRE(successor.successor);
    auto after_replace = core.group_recipients(cause);
    REQUIRE(after_replace.size() == 1);
    CHECK(after_replace[0] == *first.handle);
    auto late = core.prepare_group_effect(cause, *successor.successor, ord);
    REQUIRE(std::holds_alternative<no::PreparationError>(late));
    CHECK(std::get<no::PreparationError>(late).code == no::CoreFailure::InvalidCause);
    CHECK(core.history().size() == after_both + 1);
    auto replay_dead = core.prepare_group_effect(cause, *second.handle, ord);
    REQUIRE(std::holds_alternative<no::NoChange>(replay_dead));
    CHECK(std::get<no::NoChange>(replay_dead).reason == no::NoChangeReason::AlreadyApplied);
    CHECK(core.find_live(*first.handle));
    CHECK(!core.find_live(*second.handle));
    CHECK(core.find_live(*successor.successor));
}

void run_case(const char* name, const std::function<void()>& body) {
    scenario = name;
    ++cases;
    const int previous = failures;
    try {
        body();
    } catch (const StopCase&) {
    } catch (const std::exception& e) {
        ++failures;
        std::printf("FAIL %s unexpected exception: %s\n", name, e.what());
    } catch (...) {
        ++failures;
        std::printf("FAIL %s unexpected nonstandard exception\n", name);
    }
    std::printf("%s %s\n", failures == previous ? "PASS" : "FAIL", name);
}
}  // namespace

int main() {
    for (double sign : {1.0, -1.0}) {
        run_case(sign > 0 ? "G1 long partial Cancel silent until terminal"
                          : "G1 short partial Cancel silent until terminal",
                 [&] { g1_partial_cancel_until_terminal(sign); });
        run_case(sign > 0 ? "G1 long TargetExhausted Cancel still emits"
                          : "G1 short TargetExhausted Cancel still emits",
                 [&] { g1_target_exhausted_cancel_emits(sign); });
        run_case(sign > 0 ? "G3 long parent Reduce binds unbound child"
                          : "G3 short parent Reduce binds unbound child",
                 [&] { g3_parent_reduce_unbinds_child(sign); });
        run_case(sign > 0 ? "G4 long delta5 clips Units2"
                          : "G4 short delta5 clips Units2",
                 [&] { g4_delta_clips_units_and_cancels(sign); });
        run_case(sign > 0 ? "G7 long two recipients and later successor"
                          : "G7 short two recipients and later successor",
                 [&] { g7_two_recipients_and_later_successor(sign); });
    }
    run_case("V1 representable owner/action/size matrix", [] { v1_representable_owner_action_size(); });
    run_case("V3 rejection precedence", [] { v3_rejection_precedence(); });
    run_case("V4 self-wait replacement preserves predecessor/dependents",
             [] { v4_self_wait_and_invalid_trigger_preserve_dependents(); });
    run_case("V4 foreign/invalid/nonworking replace target beats candidate",
             [] { v4_foreign_invalid_nonworking_beat_candidate(); });
    run_case("G7 core duplicate key and successor exclusion",
             [] { core_duplicate_delivery_and_successor_exclusion(); });
    std::printf("%s native resting group contract: %d cases, %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", cases, checks, failures);
    return failures ? 1 : 0;
}
