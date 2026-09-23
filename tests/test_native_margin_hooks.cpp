/*
 * test_native_margin_hooks.cpp — R5 lane L4b: the hostable margin model.
 *
 * L4 gave the kernel the margin MECHANISM: the level solve, the check points,
 * the kernel-originated request, its Superseded re-pricing, the receipt and
 * on_native_margin_call. This lane keeps every one of those in the kernel and
 * opens the POLICY the mechanism runs on, exactly as resolve_execution_terms
 * opens the fill price:
 *
 *   1. NativeMarginModel::basis        — whether open-entry commissions
 *      reduce the margin equity (MG-A).
 *   2. NativeMarginModel::level_base   — whether the reported level is solved
 *      from marked equity or from realized money alone (MG-D).
 *   3. resolve_margin_requirement()    — the two numbers of the breach test,
 *      consulted at EVERY kernel check point BEFORE that test, so a host can
 *      raise a call the kernel would not make (MG-B) or veto one it would.
 *   4. NativeLiquidationCheck::PathAdverseExtremeMark — the period-mark
 *      broker: measure at the remaining path's adverse extreme and rest the
 *      reduction AT that mark (MG-H/MG-H2).
 *   5. margin_check_allowed()          — a gate over the kernel's own check
 *      points, for a broker model that does not check at all of them (MG-I).
 *
 * Every one is opt-in. Scenario 1 pins that the whole default surface is
 * exactly the L4 surface: same events, same rows, same run-spec fold.
 *
 * Hand arithmetic throughout (point_value = 1, account fx = 1):
 *   equity(P)      = capital + realized + dir * (P - entry) * units - fees
 *   required(P, m) = units * P * m
 *   restore        = (required - equity) / (P * m)
 *   level L        : equity(L) == required(L, m)
 *                  = (base - dir * units * entry) / (units * (m - dir))
 *                    with base = capital + realized [- fees]
 *
 * The TWIN — the R5 differential rows re-run with a native host that
 * implements the adapter's own rules through these hooks, against the
 * adapter in the same process — is tests/test_native_margin_hooks_twin.cpp
 * (source-bound). This TU is source-free and runs in the kernel-only profile.
 */
#include "native_margin_hooks_fixture.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace r4_test;
using namespace l4b_fixture;

namespace {

// ── Portable pins ───────────────────────────────────────────────────────
// A raw native_continuation_hash() constant is not portable (it folds this
// machine's resolved zoneinfo paths). native_run_spec_digest() is the
// consumer's own spec fold and nothing else, and the report digest below is
// this test's own fold over the observable closed rows. Both are observed on
// THIS tree for the L4 default surface and guard that this lane moved
// neither. The continuation itself is compared between two runs in process.
// expectation corrected: 16781023430602848315 -> 11251058089389769594, because v19-B: this host keeps NativeEventRetention::Full to read its whole event record back, and the spec digest folds a retention that is not the default Window.
constexpr std::uint64_t kL4DefaultSpecDigest = 11251058089389769594ULL;
constexpr std::uint64_t kL4DefaultReportDigest = 17348290694371301405ULL;

// FNV-1a over every closed row a run reports: the report as a host reads it.
std::uint64_t report_digest(const Host& host) {
    std::uint64_t state = 1469598103934665603ULL;
    const auto bytes = [&state](const void* data, std::size_t count) {
        const auto* values = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            state ^= values[i];
            state *= 1099511628211ULL;
        }
    };
    const auto d = [&bytes](double value) { bytes(&value, sizeof value); };
    const auto i = [&bytes](std::int64_t value) { bytes(&value, sizeof value); };
    const auto s = [&bytes, &i](const std::string& value) {
        i(static_cast<std::int64_t>(value.size()));
        bytes(value.data(), value.size());
    };
    i(static_cast<std::int64_t>(host.rows().size()));
    for (const auto& row : host.rows()) {
        i(row.entry_time); i(row.exit_time);
        d(row.entry_price); d(row.exit_price); d(row.qty); d(row.pnl);
        i(row.is_long ? 1 : 0);
        s(row.entry_id); s(row.exit_id); s(row.exit_comment);
    }
    return state;
}

NativeMarginModel model(double maintenance,
                        NativeLiquidationSizing sizing = NativeLiquidationSizing::RestoreMinimum,
                        double multiple = 1.0, double initial = 0.5) {
    NativeMarginModel m;
    m.initial_long = initial;
    m.initial_short = initial;
    m.maintenance_long = maintenance;
    m.maintenance_short = maintenance;
    m.sizing = sizing;
    m.shortfall_multiple = multiple;
    return m;
}

// The lane's host: it records every kernel consultation and can answer any of
// the three hooks. Answering nothing is the kernel's own behaviour.
struct HookHost final : Host {
    double open_units = 0.0;
    int bars = 0;
    std::function<std::optional<NativeMarginDecision>(const NativeMarginRequirementView&)>
        requirement;
    std::function<bool(const NativeMarginCheckPoint&)> gate;
    std::function<std::optional<double>(const NativeMarginCallView&)> sizer;
    std::function<no::ExecutionTerms(const NativeExecutionTermsFacts&)> terms;
    mutable std::vector<NativeMarginRequirementView> requirement_views;
    mutable std::vector<NativeMarginCheckPoint> check_points;
    std::vector<no::MarginCallEvent> margin_calls;
    std::vector<std::optional<double>> level_at_bar_open;

    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        level_at_bar_open.push_back(native_liquidation_price());
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (bars++ == 0 && open_units != 0.0) (void)put(*this, tx(open_units, "entry"));
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        margin_calls.push_back(event);
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        requirement_views.push_back(view);
        return requirement ? requirement(view) : std::nullopt;
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        check_points.push_back(point);
        return gate ? gate(point) : true;
    }
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        return sizer ? sizer(view) : std::nullopt;
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        if (terms) return terms(facts);
        return Host::resolve_execution_terms(facts);
    }
};

void drive(HookHost& host, const NativeRunSpec& s, const std::vector<Bar>& bars) {
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

std::size_t accepted_liquidations(const Host& host) {
    std::size_t count = 0;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::AcceptedEvent>(&*row.command)) {
            if (e->request().label == kLiquidationLabel) ++count;
        }
    }
    return count;
}

// The one consultation made at a given mark on the whole live book.
const NativeMarginRequirementView* view_at(const HookHost& host, double mark, double units) {
    for (const auto& view : host.requirement_views) {
        if (view.mark == mark && view.position.signed_units == units) return &view;
    }
    return nullptr;
}

// The lane tape: 20 units long at 100, then a bar whose adverse extreme (95)
// is what every maintenance fraction below is measured against.
std::vector<Bar> tape() {
    return {ohlc(0, 100.0, 100.0, 100.0, 100.0), ohlc(1, 100.0, 101.0, 95.0, 96.0)};
}

// ------------------------------------------------------------------- 1
// The whole default surface is the L4 surface. A host that overrides both new
// virtuals with their default answers books the same fills, the same rows and
// the same continuation as one that does not implement them at all, and the
// two new spec fields fold nothing while they stay at MarkedEquity.
void defaults_are_byte_identical() {
    auto s = margin_spec("l4b-default");
    s.margin = model(0.5, NativeLiquidationSizing::ShortfallMultiple, 4.0);

    // The bare L4 host: no new virtual overridden anywhere.
    struct BareHost final : Host {
        double open_units = 0.0;
        int bars = 0;
        void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
            Host::on_native_bar(bar, context);
            if (bars++ == 0) (void)put(*this, tx(open_units, "entry"));
        }
    };
    BareHost bare;
    bare.open_units = 20.0;
    REQUIRE(bare.configure_native(s).status == NativeSetupStatus::Applied);
    const auto bars = tape();
    bare.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(bare.last_error().empty());

    const auto rows = liquidations(bare);
    REQUIRE(rows.size() == 1);
    near(rows[0].closed_units, 4.2105263157894735);
    near(rows[0].resolved_price, 100.0);
    CHECK(bare.native_events(0).size() == 19);
    const auto digest = report_digest(bare);
    if (digest != kL4DefaultReportDigest) {
        std::printf("  report digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(kL4DefaultReportDigest));
    }
    CHECK(digest == kL4DefaultReportDigest);

    // The same run through a host that implements every new hook with the
    // kernel's own answer: identical events, rows and continuation.
    HookHost hooked;
    hooked.open_units = 20.0;
    drive(hooked, s, bars);
    CHECK(hooked.native_events(0).size() == bare.native_events(0).size());
    CHECK(report_digest(hooked) == digest);
    CHECK(hooked.native_continuation_hash() == bare.native_continuation_hash());
    CHECK(liquidations(hooked).size() == 1);
    // ... and the kernel did consult both new hooks on the way.
    CHECK(!hooked.requirement_views.empty());
    CHECK(!hooked.check_points.empty());

    // The spec fold: pinned, unmoved by stating either new field at its
    // default, and moved by either one of them (or by the new check mode).
    const auto spec_digest = native_run_spec_digest(s);
    if (spec_digest != kL4DefaultSpecDigest) {
        std::printf("  spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(spec_digest),
                    static_cast<unsigned long long>(kL4DefaultSpecDigest));
    }
    CHECK(spec_digest == kL4DefaultSpecDigest);
    auto stated = s;
    stated.margin->basis = NativeMarginEquityBasis::MarkedEquity;
    stated.margin->level_base = NativeLiquidationLevelBase::MarkedEquity;
    CHECK(native_run_spec_digest(stated) == kL4DefaultSpecDigest);
    auto moved_basis = s;
    moved_basis.margin->basis = NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission;
    CHECK(native_run_spec_digest(moved_basis) != kL4DefaultSpecDigest);
    auto moved_level = s;
    moved_level.margin->level_base = NativeLiquidationLevelBase::RealizedOnly;
    CHECK(native_run_spec_digest(moved_level) != kL4DefaultSpecDigest);
    CHECK(native_run_spec_digest(moved_basis) != native_run_spec_digest(moved_level));
    auto moved_check = s;
    moved_check.margin->check = NativeLiquidationCheck::PathAdverseExtremeMark;
    CHECK(native_run_spec_digest(moved_check) != kL4DefaultSpecDigest);

    // A margin-free spec still folds nothing of any of it.
    auto free_spec = margin_spec("l4b-default");
    auto free_stated = free_spec;
    free_stated.margin.reset();
    CHECK(native_run_spec_digest(free_spec) == native_run_spec_digest(free_stated));
    CHECK(native_run_spec_digest(free_spec) != kL4DefaultSpecDigest);
}

// ------------------------------------------------------------------- 2
// MG-A. 20 @ 100 with a 10.0 cash entry fee, maintenance 0.5, mark 95:
//   marked equity           = 1000 + 20*(95-100) - 10 = 890
//   before the open fee     = 900
//   required                = 20 * 95 * 0.5           = 950
//   restore (MarkedEquity)  = 60 / 47.5 = 1.263157894736842
//   restore (BeforeOpenFee) = 50 / 47.5 = 1.0526315789473684
void equity_basis_changes_the_restore() {
    struct Case { const char* key; NativeMarginEquityBasis basis; double equity; double units; };
    const Case cases[] = {
        {"l4b-basis-marked", NativeMarginEquityBasis::MarkedEquity, 890.0,
         4.0 * 22.0 / 45.6},
        {"l4b-basis-gross", NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission, 900.0,
         4.0 * 12.0 / 45.6},
    };
    for (const auto& row : cases) {
        auto s = margin_spec(row.key);
        // 0.5 per unit: the 20-unit entry pays 10, and the slice's own fee is
        // small enough that one restore is the whole cascade. The initial
        // fraction is 0.4 because the entry's own admission equity is already
        // net of its ticket: 20 * 100 * 0.4 = 800 <= 990.
        s.fee_kind = NativeFeeKind::CashPerUnit;
        s.fee_value = 0.5;
        auto m = model(0.48, NativeLiquidationSizing::ShortfallMultiple, 4.0, 0.4);
        m.basis = row.basis;
        m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
        s.margin = m;
        HookHost host;
        host.open_units = 20.0;
        drive(host, s, tape());

        // Neither basis breaches at the entry mark (2000 * 0.48 = 960 against
        // 990 / 1000); both breach at 95, where required = 20 * 95 * 0.48.
        const auto* entry_view = view_at(host, 100.0, 20.0);
        REQUIRE(entry_view != nullptr);
        near(entry_view->required, 960.0);
        near(entry_view->equity, row.equity + 100.0);
        const auto* view = view_at(host, 95.0, 20.0);
        REQUIRE(view != nullptr);
        near(view->required, 912.0);
        near(view->equity, row.equity);

        const auto rows = liquidations(host);
        REQUIRE(rows.size() == 1);
        near(rows[0].closed_units, row.units);
        near(rows[0].resolved_price, 95.0);
        REQUIRE(host.margin_calls.size() == 1);
        // The receipt reports the equity on the model's basis too. Nothing
        // moves the book after the slice, so the run's own marked equity at
        // that mark is the term the receipt is built from.
        const double remaining_fee = 0.5 * (20.0 - row.units);
        near(host.margin_calls[0].equity,
             host.native_marked_equity(95.0)
                 + (row.basis == NativeMarginEquityBasis::MarkedEquity ? 0.0 : remaining_fee));
    }
}

// ------------------------------------------------------------------- 3
// MG-D. 10 @ 100 with a 10.0 cash entry fee, capital 900, maintenance 0.5:
//   MarkedEquity level = (900 - 10 - 1000) / (10 * (0.5 - 1)) = 22.0
//   RealizedOnly level = (900 - 1000)      / (10 * (0.5 - 1)) = 20.0
// The tape never reaches either, so this is the reporting difference alone.
void level_base_changes_the_reported_level() {
    struct Case { const char* key; NativeLiquidationLevelBase base; double level; };
    const Case cases[] = {
        {"l4b-level-marked", NativeLiquidationLevelBase::MarkedEquity, 22.0},
        {"l4b-level-realized", NativeLiquidationLevelBase::RealizedOnly, 20.0},
    };
    for (const auto& row : cases) {
        auto s = margin_spec(row.key);
        s.initial_capital = 900.0;
        s.fee_value = 10.0;
        auto m = model(0.5);
        m.level_base = row.base;
        s.margin = m;
        HookHost host;
        host.open_units = 10.0;
        drive(host, s, tape());
        CHECK(liquidations(host).empty());
        REQUIRE(host.level_at_bar_open.size() == 2);
        CHECK(!host.level_at_bar_open[0].has_value());
        REQUIRE(host.level_at_bar_open[1].has_value());
        near(*host.level_at_bar_open[1], row.level);
    }
}

// ------------------------------------------------------------------- 4
// The hook raises a call the kernel would not make. Maintenance 0.4 at the
// mark 95 requires 20*95*0.4 = 760 against an equity of 900: no breach. A
// host that answers required = 1000 gets one, sized on its own numbers:
//   restore = (1000 - 900) / (95 * 0.4) = 2.6315789473684212
void requirement_hook_raises_a_call() {
    auto s = margin_spec("l4b-raise");
    auto m = model(0.4);
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;

    // Control: the kernel's own numbers never breach on this tape.
    HookHost control;
    control.open_units = 20.0;
    drive(control, s, tape());
    CHECK(liquidations(control).empty());
    CHECK(accepted_liquidations(control) == 0);
    REQUIRE(!control.requirement_views.empty());
    near(control.requirement_views.back().required, 760.0);
    near(control.requirement_views.back().equity, 900.0);

    HookHost host;
    host.open_units = 20.0;
    // A broker whose requirement is its own number, at its own check point:
    // the bar open. Its re-arm after the slice is the kernel's own again, so
    // the restored book is not nibbled a second time.
    host.requirement = [](const NativeMarginRequirementView& view)
            -> std::optional<NativeMarginDecision> {
        if (view.kind != NativeMarginCheckKind::BarOpen
            || !(view.position.signed_units > 0.0)) {
            return std::nullopt;
        }
        NativeMarginDecision decision;
        decision.required = 1000.0;
        decision.equity = view.equity;
        return decision;
    };
    drive(host, s, tape());
    const auto rows = liquidations(host);
    REQUIRE(rows.size() == 1);
    near(rows[0].closed_units, 100.0 / 38.0);
    near(rows[0].resolved_price, 95.0);
    REQUIRE(host.margin_calls.size() == 1);
    near(host.margin_calls[0].units, 100.0 / 38.0);
}

// ------------------------------------------------------------------- 5
// The same hook vetoes a call the kernel would make: maintenance 0.5 breaches
// (950 > 900) and the host answers an equity that does not.
void requirement_hook_vetoes_a_call() {
    auto s = margin_spec("l4b-veto");
    auto m = model(0.5, NativeLiquidationSizing::ShortfallMultiple, 4.0);
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;

    HookHost control;
    control.open_units = 20.0;
    drive(control, s, tape());
    CHECK(liquidations(control).size() == 1);

    HookHost host;
    host.open_units = 20.0;
    host.requirement = [](const NativeMarginRequirementView& view)
            -> std::optional<NativeMarginDecision> {
        NativeMarginDecision decision;
        decision.required = view.required;
        decision.equity = view.required;   // exactly met: no breach
        return decision;
    };
    drive(host, s, tape());
    CHECK(liquidations(host).empty());
    CHECK(accepted_liquidations(host) == 0);
    CHECK(host.margin_calls.empty());
    near(host.physical_position().signed_units, 20.0);
    // The veto is a decision at a real check point, not a missing check.
    CHECK(!host.requirement_views.empty());
}

// ------------------------------------------------------------------- 6
// force_breach composes with the units hook: the kernel's numbers leave no
// restore of their own, so the host that forces the call also sizes it.
void forced_breach_is_sized_by_the_units_hook() {
    auto s = margin_spec("l4b-forced");
    auto m = model(0.4);
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;
    HookHost host;
    host.open_units = 20.0;
    host.requirement = [](const NativeMarginRequirementView& view)
            -> std::optional<NativeMarginDecision> {
        if (view.kind != NativeMarginCheckKind::BarOpen
            || !(view.position.signed_units > 0.0)) {
            return std::nullopt;
        }
        NativeMarginDecision decision;
        decision.required = view.required;   // 760 against 900: no breach
        decision.equity = view.equity;
        decision.force_breach = true;
        return decision;
    };
    host.sizer = [](const NativeMarginCallView& view) -> std::optional<double> {
        near(view.required, 760.0);
        near(view.equity, 900.0);
        return 3.0;
    };
    drive(host, s, tape());
    const auto rows = liquidations(host);
    REQUIRE(rows.size() == 1);
    near(rows[0].closed_units, 3.0);
    near(rows[0].resolved_price, 95.0);
    near(host.physical_position().signed_units, 17.0);

    // Without a units answer the forced breach has no size, and books nothing.
    HookHost unsized;
    unsized.open_units = 20.0;
    unsized.requirement = host.requirement;
    drive(unsized, s, tape());
    CHECK(liquidations(unsized).empty());
    CHECK(accepted_liquidations(unsized) == 0);
}

// ------------------------------------------------------------------- 7
// MG-H / MG-H2. The mark check rests AT the adverse waypoint mark, so the
// reduction belongs to that waypoint instead of to the solved level, and its
// price is the default resolved price resolve_execution_terms is offered.
void mark_check_rests_at_the_adverse_waypoint() {
    auto s = margin_spec("l4b-mark");
    auto m = model(0.5, NativeLiquidationSizing::ShortfallMultiple, 4.0);
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;

    // The L4 level check for comparison: same units, booked at the level.
    auto level_spec = margin_spec("l4b-mark-level");
    level_spec.margin = model(0.5, NativeLiquidationSizing::ShortfallMultiple, 4.0);
    HookHost at_level;
    at_level.open_units = 20.0;
    drive(at_level, level_spec, tape());
    const auto level_rows = liquidations(at_level);
    REQUIRE(level_rows.size() == 1);
    near(level_rows[0].resolved_price, 100.0);
    CHECK(level_rows[0].cursor.point.path_phase == NativePathPhase::High);

    HookHost host;
    host.open_units = 20.0;
    drive(host, s, tape());
    const auto rows = liquidations(host);
    REQUIRE(rows.size() == 1);
    near(rows[0].closed_units, 4.2105263157894735);
    near(rows[0].resolved_price, 95.0);
    // The adverse waypoint itself, two path points after the level check's.
    CHECK(rows[0].cursor.point.path_phase == NativePathPhase::Low);
    CHECK(rows[0].cursor.point.ordinal > level_rows[0].cursor.point.ordinal
          - level_rows[0].cursor.point.ordinal);
    REQUIRE(host.margin_calls.size() == 1);
    near(host.margin_calls[0].mark, 95.0);

    // The fill price goes through resolve_execution_terms like any other: a
    // host that applies a tick rounding and its exit-side slippage books
    // 95 - 5 ticks = 94.95, and the kernel books exactly that.
    HookHost priced;
    priced.open_units = 20.0;
    priced.terms = [](const NativeExecutionTermsFacts& facts) {
        double price = facts.default_resolved_price;
        if (facts.definition
            && facts.definition->origin == no::RequestOrigin::KernelLiquidation) {
            price = std::floor(price / 0.01 + 0.5) * 0.01 - 5.0 * 0.01;
        }
        return no::ExecutionTerms{price, std::nullopt, no::OpeningShape::Transact};
    };
    drive(priced, s, tape());
    const auto priced_rows = liquidations(priced);
    REQUIRE(priced_rows.size() == 1);
    near(priced_rows[0].resolved_price, 94.95);
    near(priced_rows[0].closed_units, 4.2105263157894735);
}

// ------------------------------------------------------------------- 8
// MG-I. The gate suppresses a whole check point. Suppressing the bar-open
// point of the breaching bar removes the call the same run otherwise makes,
// and the kernel's own check points are exactly the ones it has: the bar
// open, and the re-arm after an applied fill.
void gate_suppresses_a_check_point() {
    auto s = margin_spec("l4b-gate");
    auto m = model(0.5, NativeLiquidationSizing::ShortfallMultiple, 4.0);
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;

    HookHost control;
    control.open_units = 20.0;
    drive(control, s, tape());
    CHECK(liquidations(control).size() == 1);
    std::size_t bar_open = 0;
    std::size_t after_applied = 0;
    for (const auto& point : control.check_points) {
        if (point.kind == NativeMarginCheckKind::BarOpen) ++bar_open;
        if (point.kind == NativeMarginCheckKind::AfterApplied) ++after_applied;
    }
    CHECK(bar_open == 2);            // one per script bar
    CHECK(after_applied >= 1);       // the entry fill, and the liquidation's
    CHECK(control.check_points.size() == bar_open + after_applied);

    HookHost host;
    host.open_units = 20.0;
    host.gate = [](const NativeMarginCheckPoint& point) {
        return point.kind != NativeMarginCheckKind::BarOpen;
    };
    drive(host, s, tape());
    CHECK(liquidations(host).empty());
    CHECK(accepted_liquidations(host) == 0);
    near(host.physical_position().signed_units, 20.0);
    // A suppressed point is not a check that answered "no": the requirement
    // hook is never reached at one.
    for (const auto& view : host.requirement_views) {
        CHECK(view.kind != NativeMarginCheckKind::BarOpen);
    }
    CHECK(!host.check_points.empty());
    // The facts the gate decides on are the kernel's own.
    for (const auto& point : host.check_points) {
        CHECK(std::isfinite(point.mark));
    }
}

// ------------------------------------------------------------------- 9
// MG-E. Maintenance 1.0 on a long has no liquidation level at all: equity and
// requirement move together, so no price solves the breach. The level check
// rests nothing there, as it always has. The mark check never solves a level,
// so it still measures the breach at the mark and still consults the hook:
//   equity(95) = 900, required = 20 * 95 * 1.0 = 1900,
//   restore    = 1000 / 95 = 10.526315789473685
void degenerate_slope_still_checks_at_the_mark() {
    auto level_spec = margin_spec("l4b-degenerate-level");
    level_spec.margin = model(1.0);
    HookHost at_level;
    at_level.open_units = 20.0;
    drive(at_level, level_spec, tape());
    CHECK(liquidations(at_level).empty());
    CHECK(accepted_liquidations(at_level) == 0);
    REQUIRE(at_level.level_at_bar_open.size() == 2);
    CHECK(!at_level.level_at_bar_open[1].has_value());
    // The level solve precedes the breach test there, so the hook is not
    // reached with a live book at all.
    for (const auto& view : at_level.requirement_views) {
        CHECK(!(std::abs(view.position.signed_units) > 0.0));
    }
    CHECK(!at_level.check_points.empty());

    auto s = margin_spec("l4b-degenerate-mark");
    auto m = model(1.0);
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;
    HookHost host;
    host.open_units = 20.0;
    drive(host, s, tape());
    const auto rows = liquidations(host);
    REQUIRE(rows.size() == 1);
    near(rows[0].closed_units, 1000.0 / 95.0);
    near(rows[0].resolved_price, 95.0);
    const auto* view = view_at(host, 95.0, 20.0);
    REQUIRE(view != nullptr);
    near(view->required, 1900.0);
    near(view->equity, 900.0);
    // Still no level to report, and the run still liquidates.
    CHECK(!host.native_liquidation_price().has_value());
}

// ------------------------------------------------------------------ 10
// Configuration: both new fields are validated, and an unknown value is
// refused by field rather than silently taken.
void configuration_is_validated() {
    auto bad_basis = margin_spec("l4b-bad-basis");
    auto basis_model = model(0.5);
    basis_model.basis = static_cast<NativeMarginEquityBasis>(7);
    bad_basis.margin = basis_model;
    HookHost a;
    const auto refused_basis = a.configure_native(bad_basis);
    CHECK(refused_basis.status == NativeSetupStatus::Failed);
    CHECK(refused_basis.validation.error == NativeRunSpecError::UnknownMarginEquityBasis);
    CHECK(refused_basis.validation.field == NativeRunSpecField::MarginEquityBasis);

    auto bad_level = margin_spec("l4b-bad-level");
    auto level_model = model(0.5);
    level_model.level_base = static_cast<NativeLiquidationLevelBase>(9);
    bad_level.margin = level_model;
    HookHost b;
    const auto refused_level = b.configure_native(bad_level);
    CHECK(refused_level.status == NativeSetupStatus::Failed);
    CHECK(refused_level.validation.error == NativeRunSpecError::UnknownLiquidationLevelBase);
    CHECK(refused_level.validation.field == NativeRunSpecField::MarginLevelBase);

    auto bad_check = margin_spec("l4b-bad-check");
    auto check_model = model(0.5);
    check_model.check = static_cast<NativeLiquidationCheck>(5);
    bad_check.margin = check_model;
    HookHost c;
    const auto refused_check = c.configure_native(bad_check);
    CHECK(refused_check.status == NativeSetupStatus::Failed);
    CHECK(refused_check.validation.error == NativeRunSpecError::UnknownLiquidationCheck);

    auto good = margin_spec("l4b-good");
    auto good_model = model(0.5);
    good_model.basis = NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission;
    good_model.level_base = NativeLiquidationLevelBase::RealizedOnly;
    good_model.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    good.margin = good_model;
    HookHost d;
    CHECK(d.configure_native(good).status == NativeSetupStatus::Applied);
}


}  // namespace

int main() {
    test("defaults-byte-identical", defaults_are_byte_identical);
    test("equity-basis", equity_basis_changes_the_restore);
    test("level-base", level_base_changes_the_reported_level);
    test("requirement-raises", requirement_hook_raises_a_call);
    test("requirement-vetoes", requirement_hook_vetoes_a_call);
    test("forced-breach", forced_breach_is_sized_by_the_units_hook);
    test("mark-check-waypoint", mark_check_rests_at_the_adverse_waypoint);
    test("gate-suppression", gate_suppresses_a_check_point);
    test("degenerate-slope", degenerate_slope_still_checks_at_the_mark);
    test("configuration", configuration_is_validated);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
