#include "native_terms_fixture.hpp"

#include <cstdio>

using namespace r4_test;
using namespace r4_terms;

namespace {

// The mutable setter ingress of a source provider, reached the way
// BacktestEngine::set_account_currency_fx_series reaches it.
struct IngressHost : TermsHost {
    bool stage_ingress_series(std::int64_t effective_from_ms, double rate) {
        return as_native_consumer(execution_consumer())
            .stage_account_currency_fx_series({effective_from_ms}, {rate});
    }
};

void stage_and_apply_at_execution_coordinate() {
    TermsHost host;
    NativeExecutionTermsFacts seen{};
    bool saw = false;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        seen = facts;
        saw = true;
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    const auto before = host.native_continuation_hash();
    REQUIRE(host.configure_native(spec("fx-activation")).status == NativeSetupStatus::Applied);
    const NativeFxCurve curve{{T}, {1.25}};
    const auto staged = host.configure_native_fx_curve(curve);
    CHECK(staged.status == NativeSetupStatus::Applied);
    CHECK(host.native_continuation_hash() != before);
    const Bar bar{100, 100, 100, 100, 1, T};
    host.run(&bar, 1);
    completed(host);
    REQUIRE(saw);
    CHECK(seen.fx_effective_time_ms == T);
    CHECK(seen.active_fx == 1.25);
    CHECK(host.physical_position().signed_units == 1.0);
}

// expectation corrected: stream_begin refused the declared curve (Ready,
// "timestamped account-currency FX is not supported by streaming") ->
// stream_begin accepts it (Running) and the refusal moves to tick-driven
// input, because audit lane N6 (design row FP6) rules a curve declared through
// configure_native_fx_curve the stream's immutable FX epoch: confirmed input
// converts on the same clock as a batch of the same bars, while an observation
// hook and a partially finalized slot read a stale one (measured: a step at
// T+30s still converted at the pre-step rate at the T+40s and T+50s prints).
void declared_curve_streams_confirmed_bars_only() {
    TermsHost host;
    std::vector<double> rates;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        rates.push_back(facts.active_fx);
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    REQUIRE(host.configure_native(spec("fx-stream")).status == NativeSetupStatus::Applied);
    REQUIRE(host.configure_native_fx_curve(NativeFxCurve{{T}, {1.25}}).status
            == NativeSetupStatus::Applied);
    const Bar warmup{100, 100, 100, 100, 1, T - 60000};
    CHECK(host.stream_begin(&warmup, 1, "1", "1"));
    CHECK(host.native_state().kind == NativeLifecycleKind::Running);
    CHECK(host.last_error().empty());

    // Tick-driven input is a refusal, not a failure: the stream keeps running.
    CHECK(!host.stream_push_tick(TradeTick{T, 1, 100.0, 1.0}));
    CHECK(host.last_error()
          == "a declared native FX curve requires confirmed-bar stream input");
    CHECK(host.native_state().kind == NativeLifecycleKind::Running);
    CHECK(!host.stream_advance_time(T + 1));
    CHECK(host.native_state().kind == NativeLifecycleKind::Running);

    // The begin-time market order fills at the warmup bar's open, before the
    // curve's first point; a realtime bar then converts at the declared rate.
    REQUIRE(!rates.empty());
    CHECK(rates.front() == 1.0);
    bool closed = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (closed || h.physical_position().signed_units == 0.0) return;
        const auto close = put(h, reduce(1, "fx-stream-close"));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(
            h.execute_current(command(close))));
        closed = true;
    };
    CHECK(host.stream_push_bar(Bar{100, 100, 100, 100, 1, T}));
    CHECK(closed);
    CHECK(rates.back() == 1.25);
    CHECK(host.stream_end(false));
    completed(host);

    // The mutable setter ingress keeps its refusal: that series belongs to an
    // owner that revalues on a broker clock of its own.
    IngressHost ingress;
    REQUIRE(ingress.configure_native(spec("fx-stream-ingress")).status
            == NativeSetupStatus::Applied);
    CHECK(ingress.stage_ingress_series(T, 1.25));
    CHECK(!ingress.stream_begin(&warmup, 1, "1", "1"));
    CHECK(ingress.native_state().kind == NativeLifecycleKind::Ready);
    CHECK(ingress.last_error().find("timestamped account-currency FX is not supported by streaming")
          != std::string::npos);
}

// A resolver that writes a projected field fails the run at the pump's end,
// not at the policy-hook boundary after it: since the v19 value epoch (R5 lane
// V19-C) a hook boundary inside a pump checks the cooperative abort alone, so
// the fill the resolver priced settles on the configuration it left.
void post_resolver_projection_write_fails_at_the_pump_end() {
    TermsHost host;
    bool called = false;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        called = true;
        host.poison_fee();
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(host, spec("fx-projection"), {100});
    CHECK(called);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::ProjectionMismatch);
    // expectation corrected: failed at the Settlement hook boundary with no lot
    // and no account record -> failed at the pump's end (Input, ordinal 0)
    // after the fill booked one lot and its account record, because v19
    // compares the projection per pump (V19-C).
    CHECK(host.native_state().failure.operation == NativeFailureOperation::Input);
    CHECK(host.native_state().failure.ordinal == 0);
    CHECK(host.lots().size() == 1);
    CHECK(host.rows().empty());
    CHECK(accounts(host) == 1);
}

void wrong_phase_is_side_effect_free() {
    TermsHost host;
    const auto before = host.native_continuation_hash();
    const auto result = host.configure_native_fx_curve(NativeFxCurve{{T}, {1.25}});
    CHECK(result.status == NativeSetupStatus::Failed);
    CHECK(result.validation.error == NativeFxCurveError::WrongPhase);
    CHECK(host.native_state().kind == NativeLifecycleKind::Unconfigured);
    CHECK(host.native_continuation_hash() == before);
}

void preview_projection_barrier_is_a_typed_refusal() {
    TermsHost host;
    bool reached = false;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        host.poison_fee();
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, tx(1));
        const auto hash = h.native_continuation_hash();
        const auto preview = h.inspect_current_execution(command(target));
        CHECK(preview.refusal == NativeCurrentRefusal::ConfigurationMismatch);
        CHECK(!preview.settlement_readiness);
        CHECK(h.native_state().kind == NativeLifecycleKind::Running);
        CHECK(h.native_continuation_hash() == hash);
        CHECK(h.lots().empty());
        reached = true;
    };
    run(host, spec("fx-preview-projection"), {100});
    // The callback leaves an intentionally poisoned projected configuration;
    // the pump's end owns the native failure, not the preview itself.
    CHECK(reached);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    // expectation corrected: failed at the end-of-callback boundary with no
    // lot -> failed at the pump's end (Input, ordinal 0) after the resting
    // request filled at its next point, because v19 compares the projection
    // per pump (V19-C). The preview's typed refusal above did not move.
    CHECK(host.native_state().failure.code == NativeFailureCode::ProjectionMismatch);
    CHECK(host.native_state().failure.operation == NativeFailureOperation::Input);
    CHECK(host.native_state().failure.ordinal == 0);
    CHECK(host.lots().size() == 1);
}

void a_f2_curve_boundaries_hash_and_a_f6_reset_clock() {
    auto run_with_curve = [](const char* key, const NativeFxCurve& curve,
                             double account_fx, double expected_fx) {
        TermsHost host;
        NativeExecutionTermsFacts seen{};
        host.resolver = [&](const NativeExecutionTermsFacts& facts) {
            seen = facts;
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::Transact};
        };
        host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
        auto configuration = spec(key);
        configuration.account_fx = account_fx;
        REQUIRE(host.configure_native(configuration).status == NativeSetupStatus::Applied);
        const auto before = host.native_continuation_hash();
        REQUIRE(host.configure_native_fx_curve(curve).status == NativeSetupStatus::Applied);
        CHECK(host.native_continuation_hash() != before);
        const Bar bar{100, 100, 100, 100, 1, T};
        host.run(&bar, 1);
        completed(host);
        CHECK(seen.active_fx == expected_fx);
        const auto [initialized, epoch, rate] = host.fx_clock_state();
        CHECK(!initialized);
        CHECK(epoch == 0);
        CHECK(rate == account_fx);
        return host.native_continuation_hash();
    };
    const auto fallback = run_with_curve("fx-fallback", NativeFxCurve{{T + 1}, {2.0}}, 1.5, 1.5);
    const auto boundary = run_with_curve("fx-boundary", NativeFxCurve{{T}, {2.0}}, 1.5, 2.0);
    CHECK(fallback != boundary);
}

void a_f3_fee_and_a_f4_batch_rate_facts() {
    TermsHost host;
    std::vector<double> rates;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        rates.push_back(facts.active_fx);
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.calculation = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto open = put(h, tx(1, "fx-open"));
        (void)h.execute_current(command(open));
        const auto close = put(h, reduce(1, "fx-close"));
        (void)h.execute_current(command(close));
    };
    auto configuration = spec("fx-forward", 2.0);
    configuration.fee_kind = NativeFeeKind::CashPerExecution;
    configuration.fee_value = 1.0;
    REQUIRE(host.configure_native(configuration).status == NativeSetupStatus::Applied);
    REQUIRE(host.configure_native_fx_curve(NativeFxCurve{{T}, {2.0}}).status
            == NativeSetupStatus::Applied);
    const Bar bar{100, 100, 100, 100, 1, T};
    host.run(&bar, 1);
    completed(host);
    REQUIRE(rates.size() >= 2);
    CHECK(rates[0] == 2.0 && rates[1] == 2.0);
    CHECK(host.rows().size() == 1);
    CHECK(host.rows().front().commission > 0.0);

    struct PercentRow {
        double at_entry = 0.0;
        double marked_up = 0.0;
        double commission = 0.0;
        double signed_units_before_close = 0.0;
        std::vector<double> rates;
    };
    auto percent_row = [](const char* key, double signed_units) {
        TermsHost percent;
        PercentRow out;
        percent.resolver = [&](const NativeExecutionTermsFacts& facts) {
            out.rates.push_back(facts.active_fx);
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::Transact};
        };
        percent.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            const auto open = put(h, tx(signed_units, "percent-open"));
            REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(
                h.execute_current(command(open))));
            out.at_entry = h.native_marked_equity(100.0);
            out.marked_up = h.native_marked_equity(110.0);
            out.signed_units_before_close = h.physical_position().signed_units;
            const auto close = put(h, reduce(1.0, "percent-close"));
            REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(
                h.execute_current(command(close))));
        };
        auto configuration = spec(key);
        configuration.fee_kind = NativeFeeKind::Percent;
        configuration.fee_value = 1.0;
        REQUIRE(percent.configure_native(configuration).status == NativeSetupStatus::Applied);
        REQUIRE(percent.configure_native_fx_curve(NativeFxCurve{{T}, {2.0}}).status
                == NativeSetupStatus::Applied);
        const Bar bar{100, 100, 100, 100, 1, T};
        percent.run(&bar, 1);
        completed(percent);
        REQUIRE(percent.rows().size() == 1);
        out.commission = percent.rows().front().commission;
        return out;
    };

    const auto long_row = percent_row("fx-percent-long", 1.0);
    const auto short_row = percent_row("fx-percent-short", -1.0);
    REQUIRE(long_row.rates.size() >= 2 && short_row.rates.size() >= 2);
    for (double rate : long_row.rates) CHECK(bits(rate) == bits(2.0));
    for (double rate : short_row.rates) CHECK(bits(rate) == bits(2.0));
    // At 2 account units per quote unit, a 10-point mark moves one contract
    // by 20 account units.  The sign proves the short path uses the same FX
    // basis rather than a long-only fee/equity shortcut.
    near(long_row.marked_up - long_row.at_entry, 20.0);
    near(short_row.marked_up - short_row.at_entry, -20.0);
    near(long_row.commission, 4.0);
    near(short_row.commission, 4.0);
    CHECK(bits(long_row.signed_units_before_close) == bits(1.0));
    CHECK(bits(short_row.signed_units_before_close) == bits(-1.0));
}

void a_f7_candidate_timestamp_sink_pin() {
    auto check_sinks = [](TermsHost& host, std::int64_t expected,
                          std::int64_t contrasting_timestamp, const char* trace_name) {
        CHECK(host.engine_timestamp() == expected);
        const auto actual_hash = host.stream_state_hash();
        CHECK(actual_hash == host.stream_hash_at_timestamp(expected));
        CHECK(actual_hash != host.stream_hash_at_timestamp(contrasting_timestamp));
        const auto trace_timestamp = host.emit_trace_timestamp(trace_name);
        REQUIRE(trace_timestamp);
        CHECK(*trace_timestamp == expected);
    };

    TermsHost rejected;
    rejected.resolver = [](const NativeExecutionTermsFacts&) {
        return no::ExecutionTerms{0.0, std::nullopt, no::OpeningShape::Transact};
    };
    bool rejected_reached = false;
    rejected.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, tx(1, "sink-rejected"));
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::MatchRejectedEvent>(result));
        const auto& event = std::get<no::MatchRejectedEvent>(result);
        CHECK(event.reason == no::MatchRejectReason::NonpositivePrice);
        check_sinks(h, event.cursor.point.effective_time_ms,
                    event.cursor.point.effective_time_ms + 1, "rejected-candidate");
        rejected_reached = true;
    };
    rejected.enable_trace_for_sink_test();
    run(rejected, spec("fx-sink-reject"), {100});
    completed(rejected);
    CHECK(rejected_reached);

    TermsHost candidate_no_effect;
    candidate_no_effect.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price, 0.0,
                                   no::OpeningShape::Transact};
    };
    candidate_no_effect.beginning = [](Host& base) {
        put(static_cast<TermsHost&>(base), host_open(no::Side::Long, "sink-candidate-noeffect"));
    };
    candidate_no_effect.enable_trace_for_sink_test();
    REQUIRE(candidate_no_effect.configure_native(spec("fx-sink-candidate-noeffect")).status
            == NativeSetupStatus::Applied);
    const Bar warmup{100, 100, 100, 100, 1, T - 60000};
    REQUIRE(candidate_no_effect.stream_begin(&warmup, 1, "1", "1"));
    REQUIRE(candidate_no_effect.stream_push_tick(TradeTick{T, 1, 100.0, 1.0}));
    const auto candidate_terminal = last_event<no::NoEffectEvent>(candidate_no_effect);
    REQUIRE(candidate_terminal);
    CHECK(candidate_no_effect.resolver_calls == 1);
    check_sinks(candidate_no_effect, candidate_terminal->cursor.point.effective_time_ms,
                candidate_terminal->cursor.point.effective_time_ms + 1, "candidate-noeffect");
    REQUIRE(candidate_no_effect.stream_end(false));
    completed(candidate_no_effect);

    TermsHost evaluation_no_effect;
    bool evaluation_reached = false;
    evaluation_no_effect.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto frame_timestamp = h.engine_timestamp();
        const auto target = put(h, reduce(1.0, "sink-evaluation-noeffect"));
        const auto result = h.execute_current(command(target));
        CHECK(std::holds_alternative<no::NoEffectEvent>(result));
        if (std::holds_alternative<no::NoEffectEvent>(result)) {
            const auto& event = std::get<no::NoEffectEvent>(result);
            const auto contrasting_timestamp = event.cursor.point.effective_time_ms == frame_timestamp
                ? frame_timestamp + 1 : event.cursor.point.effective_time_ms;
            check_sinks(h, frame_timestamp, contrasting_timestamp, "evaluation-noeffect-frame");
        }
        evaluation_reached = true;
    };
    evaluation_no_effect.enable_trace_for_sink_test();
    run(evaluation_no_effect, spec("fx-sink-evaluation-noeffect"), {100});
    completed(evaluation_no_effect);
    CHECK(evaluation_reached);
    CHECK(evaluation_no_effect.resolver_calls == 0);
}

void d1_applied_callback_uses_activation_timestamp() {
    TermsHost host;
    std::vector<double> observed_fx;
    int notifications = 0;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        observed_fx.push_back(facts.active_fx);
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        put(static_cast<TermsHost&>(base), tx(1, "queued-open"));
    };
    host.notification = [&](Host& base, const no::ExecutionAppliedEvent& event) {
        auto& h = static_cast<TermsHost&>(base);
        ++notifications;
        const auto expected = std::max(event.cursor.point.effective_time_ms,
                                       h.native_decision_floor());
        CHECK(h.engine_timestamp() == expected);
        if (event.request().label == "queued-open") {
            const auto close = put(h, reduce(1, "current-caused-close"));
            const auto result = h.execute_current(command(close));
            REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
        }
    };
    auto configuration = spec("fx-applied-activation");
    REQUIRE(host.configure_native(configuration).status == NativeSetupStatus::Applied);
    REQUIRE(host.configure_native_fx_curve(NativeFxCurve{{T}, {1.75}}).status
            == NativeSetupStatus::Applied);
    const Bar bar{100, 100, 100, 100, 1, T};
    host.run(&bar, 1);
    completed(host);
    CHECK(notifications == 2);
    REQUIRE(observed_fx.size() >= 2);
    for (double fx : observed_fx) CHECK(fx == 1.75);
}

}  // namespace

int main() {
    test("FX activation at execution coordinate", stage_and_apply_at_execution_coordinate);
    test("declared curve streams confirmed bars only", declared_curve_streams_confirmed_bars_only);
    test("resolver projection write fails at the pump end",
         post_resolver_projection_write_fails_at_the_pump_end);
    test("wrong phase staging", wrong_phase_is_side_effect_free);
    test("preview projection barrier", preview_projection_barrier_is_a_typed_refusal);
    test("A-F2 curve boundary and A-F6 reset clock", a_f2_curve_boundaries_hash_and_a_f6_reset_clock);
    test("A-F3 fee and A-F4 batch facts", a_f3_fee_and_a_f4_batch_rate_facts);
    test("A-F7 candidate timestamp sink", a_f7_candidate_timestamp_sink_pin);
    test("D1 applied callback activation timestamp", d1_applied_callback_uses_activation_timestamp);
    std::printf("R4-B FX: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
