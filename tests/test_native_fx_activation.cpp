#include "native_terms_fixture.hpp"

#include <cstdio>

using namespace r4_test;
using namespace r4_terms;

namespace {

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

void staged_curve_refuses_native_streaming() {
    TermsHost host;
    REQUIRE(host.configure_native(spec("fx-stream")).status == NativeSetupStatus::Applied);
    REQUIRE(host.configure_native_fx_curve(NativeFxCurve{{T}, {1.25}}).status
            == NativeSetupStatus::Applied);
    const Bar warmup{100, 100, 100, 100, 1, T - 60000};
    CHECK(!host.stream_begin(&warmup, 1, "1", "1"));
    CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
    CHECK(host.last_error().find("timestamped account-currency FX is not supported by streaming")
          != std::string::npos);
}

void post_resolver_projection_guard_blocks_effects() {
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
    CHECK(host.lots().empty());
    CHECK(host.rows().empty());
    CHECK(accounts(host) == 0);
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

}  // namespace

int main() {
    test("FX activation at execution coordinate", stage_and_apply_at_execution_coordinate);
    test("native stream curve refusal", staged_curve_refuses_native_streaming);
    test("resolver projection barrier", post_resolver_projection_guard_blocks_effects);
    test("wrong phase staging", wrong_phase_is_side_effect_free);
    std::printf("R4-B FX: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
