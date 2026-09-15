#include "native_terms_fixture.hpp"
#include "../src/native_matching.hpp"

#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <variant>
#include <vector>

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

bool same_coordinate_bits(const NativeCoordinate& a, const NativeCoordinate& b) {
    return a.ordinal == b.ordinal
        && a.interval_index == b.interval_index
        && a.open_ms == b.open_ms
        && a.eligible_open_ms == b.eligible_open_ms
        && a.last_traded_close_ms == b.last_traded_close_ms
        && a.next_period_open_ms == b.next_period_open_ms
        && a.next_input_open_ms == b.next_input_open_ms
        && a.effective_time_ms == b.effective_time_ms
        && a.source_price_time_ms == b.source_price_time_ms
        && a.provenance == b.provenance
        && a.path_phase == b.path_phase
        && a.completion == b.completion;
}

bool same_trigger_state_bits(const no::TriggerState& a, const no::TriggerState& b) {
    if (a.index() != b.index()) return false;
    if (const auto* left = std::get_if<no::TrailTrack>(&a)) {
        return bits(left->best) == bits(std::get<no::TrailTrack>(b).best);
    }
    if (const auto* left = std::get_if<no::TrailActive>(&a)) {
        return bits(left->best_at_trigger) == bits(std::get<no::TrailActive>(b).best_at_trigger);
    }
    return true;
}

bool same_remaining_bits(const no::Remaining& a, const no::Remaining& b) {
    if (a.index() != b.index()) return false;
    if (const auto* left = std::get_if<no::RemainingUnits>(&a)) {
        return bits(left->q) == bits(std::get<no::RemainingUnits>(b).q);
    }
    return true;
}

bool same_allowance_bits(const no::Allowance& a, const no::Allowance& b) {
    if (a.index() != b.index()) return false;
    if (const auto* left = std::get_if<no::AllowanceUnits>(&a)) {
        const auto& right = std::get<no::AllowanceUnits>(b);
        return left->point_ordinal == right.point_ordinal
            && bits(left->initial) == bits(right.initial)
            && bits(left->left) == bits(right.left);
    }
    if (const auto* left = std::get_if<no::AllowanceAllScope>(&a)) {
        return left->point_ordinal == std::get<no::AllowanceAllScope>(b).point_ordinal;
    }
    if (const auto* left = std::get_if<no::AllowanceDeferred>(&a)) {
        return left->point_ordinal == std::get<no::AllowanceDeferred>(b).point_ordinal;
    }
    return true;
}

bool same_scope_bits(const no::ExecutionScope& a, const no::ExecutionScope& b) {
    if (a.index() != b.index()) return false;
    if (const auto* left = std::get_if<execution::OpeningExposure>(&a)) {
        const auto& right = std::get<execution::OpeningExposure>(b);
        return left->incarnation == right.incarnation && left->cycle == right.cycle;
    }
    if (const auto* left = std::get_if<no::SelectedExposure>(&a)) {
        const auto& right = std::get<no::SelectedExposure>(b);
        return left->cycle == right.cycle && left->incarnations == right.incarnations;
    }
    return true;
}

bool same_optional_double_bits(const std::optional<double>& a, const std::optional<double>& b) {
    return a.has_value() == b.has_value() && (!a || bits(*a) == bits(*b));
}

bool same_terms_facts_bits(const NativeExecutionTermsFacts& a,
                           const NativeExecutionTermsFacts& b) {
    return a.target == b.target
        && a.definition == b.definition
        && same_coordinate_bits(a.cursor.point, b.cursor.point)
        && bits(a.cursor.t) == bits(b.cursor.t)
        && a.driver_class == b.driver_class
        && same_trigger_state_bits(a.trigger_state, b.trigger_state)
        && same_remaining_bits(a.remaining, b.remaining)
        && same_allowance_bits(a.allowance, b.allowance)
        && same_scope_bits(a.scope, b.scope)
        && bits(a.scope_exposure_units) == bits(b.scope_exposure_units)
        && bits(a.position.signed_units) == bits(b.position.signed_units)
        && bits(a.position.average_price) == bits(b.position.average_price)
        && a.position.lot_count == b.position.lot_count
        && bits(a.opposite_book_units) == bits(b.opposite_book_units)
        && a.is_buy == b.is_buy
        && a.price_kind == b.price_kind
        && a.shared_cursor_collision == b.shared_cursor_collision
        && bits(a.raw_price) == bits(b.raw_price)
        && same_optional_double_bits(a.trigger_level, b.trigger_level)
        && a.quote_kind == b.quote_kind
        && a.price_rule == b.price_rule
        && bits(a.default_resolved_price) == bits(b.default_resolved_price)
        && a.fx_effective_time_ms == b.fx_effective_time_ms
        && bits(a.active_fx) == bits(b.active_fx)
        && bits(a.pending_group_deduction) == bits(b.pending_group_deduction);
}

const NativeExecutionTermsFacts* facts_for(const TermsHost& host, const char* label) {
    const auto found = std::find_if(host.resolved_facts.begin(), host.resolved_facts.end(),
        [label](const NativeExecutionTermsFacts& facts) {
            return facts.definition && facts.definition->request.label == label;
        });
    return found == host.resolved_facts.end() ? nullptr : &*found;
}

void check_trigger_origin(const NativeExecutionTermsFacts& facts,
                          no::NativeCandidatePriceKind kind, double raw, double level,
                          double t) {
    CHECK(facts.price_kind == kind);
    REQUIRE(facts.trigger_level);
    CHECK(bits(facts.raw_price) == bits(raw));
    CHECK(bits(*facts.trigger_level) == bits(level));
    CHECK(bits(facts.cursor.t) == bits(t));
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

void zero_units_ignore_nonpositive_price_on_both_paths() {
    auto configure = [](TermsHost& host) {
        host.resolver = [](const NativeExecutionTermsFacts&) {
            return no::ExecutionTerms{0.0, from_bits(0x8000000000000000ULL),
                                       no::OpeningShape::Transact};
        };
    };
    TermsHost queued;
    configure(queued);
    queued.beginning = [](Host& base) {
        put(static_cast<TermsHost&>(base), host_open(no::Side::Long, "queued-zero-price"));
    };
    run(queued, spec("zero-nonpositive-queued"), {100});
    completed(queued);
    const auto queued_receipt = last_event<no::TermsResolvedEvent>(queued);
    const auto queued_terminal = last_event<no::NoEffectEvent>(queued);
    REQUIRE(queued_receipt && queued_terminal);
    REQUIRE(queued_receipt->input.terms.units);
    CHECK(bits(*queued_receipt->input.terms.units) == 0x8000000000000000ULL);
    CHECK(queued_terminal->ordinal == queued_receipt->ordinal + 1);
    CHECK(queued.lots().empty() && queued.rows().empty() && accounts(queued) == 0);
    const auto queued_hash = queued.native_continuation_hash();
    CHECK(queued.native_continuation_hash() == queued_hash);

    TermsHost queued_limit;
    configure(queued_limit);
    queued_limit.beginning = [](Host& base) {
        auto request = host_open(no::Side::Short, "queued-zero-limit");
        request.trigger = no::Limit{100.0};
        put(static_cast<TermsHost&>(base), request);
    };
    run(queued_limit, spec("zero-nonpositive-limit"), {100});
    completed(queued_limit);
    CHECK(last_event<no::TermsResolvedEvent>(queued_limit).has_value());
    CHECK(last_event<no::NoEffectEvent>(queued_limit).has_value());
    CHECK(events<no::MatchRejectedEvent>(queued_limit).empty());

    TermsHost current;
    configure(current);
    bool reached = false;
    current.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, host_open(no::Side::Long, "current-zero-price"));
        const auto result = h.execute_current(command(target));
        REQUIRE(std::holds_alternative<no::NoEffectEvent>(result));
        const auto receipt = last_event<no::TermsResolvedEvent>(h);
        const auto terminal = last_event<no::NoEffectEvent>(h);
        REQUIRE(receipt && terminal && receipt->input.terms.units);
        CHECK(bits(*receipt->input.terms.units) == 0x8000000000000000ULL);
        CHECK(terminal->ordinal == receipt->ordinal + 1);
        CHECK(h.lots().empty() && h.rows().empty() && accounts(h) == 0);
        const auto hash = h.native_continuation_hash();
        CHECK(h.native_continuation_hash() == hash);
        reached = true;
    };
    run(current, spec("zero-nonpositive-current"), {100});
    completed(current);
    CHECK(reached);
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

void a_t4c_nonpositive_group_terminal_matches_preview() {
    auto configure = [](TermsHost& host) {
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
                return no::ExecutionTerms{-1.0, 1.0, no::OpeningShape::Transact};
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::Transact};
        };
    };

    TermsHost current;
    configure(current);
    bool current_reached = false;
    current.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = tx(1.0, "current-nonpositive-group-a");
        first.group = no::Member{62, 1, no::GroupEffect::Reduce};
        auto second = host_open(no::Side::Long, "current-nonpositive-group-b");
        second.group = no::Member{62, 2, no::GroupEffect::Reduce};
        const auto first_target = put(h, first);
        const auto second_target = put(h, second);
        (void)apply(h, first_target);
        const auto deferred = last_event<no::DeferredGroupAdjustmentEvent>(h);
        REQUIRE(deferred);
        CHECK(deferred->recipient == second_target);
        CHECK(bits(deferred->pending_after.total) == bits(1.0));

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
        REQUIRE(receipt && receipt->input.terms.units);
        CHECK(bits(receipt->input.terms.resolved_price) == bits(-1.0));
        CHECK(bits(*receipt->input.terms.units) == bits(1.0));
        CHECK(cancelled.cause && cancelled.cause->ordinal == receipt->ordinal);
        CHECK(cancelled.ordinal == receipt->ordinal + 1);
        CHECK(h.native_state().kind == NativeLifecycleKind::Running);
        (void)put(h, tx(1.0, "current-nonpositive-group-after"));
        current_reached = true;
    };
    run(current, spec("current-nonpositive-group"), {100.0});
    completed(current);
    CHECK(current_reached);

    TermsHost queued;
    configure(queued);
    queued.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = tx(1.0, "queued-nonpositive-group-a");
        first.group = no::Member{63, 1, no::GroupEffect::Reduce};
        auto second = host_open(no::Side::Long, "queued-nonpositive-group-b");
        second.group = no::Member{63, 2, no::GroupEffect::Reduce};
        put(h, first);
        put(h, second);
    };
    run(queued, spec("queued-nonpositive-group"), {100.0});
    completed(queued);
    const auto rejection = last_event<no::MatchRejectedEvent>(queued);
    REQUIRE(rejection && rejection->attempted_terms);
    CHECK(rejection->request().label == "queued-nonpositive-group-b");
    CHECK(rejection->reason == no::MatchRejectReason::NonpositivePrice);
    CHECK(bits(rejection->attempted_terms->resolved_price) == bits(-1.0));
    REQUIRE(rejection->attempted_terms->units);
    CHECK(bits(*rejection->attempted_terms->units) == bits(1.0));
    CHECK(events<no::TermsResolvedEvent>(queued).empty());
}

void a_t4b_queued_group_cancellation_is_typed_terminal() {
    TermsHost host;
    no::RequestHandle second;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = tx(1, "queued-group-a");
        first.group = no::Member{51, 1, no::GroupEffect::Reduce};
        auto deferred = host_open(no::Side::Long, "queued-group-b");
        deferred.group = no::Member{51, 2, no::GroupEffect::Reduce};
        put(h, first);
        second = put(h, deferred);
    };
    run(host, spec("terms-queued-group"), {100});
    completed(host);
    const auto adjustment = last_event<no::DeferredGroupAdjustmentEvent>(host);
    const auto receipt = last_event<no::TermsResolvedEvent>(host);
    const auto cancelled = last_event<no::CancelledEvent>(host);
    REQUIRE(adjustment && receipt && cancelled);
    CHECK(adjustment->recipient == second);
    CHECK(bits(adjustment->pending_after.total) == bits(1.0));
    CHECK(receipt->handle() == second);
    CHECK(cancelled->handle() == second);
    CHECK(cancelled->reason == no::CancelReason::Group);
    REQUIRE(cancelled->cause);
    CHECK(cancelled->cause->ordinal == receipt->ordinal);
    CHECK(cancelled->ordinal == receipt->ordinal + 1);
    CHECK(events<no::ExecutionAppliedEvent>(host).size() == 1);
    CHECK(accounts(host) == 1);
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

void a_t11d_host_sized_nan_rows() {
    constexpr std::uint64_t payload_bits = 0x7ff8000000000002ULL;
    const double payload = from_bits(payload_bits);

    TermsHost queued;
    queued.resolver = [payload](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return no::ExecutionTerms{payload, 1.0, no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    queued.beginning = [](Host& base) {
        put(static_cast<TermsHost&>(base), host_open(no::Side::Long, "nan-host-queued"));
    };
    run(queued, spec("nan-host-queued"), {100.0});
    completed(queued);
    const auto rejected = last_event<no::MatchRejectedEvent>(queued);
    REQUIRE(rejected && rejected->attempted_terms);
    CHECK(rejected->reason == no::MatchRejectReason::NonpositivePrice);
    CHECK(bits(rejected->attempted_terms->resolved_price) == payload_bits);
    REQUIRE(rejected->attempted_terms->units);
    CHECK(bits(*rejected->attempted_terms->units) == bits(1.0));
    CHECK(rejected->attempted_terms->shape == no::OpeningShape::Transact);
    CHECK(events<no::TermsResolvedEvent>(queued).empty());
    CHECK(queued.lots().empty() && queued.rows().empty());

    TermsHost current;
    current.resolver = [payload](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return no::ExecutionTerms{payload, 1.0, no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool current_reached = false;
    current.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, host_open(no::Side::Long, "nan-host-current"));
        const auto history = h.native_events(0).size();
        const auto preview = h.inspect_current_execution(command(target));
        CHECK(!preview.refusal);
        CHECK(preview.settlement_readiness == execution::Status::InvalidPrice);
        CHECK(h.native_events(0).size() == history);
        CHECK(events<no::TermsResolvedEvent>(h).empty());
        bool threw = false;
        try {
            (void)h.execute_current(command(target));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "native current execution failed";
        }
        CHECK(threw);
        CHECK(h.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(h.native_state().failure.code == NativeFailureCode::SettlementFailure);
        CHECK(events<no::TermsResolvedEvent>(h).empty());
        CHECK(h.lots().empty() && h.rows().empty());
        current_reached = true;
    };
    run(current, spec("nan-host-current"), {100.0});
    CHECK(current_reached);
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

void a_t4d_authenticated_unrepresentable_deduction() {
    auto configure_terms = [](TermsHost& host) {
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
                return no::ExecutionTerms{facts.default_resolved_price, 1e16,
                                           no::OpeningShape::Transact};
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::Transact};
        };
    };

    // The queued path obtains its error only through prepare_terms after an
    // authentic deferred-group receipt has been installed.
    TermsHost queued;
    configure_terms(queued);
    no::RequestHandle queued_b;
    queued.beginning = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto a = tx(0.1, "queued-a");
        a.group = no::Member{41, 1, no::GroupEffect::Reduce};
        auto b = host_open(no::Side::Long, "queued-b");
        b.group = no::Member{41, 2, no::GroupEffect::Reduce};
        put(h, a);
        queued_b = put(h, b);
    };
    run(queued, spec("terms-numeric-queued"), {100});
    CHECK(queued.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(queued.native_state().failure.code == NativeFailureCode::SettlementFailure);
    CHECK(queued.native_state().failure.discriminator
          == static_cast<std::uint32_t>(no::CoreFailure::UnrepresentableReservation));
    const auto deferred = last_event<no::DeferredGroupAdjustmentEvent>(queued);
    REQUIRE(deferred);
    CHECK(deferred->recipient == queued_b);
    CHECK(bits(deferred->pending_after.total) == bits(0.1));
    CHECK(events<no::TermsResolvedEvent>(queued).empty());
    CHECK(events<no::ExecutionAppliedEvent>(queued).size() == 1);
    CHECK(accounts(queued) == 1);

    // The current preview throws unlatched, then execution follows the same
    // hard-failure mapping and leaves the pending chain intact.
    TermsHost current;
    configure_terms(current);
    bool reached = false;
    current.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto a = tx(0.1, "current-a");
        a.group = no::Member{42, 1, no::GroupEffect::Reduce};
        auto b = host_open(no::Side::Long, "current-b");
        b.group = no::Member{42, 2, no::GroupEffect::Reduce};
        const auto a_target = put(h, a);
        const auto b_target = put(h, b);
        (void)apply(h, a_target);
        const auto pending = last_event<no::DeferredGroupAdjustmentEvent>(h);
        REQUIRE(pending);
        CHECK(bits(pending->pending_after.total) == bits(0.1));
        const auto hash = h.native_continuation_hash();
        bool preview_threw = false;
        try {
            (void)h.inspect_current_execution(command(b_target));
        } catch (const std::overflow_error& error) {
            preview_threw = std::string(error.what())
                == "native host-sized deduction is not representable";
        }
        CHECK(preview_threw);
        CHECK(h.native_state().kind == NativeLifecycleKind::Running);
        CHECK(h.native_continuation_hash() == hash);
        CHECK(h.validator_calls == 1);
        bool execute_threw = false;
        try {
            (void)h.execute_current(command(b_target));
        } catch (const std::runtime_error& error) {
            execute_threw = std::string(error.what()) == "native current execution failed";
        }
        CHECK(execute_threw);
        CHECK(h.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(h.native_state().failure.code == NativeFailureCode::SettlementFailure);
        CHECK(h.native_state().failure.discriminator
              == static_cast<std::uint32_t>(no::CoreFailure::UnrepresentableReservation));
        CHECK(h.validator_calls == 1);
        reached = true;
    };
    run(current, spec("terms-numeric-current"), {100});
    CHECK(reached);
}

void a_t5_terms_rejections_retain_attempted_terms() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        const auto& label = facts.definition->request.label;
        if (label == "unresolved") return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                                                no::OpeningShape::Transact};
        if (label == "negative") return no::ExecutionTerms{facts.default_resolved_price, -1.0,
                                                              no::OpeningShape::Transact};
        if (label == "nan-units") return no::ExecutionTerms{facts.default_resolved_price,
            from_bits(0x7ff8000000000002ULL), no::OpeningShape::Transact};
        if (label == "off-grid") return no::ExecutionTerms{facts.default_resolved_price, 0.3,
                                                              no::OpeningShape::Transact};
        if (label == "explicit-units") return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                                                    no::OpeningShape::Transact};
        if (label == "explicit-shape") return no::ExecutionTerms{facts.default_resolved_price,
                                                                    std::nullopt,
                                                                    no::OpeningShape::ReverseTo};
        if (label == "limit-breach") return no::ExecutionTerms{101.0, std::nullopt,
                                                                  no::OpeningShape::Transact};
        if (label == "no-opposite") return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                                                 no::OpeningShape::ReverseTo};
        if (label == "bad-close-shape") return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                                                     no::OpeningShape::CloseOpposite};
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        put(h, host_open(no::Side::Long, "unresolved"));
        put(h, host_open(no::Side::Long, "negative"));
        put(h, host_open(no::Side::Long, "nan-units"));
        put(h, host_open(no::Side::Long, "off-grid"));
        put(h, tx(1, "explicit-units"));
        put(h, tx(1, "explicit-shape"));
        auto limit = tx(1, "limit-breach");
        limit.trigger = no::Limit{100};
        put(h, limit);
        put(h, host_open(no::Side::Long, "no-opposite"));
    };
    auto configuration = spec("terms-rejections");
    configuration.quantity_grid = 0.5;
    run(host, configuration, {100});
    completed(host);
    const auto rejected = events<no::MatchRejectedEvent>(host);
    CHECK(rejected.size() == 8);
    for (const auto& event : rejected) {
        CHECK(event.attempted_terms.has_value());
        REQUIRE(event.attempted_terms);
        if (event.request().label == "unresolved") {
            CHECK(event.reason == no::MatchRejectReason::TermsUnresolved);
        } else if (event.request().label == "no-opposite") {
            CHECK(event.reason == no::MatchRejectReason::NoOppositeExposure);
        } else {
            CHECK(event.reason == no::MatchRejectReason::InvalidTerms);
        }
    }
    CHECK(host.lots().empty());
    CHECK(events<no::TermsResolvedEvent>(host).empty());

    TermsHost close_shape;
    close_shape.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (facts.definition->request.label == "bad-close-shape") {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::CloseOpposite};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    close_shape.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        put(h, tx(1, "seed"));
        put(h, host_close("bad-close-shape"));
    };
    run(close_shape, spec("terms-close-shape"), {100});
    completed(close_shape);
    const auto close_rejected = last_event<no::MatchRejectedEvent>(close_shape);
    REQUIRE(close_rejected && close_rejected->attempted_terms);
    CHECK(close_rejected->request().label == "bad-close-shape");
    CHECK(close_rejected->reason == no::MatchRejectReason::InvalidTerms);
}

void a_t5_bound_host_sized_rematch_rejects_units() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        const bool binding = std::holds_alternative<no::RemainingDeferred>(facts.remaining);
        return no::ExecutionTerms{facts.default_resolved_price,
                                   binding ? std::optional<double>{2.0}
                                           : std::optional<double>{1.0},
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        auto request = host_open(no::Side::Long, "bound-rematch-units");
        request.capacity = no::PointBudget{1.0};
        put(static_cast<TermsHost&>(base), request);
    };
    run(host, spec("bound-rematch-units"), {100.0, 100.0});
    completed(host);
    const auto applied = events<no::ExecutionAppliedEvent>(host);
    REQUIRE(applied.size() == 1);
    CHECK(bits(applied.front().opened_units) == bits(1.0));
    const auto rejected = last_event<no::MatchRejectedEvent>(host);
    REQUIRE(rejected && rejected->attempted_terms);
    CHECK(rejected->request().label == "bound-rematch-units");
    CHECK(rejected->reason == no::MatchRejectReason::InvalidTerms);
    REQUIRE(rejected->attempted_terms->units);
    CHECK(bits(*rejected->attempted_terms->units) == bits(1.0));
    CHECK(rejected->attempted_terms->shape == no::OpeningShape::Transact);
    const auto receipts = events<no::TermsResolvedEvent>(host);
    REQUIRE(receipts.size() == 1);
    CHECK(receipts.front().input.terms.units);
    CHECK(bits(*receipts.front().input.terms.units) == bits(2.0));
}

void explicit_reduction_grid_policy_both_ways() {
    struct Summary {
        double position = 0.0;
        std::vector<no::ExecutionAppliedEvent> applied;
        std::vector<no::MatchRejectedEvent> rejected;
    };
    const auto run_case = [](no::ExecutionGridPolicy policy, double units,
                             const char* key) {
        TermsHost host;
        host.resolver = [policy, units](const NativeExecutionTermsFacts& facts) {
            if (facts.definition->request.label == "half-close") {
                no::ExecutionTerms terms{facts.default_resolved_price, units,
                                         no::OpeningShape::Transact};
                terms.grid_policy = policy;
                return terms;
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                      no::OpeningShape::Transact};
        };
        host.beginning = [](Host& base) {
            auto& current = static_cast<TermsHost&>(base);
            put(current, tx(1.0, "seed"));
            put(current, host_close("half-close"));
        };
        auto configuration = spec(key);
        configuration.quantity_grid = 1.0;
        run(host, configuration, {100.0});
        completed(host);
        return Summary{host.physical_position().signed_units,
                       events<no::ExecutionAppliedEvent>(host),
                       events<no::MatchRejectedEvent>(host)};
    };

    const auto explicit_units = run_case(
        no::ExecutionGridPolicy::ExplicitUnits, 0.5, "terms-explicit-units-grid");
    CHECK(bits(explicit_units.position) == bits(0.5));
    REQUIRE(explicit_units.applied.size() == 2);
    CHECK(explicit_units.applied.back().request().label == "half-close");
    CHECK(bits(explicit_units.applied.back().closed_units) == bits(0.5));
    CHECK(explicit_units.rejected.empty());

    const auto snap_to_grid = run_case(
        no::ExecutionGridPolicy::SnapToGrid, 0.5, "terms-snap-to-grid");
    CHECK(bits(snap_to_grid.position) == bits(1.0));
    REQUIRE(snap_to_grid.rejected.size() == 1);
    CHECK(snap_to_grid.rejected.front().request().label == "half-close");
    CHECK(snap_to_grid.rejected.front().reason == no::MatchRejectReason::InvalidTerms);
    REQUIRE(snap_to_grid.rejected.front().attempted_terms);
    CHECK(snap_to_grid.rejected.front().attempted_terms->grid_policy
          == no::ExecutionGridPolicy::SnapToGrid);

    const auto over_exposure = run_case(
        no::ExecutionGridPolicy::ExplicitUnits, 1.5, "terms-explicit-over-exposure");
    CHECK(bits(over_exposure.position) == bits(1.0));
    REQUIRE(over_exposure.rejected.size() == 1);
    CHECK(over_exposure.rejected.front().reason == no::MatchRejectReason::InvalidTerms);
}

void a_t10_callback_exception_mapping() {
    TermsHost queued;
    queued.resolver = [](const NativeExecutionTermsFacts&) -> no::ExecutionTerms {
        throw std::runtime_error("queued resolver exception");
    };
    queued.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1)); };
    run(queued, spec("terms-callback-queued"), {100});
    CHECK(queued.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(queued.native_state().failure.code == NativeFailureCode::CallbackException);
    CHECK(queued.rows().empty() && queued.lots().empty());

    TermsHost current;
    current.resolver = [](const NativeExecutionTermsFacts&) -> no::ExecutionTerms {
        throw std::runtime_error("current resolver exception");
    };
    bool reached = false;
    current.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, tx(1));
        bool threw = false;
        try {
            (void)h.execute_current(command(target));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(h.native_state().failure.code == NativeFailureCode::CallbackException);
        CHECK(h.rows().empty() && h.lots().empty());
        reached = true;
    };
    run(current, spec("terms-callback-current"), {100});
    CHECK(reached);
}

void a_t7_scope_facts_and_flat_close_shortcut() {
    TermsHost host;
    std::vector<NativeExecutionTermsFacts> close_facts;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            close_facts.push_back(facts);
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto opening = put(h, tx(3, "scope-opening"));
        (void)apply(h, opening);
        const auto book = put(h, host_close("book-close"));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(h.execute_current(command(book))));
        REQUIRE(!close_facts.empty());
        CHECK(std::holds_alternative<execution::Book>(close_facts.back().scope));
        CHECK(close_facts.back().scope_exposure_units == 3.0);
        CHECK(close_facts.back().opposite_book_units == 3.0);

        auto child = host_close("opening-close");
        child.owner = no::BindOpening{opening, h.cycle()};
        const auto child_handle = put(h, child);
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(
            h.execute_current(command(child_handle))));
        REQUIRE(close_facts.size() >= 2);
        CHECK(std::holds_alternative<execution::OpeningExposure>(close_facts.back().scope));
        CHECK(close_facts.back().scope_exposure_units == 2.0);
        reached = true;
    };
    run(host, spec("terms-scope"), {100});
    completed(host);
    CHECK(reached);

    TermsHost selected;
    std::optional<NativeExecutionTermsFacts> selected_facts;
    no::RequestHandle selected_opening_b;
    selected.resolver = [&](const NativeExecutionTermsFacts& facts) {
        if (facts.definition->request.label == "selected-host-close") {
            selected_facts = facts;
            return no::ExecutionTerms{facts.default_resolved_price, facts.scope_exposure_units,
                                       no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool selected_reached = false;
    selected.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto opening_a = put(h, tx(1.0, "selected-opening-a"));
        (void)apply(h, opening_a);
        selected_opening_b = put(h, tx(1.0, "selected-opening-b"));
        (void)apply(h, selected_opening_b);
        auto request = host_close("selected-host-close");
        request.owner = no::BindOpenings{{opening_a, selected_opening_b}, h.cycle()};
        const auto target = put(h, request);
        // Bind while both members are live, then externally retire the first
        // one before the selected HostSized close observes its cohort.
        (void)apply(h, put(h, reduce(1.0, "selected-retire-a")));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(
            h.execute_current(command(target))));
        selected_reached = true;
    };
    run(selected, spec("terms-selected-scope"), {100.0});
    completed(selected);
    CHECK(selected_reached);
    REQUIRE(selected_facts);
    REQUIRE(std::holds_alternative<no::SelectedExposure>(selected_facts->scope));
    const auto& scope = std::get<no::SelectedExposure>(selected_facts->scope);
    REQUIRE(scope.incarnations.size() == 1);
    CHECK(scope.incarnations.front() == selected_opening_b.incarnation);
    CHECK(bits(selected_facts->scope_exposure_units) == bits(1.0));
    CHECK(bits(selected_facts->position.signed_units) == bits(1.0));
    CHECK(selected.physical_position().signed_units == 0.0);

    TermsHost flat;
    bool resolver_called = false;
    flat.resolver = [&](const NativeExecutionTermsFacts& facts) {
        resolver_called = true;
        return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                   no::OpeningShape::Transact};
    };
    bool flat_reached = false;
    flat.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, host_close("flat-close"));
        const auto preview = h.inspect_current_execution(command(target));
        CHECK(!preview.refusal && preview.settlement_readiness == execution::Status::NoEffect);
        CHECK(!resolver_called);
        const auto result = h.execute_current(command(target));
        CHECK(std::holds_alternative<no::NoEffectEvent>(result));
        CHECK(!resolver_called);
        flat_reached = true;
    };
    run(flat, spec("terms-flat-close"), {100});
    completed(flat);
    CHECK(flat_reached);
}

void a_t8_point_budget_binding_and_price_rematch() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        const auto* bound = std::get_if<no::RemainingUnits>(&facts.remaining);
        const bool first_rematch = bound && bound->q > 1.0;
        return no::ExecutionTerms{facts.default_resolved_price + (first_rematch ? 1.0 : 0.0),
                                   bound ? std::optional<double>{} : std::optional<double>{2.5},
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        auto request = host_open(no::Side::Long, "budget-open");
        request.capacity = no::PointBudget{1.0};
        put(static_cast<TermsHost&>(base), request);
    };
    run(host, spec("terms-point-budget"), {100, 100, 100});
    completed(host);
    const auto applied = events<no::ExecutionAppliedEvent>(host);
    const auto receipts = events<no::TermsResolvedEvent>(host);
    REQUIRE(applied.size() == 3);
    CHECK(applied[0].opened_units == 1.0 && applied[1].opened_units == 1.0
          && applied[2].opened_units == 0.5);
    REQUIRE(receipts.size() == 2);
    CHECK(receipts[0].input.terms.units == 2.5);
    CHECK(!receipts[1].input.terms.units);
    CHECK(receipts[1].remaining_before.index() == receipts[1].remaining_after.index());
    CHECK(host.resolver_calls == 3);
    CHECK(host.physical_position().signed_units == 2.5);
}

void a_t13_post_binding_no_change_is_contract_failure() {
    TermsHost host;
    bool injected = false;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)
            && std::holds_alternative<no::RemainingDeferred>(facts.remaining)) {
            host.inject_post_binding_no_change(facts.target);
            injected = true;
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        put(static_cast<TermsHost&>(base), host_open(no::Side::Long, "force-nochange"));
    };
    run(host, spec("force-post-binding-nochange"), {100.0});
    CHECK(injected);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::Contract);
    const auto receipts = events<no::TermsResolvedEvent>(host);
    REQUIRE(receipts.size() == 1);
    CHECK(receipts.front().request().label == "force-nochange");
    CHECK(receipts.front().input.terms.units);
    CHECK(bits(*receipts.front().input.terms.units) == bits(1.0));
    CHECK(events<no::ExecutionAppliedEvent>(host).empty());
    CHECK(host.lots().empty() && host.rows().empty() && accounts(host) == 0);
}

void a_t8b_explicit_equivalent_allowance_and_t8c_identity_control() {
    auto run_one = [](bool host_sized) {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
                const auto units = std::holds_alternative<no::RemainingDeferred>(facts.remaining)
                    ? std::optional<double>{2.0} : std::optional<double>{};
                return no::ExecutionTerms{facts.default_resolved_price, units,
                                           no::OpeningShape::Transact};
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::Transact};
        };
        host.beginning = [host_sized](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            auto request = host_sized ? host_open(no::Side::Long, "host") : tx(2, "explicit");
            request.capacity = no::PointBudget{1.0};
            put(h, request);
        };
        run(host, spec(host_sized ? "terms-allowance-host" : "terms-allowance-explicit"),
            {100, 100});
        completed(host);
        return events<no::ExecutionAppliedEvent>(host);
    };
    const auto explicit_rows = run_one(false);
    const auto host_rows = run_one(true);
    REQUIRE(explicit_rows.size() == 2 && host_rows.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(explicit_rows[i].allowance_before.index() == host_rows[i].allowance_before.index());
        CHECK(explicit_rows[i].allowance_after.index() == host_rows[i].allowance_after.index());
        CHECK(explicit_rows[i].filled_working == host_rows[i].filled_working);
    }
}

void a_t11b_queued_notification_uses_final_terms_price() {
    TermsHost host;
    auto configuration = spec("terms-anchor");
    configuration.price_tick = 0.1;
    configuration.slippage_ticks = 1;
    bool observed = false;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (facts.definition->request.label == "anchor") {
            return no::ExecutionTerms{101.0, std::nullopt, no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        auto request = tx(-1, "anchor");
        request.trigger = no::Limit{100.0};
        put(static_cast<TermsHost&>(base), request);
    };
    host.notification = [&](Host& base, const no::ExecutionAppliedEvent& event) {
        auto& h = static_cast<TermsHost&>(base);
        if (event.request().label != "anchor") return;
        const auto point = h.current_execution_point();
        REQUIRE(point);
        CHECK(point->quote_kind == NativeCurrentQuoteKind::ExecutionAnchor);
        CHECK(point->price == 101.0);
        CHECK(point->quote_origin_ordinal == event.ordinal);
        const auto close = put(h, reduce(1, "anchor-close"));
        const auto result = h.execute_current(command(close));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
        const auto& applied = std::get<no::ExecutionAppliedEvent>(result);
        CHECK(applied.raw_price == 101.0);
        CHECK(applied.resolved_price == 101.1);
        observed = true;
    };
    run(host, configuration, {100});
    completed(host);
    CHECK(observed);
    CHECK(host.physical_position().signed_units == 0.0);
}

void a_t11_identity_infinity_and_nan_attempts() {
    for (const double sign : {1.0, -1.0}) {
        TermsHost host;
        auto configuration = spec(sign > 0 ? "terms-inf-buy" : "terms-inf-sell");
        configuration.price_tick = std::numeric_limits<double>::max();
        configuration.slippage_ticks = 2;
        host.beginning = [sign](Host& base) {
            auto request = tx(sign, sign > 0 ? "inf-buy" : "inf-sell");
            request.trigger = no::Limit{100.0};
            put(static_cast<TermsHost&>(base), request);
        };
        run(host, configuration, {100});
        completed(host);
        const auto rejected = last_event<no::MatchRejectedEvent>(host);
        REQUIRE(rejected);
        CHECK(rejected->reason == no::MatchRejectReason::NonpositivePrice);
        CHECK(!rejected->attempted_terms);
        CHECK(events<no::TermsResolvedEvent>(host).empty());
    }
}

void a_t6_matcher_price_kind_all_ten_cases() {
    auto collect = [](const char* key, no::Request request, std::vector<Bar> bars) {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        };
        host.beginning = [request = std::move(request)](Host& base) {
            put(static_cast<TermsHost&>(base), request);
        };
        REQUIRE(host.configure_native(spec(key)).status == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()));
        completed(host);
        return host.resolved_facts;
    };

    auto limit = host_open(no::Side::Short, "limit-cross");
    limit.trigger = no::Limit{100.0};
    const auto crossed_limit = collect("terms-limit-cross", limit,
        {Bar{99, 101, 99, 101, 1, T}});
    REQUIRE(crossed_limit.size() == 1);
    CHECK(crossed_limit[0].price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    REQUIRE(crossed_limit[0].trigger_level);
    CHECK(bits(crossed_limit[0].raw_price) == bits(100.0));
    CHECK(bits(*crossed_limit[0].trigger_level) == bits(100.0));

    auto gap_limit = host_open(no::Side::Short, "limit-gap");
    gap_limit.trigger = no::Limit{100.0};
    const auto gap = collect("terms-limit-gap", gap_limit,
        {Bar{101, 101, 101, 101, 1, T}});
    REQUIRE(gap.size() == 1);
    CHECK(gap[0].price_kind == no::NativeCandidatePriceKind::PointPrice);
    CHECK(bits(gap[0].raw_price) == bits(101.0));
    REQUIRE(gap[0].trigger_level);
    CHECK(bits(*gap[0].trigger_level) == bits(100.0));

    auto exact_gap = host_open(no::Side::Short, "limit-exact-gap");
    exact_gap.trigger = no::Limit{100.0};
    const auto exact = collect("terms-limit-exact-gap", exact_gap,
        {Bar{100, 100, 100, 100, 1, T}});
    REQUIRE(exact.size() == 1);
    CHECK(exact[0].price_kind == no::NativeCandidatePriceKind::PointPrice);
    CHECK(bits(exact[0].raw_price) == bits(100.0));
    REQUIRE(exact[0].trigger_level);
    CHECK(bits(*exact[0].trigger_level) == bits(100.0));

    auto stop = host_open(no::Side::Long, "stop-cross");
    stop.trigger = no::Stop{100.0};
    const auto crossed_stop = collect("terms-stop-cross", stop,
        {Bar{99, 101, 99, 101, 1, T}});
    REQUIRE(crossed_stop.size() == 1);
    CHECK(crossed_stop[0].price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    REQUIRE(crossed_stop[0].trigger_level);
    CHECK(bits(crossed_stop[0].raw_price) == bits(100.0));
    CHECK(bits(*crossed_stop[0].trigger_level) == bits(100.0));

    auto stop_gap = host_open(no::Side::Long, "stop-gap");
    stop_gap.trigger = no::Stop{100.0};
    const auto gap_stop = collect("terms-stop-gap", stop_gap,
        {Bar{101, 101, 101, 101, 1, T}});
    REQUIRE(gap_stop.size() == 1);
    CHECK(gap_stop[0].price_kind == no::NativeCandidatePriceKind::PointPrice);
    CHECK(bits(gap_stop[0].raw_price) == bits(101.0));
    REQUIRE(gap_stop[0].trigger_level);
    CHECK(bits(*gap_stop[0].trigger_level) == bits(100.0));

    TermsHost later_stop;
    later_stop.resolver = [](const NativeExecutionTermsFacts& facts) {
        const bool binding = std::holds_alternative<no::RemainingDeferred>(facts.remaining);
        return no::ExecutionTerms{facts.default_resolved_price,
                                   binding ? std::optional<double>{2.0} : std::optional<double>{},
                                   no::OpeningShape::Transact};
    };
    later_stop.beginning = [](Host& base) {
        auto& host = static_cast<TermsHost&>(base);
        host.seed(-1.0, 100.0, 151);
        auto request = host_open(no::Side::Long, "stop-later-fill");
        request.trigger = no::Stop{100.0};
        request.capacity = no::PointBudget{1.0};
        put(host, request);
    };
    const std::vector<Bar> later_stop_bars{
        Bar{99, 101, 99, 101, 1, T},
        Bar{99, 99, 99, 99, 1, T + 60000},
    };
    REQUIRE(later_stop.configure_native(spec("terms-stop-later")).status
            == NativeSetupStatus::Applied);
    later_stop.run(later_stop_bars.data(), static_cast<int>(later_stop_bars.size()));
    completed(later_stop);
    REQUIRE(later_stop.resolved_facts.size() == 2);
    CHECK(later_stop.resolved_facts[0].price_kind
          == no::NativeCandidatePriceKind::TriggerLevel);
    CHECK(later_stop.resolved_facts[1].price_kind
          == no::NativeCandidatePriceKind::PointPrice);
    REQUIRE(later_stop.resolved_facts[1].trigger_level);
    CHECK(bits(*later_stop.resolved_facts[1].trigger_level) == bits(100.0));

    auto trail = host_open(no::Side::Short, "trail-cross");
    trail.trigger = no::Trail{1.0, std::nullopt};
    const auto crossed_trail = collect("terms-trail-cross", trail,
        {Bar{101, 101, 99, 99, 1, T}});
    REQUIRE(crossed_trail.size() == 1);
    CHECK(crossed_trail[0].price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    REQUIRE(crossed_trail[0].trigger_level);
    CHECK(bits(crossed_trail[0].raw_price) == bits(100.0));
    CHECK(bits(*crossed_trail[0].trigger_level) == bits(100.0));

    auto gap_trail = host_open(no::Side::Short, "trail-gap");
    gap_trail.trigger = no::Trail{1.0, std::nullopt};
    const auto trailed_gap = collect("terms-trail-gap", gap_trail,
        {Bar{101, 101, 101, 101, 1, T}, Bar{99, 99, 99, 99, 1, T + 60000}});
    REQUIRE(trailed_gap.size() == 1);
    CHECK(trailed_gap[0].price_kind == no::NativeCandidatePriceKind::PointPrice);
    REQUIRE(trailed_gap[0].trigger_level);
    CHECK(bits(trailed_gap[0].raw_price) == bits(99.0));
    CHECK(bits(*trailed_gap[0].trigger_level) == bits(100.0));

    auto stop_limit = host_open(no::Side::Long, "stop-limit-inside");
    stop_limit.trigger = no::StopLimit{100.0, 101.0};
    const auto inside_stop_limit = collect("terms-stop-limit-inside", stop_limit,
        {Bar{100, 100, 100, 100, 1, T}});
    REQUIRE(inside_stop_limit.size() == 1);
    CHECK(inside_stop_limit[0].price_kind == no::NativeCandidatePriceKind::PointPrice);
    REQUIRE(inside_stop_limit[0].trigger_level);
    CHECK(bits(inside_stop_limit[0].raw_price) == bits(100.0));
    CHECK(bits(*inside_stop_limit[0].trigger_level) == bits(101.0));

    const auto market = collect("terms-market", host_open(no::Side::Long, "market"),
        {Bar{100, 100, 100, 100, 1, T}});
    REQUIRE(market.size() == 1);
    CHECK(market[0].price_kind == no::NativeCandidatePriceKind::PointPrice);
    CHECK(!market[0].trigger_level);
}

void a_t6c_stop_trail_and_stop_limit_sibling_origins() {
    auto identity_terms = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                   no::OpeningShape::Transact};
    };

    TermsHost stops;
    stops.resolver = identity_terms;
    stops.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = host_open(no::Side::Long, "stop-sibling-first");
        first.trigger = no::Stop{100.0};
        auto second = host_open(no::Side::Long, "stop-sibling-second");
        second.trigger = no::Stop{100.0};
        put(h, first);
        put(h, second);
    };
    const Bar stop_bar{99.0, 101.0, 99.0, 101.0, 1.0, T};
    REQUIRE(stops.configure_native(spec("terms-stop-siblings")).status == NativeSetupStatus::Applied);
    stops.run(&stop_bar, 1);
    completed(stops);
    const auto* stop_first = facts_for(stops, "stop-sibling-first");
    const auto* stop_second = facts_for(stops, "stop-sibling-second");
    REQUIRE(stop_first && stop_second);
    CHECK(std::holds_alternative<no::StopActive>(stop_first->trigger_state));
    CHECK(std::holds_alternative<no::StopActive>(stop_second->trigger_state));
    check_trigger_origin(*stop_first, no::NativeCandidatePriceKind::TriggerLevel,
                         100.0, 100.0, 0.5);
    check_trigger_origin(*stop_second, no::NativeCandidatePriceKind::TriggerLevel,
                         100.0, 100.0, 0.5);
    const auto stop_receipts = events<no::TermsResolvedEvent>(stops);
    REQUIRE(stop_receipts.size() == 2);
    CHECK(stop_receipts[0].request().label == "stop-sibling-first");
    CHECK(stop_receipts[1].request().label == "stop-sibling-second");
    for (const auto& receipt : stop_receipts) {
        CHECK(receipt.input.price_kind == no::NativeCandidatePriceKind::TriggerLevel);
        CHECK(bits(receipt.input.raw_price) == bits(100.0));
        CHECK(bits(receipt.cursor.t) == bits(0.5));
    }
    std::vector<no::ActivatedEvent> stop_activations;
    for (const auto& event : events<no::ActivatedEvent>(stops)) {
        if (event.kind == no::ActivationKind::Stop) stop_activations.push_back(event);
    }
    REQUIRE(stop_activations.size() == 2);
    CHECK(stop_activations[0].definition->request.label == "stop-sibling-first");
    CHECK(stop_activations[1].definition->request.label == "stop-sibling-second");
    CHECK(stop_activations[0].ordinal < stop_receipts[0].ordinal);
    CHECK(stop_receipts[0].ordinal < stop_activations[1].ordinal);
    CHECK(stop_activations[1].ordinal < stop_receipts[1].ordinal);

    TermsHost trails;
    trails.resolver = identity_terms;
    trails.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = host_open(no::Side::Short, "trail-sibling-first");
        first.trigger = no::Trail{1.0, std::nullopt};
        auto second = host_open(no::Side::Short, "trail-sibling-second");
        second.trigger = no::Trail{1.0, std::nullopt};
        put(h, first);
        put(h, second);
    };
    const Bar trail_bar{101.0, 101.0, 99.0, 99.0, 1.0, T};
    REQUIRE(trails.configure_native(spec("terms-trail-siblings")).status == NativeSetupStatus::Applied);
    trails.run(&trail_bar, 1);
    completed(trails);
    const auto* trail_first = facts_for(trails, "trail-sibling-first");
    const auto* trail_second = facts_for(trails, "trail-sibling-second");
    REQUIRE(trail_first && trail_second);
    CHECK(std::holds_alternative<no::TrailActive>(trail_first->trigger_state));
    CHECK(std::holds_alternative<no::TrailActive>(trail_second->trigger_state));
    check_trigger_origin(*trail_first, no::NativeCandidatePriceKind::TriggerLevel,
                         100.0, 100.0, 0.5);
    check_trigger_origin(*trail_second, no::NativeCandidatePriceKind::TriggerLevel,
                         100.0, 100.0, 0.5);
    const auto trail_receipts = events<no::TermsResolvedEvent>(trails);
    REQUIRE(trail_receipts.size() == 2);
    CHECK(trail_receipts[0].request().label == "trail-sibling-first");
    CHECK(trail_receipts[1].request().label == "trail-sibling-second");
    std::vector<no::ActivatedEvent> trail_activations;
    for (const auto& event : events<no::ActivatedEvent>(trails)) {
        if (event.kind == no::ActivationKind::TrailTrigger) trail_activations.push_back(event);
    }
    REQUIRE(trail_activations.size() == 2);
    CHECK(trail_activations[0].definition->request.label == "trail-sibling-first");
    CHECK(trail_activations[1].definition->request.label == "trail-sibling-second");
    CHECK(trail_activations[0].ordinal < trail_receipts[0].ordinal);
    CHECK(trail_receipts[0].ordinal < trail_activations[1].ordinal);
    CHECK(trail_activations[1].ordinal < trail_receipts[1].ordinal);
    for (const auto& receipt : trail_receipts) {
        CHECK(receipt.input.price_kind == no::NativeCandidatePriceKind::TriggerLevel);
        CHECK(bits(receipt.input.raw_price) == bits(100.0));
        CHECK(bits(receipt.cursor.t) == bits(0.5));
    }

    TermsHost stop_limits;
    stop_limits.resolver = identity_terms;
    stop_limits.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = host_open(no::Side::Long, "stop-limit-sibling-first");
        first.trigger = no::StopLimit{100.0, 99.0};
        auto second = host_open(no::Side::Long, "stop-limit-sibling-second");
        second.trigger = no::StopLimit{100.0, 99.0};
        put(h, first);
        put(h, second);
    };
    const std::vector<Bar> stop_limit_bars{
        Bar{99.0, 101.0, 99.0, 101.0, 1.0, T},
        Bar{101.0, 101.0, 98.0, 98.0, 1.0, T + 60000},
    };
    REQUIRE(stop_limits.configure_native(spec("terms-stop-limit-siblings")).status
            == NativeSetupStatus::Applied);
    stop_limits.run(stop_limit_bars.data(), static_cast<int>(stop_limit_bars.size()));
    completed(stop_limits);
    const auto* stop_limit_first = facts_for(stop_limits, "stop-limit-sibling-first");
    const auto* stop_limit_second = facts_for(stop_limits, "stop-limit-sibling-second");
    REQUIRE(stop_limit_first && stop_limit_second);
    CHECK(std::holds_alternative<no::StopLimitLive>(stop_limit_first->trigger_state));
    CHECK(std::holds_alternative<no::StopLimitLive>(stop_limit_second->trigger_state));
    CHECK(stop_limit_first->price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    CHECK(stop_limit_second->price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    REQUIRE(stop_limit_first->trigger_level && stop_limit_second->trigger_level);
    CHECK(bits(stop_limit_first->raw_price) == bits(99.0));
    CHECK(bits(stop_limit_second->raw_price) == bits(99.0));
    CHECK(bits(*stop_limit_first->trigger_level) == bits(99.0));
    CHECK(bits(*stop_limit_second->trigger_level) == bits(99.0));
    CHECK(bits(stop_limit_first->cursor.t) == bits(stop_limit_second->cursor.t));
    CHECK(bits(stop_limit_first->cursor.t) == bits(2.0 / 3.0));
    const auto stop_limit_receipts = events<no::TermsResolvedEvent>(stop_limits);
    REQUIRE(stop_limit_receipts.size() == 2);
    CHECK(stop_limit_receipts[0].request().label == "stop-limit-sibling-first");
    CHECK(stop_limit_receipts[1].request().label == "stop-limit-sibling-second");
    for (const auto& receipt : stop_limit_receipts) {
        CHECK(receipt.input.price_kind == no::NativeCandidatePriceKind::TriggerLevel);
        CHECK(bits(receipt.input.raw_price) == bits(99.0));
        CHECK(bits(receipt.cursor.t) == bits(stop_limit_first->cursor.t));
    }
    std::vector<no::ActivatedEvent> stop_limit_activations;
    for (const auto& event : events<no::ActivatedEvent>(stop_limits)) {
        if (event.kind == no::ActivationKind::StopLimit) stop_limit_activations.push_back(event);
    }
    REQUIRE(stop_limit_activations.size() == 2);
    CHECK(stop_limit_activations[0].definition->request.label == "stop-limit-sibling-first");
    CHECK(stop_limit_activations[1].definition->request.label == "stop-limit-sibling-second");
    CHECK(stop_limit_activations[1].ordinal < stop_limit_receipts[0].ordinal);
    CHECK(stop_limit_receipts[0].ordinal < stop_limit_receipts[1].ordinal);
}

void a_t15_price_boundary_and_a_t16_flat_preview() {
    TermsHost current;
    current.resolver = [](const NativeExecutionTermsFacts& facts) {
        const auto& label = facts.definition->request.label;
        if (label == "pure-close" || label == "would-open") {
            return no::ExecutionTerms{0.0, std::nullopt, no::OpeningShape::Transact};
        }
        if (label == "host-pure-close") {
            return no::ExecutionTerms{0.0, facts.opposite_book_units,
                                       no::OpeningShape::CloseOpposite};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    current.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        (void)apply(h, put(h, tx(1, "seed-close")));
        const auto pure = put(h, reduce(1, "pure-close"));
        const auto pure_result = h.execute_current(command(pure));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(pure_result));
        CHECK(std::get<no::ExecutionAppliedEvent>(pure_result).resolved_price == 0.0);
        CHECK(h.physical_position().signed_units == 0.0);

        (void)apply(h, put(h, tx(1, "seed-host-close")));
        const auto host_close = put(h, host_open(no::Side::Short, "host-pure-close"));
        const auto host_result = h.execute_current(command(host_close));
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(host_result));
        CHECK(std::get<no::ExecutionAppliedEvent>(host_result).resolved_price == 0.0);
        CHECK(h.physical_position().signed_units == 0.0);

        const auto open = put(h, tx(1, "would-open"));
        const auto before_terms = events<no::TermsResolvedEvent>(h).size();
        const auto open_result = h.execute_current(command(open));
        REQUIRE(std::holds_alternative<no::MatchRejectedEvent>(open_result));
        CHECK(std::get<no::MatchRejectedEvent>(open_result).reason
              == no::MatchRejectReason::NonpositivePrice);
        CHECK(events<no::TermsResolvedEvent>(h).size() == before_terms);
        reached = true;
    };
    run(current, spec("terms-current-price-boundary"), {100});
    completed(current);
    CHECK(reached);

    TermsHost queued;
    queued.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{0.0, std::nullopt, no::OpeningShape::Transact};
    };
    queued.beginning = [](Host& base) { put(static_cast<TermsHost&>(base), tx(1, "queued-zero")); };
    run(queued, spec("terms-queued-price-boundary"), {100});
    completed(queued);
    const auto queued_rejection = last_event<no::MatchRejectedEvent>(queued);
    REQUIRE(queued_rejection);
    CHECK(queued_rejection->reason == no::MatchRejectReason::NonpositivePrice);
    CHECK(events<no::TermsResolvedEvent>(queued).empty());

    TermsHost flat;
    bool called = false;
    flat.resolver = [&](const NativeExecutionTermsFacts& facts) {
        called = true;
        return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                   no::OpeningShape::Transact};
    };
    bool flat_reached = false;
    flat.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto target = put(h, host_close("flat-preview"));
        const auto hash = h.native_continuation_hash();
        const auto preview = h.inspect_current_execution(command(target));
        CHECK(!preview.refusal && preview.settlement_readiness == execution::Status::NoEffect);
        CHECK(!called && h.native_continuation_hash() == hash);
        flat_reached = true;
    };
    run(flat, spec("terms-flat-preview"), {100});
    completed(flat);
    CHECK(flat_reached);
}

void a_t14_preview_outcome_table_without_mutation() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        const auto& label = facts.definition->request.label;
        if (label == "negative") return no::ExecutionTerms{facts.default_resolved_price, -1.0,
                                                              no::OpeningShape::Transact};
        if (label == "zero") return no::ExecutionTerms{facts.default_resolved_price, 0.0,
                                                          no::OpeningShape::Transact};
        if (label == "reverse-flat") return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                                                  no::OpeningShape::ReverseTo};
        if (label == "explicit-units-preview") {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        }
        if (label == "positive") return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                                              no::OpeningShape::Transact};
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto inspect = [&](const no::RequestHandle& target) {
            const auto hash = h.native_continuation_hash();
            const auto history = h.native_events(0).size();
            const auto preview = h.inspect_current_execution(command(target));
            CHECK(h.native_continuation_hash() == hash);
            CHECK(h.native_events(0).size() == history);
            CHECK(h.validator_calls == 0);
            return preview;
        };
        const auto unresolved = put(h, host_open(no::Side::Long, "unresolved"));
        CHECK(inspect(unresolved).terms_rejection == no::MatchRejectReason::TermsUnresolved);
        h.cancel(unresolved);
        const auto negative = put(h, host_open(no::Side::Long, "negative"));
        CHECK(inspect(negative).terms_rejection == no::MatchRejectReason::InvalidTerms);
        h.cancel(negative);
        const auto reverse = put(h, host_open(no::Side::Long, "reverse-flat"));
        CHECK(inspect(reverse).terms_rejection == no::MatchRejectReason::NoOppositeExposure);
        h.cancel(reverse);
        const auto zero = put(h, host_open(no::Side::Long, "zero"));
        const auto zero_preview = inspect(zero);
        CHECK(zero_preview.settlement_readiness == execution::Status::NoEffect);
        h.cancel(zero);
        const auto positive = put(h, host_open(no::Side::Long, "positive"));
        const auto positive_preview = inspect(positive);
        CHECK(positive_preview.settlement_readiness == execution::Status::Applied);
        CHECK(positive_preview.account.status == execution::Status::Applied);
        h.cancel(positive);
        const auto explicit_units = put(h, tx(1.0, "explicit-units-preview"));
        const auto explicit_preview = inspect(explicit_units);
        CHECK(explicit_preview.terms_rejection == no::MatchRejectReason::InvalidTerms);
        CHECK(!explicit_preview.settlement_readiness && !explicit_preview.terms_cancellation);
        h.cancel(explicit_units);
        reached = true;
    };
    run(host, spec("terms-preview-table"), {100});
    completed(host);
    CHECK(reached);
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

void a_t9_all_current_fact_shapes_are_bitwise_identical() {
    TermsHost host;
    host.resolver = [](const NativeExecutionTermsFacts& facts) {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)
            && std::holds_alternative<no::RemainingDeferred>(facts.remaining)) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto compare = [&](const no::RequestHandle& target, NativeCurrentPriceRule rule) {
            std::optional<double> as_presented_default;
            if (rule == NativeCurrentPriceRule::NearestTick) {
                h.resolved_facts.clear();
                const auto as_presented = h.inspect_current_execution(
                    command(target, NativeCurrentPriceRule::AsPresented));
                REQUIRE(!as_presented.refusal);
                REQUIRE(h.resolved_facts.size() == 1);
                as_presented_default = h.resolved_facts.front().default_resolved_price;
            }
            h.resolved_facts.clear();
            const auto preview = h.inspect_current_execution(command(target, rule));
            REQUIRE(!preview.refusal);
            const auto result = h.execute_current(command(target, rule));
            REQUIRE(!std::holds_alternative<NativeCurrentRefusal>(result));
            REQUIRE(h.resolved_facts.size() == 2);
            const auto& a = h.resolved_facts[0];
            const auto& b = h.resolved_facts[1];
            CHECK(same_terms_facts_bits(a, b));
            CHECK(a.definition == b.definition);
            CHECK(a.price_rule == rule && b.price_rule == rule);
            if (as_presented_default) {
                CHECK(bits(a.default_resolved_price) == bits(*as_presented_default));
                CHECK(bits(b.default_resolved_price) == bits(*as_presented_default));
            }
        };

        h.seed(3.0, 100, 171);
        compare(put(h, reduce(1, "facts-reduce")), NativeCurrentPriceRule::AsPresented);
        compare(put(h, host_close("facts-host-close")), NativeCurrentPriceRule::AsPresented);
        compare(put(h, host_open(no::Side::Long, "facts-host-open")),
                NativeCurrentPriceRule::AsPresented);
        compare(put(h, flat("facts-flatten")), NativeCurrentPriceRule::AsPresented);
        compare(put(h, tx(1, "facts-nearest")), NativeCurrentPriceRule::NearestTick);
        reached = true;
    };
    auto configuration = spec("terms-fact-shapes");
    configuration.price_tick = 0.1;
    run(host, configuration, {100.0});
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

void a_t6b_c_t6e_sibling_control_and_gap_provenance() {
    auto run_limits = [](const char* key, double open, double high, double level_a,
                         double level_b) {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        };
        host.beginning = [=](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            auto a = host_open(no::Side::Short, "sibling-a");
            a.trigger = no::Limit{level_a};
            auto b = host_open(no::Side::Short, "sibling-b");
            b.trigger = no::Limit{level_b};
            put(h, a); put(h, b);
        };
        REQUIRE(host.configure_native(spec(key)).status == NativeSetupStatus::Applied);
        const Bar bar{open, high, open, high, 1, T};
        host.run(&bar, 1);
        completed(host);
        return host.resolved_facts;
    };

    const auto control = run_limits("terms-control-one", 0.25, 3.5,
                                    1.9999999999999998, 4.0);
    REQUIRE(control.size() == 1);
    CHECK(control.front().price_kind == no::NativeCandidatePriceKind::TriggerLevel);
    CHECK(!control.front().shared_cursor_collision);
    REQUIRE(control.front().trigger_level);
    CHECK(bits(control.front().raw_price) == bits(*control.front().trigger_level));

    const auto siblings = run_limits("terms-sibling-cross", 99, 101, 100, 100);
    REQUIRE(siblings.size() == 2);
    for (const auto& facts : siblings) {
        CHECK(facts.price_kind == no::NativeCandidatePriceKind::TriggerLevel);
        REQUIRE(facts.trigger_level);
        CHECK(bits(facts.raw_price) == bits(100.0));
        CHECK(bits(*facts.trigger_level) == bits(100.0));
    }

    const auto gaps = run_limits("terms-sibling-gap", 101, 101, 100, 100);
    REQUIRE(gaps.size() == 2);
    for (const auto& facts : gaps) {
        CHECK(facts.price_kind == no::NativeCandidatePriceKind::PointPrice);
        CHECK(bits(facts.raw_price) == bits(101.0));
    }
}

void a_t6d_newly_eligible_wait_child_is_point_price() {
    TermsHost host;
    std::optional<NativeExecutionTermsFacts> child_facts;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        if (facts.definition->request.label == "wait-child") child_facts = facts;
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    host.beginning = [](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        const auto parent = put(h, tx(1, "wait-parent"));
        auto child = tx(1, "wait-child");
        child.owner = no::WaitForApplied{parent};
        child.trigger = no::Limit{100.0};
        put(h, child);
    };
    run(host, spec("terms-newly-eligible"), {100});
    completed(host);
    REQUIRE(child_facts);
    CHECK(child_facts->price_kind == no::NativeCandidatePriceKind::PointPrice);
    CHECK(bits(child_facts->raw_price) == bits(100.0));
}

void a_t6d_replacement_stop_limit_and_trail_invalidation() {
    TermsHost replacement;
    no::RequestHandle stale;
    no::RequestHandle predecessor;
    bool replaced = false;
    replacement.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                   no::OpeningShape::Transact};
    };
    replacement.beginning = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        auto first = host_open(no::Side::Short, "replacement-first");
        first.trigger = no::Limit{100.0};
        auto second = host_open(no::Side::Short, "replacement-stale");
        second.trigger = no::Limit{100.0};
        put(h, first);
        stale = put(h, second);
        predecessor = stale;
    };
    replacement.notification = [&](Host& base, const no::ExecutionAppliedEvent& event) {
        if (event.request().label != "replacement-first") return;
        auto& h = static_cast<TermsHost&>(base);
        auto successor = host_open(no::Side::Short, "replacement-successor");
        successor.trigger = no::Limit{100.0};
        const auto result = h.replace(stale, successor);
        REQUIRE(result.status == no::ReplaceStatus::Replaced && result.successor);
        stale = *result.successor;
        replaced = true;
    };
    const std::vector<Bar> replacement_bars{
        Bar{99.0, 101.0, 99.0, 101.0, 1.0, T},
        Bar{100.0, 100.0, 100.0, 100.0, 1.0, T + 60000},
    };
    REQUIRE(replacement.configure_native(spec("terms-replacement-origin")).status
            == NativeSetupStatus::Applied);
    replacement.run(replacement_bars.data(), static_cast<int>(replacement_bars.size()));
    completed(replacement);
    CHECK(replaced);
    const auto* successor_facts = facts_for(replacement, "replacement-successor");
    REQUIRE(successor_facts);
    CHECK(successor_facts->target == stale);
    CHECK(successor_facts->target.incarnation != predecessor.incarnation);
    CHECK(successor_facts->price_kind == no::NativeCandidatePriceKind::PointPrice);
    REQUIRE(successor_facts->trigger_level);
    // The successor is born at the sibling's fill cursor and first sees the
    // continuing point at 101, not the predecessor's level crossing.
    CHECK(bits(successor_facts->raw_price) == bits(101.0));
    CHECK(bits(*successor_facts->trigger_level) == bits(100.0));
    CHECK(!facts_for(replacement, "replacement-stale"));

    TermsHost stop_limit;
    stop_limit.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                   no::OpeningShape::Transact};
    };
    stop_limit.beginning = [](Host& base) {
        auto request = host_open(no::Side::Long, "stop-limit-no-transfer");
        request.trigger = no::StopLimit{100.0, 101.0};
        put(static_cast<TermsHost&>(base), request);
    };
    run(stop_limit, spec("terms-stop-limit-no-transfer"), {100.0});
    completed(stop_limit);
    const auto* stop_limit_facts = facts_for(stop_limit, "stop-limit-no-transfer");
    REQUIRE(stop_limit_facts);
    CHECK(stop_limit_facts->price_kind == no::NativeCandidatePriceKind::PointPrice);
    REQUIRE(stop_limit_facts->trigger_level);
    CHECK(bits(stop_limit_facts->raw_price) == bits(100.0));
    CHECK(bits(*stop_limit_facts->trigger_level) == bits(101.0));
    const auto stop_limit_activations = events<no::ActivatedEvent>(stop_limit);
    REQUIRE(stop_limit_activations.size() == 1);
    CHECK(stop_limit_activations.front().kind == no::ActivationKind::StopLimit);

    TermsHost armed_trail;
    armed_trail.resolver = [](const NativeExecutionTermsFacts& facts) {
        return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                   no::OpeningShape::Transact};
    };
    armed_trail.beginning = [](Host& base) {
        auto request = host_open(no::Side::Short, "trail-arm-no-transfer");
        request.trigger = no::Trail{1.0, 101.0};
        put(static_cast<TermsHost&>(base), request);
    };
    const std::vector<Bar> trail_bars{
        Bar{100.0, 101.0, 100.0, 101.0, 1.0, T},
        Bar{99.0, 99.0, 99.0, 99.0, 1.0, T + 60000},
    };
    REQUIRE(armed_trail.configure_native(spec("terms-trail-arm-no-transfer")).status
            == NativeSetupStatus::Applied);
    armed_trail.run(trail_bars.data(), static_cast<int>(trail_bars.size()));
    completed(armed_trail);
    const auto* trail_facts = facts_for(armed_trail, "trail-arm-no-transfer");
    REQUIRE(trail_facts);
    CHECK(trail_facts->price_kind == no::NativeCandidatePriceKind::PointPrice);
    REQUIRE(trail_facts->trigger_level);
    CHECK(bits(trail_facts->raw_price) == bits(99.0));
    CHECK(bits(*trail_facts->trigger_level) == bits(100.0));
    std::vector<no::ActivatedEvent> trail_activations;
    for (const auto& event : events<no::ActivatedEvent>(armed_trail)) {
        if (event.kind == no::ActivationKind::TrailArm
            || event.kind == no::ActivationKind::TrailTrigger) {
            trail_activations.push_back(event);
        }
    }
    REQUIRE(trail_activations.size() == 2);
    CHECK(trail_activations[0].kind == no::ActivationKind::TrailArm);
    CHECK(trail_activations[1].kind == no::ActivationKind::TrailTrigger);
    CHECK(trail_activations[0].cursor.point.ordinal < trail_activations[1].cursor.point.ordinal);
}

void a_t6e_stop_and_trail_gap_siblings_replay() {
    struct GapSummary {
        std::vector<NativeExecutionTermsFacts> facts;
        std::vector<no::TermsResolvedEvent> receipts;
        std::uint64_t hash = 0;
    };
    auto run_stop_gap = [](const char* key, double price) {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        };
        host.beginning = [](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            auto first = host_open(no::Side::Long, "stop-gap-first");
            first.trigger = no::Stop{100.0};
            auto second = host_open(no::Side::Long, "stop-gap-second");
            second.trigger = no::Stop{100.0};
            put(h, first);
            put(h, second);
        };
        run(host, spec(key), {price});
        completed(host);
        return GapSummary{host.resolved_facts, events<no::TermsResolvedEvent>(host),
                          host.native_continuation_hash()};
    };
    auto check_stop_gap = [](const GapSummary& summary, double expected) {
        REQUIRE(summary.facts.size() == 2);
        REQUIRE(summary.receipts.size() == 2);
        for (std::size_t i = 0; i < summary.facts.size(); ++i) {
            const auto& facts = summary.facts[i];
            CHECK(facts.definition->request.label
                  == (i == 0 ? "stop-gap-first" : "stop-gap-second"));
            CHECK(std::holds_alternative<no::StopActive>(facts.trigger_state));
            CHECK(facts.price_kind == no::NativeCandidatePriceKind::PointPrice);
            REQUIRE(facts.trigger_level);
            CHECK(bits(facts.raw_price) == bits(expected));
            CHECK(bits(*facts.trigger_level) == bits(100.0));
            const auto& receipt = summary.receipts[i];
            CHECK(receipt.request().label == facts.definition->request.label);
            CHECK(receipt.input.price_kind == no::NativeCandidatePriceKind::PointPrice);
            CHECK(bits(receipt.input.raw_price) == bits(expected));
            CHECK(bits(receipt.cursor.t) == bits(facts.cursor.t));
        }
    };
    const auto stop_gap = run_stop_gap("terms-stop-gap-siblings", 101.0);
    const auto stop_gap_replay = run_stop_gap("terms-stop-gap-siblings", 101.0);
    check_stop_gap(stop_gap, 101.0);
    CHECK(stop_gap.hash == stop_gap_replay.hash);
    const auto stop_exact_gap = run_stop_gap("terms-stop-exact-gap-siblings", 100.0);
    check_stop_gap(stop_exact_gap, 100.0);

    auto run_trail_gap = [](const char* key, double final_price) {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            return no::ExecutionTerms{facts.default_resolved_price, 1.0,
                                       no::OpeningShape::Transact};
        };
        host.beginning = [](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            auto first = host_open(no::Side::Short, "trail-gap-first");
            first.trigger = no::Trail{1.0, std::nullopt};
            auto second = host_open(no::Side::Short, "trail-gap-second");
            second.trigger = no::Trail{1.0, std::nullopt};
            put(h, first);
            put(h, second);
        };
        run(host, spec(key), {101.0, final_price});
        completed(host);
        return GapSummary{host.resolved_facts, events<no::TermsResolvedEvent>(host),
                          host.native_continuation_hash()};
    };
    auto check_trail_gap = [](const GapSummary& summary, double expected) {
        REQUIRE(summary.facts.size() == 2);
        REQUIRE(summary.receipts.size() == 2);
        for (std::size_t i = 0; i < summary.facts.size(); ++i) {
            const auto& facts = summary.facts[i];
            CHECK(facts.definition->request.label
                  == (i == 0 ? "trail-gap-first" : "trail-gap-second"));
            CHECK(std::holds_alternative<no::TrailActive>(facts.trigger_state));
            CHECK(facts.price_kind == no::NativeCandidatePriceKind::PointPrice);
            REQUIRE(facts.trigger_level);
            CHECK(bits(facts.raw_price) == bits(expected));
            CHECK(bits(*facts.trigger_level) == bits(100.0));
            const auto& receipt = summary.receipts[i];
            CHECK(receipt.request().label == facts.definition->request.label);
            CHECK(receipt.input.price_kind == no::NativeCandidatePriceKind::PointPrice);
            CHECK(bits(receipt.input.raw_price) == bits(expected));
            CHECK(bits(receipt.cursor.t) == bits(facts.cursor.t));
        }
    };
    const auto trail_gap = run_trail_gap("terms-trail-gap-siblings", 99.0);
    const auto trail_gap_replay = run_trail_gap("terms-trail-gap-siblings", 99.0);
    check_trail_gap(trail_gap, 99.0);
    CHECK(trail_gap.hash == trail_gap_replay.hash);
    const auto trail_exact_gap = run_trail_gap("terms-trail-exact-gap-siblings", 100.0);
    check_trail_gap(trail_exact_gap, 100.0);
}

}  // namespace

int main() {
    test("private binary64 helper", bit_helper_controls);
    test("test-only facts oracle", test_only_oracle_host_uses_facts);
    test("queued host-sized opening", queued_host_sized_open);
    test("zero host-sized receipt", zero_terms_are_a_receipt_then_no_effect);
    test("zero units with nonpositive price", zero_units_ignore_nonpositive_price_on_both_paths);
    test("queued price receipt", overridden_price_is_durable_before_settlement);
    test("current typed preview outcomes", current_preview_has_typed_terms_outcomes_without_writes);
    test("current deferred group cancellation", current_group_deduction_returns_cancelled);
    test("A-T4c nonpositive group terminal", a_t4c_nonpositive_group_terminal_matches_preview);
    test("A-T4b queued deferred cancellation", a_t4b_queued_group_cancellation_is_typed_terminal);
    test("host NaN attempted terms", host_nan_price_is_rejected_without_a_receipt);
    test("A-T11d host-sized NaN rows", a_t11d_host_sized_nan_rows);
    test("host price limit fence", host_price_cannot_breach_active_limit);
    test("A-T4d authenticated unrepresentable deduction", a_t4d_authenticated_unrepresentable_deduction);
    test("A-T5 terms rejections retain attempts", a_t5_terms_rejections_retain_attempted_terms);
    test("A-T5 bound host-sized rematch units", a_t5_bound_host_sized_rematch_rejects_units);
    test("explicit reduction grid policy both ways", explicit_reduction_grid_policy_both_ways);
    test("A-T10 resolver callback exception mapping", a_t10_callback_exception_mapping);
    test("A-T7 scoped facts and flat close", a_t7_scope_facts_and_flat_close_shortcut);
    test("A-T8 host-sized PointBudget rematch", a_t8_point_budget_binding_and_price_rematch);
    test("A-T13 post-binding NoChange", a_t13_post_binding_no_change_is_contract_failure);
    test("A-T8b explicit allowance equivalence", a_t8b_explicit_equivalent_allowance_and_t8c_identity_control);
    test("A-T11b queued notification anchor price", a_t11b_queued_notification_uses_final_terms_price);
    test("A-T11 identity infinity rows", a_t11_identity_infinity_and_nan_attempts);
    test("A-T6 matcher price-kind cases", a_t6_matcher_price_kind_all_ten_cases);
    test("A-T6c stop/trail/stop-limit sibling origins",
         a_t6c_stop_trail_and_stop_limit_sibling_origins);
    test("A-T15 price boundary and A-T16 flat preview", a_t15_price_boundary_and_a_t16_flat_preview);
    test("A-T14 preview outcome table", a_t14_preview_outcome_table_without_mutation);
    test("current preview/execute terms facts", current_preview_and_execute_see_same_terms_facts);
    test("A-T9 all current fact shapes", a_t9_all_current_fact_shapes_are_bitwise_identical);
    test("rounded crossing provenance collision", rounded_crossing_collision_keeps_request_origin);
    test("A-T6b/c/e sibling provenance controls", a_t6b_c_t6e_sibling_control_and_gap_provenance);
    test("A-T6d newly eligible point provenance", a_t6d_newly_eligible_wait_child_is_point_price);
    test("A-T6d replacement and transfer invalidation",
         a_t6d_replacement_stop_limit_and_trail_invalidation);
    test("A-T6e stop/trail gap sibling replay", a_t6e_stop_and_trail_gap_siblings_replay);
    std::printf("R4-B terms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
