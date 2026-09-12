// Bounded R2 P2 replay/reset/hash acceptance. Real native hosts only.
// Compares observations and projections; does not reimplement hashing.
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;
namespace {
int checks = 0, failures = 0, cases = 0;
const char* scenario = "setup";
const char* boundary = "";
struct StopCase {};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s %s:%d %s\n", scenario, boundary, __LINE__, #x); } } while (0)
#define REQUIRE(x) do { const bool ok_ = bool(x); CHECK(ok_); if (!ok_) throw StopCase{}; } while (0)

constexpr int64_t T = 1736121600000LL;

bool bits_eq(double a, double b) {
    std::uint64_t x = 0, y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}
std::uint64_t bits_of(double v) {
    std::uint64_t x = 0;
    std::memcpy(&x, &v, sizeof x);
    return x;
}
double bits_to(std::uint64_t x) {
    double v = 0;
    std::memcpy(&v, &x, sizeof v);
    return v;
}
void same_d(double a, double b) {
    if (!bits_eq(a, b))
        std::printf("  bits %a (%llx) vs %a (%llx)\n",
                    a, (unsigned long long)bits_of(a), b, (unsigned long long)bits_of(b));
    CHECK(bits_eq(a, b));
}
void same_u64(std::uint64_t a, std::uint64_t b) { CHECK(a == b); }
void same_i64(std::int64_t a, std::int64_t b) { CHECK(a == b); }
void same_str(const std::string& a, const std::string& b) { CHECK(a == b); }

struct Host final : NativeStrategyHost {
    std::function<void(Host&)> beginning;
    std::function<void(Host&)> calculation;
    int calculations = 0;
    uint64_t sequence = 0;
    void on_native_run_begin() override { if (beginning) beginning(*this); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++calculations;
        if (calculation) calculation(*this);
    }
    bool input(int64_t offset, double price) {
        return stream_push_tick(TradeTick{T + offset + 1, ++sequence, price, 1});
    }
    void tick(int64_t offset, double price) {
        REQUIRE(input(offset, price));
        REQUIRE(native_state().kind == NativeLifecycleKind::Running);
    }
    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
};

NativeRunSpec configuration(const char* key, uint64_t run = 1, double fee = 0) {
    NativeRunSpec s;
    s.identity = {key, run};
    s.input_tf = "1"; s.script_tf = "1";
    s.ticker = "N"; s.tickerid = "TEST:N"; s.type = "crypto";
    s.currency = "USD"; s.basecurrency = "USD"; s.description = "R2 replay";
    s.volumetype = "base"; s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 10000; s.point_value = 1; s.account_fx = 1;
    s.price_tick = .01; s.fee_kind = NativeFeeKind::CashPerExecution; s.fee_value = fee;
    return s;
}
void start(Host& h, const char* key, uint64_t run = 1, double fee = 0, double first = 100) {
    REQUIRE(h.configure_native(configuration(key, run, fee)).status == NativeSetupStatus::Applied);
    auto beginning = std::exchange(h.beginning, {});
    const Bar warmup{first, first, first, first, 1, T - 60000};
    h.sequence = 0;
    h.calculations = 0;
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
no::Request tx(double q, const char* label = "", const char* comment = "") {
    return {no::Transact{q}, label, comment};
}
no::Request reduce(double q, const char* label = "", const char* comment = "") {
    return {no::Reduce{no::ExplicitUnits{q}}, label, comment};
}
no::RequestHandle put(Host& h, const no::Request& request) {
    auto result = h.submit(request);
    REQUIRE(result.status == no::SubmitStatus::Accepted);
    REQUIRE(result.handle.has_value());
    return *result.handle;
}

void same_handle(const no::RequestHandle& a, const no::RequestHandle& b) {
    same_str(a.run.session_key, b.run.session_key);
    same_u64(a.run.run_number, b.run.run_number);
    same_u64(a.incarnation, b.incarnation);
}
void same_event_id(const no::EventId& a, const no::EventId& b) {
    same_str(a.run.session_key, b.run.session_key);
    same_u64(a.run.run_number, b.run.run_number);
    same_u64(a.ordinal, b.ordinal);
}
void same_coord(const NativeCoordinate& a, const NativeCoordinate& b) {
    same_u64(a.ordinal, b.ordinal);
    CHECK(a.interval_index == b.interval_index);
    same_i64(a.open_ms, b.open_ms);
    same_i64(a.eligible_open_ms, b.eligible_open_ms);
    same_i64(a.last_traded_close_ms, b.last_traded_close_ms);
    same_i64(a.next_period_open_ms, b.next_period_open_ms);
    same_i64(a.next_input_open_ms, b.next_input_open_ms);
    same_i64(a.effective_time_ms, b.effective_time_ms);
    same_i64(a.source_price_time_ms, b.source_price_time_ms);
    CHECK(a.provenance == b.provenance);
    CHECK(a.path_phase == b.path_phase);
    CHECK(a.completion == b.completion);
}
void same_cursor(const no::MatchCursor& a, const no::MatchCursor& b) {
    same_coord(a.point, b.point);
    same_d(a.t, b.t);
}
void same_intent(const no::OrderIntent& a, const no::OrderIntent& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* t = std::get_if<no::Transact>(&a))
        same_d(t->signed_units, std::get<no::Transact>(b).signed_units);
    else if (const auto* r = std::get_if<no::Reduce>(&a)) {
        REQUIRE(r->size.index() == std::get<no::Reduce>(b).size.index());
        if (const auto* u = std::get_if<no::ExplicitUnits>(&r->size))
            same_d(u->units, std::get<no::ExplicitUnits>(std::get<no::Reduce>(b).size).units);
    }
}
void same_trigger(const no::Trigger& a, const no::Trigger& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* l = std::get_if<no::Limit>(&a))
        same_d(l->price, std::get<no::Limit>(b).price);
    else if (const auto* s = std::get_if<no::Stop>(&a))
        same_d(s->price, std::get<no::Stop>(b).price);
    else if (const auto* sl = std::get_if<no::StopLimit>(&a)) {
        same_d(sl->stop, std::get<no::StopLimit>(b).stop);
        same_d(sl->limit, std::get<no::StopLimit>(b).limit);
    } else if (const auto* t = std::get_if<no::Trail>(&a)) {
        same_d(t->offset, std::get<no::Trail>(b).offset);
        CHECK(t->arm_price.has_value() == std::get<no::Trail>(b).arm_price.has_value());
        if (t->arm_price) same_d(*t->arm_price, *std::get<no::Trail>(b).arm_price);
    }
}
void same_capacity(const no::Capacity& a, const no::Capacity& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* p = std::get_if<no::PointBudget>(&a))
        same_d(p->units, std::get<no::PointBudget>(b).units);
}
void same_owner(const no::Owner& a, const no::Owner& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* w = std::get_if<no::WaitForApplied>(&a))
        same_handle(w->parent, std::get<no::WaitForApplied>(b).parent);
    else if (const auto* o = std::get_if<no::BindOpening>(&a)) {
        same_handle(o->opening, std::get<no::BindOpening>(b).opening);
        same_i64(o->cycle, std::get<no::BindOpening>(b).cycle);
    }
}
void same_group(const no::Group& a, const no::Group& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* m = std::get_if<no::Member>(&a)) {
        same_u64(m->group, std::get<no::Member>(b).group);
        same_i64(m->cohort, std::get<no::Member>(b).cohort);
        CHECK(m->effect == std::get<no::Member>(b).effect);
    }
}
void same_request(const no::Request& a, const no::Request& b, bool text) {
    same_intent(a.intent, b.intent);
    if (text) { same_str(a.label, b.label); same_str(a.comment, b.comment); }
    same_trigger(a.trigger, b.trigger);
    same_capacity(a.capacity, b.capacity);
    same_owner(a.owner, b.owner);
    same_group(a.group, b.group);
}
void same_birth(const no::Birth& a, const no::Birth& b) {
    same_u64(a.acceptance_ordinal, b.acceptance_ordinal);
    same_i64(a.decision_time_lower_bound, b.decision_time_lower_bound);
}
void same_definition(const no::DefinitionRef& a, const no::DefinitionRef& b, bool text) {
    CHECK(static_cast<bool>(a) == static_cast<bool>(b));
    if (!a || !b) return;
    same_handle(a->handle, b->handle);
    same_request(a->request, b->request, text);
    same_birth(a->birth, b->birth);
    CHECK(a->predecessor.has_value() == b->predecessor.has_value());
    if (a->predecessor) same_handle(*a->predecessor, *b->predecessor);
}
void same_remaining_proj(const no::RemainingProjection& a, const no::RemainingProjection& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* u = std::get_if<no::RemainingProjectionUnits>(&a))
        same_d(u->q, std::get<no::RemainingProjectionUnits>(b).q);
}
void same_remaining(const no::Remaining& a, const no::Remaining& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* u = std::get_if<no::RemainingUnits>(&a))
        same_d(u->q, std::get<no::RemainingUnits>(b).q);
}
void same_enrollment(const no::Enrollment& a, const no::Enrollment& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* c = std::get_if<no::EnrollmentFromCommand>(&a))
        same_event_id(c->accepted, std::get<no::EnrollmentFromCommand>(b).accepted);
    else if (const auto* p = std::get_if<no::EnrollmentFromApplied>(&a)) {
        same_event_id(p->cause, std::get<no::EnrollmentFromApplied>(b).cause);
        same_cursor(p->cursor, std::get<no::EnrollmentFromApplied>(b).cursor);
    }
}
void same_authority(const no::Authority& a, const no::Authority& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* w = std::get_if<no::Wait>(&a))
        same_handle(w->parent, std::get<no::Wait>(b).parent);
    else if (const auto* t = std::get_if<no::ArmedTransaction>(&a)) {
        same_handle(t->parent, std::get<no::ArmedTransaction>(b).parent);
        same_event_id(t->cause, std::get<no::ArmedTransaction>(b).cause);
        same_cursor(t->cause_cursor, std::get<no::ArmedTransaction>(b).cause_cursor);
    } else if (const auto* c = std::get_if<no::BookClose>(&a)) {
        same_i64(c->cycle, std::get<no::BookClose>(b).cycle);
        CHECK(c->side == std::get<no::BookClose>(b).side);
        same_event_id(c->binding_event, std::get<no::BookClose>(b).binding_event);
        same_cursor(c->binding_cursor, std::get<no::BookClose>(b).binding_cursor);
    } else if (const auto* o = std::get_if<no::OpeningClose>(&a)) {
        same_handle(o->opening, std::get<no::OpeningClose>(b).opening);
        same_i64(o->cycle, std::get<no::OpeningClose>(b).cycle);
        CHECK(o->side == std::get<no::OpeningClose>(b).side);
        same_enrollment(o->enrollment, std::get<no::OpeningClose>(b).enrollment);
    }
}
void same_trigger_state(const no::TriggerState& a, const no::TriggerState& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* t = std::get_if<no::TrailTrack>(&a))
        same_d(t->best, std::get<no::TrailTrack>(b).best);
    else if (const auto* t = std::get_if<no::TrailActive>(&a))
        same_d(t->best_at_trigger, std::get<no::TrailActive>(b).best_at_trigger);
}
void same_allowance(const no::Allowance& a, const no::Allowance& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* u = std::get_if<no::AllowanceUnits>(&a)) {
        same_u64(u->point_ordinal, std::get<no::AllowanceUnits>(b).point_ordinal);
        same_d(u->initial, std::get<no::AllowanceUnits>(b).initial);
        same_d(u->left, std::get<no::AllowanceUnits>(b).left);
    } else if (const auto* all = std::get_if<no::AllowanceAllScope>(&a)) {
        same_u64(all->point_ordinal, std::get<no::AllowanceAllScope>(b).point_ordinal);
    }
}
void same_pending(const no::PendingAdjustments& a, const no::PendingAdjustments& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* d = std::get_if<no::PendingDeferred>(&a)) {
        same_d(d->total, std::get<no::PendingDeferred>(b).total);
        same_u64(d->count, std::get<no::PendingDeferred>(b).count);
        same_event_id(d->tail_receipt, std::get<no::PendingDeferred>(b).tail_receipt);
    }
}
void same_scope(const execution::CloseScope& a, const execution::CloseScope& b) {
    REQUIRE(a.index() == b.index());
    if (const auto* o = std::get_if<execution::OpeningExposure>(&a)) {
        same_u64(o->incarnation, std::get<execution::OpeningExposure>(b).incarnation);
        same_i64(o->cycle, std::get<execution::OpeningExposure>(b).cycle);
    }
}
void same_opt_event(const std::optional<no::EventId>& a, const std::optional<no::EventId>& b) {
    CHECK(a.has_value() == b.has_value());
    if (a) same_event_id(*a, *b);
}
void same_opt_request(const std::optional<no::Request>& a, const std::optional<no::Request>& b, bool text) {
    CHECK(a.has_value() == b.has_value());
    if (a) same_request(*a, *b, text);
}

void same_accepted(const no::AcceptedEvent& a, const no::AcceptedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    CHECK(a.surface == b.surface);
}
void same_rejected(const no::RejectedEvent& a, const no::RejectedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_request(a.request, b.request, text);
    CHECK(a.reason == b.reason);
    CHECK(a.surface == b.surface);
}
void same_replaced(const no::ReplacedEvent& a, const no::ReplacedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.predecessor_definition, b.predecessor_definition, text);
    same_definition(a.successor_definition, b.successor_definition, text);
    CHECK(a.surface == b.surface);
}
void same_replace_rejected(const no::ReplaceRejectedEvent& a, const no::ReplaceRejectedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.live_definition, b.live_definition, text);
    same_request(a.attempted, b.attempted, text);
    CHECK(a.reason == b.reason);
    CHECK(a.surface == b.surface);
}
void same_cancelled(const no::CancelledEvent& a, const no::CancelledEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    CHECK(a.reason == b.reason);
    same_opt_event(a.cause, b.cause);
    same_authority(a.prior_authority, b.prior_authority);
    same_remaining_proj(a.unexecuted, b.unexecuted);
    same_pending(a.pending, b.pending);
}
void same_not_working(const no::NotWorkingEvent& a, const no::NotWorkingEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_handle(a.target, b.target);
    same_opt_request(a.attempted, b.attempted, text);
    CHECK(a.surface == b.surface);
}
void same_invalid_handle(const no::InvalidHandleEvent& a, const no::InvalidHandleEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_handle(a.target, b.target);
    same_opt_request(a.attempted, b.attempted, text);
    CHECK(a.surface == b.surface);
}
void same_no_effect(const no::NoEffectEvent& a, const no::NoEffectEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    same_remaining_proj(a.remaining, b.remaining);
    same_authority(a.authority, b.authority);
    same_cursor(a.cursor, b.cursor);
}
void same_match_rejected(const no::MatchRejectedEvent& a, const no::MatchRejectedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    CHECK(a.reason == b.reason);
    same_definition(a.definition, b.definition, text);
    same_remaining_proj(a.remaining, b.remaining);
    same_authority(a.authority, b.authority);
    same_cursor(a.cursor, b.cursor);
}
void same_close_bound(const no::CloseBoundEvent& a, const no::CloseBoundEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    same_i64(a.cycle, b.cycle);
    CHECK(a.side == b.side);
    same_cursor(a.cursor, b.cursor);
    same_authority(a.before, b.before);
    same_authority(a.after, b.after);
}
void same_activated(const no::ActivatedEvent& a, const no::ActivatedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    CHECK(a.kind == b.kind);
    same_trigger_state(a.before, b.before);
    same_trigger_state(a.after, b.after);
    same_d(a.reached_price, b.reached_price);
    same_cursor(a.cursor, b.cursor);
}
void same_applied(const no::ExecutionAppliedEvent& a, const no::ExecutionAppliedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    same_d(a.raw_price, b.raw_price);
    same_d(a.resolved_price, b.resolved_price);
    same_d(a.current_ticket, b.current_ticket);
    CHECK(a.first_trade_index == b.first_trade_index);
    CHECK(a.closed_trade_count == b.closed_trade_count);
    same_u64(a.opened_lot_incarnation, b.opened_lot_incarnation);
    same_d(a.closed_units, b.closed_units);
    same_d(a.opened_units, b.opened_units);
    same_d(a.filled_working, b.filled_working);
    same_remaining_proj(a.remaining_before, b.remaining_before);
    same_remaining_proj(a.remaining_after, b.remaining_after);
    same_allowance(a.allowance_before, b.allowance_before);
    same_allowance(a.allowance_after, b.allowance_after);
    CHECK(a.terminal == b.terminal);
    CHECK(a.terminal_reason.has_value() == b.terminal_reason.has_value());
    if (a.terminal_reason) CHECK(*a.terminal_reason == *b.terminal_reason);
    same_i64(a.cycle_before, b.cycle_before);
    same_i64(a.cycle_after, b.cycle_after);
    same_scope(a.scope, b.scope);
    same_cursor(a.cursor, b.cursor);
}
void same_reservation(const no::ReservationReducedEvent& a, const no::ReservationReducedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    same_event_id(a.cause, b.cause);
    same_handle(a.recipient, b.recipient);
    CHECK(a.effect == b.effect);
    same_d(a.requested_delta, b.requested_delta);
    same_d(a.actual_deduction, b.actual_deduction);
    same_d(a.before.q, b.before.q);
    same_remaining_proj(a.after, b.after);
}
void same_deferred(const no::DeferredGroupAdjustmentEvent& a, const no::DeferredGroupAdjustmentEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    same_event_id(a.cause, b.cause);
    same_handle(a.recipient, b.recipient);
    CHECK(a.effect == b.effect);
    same_d(a.deferred_delta, b.deferred_delta);
    same_pending(a.pending_before, b.pending_before);
    same_d(a.pending_after.total, b.pending_after.total);
    same_u64(a.pending_after.count, b.pending_after.count);
    same_event_id(a.pending_after.tail_receipt, b.pending_after.tail_receipt);
    same_opt_event(a.previous_pending_receipt, b.previous_pending_receipt);
}
void same_qty_bound(const no::QuantityBoundEvent& a, const no::QuantityBoundEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    same_event_id(a.source, b.source);
    same_d(a.source_units, b.source_units);
    REQUIRE(a.prior_adjustment_ids.size() == b.prior_adjustment_ids.size());
    for (size_t i = 0; i < a.prior_adjustment_ids.size(); ++i)
        same_event_id(a.prior_adjustment_ids[i], b.prior_adjustment_ids[i]);
    same_d(a.pending_total, b.pending_total);
    same_d(a.effective_deduction, b.effective_deduction);
    same_remaining_proj(a.remaining, b.remaining);
}
void same_armed(const no::ArmedEvent& a, const no::ArmedEvent& b, bool text) {
    same_u64(a.ordinal, b.ordinal);
    same_definition(a.definition, b.definition, text);
    same_authority(a.before, b.before);
    same_authority(a.after, b.after);
    same_enrollment(a.enrollment, b.enrollment);
    same_opt_event(a.quantity_resolution, b.quantity_resolution);
}
void same_command(const no::CommandEvent& a, const no::CommandEvent& b, bool text) {
    REQUIRE(a.index() == b.index());
    if (const auto* e = std::get_if<no::AcceptedEvent>(&a))
        same_accepted(*e, std::get<no::AcceptedEvent>(b), text);
    else if (const auto* e = std::get_if<no::RejectedEvent>(&a))
        same_rejected(*e, std::get<no::RejectedEvent>(b), text);
    else if (const auto* e = std::get_if<no::ReplacedEvent>(&a))
        same_replaced(*e, std::get<no::ReplacedEvent>(b), text);
    else if (const auto* e = std::get_if<no::ReplaceRejectedEvent>(&a))
        same_replace_rejected(*e, std::get<no::ReplaceRejectedEvent>(b), text);
    else if (const auto* e = std::get_if<no::CancelledEvent>(&a))
        same_cancelled(*e, std::get<no::CancelledEvent>(b), text);
    else if (const auto* e = std::get_if<no::NotWorkingEvent>(&a))
        same_not_working(*e, std::get<no::NotWorkingEvent>(b), text);
    else if (const auto* e = std::get_if<no::InvalidHandleEvent>(&a))
        same_invalid_handle(*e, std::get<no::InvalidHandleEvent>(b), text);
    else if (const auto* e = std::get_if<no::NoEffectEvent>(&a))
        same_no_effect(*e, std::get<no::NoEffectEvent>(b), text);
    else if (const auto* e = std::get_if<no::MatchRejectedEvent>(&a))
        same_match_rejected(*e, std::get<no::MatchRejectedEvent>(b), text);
    else if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&a))
        same_applied(*e, std::get<no::ExecutionAppliedEvent>(b), text);
    else if (const auto* e = std::get_if<no::CloseBoundEvent>(&a))
        same_close_bound(*e, std::get<no::CloseBoundEvent>(b), text);
    else if (const auto* e = std::get_if<no::ActivatedEvent>(&a))
        same_activated(*e, std::get<no::ActivatedEvent>(b), text);
    else if (const auto* e = std::get_if<no::ReservationReducedEvent>(&a))
        same_reservation(*e, std::get<no::ReservationReducedEvent>(b), text);
    else if (const auto* e = std::get_if<no::DeferredGroupAdjustmentEvent>(&a))
        same_deferred(*e, std::get<no::DeferredGroupAdjustmentEvent>(b), text);
    else if (const auto* e = std::get_if<no::QuantityBoundEvent>(&a))
        same_qty_bound(*e, std::get<no::QuantityBoundEvent>(b), text);
    else if (const auto* e = std::get_if<no::ArmedEvent>(&a))
        same_armed(*e, std::get<no::ArmedEvent>(b), text);
}
void same_driver(const NativeDriverPoint& a, const NativeDriverPoint& b) {
    same_coord(a.coordinate, b.coordinate);
    same_d(a.raw_price, b.raw_price);
    CHECK(a.sequence.has_value() == b.sequence.has_value());
    if (a.sequence) same_u64(*a.sequence, *b.sequence);
    CHECK(a.matching == b.matching);
    CHECK(a.excursion == b.excursion);
}
void same_account(const NativeAccountObservation& a, const NativeAccountObservation& b) {
    same_u64(a.ordinal, b.ordinal);
    same_i64(a.effective_time_ms, b.effective_time_ms);
    same_d(a.marked_equity, b.marked_equity);
    same_d(a.realized_balance, b.realized_balance);
    same_d(a.signed_units, b.signed_units);
}
void same_market_event(const NativeMarketEvent& a, const NativeMarketEvent& b, bool text) {
    CHECK(a.kind == b.kind);
    same_u64(a.ordinal, b.ordinal);
    CHECK(a.command.has_value() == b.command.has_value());
    CHECK(a.driver.has_value() == b.driver.has_value());
    CHECK(a.account.has_value() == b.account.has_value());
    if (a.command) same_command(*a.command, *b.command, text);
    if (a.driver) same_driver(*a.driver, *b.driver);
    if (a.account) same_account(*a.account, *b.account);
}
void same_trade(const Trade& a, const Trade& b, bool text) {
    same_i64(a.entry_time, b.entry_time);
    same_i64(a.exit_time, b.exit_time);
    same_d(a.entry_price, b.entry_price);
    same_d(a.exit_price, b.exit_price);
    same_d(a.qty, b.qty);
    same_d(a.pnl, b.pnl);
    same_d(a.pnl_pct, b.pnl_pct);
    CHECK(a.is_long == b.is_long);
    CHECK(a.entry_bar_index == b.entry_bar_index);
    CHECK(a.exit_bar_index == b.exit_bar_index);
    if (text) {
        same_str(a.entry_id, b.entry_id);
        same_str(a.entry_comment, b.entry_comment);
        same_str(a.exit_comment, b.exit_comment);
        same_str(a.exit_id, b.exit_id);
    }
    CHECK(a.exit_from_bracket == b.exit_from_bracket);
    same_d(a.max_runup, b.max_runup);
    same_d(a.max_drawdown, b.max_drawdown);
    same_d(a.commission, b.commission);
    same_u64(a.entry_incarnation, b.entry_incarnation);
    CHECK(a.open_at_end == b.open_at_end);
}
void same_lot(const PyramidEntry& a, const PyramidEntry& b, bool text) {
    same_d(a.price, b.price);
    same_i64(a.time, b.time);
    same_d(a.qty, b.qty);
    if (text) {
        same_str(a.entry_id, b.entry_id);
        same_str(a.entry_comment, b.entry_comment);
    }
    CHECK(a.entry_bar_index == b.entry_bar_index);
    same_d(a.max_runup, b.max_runup);
    same_d(a.max_drawdown, b.max_drawdown);
    CHECK(a.skip_entry_bar_high == b.skip_entry_bar_high);
    CHECK(a.skip_entry_bar_low == b.skip_entry_bar_low);
    CHECK(a.market_pyramid_add == b.market_pyramid_add);
    same_d(a.entry_path_position, b.entry_path_position);
    same_d(a.entry_commission_account, b.entry_commission_account);
    same_u64(a.entry_incarnation, b.entry_incarnation);
    CHECK(a.bracket_slot_shadowed == b.bracket_slot_shadowed);
    CHECK(a.ordinary_market_open == b.ordinary_market_open);
    CHECK(a.pooc_terminal_market_entry == b.pooc_terminal_market_entry);
    CHECK(a.ordinary_stop_open == b.ordinary_stop_open);
}
void same_action(const StreamOrderAction& a, const StreamOrderAction& b, bool text) {
    same_u64(a.sequence, b.sequence);
    same_i64(a.timestamp_ms, b.timestamp_ms);
    CHECK(a.bar_index == b.bar_index);
    CHECK(a.is_entry == b.is_entry);
    CHECK(a.is_long == b.is_long);
    same_d(a.quantity, b.quantity);
    same_d(a.price, b.price);
    if (text) {
        same_str(a.order_id, b.order_id);
        same_str(a.comment, b.comment);
    }
    same_u64(a.entry_incarnation, b.entry_incarnation);
}
void same_failure(const NativeFailure& a, const NativeFailure& b) {
    CHECK(a.code == b.code);
    CHECK(a.operation == b.operation);
    same_u64(a.ordinal, b.ordinal);
    CHECK(a.discriminator == b.discriminator);
    CHECK(a.context.kind == b.context.kind);
    same_u64(a.context.cause.ordinal, b.context.cause.ordinal);
    same_u64(a.context.recipient.incarnation, b.context.recipient.incarnation);
    same_coord(a.context.cursor.point, b.context.cursor.point);
    same_d(a.context.cursor.t, b.context.cursor.t);
}
void same_physical(const Host& a, const Host& b, bool text) {
    same_d(a.physical_position().signed_units, b.physical_position().signed_units);
    same_d(a.physical_position().average_price, b.physical_position().average_price);
    CHECK(a.physical_position().lot_count == b.physical_position().lot_count);
    REQUIRE(a.lots().size() == b.lots().size());
    for (size_t i = 0; i < a.lots().size(); ++i) same_lot(a.lots()[i], b.lots()[i], text);
    REQUIRE(a.trade_count() == b.trade_count());
    for (int i = 0; i < a.trade_count(); ++i) same_trade(a.get_trade(i), b.get_trade(i), text);
    REQUIRE(a.stream_order_actions_len() == b.stream_order_actions_len());
    for (int i = 0; i < a.stream_order_actions_len(); ++i)
        same_action(a.stream_order_action_at(i), b.stream_order_action_at(i), text);
    same_d(a.native_marked_equity(100), b.native_marked_equity(100));
}
void same_history(const Host& a, const Host& b, bool text) {
    const auto ea = a.native_events(0), eb = b.native_events(0);
    if (ea.size() != eb.size())
        std::printf("  event count %zu vs %zu\n", ea.size(), eb.size());
    REQUIRE(ea.size() == eb.size());
    for (size_t i = 0; i < ea.size(); ++i) same_market_event(ea[i], eb[i], text);
}
void same_hosts(const char* at, const Host& a, const Host& b) {
    boundary = at;
    const auto sa = a.native_state(), sb = b.native_state();
    CHECK(sa.kind == sb.kind);
    CHECK(sa.phase == sb.phase);
    CHECK(sa.completion == sb.completion);
    same_u64(sa.consumed_high_water, sb.consumed_high_water);
    same_i64(sa.decision_floor_ms, sb.decision_floor_ms);
    same_failure(sa.failure, sb.failure);
    same_u64(a.native_consumed_high_water(), b.native_consumed_high_water());
    same_i64(a.native_decision_floor(), b.native_decision_floor());
    const auto ha = a.native_continuation_hash();
    const auto hb = b.native_continuation_hash();
    if (ha != hb) std::printf("  hash %llx vs %llx\n", (unsigned long long)ha, (unsigned long long)hb);
    CHECK(ha == hb);
    CHECK(a.native_continuation_hash() == ha);
    CHECK(b.native_continuation_hash() == hb);
    CHECK(ha != 0);
    same_u64(a.stream_state_hash(), b.stream_state_hash());
    same_history(a, b, true);
    same_physical(a, b, true);
    boundary = "";
}

template<class Event>
std::vector<Event> events_of(const Host& h) {
    std::vector<Event> out;
    for (const auto& row : h.native_events(0)) {
        if (row.command) if (const auto* v = std::get_if<Event>(&*row.command)) out.push_back(*v);
    }
    return out;
}
std::vector<no::ExecutionAppliedEvent> fills(const Host& h, const no::RequestHandle& handle) {
    std::vector<no::ExecutionAppliedEvent> out;
    for (const auto& e : events_of<no::ExecutionAppliedEvent>(h)) {
        REQUIRE(e.definition);
        if (e.definition->handle == handle) out.push_back(e);
    }
    return out;
}
std::vector<no::ActivatedEvent> activations(const Host& h, const no::RequestHandle& handle) {
    std::vector<no::ActivatedEvent> out;
    for (const auto& e : events_of<no::ActivatedEvent>(h)) {
        REQUIRE(e.definition);
        if (e.definition->handle == handle) out.push_back(e);
    }
    return out;
}
std::vector<no::CancelledEvent> cancellations(const Host& h, const no::RequestHandle& handle) {
    std::vector<no::CancelledEvent> out;
    for (const auto& e : events_of<no::CancelledEvent>(h)) {
        REQUIRE(e.definition);
        if (e.definition->handle == handle) out.push_back(e);
    }
    return out;
}

void units_after(const no::ExecutionAppliedEvent& e, double before, double after) {
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(e.remaining_before));
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(e.remaining_after));
    same_d(std::get<no::RemainingProjectionUnits>(e.remaining_before).q, before);
    same_d(std::get<no::RemainingProjectionUnits>(e.remaining_after).q, after);
}

void failed_is_permanent(Host& h, int64_t next_offset) {
    REQUIRE(h.native_state().kind == NativeLifecycleKind::Failed);
    const auto before = h.native_state().failure;
    const auto hash = h.native_continuation_hash();
    const auto count = h.native_events(0).size();
    const auto position = h.physical_position();
    CHECK(!h.input(next_offset, 100));
    CHECK(!h.stream_advance_time(T + next_offset + 100));
    CHECK(!h.stream_end(false));
    bool threw = false;
    try { (void)h.submit(tx(1)); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(h.native_state().kind == NativeLifecycleKind::Failed);
    same_failure(h.native_state().failure, before);
    CHECK(h.native_events(0).size() == count);
    CHECK(h.native_continuation_hash() == hash);
    same_d(h.physical_position().signed_units, position.signed_units);
}

void replay_partial_budget(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-budget-L" : "P2-budget-S";
    start(a, key); start(b, key);
    same_hosts("start", a, b);
    auto r = tx(sign * 5, "budget");
    r.trigger = no::Limit{100};
    r.capacity = no::PointBudget{2};
    const auto ha = put(a, r), hb = put(b, r);
    CHECK(ha.incarnation == hb.incarnation);
    same_hosts("submit", a, b);
    a.tick(0, 100); b.tick(0, 100);
    same_hosts("p1", a, b);
    a.tick(1, 100); b.tick(1, 100);
    same_hosts("p2", a, b);
    a.tick(2, 100); b.tick(2, 100);
    same_hosts("p3", a, b);
    const auto fa = fills(a, ha);
    REQUIRE(fa.size() == 3);
    const double before[] = {5, 3, 1}, after[] = {3, 1, 0}, amt[] = {2, 2, 1};
    for (size_t i = 0; i < 3; ++i) {
        same_d(fa[i].filled_working, amt[i]);
        same_d(fa[i].opened_units, sign * amt[i]);
        units_after(fa[i], before[i], after[i]);
        REQUIRE(std::holds_alternative<no::AllowanceUnits>(fa[i].allowance_before));
        REQUIRE(std::holds_alternative<no::AllowanceUnits>(fa[i].allowance_after));
        const auto& left = std::get<no::AllowanceUnits>(fa[i].allowance_before);
        const auto& gone = std::get<no::AllowanceUnits>(fa[i].allowance_after);
        same_u64(left.point_ordinal, fa[i].cursor.point.ordinal);
        same_d(left.left, amt[i]);
        same_d(left.initial, amt[i]);
        same_u64(gone.point_ordinal, left.point_ordinal);
        same_d(gone.left, 0);
        CHECK(fa[i].terminal == (i == 2));
        CHECK(fa[i].cursor.point.provenance == NativePriceProvenance::ObservedPrint);
        CHECK(fa[i].cursor.t == 0);
        same_d(fa[i].raw_price, 100);
        if (i) CHECK(fa[i].cursor.point.ordinal != fa[i - 1].cursor.point.ordinal);
    }
    same_d(a.physical_position().signed_units, sign * 5);
    finish(a); finish(b);
    same_hosts("end", a, b);
}

void replay_stop_stoplimit_trail(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-triggers-L" : "P2-triggers-S";
    start(a, key); start(b, key);
    const auto open_a = put(a, tx(sign * 3, "open")), open_b = put(b, tx(sign * 3, "open"));
    a.tick(0, 100); b.tick(0, 100);
    same_hosts("opened", a, b);
    REQUIRE(fills(a, open_a).size() == 1);
    auto s = reduce(1, "stop"); s.trigger = no::Stop{100 - sign};
    auto sl = reduce(1, "stop-limit"); sl.trigger = no::StopLimit{100 - sign, 100 - sign * 2};
    auto tr = reduce(1, "trail"); tr.trigger = no::Trail{2, 100 + sign * 5};
    const auto sa = put(a, s), sb = put(b, s);
    const auto sla = put(a, sl), slb = put(b, sl);
    const auto ta = put(a, tr), tb = put(b, tr);
    (void)sb; (void)slb; (void)tb;
    same_hosts("working", a, b);
    a.tick(1, 100); b.tick(1, 100);
    same_hosts("quiet", a, b);
    CHECK(fills(a, sa).empty() && fills(a, sla).empty() && fills(a, ta).empty());
    CHECK(activations(a, sa).empty() && activations(a, sla).empty() && activations(a, ta).empty());
    a.tick(2, 100 + sign * 6); b.tick(2, 100 + sign * 6);
    same_hosts("arm", a, b);
    auto arm = activations(a, ta);
    REQUIRE(arm.size() == 1);
    CHECK(arm[0].kind == no::ActivationKind::TrailArm);
    same_d(arm[0].reached_price, 100 + sign * 6);
    REQUIRE(std::holds_alternative<no::TrailTrack>(arm[0].after));
    same_d(std::get<no::TrailTrack>(arm[0].after).best, 100 + sign * 6);
    CHECK(arm[0].cursor.point.provenance == NativePriceProvenance::ObservedPrint);
    CHECK(arm[0].cursor.t == 0);
    CHECK(fills(a, ta).empty());
    a.tick(3, 100 + sign * 10); b.tick(3, 100 + sign * 10);
    same_hosts("best", a, b);
    CHECK(fills(a, ta).empty());
    CHECK(activations(a, ta).size() == 1);
    a.tick(4, 100 + sign * 8); b.tick(4, 100 + sign * 8);
    same_hosts("trail-fill", a, b);
    auto tf = fills(a, ta);
    REQUIRE(tf.size() == 1);
    same_d(tf[0].raw_price, 100 + sign * 8);
    same_d(tf[0].closed_units, 1);
    CHECK(tf[0].terminal);
    auto trig = activations(a, ta);
    REQUIRE(trig.size() == 2);
    CHECK(trig[1].kind == no::ActivationKind::TrailTrigger);
    REQUIRE(std::holds_alternative<no::TrailActive>(trig[1].after));
    same_d(std::get<no::TrailActive>(trig[1].after).best_at_trigger, 100 + sign * 10);
    same_d(trig[1].reached_price, 100 + sign * 8);
    a.tick(5, 100 - sign); b.tick(5, 100 - sign);
    same_hosts("stop-print", a, b);
    auto sf = fills(a, sa);
    REQUIRE(sf.size() == 1);
    same_d(sf[0].raw_price, 100 - sign);
    auto sact = activations(a, sa);
    REQUIRE(sact.size() == 1);
    CHECK(sact[0].kind == no::ActivationKind::Stop);
    same_d(sact[0].reached_price, 100 - sign);
    CHECK(std::holds_alternative<no::StopActive>(sact[0].after));
    auto sl_act = activations(a, sla);
    REQUIRE(sl_act.size() == 1);
    CHECK(sl_act[0].kind == no::ActivationKind::StopLimit);
    CHECK(std::holds_alternative<no::StopLimitLive>(sl_act[0].after));
    a.tick(6, 100 - sign * 2); b.tick(6, 100 - sign * 2);
    same_hosts("stop-limit-fill", a, b);
    auto slf = fills(a, sla);
    REQUIRE(slf.size() == 1);
    CHECK(slf[0].terminal);
    CHECK(activations(a, sla).size() == 1);
    CHECK(slf[0].cursor.point.provenance == NativePriceProvenance::ObservedPrint);
    same_d(a.physical_position().signed_units, 0);
    CHECK(open_b.incarnation == open_a.incarnation);
    finish(a); finish(b);
    same_hosts("end", a, b);
}

void replay_deferred_bind(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-deferred-L" : "P2-deferred-S";
    start(a, key); start(b, key);
    auto p = tx(sign * 5, "parent"); p.trigger = no::Stop{100 + sign * 5};
    const auto pa = put(a, p), pb = put(b, p);
    no::Request child{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
    child.owner = no::WaitForApplied{pa};
    child.trigger = no::Limit{sign > 0 ? 200.0 : 1.0};
    child.group = no::Member{7, 0, no::GroupEffect::Reduce};
    no::Request child_b = child; child_b.owner = no::WaitForApplied{pb};
    const auto ca = put(a, child), cb = put(b, child_b);
    auto f = tx(sign * 2, "A"); f.group = no::Member{7, 1, no::GroupEffect::Reduce};
    const auto aa = put(a, f), ab = put(b, f);
    f.label = "B";
    const auto ba = put(a, f), bb = put(b, f);
    same_hosts("accepted", a, b);
    a.tick(0, 100); b.tick(0, 100);
    same_hosts("deferred", a, b);
    CHECK(fills(a, pa).empty());
    REQUIRE(fills(a, aa).size() == 1 && fills(a, ba).size() == 1);
    auto deferred = events_of<no::DeferredGroupAdjustmentEvent>(a);
    REQUIRE(deferred.size() == 2);
    same_d(deferred[0].deferred_delta, 2);
    same_d(deferred[1].deferred_delta, 2);
    CHECK(deferred[0].recipient == ca && deferred[1].recipient == ca);
    CHECK(deferred[0].cause.ordinal == fills(a, aa)[0].ordinal);
    CHECK(deferred[1].cause.ordinal == fills(a, ba)[0].ordinal);
    REQUIRE(std::holds_alternative<no::PendingNone>(deferred[0].pending_before));
    same_d(deferred[0].pending_after.total, 2);
    same_u64(deferred[0].pending_after.count, 1);
    same_d(deferred[1].pending_after.total, 4);
    same_u64(deferred[1].pending_after.count, 2);
    a.tick(1, 100 + sign * 5); b.tick(1, 100 + sign * 5);
    same_hosts("bound", a, b);
    auto pf = fills(a, pa);
    REQUIRE(pf.size() == 1);
    same_d(pf[0].opened_units, sign * 5);
    auto bound = events_of<no::QuantityBoundEvent>(a);
    REQUIRE(bound.size() == 1);
    CHECK(bound[0].source.ordinal == pf[0].ordinal);
    same_d(bound[0].source_units, 5);
    same_d(bound[0].pending_total, 4);
    same_d(bound[0].effective_deduction, 4);
    REQUIRE(bound[0].prior_adjustment_ids.size() == 2);
    CHECK(bound[0].prior_adjustment_ids[0].ordinal == deferred[0].ordinal);
    CHECK(bound[0].prior_adjustment_ids[1].ordinal == deferred[1].ordinal);
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(bound[0].remaining));
    same_d(std::get<no::RemainingProjectionUnits>(bound[0].remaining).q, 1);
    CHECK(fills(a, ca).empty());
    a.tick(2, sign > 0 ? 200.0 : 1.0); b.tick(2, sign > 0 ? 200.0 : 1.0);
    same_hosts("child-fill", a, b);
    auto cf = fills(a, ca);
    REQUIRE(cf.size() == 1);
    same_d(cf[0].closed_units, 1);
    CHECK(cf[0].terminal);
    const auto* scope = std::get_if<execution::OpeningExposure>(&cf[0].scope);
    REQUIRE(scope);
    CHECK(scope->incarnation == pa.incarnation);
    CHECK(scope->cycle == pf[0].cycle_after);
    same_d(a.physical_position().signed_units, sign * 8);
    CHECK(cb.incarnation == ca.incarnation);
    CHECK(ab.incarnation == aa.incarnation && bb.incarnation == ba.incarnation);
    finish(a); finish(b);
    same_hosts("end", a, b);
}

void replay_owner_expiry_replace(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-owner-L" : "P2-owner-S";
    start(a, key); start(b, key);
    auto p = tx(sign, "parent"); p.trigger = no::Stop{200};
    const auto pa = put(a, p), pb = put(b, p);
    auto c = reduce(1, "child"); c.owner = no::WaitForApplied{pa};
    auto c2 = c; c2.owner = no::WaitForApplied{pb};
    const auto ca = put(a, c), cb = put(b, c2);
    same_hosts("waiting", a, b);
    const auto xa = a.cancel(pa), xb = b.cancel(pb);
    CHECK(xa.status == no::CancelStatus::Cancelled);
    CHECK(xb.status == no::CancelStatus::Cancelled);
    same_hosts("cancelled", a, b);
    auto pc = cancellations(a, pa), cc = cancellations(a, ca);
    REQUIRE(pc.size() == 1 && cc.size() == 1);
    CHECK(pc[0].reason == no::CancelReason::User);
    CHECK(cc[0].reason == no::CancelReason::OwnerGone);
    REQUIRE(cc[0].cause);
    CHECK(cc[0].cause->ordinal == pc[0].ordinal);
    CHECK(std::holds_alternative<no::Wait>(cc[0].prior_authority));
    CHECK(cb.incarnation == ca.incarnation);
    auto q = tx(sign, "replace-me"); q.trigger = no::Stop{200};
    const auto qa = put(a, q), qb = put(b, q);
    auto w = reduce(1, "wait-replace"); w.owner = no::WaitForApplied{qa};
    auto w2 = w; w2.owner = no::WaitForApplied{qb};
    const auto wa = put(a, w), wb = put(b, w2);
    auto nxt = tx(sign, "successor"); nxt.trigger = no::Stop{201};
    const auto ra = a.replace(qa, nxt), rb = b.replace(qb, nxt);
    CHECK(ra.status == no::ReplaceStatus::Replaced && ra.successor);
    CHECK(rb.status == no::ReplaceStatus::Replaced && rb.successor);
    same_hosts("replaced", a, b);
    auto wc = cancellations(a, wa);
    REQUIRE(wc.size() == 1);
    CHECK(wc[0].reason == no::CancelReason::OwnerGone);
    REQUIRE(events_of<no::ReplacedEvent>(a).size() >= 1);
    CHECK(wb.incarnation == wa.incarnation);
    REQUIRE(ra.successor->incarnation != qa.incarnation);
    finish(a); finish(b);
    same_hosts("end", a, b);
}

void replay_confirmed_bar(double sign) {
    Host a, b;
    std::vector<uint64_t> ha, hb;
    std::vector<std::vector<NativeMarketEvent>> sa, sb;
    no::RequestHandle trail_a, stop_a, trail_b, stop_b;
    auto setup = [&](Host& h, no::RequestHandle& trail, no::RequestHandle& stop,
                     std::vector<uint64_t>& hashes,
                     std::vector<std::vector<NativeMarketEvent>>& snaps) {
        h.beginning = [&](Host& self) { put(self, tx(sign * 2, "open")); };
        h.calculation = [&](Host& self) {
            if (self.calculations == 1) {
                auto t = reduce(1, "trail"); t.trigger = no::Trail{2, 100 + sign * 5}; trail = put(self, t);
                auto s = reduce(1, "stop"); s.trigger = no::Stop{100 - sign}; stop = put(self, s);
            }
            hashes.push_back(self.native_continuation_hash());
            snaps.push_back(self.native_events(0));
        };
    };
    setup(a, trail_a, stop_a, ha, sa);
    setup(b, trail_b, stop_b, hb, sb);
    const char* key = sign > 0 ? "P2-ohlc-L" : "P2-ohlc-S";
    REQUIRE(a.configure_native(configuration(key)).status == NativeSetupStatus::Applied);
    REQUIRE(b.configure_native(configuration(key)).status == NativeSetupStatus::Applied);
    const Bar tape[] = {{100, 100, 100, 100, 1, T},
        sign > 0 ? Bar{100, 110, 90, 100, 1, T + 60000} : Bar{100, 110, 90, 100, 1, T + 60000}};
    a.run(tape, 2); b.run(tape, 2);
    REQUIRE(a.native_state().kind == NativeLifecycleKind::Completed);
    REQUIRE(b.native_state().kind == NativeLifecycleKind::Completed);
    REQUIRE(ha.size() == hb.size());
    REQUIRE(sa.size() == sb.size());
    for (size_t i = 0; i < ha.size(); ++i) {
        boundary = "bar-boundary";
        CHECK(ha[i] == hb[i]);
        REQUIRE(sa[i].size() == sb[i].size());
        for (size_t j = 0; j < sa[i].size(); ++j) same_market_event(sa[i][j], sb[i][j], true);
    }
    same_hosts("complete", a, b);
    auto arm = activations(a, trail_a);
    REQUIRE(arm.size() >= 1);
    CHECK(arm[0].kind == no::ActivationKind::TrailArm);
    same_d(arm[0].reached_price, 100 + sign * 5);
    CHECK(arm[0].cursor.point.provenance == NativePriceProvenance::Confirmed);
    auto sf = fills(a, stop_a);
    REQUIRE(sf.size() == 1);
    same_d(sf[0].raw_price, 100 - sign);
    CHECK(stop_b.incarnation == stop_a.incarnation);
    CHECK(trail_b.incarnation == trail_a.incarnation);
}

void replay_failed_capacity(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-C8-L" : "P2-C8-S";
    start(a, key); start(b, key);
    auto r = tx(sign * 0x1p60, "huge"); r.capacity = no::PointBudget{1};
    const auto ha = put(a, r), hb = put(b, r);
    same_hosts("accepted", a, b);
    CHECK(!a.input(0, 100)); CHECK(!b.input(0, 100));
    same_hosts("failed", a, b);
    REQUIRE(a.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(a.native_state().failure.code == NativeFailureCode::SettlementFailure);
    CHECK(a.native_state().failure.discriminator
          == static_cast<uint32_t>(no::CoreFailure::NonrepresentableQuantity));
    CHECK(native_failure_has_recipient(a.native_state().failure.context));
    CHECK(a.native_state().failure.context.recipient.incarnation == ha.incarnation);
    CHECK(fills(a, ha).empty());
    CHECK(events_of<no::ExecutionAppliedEvent>(a).empty());
    same_d(a.physical_position().signed_units, 0);
    failed_is_permanent(a, 1);
    failed_is_permanent(b, 1);
    same_hosts("refused", a, b);
    CHECK(hb.incarnation == ha.incarnation);
}

void replay_failed_group(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-G5-L" : "P2-G5-S";
    start(a, key, 1, 2); start(b, key, 1, 2);
    auto waiting = tx(sign * 0x1p60, "recipient");
    waiting.trigger = no::Limit{sign > 0 ? 1.0 : 200.0};
    waiting.group = no::Member{7, 2, no::GroupEffect::Cancel};
    const auto ra = put(a, waiting), rb = put(b, waiting);
    auto emit = tx(sign, "filler"); emit.group = no::Member{7, 1, no::GroupEffect::Reduce};
    const auto fa = put(a, emit), fb = put(b, emit);
    same_hosts("accepted", a, b);
    CHECK(!a.input(0, 100)); CHECK(!b.input(0, 100));
    same_hosts("failed", a, b);
    REQUIRE(fills(a, fa).size() == 1);
    CHECK(fills(a, fa)[0].terminal);
    CHECK(fills(a, ra).empty());
    CHECK(events_of<no::ReservationReducedEvent>(a).empty());
    const auto failure = a.native_state().failure;
    CHECK(failure.discriminator == static_cast<uint32_t>(no::CoreFailure::UnrepresentableReservation));
    CHECK(native_failure_has_cause(failure.context) && native_failure_has_recipient(failure.context));
    CHECK(failure.context.cause.ordinal == fills(a, fa)[0].ordinal);
    CHECK(failure.context.recipient.incarnation == ra.incarnation);
    same_d(a.physical_position().signed_units, sign);
    failed_is_permanent(a, 1);
    failed_is_permanent(b, 1);
    same_hosts("refused", a, b);
    CHECK(fb.incarnation == fa.incarnation && rb.incarnation == ra.incarnation);
}

void perturb_one_field(double sign) {
    struct Step {
        const char* name;
        no::Request base;
        no::Request changed;
        bool physical_diverges;
    };
    auto limit = tx(sign * 2, "base", "c");
    limit.trigger = no::Limit{100};
    auto qty = limit; std::get<no::Transact>(qty.intent).signed_units = sign * 3;
    auto trig = limit; trig.trigger = no::Limit{100 - sign};
    auto cap = limit; cap.capacity = no::PointBudget{1};
    auto grp = limit; grp.group = no::Member{9, 1, no::GroupEffect::Cancel};
    const Step steps[] = {
        {"units", limit, qty, true},
        {"trigger", limit, trig, true},
        {"capacity", limit, cap, true},
        {"group", limit, grp, false},
    };
    for (const auto& step : steps) {
        Host a, b;
        const std::string key = std::string("P2-pert-") + step.name + (sign > 0 ? "-L" : "-S");
        start(a, key.c_str()); start(b, key.c_str());
        put(a, step.base); put(b, step.changed);
        boundary = step.name;
        const auto ha = a.native_continuation_hash(), hb = b.native_continuation_hash();
        CHECK(ha != hb);
        // Distinction is in request history before any physical fill.
        same_d(a.physical_position().signed_units, 0);
        same_d(b.physical_position().signed_units, 0);
        CHECK(a.trade_count() == 0 && b.trade_count() == 0);
        REQUIRE(events_of<no::AcceptedEvent>(a).size() == 1);
        REQUIRE(events_of<no::AcceptedEvent>(b).size() == 1);
        same_request(events_of<no::AcceptedEvent>(a)[0].request(), step.base, true);
        same_request(events_of<no::AcceptedEvent>(b)[0].request(), step.changed, true);
        CHECK(events_of<no::ExecutionAppliedEvent>(a).empty());
        CHECK(events_of<no::ExecutionAppliedEvent>(b).empty());
        a.tick(0, 100); b.tick(0, 100);
        CHECK(a.native_continuation_hash() != b.native_continuation_hash());
        if (step.physical_diverges) {
            CHECK(!bits_eq(a.physical_position().signed_units, b.physical_position().signed_units)
                  || a.trade_count() != b.trade_count()
                  || fills(a, events_of<no::AcceptedEvent>(a)[0].handle()).size()
                         != fills(b, events_of<no::AcceptedEvent>(b)[0].handle()).size());
        } else {
            same_d(a.physical_position().signed_units, b.physical_position().signed_units);
            same_d(a.physical_position().average_price, b.physical_position().average_price);
            CHECK(a.trade_count() == b.trade_count());
        }
        if (a.native_state().kind == NativeLifecycleKind::Running) finish(a);
        if (b.native_state().kind == NativeLifecycleKind::Running) finish(b);
    }
}

void perturb_owner(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-pert-owner-L" : "P2-pert-owner-S";
    start(a, key); start(b, key);
    put(a, tx(sign * 2, "seed")); put(b, tx(sign * 2, "seed"));
    a.tick(0, 100); b.tick(0, 100);
    auto parent_req = tx(sign, "later");
    parent_req.trigger = no::Limit{sign > 0 ? 1.0 : 10000.0};
    const auto pa = put(a, parent_req), pb = put(b, parent_req);
    auto independent = reduce(1, "close");
    auto waiting = independent; waiting.owner = no::WaitForApplied{pb};
    put(a, independent); put(b, waiting);
    boundary = "owner-submit";
    CHECK(a.native_continuation_hash() != b.native_continuation_hash());
    same_d(a.physical_position().signed_units, sign * 2);
    same_d(b.physical_position().signed_units, sign * 2);
    a.tick(1, 100); b.tick(1, 100);
    CHECK(a.native_continuation_hash() != b.native_continuation_hash());
    same_d(a.physical_position().signed_units, sign);
    same_d(b.physical_position().signed_units, sign * 2);
    CHECK(events_of<no::ExecutionAppliedEvent>(a).size()
          > events_of<no::ExecutionAppliedEvent>(b).size());
    CHECK(pa.incarnation == pb.incarnation);
    finish(a); finish(b);
}

void inert_text(double sign, bool comment) {
    Host a, b;
    const char* key = comment
        ? (sign > 0 ? "P2-comment-L" : "P2-comment-S")
        : (sign > 0 ? "P2-label-L" : "P2-label-S");
    start(a, key, 1, 6); start(b, key, 1, 6);
    no::Request ra = tx(sign * 2, "keep", "keep");
    no::Request rb = ra;
    if (comment) rb.comment = "other"; else rb.label = "other";
    const auto ha = put(a, ra), hb = put(b, rb);
    boundary = "text-submit";
    CHECK(a.native_continuation_hash() != b.native_continuation_hash());
    same_physical(a, b, false);
    same_d(a.physical_position().signed_units, 0);
    a.tick(0, 100); b.tick(0, 100);
    CHECK(a.native_continuation_hash() != b.native_continuation_hash());
    same_physical(a, b, false);
    same_d(a.physical_position().signed_units, sign * 2);
    same_d(a.physical_position().average_price, 100);
    REQUIRE(a.lots().size() == 1 && b.lots().size() == 1);
    same_d(a.lots()[0].qty, b.lots()[0].qty);
    same_d(a.lots()[0].price, b.lots()[0].price);
    same_d(a.lots()[0].entry_commission_account, b.lots()[0].entry_commission_account);
    CHECK(a.lots()[0].entry_incarnation == ha.incarnation);
    CHECK(b.lots()[0].entry_incarnation == hb.incarnation);
    auto fa = fills(a, ha), fb = fills(b, hb);
    REQUIRE(fa.size() == 1 && fb.size() == 1);
    same_d(fa[0].raw_price, fb[0].raw_price);
    same_d(fa[0].opened_units, fb[0].opened_units);
    same_d(fa[0].current_ticket, fb[0].current_ticket);
    same_d(fa[0].current_ticket, 6);
    CHECK(ha.incarnation == hb.incarnation);
    finish(a); finish(b);
    same_physical(a, b, false);
}

void labels_do_not_select(double sign) {
    Host a, b;
    const char* key = sign > 0 ? "P2-label-select-L" : "P2-label-select-S";
    start(a, key); start(b, key);
    // Same labels on two openings; ownership is the handle, not the text.
    const auto a1 = put(a, tx(sign, "same"));
    const auto a2 = put(a, tx(sign, "same"));
    const auto b1 = put(b, tx(sign, "other"));
    const auto b2 = put(b, tx(sign, "other"));
    auto ca = reduce(1, "child"); ca.owner = no::WaitForApplied{a2};
    auto cb = reduce(1, "child"); cb.owner = no::WaitForApplied{b2};
    const auto cha = put(a, ca), chb = put(b, cb);
    CHECK(a.native_continuation_hash() != b.native_continuation_hash());
    a.tick(0, 100); b.tick(0, 100);
    auto fa2 = fills(a, a2), fc = fills(a, cha);
    REQUIRE(fa2.size() == 1 && fc.size() == 1);
    const auto* scope = std::get_if<execution::OpeningExposure>(&fc[0].scope);
    REQUIRE(scope);
    CHECK(scope->incarnation == a2.incarnation);
    CHECK(scope->incarnation != a1.incarnation);
    auto fb2 = fills(b, b2), fbc = fills(b, chb);
    REQUIRE(fb2.size() == 1 && fbc.size() == 1);
    const auto* scope_b = std::get_if<execution::OpeningExposure>(&fbc[0].scope);
    REQUIRE(scope_b);
    CHECK(scope_b->incarnation == b2.incarnation);
    CHECK(scope_b->incarnation != b1.incarnation);
    same_physical(a, b, false);
    same_d(a.physical_position().signed_units, sign);
    CHECK(a1.incarnation == b1.incarnation && a2.incarnation == b2.incarnation);
    finish(a); finish(b);
}

void reset_clears_state(double sign) {
    Host h;
    const char* key = sign > 0 ? "P2-reset-L" : "P2-reset-S";
    start(h, key, 1);
    const auto seed = put(h, tx(sign * 2, "seed"));
    CHECK(seed.incarnation == 1);
    h.tick(0, 100);
    const auto opened1 = fills(h, seed);
    REQUIRE(opened1.size() == 1);
    const auto cycle = opened1[0].cycle_after;
    auto parent = tx(sign * 5, "parent");
    parent.trigger = no::Limit{sign > 0 ? 1.0 : 10000.0};
    const auto p1 = put(h, parent);
    no::Request child{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
    child.owner = no::WaitForApplied{p1};
    child.group = no::Member{7, 0, no::GroupEffect::Reduce};
    const auto c1 = put(h, child);
    auto filler = tx(sign * 2, "filler"); filler.group = no::Member{7, 1, no::GroupEffect::Reduce};
    put(h, filler);
    auto stop = reduce(1, "live-stop"); stop.trigger = no::Stop{100 - sign};
    stop.capacity = no::PointBudget{1};
    const auto stop_h = put(h, stop);
    h.tick(1, 100);
    REQUIRE(!events_of<no::DeferredGroupAdjustmentEvent>(h).empty());
    h.tick(2, 100 - sign);
    REQUIRE(!activations(h, stop_h).empty());
    CHECK(seed.run.run_number == 1);
    finish(h);
    const auto old_hash = h.native_continuation_hash();
    const auto old_events = h.native_events(0).size();
    start(h, key, 2);
    CHECK(h.native_state().kind == NativeLifecycleKind::Running);
    CHECK(h.native_continuation_hash() != old_hash);
    CHECK(h.native_events(0).size() < old_events);
    CHECK(events_of<no::DeferredGroupAdjustmentEvent>(h).empty());
    CHECK(events_of<no::ActivatedEvent>(h).empty());
    CHECK(events_of<no::QuantityBoundEvent>(h).empty());
    CHECK(events_of<no::ExecutionAppliedEvent>(h).empty());
    CHECK(events_of<no::AcceptedEvent>(h).empty());
    same_d(h.physical_position().signed_units, 0);
    CHECK(h.lots().empty());
    CHECK(h.trade_count() == 0);
    const auto fresh = put(h, tx(sign * 2, "new"));
    CHECK(fresh.incarnation == 1);
    CHECK(fresh.incarnation == seed.incarnation);
    CHECK(fresh.run.run_number == 2);
    CHECK(seed.run.run_number == 1);
    h.tick(0, 100);
    const auto opened = fills(h, fresh);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].cycle_after == cycle);
    auto stale_bind = reduce(1);
    stale_bind.owner = no::BindOpening{seed, cycle};
    const auto refused = h.submit(stale_bind);
    CHECK(refused.status == no::SubmitStatus::Rejected && !refused.handle);
    CHECK(refused.reason == no::RequestRejectReason::InvalidOwner);
    const auto stale_cancel = h.cancel(seed);
    CHECK(stale_cancel.status == no::CancelStatus::InvalidHandle);
    const auto stale_child = h.cancel(c1);
    CHECK(stale_child.status == no::CancelStatus::InvalidHandle);
    const auto stale_parent = h.cancel(p1);
    CHECK(stale_parent.status == no::CancelStatus::InvalidHandle);
    auto invalid = events_of<no::InvalidHandleEvent>(h);
    REQUIRE(invalid.size() >= 3);
    CHECK(invalid[0].target.run.run_number == 1);
    CHECK(invalid[0].target.incarnation == seed.incarnation);
    auto current_bind = reduce(1);
    current_bind.owner = no::BindOpening{fresh, opened[0].cycle_after};
    const auto ok = put(h, current_bind);
    h.tick(1, 100);
    auto cf = fills(h, ok);
    REQUIRE(cf.size() == 1);
    const auto* scope = std::get_if<execution::OpeningExposure>(&cf[0].scope);
    REQUIRE(scope);
    CHECK(scope->incarnation == fresh.incarnation);
    CHECK(scope->cycle == opened[0].cycle_after);
    auto again = reduce(1, "new-stop"); again.trigger = no::Stop{100 + sign * 50};
    const auto idle = put(h, again);
    CHECK(activations(h, idle).empty());
    CHECK(events_of<no::DeferredGroupAdjustmentEvent>(h).empty());
    finish(h);
}

void ieee_rejection_payloads() {
    Host a, b;
    start(a, "P2-ieee"); start(b, "P2-ieee");
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const double nnan = std::copysign(qnan, -1.0);
    const double n1 = bits_to(0x7ff8000000000001ULL);
    const double n2 = bits_to(0x7ff8000000000002ULL);
    const double neg0 = std::copysign(0.0, -1.0);
    struct Row { no::Request request; no::RequestRejectReason reason; };
    const Row rows[] = {
        {tx(qnan, "qnan"), no::RequestRejectReason::InvalidQuantity},
        {tx(nnan, "nnan"), no::RequestRejectReason::InvalidQuantity},
        {tx(n1, "n1"), no::RequestRejectReason::InvalidQuantity},
        {tx(n2, "n2"), no::RequestRejectReason::InvalidQuantity},
        {tx(neg0, "neg0"), no::RequestRejectReason::InvalidQuantity},
        {reduce(qnan, "rqnan"), no::RequestRejectReason::InvalidQuantity},
        {[] { auto r = tx(1, "stop-nan"); r.trigger = no::Stop{std::numeric_limits<double>::quiet_NaN()}; return r; }(),
         no::RequestRejectReason::InvalidTrigger},
        {[] { auto r = tx(1, "stop-n1"); r.trigger = no::Stop{bits_to(0x7ff8000000000001ULL)}; return r; }(),
         no::RequestRejectReason::InvalidTrigger},
    };
    for (const auto& row : rows) {
        const auto ra = a.submit(row.request);
        const auto rb = b.submit(row.request);
        CHECK(ra.status == no::SubmitStatus::Rejected && rb.status == no::SubmitStatus::Rejected);
        CHECK(ra.reason == row.reason && rb.reason == row.reason);
        CHECK(!ra.handle && !rb.handle);
    }
    same_hosts("ieee-both", a, b);
    auto rejected = events_of<no::RejectedEvent>(a);
    REQUIRE(rejected.size() == sizeof(rows) / sizeof(rows[0]));
    for (size_t i = 0; i < rejected.size(); ++i) {
        CHECK(rejected[i].reason == rows[i].reason);
        same_request(rejected[i].request, rows[i].request, true);
        if (const auto* t = std::get_if<no::Transact>(&rows[i].request.intent)) {
            REQUIRE(std::holds_alternative<no::Transact>(rejected[i].request.intent));
            same_d(std::get<no::Transact>(rejected[i].request.intent).signed_units, t->signed_units);
        }
        if (const auto* r = std::get_if<no::Reduce>(&rows[i].request.intent)) {
            if (const auto* u = std::get_if<no::ExplicitUnits>(&r->size)) {
                REQUIRE(std::holds_alternative<no::Reduce>(rejected[i].request.intent));
                same_d(std::get<no::ExplicitUnits>(
                           std::get<no::Reduce>(rejected[i].request.intent).size).units,
                       u->units);
            }
        }
        if (const auto* s = std::get_if<no::Stop>(&rows[i].request.trigger)) {
            REQUIRE(std::holds_alternative<no::Stop>(rejected[i].request.trigger));
            same_d(std::get<no::Stop>(rejected[i].request.trigger).price, s->price);
        }
    }
    CHECK(!bits_eq(n1, n2));
    CHECK(bits_of(std::get<no::Transact>(rejected[2].request.intent).signed_units)
          == 0x7ff8000000000001ULL);
    CHECK(bits_of(std::get<no::Transact>(rejected[3].request.intent).signed_units)
          == 0x7ff8000000000002ULL);
    same_d(a.physical_position().signed_units, 0);
    Host c, d;
    start(c, "P2-ieee-distinct-a"); start(d, "P2-ieee-distinct-b");
    (void)c.submit(tx(n1, "n1"));
    (void)d.submit(tx(n2, "n1"));
    CHECK(c.native_continuation_hash() != d.native_continuation_hash());
    same_d(c.physical_position().signed_units, 0);
    same_d(d.physical_position().signed_units, 0);
    finish(a); finish(b);
}

void run_case(const char* name, const std::function<void()>& body) {
    scenario = name; boundary = ""; ++cases;
    const int previous = failures;
    try { body(); }
    catch (const StopCase&) {}
    catch (const std::exception& e) {
        std::printf("FAIL %s unexpected exception: %s\n", name, e.what()); ++failures;
    } catch (...) {
        std::printf("FAIL %s unexpected nonstandard exception\n", name); ++failures;
    }
    std::printf("%s %s\n", failures == previous ? "PASS" : "FAIL", name);
}
}  // namespace

int main() {
    for (double sign : {1.0, -1.0}) {
        run_case(sign > 0 ? "identical partial-budget long" : "identical partial-budget short",
                 [&] { replay_partial_budget(sign); });
        run_case(sign > 0 ? "identical Stop/StopLimit/Trail long" : "identical Stop/StopLimit/Trail short",
                 [&] { replay_stop_stoplimit_trail(sign); });
        run_case(sign > 0 ? "identical deferred bind long" : "identical deferred bind short",
                 [&] { replay_deferred_bind(sign); });
        run_case(sign > 0 ? "identical owner expiry/replace long" : "identical owner expiry/replace short",
                 [&] { replay_owner_expiry_replace(sign); });
        run_case(sign > 0 ? "identical confirmed-bar long" : "identical confirmed-bar short",
                 [&] { replay_confirmed_bar(sign); });
        run_case(sign > 0 ? "failed capacity prefix long" : "failed capacity prefix short",
                 [&] { replay_failed_capacity(sign); });
        run_case(sign > 0 ? "failed group prefix long" : "failed group prefix short",
                 [&] { replay_failed_group(sign); });
        run_case(sign > 0 ? "perturb fields long" : "perturb fields short",
                 [&] { perturb_one_field(sign); });
        run_case(sign > 0 ? "perturb owner long" : "perturb owner short",
                 [&] { perturb_owner(sign); });
        run_case(sign > 0 ? "inert label long" : "inert label short",
                 [&] { inert_text(sign, false); });
        run_case(sign > 0 ? "inert comment long" : "inert comment short",
                 [&] { inert_text(sign, true); });
        run_case(sign > 0 ? "labels do not select long" : "labels do not select short",
                 [&] { labels_do_not_select(sign); });
        run_case(sign > 0 ? "reset next run long" : "reset next run short",
                 [&] { reset_clears_state(sign); });
    }
    run_case("IEEE rejection payloads", [] { ieee_rejection_payloads(); });
    std::printf("%s native resting replay: %d cases, %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", cases, checks, failures);
    return failures ? 1 : 0;
}
