#include "native_terms_fixture.hpp"

#include <cstdio>
#include <variant>

using namespace r4_test;
using namespace r4_terms;

static_assert(std::variant_size_v<no::ExecutionPlan> == 4);
static_assert(std::variant_size_v<NativeCurrentExecutionResult> == 5);

namespace {

void queued_explicit_reverse() {
    TermsHost host;
    host.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        put(h, tx(2, "opening"));
        put(h, reverse(-1, "queued-reverse"));
    };
    run(host, spec("queued-reverse"), {100});
    completed(host);
    const auto applied = events<no::ExecutionAppliedEvent>(host);
    REQUIRE(applied.size() == 2);
    CHECK(applied[1].request().label == "queued-reverse");
    CHECK(applied[1].opened_units == -1.0);
    CHECK(applied[1].closed_units == 2.0);
    CHECK(applied[1].filled_working == 3.0);
    CHECK(std::holds_alternative<execution::Book>(applied[1].scope));
    CHECK(host.physical_position().signed_units == -1.0);
    CHECK(host.validator_calls == 2);
    REQUIRE(host.precommit_views.size() == 2);
    CHECK(std::holds_alternative<execution::ReverseTo>(host.precommit_views[1].plan));
}

void current_reverse_preview_and_execute() {
    TermsHost host;
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        apply(h, put(h, tx(2)));
        const auto target = put(h, reverse(-1));
        const auto before = h.native_continuation_hash();
        const auto preview = h.inspect_current_execution(command(target));
        CHECK(!preview.refusal);
        CHECK(preview.settlement_readiness == execution::Status::Applied);
        CHECK(preview.closed_row_pnl.size() == 1);
        CHECK(h.validator_calls == 1);
        CHECK(h.native_continuation_hash() == before);
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
        const auto& applied = std::get<no::ExecutionAppliedEvent>(result);
        CHECK(applied.opened_units == -1.0);
        CHECK(applied.closed_units == 2.0);
        CHECK(h.physical_position().signed_units == -1.0);
        CHECK(h.validator_calls == 2);
        reached = true;
    };
    run(host, spec("current-reverse"), {100});
    completed(host);
    CHECK(reached);
}

void host_sized_reverse_shape() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::ReverseTo};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        apply(h, put(h, tx(2)));
        const auto target = put(h, host_open(no::Side::Short));
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
        const auto& applied = std::get<no::ExecutionAppliedEvent>(result);
        CHECK(applied.opened_units == -1.0);
        CHECK(applied.closed_units == 2.0);
        CHECK(h.physical_position().signed_units == -1.0);
        const auto receipt = last_event<no::TermsResolvedEvent>(h);
        REQUIRE(receipt);
        CHECK(receipt->input.terms.shape == no::OpeningShape::ReverseTo);
        reached = true;
    };
    run(host, spec("host-reverse"), {100});
    completed(host);
    CHECK(reached);
}

void host_sized_close_opposite_uses_flatten() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return no::ExecutionTerms{facts.default_resolved_price, facts.opposite_book_units,
                                       no::OpeningShape::CloseOpposite};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        apply(h, put(h, tx(2)));
        const auto target = put(h, host_open(no::Side::Short));
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
        const auto& applied = std::get<no::ExecutionAppliedEvent>(result);
        CHECK(applied.closed_units == 2.0);
        CHECK(applied.opened_units == 0.0);
        CHECK(applied.terminal);
        CHECK(applied.terminal_reason == no::AppliedTerminalReason::WorkingUnitsSatisfied);
        CHECK(h.physical_position().signed_units == 0.0);
        REQUIRE(!h.precommit_views.empty());
        CHECK(std::holds_alternative<execution::Flatten>(h.precommit_views.back().plan));
        reached = true;
    };
    run(host, spec("host-close-opposite"), {100});
    completed(host);
    CHECK(reached);
}

void a_r1_exact_queued_reverse_both_signs() {
    const double target = from_bits(0x3fb999999999999aULL);
    for (const double sign : {1.0, -1.0}) {
        TermsHost host;
        no::RequestHandle reverse_target;
        bool submitted = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (submitted) return;
            submitted = true;
            // Two lots make the closing leg FIFO-visible; the requested
            // target is the exact binary64 opening magnitude.
            h.seed(-sign * 0.7, 99.7, 71);
            h.seed(-sign * 0.6, 100.3, 72);
            reverse_target = put(h, reverse(sign * target, sign > 0 ? "r-long" : "r-short"));
        };
        run(host, spec(sign > 0 ? "reverse-exact-long" : "reverse-exact-short"), {100});
        completed(host);
        const auto rows = events<no::ExecutionAppliedEvent>(host);
        REQUIRE(rows.size() == 1);
        const auto& applied = rows.front();
        CHECK(applied.handle() == reverse_target);
        CHECK(bits(applied.opened_units) == bits(sign * target));
        near(applied.closed_units, 1.3);
        CHECK(applied.filled_working == applied.closed_units + std::abs(applied.opened_units));
        CHECK(host.physical_position().signed_units == sign * target);
        CHECK(host.validator_calls == 1);
        REQUIRE(!host.precommit_views.empty());
        CHECK(std::holds_alternative<execution::ReverseTo>(host.precommit_views.front().plan));
    }
}

void a_r2_priced_stop_reverse_and_r4_no_opposite() {
    TermsHost host;
    host.beginning = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        h.seed(-1.0, 100, 91);
        auto request = reverse(0.5, "stop-reverse");
        request.trigger = no::Stop{101};
        put(h, request);
    };
    REQUIRE(host.configure_native(spec("reverse-stop")).status == NativeSetupStatus::Applied);
    const Bar bar{100, 102, 100, 102, 1, T};
    host.run(&bar, 1);
    completed(host);
    const auto applied = last_event<no::ExecutionAppliedEvent>(host);
    REQUIRE(applied);
    CHECK(applied->request().label == "stop-reverse");
    CHECK(applied->raw_price == 101.0);
    CHECK(bits(applied->opened_units) == bits(0.5));

    TermsHost flat;
    flat.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), reverse(1.0, "flat")); };
    run(flat, spec("reverse-flat"), {100});
    completed(flat);
    const auto rejected = last_event<no::MatchRejectedEvent>(flat);
    REQUIRE(rejected);
    CHECK(rejected->reason == no::MatchRejectReason::NoOppositeExposure);
    CHECK(flat.lots().empty() && flat.rows().empty());
}

void a_r3_preview_rows_match_current_reverse() {
    TermsHost host;
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        h.seed(1.0, 100, 111);
        const auto target = put(h, reverse(-0.5, "preview-reverse"));
        const auto preview = h.inspect_current_execution(command(target));
        REQUIRE(preview.settlement_readiness == execution::Status::Applied);
        REQUIRE(preview.closed_row_pnl.size() == 1);
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
        CHECK(h.rows().size() == 1);
        CHECK(h.rows().back().pnl == preview.closed_row_pnl.front());
        reached = true;
    };
    run(host, spec("reverse-preview"), {100});
    completed(host);
    CHECK(reached);
}

}  // namespace

int main() {
    test("queued explicit ReverseTo", queued_explicit_reverse);
    test("current ReverseTo preview/execute", current_reverse_preview_and_execute);
    test("host-sized ReverseTo", host_sized_reverse_shape);
    test("host-sized CloseOpposite Flatten", host_sized_close_opposite_uses_flatten);
    test("A-R1 exact queued reverse both signs", a_r1_exact_queued_reverse_both_signs);
    test("A-R2/A-R4 priced and flat reverse", a_r2_priced_stop_reverse_and_r4_no_opposite);
    test("A-R3 current reversal preview", a_r3_preview_rows_match_current_reverse);
    std::printf("R4-B reverse: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
