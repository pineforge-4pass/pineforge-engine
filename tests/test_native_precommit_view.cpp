#include "native_terms_fixture.hpp"

#include <cstdio>
#include <stdexcept>
#include <variant>

using namespace r4_test;
using namespace r4_terms;

namespace {

void queued_refusal_is_nonfinancial_terminal() {
    TermsHost host;
    host.validator = [](const NativePrecommitView& view) {
        CHECK(!view.current);
        CHECK(view.settlement_readiness == execution::Status::Applied);
        return NativePrecommitVerdict::Refuse;
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(host, spec("precommit-refuse"), {100});
    completed(host);
    const auto rejection = last_event<no::MatchRejectedEvent>(host);
    REQUIRE(rejection);
    CHECK(rejection->reason == no::MatchRejectReason::HostPrecommit);
    CHECK(host.validator_calls == 1);
    CHECK(host.lots().empty());
    CHECK(host.rows().empty());
    CHECK(accounts(host) == 0);
}

void preview_never_calls_validator() {
    TermsHost host;
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, tx(1));
        const auto hash = h.native_continuation_hash();
        const auto preview = h.inspect_current_execution(command(target));
        CHECK(!preview.refusal);
        CHECK(preview.settlement_readiness == execution::Status::Applied);
        CHECK(h.validator_calls == 0);
        CHECK(h.native_continuation_hash() == hash);
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
        CHECK(h.validator_calls == 1);
        REQUIRE(!h.precommit_views.empty());
        CHECK(h.precommit_views.back().current);
        reached = true;
    };
    run(host, spec("precommit-preview"), {100});
    completed(host);
    CHECK(reached);
}

void reentrant_current_is_typed_and_nonmutating() {
    TermsHost host;
    no::RequestHandle target;
    bool nested_seen = false;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        if (facts.target == target) {
            const auto nested = host.execute_current(command(target));
            CHECK(std::holds_alternative<NativeCurrentRefusal>(nested));
            if (std::holds_alternative<NativeCurrentRefusal>(nested)) {
                CHECK(std::get<NativeCurrentRefusal>(nested) == NativeCurrentRefusal::Reentrant);
                nested_seen = true;
            }
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        target = put(h, tx(1));
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
    };
    run(host, spec("precommit-reentrant"), {100});
    completed(host);
    CHECK(nested_seen);
    CHECK(host.rows().empty());
    CHECK(host.physical_position().signed_units == 1.0);
}

void post_hook_latch_prevents_effects() {
    TermsHost host;
    bool hook_called = false;
    host.validator = [&](const NativePrecommitView&) {
        hook_called = true;
        const Bar forbidden{100, 100, 100, 100, 1, T + 60000};
        CHECK(!host.stream_push_bar(forbidden));
        return NativePrecommitVerdict::Proceed;
    };
    host.calculation = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, tx(1));
        try {
            (void)h.execute_current(command(target));
        } catch (const std::runtime_error&) {
        }
    };
    run(host, spec("precommit-latch"), {100});
    CHECK(hook_called);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.lots().empty());
    CHECK(host.rows().empty());
    CHECK(accounts(host) == 0);
}

void a2_queued_validator_latch_barrier() {
    TermsHost host;
    bool hook_called = false;
    host.validator = [&](const NativePrecommitView& view) {
        hook_called = true;
        CHECK(!view.current);
        CHECK(!host.stream_push_bar(Bar{100, 100, 100, 100, 1, T + 1}));
        return NativePrecommitVerdict::Proceed;
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(host, spec("precommit-queued-latch"), {100});
    CHECK(hook_called);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::Contract);
    CHECK(host.rows().empty() && host.lots().empty() && accounts(host) == 0);
}

void a_p2_opening_only_and_a_p3_noeffect_skip() {
    TermsHost opening;
    bool opening_seen = false;
    opening.validator = [&](const NativePrecommitView& view) {
        opening_seen = true;
        CHECK(view.settlement_readiness == execution::Status::Applied);
        CHECK(view.inspected_closed_units == 0.0);
        CHECK(view.closed_row_pnl.empty());
        return NativePrecommitVerdict::Proceed;
    };
    opening.calculation = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, tx(1));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(h.execute_current(command(target))));
    };
    run(opening, spec("precommit-opening-only"), {100});
    completed(opening);
    CHECK(opening_seen && opening.validator_calls == 1);

    TermsHost no_effect;
    bool reached = false;
    no_effect.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, flat("flat-noeffect"));
        const auto preview = h.inspect_current_execution(command(target));
        CHECK(preview.settlement_readiness == execution::Status::NoEffect);
        CHECK(h.validator_calls == 0);
        const auto result = h.execute_current(command(target));
        CHECK(std::holds_alternative<no::NoEffectEvent>(result));
        CHECK(h.validator_calls == 0);
        reached = true;
    };
    run(no_effect, spec("precommit-noeffect"), {100});
    completed(no_effect);
    CHECK(reached);
}

void a_p5_validator_exception_and_a_p6b_preview_exception() {
    TermsHost queued;
    queued.validator = [](const NativePrecommitView&) -> NativePrecommitVerdict {
        throw std::runtime_error("validator escaped");
    };
    queued.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(queued, spec("precommit-validator-queued"), {100});
    CHECK(queued.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(queued.native_state().failure.code == NativeFailureCode::CallbackException);
    CHECK(queued.rows().empty() && queued.lots().empty());

    TermsHost preview;
    no::RequestHandle target;
    bool typed_preview_reentrant = false;
    preview.resolver = [&](const NativeExecutionTermsFacts& facts) {
        const auto nested = preview.inspect_current_execution(command(target));
        CHECK(nested.refusal == NativeCurrentRefusal::Reentrant);
        typed_preview_reentrant = true;
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    preview.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        target = put(h, tx(1));
        const auto hash = h.native_continuation_hash();
        const auto row = h.inspect_current_execution(command(target));
        CHECK(!row.refusal && row.settlement_readiness == execution::Status::Applied);
        CHECK(typed_preview_reentrant);
        CHECK(h.native_continuation_hash() == hash);
        reached = true;
    };
    run(preview, spec("precommit-preview-reentrant"), {100});
    completed(preview);
    CHECK(reached);
}

void a_p6_submit_exception_is_callback_failure() {
    TermsHost host;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) -> no::ExecutionTerms {
        // submit throws inside the seal; unlike execute_current's typed
        // Reentrant result this deliberately escapes the hook.
        (void)host.submit(tx(1, "forbidden"));
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(host, spec("precommit-submit-exception"), {100});
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::CallbackException);
    CHECK(host.rows().empty() && host.lots().empty());
}

}  // namespace

int main() {
    test("queued precommit refusal", queued_refusal_is_nonfinancial_terminal);
    test("preview excludes validator", preview_never_calls_validator);
    test("typed reentrant current refusal", reentrant_current_is_typed_and_nonmutating);
    test("post-hook barrier", post_hook_latch_prevents_effects);
    test("A2 queued validator barrier", a2_queued_validator_latch_barrier);
    test("A-P2 opening-only and A-P3 no-effect", a_p2_opening_only_and_a_p3_noeffect_skip);
    test("A-P5 validator and A-P6b preview", a_p5_validator_exception_and_a_p6b_preview_exception);
    test("A-P6 submit exception split", a_p6_submit_exception_is_callback_failure);
    std::printf("R4-B precommit: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
