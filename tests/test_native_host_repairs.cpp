#include <pineforge/pineforge.h>
#include <pineforge/market_driver.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <variant>
#include <vector>

#ifndef PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18
#error "native strategy host must fail closed against an unversioned epoch"
#endif

using namespace pineforge;
using pineforge::native_order::Request;
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

void near(double a, double b) {
    const bool equal = std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
    if (!equal) std::printf("actual=%.17g expected=%.17g\n", a, b);
    CHECK(equal);
}

double bits_to_double(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

NativeRunSpec spec_for(const std::string& key, uint64_t run) {
    NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = run;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "X";
    spec.tickerid = "EXCHANGE:X";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 6;
    return spec;
}

Bar bar_at(int64_t open_ms, double o, double h, double l, double c) {
    return Bar{o, h, l, c, 1.0, open_ms};
}

class EmptyHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++callbacks; }
};

class ThrowHost final : public NativeStrategyHost {
public:
    bool throw_next = true;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (throw_next) throw std::runtime_error("callback boom");
    }
};

class CommentHost final : public NativeStrategyHost {
public:
    std::string comment;
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) {
            submit_market(Request{Transact{1.0}, "buy", comment});
        }
    }
};

class AfterCalcHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (physical_position().lot_count == 0) {
            submit_market(Request{Transact{1.0}, "buy", ""});
        }
    }
    double runup() const { return open_trade_max_runup(0); }
    double drawdown() const { return open_trade_max_drawdown(0); }
};

class CountHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++callbacks; }
};

class InputTraceHost final : public NativeStrategyHost {
public:
    std::vector<Bar> inputs;
    std::vector<NativeInputContext> contexts;
    std::vector<std::uint64_t> callback_hashes;
    int callbacks = 0;

    void on_native_input(const Bar& bar, const NativeInputContext& context) override {
        inputs.push_back(bar);
        contexts.push_back(context);
        callback_hashes.push_back(native_continuation_hash());
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++callbacks; }
};

class SubmitRealtimeHost final : public NativeStrategyHost {
public:
    native_order::RequestHandle live{};
    bool submitted = false;
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++callbacks; }
    double runup() const { return open_trade_max_runup(0); }
};

class RecordHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    std::vector<Bar> bars;
    std::vector<NativeDecisionContext> contexts;
    bool buy_on_first = false;
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        ++callbacks;
        bars.push_back(bar);
        contexts.push_back(context);
        if (buy_on_first && callbacks == 1) {
            submit_market(Request{Transact{1.0}, "buy", ""});
        }
    }
    double runup() const { return open_trade_max_runup(0); }
};

int applied_fill_count(const NativeStrategyHost& host);

class PostCalculationCurrentHost final : public NativeStrategyHost {
public:
    bool script_body_completed = false;
    native_order::SubmitStatus submit_status = native_order::SubmitStatus::Rejected;
    std::optional<NativeCurrentPointView> point_after_script;
    std::optional<native_order::ExecutionAppliedEvent> applied;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        // This models a host's script body completing before the source
        // policy submits its current-point operation.
        script_body_completed = true;
        const auto submitted = submit_market(Request{Transact{1.0}, "post-calc", ""});
        submit_status = submitted.status;
        point_after_script = current_execution_point();
        if (!submitted.handle) return;
        const auto result = execute_current(
            {*submitted.handle, NativeCurrentPriceRule::NearestTick});
        if (const auto* event =
                std::get_if<native_order::ExecutionAppliedEvent>(&result)) {
            applied = *event;
        }
    }
};

class TickCurrentHost final : public NativeStrategyHost {
public:
    std::vector<Bar> ticks;
    std::vector<NativeTickContext> contexts;
    std::vector<std::uint64_t> callback_hashes;
    int applied_before_first_tick = -1;
    std::optional<NativeCurrentPointView> first_current_point;
    std::optional<native_order::ExecutionAppliedEvent> current_applied;

    void on_native_tick(const Bar& tick, const NativeTickContext& context) override {
        ticks.push_back(tick);
        contexts.push_back(context);
        callback_hashes.push_back(native_continuation_hash());
        if (ticks.size() != 1) return;
        applied_before_first_tick = applied_fill_count(*this);
        const auto submitted = submit_market(Request{Transact{1.0}, "tick-current", ""});
        first_current_point = current_execution_point();
        if (!submitted.handle) return;
        const auto result = execute_current(
            {*submitted.handle, NativeCurrentPriceRule::NearestTick});
        if (const auto* event =
                std::get_if<native_order::ExecutionAppliedEvent>(&result)) {
            current_applied = *event;
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

class NanHost final : public NativeStrategyHost {
public:
    double qty = 0.0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        submit_market(Request{Transact{qty}, "bad", ""});
    }
};

class AbortHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) request_abort();
    }
};

class BeginAbortHost final : public NativeStrategyHost {
public:
    int bars = 0;
    void on_native_run_begin() override { request_abort(); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bars; }
};

class ReenterHost final : public NativeStrategyHost {
public:
    enum class Action {
        None = 0,
        End,
        Advance,
        PushBar,
        PushTick,
        PushTicks,
        Run,
        StreamBegin,
    };
    Action from_begin = Action::None;
    Action from_bar = Action::None;
    Bar nested = bar_at(120000, 110, 111, 109, 110);
    TradeTick nested_tick{120000, 1, 110.0, 1.0};
    bool nested_ok = true;
    int callbacks = 0;
    int begins = 0;

    void on_native_run_begin() override {
        ++begins;
        nested_ok = fire(from_begin);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        nested_ok = fire(from_bar);
    }

    bool fire(Action action) {
        switch (action) {
        case Action::None:
            return true;
        case Action::End:
            return stream_end(false);
        case Action::Advance:
            return stream_advance_time(180000);
        case Action::PushBar:
            return stream_push_bar(nested);
        case Action::PushTick:
            return stream_push_tick(nested_tick);
        case Action::PushTicks:
            return stream_push_ticks(&nested_tick, 1);
        case Action::Run:
            run(&nested, 1);
            return native_state().kind != NativeLifecycleKind::Failed;
        case Action::StreamBegin:
            return stream_begin(&nested, 1, "", "");
        }
        return true;
    }
};

class ReenterEndAfterCalcHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        submit_market(Request{Transact{1.0}, "reenter-buy", ""});
        stream_end(false);
    }
};

int applied_fill_count(const NativeStrategyHost& host) {
    int n = 0;
    for (const auto& event : host.native_events(0)) {
        if (event.command
            && std::holds_alternative<native_order::ExecutionAppliedEvent>(*event.command)) {
            ++n;
        }
    }
    return n;
}

class BeginSubmitHost final : public NativeStrategyHost {
public:
    bool threw = false;
    native_order::SubmitStatus status = native_order::SubmitStatus::Rejected;
    int64_t floor_at_begin = 0;
    int64_t state_floor_at_begin = 0;
    int64_t birth_floor = 0;
    int callbacks = 0;
    void on_native_run_begin() override {
        floor_at_begin = native_decision_floor();
        state_floor_at_begin = native_state().decision_floor_ms;
        try {
            status = submit_market(Request{Transact{1.0}, "begin", ""}).status;
        } catch (...) {
            threw = true;
        }
        for (const auto& event : native_events(0)) {
            if (!event.command) continue;
            if (const auto* accepted =
                    std::get_if<native_order::AcceptedEvent>(&*event.command)) {
                birth_floor = accepted->birth().decision_time_lower_bound;
            }
        }
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++callbacks; }
};

class NonstandardThrowHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { throw 42; }
};

class BeginNonstandardThrowHost final : public NativeStrategyHost {
public:
    int bars = 0;
    void on_native_run_begin() override { throw 42; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bars; }
};

const native_order::ExecutionAppliedEvent* first_applied(
        const std::vector<NativeMarketEvent>& events) {
    for (const auto& event : events) {
        if (!event.command) continue;
        if (const auto* applied =
                std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
            return applied;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    {
        const std::string name = typeid(NativeStrategyHost).name();
        CHECK(name.find("engine_script_run_v18") != std::string::npos);
        CHECK(name.find("NativeStrategyHost") != std::string::npos);
    }
    {
        NativeRunSpec spec = spec_for("preflight-empty-batch", 1);
        CHECK(preflight_native_inputs(spec, nullptr, 0, NativeInputPolicy::Batch));
        CHECK(preflight_native_inputs(spec, nullptr, 0, NativeInputPolicy::StreamWarmup));
        Bar bars[2] = {bar_at(60000, 100, 101, 99, 100), bar_at(120000, 101, 102, 100, 101)};
        CHECK(preflight_native_inputs(spec, bars, 2, NativeInputPolicy::Batch));
        Bar mid = bar_at(90000, 100, 101, 99, 100);
        const auto off = preflight_native_inputs(spec, &mid, 1, NativeInputPolicy::Batch);
        CHECK(off.error == NativeInputPreflightError::OffGridLabel);
        CHECK(off.index == 0);
    }

    // A28(4): the generic calculation callback remains a current-execution
    // frame through host work that follows the script body.
    {
        PostCalculationCurrentHost host;
        auto spec = spec_for("post-calculation-current-permission", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const Bar bar = bar_at(60000, 100, 110, 90, 101);
        host.run(&bar, 1);
        CHECK(host.last_error().empty());
        CHECK(host.script_body_completed);
        CHECK(host.submit_status == native_order::SubmitStatus::Accepted);
        CHECK(host.point_after_script.has_value());
        CHECK(host.applied.has_value());
        if (host.point_after_script && host.applied) {
            CHECK(host.applied->effective_time_ms()
                  == host.point_after_script->decision.coordinate.effective_time_ms);
            CHECK(host.applied->interval_open_ms()
                  == host.point_after_script->decision.coordinate.open_ms);
            near(host.applied->resolved_price, 101.0);
        }
        near(host.physical_position().signed_units, 1.0);
    }

    // A28(3): accepted realtime prints enter a current generic callback in
    // arrival order before matching. The first callback observes an already
    // live market request still unfilled, then executes its own current
    // request at the print coordinate; the prior request is matched only
    // after that callback returns.
    {
        TickCurrentHost host;
        auto spec = spec_for("native-tick-current-order", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const Bar warmup = bar_at(0, 100, 101, 99, 100);
        CHECK(host.stream_begin(&warmup, 1, "1", "1"));
        const auto preexisting = host.submit_market(Request{Transact{1.0}, "preexisting", ""});
        CHECK(preexisting.status == native_order::SubmitStatus::Accepted);
        CHECK(preexisting.handle.has_value());
        const TradeTick ticks[] = {
            {60001, 41, 100.25, 2.0},
            {60002, 42, 100.50, 3.0},
            {60003, 43, 100.75, 4.0},
        };
        CHECK(host.stream_push_ticks(ticks, 3));
        CHECK(host.ticks.size() == 3);
        CHECK(host.contexts.size() == 3);
        CHECK(host.callback_hashes.size() == 3);
        CHECK(host.applied_before_first_tick == 0);
        CHECK(host.first_current_point.has_value());
        CHECK(host.current_applied.has_value());
        for (std::size_t i = 0; i < host.ticks.size() && i < host.contexts.size(); ++i) {
            CHECK(host.ticks[i].timestamp == ticks[i].timestamp);
            near(host.ticks[i].open, ticks[i].price);
            near(host.ticks[i].volume, ticks[i].quantity);
            CHECK(host.contexts[i].sequence == ticks[i].sequence);
            CHECK(host.contexts[i].decision.coordinate.effective_time_ms == ticks[i].timestamp);
            CHECK(host.contexts[i].decision.coordinate.source_price_time_ms == ticks[i].timestamp);
            CHECK(host.contexts[i].decision.sub_bar_open_ms == ticks[i].timestamp);
            CHECK(host.callback_hashes[i] != 0);
        }
        if (host.first_current_point && host.current_applied) {
            CHECK(host.current_applied->effective_time_ms()
                  == host.first_current_point->decision.coordinate.effective_time_ms);
            near(host.current_applied->resolved_price, ticks[0].price);
        }
        near(host.physical_position().signed_units, 2.0);
        CHECK(applied_fill_count(host) == 2);
        CHECK(host.stream_end(false));
    }

    // L4a / P1-1: Canonical hosts keep the base per-bar refusal ordering.
    // An off-session bar wins over a later non-monotonic timestamp, while an
    // invalid calendar refuses before any structural inspection.
    {
        NativeRunSpec spec = spec_for("canonical-refusal-order", 1);
        spec.session = "0930-1600:23456";
        const Bar bars[] = {
            bar_at(1749225540000LL, 100, 101, 99, 100),  // known RTH label
            bar_at(std::numeric_limits<int64_t>::max() - 1000,
                   100, 101, 99, 100),                    // outside calendar range
            bar_at(std::numeric_limits<int64_t>::max() - 2000,
                   100, 101, 99, 100),                    // also decreasing
        };
        const auto unaligned = preflight_native_inputs(
            spec, bars, 3, NativeInputPolicy::Batch);
        CHECK(unaligned.error == NativeInputPreflightError::Unaligned);
        CHECK(unaligned.index == 1);

        auto malformed = spec;
        malformed.input_tf = "not-a-timeframe";
        const Bar structural = bar_at(60000, 0.0, 1.0, 0.0, 1.0);
        const auto calendar = preflight_native_inputs(
            malformed, &structural, 1, NativeInputPolicy::Batch);
        CHECK(calendar.error == NativeInputPreflightError::CalendarFailure);
    }

    // A25: a generic host observes every accepted input before the consumer
    // folds the input_tf=1 feed into its script_tf=5 interval. The context is
    // live in the continuation hash during the callback and vanishes after it.
    {
        InputTraceHost host;
        auto spec = spec_for("accepted-input-hook", 1);
        spec.script_tf = "5";
        const Bar bars[] = {
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 101, 102, 100, 101),
            bar_at(180000, 102, 103, 101, 102),
            bar_at(240000, 103, 104, 102, 103),
        };
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.run(bars, 4);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.inputs.size() == 4 && host.contexts.size() == 4
              && host.callback_hashes.size() == 4);
        for (std::size_t i = 0; i < host.inputs.size() && i < host.contexts.size(); ++i) {
            CHECK(host.inputs[i].timestamp == bars[i].timestamp);
            CHECK(host.contexts[i].input_index == static_cast<int>(i));
            CHECK(host.contexts[i].input_interval.open_ms == bars[i].timestamp);
            CHECK(host.contexts[i].script_interval.open_ms == 0);
            CHECK(host.callback_hashes[i] != 0);
        }
        if (host.contexts.size() == 4) {
            CHECK(!host.contexts[0].completes_script_interval);
            CHECK(!host.contexts[1].completes_script_interval);
            CHECK(!host.contexts[2].completes_script_interval);
            CHECK(host.contexts[3].completes_script_interval);
        }

        // The raw Bar itself is part of the in-callback continuation state,
        // not merely its calendar coordinate.
        InputTraceHost first;
        InputTraceHost second;
        auto bits_spec = spec_for("accepted-input-hook-bits", 1);
        CHECK(first.configure_native(bits_spec).status == NativeSetupStatus::Applied);
        CHECK(second.configure_native(bits_spec).status == NativeSetupStatus::Applied);
        const Bar first_bar = bar_at(60000, 100, 100, 100, 100);
        const Bar second_bar = bar_at(60000, 101, 101, 101, 101);
        first.run(&first_bar, 1);
        second.run(&second_bar, 1);
        CHECK(first.callback_hashes.size() == 1 && second.callback_hashes.size() == 1);
        if (first.callback_hashes.size() == 1 && second.callback_hashes.size() == 1) {
            CHECK(first.callback_hashes.front() != second.callback_hashes.front());
        }
    }

    // A13: source-compatible batch labels retain the caller's timestamp as
    // the decision coordinate. This is a pure native fixture: no adapter or
    // source host participates in either the canonical refusal or lowering.
    {
        const Bar legacy_labels[] = {
            bar_at(1000, 100, 101, 99, 100),
            bar_at(2000, 100, 101, 99, 100),
            bar_at(3000, 100, 101, 99, 100),
            bar_at(4000, 100, 101, 99, 100),
        };
        auto canonical_spec = spec_for("slot-label-canonical", 1);
        RecordHost canonical;
        CHECK(canonical.configure_native(canonical_spec).status == NativeSetupStatus::Applied);
        const uint64_t canonical_hash = canonical.native_continuation_hash();
        canonical.run(legacy_labels, 4);
        CHECK(canonical.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(canonical.callbacks == 0);
        CHECK(canonical.last_error()
              == "native confirmed bar timestamp is not a canonical slot label");
        CHECK(canonical.native_continuation_hash() == canonical_hash);

        auto structural_spec = canonical_spec;
        structural_spec.legacy_tolerance = NativeFeedTolerance::BatchStructuralBars;
        RecordHost structural;
        CHECK(structural.configure_native(structural_spec).status == NativeSetupStatus::Applied);
        CHECK(structural.native_continuation_hash() != canonical_hash);

        auto tolerant_spec = canonical_spec;
        tolerant_spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        RecordHost tolerant;
        tolerant.buy_on_first = true;
        CHECK(tolerant.configure_native(tolerant_spec).status == NativeSetupStatus::Applied);
        CHECK(tolerant.native_continuation_hash() != canonical_hash);
        tolerant.run(legacy_labels, 4);
        CHECK(tolerant.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(tolerant.callbacks == 4);
        CHECK(tolerant.bars.size() == 4);
        CHECK(tolerant.contexts.size() == 4);
        for (std::size_t i = 0; i < tolerant.bars.size() && i < tolerant.contexts.size(); ++i) {
            CHECK(tolerant.bars[i].timestamp == legacy_labels[i].timestamp);
            CHECK(tolerant.contexts[i].coordinate.open_ms == legacy_labels[i].timestamp);
            CHECK(tolerant.contexts[i].input_interval.open_ms == legacy_labels[i].timestamp);
            CHECK(tolerant.contexts[i].script_interval.open_ms == legacy_labels[i].timestamp);
            CHECK(tolerant.contexts[i].script_bar_open_ms == legacy_labels[i].timestamp);
        }
        CHECK(applied_fill_count(tolerant) == 1);
        near(tolerant.physical_position().signed_units, 1.0);

        constexpr std::int64_t minute = 60'000;
        const std::int64_t latest_aligned = std::numeric_limits<int64_t>::max()
            - std::numeric_limits<int64_t>::max() % minute;
        const Bar delta_overflow[] = {
            bar_at(-minute, 100, 101, 99, 100),
            bar_at(latest_aligned, 100, 101, 99, 100),
        };
        const auto canonical_overflow = preflight_native_inputs(
            canonical_spec, delta_overflow, 2, NativeInputPolicy::Batch);
        CHECK(canonical_overflow.error
              == NativeInputPreflightError::TimestampDeltaOverflow);
        CHECK(canonical_overflow.index == 1);
        const auto tolerant_overflow = preflight_native_inputs(
            tolerant_spec, delta_overflow, 2, NativeInputPolicy::Batch);
        // ab9714be pine_scheduler.cpp:64-66 refused the overflow on the source
        // route; the tolerant branch keeps that structural refusal.
        CHECK(tolerant_overflow.error
              == NativeInputPreflightError::TimestampDeltaOverflow);
        CHECK(tolerant_overflow.index == 1);
    }

    {
        NativeRunSpec spec = spec_for("preflight-rth-gap", 1);
        spec.session = "0930-1600:23456";
        const int64_t fri_1559 = 1749225540000LL;
        const int64_t mon_0930 = 1749461400000LL;
        const int64_t mon_1100 = 1749466800000LL;
        Bar contiguous[2] = {
            bar_at(fri_1559, 100, 101, 99, 100),
            bar_at(mon_0930, 101, 102, 100, 101),
        };
        Bar skipped[2] = {
            bar_at(fri_1559, 100, 101, 99, 100),
            bar_at(mon_1100, 101, 102, 100, 101),
        };
        const auto batch_sparse = preflight_native_inputs(
            spec, skipped, 2, NativeInputPolicy::Batch);
        CHECK(batch_sparse.ok());
        const auto stream_gap = preflight_native_inputs(
            spec, skipped, 2, NativeInputPolicy::StreamWarmup);
        CHECK(stream_gap.error == NativeInputPreflightError::InSessionGap);
        CHECK(stream_gap.index == 1);
        CHECK(preflight_native_inputs(spec, contiguous, 2, NativeInputPolicy::StreamWarmup));
    }

    {
        NativeRunSpec spec = spec_for("preflight-lunch-slot", 1);
        spec.session = "0930-1130,1300-1600";
        spec.input_tf = "60";
        spec.script_tf = "60";
        const int64_t nominal = 1749213000000LL;   // 12:30
        const int64_t eligible = 1749214800000LL;  // 13:00
        const int64_t mid = 1749213900000LL;       // 12:45
        auto cal = native_calendar::parse_session(spec.session, spec.timezone);
        auto tf = native_calendar::parse_timeframe("60");
        CHECK(cal.has_value());
        CHECK(tf.has_value());
        auto slot_a = native_calendar::interval_containing(*cal, *tf, nominal);
        auto slot_b = native_calendar::interval_containing(*cal, *tf, eligible);
        CHECK(slot_a.has_value());
        CHECK(slot_b.has_value());
        CHECK(slot_a->open_ms == slot_b->open_ms);
        CHECK(native_confirmed_bar_label_admitted(*slot_a, nominal));
        CHECK(native_confirmed_bar_label_admitted(*slot_a, eligible));
        CHECK(!native_confirmed_bar_label_admitted(*slot_a, mid));
        Bar one = bar_at(nominal, 100, 101, 99, 100);
        CHECK(preflight_native_inputs(spec, &one, 1, NativeInputPolicy::Batch));
        Bar clip = bar_at(eligible, 100, 101, 99, 100);
        CHECK(preflight_native_inputs(spec, &clip, 1, NativeInputPolicy::Batch));
        Bar both[2] = {bar_at(nominal, 100, 101, 99, 100),
                       bar_at(eligible, 101, 102, 100, 101)};
        const auto overlap = preflight_native_inputs(spec, both, 2, NativeInputPolicy::Batch);
        CHECK(overlap.error == NativeInputPreflightError::OverlappingSlot);
        Bar off = bar_at(mid, 100, 101, 99, 100);
        CHECK(preflight_native_inputs(spec, &off, 1, NativeInputPolicy::Batch).error
              == NativeInputPreflightError::OffGridLabel);
    }

    {
        ThrowHost host;
        auto spec = spec_for("callback-failure-witness", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[1] = {bar_at(60000, 100, 101, 99, 100)};
        host.run(bars, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::CallbackException);
        const auto first = host.native_state().failure;
        bool threw = false;
        try {
            host.set_input("x", "1");
        } catch (...) {
            threw = true;
        }
        CHECK(threw);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == first.code);
        CHECK(host.native_state().failure.operation == first.operation);
        CHECK(host.native_state().failure.ordinal == first.ordinal);
    }

    {
        EmptyHost host;
        auto spec = spec_for("cabi-no-exception", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        strategy_set_trace_enabled(reinterpret_cast<pf_strategy_t>(&host), 1);
        // L1 pre-begin ingress is generic staging on native-bound hosts.
        CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(host.last_run_status() == 0);
    }

    {
        EmptyHost empty_type;
        EmptyHost crypto_type;
        auto a = spec_for("empty-type-a", 1);
        auto b = spec_for("empty-type-a", 1);
        a.type.clear();
        CHECK(empty_type.configure_native(a).status == NativeSetupStatus::Applied);
        CHECK(crypto_type.configure_native(b).status == NativeSetupStatus::Applied);
        empty_type.run(nullptr, 0);
        crypto_type.run(nullptr, 0);
        CHECK(empty_type.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(crypto_type.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(empty_type.native_state().spec != nullptr);
        CHECK(empty_type.native_state().spec->type.empty());
        CHECK(crypto_type.native_state().spec->type == "crypto");
        CHECK(empty_type.native_continuation_hash() != crypto_type.native_continuation_hash());
        near(empty_type.physical_position().signed_units, 0.0);
        near(crypto_type.physical_position().signed_units, 0.0);
    }

    {
        EmptyHost reused;
        EmptyHost fresh;
        auto spec = spec_for("empty-type-reuse", 1);
        spec.type.clear();
        CHECK(reused.configure_native(spec).status == NativeSetupStatus::Applied);
        reused.run(nullptr, 0);
        CHECK(reused.native_state().kind == NativeLifecycleKind::Completed);
        auto spec2 = spec;
        spec2.identity.run_number = 2;
        CHECK(reused.configure_native(spec2).status == NativeSetupStatus::Applied);
        reused.run(nullptr, 0);
        CHECK(fresh.configure_native(spec2).status == NativeSetupStatus::Applied);
        fresh.run(nullptr, 0);
        CHECK(reused.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(fresh.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(reused.native_state().spec->type.empty());
        CHECK(fresh.native_state().spec->type.empty());
    }

    {
        CommentHost a;
        CommentHost b;
        a.comment = "alpha";
        b.comment = "beta";
        auto spec = spec_for("hash-comment-metadata", 1);
        CHECK(a.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(b.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 102, 103, 101, 102),
        };
        a.run(bars, 2);
        b.run(bars, 2);
        CHECK(a.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(b.native_state().kind == NativeLifecycleKind::Completed);
        near(a.physical_position().signed_units, 1.0);
        near(b.physical_position().signed_units, 1.0);
        CHECK(a.native_continuation_hash() != b.native_continuation_hash());
        const auto events_a = a.native_events(0);
        const auto events_b = b.native_events(0);
        const auto* applied_a = first_applied(events_a);
        const auto* applied_b = first_applied(events_b);
        CHECK(applied_a != nullptr);
        CHECK(applied_b != nullptr);
        if (applied_a && applied_b) {
            near(applied_a->current_ticket, 6.0);
            near(applied_b->current_ticket, 6.0);
            near(applied_a->resolved_price, applied_b->resolved_price);
        }
    }

    {
        NanHost a;
        NanHost b;
        a.qty = bits_to_double(0x7ff8000000000001ULL);
        b.qty = bits_to_double(0x7ff8000000000002ULL);
        auto spec = spec_for("hash-nan-payload", 1);
        CHECK(a.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(b.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[1] = {bar_at(60000, 100, 101, 99, 100)};
        a.run(bars, 1);
        b.run(bars, 1);
        CHECK(a.native_continuation_hash() != b.native_continuation_hash());
        near(a.physical_position().signed_units, 0.0);
        near(b.physical_position().signed_units, 0.0);
    }

    {
        NanHost plus;
        NanHost minus;
        plus.qty = 0.0;
        minus.qty = bits_to_double(0x8000000000000000ULL);
        auto spec = spec_for("hash-signed-zero", 1);
        CHECK(plus.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(minus.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[1] = {bar_at(60000, 100, 101, 99, 100)};
        plus.run(bars, 1);
        minus.run(bars, 1);
        CHECK(plus.native_continuation_hash() != minus.native_continuation_hash());
    }

    {
        AfterCalcHost host;
        auto spec = spec_for("aftercalc-no-prior-hl", 1);
        spec.close_execution = NativeCloseExecution::AfterCalculation;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[1] = {bar_at(60000, 100, 110, 90, 101)};
        host.run(bars, 1);
        CHECK(host.last_error().empty());
        CHECK(host.physical_position().lot_count == 1);
        near(host.runup(), 0.0);
        near(host.drawdown(), 0.0);
    }

    {
        CountHost complete;
        CountHost trailing;
        auto spec = spec_for("coarser-seal-complete", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(complete.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(trailing.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar five[5] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
            bar_at(180000, 100, 101, 99, 100),
            bar_at(240000, 100, 101, 99, 100),
        };
        complete.run(five, 5);
        trailing.run(five, 3);
        CHECK(complete.last_error().empty());
        CHECK(trailing.last_error().empty());
        CHECK(complete.callbacks == 1);
        CHECK(trailing.callbacks == 0);
        CHECK(complete.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(trailing.native_state().kind == NativeLifecycleKind::Completed);
    }

    {
        SubmitRealtimeHost host;
        auto spec = spec_for("warmup-partial-then-seal", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
        };
        CHECK(host.stream_begin(warmup, 3, "1", "5"));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        CHECK(host.callbacks == 0);
        CHECK(host.native_decision_floor() >= 180000);
        auto submitted = host.submit_market(Request{Transact{1.0}, "buy", ""});
        CHECK(submitted.status == native_order::SubmitStatus::Accepted);
        CHECK(host.stream_push_bar(bar_at(180000, 100, 101, 99, 100)));
        CHECK(host.stream_push_bar(bar_at(240000, 100, 101, 99, 100)));
        CHECK(host.callbacks == 1);
        bool opening_fill = false;
        for (const auto& event : host.native_events(0)) {
            if (!event.command) continue;
            if (const auto* applied =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
                if (applied->effective_time_ms() == 0
                    && applied->provenance()
                        == static_cast<std::uint8_t>(NativePriceProvenance::ModeledOHLCOpen)) {
                    opening_fill = true;
                }
            }
        }
        CHECK(!opening_fill);
        CHECK(host.stream_end(false));
    }

    {
        EmptyHost host;
        auto spec = spec_for("stream-gap-push", 1);
        spec.session = "0930-1600:23456";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[1] = {bar_at(1749225540000LL, 100, 101, 99, 100)};
        CHECK(host.stream_begin(warmup, 1, "", ""));
        CHECK(!host.stream_push_bar(bar_at(1749466800000LL, 101, 102, 100, 101)));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        CHECK(host.stream_push_bar(bar_at(1749461400000LL, 101, 102, 100, 101)));
        CHECK(host.stream_end(false));
    }

    {
        CountHost host;
        auto spec = spec_for("seven-input-warmup-partial", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[7] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
            bar_at(180000, 100, 101, 99, 100),
            bar_at(240000, 100, 101, 99, 100),
            bar_at(300000, 100, 101, 99, 100),
            bar_at(360000, 100, 101, 99, 100),
        };
        CHECK(host.stream_begin(warmup, 7, "1", "5"));
        CHECK(host.callbacks == 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        CHECK(host.stream_end(false));
        CHECK(host.callbacks == 1);
    }

    {
        SubmitRealtimeHost host;
        auto spec = spec_for("d2-carried-open-birth", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[1] = {bar_at(0, 100, 101, 99, 100)};
        CHECK(host.stream_begin(warmup, 1, "1", "1"));
        CHECK(host.native_decision_floor() >= 60000);
        CHECK(host.stream_advance_time(90000));
        auto submitted = host.submit_market(Request{Transact{1.0}, "buy", ""});
        CHECK(submitted.status == native_order::SubmitStatus::Accepted);
        CHECK(host.stream_advance_time(180000));
        bool saw_60000 = false;
        bool filled_60000 = false;
        bool filled_120000 = false;
        for (const auto& event : host.native_events(0)) {
            if (event.driver && event.driver->coordinate.provenance
                    == NativePriceProvenance::CarriedOpen) {
                if (event.driver->coordinate.effective_time_ms == 60000) saw_60000 = true;
            }
            if (event.command) {
                if (const auto* applied =
                        std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
                    if (applied->effective_time_ms() == 60000) filled_60000 = true;
                    if (applied->effective_time_ms() == 120000) filled_120000 = true;
                }
            }
        }
        CHECK(saw_60000);
        CHECK(!filled_60000);
        CHECK(filled_120000);
        CHECK(host.stream_end(false));
    }

    {
        EmptyHost host;
        auto spec = spec_for("tick-array-preflight", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[1] = {bar_at(0, 100, 101, 99, 100)};
        CHECK(host.stream_begin(warmup, 1, "1", "1"));
        TradeTick ticks[2] = {
            {60000, 1, 100.25, 1.0},
            {60000, 2, std::numeric_limits<double>::quiet_NaN(), 1.0},
        };
        CHECK(!host.stream_push_ticks(ticks, 2));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        bool observed = false;
        for (const auto& event : host.native_events(0)) {
            if (event.driver && event.driver->coordinate.provenance
                    == NativePriceProvenance::ObservedPrint) {
                observed = true;
            }
        }
        CHECK(!observed);
        CHECK(host.stream_push_ticks(nullptr, 0));
        CHECK(host.stream_end(false));
    }

    {
        ThrowHost host;
        host.throw_next = false;
        auto spec = spec_for("stream-end-keeps-first-failure", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[1] = {bar_at(0, 100, 101, 99, 100)};
        CHECK(host.stream_begin(warmup, 1, "1", "1"));
        TradeTick tick{60100, 1, 100.5, 1.0};
        CHECK(host.stream_push_tick(tick));
        host.throw_next = true;
        CHECK(!host.stream_end(true));
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::CallbackException);
    }

    {
        SubmitRealtimeHost host;
        auto spec = spec_for("tick-no-preentry-hl", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[1] = {bar_at(0, 100, 101, 99, 100)};
        CHECK(host.stream_begin(warmup, 1, "1", "1"));
        TradeTick pre{60100, 1, 110.0, 1.0};
        CHECK(host.stream_push_tick(pre));
        auto submitted = host.submit_market(Request{Transact{1.0}, "buy", ""});
        CHECK(submitted.status == native_order::SubmitStatus::Accepted);
        TradeTick fill{60200, 2, 100.0, 1.0};
        CHECK(host.stream_push_tick(fill));
        CHECK(host.stream_advance_time(120000));
        CHECK(host.physical_position().lot_count == 1);
        near(host.runup(), 0.0);
        CHECK(host.stream_end(false));
    }

    {
        RecordHost host;
        auto spec = spec_for("ticks-continue-coarser", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 101, 102, 100, 101),
            bar_at(120000, 102, 103, 101, 102),
        };
        CHECK(host.stream_begin(warmup, 3, "1", "5"));
        CHECK(host.callbacks == 0);
        CHECK(host.stream_push_tick(TradeTick{180000, 1, 104.0, 2.0}));
        CHECK(host.stream_push_tick(TradeTick{180500, 2, 105.0, 3.0}));
        CHECK(host.callbacks == 0);
        CHECK(host.stream_push_tick(TradeTick{240000, 3, 106.0, 4.0}));
        CHECK(host.callbacks == 0);
        CHECK(host.stream_advance_time(300000));
        CHECK(host.callbacks == 1);
        CHECK(host.bars.size() == 1);
        near(host.bars[0].open, 100.0);
        near(host.bars[0].high, 106.0);
        near(host.bars[0].low, 99.0);
        near(host.bars[0].close, 106.0);
        near(host.bars[0].volume, 1.0 + 1.0 + 1.0 + 5.0 + 4.0);
        CHECK(host.contexts[0].coordinate.provenance == NativePriceProvenance::Calculation);
        bool replayed_ohlc = false;
        bool saw_tick = false;
        for (const auto& event : host.native_events(0)) {
            if (!event.driver) continue;
            if (event.driver->coordinate.provenance == NativePriceProvenance::ObservedPrint) {
                saw_tick = true;
            }
            if (event.driver->coordinate.provenance == NativePriceProvenance::ModeledOHLCOpen
                || event.driver->coordinate.provenance == NativePriceProvenance::ModeledOHLCClose) {
                replayed_ohlc = true;
            }
        }
        CHECK(saw_tick);
        CHECK(!replayed_ohlc);
        CHECK(host.stream_end(false));
        CHECK(host.callbacks == 1);
    }

    {
        RecordHost host;
        auto spec = spec_for("quiet-complete-coarser", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 101, 102, 100, 101),
            bar_at(120000, 102, 103, 101, 102),
        };
        CHECK(host.stream_begin(warmup, 3, "1", "5"));
        CHECK(host.callbacks == 0);
        CHECK(host.stream_advance_time(300000));
        CHECK(host.callbacks == 1);
        CHECK(host.bars.size() == 1);
        near(host.bars[0].open, 100.0);
        near(host.bars[0].high, 103.0);
        near(host.bars[0].low, 99.0);
        near(host.bars[0].close, 102.0);
        near(host.bars[0].volume, 3.0);
        bool saw_carried = false;
        bool replayed_ohlc = false;
        for (const auto& event : host.native_events(0)) {
            if (!event.driver) continue;
            if (event.driver->coordinate.provenance == NativePriceProvenance::CarriedOpen) {
                saw_carried = true;
            }
            if (event.driver->coordinate.provenance == NativePriceProvenance::ModeledOHLCOpen) {
                replayed_ohlc = true;
            }
        }
        CHECK(saw_carried);
        CHECK(!replayed_ohlc);
        CHECK(host.stream_end(false));
    }

    {
        RecordHost mixed;
        mixed.buy_on_first = false;
        auto spec = spec_for("mixed-post-entry-extremes", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(mixed.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(0, 100, 110, 90, 100),
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
        };
        CHECK(mixed.stream_begin(warmup, 3, "1", "5"));
        auto submitted = mixed.submit_market(Request{Transact{1.0}, "buy", ""});
        CHECK(submitted.status == native_order::SubmitStatus::Accepted);
        CHECK(mixed.stream_push_tick(TradeTick{180000, 1, 100.0, 1.0}));
        CHECK(mixed.stream_push_tick(TradeTick{181000, 2, 105.0, 1.0}));
        CHECK(mixed.stream_push_tick(TradeTick{240000, 3, 104.0, 1.0}));
        CHECK(mixed.stream_advance_time(300000));
        CHECK(mixed.callbacks == 1);
        near(mixed.bars[0].high, 110.0);
        CHECK(mixed.physical_position().lot_count == 1);
        near(mixed.runup(), 5.0);
        CHECK(mixed.stream_end(false));
    }

    {
        RecordHost next_el;
        RecordHost after;
        next_el.buy_on_first = true;
        after.buy_on_first = true;
        auto spec_n = spec_for("coarser-next-eligible", 1);
        spec_n.input_tf = "1";
        spec_n.script_tf = "5";
        auto spec_a = spec_n;
        spec_a.identity.session_key = "coarser-after-calc";
        spec_a.close_execution = NativeCloseExecution::AfterCalculation;
        CHECK(next_el.configure_native(spec_n).status == NativeSetupStatus::Applied);
        CHECK(after.configure_native(spec_a).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
        };
        CHECK(next_el.stream_begin(warmup, 3, "1", "5"));
        CHECK(after.stream_begin(warmup, 3, "1", "5"));
        CHECK(next_el.stream_advance_time(300000));
        CHECK(after.stream_advance_time(300000));
        CHECK(next_el.callbacks == 1);
        CHECK(after.callbacks == 1);
        CHECK(next_el.physical_position().lot_count == 0);
        CHECK(after.physical_position().lot_count == 1);
        bool after_fill = false;
        for (const auto& event : after.native_events(0)) {
            if (!event.command) continue;
            if (const auto* applied =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
                if (applied->provenance()
                    == static_cast<std::uint8_t>(NativePriceProvenance::AfterCalculationClose)) {
                    after_fill = true;
                }
            }
        }
        CHECK(after_fill);
        CHECK(next_el.stream_end(false));
        CHECK(after.stream_end(false));
    }

    {
        RecordHost host;
        auto spec = spec_for("rth-friday-quiet-complete", 1);
        spec.session = "0930-1600:23456";
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(1749225300000LL, 100, 101, 99, 100),
            bar_at(1749225360000LL, 100, 101, 99, 100),
            bar_at(1749225420000LL, 100, 101, 99, 100),
        };
        CHECK(host.stream_begin(warmup, 3, "1", "5"));
        CHECK(host.callbacks == 0);
        CHECK(host.stream_advance_time(1749461400000LL));
        CHECK(host.callbacks == 1);
        bool weekend_synth = false;
        for (const auto& event : host.native_events(0)) {
            if (!event.driver) continue;
            if (event.driver->coordinate.provenance == NativePriceProvenance::CarriedOpen
                && event.driver->coordinate.open_ms > 1749225540000LL
                && event.driver->coordinate.open_ms < 1749461400000LL) {
                weekend_synth = true;
            }
        }
        CHECK(!weekend_synth);
        CHECK(host.stream_end(false));
        CHECK(host.callbacks == 1);
    }

    {
        RecordHost host;
        auto spec = spec_for("ny-dst-gap", 1);
        spec.timezone = "America/New_York";
        spec.session = "24x7";
        spec.input_tf = "1";
        spec.script_tf = "5";
        const auto configured = host.configure_native(spec);
        CHECK(configured.status == NativeSetupStatus::Applied);
        auto cal = native_calendar::parse_session("24x7", "America/New_York");
        auto tf = native_calendar::parse_timeframe("1");
        CHECK(cal.has_value());
        CHECK(tf.has_value());
        const int64_t before = 1741492800000LL;
        auto iv = native_calendar::interval_containing(*cal, *tf, before);
        CHECK(iv.has_value());
        Bar warmup[1] = {bar_at(iv->open_ms, 100, 101, 99, 100)};
        CHECK(host.stream_begin(warmup, 1, "1", "5"));
        CHECK(host.stream_advance_time(before + 20LL * 60000LL));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        CHECK(host.stream_end(false));
    }

    {
        RecordHost host;
        auto spec = spec_for("partial-end-unfinished-script", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
        };
        CHECK(host.stream_begin(warmup, 3, "1", "5"));
        CHECK(host.stream_push_tick(TradeTick{180100, 1, 104.0, 1.0}));
        CHECK(host.stream_end(true));
        CHECK(host.callbacks == 0);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    }

    {
        EmptyHost every;
        EmptyHost once;
        auto spec = spec_for("hash-cadence", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(every.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(once.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar first = bar_at(0, 100, 101, 99, 100);
        CHECK(every.stream_begin(&first, 1, "1", "5"));
        CHECK(once.stream_begin(&first, 1, "1", "5"));
        uint64_t ignored = every.native_continuation_hash();
        (void)ignored;
        Bar rest[4] = {
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
            bar_at(180000, 100, 101, 99, 100),
            bar_at(240000, 100, 101, 99, 100),
        };
        for (int i = 0; i < 4; ++i) {
            CHECK(every.stream_push_bar(rest[i]));
            ignored = every.native_continuation_hash();
            CHECK(once.stream_push_bar(rest[i]));
        }
        CHECK(every.stream_end(false));
        CHECK(once.stream_end(false));
        const uint64_t h_every = every.native_continuation_hash();
        const uint64_t h_once = once.native_continuation_hash();
        CHECK(h_every == h_once);
        CHECK(h_every == every.native_continuation_hash());
    }

    {
        NanHost a;
        NanHost b;
        a.qty = bits_to_double(0x7ff8000000000001ULL);
        b.qty = bits_to_double(0x7ff8000000000002ULL);
        auto spec = spec_for("hash-nan-after-cadence", 1);
        CHECK(a.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(b.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[1] = {bar_at(60000, 100, 101, 99, 100)};
        a.run(bars, 1);
        (void)a.native_continuation_hash();
        b.run(bars, 1);
        CHECK(a.native_continuation_hash() != b.native_continuation_hash());
        CommentHost ca;
        CommentHost cb;
        ca.comment = "alpha";
        cb.comment = "beta";
        auto spec_c = spec_for("hash-comment-after-cadence", 1);
        CHECK(ca.configure_native(spec_c).status == NativeSetupStatus::Applied);
        CHECK(cb.configure_native(spec_c).status == NativeSetupStatus::Applied);
        Bar two[2] = {bar_at(60000, 100, 101, 99, 100), bar_at(120000, 102, 103, 101, 102)};
        ca.run(two, 2);
        (void)ca.native_continuation_hash();
        cb.run(two, 2);
        CHECK(ca.native_continuation_hash() != cb.native_continuation_hash());
        near(ca.physical_position().signed_units, 1.0);
        near(cb.physical_position().signed_units, 1.0);
    }

    {
        EmptyHost host;
        auto spec = spec_for("positive-ohlc-nan-volume", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const uint64_t hash_ready = host.native_continuation_hash();
        // Keep the original non-tolerant structural witness unchanged. A
        // separate all-nonpositive row below is the A13 tolerant fixture;
        // do not silently rewrite this historical rejection shape to fit it.
        Bar zero_px{0.0, 101.0, 99.0, 100.0, 1.0, 60000};
        host.run(&zero_px, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(host.last_run_status() != 0);
        CHECK(!host.last_error().empty());
        CHECK(host.native_continuation_hash() == hash_ready);
        CHECK(host.native_consumed_high_water() == 0);
        Bar nanvol{100, 101, 99, 100, std::numeric_limits<double>::quiet_NaN(), 60000};
        host.run(&nanvol, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(host.last_run_status() != 0);
        Bar neg{100, 101, -1, 100, 1.0, 60000};
        host.run(&neg, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(preflight_native_inputs(spec, &zero_px, 1, NativeInputPolicy::Batch).error
              == NativeInputPreflightError::StructuralInvalid);
        CHECK(preflight_native_inputs(spec, &nanvol, 1, NativeInputPolicy::Batch).error
              == NativeInputPreflightError::StructuralInvalid);

        auto tolerant = spec;
        tolerant.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        tolerant.legacy_tolerance = NativeFeedTolerance::BatchStructuralBars;
        Bar tolerant_zero_px{0.0, 1.0, 0.0, 1.0, 1.0, 60000};
        CHECK(preflight_native_inputs(tolerant, &tolerant_zero_px, 1, NativeInputPolicy::Batch));
        CHECK(preflight_native_inputs(tolerant, &nanvol, 1, NativeInputPolicy::Batch));
        CHECK(preflight_native_inputs(tolerant, &zero_px, 1,
                                     NativeInputPolicy::StreamWarmup).error
              == NativeInputPreflightError::StructuralInvalid);
    }

    {
        EmptyHost scalar;
        EmptyHost array;
        auto spec = spec_for("tick-scalar-array-identity", 1);
        CHECK(scalar.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(array.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup{100, 101, 99, 100, 1.0, 0};
        CHECK(scalar.stream_begin(&warmup, 1, "1", "1"));
        CHECK(array.stream_begin(&warmup, 1, "1", "1"));
        TradeTick good{120000, 1, 100.25, 1.0};
        CHECK(scalar.stream_push_tick(good));
        CHECK(array.stream_push_tick(good));
        TradeTick back{60000, 2, 200.0, 1.0};
        const uint64_t h_s = scalar.native_continuation_hash();
        const uint64_t h_a = array.native_continuation_hash();
        CHECK(!scalar.stream_push_tick(back));
        CHECK(!array.stream_push_ticks(&back, 1));
        CHECK(scalar.native_state().kind == NativeLifecycleKind::Running);
        CHECK(array.native_state().kind == NativeLifecycleKind::Running);
        CHECK(scalar.last_run_status() != 0);
        CHECK(array.last_run_status() != 0);
        CHECK(scalar.native_continuation_hash() == h_s);
        CHECK(array.native_continuation_hash() == h_a);
        CHECK(scalar.native_continuation_hash() == array.native_continuation_hash());
        CHECK(scalar.stream_end(false));
        CHECK(array.stream_end(false));
    }

    {
        EmptyHost host;
        auto spec = spec_for("time-advance-regression", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup{100, 101, 99, 100, 1.0, 0};
        CHECK(host.stream_begin(&warmup, 1, "1", "1"));
        CHECK(host.stream_advance_time(90000));
        const int64_t floor = host.native_decision_floor();
        CHECK(floor >= 90000);
        const uint64_t h = host.native_continuation_hash();
        CHECK(!host.stream_advance_time(30000));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        CHECK(host.last_run_status() != 0);
        CHECK(host.native_decision_floor() == floor);
        CHECK(host.native_continuation_hash() == h);
        CHECK(!host.stream_push_tick(TradeTick{60000, 1, 100.5, 1.0}));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        CHECK(host.stream_end(false));
    }

    {
        AbortHost host;
        auto spec = spec_for("abort-batch", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 102, 103, 101, 102),
            bar_at(120000, 104, 105, 103, 104),
        };
        host.run(bars, 3);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);
        CHECK(host.callbacks == 1);
        CHECK(host.last_run_status() != 0);
        const auto refused = host.configure_native(spec);
        CHECK(refused.status != NativeSetupStatus::Applied);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    }

    {
        AbortHost host;
        auto spec = spec_for("abort-realtime", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 100, 101, 99, 100),
        };
        CHECK(host.stream_begin(warmup, 3, "1", "5"));
        CHECK(host.callbacks == 0);
        CHECK(host.stream_push_bar(bar_at(180000, 100, 101, 99, 100)));
        (void)host.stream_push_bar(bar_at(240000, 100, 101, 99, 100));
        CHECK(host.callbacks == 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);
        CHECK(!host.stream_advance_time(360000));
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    }

    {
        BeginAbortHost host;
        auto spec = spec_for("abort-begin-callback", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {bar_at(0, 100, 101, 99, 100), bar_at(60000, 102, 103, 101, 102)};
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);
        CHECK(host.bars == 0);
    }

    {
        BeginSubmitHost host;
        auto spec = spec_for("begin-submit-commands", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {bar_at(0, 100, 101, 99, 100), bar_at(60000, 102, 103, 101, 102)};
        host.run(bars, 2);
        CHECK(!host.threw);
        CHECK(host.status == native_order::SubmitStatus::Accepted);
        CHECK(host.physical_position().lot_count == 1);
    }

    {
        NonstandardThrowHost host;
        auto spec = spec_for("nonstandard-bar-exception", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bar = bar_at(60000, 100, 101, 99, 100);
        host.run(&bar, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::CallbackException);
    }

    {
        BeginNonstandardThrowHost host;
        auto spec = spec_for("nonstandard-begin-exception", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bar = bar_at(60000, 100, 101, 99, 100);
        host.run(&bar, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::CallbackException);
        CHECK(host.bars == 0);
    }

    {
        EmptyHost same;
        EmptyHost split;
        auto spec = spec_for("per-slot-volume-overflow", 1);
        CHECK(same.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(split.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup{100, 101, 99, 100, 1.0, 0};
        CHECK(same.stream_begin(&warmup, 1, "1", "1"));
        CHECK(split.stream_begin(&warmup, 1, "1", "1"));
        const double huge = 1.0e308;
        TradeTick same_slot[2] = {
            {120000, 1, 100.25, huge},
            {120000, 2, 100.50, huge},
        };
        CHECK(!same.stream_push_ticks(same_slot, 2));
        CHECK(same.native_state().kind == NativeLifecycleKind::Running);
        CHECK(same.last_run_status() != 0);
        TradeTick split_slot[2] = {
            {120000, 1, 100.25, huge},
            {180000, 2, 100.50, huge},
        };
        CHECK(split.stream_push_ticks(split_slot, 2));
        CHECK(split.native_state().kind == NativeLifecycleKind::Running);
        CHECK(split.stream_end(false));
        CHECK(same.stream_end(false));
    }

    {
        RecordHost host;
        auto spec = spec_for("next-key-lazy-complete", 1);
        spec.input_tf = "1";
        spec.script_tf = "5";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[3] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 101, 102, 100, 101),
            bar_at(300000, 110, 111, 109, 110),
        };
        host.run(bars, 3);
        CHECK(host.callbacks >= 1);
        bool saw_lazy = false;
        for (const auto& event : host.native_events(0)) {
            if (event.driver && event.driver->coordinate.completion
                    == NativeCompletionKind::LazyComplete) {
                saw_lazy = true;
            }
        }
        CHECK(saw_lazy);
    }

    {
        RecordHost host;
        auto spec = spec_for("clipped-opening-source-time", 1);
        spec.input_tf = "60";
        spec.script_tf = "60";
        spec.session = "0930-1130,1300-1600";
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const int64_t nominal = 1749472200000LL;   // 12:30
        const int64_t clipped = 1749474000000LL;   // 13:00
        Bar bar = bar_at(clipped, 100, 101, 99, 100);
        host.run(&bar, 1);
        CHECK(host.callbacks == 1);
        CHECK(host.contexts[0].coordinate.open_ms == nominal);
        CHECK(host.contexts[0].coordinate.eligible_open_ms == clipped);
        const auto events = host.native_events(0);
        bool saw_open = false;
        for (const auto& event : events) {
            if (!event.driver) continue;
            if (event.driver->coordinate.provenance != NativePriceProvenance::ModeledOHLCOpen) {
                continue;
            }
            saw_open = true;
            CHECK(event.driver->coordinate.open_ms == nominal);
            CHECK(event.driver->coordinate.effective_time_ms == clipped);
            CHECK(event.driver->coordinate.source_price_time_ms == clipped);
        }
        CHECK(saw_open);
    }

    {
        struct Case {
            const char* input;
            const char* script;
            int64_t first;
            int64_t first_close;
            int64_t next;
            int64_t script_open;
            int64_t script_seal;
        };
        const Case cases[] = {
            {"W", "M", 1737936000000LL, 1738540800000LL, 1738540800000LL,
             1735689600000LL, 1738368000000LL},
            {"2D", "W", 1736640000000LL, 1736812800000LL, 1736812800000LL,
             1736121600000LL, 1736726400000LL},
        };
        for (const auto& item : cases) {
            for (auto policy : {NativeCloseExecution::NextEligiblePoint,
                                NativeCloseExecution::AfterCalculation}) {
                RecordHost host;
                auto spec = spec_for(std::string("c5-") + item.input + item.script
                                         + (policy == NativeCloseExecution::AfterCalculation
                                                ? "-after"
                                                : "-next"),
                                     1);
                spec.input_tf = item.input;
                spec.script_tf = item.script;
                spec.close_execution = policy;
                CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
                Bar first{100, 110, 90, 105, 7, item.first};
                CHECK(host.stream_begin(&first, 1, "", ""));
                CHECK(host.callbacks == 1);
                CHECK(host.contexts[0].coordinate.open_ms == item.script_open);
                CHECK(host.contexts[0].coordinate.next_period_open_ms == item.script_seal);
                CHECK(host.contexts[0].coordinate.last_traded_close_ms == item.script_seal);
                CHECK(host.contexts[0].coordinate.effective_time_ms == item.first_close);
                CHECK(host.contexts[0].decision_floor_ms == item.first_close);
                CHECK(host.native_decision_floor() == item.first_close);
                near(host.bars[0].open, 100);
                near(host.bars[0].high, 110);
                near(host.bars[0].low, 90);
                near(host.bars[0].close, 105);
                near(host.bars[0].volume, 7);
                CHECK(host.submit_market(Request{Transact{1.0}, "buy", ""}).status
                      == native_order::SubmitStatus::Accepted);
                CHECK(host.stream_push_bar(bar_at(item.next, 120, 121, 119, 120)));
                CHECK(host.callbacks == 1);
                CHECK(host.physical_position().lot_count == 0);
                CHECK(host.stream_end(false));
            }
        }
    }

    {
        RecordHost ended;
        RecordHost advanced;
        auto spec = spec_for("rth-partial-daily-warmup-seal", 1);
        spec.input_tf = "1";
        spec.script_tf = "D";
        spec.session = "0930-1600:23456";
        CHECK(ended.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(advanced.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar last = bar_at(1749225540000LL, 101, 101, 101, 101);
        CHECK(ended.stream_begin(&last, 1, "", ""));
        CHECK(ended.callbacks == 0);
        CHECK(ended.native_decision_floor() == 1749225600000LL);
        CHECK(ended.stream_end(false));
        CHECK(ended.callbacks == 0);
        CHECK(advanced.stream_begin(&last, 1, "", ""));
        CHECK(advanced.stream_advance_time(1749288600000LL));
        CHECK(advanced.callbacks == 1);
        CHECK(advanced.contexts[0].coordinate.open_ms == 1749202200000LL);
        CHECK(advanced.contexts[0].coordinate.last_traded_close_ms == 1749225600000LL);
        CHECK(advanced.contexts[0].coordinate.next_period_open_ms == 1749288600000LL);
        near(advanced.bars[0].open, 101);
        near(advanced.bars[0].volume, 1);
        bool carried = false;
        for (const auto& event : advanced.native_events(0)) {
            if (event.driver && event.driver->coordinate.provenance
                    == NativePriceProvenance::CarriedOpen) {
                carried = true;
            }
        }
        CHECK(!carried);
        CHECK(advanced.native_decision_floor() == 1749288600000LL);
        CHECK(advanced.stream_end(false));
        CHECK(advanced.callbacks == 1);
    }

    {
        EmptyHost host;
        auto spec = spec_for("magnifier-default-and-refuse", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bar = bar_at(60000, 100, 101, 99, 100);
        host.run(&bar, 1, "1", "1");
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.callbacks == 1);
        EmptyHost refused;
        CHECK(refused.configure_native(spec).status == NativeSetupStatus::Applied);
        const uint64_t before = refused.native_continuation_hash();
        refused.run(&bar, 1, "1", "1", true);
        CHECK(refused.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(refused.last_run_status() != 0);
        CHECK(refused.native_consumed_high_water() == 0);
        CHECK(refused.native_continuation_hash() == before);
        refused.run(&bar, 1, "1", "1", false, 8);
        CHECK(refused.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(refused.last_run_status() != 0);
        refused.run(&bar, 1, "1", "1", false, 4, MagnifierDistribution::UNIFORM);
        CHECK(refused.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(refused.last_run_status() != 0);
        refused.run(&bar, 1, "1", "1", false, 4, MagnifierDistribution::ENDPOINTS);
        CHECK(refused.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(refused.callbacks == 1);
    }

    {
        RecordHost host;
        host.buy_on_first = true;
        auto spec = spec_for("completed-repeat-begin-preserves", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 102, 103, 101, 102),
        };
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.physical_position().lot_count == 1);
        const uint64_t hash = host.native_continuation_hash();
        const uint64_t hw = host.native_consumed_high_water();
        const auto events = host.native_events(0).size();
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.last_run_status() != 0);
        CHECK(!host.last_error().empty());
        CHECK(host.native_continuation_hash() == hash);
        CHECK(host.native_consumed_high_water() == hw);
        CHECK(host.physical_position().lot_count == 1);
        CHECK(host.native_events(0).size() == events);
        host.run(bars, 2, "1", "1");
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.native_continuation_hash() == hash);
        CHECK(!host.stream_begin(&bars[0], 1, "", ""));
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.native_continuation_hash() == hash);
        auto spec2 = spec;
        spec2.identity.run_number = 2;
        CHECK(host.configure_native(spec2).status == NativeSetupStatus::Applied);
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.native_consumed_high_water() == 2);
    }

    {
        EmptyHost host;
        Bar bar = bar_at(60000, 100, 101, 99, 100);
        host.run(&bar, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Unconfigured);
        CHECK(host.last_run_status() != 0);
        CHECK(!host.last_error().empty());
        CHECK(host.native_consumed_high_water() == 0);
        CHECK(!host.stream_begin(&bar, 1, "", ""));
        CHECK(host.native_state().kind == NativeLifecycleKind::Unconfigured);
        auto spec = spec_for("unconfigured-begin-then-configure", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.run(&bar, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.native_consumed_high_water() == 1);
    }

    {
        EmptyHost host;
        auto spec = spec_for("running-forbidden-begin", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup = bar_at(0, 100, 101, 99, 100);
        CHECK(host.stream_begin(&warmup, 1, "", ""));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        const uint64_t hw = host.native_consumed_high_water();
        Bar next = bar_at(60000, 102, 103, 101, 102);
        host.run(&next, 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Contract);
        CHECK(host.native_state().failure.operation == NativeFailureOperation::Begin);
        CHECK(host.native_consumed_high_water() == hw);
        CHECK(host.configure_native(spec).status != NativeSetupStatus::Applied);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Contract);
    }

    {
        AbortHost host;
        auto spec = spec_for("failed-first-failure-persists", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {bar_at(0, 100, 101, 99, 100), bar_at(60000, 102, 103, 101, 102)};
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);
        CHECK(!host.stream_begin(&bars[0], 1, "", ""));
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);
    }

    {
        EmptyHost ready;
        auto spec = spec_for("rich-source-run-failed", 1);
        CHECK(ready.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bar = bar_at(60000, 100, 101, 99, 100);
        std::unordered_map<std::string, std::string> inputs;
        SymInfo info;
        ready.run(&bar, 1, "1", "1", inputs, info);
        CHECK(ready.native_state().kind == NativeLifecycleKind::Completed);
        EmptyHost completed;
        CHECK(completed.configure_native(spec).status == NativeSetupStatus::Applied);
        completed.run(&bar, 1);
        CHECK(completed.native_state().kind == NativeLifecycleKind::Completed);
        completed.run(&bar, 1, "1", "1", inputs, info);
        CHECK(completed.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(completed.last_run_status() != 0);
    }

    {
        ReenterEndAfterCalcHost host;
        auto spec = spec_for("reenter-stream-end-no-aftercalc-fill", 1);
        spec.close_execution = NativeCloseExecution::AfterCalculation;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bar = bar_at(0, 100, 110, 90, 101);
        host.run(&bar, 1);
        CHECK(host.callbacks == 1);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Contract);
        CHECK(host.physical_position().lot_count == 0);
        CHECK(applied_fill_count(host) == 0);
        CHECK(host.last_run_status() != 0);
    }

    {
        const ReenterHost::Action actions[] = {
            ReenterHost::Action::End,
            ReenterHost::Action::Advance,
            ReenterHost::Action::PushBar,
            ReenterHost::Action::PushTick,
            ReenterHost::Action::PushTicks,
            ReenterHost::Action::Run,
            ReenterHost::Action::StreamBegin,
        };
        for (auto action : actions) {
            ReenterHost from_bar;
            from_bar.from_bar = action;
            auto spec = spec_for(std::string("reenter-bar-") + std::to_string(static_cast<int>(action)), 1);
            CHECK(from_bar.configure_native(spec).status == NativeSetupStatus::Applied);
            Bar bar = bar_at(0, 100, 101, 99, 100);
            from_bar.run(&bar, 1);
            CHECK(from_bar.callbacks == 1);
            CHECK(!from_bar.nested_ok);
            CHECK(from_bar.native_state().kind == NativeLifecycleKind::Failed);
            CHECK(from_bar.native_state().failure.code == NativeFailureCode::Contract);
            CHECK(from_bar.native_state().kind != NativeLifecycleKind::Completed);
            CHECK(applied_fill_count(from_bar) == 0);

            ReenterHost from_begin;
            from_begin.from_begin = action;
            auto spec_b = spec_for(std::string("reenter-begin-") + std::to_string(static_cast<int>(action)), 1);
            CHECK(from_begin.configure_native(spec_b).status == NativeSetupStatus::Applied);
            from_begin.run(&bar, 1);
            CHECK(from_begin.begins == 1);
            CHECK(!from_begin.nested_ok);
            CHECK(from_begin.native_state().kind == NativeLifecycleKind::Failed);
            CHECK(from_begin.native_state().failure.code == NativeFailureCode::Contract);
        }
    }

    {
        RecordHost host;
        host.buy_on_first = true;
        auto spec = spec_for("ordinary-callback-submit-still-allowed", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 102, 103, 101, 102),
        };
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.physical_position().lot_count == 1);
        CHECK(applied_fill_count(host) == 1);
    }

    {
        EmptyHost inert;
        CHECK(inert.native_decision_floor() == std::numeric_limits<int64_t>::min());
        CHECK(inert.native_state().decision_floor_ms == std::numeric_limits<int64_t>::min());
        BeginSubmitHost simple;
        auto spec = spec_for("negative-begin-batch-simple", 1);
        CHECK(simple.configure_native(spec).status == NativeSetupStatus::Applied);
        CHECK(simple.native_decision_floor() == std::numeric_limits<int64_t>::min());
        Bar negative = bar_at(-60000, 100, 102, 99, 101);
        simple.run(&negative, 1);
        CHECK(simple.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(simple.status == native_order::SubmitStatus::Accepted);
        CHECK(simple.floor_at_begin == -60000);
        CHECK(simple.state_floor_at_begin == -60000);
        CHECK(simple.birth_floor == -60000);
        CHECK(simple.floor_at_begin == simple.birth_floor);
        CHECK(applied_fill_count(simple) == 1);
        CHECK(simple.physical_position().lot_count == 1);
        near(simple.physical_position().signed_units, 1.0);
        bool saw_open_fill = false;
        for (const auto& event : simple.native_events(0)) {
            if (!event.command) continue;
            if (const auto* applied =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
                CHECK(applied->effective_time_ms() == -60000);
                near(applied->raw_price, 100.0);
                CHECK(applied->birth().decision_time_lower_bound == -60000);
                saw_open_fill = true;
            }
        }
        CHECK(saw_open_fill);

        BeginSubmitHost tf;
        CHECK(tf.configure_native(spec_for("negative-begin-batch-tf", 1)).status
              == NativeSetupStatus::Applied);
        tf.run(&negative, 1, "1", "1");
        CHECK(tf.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(tf.floor_at_begin == -60000);
        CHECK(tf.birth_floor == -60000);
        CHECK(applied_fill_count(tf) == 1);

        BeginSubmitHost warmup;
        CHECK(warmup.configure_native(spec_for("negative-begin-warmup", 1)).status
              == NativeSetupStatus::Applied);
        CHECK(warmup.stream_begin(&negative, 1, "", ""));
        CHECK(warmup.floor_at_begin == -60000);
        CHECK(warmup.birth_floor == -60000);
        CHECK(applied_fill_count(warmup) == 1);
        CHECK(warmup.stream_end(false));
    }

    {
        BeginSubmitHost positive;
        auto spec = spec_for("positive-begin-floor-control", 1);
        CHECK(positive.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bar = bar_at(60000, 100, 101, 99, 100);
        positive.run(&bar, 1);
        CHECK(positive.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(positive.floor_at_begin == 60000);
        CHECK(positive.birth_floor == 60000);
        CHECK(applied_fill_count(positive) == 1);
    }

    {
        BeginSubmitHost empty;
        auto spec = spec_for("empty-batch-unbounded-floor", 1);
        CHECK(empty.configure_native(spec).status == NativeSetupStatus::Applied);
        empty.run(nullptr, 0);
        CHECK(empty.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(empty.native_consumed_high_water() == 1);
        CHECK(empty.floor_at_begin == std::numeric_limits<int64_t>::min());
        CHECK(empty.state_floor_at_begin == std::numeric_limits<int64_t>::min());
        CHECK(empty.birth_floor == std::numeric_limits<int64_t>::min());
        CHECK(empty.status == native_order::SubmitStatus::Accepted);
        CHECK(applied_fill_count(empty) == 0);
        CHECK(empty.physical_position().lot_count == 0);
        CHECK(empty.callbacks == 0);
        bool invented_price = false;
        for (const auto& event : empty.native_events(0)) {
            if (event.driver) invented_price = true;
        }
        CHECK(!invented_price);
        CHECK(empty.native_decision_floor() == std::numeric_limits<int64_t>::min());
    }

    {
        RecordHost host;
        host.buy_on_first = true;
        auto spec = spec_for("late-callback-no-retrofill", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 102, 103, 101, 102),
        };
        host.run(bars, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(applied_fill_count(host) == 1);
        for (const auto& event : host.native_events(0)) {
            if (!event.command) continue;
            if (const auto* applied =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
                CHECK(applied->effective_time_ms() == 60000);
                CHECK(applied->birth().decision_time_lower_bound >= 60000);
                near(applied->raw_price, 102.0);
            }
        }
    }

    {
        EmptyHost bars_then_advance;
        auto spec = spec_for("mode-refuse-bar-then-advance", 1);
        CHECK(bars_then_advance.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup = bar_at(0, 100, 101, 99, 100);
        CHECK(bars_then_advance.stream_begin(&warmup, 1, "", ""));
        CHECK(bars_then_advance.stream_push_bar(bar_at(60000, 102, 103, 101, 102)));
        const uint64_t hash = bars_then_advance.native_continuation_hash();
        const int64_t floor = bars_then_advance.native_decision_floor();
        const auto lots = bars_then_advance.physical_position().lot_count;
        CHECK(!bars_then_advance.stream_advance_time(180000));
        CHECK(bars_then_advance.native_state().kind == NativeLifecycleKind::Running);
        CHECK(bars_then_advance.last_run_status() != 0);
        CHECK(bars_then_advance.native_continuation_hash() == hash);
        CHECK(bars_then_advance.native_decision_floor() == floor);
        CHECK(bars_then_advance.physical_position().lot_count == lots);
        CHECK(bars_then_advance.stream_end(false));

        EmptyHost advance_then_bar;
        CHECK(advance_then_bar.configure_native(spec_for("mode-refuse-advance-then-bar", 1)).status
              == NativeSetupStatus::Applied);
        CHECK(advance_then_bar.stream_begin(&warmup, 1, "", ""));
        CHECK(advance_then_bar.stream_advance_time(90000));
        const uint64_t hash_a = advance_then_bar.native_continuation_hash();
        const int64_t floor_a = advance_then_bar.native_decision_floor();
        CHECK(!advance_then_bar.stream_push_bar(bar_at(60000, 102, 103, 101, 102)));
        CHECK(advance_then_bar.native_state().kind == NativeLifecycleKind::Running);
        CHECK(advance_then_bar.last_run_status() != 0);
        CHECK(advance_then_bar.native_continuation_hash() == hash_a);
        CHECK(advance_then_bar.native_decision_floor() == floor_a);
        CHECK(advance_then_bar.stream_end(false));
    }

    {
        EmptyHost warmup_bar;
        auto spec = spec_for("mode-warmup-then-bar", 1);
        CHECK(warmup_bar.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar warmup = bar_at(0, 100, 101, 99, 100);
        CHECK(warmup_bar.stream_begin(&warmup, 1, "", ""));
        CHECK(warmup_bar.stream_push_bar(bar_at(60000, 102, 103, 101, 102)));
        CHECK(warmup_bar.stream_end(false));
        CHECK(warmup_bar.native_state().kind == NativeLifecycleKind::Completed);

        EmptyHost warmup_ticks;
        CHECK(warmup_ticks.configure_native(spec_for("mode-warmup-then-ticks", 1)).status
              == NativeSetupStatus::Applied);
        CHECK(warmup_ticks.stream_begin(&warmup, 1, "", ""));
        CHECK(warmup_ticks.stream_push_tick(TradeTick{60000, 1, 100.25, 1.0}));
        CHECK(warmup_ticks.stream_advance_time(120000));
        CHECK(warmup_ticks.stream_end(false));
        CHECK(warmup_ticks.native_state().kind == NativeLifecycleKind::Completed);

        EmptyHost warmup_advance;
        CHECK(warmup_advance.configure_native(spec_for("mode-warmup-then-advance", 1)).status
              == NativeSetupStatus::Applied);
        CHECK(warmup_advance.stream_begin(&warmup, 1, "", ""));
        CHECK(warmup_advance.stream_advance_time(180000));
        CHECK(warmup_advance.stream_end(false));
        CHECK(warmup_advance.native_state().kind == NativeLifecycleKind::Completed);
    }

    {
        RecordHost host;
        host.buy_on_first = true;
        auto spec = spec_for("events-after-ordinal-kind-order", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(0, 100, 101, 99, 100),
            bar_at(60000, 102, 103, 101, 102),
        };
        host.run(bars, 2);
        const auto all = host.native_events(0);
        CHECK(host.native_events(0).size() == all.size());
        uint64_t fill_ordinal = 0;
        bool saw_pair = false;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (i + 1 < all.size() && all[i].ordinal == all[i + 1].ordinal) {
                CHECK(static_cast<uint8_t>(all[i].kind) < static_cast<uint8_t>(all[i + 1].kind));
            }
            if (all[i].command
                && std::holds_alternative<native_order::ExecutionAppliedEvent>(*all[i].command)) {
                fill_ordinal = all[i].ordinal;
                CHECK(all[i].kind == NativeEventKind::Command);
                CHECK(i + 1 < all.size());
                CHECK(all[i + 1].ordinal == fill_ordinal);
                CHECK(all[i + 1].kind == NativeEventKind::Account);
                saw_pair = true;
            }
        }
        CHECK(saw_pair);
        CHECK(fill_ordinal > 0);
        const auto suffix = host.native_events(fill_ordinal);
        for (const auto& event : suffix) CHECK(event.ordinal > fill_ordinal);
        const auto group = host.native_events(fill_ordinal - 1);
        CHECK(group.size() >= 2);
        CHECK(group[0].ordinal == fill_ordinal);
        CHECK(group[0].kind == NativeEventKind::Command);
        CHECK(group[1].ordinal == fill_ordinal);
        CHECK(group[1].kind == NativeEventKind::Account);
        CHECK(host.native_events(0).size() == all.size());
    }

    std::printf("%s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
