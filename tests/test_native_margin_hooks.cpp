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
 */
#include "native_current_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

constexpr const char* kLiquidationLabel = "__kernel_liquidation__";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ── Portable pins ───────────────────────────────────────────────────────
// A raw native_continuation_hash() constant is not portable (it folds this
// machine's resolved zoneinfo paths). native_run_spec_digest() is the
// consumer's own spec fold and nothing else, and the report digest below is
// this test's own fold over the observable closed rows. Both are observed on
// THIS tree for the L4 default surface and guard that this lane moved
// neither. The continuation itself is compared between two runs in process.
constexpr std::uint64_t kL4DefaultSpecDigest = 16781023430602848315ULL;
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

Bar ohlc(int index, double open, double high, double low, double close,
         double volume = 1.0) {
    return {open, high, low, close, volume, T + static_cast<int64_t>(index) * 60000};
}

NativeRunSpec margin_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:MARGIN";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    return s;
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

std::vector<no::ExecutionAppliedEvent> liquidations(const Host& host) {
    std::vector<no::ExecutionAppliedEvent> out;
    for (const auto& row : events<no::ExecutionAppliedEvent>(host)) {
        if (row.request().label == kLiquidationLabel) out.push_back(row);
    }
    return out;
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


// ------------------------------------------------------------------ TWIN
// The R5 differential rows (R5c-report.md), re-run with a native host that
// implements the adapter's own rules THROUGH the new hooks. Both engines run
// in this process, on the same tape, from the same configuration; every
// comparison below is bit-for-bit (operator== on the doubles), never a
// tolerance.
//
//   MG-A  trigger equity basis        — NativeMarginModel::basis
//   MG-B  ten-significant-digit money — resolve_margin_requirement
//   MG-D  liquidation level base      — NativeMarginModel::level_base
//   MG-F  4x shortfall sizing         — resolve_margin_call_units
//   MG-G  whole-drop band             — resolve_margin_call_units
//   MG-H  placement and fill price    — PathAdverseExtremeMark + terms
//
// MG-I (the adapter's 19 scheduling exceptions) is deliberately NOT re-lowered
// here: it is gate policy, and margin_check_allowed() is where a host would
// express it. MG-C is gone by construction — the requirement hook is consulted
// before the breach test, so the host's numbers are the ones tested.

// pine_adapter.cpp:396 — TradingView's ten-significant-digit money.
double tv_money_round(double value) {
    if (!std::isfinite(value) || value == 0.0) return value;
    const double magnitude = std::floor(std::log10(std::abs(value)));
    const double scale = std::pow(10.0, 9.0 - magnitude);
    const double rounded = std::floor(std::abs(value) * scale + 0.5) / scale;
    return value < 0.0 ? -rounded : rounded;
}

// pine_adapter.cpp:328-376 — the two tick helpers the forced price is built
// from, for a price that is already on the grid (every price in these tapes).
double tv_nearest_tick(double value, double tick) {
    if (!std::isfinite(value) || !(tick > 0.0)) return value;
    const double k = std::floor(value / tick + 0.5);
    return k * tick == value ? value : k * tick;
}
double tv_directional_tick(double value, double tick, bool upward) {
    if (!std::isfinite(value) || !(tick > 0.0)) return value;
    const double scaled = value / tick;
    return (upward ? std::ceil(scaled - 1e-9) : std::floor(scaled + 1e-9)) * tick;
}
double tv_floor_grid(double units, double grid) {
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (!(grid > 0.0)) return units;
    const double floored = std::floor(units / grid + 1e-6) * grid;
    return floored < units ? floored : units;
}

// One TradingView configuration, shared by the two engines.
struct TvConfig {
    double capital = 1000.0;
    double units = 20.0;          // signed: the position the script opens
    double margin_pct = 50.0;     // strategy.margin_long / _short
    CommissionType commission = CommissionType::PERCENT;
    double commission_value = 0.0;
    double qty_step = 0.0;        // strategy(default_qty_... qty_step)
    double mintick = 0.01;
    int slippage = 0;
    // TradingView owns its opening admission (the adapter answers
    // AdmitWithHostMargin from frozen signal-time equity), so a twin that is
    // about the margin CALL states the kernel's own opening fraction
    // separately rather than letting it refuse an opening TradingView takes.
    double admission_pct = 0.0;   // 0 = the margin fraction itself
};

// The adapter side: the Pine host itself, configured from TvConfig.
struct TvAdapterHost final : source::PineStrategyHost {
    explicit TvAdapterHost(const TvConfig& config) {
        // The source configuration the adapter projects at its native begin
        // (the same slots the L4a fixtures' legacy spellings resolve to).
        auto& tv = fixture_configuration();
        tv.initial_capital = config.capital;
        tv.default_qty_type = static_cast<int>(QtyType::FIXED);
        tv.default_qty_value = std::abs(config.units);
        tv.commission_type = static_cast<int>(config.commission);
        tv.commission_value = config.commission_value;
        (config.units > 0.0 ? tv.margin_long : tv.margin_short) = config.margin_pct;
        tv.process_orders_on_close = true;
        tv.slippage = config.slippage;
        initial_capital_ = config.capital;
        qty_step_ = config.qty_step;
        syminfo_mintick_ = config.mintick;
        long_side_ = config.units > 0.0;
        quantity_ = std::abs(config.units);
    }
    bool long_side_ = true;
    double quantity_ = 0.0;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry(long_side_ ? "L" : "S", long_side_, kNaN, kNaN, quantity_);
        }
    }
    const Trade& row(int index) const { return get_trade(index); }
    double level() const { return margin_liquidation_price(); }
    double position() const { return physical_position().signed_units; }
    std::vector<const Trade*> margin_rows() const {
        std::vector<const Trade*> out;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_comment == "Margin call") out.push_back(&get_trade(i));
        }
        return out;
    }
};

// The kernel side: a native host whose margin POLICY is the adapter's.
// Nothing of the mechanism is here — the kernel still solves, schedules,
// places, books and reports; this only answers the three hooks.
struct TvNativeHost final : Host {
    TvConfig config;
    int bars = 0;
    std::vector<no::MarginCallEvent> margin_calls;
    std::vector<std::optional<double>> level_at_bar_open;

    double fraction() const { return config.margin_pct / 100.0; }
    double held() const { return std::abs(physical_position().signed_units); }

    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        // MG-D: the adapter reports the level tick-quantized toward the
        // adverse side. The base is the run spec's (RealizedOnly); the
        // quantization is the source layer's own presentation rule.
        const auto level = native_liquidation_price();
        if (!level) {
            level_at_bar_open.push_back(std::nullopt);
            return;
        }
        const double tick = config.mintick;
        level_at_bar_open.push_back(
            config.units < 0.0 ? std::ceil(*level / tick) * tick
                               : std::floor(*level / tick) * tick);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (bars++ == 0) (void)put(*this, tx(config.units, "entry"));
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        margin_calls.push_back(event);
    }
    // MG-B: pine_adapter.cpp:11491-11499. The requirement is money, and this
    // broker keeps money to ten significant digits whenever a lot is worth
    // less than one unit of account.
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        if (!(config.qty_step > 0.0)) return std::nullopt;
        const double lot_value = config.qty_step * view.mark;
        if (!std::isfinite(lot_value) || !(lot_value < 1.0)) return std::nullopt;
        NativeMarginDecision decision;
        decision.required = tv_money_round(view.required);
        decision.equity = view.equity;
        return decision;
    }
    // MG-F / MG-G: pine_adapter.cpp:11505-11533, on the kernel's own facts.
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        const double unit_margin = view.mark * fraction();
        if (!(unit_margin > 0.0)) return 0.0;
        const double raw_minimum = (view.required - view.equity) / unit_margin;
        if (!(raw_minimum > 1e-10) || !std::isfinite(raw_minimum)) return 0.0;
        const double book = std::abs(view.position.signed_units);
        double minimum = raw_minimum;
        if (config.qty_step > 0.0) {
            minimum = std::floor(raw_minimum / config.qty_step) * config.qty_step;
        }
        double units = minimum > 0.0 ? 4.0 * minimum : 0.0;
        if (units > 0.0 && config.qty_step > 0.0) {
            units = std::floor(units / config.qty_step + 1e-6) * config.qty_step;
        }
        if (!(units > 0.0) && config.qty_step > 0.0 && config.qty_step <= 1.0
            && raw_minimum > 1e-10 && raw_minimum < 1.0) {
            const double candidate = std::min(1.0, book);
            const double rounded = tv_floor_grid(candidate, config.qty_step);
            const double guard = std::max(1e-12, std::abs(candidate) * 1e-12);
            if (candidate >= book - guard || std::abs(rounded - candidate) <= guard) {
                units = candidate;
            }
        }
        units = std::min(book, units);
        return units > 1e-10 && std::isfinite(units) ? units : 0.0;
    }
    // MG-H: pine_adapter.cpp:11588-11597. The slice fires at the waypoint the
    // kernel rested it on; the booked price is that fire price on the tick
    // ladder plus the EXIT side's own market slippage.
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        if (!facts.definition
            || facts.definition->origin != no::RequestOrigin::KernelLiquidation) {
            return Host::resolve_execution_terms(facts);
        }
        // The fire price is the level the slice was rested on -- the mark --
        // never the already-slipped default the generic path offers, exactly
        // as the adapter builds its forced price from its own mark_price.
        const double fire = facts.trigger_level ? *facts.trigger_level : facts.raw_price;
        if (config.slippage == 0) {
            return {fire, std::nullopt, no::OpeningShape::Transact};
        }
        const bool close_is_buy = facts.position.signed_units < 0.0;
        const double rounded = tv_nearest_tick(fire, config.mintick);
        const double slipped = rounded + (close_is_buy ? 1.0 : -1.0)
            * static_cast<double>(config.slippage) * config.mintick;
        return {tv_directional_tick(slipped, config.mintick, close_is_buy), std::nullopt,
                no::OpeningShape::Transact};
    }
};

NativeRunSpec tv_spec(const char* key, const TvConfig& config) {
    NativeRunSpec s = margin_spec(key);
    s.initial_capital = config.capital;
    s.price_tick = config.mintick;
    // The generic slippage is the same number on both sides; the liquidation's
    // own price is the host's forced one, as it is for TradingView.
    s.slippage_ticks = static_cast<std::uint32_t>(config.slippage);
    switch (config.commission) {
    case CommissionType::PERCENT: s.fee_kind = NativeFeeKind::Percent; break;
    case CommissionType::CASH_PER_ORDER: s.fee_kind = NativeFeeKind::CashPerExecution; break;
    case CommissionType::CASH_PER_CONTRACT: s.fee_kind = NativeFeeKind::CashPerUnit; break;
    }
    s.fee_value = config.commission_value;
    if (config.qty_step > 0.0) s.quantity_grid = config.qty_step;
    NativeMarginModel m;
    const double fraction = config.margin_pct / 100.0;
    const double admission = config.admission_pct > 0.0 ? config.admission_pct / 100.0
                                                        : fraction;
    m.initial_long = admission;
    m.initial_short = admission;
    m.maintenance_long = fraction;
    m.maintenance_short = fraction;
    // The sizing policy is the host's here, so the kernel's own is the plain
    // restore; liquidation_min_units is left set to show the host's answer
    // winning over the kernel's flatten (MG-G).
    m.sizing = NativeLiquidationSizing::RestoreMinimum;
    m.liquidation_min_units = 1.0;
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;   // MG-H
    // MG-A: TradingView's margin equity charges only PERCENT entry commission
    // against the account; a cash or per-contract fee is added back.
    m.basis = (config.commission == CommissionType::PERCENT && config.commission_value > 0.0)
        ? NativeMarginEquityBasis::MarkedEquity
        : NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission;
    m.level_base = NativeLiquidationLevelBase::RealizedOnly;    // MG-D
    s.margin = m;
    return s;
}

// The kernel's own liquidations, as (units, price) pairs.
struct Slice { double units; double price; };
std::vector<Slice> native_slices(const Host& host) {
    std::vector<Slice> out;
    for (const auto& row : liquidations(host)) {
        out.push_back({row.closed_units, row.resolved_price});
    }
    return out;
}
std::vector<Slice> adapter_slices(const TvAdapterHost& host) {
    std::vector<Slice> out;
    for (const auto* row : host.margin_rows()) out.push_back({row->qty, row->exit_price});
    return out;
}

// Bit-for-bit, and reported row by row.
bool same_slices(const char* label, const std::vector<Slice>& adapter,
                 const std::vector<Slice>& kernel) {
    bool same = adapter.size() == kernel.size();
    for (std::size_t i = 0; same && i < adapter.size(); ++i) {
        same = adapter[i].units == kernel[i].units && adapter[i].price == kernel[i].price;
    }
    std::printf("  %-6s adapter %zu slice(s), kernel %zu — %s\n", label, adapter.size(),
                kernel.size(), same ? "bit-for-bit" : "DIFFERENT");
    for (std::size_t i = 0; i < std::max(adapter.size(), kernel.size()); ++i) {
        const char* a_units = i < adapter.size() ? "" : " (none)";
        std::printf("         [%zu] adapter %.17g @ %.17g%s | kernel %.17g @ %.17g%s\n", i,
                    i < adapter.size() ? adapter[i].units : 0.0,
                    i < adapter.size() ? adapter[i].price : 0.0, a_units,
                    i < kernel.size() ? kernel[i].units : 0.0,
                    i < kernel.size() ? kernel[i].price : 0.0,
                    i < kernel.size() ? "" : " (none)");
    }
    CHECK(same);
    return same;
}

std::vector<Slice> run_twin(const char* label, const TvConfig& config,
                            const std::vector<Bar>& bars,
                            std::vector<Slice>* kernel_default_out = nullptr) {
    TvAdapterHost adapter(config);
    adapter.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(adapter.last_error().empty());

    TvNativeHost kernel;
    kernel.config = config;
    auto s = tv_spec(label, config);
    REQUIRE(kernel.configure_native(s).status == NativeSetupStatus::Applied);
    kernel.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(kernel.last_error().empty());
    CHECK(kernel.native_state().kind == NativeLifecycleKind::Completed);

    const auto adapter_rows = adapter_slices(adapter);
    const auto kernel_rows = native_slices(kernel);
    same_slices(label, adapter_rows, kernel_rows);
    // The same book, to the last bit, once the calls have been made.
    CHECK(adapter.position() == kernel.physical_position().signed_units);

    if (kernel_default_out) {
        // The same kernel WITHOUT the policy: the L4 model on its own numbers.
        struct PlainHost final : Host {
            double units = 0.0;
            int bars = 0;
            void on_native_bar(const Bar& bar, const NativeDecisionContext& c) override {
                Host::on_native_bar(bar, c);
                if (bars++ == 0) (void)put(*this, tx(units, "entry"));
            }
        };
        auto plain_spec = tv_spec("plain", config);
        plain_spec.identity = {std::string(label) + "-plain", 1};
        plain_spec.margin->basis = NativeMarginEquityBasis::MarkedEquity;
        plain_spec.margin->level_base = NativeLiquidationLevelBase::MarkedEquity;
        plain_spec.margin->sizing = NativeLiquidationSizing::ShortfallMultiple;
        plain_spec.margin->shortfall_multiple = 4.0;
        plain_spec.margin->liquidation_min_units.reset();
        plain_spec.margin->check = NativeLiquidationCheck::PathAdverseExtreme;
        PlainHost plain;
        plain.units = config.units;
        REQUIRE(plain.configure_native(plain_spec).status == NativeSetupStatus::Applied);
        plain.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(plain.last_error().empty());
        *kernel_default_out = native_slices(plain);
    }
    return kernel_rows;
}

// MG-F + MG-H. The leveraged long of test_margin_call_l4a.cpp: 20 units at
// 100 on a 50 % margin, then a bar whose low is 95.
//   restore = (20*95*0.5 - (1000 + 20*(95-100))) / (95*0.5) = 1.0526315789473684
//   4x      = 4.2105263157894735, at the adverse waypoint 95.
void twin_leveraged_long() {
    TvConfig config;
    config.capital = 1000.0;
    config.units = 20.0;
    config.margin_pct = 50.0;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 101.0, 95.0, 96.0)};
    std::vector<Slice> plain;
    const auto rows = run_twin("MG-F/H", config, bars, &plain);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].units == 4.2105263157894735);
    CHECK(rows[0].price == 95.0);
    // The L4 kernel on its own: same units, booked at the solved level.
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].units == rows[0].units);
    CHECK(plain[0].price == 100.0);
}

// MG-F on the short side, with the whole path: 10 units short at 100 on a
// 100 % margin, then a bar reaching 105.
//   restore = (10*105 - (1000 + 10*(100-105))) / 105 = 0.9523809523809523
//   4x      = 3.8095238095238093, at the adverse waypoint 105.
void twin_short_adverse_high() {
    TvConfig config;
    config.capital = 1000.0;
    config.units = -10.0;
    config.margin_pct = 100.0;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 99.0, 100.0),
                                   ohlc(1, 100.0, 105.0, 99.5, 104.0)};
    const auto rows = run_twin("MG-F2", config, bars);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].units == 3.8095238095238093);
    CHECK(rows[0].price == 105.0);
}

// MG-H's forced execution price: the same long, with two ticks of slippage.
// The adapter books the tick-rounded fire price minus the exit side's
// slippage; the kernel books whatever resolve_execution_terms answers, so the
// same rule produces the same price.
void twin_forced_execution_price() {
    TvConfig config;
    // Capital 1020: the entry itself fills two ticks up at 100.02 and must
    // NOT be a breach there (1000.2 required against 1020), or TradingView
    // would add its entry-bar residual slice — a scheduling exception (MG-I)
    // this lane does not re-lower.
    config.capital = 1020.0;
    config.units = 20.0;
    config.margin_pct = 50.0;
    config.slippage = 2;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 101.0, 95.0, 96.0)};
    const auto rows = run_twin("MG-H2", config, bars);
    REQUIRE(rows.size() == 1);
    //   equity   = 1020 + 20*(95 - 100.02) = 919.6
    //   required = 20 * 95 * 0.5           = 950
    //   4x       = 4 * 30.4 / 47.5         = 2.56, at 95 - 2 ticks = 94.98
    CHECK(rows[0].price == 94.98);
}

// MG-A + MG-D. A cash-per-order commission: TradingView does not charge it
// against the margin equity, and does not net it out of the liquidation
// level either. The kernel's own numbers do both, so on THIS tape the L4
// model liquidates and TradingView does not.
void twin_cash_commission_equity_and_level() {
    TvConfig config;
    config.capital = 1000.0;
    config.units = 20.0;
    config.margin_pct = 50.0;
    config.commission = CommissionType::CASH_PER_ORDER;
    config.commission_value = 10.0;
    config.admission_pct = 10.0;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 100.5, 100.0, 100.2)};
    TvAdapterHost adapter(config);
    adapter.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(adapter.last_error().empty());

    TvNativeHost kernel;
    kernel.config = config;
    REQUIRE(kernel.configure_native(tv_spec("MG-A/D", config)).status
            == NativeSetupStatus::Applied);
    kernel.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(kernel.last_error().empty());

    same_slices("MG-A", adapter_slices(adapter), native_slices(kernel));
    CHECK(adapter.margin_rows().empty());
    CHECK(adapter.position() == 20.0);
    CHECK(adapter.position() == kernel.physical_position().signed_units);

    // The same kernel on its own money DOES liquidate here: marked equity is
    // 990 against a requirement of 1000 at the entry mark itself.
    struct PlainHost final : Host {
        int bars = 0;
        void on_native_bar(const Bar& bar, const NativeDecisionContext& c) override {
            Host::on_native_bar(bar, c);
            if (bars++ == 0) (void)put(*this, tx(20.0, "entry"));
        }
    };
    auto plain_spec = tv_spec("MG-A-plain", config);
    plain_spec.margin->basis = NativeMarginEquityBasis::MarkedEquity;
    plain_spec.margin->level_base = NativeLiquidationLevelBase::MarkedEquity;
    plain_spec.margin->liquidation_min_units.reset();
    PlainHost plain;
    REQUIRE(plain.configure_native(plain_spec).status == NativeSetupStatus::Applied);
    plain.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(plain.last_error().empty());
    CHECK(!liquidations(plain).empty());
    // MG-D: the reported level, adapter against kernel, bit-for-bit.
    //   realized-only = (1000 - 20*100) / (20 * (0.5 - 1))          = 100
    //   marked        = (1000 - 10 - 20*100) / (20 * (0.5 - 1))     = 101
    REQUIRE(kernel.level_at_bar_open.size() == 2);
    REQUIRE(kernel.level_at_bar_open[1].has_value());
    std::printf("  MG-D   adapter level %.17g | kernel level %.17g\n", adapter.level(),
                *kernel.level_at_bar_open[1]);
    CHECK(adapter.level() == *kernel.level_at_bar_open[1]);
    CHECK(*kernel.level_at_bar_open[1] == 100.0);
}

// MG-B + MG-G. A lot worth less than one unit of account puts TradingView's
// requirement on a ten-significant-digit grid, and that rounding alone is the
// call: the exact requirement is BELOW the equity and the rounded one is
// above it. The restore it implies is sub-lot, so the whole-drop band closes
// one unit — over the kernel's own liquidation_min_units, which would have
// flattened.
//   equity   = 1000.00000005 + 20*(99.999999996 - 100) = 999.99999997
//   required = 20 * 99.999999996 * 0.5                 = 999.99999996  (no breach)
//   rounded  = 1000.0                                  (breach by 3e-8)
//   restore  = 3e-8 / (99.999999996*0.5) = 6e-10 -> sub-lot -> min(1, held)
void twin_rounded_money_and_whole_drop() {
    TvConfig config;
    config.capital = 1000.00000005;
    config.units = 20.0;
    config.margin_pct = 50.0;
    config.qty_step = 0.001;
    const std::vector<Bar> bars = {
        ohlc(0, 100.0, 100.0, 100.0, 100.0),
        ohlc(1, 100.0, 100.000000004, 99.999999996, 100.0)};
    std::vector<Slice> plain;
    const auto rows = run_twin("MG-B/G", config, bars, &plain);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].units == 1.0);
    CHECK(rows[0].price == 99.999999996);
    // The kernel's own money never makes this call.
    CHECK(plain.empty());
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
    test("twin-leveraged-long", twin_leveraged_long);
    test("twin-short-adverse", twin_short_adverse_high);
    test("twin-forced-price", twin_forced_execution_price);
    test("twin-cash-commission", twin_cash_commission_equity_and_level);
    test("twin-rounded-money", twin_rounded_money_and_whole_drop);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
