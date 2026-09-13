#include "native_terms_fixture.hpp"
#include "../src/native_matching.hpp"

#include <cstdio>
#include <type_traits>
#include <variant>

using namespace r4_test;
using namespace r4_terms;

static_assert(std::variant_size_v<NativeCurrentExecutionResult> == 5);
static_assert(std::is_same_v<std::variant_alternative_t<4, NativeCurrentExecutionResult>,
                             no::CancelledEvent>);

namespace {

// Test-only oracle seam: a free function receives only the facts the native
// host surface exposes. It models no adapter policy and is not product code.
no::ExecutionTerms oracle_terms_from_facts(const NativeExecutionTermsFacts& facts) {
    const double units = facts.price_kind == no::NativeCandidatePriceKind::CurrentQuote
        ? 2.0 : 1.0;
    return {facts.default_resolved_price, units, no::OpeningShape::Transact};
}

class OracleHost final : public TermsHost {
public:
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        ++oracle_calls;
        last = facts;
        return oracle_terms_from_facts(facts);
    }

    mutable int oracle_calls = 0;
    mutable NativeExecutionTermsFacts last{};
};

void bit_helper_controls() {
    const double nan_a = from_bits(0x7ff8000000000001ULL);
    const double nan_b = from_bits(0x7ff8000000000002ULL);
    CHECK(native_matching::double_bits(nan_a) == 0x7ff8000000000001ULL);
    CHECK(native_matching::double_bits(nan_a) == native_matching::double_bits(nan_a));
    CHECK(native_matching::double_bits(nan_a) != native_matching::double_bits(nan_b));
    CHECK(native_matching::double_bits(0.0) == 0x0000000000000000ULL);
    CHECK(native_matching::double_bits(-0.0) == 0x8000000000000000ULL);
}

void test_only_oracle_host_uses_facts() {
    OracleHost host;
    host.beginning = [](Host& base) { put(static_cast<OracleHost&>(base), host_open()); };
    run(host, spec("terms-oracle"), {100});
    completed(host);
    CHECK(host.oracle_calls == 1);
    CHECK(host.last.price_kind == no::NativeCandidatePriceKind::PointPrice);
    CHECK(host.physical_position().signed_units == 1.0);
}

void queued_host_sized_open() {
    TermsHost host;
    no::RequestHandle target;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price, 2.0,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [&](Host& base) { target = put(static_cast<TermsHost&>(base), host_open()); };
    run(host, spec("terms-queued-open"), {100});
    completed(host);
    const auto receipt = last_event<no::TermsResolvedEvent>(host);
    const auto applied = last_event<no::ExecutionAppliedEvent>(host);
    REQUIRE(receipt && applied);
    CHECK(receipt->handle() == target);
    CHECK(receipt->input.terms.units == 2.0);
    CHECK(receipt->input.terms.shape == no::OpeningShape::Transact);
    CHECK(!receipt->input.shared_cursor_collision);
    CHECK(receipt->ordinal < applied->ordinal);
    CHECK(applied->opened_units == 2.0);
    CHECK(host.resolver_calls == 1);
    CHECK(host.validator_calls == 1);
    REQUIRE(!host.resolved_facts.empty());
    CHECK(std::holds_alternative<no::RemainingDeferred>(host.resolved_facts.front().remaining));
    CHECK(std::holds_alternative<no::AllowanceDeferred>(host.resolved_facts.front().allowance));
}

void zero_terms_are_a_receipt_then_no_effect() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price,
                                   from_bits(0x8000000000000000ULL),
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), host_open()); };
    run(host, spec("terms-zero"), {100});
    completed(host);
    const auto receipt = last_event<no::TermsResolvedEvent>(host);
    const auto no_effect = last_event<no::NoEffectEvent>(host);
    REQUIRE(receipt && no_effect);
    REQUIRE(receipt->input.terms.units);
    CHECK(bits(*receipt->input.terms.units) == 0x8000000000000000ULL);
    CHECK(receipt->ordinal + 1 == no_effect->ordinal);
    CHECK(host.rows().empty());
    CHECK(accounts(host) == 0);
    CHECK(host.validator_calls == 0);
}

void overridden_price_is_durable_before_settlement() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price + 1.0, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(host, spec("terms-price"), {100});
    completed(host);
    const auto receipt = last_event<no::TermsResolvedEvent>(host);
    const auto applied = last_event<no::ExecutionAppliedEvent>(host);
    REQUIRE(receipt && applied);
    CHECK(receipt->input.raw_price == 100.0);
    CHECK(receipt->input.default_resolved_price == 100.0);
    CHECK(receipt->input.terms.resolved_price == 101.0);
    CHECK(applied->resolved_price == 101.0);
}

void current_preview_has_typed_terms_outcomes_without_writes() {
    TermsHost host;
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto unresolved = put(h, host_open());
        const auto before = h.native_continuation_hash();
        const auto preview = h.inspect_current_execution(command(unresolved));
        CHECK(!preview.refusal);
        CHECK(preview.terms_rejection == no::MatchRejectReason::TermsUnresolved);
        CHECK(!preview.terms_cancellation);
        CHECK(!preview.settlement_readiness);
        CHECK(h.native_continuation_hash() == before);
        h.cancel(unresolved);

        h.resolver = [](const NativeExecutionTermsFacts& facts) {
            return no::ExecutionTerms{facts.default_resolved_price, 0.0,
                                       no::OpeningShape::Transact};
        };
        const auto zero = put(h, host_open());
        const auto zero_before = h.native_continuation_hash();
        const auto zero_preview = h.inspect_current_execution(command(zero));
        CHECK(!zero_preview.refusal);
        CHECK(zero_preview.settlement_readiness == execution::Status::NoEffect);
        CHECK(!zero_preview.terms_rejection && !zero_preview.terms_cancellation);
        CHECK(h.native_continuation_hash() == zero_before);
        const auto outcome = h.execute_current(command(zero));
        CHECK(std::holds_alternative<no::NoEffectEvent>(outcome));
        reached = true;
    };
    run(host, spec("terms-preview"), {100});
    completed(host);
    CHECK(reached);
}

void current_group_deduction_returns_cancelled() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = tx(1, "group-first");
        first.group = no::Member{7, 1, no::GroupEffect::Reduce};
        auto second = host_open(no::Side::Long, "group-second");
        second.group = no::Member{7, 2, no::GroupEffect::Reduce};
        const auto first_target = put(h, first);
        const auto second_target = put(h, second);
        (void)apply(h, first_target);
        const auto deferred = last_event<no::DeferredGroupAdjustmentEvent>(h);
        REQUIRE(deferred);
        CHECK(deferred->recipient == second_target);
        CHECK(deferred->pending_after.total == 1.0);
        const auto before = h.native_continuation_hash();
        const auto preview = h.inspect_current_execution(command(second_target));
        CHECK(!preview.refusal);
        CHECK(preview.terms_cancellation == no::CancelReason::Group);
        CHECK(!preview.settlement_readiness);
        CHECK(h.native_continuation_hash() == before);
        const auto result = h.execute_current(command(second_target));
        REQUIRE(std::holds_alternative<no::CancelledEvent>(result));
        const auto& cancelled = std::get<no::CancelledEvent>(result);
        CHECK(cancelled.reason == no::CancelReason::Group);
        const auto receipt = last_event<no::TermsResolvedEvent>(h);
        REQUIRE(receipt && cancelled.cause);
        CHECK(receipt->ordinal + 1 == cancelled.ordinal);
        CHECK(cancelled.cause->ordinal == receipt->ordinal);
        reached = true;
    };
    run(host, spec("terms-group-cancel"), {100});
    completed(host);
    CHECK(reached);
}

void host_nan_price_is_rejected_without_a_receipt() {
    TermsHost host;
    const double payload = from_bits(0x7ff8000000000001ULL);
    host.resolver = [payload](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{payload, std::nullopt, no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(host, spec("terms-nan"), {100});
    completed(host);
    const auto rejected = last_event<no::MatchRejectedEvent>(host);
    REQUIRE(rejected && rejected->attempted_terms);
    CHECK(rejected->reason == no::MatchRejectReason::NonpositivePrice);
    CHECK(bits(rejected->attempted_terms->resolved_price) == 0x7ff8000000000001ULL);
    CHECK(events<no::TermsResolvedEvent>(host).empty());
    CHECK(host.lots().empty());
}

void host_price_cannot_breach_active_limit() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price + 1.0, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        auto request = tx(1);
        request.trigger = no::Limit{100.0};
        put(static_cast<TermsHost&>(base), request);
    };
    run(host, spec("terms-limit-fence"), {100});
    completed(host);
    const auto rejected = last_event<no::MatchRejectedEvent>(host);
    REQUIRE(rejected && rejected->attempted_terms);
    CHECK(rejected->reason == no::MatchRejectReason::InvalidTerms);
    CHECK(rejected->attempted_terms->resolved_price == 101.0);
    CHECK(events<no::TermsResolvedEvent>(host).empty());
}

void current_preview_and_execute_see_same_terms_facts() {
    TermsHost host;
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, tx(1));
        const auto preview = h.inspect_current_execution(command(target, NativeCurrentPriceRule::NearestTick));
        CHECK(!preview.refusal);
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(
            h.execute_current(command(target, NativeCurrentPriceRule::NearestTick))));
        REQUIRE(h.resolved_facts.size() == 2);
        const auto& preview_facts = h.resolved_facts[0];
        const auto& execute_facts = h.resolved_facts[1];
        CHECK(preview_facts.target == execute_facts.target);
        CHECK(preview_facts.definition == execute_facts.definition);
        CHECK(bits(preview_facts.cursor.t) == bits(execute_facts.cursor.t));
        CHECK(preview_facts.cursor.point.ordinal == execute_facts.cursor.point.ordinal);
        CHECK(preview_facts.price_kind == execute_facts.price_kind);
        CHECK(preview_facts.quote_kind == execute_facts.quote_kind);
        CHECK(preview_facts.price_rule == NativeCurrentPriceRule::NearestTick);
        CHECK(execute_facts.price_rule == NativeCurrentPriceRule::NearestTick);
        CHECK(bits(preview_facts.raw_price) == bits(execute_facts.raw_price));
        CHECK(bits(preview_facts.default_resolved_price) == bits(execute_facts.default_resolved_price));
        CHECK(preview_facts.allowance.index() == execute_facts.allowance.index());
        reached = true;
    };
    run(host, spec("terms-current-facts"), {100.04});
    completed(host);
    CHECK(reached);
}

void rounded_crossing_collision_keeps_request_origin() {
    auto execute = [](TermsHost& host, const char* key) {
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            const bool older = facts.definition && facts.definition->request.label == "older";
            return no::ExecutionTerms{facts.default_resolved_price, older ? 0.0 : 1.0,
                                       no::OpeningShape::Transact};
        };
        host.beginning = [](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            auto older = host_open(no::Side::Short, "older");
            older.trigger = no::Limit{2.0};
            auto younger = host_open(no::Side::Short, "younger");
            younger.trigger = no::Limit{1.9999999999999998};
            put(h, older);
            put(h, younger);
        };
        const auto configuration = spec(key);
        REQUIRE(host.configure_native(configuration).status == NativeSetupStatus::Applied);
        const Bar bar{0.25, 3.5, 0.25, 3.5, 1.0, T};
        host.run(&bar, 1);
        completed(host);
    };
    TermsHost first;
    TermsHost second;
    execute(first, "terms-collision");
    execute(second, "terms-collision");
    const auto receipts = events<no::TermsResolvedEvent>(first);
    REQUIRE(receipts.size() == 2);
    const auto& younger = receipts.back();
    CHECK(younger.input.price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    CHECK(younger.input.shared_cursor_collision);
    CHECK(bits(younger.input.raw_price) == bits(2.0));
    CHECK(bits(younger.input.terms.resolved_price) == bits(2.0));
    const auto younger_facts = std::find_if(first.resolved_facts.begin(), first.resolved_facts.end(),
        [](const NativeExecutionTermsFacts& facts) {
            return facts.definition && facts.definition->request.label == "younger";
        });
    REQUIRE(younger_facts != first.resolved_facts.end());
    CHECK(younger_facts->price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    CHECK(younger_facts->shared_cursor_collision);
    REQUIRE(younger_facts->trigger_level);
    CHECK(bits(*younger_facts->trigger_level) == bits(1.9999999999999998));
    CHECK(bits(younger_facts->raw_price) == bits(2.0));
    CHECK(first.native_continuation_hash() != 0);
    CHECK(first.native_continuation_hash() == second.native_continuation_hash());
}

}  // namespace

int main() {
    test("private binary64 helper", bit_helper_controls);
    test("test-only facts oracle", test_only_oracle_host_uses_facts);
    test("queued host-sized opening", queued_host_sized_open);
    test("zero host-sized receipt", zero_terms_are_a_receipt_then_no_effect);
    test("queued price receipt", overridden_price_is_durable_before_settlement);
    test("current typed preview outcomes", current_preview_has_typed_terms_outcomes_without_writes);
    test("current deferred group cancellation", current_group_deduction_returns_cancelled);
    test("host NaN attempted terms", host_nan_price_is_rejected_without_a_receipt);
    test("host price limit fence", host_price_cannot_breach_active_limit);
    test("current preview/execute terms facts", current_preview_and_execute_see_same_terms_facts);
    test("rounded crossing provenance collision", rounded_crossing_collision_keeps_request_origin);
    std::printf("R4-B terms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
