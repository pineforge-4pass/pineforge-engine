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

}  // namespace

int main() {
    test("queued precommit refusal", queued_refusal_is_nonfinancial_terminal);
    test("preview excludes validator", preview_never_calls_validator);
    test("typed reentrant current refusal", reentrant_current_is_typed_and_nonmutating);
    test("post-hook barrier", post_hook_latch_prevents_effects);
    std::printf("R4-B precommit: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
