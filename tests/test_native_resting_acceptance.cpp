// Bounded R2 v5 acceptance: real native commands, drivers, book and observations.
// No direct settlement, injected lots, synthetic committed facts or source commands.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
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
    // Read-only physical observations; setup always uses the native producer.
    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
};

NativeRunSpec configuration(const char* key, uint64_t run = 1, double fee = 0) {
    NativeRunSpec s;
    s.identity = {key, run}; s.input_tf = "1"; s.script_tf = "1";
    s.ticker = "N"; s.tickerid = "TEST:N"; s.type = "crypto";
    s.currency = "USD"; s.basecurrency = "USD"; s.description = "R2 acceptance";
    s.volumetype = "base"; s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 10000; s.point_value = 1; s.account_fx = 1;
    s.price_tick = .01; s.fee_kind = NativeFeeKind::CashPerExecution; s.fee_value = fee;
    return s;
}

void start(Host& h, const char* key, uint64_t run = 1, double fee = 0,
           double first_price = 100) {
    REQUIRE(h.configure_native(configuration(key, run, fee)).status == NativeSetupStatus::Applied);
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
std::vector<no::ExecutionAppliedEvent> fills(const Host& h, const no::RequestHandle& handle) {
    std::vector<no::ExecutionAppliedEvent> result;
    for (const auto& event : events<no::ExecutionAppliedEvent>(h)) {
        REQUIRE(event.definition);
        if (event.definition->handle == handle) result.push_back(event);
    }
    return result;
}
const PyramidEntry& physical_lot(const Host& h, const no::RequestHandle& handle) {
    const auto found = std::find_if(h.lots().begin(), h.lots().end(), [&](const auto& value) {
        return value.entry_incarnation == handle.incarnation;
    });
    REQUIRE(found != h.lots().end());
    return *found;
}
int64_t opening_cycle(const Host& h, const no::RequestHandle& handle) {
    const auto f = fills(h, handle);
    REQUIRE(!f.empty());
    REQUIRE(f.front().opened_units != 0);
    REQUIRE(f.front().cycle_after > 0);
    return f.front().cycle_after;
}
void check_scope(const no::ExecutionAppliedEvent& event,
                 const no::RequestHandle& opening, int64_t cycle) {
    const auto* scope = std::get_if<execution::OpeningExposure>(&event.scope);
    REQUIRE(scope);
    CHECK(scope->incarnation == opening.incarnation);
    CHECK(scope->cycle == cycle);
}

void scoped_cost_and_terminal_enrollment(double sign) {
    Host h;
    start(h, sign > 0 ? "S1-O6-long" : "S1-O6-short", 1, 6);
    // A3/cost6 -> reduce A1 -> A2/cost4. Then B3/cost6, behind A.
    const auto a = put(h, tx(sign * 3, "A")); h.tick(0, 100);
    put(h, reduce(1, "prepare-A2")); h.tick(1, 100);
    near(physical_lot(h, a).qty, 2); near(physical_lot(h, a).entry_commission_account, 4);
    const auto b = put(h, tx(sign * 3, "B")); h.tick(2, 100 + sign * 10);
    const auto cycle = opening_cycle(h, b);
    CHECK(cycle == opening_cycle(h, a));
    REQUIRE(h.lots().size() == 2);
    const PyramidEntry a_before = physical_lot(h, a);

    auto wait = reduce(1, "late-Wait"); wait.owner = no::WaitForApplied{b};
    const auto refused = h.submit(wait);
    CHECK(refused.status == no::SubmitStatus::Rejected && !refused.handle);
    CHECK(refused.reason == no::RequestRejectReason::InvalidOwner);
    auto bound = reduce(1, "B-only"); bound.owner = no::BindOpening{b, cycle};
    const auto child = put(h, bound);
    const int first_row = h.trade_count();
    h.tick(3, 100 + sign * 20);
    const auto f = fills(h, child);
    REQUIRE(f.size() == 1); check_scope(f.front(), b, cycle);
    CHECK(f.front().closed_units == 1 && f.front().opened_units == 0 && f.front().terminal);
    CHECK(f.front().current_ticket == 6 && f.front().closed_trade_count == 1);
    CHECK(f.front().first_trade_index == static_cast<size_t>(first_row));
    REQUIRE(h.trade_count() == first_row + 1);
    const Trade& row = h.get_trade(first_row);
    CHECK(row.entry_incarnation == b.incarnation && row.is_long == (sign > 0));
    near(row.entry_price, 100 + sign * 10); near(row.exit_price, 100 + sign * 20);
    near(row.qty, 1); near(row.commission, 8); near(row.pnl, 2);
    near(physical_lot(h, b).qty, 2); near(physical_lot(h, b).entry_commission_account, 4);
    const auto& a_after = physical_lot(h, a);
    CHECK(a_after.entry_incarnation == a_before.entry_incarnation);
    CHECK(a_after.entry_id == a_before.entry_id && a_after.time == a_before.time);
    CHECK(a_after.price == a_before.price && a_after.qty == a_before.qty);
    CHECK(a_after.entry_commission_account == a_before.entry_commission_account);
    near(h.physical_position().signed_units, sign * 4);
    near(h.physical_position().average_price, 100 + sign * 5);
    finish(h);
}

void foreign_run_bind(double sign) {
    Host h;
    const char* key = sign > 0 ? "V2-long" : "V2-short";
    start(h, key, 1);
    const auto saved = put(h, tx(sign * 2)); h.tick(0, 100);
    const auto saved_cycle = opening_cycle(h, saved);
    finish(h);
    start(h, key, 2);
    const auto current = put(h, tx(sign * 2)); h.tick(0, 100);
    const auto cycle = opening_cycle(h, current);
    REQUIRE(saved.incarnation == current.incarnation && saved_cycle == cycle);
    CHECK(saved.run.run_number == 1 && current.run.run_number == 2);
    auto bad = reduce(1); bad.owner = no::BindOpening{saved, cycle};
    const auto refusal = h.submit(bad);
    CHECK(refusal.status == no::SubmitStatus::Rejected && !refusal.handle);
    CHECK(refusal.reason == no::RequestRejectReason::InvalidOwner);
    near(h.physical_position().signed_units, sign * 2);
    CHECK(h.trade_count() == 0);
    bad.owner = no::BindOpening{current, cycle};
    const auto child = put(h, bad); h.tick(1, 100);
    const auto f = fills(h, child); REQUIRE(f.size() == 1);
    check_scope(f.front(), current, cycle);
    near(h.physical_position().signed_units, sign); finish(h);
}

void owner_remaining_ohlc(double sign) {
    Host h; no::RequestHandle a, parent, child;
    h.beginning = [&](Host& self) { a = put(self, tx(sign * 2, "A")); };
    h.calculation = [&](Host& self) {
        if (self.calculations != 1) return;
        parent = put(self, tx(sign, "B-parent"));
        auto r = reduce(1, "B-stop"); r.owner = no::WaitForApplied{parent};
        r.trigger = no::Stop{100 - sign}; child = put(self, r);
    };
    REQUIRE(h.configure_native(configuration(sign > 0 ? "O1-long" : "O1-short")).status
            == NativeSetupStatus::Applied);
    const Bar tape[] = {{100,100,100,100,1,T},
        sign > 0 ? Bar{100,105,90,100,1,T+60000} : Bar{100,110,95,100,1,T+60000}};
    h.run(tape, 2);
    REQUIRE(h.native_state().kind == NativeLifecycleKind::Completed);
    const auto pf = fills(h, parent), cf = fills(h, child);
    REQUIRE(pf.size() == 1 && cf.size() == 1);
    CHECK(child.incarnation > parent.incarnation);
    CHECK(pf[0].cycle_after == opening_cycle(h, a));
    check_scope(cf[0], parent, pf[0].cycle_after);
    CHECK(cf[0].ordinal > pf[0].ordinal);
    CHECK(cf[0].cursor.point.ordinal > pf[0].cursor.point.ordinal);
    CHECK(cf[0].cursor.point.path_phase == (sign > 0 ? NativePathPhase::Low : NativePathPhase::High));
    CHECK(cf[0].cursor.point.provenance == NativePriceProvenance::Confirmed);
    near(cf[0].cursor.t, .4); near(cf[0].raw_price, 100 - sign);
    near(cf[0].closed_units, 1); CHECK(cf[0].terminal);
    REQUIRE(h.trade_count() == 1);
    CHECK(h.get_trade(0).entry_incarnation == parent.incarnation);
    near(h.get_trade(0).pnl, -1);
    REQUIRE(h.lots().size() == 1);
    CHECK(h.lots()[0].entry_incarnation == a.incarnation);
    near(h.physical_position().signed_units, sign * 2);
}

void chronological_crossings(double sign, bool unrelated) {
    Host h; no::RequestHandle later, earlier, other;
    h.beginning = [&](Host& self) {
        if (unrelated) {
            auto idle = tx(sign); idle.trigger = no::Limit{sign > 0 ? 80.0 : 120.0};
            other = put(self, idle);
        }
        auto late = tx(sign, "lower-incarnation-later-hit"); late.trigger = no::Stop{100 + sign * 4};
        later = put(self, late);
        auto early = tx(sign, "higher-incarnation-earlier-hit"); early.trigger = no::Stop{100 + sign * 2};
        earlier = put(self, early);
    };
    REQUIRE(h.configure_native(configuration(sign > 0 ? "chronology-long" : "chronology-short")).status
            == NativeSetupStatus::Applied);
    const Bar bar = sign > 0 ? Bar{100,106,90,100,1,T} : Bar{100,110,94,100,1,T};
    h.run(&bar, 1); REQUIRE(h.native_state().kind == NativeLifecycleKind::Completed);
    const auto first = fills(h, earlier), second = fills(h, later);
    REQUIRE(first.size() == 1 && second.size() == 1);
    CHECK(later.incarnation < earlier.incarnation && first[0].ordinal < second[0].ordinal);
    CHECK(first[0].cursor.point.ordinal == second[0].cursor.point.ordinal);
    CHECK(first[0].cursor.point.path_phase == (sign > 0 ? NativePathPhase::High : NativePathPhase::Low));
    CHECK(first[0].cursor.point.effective_time_ms == second[0].cursor.point.effective_time_ms);
    CHECK(first[0].cursor.point.provenance == NativePriceProvenance::Confirmed);
    near(first[0].cursor.t, 1.0/3.0); near(second[0].cursor.t, 2.0/3.0);
    near(first[0].raw_price, 100 + sign * 2); near(second[0].raw_price, 100 + sign * 4);
    near(h.physical_position().signed_units, sign * 2);
    near(h.physical_position().average_price, 100 + sign * 3);
    if (unrelated) CHECK(fills(h, other).empty());
}

void flat_incarnation_order(double sign, bool transaction_first) {
    Host h; no::RequestHandle opening, closing;
    h.beginning = [&](Host& self) {
        if (transaction_first) { opening = put(self, tx(sign)); closing = put(self, reduce(1)); }
        else { closing = put(self, reduce(1)); opening = put(self, tx(sign)); }
    };
    REQUIRE(h.configure_native(configuration(sign > 0 ? "C9-long" : "C9-short")).status
            == NativeSetupStatus::Applied);
    const Bar bar{100,100,100,100,1,T}; h.run(&bar,1);
    REQUIRE(h.native_state().kind == NativeLifecycleKind::Completed);
    const auto opened = fills(h, opening), closed = fills(h, closing);
    REQUIRE(opened.size() == 1);
    const auto no_effect = events<no::NoEffectEvent>(h);
    if (transaction_first) {
        REQUIRE(closed.size() == 1); CHECK(no_effect.empty());
        CHECK(opened[0].ordinal < closed[0].ordinal);
        CHECK(opened[0].cursor.point.ordinal == closed[0].cursor.point.ordinal);
        CHECK(closed[0].closed_units == 1 && closed[0].terminal);
        CHECK(h.trade_count() == 1); near(h.physical_position().signed_units,0);
    } else {
        CHECK(closed.empty()); REQUIRE(no_effect.size() == 1);
        REQUIRE(no_effect[0].definition); CHECK(no_effect[0].definition->handle == closing);
        CHECK(no_effect[0].ordinal < opened[0].ordinal);
        CHECK(h.trade_count() == 0); near(h.physical_position().signed_units,sign);
    }
}

void failed_is_permanent(Host& h, int64_t next_offset, double price) {
    REQUIRE(h.native_state().kind == NativeLifecycleKind::Failed);
    const auto before = h.native_state().failure;
    const auto hash = h.native_continuation_hash();
    const auto count = h.native_events(0).size();
    const auto position = h.physical_position();
    CHECK(!h.input(next_offset, price));
    CHECK(!h.stream_advance_time(T + next_offset + 100));
    CHECK(!h.stream_end(false));
    bool threw = false;
    try { (void)h.submit(tx(1)); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    const auto after = h.native_state();
    CHECK(after.kind == NativeLifecycleKind::Failed);
    CHECK(after.failure.code == before.code && after.failure.operation == before.operation);
    CHECK(after.failure.ordinal == before.ordinal && after.failure.discriminator == before.discriminator);
    CHECK(after.failure.context.kind == before.context.kind);
    CHECK(after.failure.context.cause.ordinal == before.context.cause.ordinal);
    CHECK(after.failure.context.recipient.incarnation == before.context.recipient.incarnation);
    CHECK(h.native_events(0).size() == count);
    CHECK(h.native_continuation_hash() == hash);
    CHECK(h.physical_position().signed_units == position.signed_units);
    CHECK(h.physical_position().lot_count == position.lot_count);
}

void capacity_progress_failure(double sign) {
    Host h; start(h, sign > 0 ? "C8-long" : "C8-short");
    auto r = tx(sign * 0x1p60); r.capacity = no::PointBudget{1};
    const auto handle = put(h,r);
    CHECK(!h.input(0,100)); REQUIRE(h.native_state().kind == NativeLifecycleKind::Failed);
    const auto failure = h.native_state().failure;
    CHECK(failure.code == NativeFailureCode::SettlementFailure);
    CHECK(failure.operation == NativeFailureOperation::Settlement);
    CHECK(failure.discriminator == static_cast<uint32_t>(no::CoreFailure::NonrepresentableQuantity));
    CHECK(native_failure_has_recipient(failure.context));
    CHECK(failure.context.recipient.incarnation == handle.incarnation);
    CHECK(fills(h,handle).empty()); CHECK(events<no::ExecutionAppliedEvent>(h).empty());
    CHECK(h.trade_count() == 0); near(h.physical_position().signed_units,0);
    near(h.native_marked_equity(100),10000); failed_is_permanent(h,1,100);
}

void check_group_failure(Host& h, const no::RequestHandle& filler,
                         const no::RequestHandle& recipient) {
    REQUIRE(h.native_state().kind == NativeLifecycleKind::Failed);
    const auto f = fills(h,filler); REQUIRE(f.size() == 1);
    CHECK(f[0].terminal);
    const auto failure = h.native_state().failure;
    CHECK(failure.code == NativeFailureCode::SettlementFailure);
    CHECK(failure.operation == NativeFailureOperation::Settlement);
    CHECK(failure.discriminator == static_cast<uint32_t>(no::CoreFailure::UnrepresentableReservation));
    CHECK(native_failure_has_cause(failure.context) && native_failure_has_recipient(failure.context));
    CHECK(failure.context.cause.ordinal == f[0].ordinal);
    CHECK(failure.context.recipient.incarnation == recipient.incarnation);
    CHECK(fills(h,recipient).empty());
    bool account_seen = false;
    for (const auto& event : h.native_events(0)) {
        if (event.account && event.account->ordinal == f[0].ordinal) account_seen = true;
    }
    CHECK(account_seen);
}

void group_subtraction_failure(double sign) {
    Host h; start(h,sign > 0 ? "G5-subtract-long" : "G5-subtract-short",1,2);
    auto waiting = tx(sign * 0x1p60,"large-recipient");
    waiting.trigger = no::Limit{sign > 0 ? 1.0 : 200.0};
    waiting.group = no::Member{7,2,no::GroupEffect::Cancel};
    const auto recipient = put(h,waiting);
    auto emit = tx(sign,"committed-filler"); emit.group = no::Member{7,1,no::GroupEffect::Reduce};
    const auto filler = put(h,emit);
    CHECK(!h.input(0,100)); check_group_failure(h,filler,recipient);
    REQUIRE(fills(h,filler).size() == 1);
    CHECK(fills(h,filler)[0].filled_working == 1 && fills(h,filler)[0].current_ticket == 2);
    CHECK(events<no::ReservationReducedEvent>(h).empty());
    CHECK(events<no::CancelledEvent>(h).empty());
    near(h.physical_position().signed_units,sign); near(h.native_marked_equity(100),9998);
    failed_is_permanent(h,1,100);
}

void pending_addition_failure(bool overflow) {
    Host h;
    const double first = overflow ? std::numeric_limits<double>::max() : 0x1p60;
    const double second = overflow ? std::numeric_limits<double>::max() : 1.0;
    const double price = overflow ? .01 : 100.0;
    start(h,overflow ? "G5-pending-overflow" : "G5-pending-absorption",1,0,price);
    auto parent_request = tx(1,"unfilled-owner"); parent_request.trigger = no::Stop{price*2};
    const auto parent = put(h,parent_request);
    no::Request child{no::Reduce{no::OwnerOpenedUnits{}},"unbound-recipient",""};
    child.owner = no::WaitForApplied{parent}; child.group = no::Member{7,2,no::GroupEffect::Reduce};
    const auto recipient = put(h,child);
    auto emit = tx(first,"first-delta"); emit.group = no::Member{7,1,no::GroupEffect::Reduce};
    const auto first_filler = put(h,emit); h.tick(0,price);
    auto prior = events<no::DeferredGroupAdjustmentEvent>(h); REQUIRE(prior.size() == 1);
    CHECK(prior[0].recipient == recipient);
    REQUIRE(fills(h,first_filler).size() == 1);
    CHECK(prior[0].cause.ordinal == fills(h,first_filler)[0].ordinal);
    // Remove the first physical exposure without emitting a group effect.
    // The still-Wait recipient retains its first deferred adjustment.
    put(h,no::Request{execution::Flatten{},"clear-book",""}); h.tick(1,price);
    near(h.physical_position().signed_units,0);
    emit = tx(second,"second-delta"); emit.group = no::Member{7,1,no::GroupEffect::Reduce};
    const auto second_filler = put(h,emit);
    CHECK(!h.input(2,price)); check_group_failure(h,second_filler,recipient);
    const auto after = events<no::DeferredGroupAdjustmentEvent>(h); REQUIRE(after.size() == 1);
    CHECK(after[0].ordinal == prior[0].ordinal && after[0].cause == prior[0].cause);
    CHECK(events<no::QuantityBoundEvent>(h).empty());
    CHECK(events<no::ReservationReducedEvent>(h).empty());
    CHECK(events<no::CancelledEvent>(h).empty());
    CHECK(fills(h,parent).empty());
    CHECK(h.physical_position().signed_units == second);
    CHECK(fills(h,second_filler)[0].filled_working == second);
    failed_is_permanent(h,3,price);
}

void run_case(const char* name, const std::function<void()>& body) {
    scenario = name; ++cases;
    const int previous = failures;
    try { body(); }
    catch (const StopCase&) {}
    catch (const std::exception& e) { std::printf("FAIL %s unexpected exception: %s\n",name,e.what()); ++failures; }
    catch (...) { std::printf("FAIL %s unexpected nonstandard exception\n",name); ++failures; }
    std::printf("%s %s\n", failures == previous ? "PASS" : "FAIL",name);
}
} // namespace

int main() {
    for (double sign : {1.0,-1.0}) {
        run_case(sign>0?"S1/O6 long scoped costs":"S1/O6 short scoped costs",[&]{scoped_cost_and_terminal_enrollment(sign);});
        run_case(sign>0?"V2 long full run identity":"V2 short full run identity",[&]{foreign_run_bind(sign);});
        run_case(sign>0?"O1 long remaining OHLC":"O1 short remaining OHLC",[&]{owner_remaining_ohlc(sign);});
        run_case(sign>0?"OHLC long chronological competitors":"OHLC short chronological competitors",[&]{chronological_crossings(sign,false);});
        run_case(sign>0?"OHLC long unrelated insertion":"OHLC short unrelated insertion",[&]{chronological_crossings(sign,true);});
        run_case(sign>0?"C9 long transaction first":"C9 short transaction first",[&]{flat_incarnation_order(sign,true);});
        run_case(sign>0?"C9 long close first":"C9 short close first",[&]{flat_incarnation_order(sign,false);});
        run_case(sign>0?"C8 long precommit arithmetic failure":"C8 short precommit arithmetic failure",[&]{capacity_progress_failure(sign);});
        run_case(sign>0?"G5 long postcommit subtraction failure":"G5 short postcommit subtraction failure",[&]{group_subtraction_failure(sign);});
    }
    run_case("G5 pending 2^60 plus 1",[]{pending_addition_failure(false);});
    run_case("G5 pending DBL_MAX plus DBL_MAX",[]{pending_addition_failure(true);});
    std::printf("%s native resting acceptance: %d cases, %d checks, %d failures\n",
                failures?"FAIL":"PASS",cases,checks,failures);
    return failures?1:0;
}
