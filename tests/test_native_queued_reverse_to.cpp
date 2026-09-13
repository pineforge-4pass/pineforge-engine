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

}  // namespace

int main() {
    test("queued explicit ReverseTo", queued_explicit_reverse);
    test("current ReverseTo preview/execute", current_reverse_preview_and_execute);
    test("host-sized ReverseTo", host_sized_reverse_shape);
    test("host-sized CloseOpposite Flatten", host_sized_close_opposite_uses_flatten);
    std::printf("R4-B reverse: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
