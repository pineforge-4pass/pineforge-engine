// R5 L3b: the generic sizing-price rule, placement-time sizing and the
// reversal shapes a kernel-sized opening may name.
//
// Every scenario drives NativeStrategyHost only. The harness is the R5 R2
// differential's: both halves run on the same account, instrument and bars,
// and every number is pinned bit for bit rather than compared with a
// tolerance.
#include "native_terms_fixture.hpp"

#include <cmath>
#include <cstdio>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

using namespace r4_test;
using namespace r4_terms;

static_assert(static_cast<int>(no::SizePrice::Resolved) == 0);
static_assert(static_cast<int>(no::SizePrice::Signal) == 1);
static_assert(std::is_same_v<decltype(no::Sized{}.price), no::SizePrice>);

namespace {

// The half-up money grid a source layer applies to a price before it becomes
// a sizing price. This is the control arithmetic, written independently of
// the kernel's own grid_round_half_up.
double nearest_tick(double value, double tick) {
    if (!std::isfinite(value) || !(tick > 0.0)) return value;
    return std::floor(value / tick + 0.5) * tick;
}

no::Request sized_open(no::SizeBasis basis, no::Side side, no::SizeTime time,
                       no::SizePrice price, const char* label,
                       bool reserve_fee = false) {
    no::Sized sized;
    sized.side = side;
    sized.basis = basis;
    sized.time = time;
    sized.price = price;
    sized.reserve_percent_fee = reserve_fee;
    no::Request out;
    out.intent = sized;
    out.label = label;
    return out;
}

// One opening per run, executed through the guarded current-execution target
// on the first calculation. Same shape as the L3 sizing-bases harness.
no::ExecutionAppliedEvent open_once(TermsHost& host, const NativeRunSpec& setup,
                                    const no::Request& request,
                                    std::initializer_list<double> prices) {
    std::optional<no::ExecutionAppliedEvent> applied;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (applied) return;
        applied = apply(h, put(h, request));
    };
    run(host, setup, prices);
    completed(host);
    if (!applied) {
        ++failures;
        std::printf("FAIL %s: nothing applied\n", scenario);
        return {};
    }
    return *applied;
}

// A run on the instrument price grid: HalfUp quantized fills on a one-cent
// tick, which is what {Signal, HalfUp} sizes against.
NativeRunSpec gridded(const char* key, double tick = 0.01,
                      std::uint32_t slippage_ticks = 0) {
    auto setup = spec(key);
    setup.initial_capital = 100000.0;
    setup.price_tick = tick;
    setup.slippage_ticks = slippage_ticks;
    setup.price_grid = NativePriceGrid::QuantizeFills;
    setup.grid_rounding = NativeGridRounding::HalfUp;
    return setup;
}

// ===========================================================================
// 1. SizePrice: which price the basis converts at.
// ===========================================================================

// 2517.70 is not 251770 * 0.01 in binary64: the decimal literal a feed carries
// is one ulp away from the tick ladder. Resolved divides by the raw price,
// Signal divides by the price the expected market fill lands on.
void the_signal_rule_sizes_against_the_expected_fill_price() {
    const double c = 2517.70;
    const double tick = 0.01;
    scenario = "off-grid signal close";
    CHECK(bits(nearest_tick(c, tick)) != bits(c));

    TermsHost resolved;
    const auto resolved_applied = open_once(
        resolved, gridded("l3b-price-resolved"),
        sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                   no::SizePrice::Resolved, "resolved"),
        {c});
    TermsHost signal;
    const auto signal_applied = open_once(
        signal, gridded("l3b-price-signal"),
        sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                   no::SizePrice::Signal, "signal"),
        {c});

    std::printf("  [off-grid close] resolved=%.17g signal=%.17g\n",
                resolved_applied.opened_units, signal_applied.opened_units);
    CHECK(bits(resolved_applied.opened_units) == bits(5000.0 / c));
    CHECK(bits(signal_applied.opened_units) == bits(5000.0 / nearest_tick(c, tick)));
    CHECK(bits(resolved_applied.opened_units) != bits(signal_applied.opened_units));

    // On a price that is already on the grid the two rules coincide exactly.
    TermsHost on_grid_resolved;
    const auto a = open_once(
        on_grid_resolved, gridded("l3b-price-on-grid-a"),
        sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                   no::SizePrice::Resolved, "resolved"),
        {125.0});
    TermsHost on_grid_signal;
    const auto b = open_once(
        on_grid_signal, gridded("l3b-price-on-grid-b"),
        sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                   no::SizePrice::Signal, "signal"),
        {125.0});
    scenario = "on-grid signal close";
    CHECK(bits(nearest_tick(125.0, tick)) == bits(125.0));
    CHECK(bits(a.opened_units) == bits(b.opened_units));
}

// The slippage half of the rule: the signal price is the expected market fill,
// so it moves adversely for the request's own side.
void the_signal_price_carries_the_run_slippage_on_the_requests_side() {
    const double c = 100.0;
    const double tick = 0.01;
    const std::uint32_t ticks = 3;
    const double slip = static_cast<double>(ticks) * tick;

    struct Row { const char* name; no::Side side; double sign; };
    for (const Row& row : {Row{"long pays up", no::Side::Long, 1.0},
                           Row{"short sells down", no::Side::Short, -1.0}}) {
        TermsHost host;
        const auto applied = open_once(
            host, gridded("l3b-price-slip", tick, ticks),
            sized_open(no::CashValue{5000.0}, row.side, no::SizeTime::AtAcceptance,
                       no::SizePrice::Signal, "signal"),
            {c});
        scenario = row.name;
        const double expected_price = nearest_tick(c + row.sign * slip, tick);
        std::printf("  [%s] price=%.17g units=%.17g\n", row.name, expected_price,
                    std::abs(applied.opened_units));
        CHECK(bits(std::abs(applied.opened_units)) == bits(5000.0 / expected_price));
    }

    // Without a declared price grid the signal price is the unrounded
    // slippage-adjusted decision price: the rule names no source language and
    // adds no rounding of its own.
    auto ungridded = spec("l3b-price-ungridded");
    ungridded.initial_capital = 100000.0;
    ungridded.price_tick = tick;
    ungridded.slippage_ticks = ticks;
    ungridded.price_grid = NativePriceGrid::None;
    TermsHost raw;
    const auto applied = open_once(
        raw, ungridded,
        sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                   no::SizePrice::Signal, "signal"),
        {2517.70});
    scenario = "no price grid leaves the signal price unrounded";
    CHECK(bits(applied.opened_units) == bits(5000.0 / (2517.70 + slip)));
}

// The signal price is frozen at acceptance, so it composes with SizeTime.
// With AtMatch the basis still converts at that frozen price even though the
// candidate is a different bar at a different price.
void the_frozen_signal_price_survives_to_a_later_candidate() {
    auto setup = gridded("l3b-price-frozen");
    setup.close_execution = NativeCloseExecution::NextEligiblePoint;
    TermsHost host;
    std::vector<double> opened;
    host.notification = [&](Host&, const no::ExecutionAppliedEvent& event) {
        opened.push_back(event.opened_units);
    };
    bool submitted = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (submitted) return;
        submitted = true;
        // Frozen signal price, resolved at the candidate.
        put(h, sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtMatch,
                          no::SizePrice::Signal, "at-match-signal"));
        // The kernel's own candidate price, for contrast.
        put(h, sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtMatch,
                          no::SizePrice::Resolved, "at-match-resolved"));
    };
    run(host, setup, {100.0, 200.0, 200.0});
    completed(host);
    scenario = "at-match keeps the frozen signal price";
    REQUIRE(opened.size() == 2);
    CHECK(bits(opened[0]) == bits(5000.0 / 100.0));
    CHECK(bits(opened[1]) == bits(5000.0 / 200.0));
}

// ===========================================================================
// 2. Placement-time units: visible to the host in one pass, and an admission
//    input before the request exists.
// ===========================================================================

void the_acceptance_quantity_reaches_the_host_in_one_pass() {
    auto setup = gridded("l3b-one-pass");
    TermsHost host;
    std::optional<double> published;
    int passes = 0;
    host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        ++passes;
        if (const auto* units = std::get_if<no::RemainingUnits>(&facts.remaining)) {
            published = units->q;
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                   no::OpeningShape::Transact};
    };
    const auto applied = open_once(
        host, setup,
        sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                   no::SizePrice::Signal, "frozen"),
        {2517.70});
    scenario = "the frozen quantity is published to the host";
    // Exactly one terms pass, and it already carried the kernel's number.
    CHECK(passes == 1);
    REQUIRE(published.has_value());
    CHECK(bits(*published) == bits(5000.0 / nearest_tick(2517.70, 0.01)));
    CHECK(bits(applied.opened_units) == bits(*published));
    // The precommit view saw the same quantity, on the same single pass.
    REQUIRE(host.precommit_views.size() == 1);
    CHECK(bits(host.precommit_views.front().inspected_opened_units) == bits(*published));

    // A host that answers with its own units overrides the kernel's, and the
    // number it overrode was in front of it when it decided.
    TermsHost override_host;
    std::optional<double> seen;
    override_host.resolver = [&](const NativeExecutionTermsFacts& facts) {
        if (const auto* units = std::get_if<no::RemainingUnits>(&facts.remaining)) {
            seen = units->q;
        }
        // The host's own quirk floor: whole units only.
        const double floored = seen ? std::floor(*seen) : 0.0;
        return no::ExecutionTerms{facts.default_resolved_price, floored,
                                   no::OpeningShape::Transact};
    };
    const auto overridden = open_once(
        override_host, setup,
        sized_open(no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                   no::SizePrice::Signal, "frozen"),
        {2517.70});
    scenario = "a host override still has the last word";
    REQUIRE(seen.has_value());
    CHECK(bits(*seen) == bits(5000.0 / nearest_tick(2517.70, 0.01)));
    CHECK(bits(overridden.opened_units) == bits(std::floor(*seen)));
    CHECK(bits(overridden.opened_units) != bits(*seen));
}

void the_acceptance_quantity_is_an_admission_input_at_placement() {
    // max_abs_units: 100 % of 10 000 at a price of 100 is 100 units, over a
    // 30-unit cap. The command never becomes a request.
    {
        auto setup = spec("l3b-admit-units");
        setup.max_abs_units = 30.0;
        TermsHost host;
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            const auto result = h.submit(sized_open(
                no::EquityFraction{1.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "over-cap"));
            CHECK(result.status == no::SubmitStatus::Rejected);
            REQUIRE(result.reason.has_value());
            CHECK(*result.reason == no::RequestRejectReason::PlacementAdmission);
            CHECK(!result.handle.has_value());
            CHECK(h.native_working_requests().empty());
            // The same basis inside the cap is accepted and books.
            const auto ok = apply(h, put(h, sized_open(
                no::EquityFraction{0.2}, no::Side::Long, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "in-cap")));
            CHECK(bits(ok.opened_units) == bits(20.0));
        };
        scenario = "max_abs_units rejects at placement";
        run(host, setup, {100.0});
        completed(host);
        CHECK(reached);
    }

    // max_open_lots counts the lot the frozen quantity would add.
    {
        auto setup = spec("l3b-admit-lots");
        setup.max_open_lots = 1;
        TermsHost host;
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            (void)apply(h, put(h, tx(1.0, "first-lot")));
            const auto result = h.submit(sized_open(
                no::EquityFraction{0.2}, no::Side::Long, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "second-lot"));
            CHECK(result.status == no::SubmitStatus::Rejected);
            REQUIRE(result.reason.has_value());
            CHECK(*result.reason == no::RequestRejectReason::PlacementAdmission);
        };
        scenario = "max_open_lots rejects at placement";
        run(host, setup, {100.0});
        completed(host);
        CHECK(reached);
    }

    // The initial-margin gate, at the sizing price: 20 000 of notional on
    // 10 000 of equity at a full initial fraction.
    {
        auto setup = spec("l3b-admit-margin");
        setup.initial_margin_fraction = 1.0;
        TermsHost host;
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            const auto result = h.submit(sized_open(
                no::CashValue{20000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "over-margin"));
            CHECK(result.status == no::SubmitStatus::Rejected);
            REQUIRE(result.reason.has_value());
            CHECK(*result.reason == no::RequestRejectReason::PlacementAdmission);
            // Half the notional fits.
            const auto ok = h.submit(sized_open(
                no::CashValue{5000.0}, no::Side::Long, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "in-margin"));
            CHECK(ok.status == no::SubmitStatus::Accepted);
        };
        scenario = "initial margin rejects at placement";
        run(host, setup, {100.0});
        completed(host);
        CHECK(reached);
    }

    // AtMatch has no placement quantity, so it is admitted at submit and the
    // candidate keeps the one gate it always had.
    {
        auto setup = spec("l3b-admit-at-match");
        setup.max_abs_units = 30.0;
        TermsHost host;
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            const auto target = put(h, sized_open(
                no::EquityFraction{1.0}, no::Side::Long, no::SizeTime::AtMatch,
                no::SizePrice::Resolved, "over-cap-at-match"));
            const auto outcome = h.execute_current(command(target));
            const auto* rejected = std::get_if<no::MatchRejectedEvent>(&outcome);
            REQUIRE(rejected);
            CHECK(rejected->reason == no::MatchRejectReason::MaxAbsUnits);
            CHECK(h.lots().empty());
        };
        scenario = "at-match is still admitted at the candidate";
        run(host, setup, {100.0});
        completed(host);
        CHECK(reached);
    }
}

// ===========================================================================
// 3. A kernel-sized opening may name the reversal shapes.
// ===========================================================================

void a_kernel_sized_opening_serves_the_reversal_shapes() {
    // ReverseTo: the transaction closes the opposite book and opens the sized
    // units on the declared side, exactly as HostSized{Open} already does.
    {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            if (facts.definition->request.label != "reverse") {
                return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                           no::OpeningShape::Transact};
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::ReverseTo};
        };
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            (void)apply(h, put(h, tx(10.0, "long")));
            REQUIRE(h.lots().size() == 1);
            const auto applied = apply(h, put(h, sized_open(
                no::CashValue{2000.0}, no::Side::Short, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "reverse")));
            // 2000 of cash at 100 is 20 units short, after closing the 10 long.
            CHECK(bits(applied.closed_units) == bits(10.0));
            CHECK(bits(applied.opened_units) == bits(-20.0));
            REQUIRE(h.lots().size() == 1);
            CHECK(bits(h.lots().front().qty) == bits(20.0));
            CHECK(h.rows().size() == 1);
        };
        scenario = "kernel-sized ReverseTo";
        run(host, gridded("l3b-shape-reverse"), {100.0});
        completed(host);
        CHECK(reached);
    }

    // CloseOpposite: the sized units close the opposite book and open nothing.
    {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            if (facts.definition->request.label != "close-opposite") {
                return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                           no::OpeningShape::Transact};
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::CloseOpposite};
        };
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            (void)apply(h, put(h, tx(10.0, "long")));
            const auto applied = apply(h, put(h, sized_open(
                no::CashValue{400.0}, no::Side::Short, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "close-opposite")));
            // 400 of cash at 100 is 4 units of the 10-unit long, closed only.
            CHECK(bits(applied.closed_units) == bits(4.0));
            CHECK(bits(applied.opened_units) == bits(0.0));
            REQUIRE(h.lots().size() == 1);
            CHECK(bits(h.lots().front().qty) == bits(6.0));
        };
        scenario = "kernel-sized CloseOpposite";
        run(host, gridded("l3b-shape-close-opposite"), {100.0});
        completed(host);
        CHECK(reached);
    }

    // CloseOpposite that consumes the whole opposite book flattens it.
    {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            if (facts.definition->request.label != "flatten-opposite") {
                return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                           no::OpeningShape::Transact};
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::CloseOpposite};
        };
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            (void)apply(h, put(h, tx(10.0, "long")));
            const auto applied = apply(h, put(h, sized_open(
                no::CashValue{1000.0}, no::Side::Short, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "flatten-opposite")));
            CHECK(bits(applied.closed_units) == bits(10.0));
            CHECK(h.lots().empty());
        };
        scenario = "kernel-sized CloseOpposite flattens";
        run(host, gridded("l3b-shape-flatten"), {100.0});
        completed(host);
        CHECK(reached);
    }

    // The shapes still need an opposite book, and a reduce-shaped claim may
    // not exceed it.
    {
        TermsHost host;
        host.resolver = [](const NativeExecutionTermsFacts& facts) {
            const auto& label = facts.definition->request.label;
            if (label == "no-opposite") {
                return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                           no::OpeningShape::ReverseTo};
            }
            if (label == "over-opposite") {
                return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                           no::OpeningShape::CloseOpposite};
            }
            return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                       no::OpeningShape::Transact};
        };
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            const auto flat = put(h, sized_open(
                no::CashValue{1000.0}, no::Side::Short, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "no-opposite"));
            const auto outcome = h.execute_current(command(flat));
            const auto* rejected = std::get_if<no::MatchRejectedEvent>(&outcome);
            REQUIRE(rejected);
            CHECK(rejected->reason == no::MatchRejectReason::NoOppositeExposure);

            (void)apply(h, put(h, tx(5.0, "long")));
            const auto over = put(h, sized_open(
                no::CashValue{1000.0}, no::Side::Short, no::SizeTime::AtAcceptance,
                no::SizePrice::Signal, "over-opposite"));
            const auto second = h.execute_current(command(over));
            const auto* refused = std::get_if<no::MatchRejectedEvent>(&second);
            REQUIRE(refused);
            CHECK(refused->reason == no::MatchRejectReason::InvalidTerms);
            CHECK(bits(h.lots().front().qty) == bits(5.0));
        };
        scenario = "kernel-sized shapes still need an opposite book";
        run(host, gridded("l3b-shape-guards"), {100.0});
        completed(host);
        CHECK(reached);
    }
}

// ===========================================================================
// 4. The adapter is untouched by all of it.
// ===========================================================================

void the_defaults_leave_every_established_resolution_alone() {
    // A defaulted Sized is SizePrice::Resolved, a defaulted ScopeFraction is
    // ScopeBasis::AtMatch, and neither folds anything new into the request
    // digest: the continuation identity of a run that never names them is the
    // pre-lane identity.
    no::Sized defaulted;
    CHECK(defaulted.price == no::SizePrice::Resolved);
    CHECK(defaulted.time == no::SizeTime::AtMatch);
    no::ScopeFraction fraction;
    CHECK(fraction.basis == no::ScopeBasis::AtMatch);
    CHECK(fraction.claim == no::ScopeClaim::Gross);

    TermsHost a;
    const auto first = open_once(
        a, spec("l3b-default"),
        sized_open(no::CashValue{1000.0}, no::Side::Long, no::SizeTime::AtMatch,
                   no::SizePrice::Resolved, "defaulted"),
        {100.0});
    TermsHost b;
    no::Request implicit;
    implicit.intent = no::Sized{no::Side::Long, no::CashValue{1000.0}};
    implicit.label = "defaulted";
    const auto second = open_once(b, spec("l3b-default"), implicit, {100.0});
    scenario = "an explicit default is the implicit default";
    CHECK(bits(first.opened_units) == bits(second.opened_units));
    CHECK(a.native_continuation_hash() == b.native_continuation_hash());
}

}  // namespace

int main() {
    test("the signal rule sizes against the expected fill price",
         the_signal_rule_sizes_against_the_expected_fill_price);
    test("the signal price carries the run slippage",
         the_signal_price_carries_the_run_slippage_on_the_requests_side);
    test("the frozen signal price survives to a later candidate",
         the_frozen_signal_price_survives_to_a_later_candidate);
    test("the acceptance quantity reaches the host in one pass",
         the_acceptance_quantity_reaches_the_host_in_one_pass);
    test("the acceptance quantity is an admission input at placement",
         the_acceptance_quantity_is_an_admission_input_at_placement);
    test("a kernel-sized opening serves the reversal shapes",
         a_kernel_sized_opening_serves_the_reversal_shapes);
    test("the defaults leave every established resolution alone",
         the_defaults_leave_every_established_resolution_alone);
    std::printf("R5 L3b sizing price rule: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
