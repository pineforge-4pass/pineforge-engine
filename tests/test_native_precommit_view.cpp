#include "native_terms_fixture.hpp"

#include <cstdio>
#include <stdexcept>
#include <variant>
#include <vector>

using namespace r4_test;
using namespace r4_terms;

namespace {

void check_queued_precommit_rows(const TermsHost& host, const no::RequestHandle& target) {
    const NativePrecommitView* matched = nullptr;
    std::size_t count = 0;
    for (const auto& view : host.precommit_views) {
        if (view.target == target) {
            ++count;
            matched = &view;
        }
    }
    REQUIRE(count == 1);
    REQUIRE(matched);
    CHECK(!matched->current);
    CHECK(matched->settlement_readiness == execution::Status::Applied);
    REQUIRE(matched->closed_row_pnl.size() == host.rows().size());
    for (std::size_t i = 0; i < host.rows().size(); ++i) {
        CHECK(bits(matched->closed_row_pnl[i]) == bits(host.rows()[i].pnl));
    }
}

void a_p1_every_queued_fill_precommits_once() {
    auto check_seeded_close = [](const char* key, no::Request request,
                                 std::vector<Bar> bars) {
        TermsHost host;
        no::RequestHandle target;
        host.beginning = [&, request = std::move(request)](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            h.seed(1.0, 99.0, 301);
            target = put(h, request);
        };
        REQUIRE(host.configure_native(spec(key)).status == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        completed(host);
        check_queued_precommit_rows(host, target);
        REQUIRE(host.rows().size() == 1);
    };

    check_seeded_close("precommit-market", reduce(1.0, "market-close"),
                       {Bar{100, 100, 100, 100, 1, T}});

    auto limit = reduce(1.0, "limit-close");
    limit.trigger = no::Limit{100.0};
    check_seeded_close("precommit-limit", limit, {Bar{100, 100, 100, 100, 1, T}});

    auto stop = reduce(1.0, "stop-close");
    stop.trigger = no::Stop{100.0};
    check_seeded_close("precommit-stop", stop, {Bar{100, 100, 100, 100, 1, T}});

    auto trail = reduce(1.0, "trail-close");
    trail.trigger = no::Trail{1.0, std::nullopt};
    check_seeded_close("precommit-trail", trail, {Bar{101, 101, 99, 99, 1, T}});

    check_seeded_close("precommit-reversal", reverse(-1.0, "reverse-close"),
                       {Bar{100, 100, 100, 100, 1, T}});

    TermsHost selected;
    no::RequestHandle opening_a;
    no::RequestHandle opening_b;
    no::RequestHandle selected_target;
    selected.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (h.calculations == 1) {
            opening_a = put(h, tx(1.0, "selected-a"));
            opening_b = put(h, tx(1.0, "selected-b"));
            return;
        }
        if (h.calculations == 2) {
            auto request = reduce(2.0, "selected-close");
            request.owner = no::BindOpenings{{opening_a, opening_b}, h.cycle()};
            selected_target = put(h, request);
        }
    };
    run(selected, spec("precommit-selected"), {100, 110});
    completed(selected);
    check_queued_precommit_rows(selected, selected_target);
    REQUIRE(selected.rows().size() == 2);

    TermsHost whole_close;
    no::RequestHandle whole_target;
    whole_close.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return no::ExecutionTerms{facts.default_resolved_price, facts.opposite_book_units,
                                       no::OpeningShape::CloseOpposite};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    whole_close.beginning = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        h.seed(1.0, 99.0, 311);
        whole_target = put(h, host_open(no::Side::Short, "whole-close"));
    };
    run(whole_close, spec("precommit-whole-close"), {100});
    completed(whole_close);
    check_queued_precommit_rows(whole_close, whole_target);
    REQUIRE(whole_close.precommit_views.size() == 1);
    CHECK(std::holds_alternative<execution::Flatten>(whole_close.precommit_views.front().plan));
    REQUIRE(whole_close.rows().size() == 1);

    TermsHost refused;
    no::RequestHandle refused_parent;
    no::RequestHandle refused_child;
    refused.validator = [](const NativePrecommitView&) { return NativePrecommitVerdict::Refuse; };
    refused.beginning = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        refused_parent = put(h, tx(1.0, "refused-parent"));
        auto child = tx(1.0, "refused-child");
        child.owner = no::WaitForApplied{refused_parent};
        refused_child = put(h, child);
    };
    run(refused, spec("precommit-refused-parent"), {100});
    completed(refused);
    check_queued_precommit_rows(refused, refused_parent);
    const auto parent_rejection = last_event<no::MatchRejectedEvent>(refused);
    REQUIRE(parent_rejection);
    CHECK(parent_rejection->handle() == refused_parent);
    CHECK(parent_rejection->reason == no::MatchRejectReason::HostPrecommit);
    CHECK(!parent_rejection->attempted_terms);
    CHECK(refused.lots().empty() && refused.rows().empty() && accounts(refused) == 0);
    bool drained_child = false;
    for (const auto& event : events<no::CancelledEvent>(refused)) {
        if (event.handle() == refused_child) {
            drained_child = true;
            CHECK(event.reason == no::CancelReason::OwnerGone);
            REQUIRE(event.cause);
            CHECK(event.cause->ordinal == parent_rejection->ordinal);
        }
    }
    CHECK(drained_child);
}

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

void a_p4_cycle_exhaustion_precedes_effects_and_validator() {
    TermsHost host;
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        h.poison_next_cycle();
        const auto target = put(h, tx(1, "cycle-overflow"));
        bool threw = false;
        try {
            (void)h.execute_current(command(target));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(h.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(h.native_state().failure.code == NativeFailureCode::SettlementFailure);
        CHECK(h.validator_calls == 0);
        CHECK(h.lots().empty() && h.rows().empty());
        reached = true;
    };
    run(host, spec("precommit-cycle"), {100});
    CHECK(reached);
}

void a36_host_margin_verdict_precedes_generic_margin_gate() {
    TermsHost host_margin;
    host_margin.validator = [](const NativePrecommitView&) {
        return NativePrecommitVerdict::AdmitWithHostMargin;
    };
    host_margin.calculation = [](Host& base) {
        put(static_cast<TermsHost&>(base), tx(1.0, "host-margin"));
    };
    auto host_spec = spec("precommit-host-margin");
    host_spec.initial_capital = 50.0;
    host_spec.initial_margin_fraction = 1.0;
    run(host_margin, host_spec, {100.0});
    completed(host_margin);
    CHECK(host_margin.validator_calls == 1);
    CHECK(host_margin.lots().size() == 1);

    TermsHost native_gate;
    native_gate.validator = [](const NativePrecommitView&) {
        return NativePrecommitVerdict::Admit;
    };
    native_gate.calculation = [](Host& base) {
        put(static_cast<TermsHost&>(base), tx(1.0, "native-margin"));
    };
    auto native_spec = spec("precommit-native-margin");
    native_spec.initial_capital = 50.0;
    native_spec.initial_margin_fraction = 1.0;
    run(native_gate, native_spec, {100.0});
    completed(native_gate);
    CHECK(native_gate.validator_calls == 1);
    CHECK(native_gate.lots().empty());
    const auto rejection = last_event<no::MatchRejectedEvent>(native_gate);
    REQUIRE(rejection);
    CHECK(rejection->reason == no::MatchRejectReason::InitialMargin);
}

}  // namespace

int main() {
    test("A-P1 queued precommit matrix", a_p1_every_queued_fill_precommits_once);
    test("queued precommit refusal", queued_refusal_is_nonfinancial_terminal);
    test("preview excludes validator", preview_never_calls_validator);
    test("typed reentrant current refusal", reentrant_current_is_typed_and_nonmutating);
    test("post-hook barrier", post_hook_latch_prevents_effects);
    test("A2 queued validator barrier", a2_queued_validator_latch_barrier);
    test("A-P2 opening-only and A-P3 no-effect", a_p2_opening_only_and_a_p3_noeffect_skip);
    test("A-P5 validator and A-P6b preview", a_p5_validator_exception_and_a_p6b_preview_exception);
    test("A-P6 submit exception split", a_p6_submit_exception_is_callback_failure);
    test("A-P4 cycle exhaustion pre-effects", a_p4_cycle_exhaustion_precedes_effects_and_validator);
    test("A36 host-margin verdict ordering", a36_host_margin_verdict_precedes_generic_margin_gate);
    std::printf("R4-B precommit: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
