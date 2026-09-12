// Bounded R2 owner/lifetime acceptance: C3/C4/C6/C7 and O2-O5/O7-O9.
// Real NativeStrategyHost commands, warmup+tick or confirmed-bar drivers, and
// physical observations only. O1/O6/O10-O12 are covered elsewhere.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
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
struct StopCase {};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #x); } } while (0)
#define REQUIRE(x) do { const bool ok_ = bool(x); CHECK(ok_); if (!ok_) throw StopCase{}; } while (0)

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
    s.identity = {key, run}; s.input_tf = "1"; s.script_tf = "1";
    s.ticker = "N"; s.tickerid = "TEST:N"; s.type = "crypto";
    s.currency = "USD"; s.basecurrency = "USD"; s.description = "R2 owner acceptance";
    s.volumetype = "base"; s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 10000; s.point_value = 1; s.account_fx = 1;
    s.price_tick = .01; s.fee_kind = NativeFeeKind::CashPerExecution; s.fee_value = fee;
    return s;
}

void start(Host& h, const char* key, uint64_t run = 1, double fee = 0,
           double first_price = 100,
           const std::function<void(NativeRunSpec&)>& tweak = {}) {
    auto spec = configuration(key, run, fee);
    if (tweak) tweak(spec);
    REQUIRE(h.configure_native(spec).status == NativeSetupStatus::Applied);
    auto beginning = std::exchange(h.beginning, {});
    const Bar warmup{first_price, first_price, first_price, first_price, 1, T - 60000};
    h.sequence = 0;
    REQUIRE(h.stream_begin(&warmup, 1, "1", "1"));
    REQUIRE(h.stream_push_tick(TradeTick{T, ++h.sequence, first_price, 0}));
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
no::Request event_reduce(const char* label = "event-child") {
    return {no::Reduce{no::OwnerOpenedUnits{}}, label, ""};
}
no::Request flatten(const char* label = "flatten") { return {no::Flatten{}, label, ""}; }

no::RequestHandle put(Host& h, const no::Request& request) {
    auto result = h.submit(request);
    REQUIRE(result.status == no::SubmitStatus::Accepted);
    REQUIRE(result.handle.has_value());
    return *result.handle;
}

template<class Event> std::vector<Event> events(const Host& h) {
    std::vector<Event> result;
    for (const auto& event : h.native_events(0)) {
        if (event.command) if (const auto* value = std::get_if<Event>(&*event.command))
            result.push_back(*value);
    }
    return result;
}
template<class Event>
std::vector<Event> events(const Host& h, const no::RequestHandle& handle) {
    std::vector<Event> result;
    for (const auto& event : events<Event>(h)) {
        REQUIRE(event.definition);
        if (event.definition->handle == handle) result.push_back(event);
    }
    return result;
}
std::vector<no::ExecutionAppliedEvent> fills(const Host& h, const no::RequestHandle& handle) {
    return events<no::ExecutionAppliedEvent>(h, handle);
}
std::vector<no::CancelledEvent> cancellations(const Host& h, const no::RequestHandle& handle) {
    return events<no::CancelledEvent>(h, handle);
}

double units_after(const no::ExecutionAppliedEvent& event) {
    const auto* remaining = std::get_if<no::RemainingProjectionUnits>(&event.remaining_after);
    REQUIRE(remaining);
    return remaining->q;
}
double units_before(const no::ExecutionAppliedEvent& event) {
    const auto* remaining = std::get_if<no::RemainingProjectionUnits>(&event.remaining_before);
    REQUIRE(remaining);
    return remaining->q;
}
void check_point_budget(const no::ExecutionAppliedEvent& event, double filled) {
    const auto* before = std::get_if<no::AllowanceUnits>(&event.allowance_before);
    const auto* after = std::get_if<no::AllowanceUnits>(&event.allowance_after);
    REQUIRE(before && after);
    CHECK(before->point_ordinal == event.cursor.point.ordinal);
    CHECK(after->point_ordinal == before->point_ordinal);
    near(before->left, filled);
    near(after->left, 0);
    near(event.filled_working, filled);
}
void check_scope(const no::ExecutionAppliedEvent& event,
                 const no::RequestHandle& opening, int64_t cycle) {
    const auto* scope = std::get_if<execution::OpeningExposure>(&event.scope);
    REQUIRE(scope);
    CHECK(scope->incarnation == opening.incarnation);
    CHECK(scope->cycle == cycle);
}
int64_t opening_cycle(const Host& h, const no::RequestHandle& handle) {
    const auto f = fills(h, handle);
    REQUIRE(!f.empty());
    REQUIRE(f.front().opened_units != 0);
    REQUIRE(f.front().cycle_after > 0);
    return f.front().cycle_after;
}
const PyramidEntry* lot_of(const Host& h, const no::RequestHandle& handle) {
    const auto found = std::find_if(h.lots().begin(), h.lots().end(), [&](const auto& value) {
        return value.entry_incarnation == handle.incarnation;
    });
    return found == h.lots().end() ? nullptr : &*found;
}
void one_cancel(const Host& h, const no::RequestHandle& handle, no::CancelReason reason,
                uint64_t cause_ordinal) {
    const auto ce = cancellations(h, handle);
    REQUIRE(ce.size() == 1);
    CHECK(ce[0].reason == reason);
    REQUIRE(ce[0].cause);
    CHECK(ce[0].cause->ordinal == cause_ordinal);
    CHECK(ce[0].cause->run == handle.run);
    CHECK(fills(h, handle).empty());
}
void no_cancel(const Host& h, const no::RequestHandle& handle) {
    CHECK(cancellations(h, handle).empty());
}
bool wait_authority(const no::Authority& authority, const no::RequestHandle& parent) {
    const auto* wait = std::get_if<no::Wait>(&authority);
    return wait && wait->parent == parent;
}

void independent_reduce_budget(double sign) {
    Host h; start(h, sign > 0 ? "C3-long" : "C3-short");
    const auto seed = put(h, tx(sign * 5, "seed")); h.tick(0, 100);
    const auto cycle = opening_cycle(h, seed);
    auto closing = reduce(5, "C3"); closing.capacity = no::PointBudget{2};
    const auto handle = put(h, closing);
    h.tick(1, 100); h.tick(2, 100); h.tick(3, 100);
    const auto bound = events<no::CloseBoundEvent>(h, handle);
    REQUIRE(bound.size() == 1);
    CHECK(bound[0].cycle == cycle);
    CHECK(bound[0].side == (sign > 0 ? no::Side::Long : no::Side::Short));
    CHECK(std::holds_alternative<no::UnboundBookClose>(bound[0].before));
    REQUIRE(std::holds_alternative<no::BookClose>(bound[0].after));
    const auto f = fills(h, handle);
    REQUIRE(f.size() == 3);
    const double closed[] = {2, 2, 1};
    const double before[] = {5, 3, 1};
    const double after[] = {3, 1, 0};
    for (size_t i = 0; i < 3; ++i) {
        near(f[i].closed_units, closed[i]); CHECK(f[i].opened_units == 0);
        near(units_before(f[i]), before[i]); near(units_after(f[i]), after[i]);
        check_point_budget(f[i], closed[i]);
        CHECK(f[i].terminal == (i == 2));
        CHECK(std::holds_alternative<execution::Book>(f[i].scope));
        if (i < 2) {
            CHECK(f[i].cycle_before == cycle && f[i].cycle_after == cycle);
            CHECK(!f[i].terminal_reason);
        }
    }
    CHECK(f[0].cursor.point.ordinal != f[1].cursor.point.ordinal);
    CHECK(f[1].cursor.point.ordinal != f[2].cursor.point.ordinal);
    CHECK(bound[0].ordinal < f[0].ordinal);
    REQUIRE(f[2].terminal_reason);
    CHECK(*f[2].terminal_reason == no::AppliedTerminalReason::WorkingUnitsSatisfied);
    CHECK(f[2].cycle_before == cycle && f[2].cycle_after == 0);
    no_cancel(h, handle);
    CHECK(h.cancel(handle).status == no::CancelStatus::NotWorking);
    CHECK(cancellations(h, handle).empty());
    near(h.physical_position().signed_units, 0);
    REQUIRE(h.trade_count() == 3);
    near(h.get_trade(0).qty, 2); near(h.get_trade(1).qty, 2); near(h.get_trade(2).qty, 1);
    finish(h);
}

void target_exhausted_remainder(double sign) {
    Host h; start(h, sign > 0 ? "C4-long" : "C4-short");
    put(h, tx(sign * 2, "seed")); h.tick(0, 100);
    auto closing = reduce(5, "C4"); closing.capacity = no::PointBudget{2};
    const auto handle = put(h, closing);
    h.tick(1, 100);
    const auto f = fills(h, handle);
    REQUIRE(f.size() == 1);
    CHECK(f[0].terminal);
    REQUIRE(f[0].terminal_reason);
    CHECK(*f[0].terminal_reason == no::AppliedTerminalReason::TargetExhausted);
    near(f[0].closed_units, 2); CHECK(f[0].opened_units == 0);
    near(units_before(f[0]), 5); near(units_after(f[0]), 3);
    check_point_budget(f[0], 2);
    no_cancel(h, handle);
    CHECK(h.cancel(handle).status == no::CancelStatus::NotWorking);
    const auto later = put(h, tx(sign * 3, "later")); h.tick(2, 100);
    CHECK(fills(h, handle).size() == 1);
    REQUIRE(fills(h, later).size() == 1);
    near(h.physical_position().signed_units, sign * 3);
    REQUIRE(lot_of(h, later));
    near(lot_of(h, later)->qty, 3);
    finish(h);
}

void trail_canceled_by_reversal(double sign) {
    Host h; start(h, sign > 0 ? "C6-long" : "C6-short");
    put(h, tx(sign, "seed")); h.tick(0, 100);
    auto trail_req = reduce(1, "trail");
    trail_req.trigger = no::Trail{2, 100 + sign * 5};
    const auto trail = put(h, trail_req);
    h.tick(1, 100);
    CHECK(fills(h, trail).empty());
    CHECK(events<no::ActivatedEvent>(h, trail).empty());
    REQUIRE(events<no::CloseBoundEvent>(h, trail).size() == 1);
    h.tick(2, 100 + sign * 6);
    auto activated = events<no::ActivatedEvent>(h, trail);
    REQUIRE(activated.size() == 1);
    CHECK(activated[0].kind == no::ActivationKind::TrailArm);
    REQUIRE(std::holds_alternative<no::TrailTrack>(activated[0].after));
    near(std::get<no::TrailTrack>(activated[0].after).best, 100 + sign * 6);
    CHECK(fills(h, trail).empty());
    h.tick(3, 100 + sign * 10);
    CHECK(fills(h, trail).empty());
    CHECK(events<no::ActivatedEvent>(h, trail).size() == 1);
    const auto reverse = put(h, tx(-sign * 2, "reverse"));
    h.tick(4, 100 + sign * 10);
    REQUIRE(fills(h, reverse).size() == 1);
    CHECK(fills(h, trail).empty());
    CHECK(events<no::ActivatedEvent>(h, trail).size() == 1);
    one_cancel(h, trail, no::CancelReason::OwnerGone, fills(h, reverse)[0].ordinal);
    REQUIRE(std::holds_alternative<no::BookClose>(cancellations(h, trail)[0].prior_authority));
    near(h.physical_position().signed_units, -sign);
    h.tick(5, 100 + sign * 12);
    CHECK(fills(h, trail).empty());
    CHECK(cancellations(h, trail).size() == 1);
    CHECK(events<no::ActivatedEvent>(h, trail).size() == 1);
    near(h.physical_position().signed_units, -sign);
    CHECK(h.replace(trail, reduce(1, "dead-replace")).status == no::ReplaceStatus::NotWorking);
    auto fresh = reduce(1, "new-lifetime");
    fresh.trigger = no::Trail{2, std::nullopt};
    const auto replacement = put(h, fresh);
    CHECK(replacement.incarnation != trail.incarnation);
    h.tick(6, 100 + sign * 12);
    const auto rebound = events<no::CloseBoundEvent>(h, replacement);
    REQUIRE(rebound.size() == 1);
    CHECK(rebound[0].cycle == fills(h, reverse)[0].cycle_after);
    CHECK(rebound[0].side == (sign > 0 ? no::Side::Short : no::Side::Long));
    CHECK(cancellations(h, trail).size() == 1);
    CHECK(fills(h, trail).empty());
    finish(h);
}

void flat_unhit_independent_close(double sign) {
    Host h; start(h, sign > 0 ? "C7-long" : "C7-short");
    auto resting = reduce(1, "unhit");
    resting.trigger = no::Stop{100 - sign * 10};
    const auto handle = put(h, resting);
    h.tick(0, 100);
    CHECK(fills(h, handle).empty());
    CHECK(events<no::CloseBoundEvent>(h, handle).empty());
    CHECK(events<no::ActivatedEvent>(h, handle).empty());
    const auto none = events<no::NoEffectEvent>(h, handle);
    REQUIRE(none.size() == 1);
    CHECK(std::holds_alternative<no::UnboundBookClose>(none[0].authority));
    no_cancel(h, handle);
    CHECK(h.cancel(handle).status == no::CancelStatus::NotWorking);
    near(h.physical_position().signed_units, 0);
    finish(h);

    Host waiting; start(waiting, sign > 0 ? "C7-wait-long" : "C7-wait-short");
    auto parent_req = tx(sign, "unfilled-parent");
    parent_req.trigger = no::Stop{100 + sign * 50};
    const auto parent = put(waiting, parent_req);
    auto child_req = reduce(1, "still-wait");
    child_req.owner = no::WaitForApplied{parent};
    const auto child = put(waiting, child_req);
    waiting.tick(0, 100);
    CHECK(fills(waiting, parent).empty());
    CHECK(fills(waiting, child).empty());
    CHECK(events<no::NoEffectEvent>(waiting, child).empty());
    no_cancel(waiting, child);
    CHECK(waiting.cancel(child).status == no::CancelStatus::Cancelled);
    finish(waiting);
}

void cancel_cleanup_before_return(double sign) {
    Host h; start(h, sign > 0 ? "O2-cancel-long" : "O2-cancel-short");
    auto parent_req = tx(sign, "parent");
    parent_req.trigger = no::Stop{100 + sign * 50};
    const auto parent = put(h, parent_req);
    auto child_req = reduce(1, "child");
    child_req.owner = no::WaitForApplied{parent};
    const auto child = put(h, child_req);
    const auto cancelled = h.cancel(parent);
    REQUIRE(cancelled.status == no::CancelStatus::Cancelled);
    REQUIRE(cancelled.event_ordinal != 0);
    const auto parent_cancel = cancellations(h, parent);
    REQUIRE(parent_cancel.size() == 1);
    CHECK(parent_cancel[0].reason == no::CancelReason::User);
    CHECK(parent_cancel[0].ordinal == cancelled.event_ordinal);
    one_cancel(h, child, no::CancelReason::OwnerGone, cancelled.event_ordinal);
    REQUIRE(wait_authority(cancellations(h, child)[0].prior_authority, parent));
    CHECK(h.cancel(child).status == no::CancelStatus::NotWorking);
    CHECK(cancellations(h, child).size() == 1);
    const auto successor = put(h, tx(sign, "unrelated"));
    h.tick(0, 100);
    REQUIRE(fills(h, successor).size() == 1);
    CHECK(fills(h, child).empty());
    CHECK(cancellations(h, child).size() == 1);
    finish(h);
}

void replace_cleanup_before_return(double sign) {
    Host h; start(h, sign > 0 ? "O2-replace-long" : "O2-replace-short");
    auto parent_req = tx(sign, "parent");
    parent_req.trigger = no::Stop{100 + sign * 50};
    const auto parent = put(h, parent_req);
    auto child_req = reduce(1, "child");
    child_req.owner = no::WaitForApplied{parent};
    const auto child = put(h, child_req);
    const auto replaced = h.replace(parent, tx(sign, "successor"));
    REQUIRE(replaced.status == no::ReplaceStatus::Replaced);
    REQUIRE(replaced.successor);
    CHECK(replaced.successor->incarnation != parent.incarnation);
    const auto rows = events<no::ReplacedEvent>(h);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].predecessor() == parent);
    CHECK(rows[0].successor() == *replaced.successor);
    CHECK(rows[0].ordinal == replaced.event_ordinal);
    one_cancel(h, child, no::CancelReason::OwnerGone, replaced.event_ordinal);
    REQUIRE(wait_authority(cancellations(h, child)[0].prior_authority, parent));
    auto follow = reduce(1, "must-not-follow");
    follow.owner = no::WaitForApplied{parent};
    const auto refused = h.submit(follow);
    CHECK(refused.status == no::SubmitStatus::Rejected);
    CHECK(refused.reason == no::RequestRejectReason::InvalidOwner);
    h.tick(0, 100);
    REQUIRE(fills(h, *replaced.successor).size() == 1);
    CHECK(fills(h, child).empty());
    CHECK(cancellations(h, child).size() == 1);
    CHECK(events<no::ArmedEvent>(h, child).empty());
    finish(h);
}

void parent_match_rejected_cascade(double sign) {
    Host h;
    start(h, sign > 0 ? "O2-reject-long" : "O2-reject-short", 1, 0, 100, [&](NativeRunSpec& spec) {
        spec.allowed_open_directions = sign > 0 ? NativeOpenDirections::Short
                                                : NativeOpenDirections::Long;
    });
    const auto parent = put(h, tx(sign, "disallowed-open"));
    auto child_req = reduce(1, "child");
    child_req.owner = no::WaitForApplied{parent};
    const auto child = put(h, child_req);
    h.tick(0, 100);
    const auto rejected = events<no::MatchRejectedEvent>(h, parent);
    REQUIRE(rejected.size() == 1);
    CHECK(rejected[0].reason == no::MatchRejectReason::OpeningDirection);
    one_cancel(h, child, no::CancelReason::OwnerGone, rejected[0].ordinal);
    REQUIRE(wait_authority(cancellations(h, child)[0].prior_authority, parent));
    CHECK(fills(h, parent).empty());
    near(h.physical_position().signed_units, 0);
    finish(h);
}

void parent_no_effect_cascade(double sign) {
    Host h; start(h, sign > 0 ? "O2-noeffect-long" : "O2-noeffect-short");
    const auto parent = put(h, reduce(1, "flat-parent"));
    auto child_req = tx(sign, "child");
    child_req.owner = no::WaitForApplied{parent};
    const auto child = put(h, child_req);
    h.tick(0, 100);
    const auto none = events<no::NoEffectEvent>(h, parent);
    REQUIRE(none.size() == 1);
    CHECK(std::holds_alternative<no::UnboundBookClose>(none[0].authority));
    one_cancel(h, child, no::CancelReason::OwnerGone, none[0].ordinal);
    REQUIRE(wait_authority(cancellations(h, child)[0].prior_authority, parent));
    CHECK(events<no::ArmedEvent>(h, child).empty());
    CHECK(fills(h, child).empty());
    near(h.physical_position().signed_units, 0);
    finish(h);
}

void partial_parent_oneshot_flatten(double sign) {
    Host h; start(h, sign > 0 ? "O3-long" : "O3-short");
    const auto a = put(h, tx(sign, "A")); h.tick(0, 100);
    const auto cycle = opening_cycle(h, a);
    auto parent_req = tx(sign * 2, "parent"); parent_req.capacity = no::PointBudget{1};
    const auto parent = put(h, parent_req);
    auto child_req = flatten("oneshot"); child_req.owner = no::WaitForApplied{parent};
    const auto child = put(h, child_req);
    h.tick(1, 100);
    const auto pf = fills(h, parent), cf = fills(h, child);
    REQUIRE(pf.size() == 1 && cf.size() == 1);
    near(pf[0].opened_units, sign); CHECK(!pf[0].terminal);
    CHECK(pf[0].cycle_after == cycle);
    CHECK(cf[0].terminal);
    REQUIRE(cf[0].terminal_reason);
    CHECK(*cf[0].terminal_reason == no::AppliedTerminalReason::Flattened);
    check_scope(cf[0], parent, cycle);
    near(cf[0].closed_units, 1);
    no_cancel(h, child);
    const auto armed = events<no::ArmedEvent>(h, child);
    REQUIRE(armed.size() == 1);
    CHECK(armed[0].ordinal < cf[0].ordinal && pf[0].ordinal < armed[0].ordinal);
    REQUIRE(lot_of(h, a));
    near(lot_of(h, a)->qty, 1);
    CHECK(lot_of(h, parent) == nullptr);
    near(h.physical_position().signed_units, sign);
    h.tick(2, 100);
    const auto later = fills(h, parent);
    REQUIRE(later.size() == 2);
    CHECK(later[1].terminal);
    near(later[1].opened_units, sign);
    CHECK(later[1].cycle_after == cycle);
    CHECK(fills(h, child).size() == 1);
    no_cancel(h, child);
    REQUIRE(lot_of(h, a) && lot_of(h, parent));
    near(lot_of(h, a)->qty, 1); near(lot_of(h, parent)->qty, 1);
    near(h.physical_position().signed_units, sign * 2);
    finish(h);
}

void replacement_keeps_predecessor_scope(double sign) {
    Host h; start(h, sign > 0 ? "O4-long" : "O4-short");
    auto parent_req = tx(sign * 2, "parent"); parent_req.capacity = no::PointBudget{1};
    const auto parent = put(h, parent_req);
    auto child_req = reduce(1, "armed-child");
    child_req.owner = no::WaitForApplied{parent};
    child_req.trigger = no::Limit{100 + sign * 10};
    const auto child = put(h, child_req);
    h.tick(0, 100);
    const auto first = fills(h, parent);
    REQUIRE(first.size() == 1);
    CHECK(!first[0].terminal);
    const auto cycle = first[0].cycle_after;
    const auto armed = events<no::ArmedEvent>(h, child);
    REQUIRE(armed.size() == 1);
    const auto* opened = std::get_if<no::OpeningClose>(&armed[0].after);
    REQUIRE(opened);
    CHECK(opened->opening == parent && opened->cycle == cycle);
    CHECK(fills(h, child).empty());
    const auto replaced = h.replace(parent, tx(sign, "successor"));
    REQUIRE(replaced.status == no::ReplaceStatus::Replaced);
    REQUIRE(replaced.successor);
    no_cancel(h, child);
    h.tick(1, 100);
    REQUIRE(fills(h, *replaced.successor).size() == 1);
    CHECK(fills(h, *replaced.successor)[0].cycle_after == cycle);
    CHECK(fills(h, child).empty());
    REQUIRE(lot_of(h, parent) && lot_of(h, *replaced.successor));
    h.tick(2, 100 + sign * 10);
    const auto cf = fills(h, child);
    REQUIRE(cf.size() == 1);
    CHECK(cf[0].terminal);
    near(cf[0].closed_units, 1);
    check_scope(cf[0], parent, cycle);
    CHECK(lot_of(h, parent) == nullptr);
    REQUIRE(lot_of(h, *replaced.successor));
    near(lot_of(h, *replaced.successor)->qty, 1);
    no_cancel(h, child);
    CHECK(h.cancel(child).status == no::CancelStatus::NotWorking);
    near(h.physical_position().signed_units, sign);
    finish(h);
}

void close_only_parent_arms_transaction(double sign) {
    Host h; start(h, sign > 0 ? "O5-long" : "O5-short");
    put(h, tx(sign, "seed")); h.tick(0, 100);
    const auto parent = put(h, flatten("close-only"));
    auto reduce_req = reduce(1, "closing-child");
    reduce_req.owner = no::WaitForApplied{parent};
    const auto closing = put(h, reduce_req);
    auto tx_req = tx(sign, "transaction-child");
    tx_req.owner = no::WaitForApplied{parent};
    const auto opening = put(h, tx_req);
    h.tick(1, 100);
    const auto pf = fills(h, parent);
    REQUIRE(pf.size() == 1);
    CHECK(pf[0].terminal && pf[0].opened_units == 0);
    REQUIRE(pf[0].terminal_reason);
    CHECK(*pf[0].terminal_reason == no::AppliedTerminalReason::Flattened);
    one_cancel(h, closing, no::CancelReason::UnsupportedRelation, pf[0].ordinal);
    REQUIRE(wait_authority(cancellations(h, closing)[0].prior_authority, parent));
    const auto armed = events<no::ArmedEvent>(h, opening);
    REQUIRE(armed.size() == 1);
    REQUIRE(std::holds_alternative<no::ArmedTransaction>(armed[0].after));
    const auto& after = std::get<no::ArmedTransaction>(armed[0].after);
    CHECK(after.parent == parent && after.cause.ordinal == pf[0].ordinal);
    const auto of = fills(h, opening);
    REQUIRE(of.size() == 1);
    CHECK(of[0].ordinal > armed[0].ordinal);
    near(of[0].opened_units, sign);
    CHECK(std::holds_alternative<execution::Book>(of[0].scope));
    no_cancel(h, opening);
    CHECK(fills(h, closing).empty());
    near(h.physical_position().signed_units, sign);
    finish(h);
}

void late_bind_rejects_event_size(double sign) {
    Host h; start(h, sign > 0 ? "O7-long" : "O7-short");
    auto parent_req = tx(sign * 4, "parent"); parent_req.capacity = no::PointBudget{2};
    const auto parent = put(h, parent_req);
    h.tick(0, 100); h.tick(1, 100);
    const auto pf = fills(h, parent);
    REQUIRE(pf.size() == 2);
    CHECK(pf[1].terminal);
    const auto cycle = pf[0].cycle_after;
    CHECK(pf[1].cycle_after == cycle);
    put(h, reduce(1, "trim")); h.tick(2, 100);
    near(h.physical_position().signed_units, sign * 3);
    auto bad = event_reduce("late-event");
    bad.owner = no::BindOpening{parent, cycle};
    const auto refused = h.submit(bad);
    CHECK(refused.status == no::SubmitStatus::Rejected && !refused.handle);
    CHECK(refused.reason == no::RequestRejectReason::InvalidQuantityBasis);
    near(h.physical_position().signed_units, sign * 3);
    auto ok = reduce(1, "explicit-bind");
    ok.owner = no::BindOpening{parent, cycle};
    const auto child = put(h, ok);
    h.tick(3, 100);
    const auto cf = fills(h, child);
    REQUIRE(cf.size() == 1);
    CHECK(cf[0].terminal);
    near(cf[0].closed_units, 1);
    check_scope(cf[0], parent, cycle);
    no_cancel(h, child);
    near(h.physical_position().signed_units, sign * 2);
    finish(h);
}

void event_child_never_resizes(double sign) {
    Host h; start(h, sign > 0 ? "O8-long" : "O8-short");
    put(h, tx(-sign * 3, "opposite")); h.tick(0, 100);
    auto parent_req = tx(sign * 10, "parent"); parent_req.capacity = no::PointBudget{5};
    const auto parent = put(h, parent_req);
    auto child_req = event_reduce("frozen");
    child_req.owner = no::WaitForApplied{parent};
    child_req.trigger = no::Limit{100 + sign * 10};
    const auto child = put(h, child_req);
    h.tick(1, 100);
    const auto first = fills(h, parent);
    REQUIRE(first.size() == 1);
    near(first[0].closed_units, 3); near(first[0].opened_units, sign * 2);
    CHECK(!first[0].terminal);
    const auto bound = events<no::QuantityBoundEvent>(h, child);
    REQUIRE(bound.size() == 1);
    CHECK(bound[0].source.ordinal == first[0].ordinal);
    near(bound[0].source_units, 2);
    near(bound[0].pending_total, 0);
    REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(bound[0].remaining));
    near(std::get<no::RemainingProjectionUnits>(bound[0].remaining).q, 2);
    CHECK(fills(h, child).empty());
    const auto cycle = first[0].cycle_after;
    h.tick(2, 100);
    const auto second = fills(h, parent);
    REQUIRE(second.size() == 2);
    CHECK(second[1].terminal);
    near(second[1].opened_units, sign * 5);
    CHECK(second[1].cycle_after == cycle);
    CHECK(events<no::QuantityBoundEvent>(h, child).size() == 1);
    CHECK(fills(h, child).empty());
    h.tick(3, 100 + sign * 10);
    const auto cf = fills(h, child);
    REQUIRE(cf.size() == 1);
    CHECK(cf[0].terminal);
    near(cf[0].closed_units, 2);
    near(units_before(cf[0]), 2); near(units_after(cf[0]), 0);
    REQUIRE(cf[0].terminal_reason);
    CHECK(*cf[0].terminal_reason == no::AppliedTerminalReason::WorkingUnitsSatisfied);
    check_scope(cf[0], parent, cycle);
    no_cancel(h, child);
    CHECK(events<no::QuantityBoundEvent>(h, child).size() == 1);
    REQUIRE(lot_of(h, parent));
    near(lot_of(h, parent)->qty, 5);
    near(h.physical_position().signed_units, sign * 5);
    finish(h);
}

void sibling_keeps_cycle_no_revival(double sign) {
    Host h; start(h, sign > 0 ? "O9-long" : "O9-short");
    const auto a = put(h, tx(sign, "A")); h.tick(0, 100);
    const auto cycle = opening_cycle(h, a);
    auto parent_req = tx(sign * 2, "parent"); parent_req.capacity = no::PointBudget{1};
    const auto parent = put(h, parent_req);
    auto resting_req = reduce(1, "resting");
    resting_req.owner = no::WaitForApplied{parent};
    resting_req.trigger = no::Limit{100 + sign * 10};
    const auto resting = put(h, resting_req);
    auto flat_req = flatten("prearmed"); flat_req.owner = no::WaitForApplied{parent};
    const auto closer = put(h, flat_req);
    h.tick(1, 100);
    const auto pf = fills(h, parent), ff = fills(h, closer);
    REQUIRE(pf.size() == 1 && ff.size() == 1);
    CHECK(!pf[0].terminal);
    CHECK(pf[0].cycle_after == cycle);
    CHECK(ff[0].terminal);
    REQUIRE(ff[0].terminal_reason);
    CHECK(*ff[0].terminal_reason == no::AppliedTerminalReason::Flattened);
    check_scope(ff[0], parent, cycle);
    no_cancel(h, closer);
    one_cancel(h, resting, no::CancelReason::OwnerGone, ff[0].ordinal);
    const auto armed_resting = events<no::ArmedEvent>(h, resting);
    REQUIRE(armed_resting.size() == 1);
    const auto* armed_close = std::get_if<no::OpeningClose>(&armed_resting[0].after);
    REQUIRE(armed_close);
    CHECK(armed_close->opening == parent);
    CHECK(armed_close->cycle == cycle);
    const auto resting_cancel = cancellations(h, resting);
    REQUIRE(resting_cancel.size() == 1);
    const auto* prior = std::get_if<no::OpeningClose>(&resting_cancel[0].prior_authority);
    REQUIRE(prior);
    CHECK(prior->opening == parent);
    CHECK(prior->cycle == cycle);
    CHECK(resting_cancel[0].ordinal > ff[0].ordinal);
    REQUIRE(lot_of(h, a));
    CHECK(lot_of(h, parent) == nullptr);
    near(h.physical_position().signed_units, sign);
    h.tick(2, 100);
    const auto later = fills(h, parent);
    REQUIRE(later.size() == 2);
    CHECK(later[1].terminal);
    near(later[1].opened_units, sign);
    CHECK(later[1].cycle_after == cycle);
    CHECK(later[1].definition->handle.incarnation == parent.incarnation);
    CHECK(fills(h, resting).empty());
    CHECK(cancellations(h, resting).size() == 1);
    CHECK(events<no::ArmedEvent>(h, resting).size() == 1);
    CHECK(h.cancel(resting).status == no::CancelStatus::NotWorking);
    REQUIRE(lot_of(h, a) && lot_of(h, parent));
    near(lot_of(h, a)->qty, 1); near(lot_of(h, parent)->qty, 1);
    near(h.physical_position().signed_units, sign * 2);
    h.tick(3, 100 + sign * 10);
    CHECK(fills(h, resting).empty());
    CHECK(cancellations(h, resting).size() == 1);
    finish(h);
}

void run_case(const char* name, const std::function<void()>& body) {
    scenario = name; ++cases;
    const int previous = failures;
    try { body(); }
    catch (const StopCase&) {}
    catch (const std::exception& e) {
        std::printf("FAIL %s unexpected exception: %s\n", name, e.what()); ++failures;
    }
    catch (...) {
        std::printf("FAIL %s unexpected nonstandard exception\n", name); ++failures;
    }
    std::printf("%s %s\n", failures == previous ? "PASS" : "FAIL", name);
}
}  // namespace

int main() {
    for (double sign : {1.0, -1.0}) {
        run_case(sign > 0 ? "C3 long Reduce5 budget2 across points"
                          : "C3 short Reduce5 budget2 across points",
                 [&] { independent_reduce_budget(sign); });
        run_case(sign > 0 ? "C4 long target-exhausted remainder"
                          : "C4 short target-exhausted remainder",
                 [&] { target_exhausted_remainder(sign); });
        run_case(sign > 0 ? "C6 long trail canceled by reversal"
                          : "C6 short trail canceled by reversal",
                 [&] { trail_canceled_by_reversal(sign); });
        run_case(sign > 0 ? "C7 long flat unhit independent close"
                          : "C7 short flat unhit independent close",
                 [&] { flat_unhit_independent_close(sign); });
        run_case(sign > 0 ? "O2 long cancel cleanup before return"
                          : "O2 short cancel cleanup before return",
                 [&] { cancel_cleanup_before_return(sign); });
        run_case(sign > 0 ? "O2 long replace cleanup before return"
                          : "O2 short replace cleanup before return",
                 [&] { replace_cleanup_before_return(sign); });
        run_case(sign > 0 ? "O2 long parent MatchRejected cascade"
                          : "O2 short parent MatchRejected cascade",
                 [&] { parent_match_rejected_cascade(sign); });
        run_case(sign > 0 ? "O2 long parent NoEffect cascade"
                          : "O2 short parent NoEffect cascade",
                 [&] { parent_no_effect_cascade(sign); });
        run_case(sign > 0 ? "O3 long partial parent oneshot Flatten"
                          : "O3 short partial parent oneshot Flatten",
                 [&] { partial_parent_oneshot_flatten(sign); });
        run_case(sign > 0 ? "O4 long predecessor scope after replace"
                          : "O4 short predecessor scope after replace",
                 [&] { replacement_keeps_predecessor_scope(sign); });
        run_case(sign > 0 ? "O5 long close-only parent arms transaction"
                          : "O5 short close-only parent arms transaction",
                 [&] { close_only_parent_arms_transaction(sign); });
        run_case(sign > 0 ? "O7 long late Bind OwnerOpenedUnits"
                          : "O7 short late Bind OwnerOpenedUnits",
                 [&] { late_bind_rejects_event_size(sign); });
        run_case(sign > 0 ? "O8 long frozen opening2 vs later5"
                          : "O8 short frozen opening2 vs later5",
                 [&] { event_child_never_resizes(sign); });
        run_case(sign > 0 ? "O9 long sibling cycle no revival"
                          : "O9 short sibling cycle no revival",
                 [&] { sibling_keeps_cycle_no_revival(sign); });
    }
    std::printf("%s native resting owner contract: %d cases, %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", cases, checks, failures);
    return failures ? 1 : 0;
}
