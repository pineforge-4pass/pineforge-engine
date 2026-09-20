/*
 * test_native_margin_fx_roll.cpp — audit gap lane N6, design row MG9: a step
 * of the run's declared NativeFxCurve is a margin check point of its own.
 *
 * The maintenance requirement Q * P * pv * fx * m is FX-bearing, so an account
 * can cross its maintenance line with every price exactly where it was. The
 * kernel's other check points only see that at the next point of their own —
 * the next script bar's open, the next applied fill — so a rate that steps
 * inside a script bar was measured late, at whatever the price had become.
 * NativeMarginCheckKind::FxRoll is the first driver point the account converts
 * at a different rate than the point before it, offered immediately before
 * that point is matched.
 *
 * Design row FP6 rides on the same clock: a curve declared through
 * configure_native_fx_curve is the run's immutable FX epoch, so a stream of
 * confirmed bars converts — and rolls — exactly as a batch of the same bars
 * does. The stream twin at the end runs every roll scenario both ways.
 *
 * No Pine twin lives here on purpose: the TU reaches no source header, so the
 * kernel-only build (PINEFORGE_BUILD_SOURCE_LAYER=OFF) runs every row.
 *
 * Hand arithmetic (point_value = 1, no fee, capital 1000, LONG 10 @ 100,
 * maintenance 0.5):
 *   equity(P, fx)   = 1000 + (P - 100) * 10 * fx
 *   required(P, fx) = 10 * P * fx * 0.5
 *   at P = 100      : equity = 1000 at every rate, required = 500 * fx
 *   fx = 1.0        : 500 <= 1000, no call
 *   fx = 2.5        : 1250 > 1000, a call with no price move
 *   restore         = (1250 - 1000) / (100 * 2.5 * 0.5) = 2 units
 *                     -> 8 left, required 1000 == equity 1000, restored
 *   level(fx)       = 200 * (fx - 1) / fx : 0 at fx = 1 (nothing rests),
 *                     100 at fx = 2, 120 at fx = 2.5
 */
#include "native_current_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

constexpr int64_t kMinute = 60000;

Bar ohlc(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, T + static_cast<int64_t>(index) * kMinute};
}

Bar calm(int index, double price = 100.0) { return ohlc(index, price, price, price, price); }

NativeRunSpec roll_spec(const char* key, const char* script = "1") {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = script;
    s.tickerid = "TEST:FXROLL";
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

NativeMarginModel model(NativeLiquidationSizing sizing = NativeLiquidationSizing::RestoreMinimum,
                        NativeLiquidationCheck check = NativeLiquidationCheck::PathAdverseExtreme) {
    NativeMarginModel m;
    m.initial_long = 0.5;
    m.initial_short = 0.5;
    m.maintenance_long = 0.5;
    m.maintenance_short = 0.5;
    m.sizing = sizing;
    m.check = check;
    return m;
}

// Opens a literal position at the first calculation and records every check
// point it is offered, every requirement view, every fill and every receipt.
struct RollHost final : Host {
    double open_units = 0.0;
    bool refuse_fx_roll = false;
    std::vector<NativeMarginCheckPoint> points;
    std::vector<NativeMarginRequirementView> views;
    std::vector<no::MarginCallEvent> margin_calls;
    std::vector<no::ExecutionAppliedEvent> fills;
    int bars = 0;

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (bars++ == 0 && open_units != 0.0) (void)put(*this, tx(open_units, "entry"));
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        Host::on_native_applied(event, context);
        fills.push_back(event);
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        margin_calls.push_back(event);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        const_cast<RollHost&>(*this).points.push_back(point);
        return !(refuse_fx_roll && point.kind == NativeMarginCheckKind::FxRoll);
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        const_cast<RollHost&>(*this).views.push_back(view);
        return std::nullopt;
    }

    std::vector<NativeMarginCheckPoint> offered(NativeMarginCheckKind kind) const {
        std::vector<NativeMarginCheckPoint> out;
        for (const auto& point : points) if (point.kind == kind) out.push_back(point);
        return out;
    }
    std::vector<NativeMarginRequirementView> measured(NativeMarginCheckKind kind) const {
        std::vector<NativeMarginRequirementView> out;
        for (const auto& view : views) if (view.kind == kind) out.push_back(view);
        return out;
    }
};

void begin(RollHost& host, const NativeRunSpec& spec, const std::optional<NativeFxCurve>& curve) {
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    if (curve) {
        REQUIRE(host.configure_native_fx_curve(*curve).status == NativeSetupStatus::Applied);
    }
}

void run_bars(RollHost& host, const NativeRunSpec& spec,
              const std::optional<NativeFxCurve>& curve, const std::vector<Bar>& bars) {
    begin(host, spec, curve);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

// ── 1. The acceptance row: the rate moves into a breach, no price moves ──
// Four flat bars at 100. The entry fills at bar 0's close under rate 1.0. The
// curve steps to 2.5 at T+2m, which is bar 1's CLOSE coordinate: the roll is
// the head of bar 1's Close segment, inside script bar 1 and ahead of bar 2's
// own open.
void rate_step_breaches_with_prices_unchanged() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    auto spec = roll_spec("fx-roll-flat");
    spec.margin = model();

    RollHost host;
    host.open_units = 10.0;
    run_bars(host, spec, curve, bars);

    // Exactly one roll in the run, and it is offered as its own kind.
    const auto rolls = host.offered(NativeMarginCheckKind::FxRoll);
    REQUIRE(rolls.size() == 1);
    CHECK(rolls[0].cursor.point.open_ms == T + kMinute);             // inside bar 1
    CHECK(rolls[0].cursor.point.effective_time_ms == T + 2 * kMinute);
    CHECK(rolls[0].cursor.point.path_phase == NativePathPhase::Close);
    CHECK(rolls[0].cursor.t == 0.0);                                 // before the segment
    CHECK(rolls[0].mark == 100.0);                                   // no price moved
    CHECK(rolls[0].position.signed_units == 10.0);
    CHECK(!rolls[0].liquidation_resting);                            // level(1.0) == 0

    // The numbers it measured are the new rate's, at the unchanged price.
    const auto measured = host.measured(NativeMarginCheckKind::FxRoll);
    REQUIRE(measured.size() == 1);
    CHECK(measured[0].mark == 100.0);
    near(measured[0].equity, 1000.0);
    near(measured[0].required, 1250.0);

    // The roll RESTED the liquidation: bar 2's own open finds it live. Its
    // level, 120, is already through the standing 100, and a request never
    // inherits a crossing its birth segment has consumed — so, exactly like a
    // slice the post-fill re-arm rests mid-path, it takes the next print:
    // bar 2's OPENING print, at the roll's own instant and the unchanged price.
    bool saw_next_open = false;
    for (const auto& point : host.offered(NativeMarginCheckKind::BarOpen)) {
        if (point.cursor.point.open_ms != T + 2 * kMinute) continue;
        saw_next_open = true;
        CHECK(point.liquidation_resting);
    }
    CHECK(saw_next_open);
    REQUIRE(host.margin_calls.size() == 1);
    const auto& call = host.margin_calls[0];
    CHECK(call.cursor.point.effective_time_ms == T + 2 * kMinute);
    CHECK(call.cursor.point.path_phase == NativePathPhase::Open);
    CHECK(call.side == no::Side::Long);
    near(call.units, 2.0);
    near(call.mark, 100.0);
    near(call.equity, 1000.0);
    near(call.required, 1000.0);                                     // 8 * 100 * 2.5 * 0.5
    near(call.position_before, 10.0);
    near(call.position_after, 8.0);
    // The receipt's level is the book's it left behind: 8 units at 2.5 solve
    // 1000 + 20 * (L - 100) == 10 * L, i.e. L == 100 — restored exactly onto
    // its maintenance line, which is what RestoreMinimum means.
    near(call.liquidation_price, 100.0);
    near(host.physical_position().signed_units, 8.0);
    REQUIRE(host.rows().size() == 1);
    near(host.rows()[0].exit_price, 100.0);
    near(host.rows()[0].qty, 2.0);
    near(host.rows()[0].pnl, 0.0);

    // Every earlier point of the kernel's own was measured under rate 1.0 and
    // found nothing: the roll is what made the call.
    for (const auto& view : host.views) {
        if (view.cursor.point.ordinal < rolls[0].cursor.point.ordinal
            && view.position.signed_units != 0.0) {
            near(view.required, 500.0);
        }
    }
}

// ── 2. The host's gate owns the point ───────────────────────────────────
// The same run with the FxRoll point suppressed. Nothing is measured at the
// roll, so the breach is only seen at the next point the kernel has: bar 2's
// open. Same slice, one script bar later — which is the gap the kind closes.
void suppressed_roll_is_inert_until_the_next_bar_open() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    auto spec = roll_spec("fx-roll-suppressed");
    spec.margin = model();

    RollHost host;
    host.open_units = 10.0;
    host.refuse_fx_roll = true;
    run_bars(host, spec, curve, bars);

    CHECK(host.offered(NativeMarginCheckKind::FxRoll).size() == 1);  // offered
    CHECK(host.measured(NativeMarginCheckKind::FxRoll).empty());     // never measured
    // Nothing rests when bar 2 opens: its BarOpen point is the first to see
    // the breach, and the slice it rests is born AT that opening print, so it
    // cannot take it — it fills on bar 2's first segment instead.
    for (const auto& point : host.offered(NativeMarginCheckKind::BarOpen)) {
        if (point.cursor.point.open_ms == T + 2 * kMinute) CHECK(!point.liquidation_resting);
    }
    REQUIRE(host.margin_calls.size() == 1);
    CHECK(host.margin_calls[0].cursor.point.open_ms == T + 2 * kMinute);   // bar 2
    CHECK(host.margin_calls[0].cursor.point.path_phase != NativePathPhase::Open);
    near(host.margin_calls[0].units, 2.0);
    near(host.physical_position().signed_units, 8.0);
}

// ── 3. A level the new rate moves INSIDE the segment that remains ────────
// Bar 1 walks 100 -> 98 -> 104 -> 99 (low first). The curve steps to 2.0 at
// bar 1's Close coordinate, where the walk stands at 104:
//   equity(104, 2) = 1080 >= required(104, 2) = 1040   no breach where it stands
//   equity( 99, 2) =  980 <  required( 99, 2) =  990   a breach at the close
//   level(2.0) = 100
// so the roll rests the liquidation at 100 in time for the Close segment to
// reach it, and the position is closed AT the level. Without the roll point
// the same breach waits for bar 2's open and is closed at its 99 instead.
void rolled_level_is_reached_inside_the_remaining_segment() {
    const std::vector<Bar> bars = {calm(0), ohlc(1, 100.0, 104.0, 98.0, 99.0),
                                   calm(2, 99.0), calm(3, 99.0)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.0}};
    auto spec = roll_spec("fx-roll-inside-segment");
    spec.path_order = NativePathOrder::LowFirst;
    spec.margin = model(NativeLiquidationSizing::Flatten);

    RollHost host;
    host.open_units = 10.0;
    run_bars(host, spec, curve, bars);

    const auto rolls = host.offered(NativeMarginCheckKind::FxRoll);
    REQUIRE(rolls.size() == 1);
    CHECK(rolls[0].cursor.point.path_phase == NativePathPhase::Close);
    CHECK(rolls[0].mark == 99.0);                 // the adverse waypoint that remains
    REQUIRE(host.margin_calls.size() == 1);
    CHECK(host.margin_calls[0].cursor.point.open_ms == T + kMinute);
    near(host.margin_calls[0].position_after, 0.0);
    REQUIRE(host.rows().size() == 1);
    near(host.rows()[0].exit_price, 100.0);       // AT the level the new rate solved
    near(host.rows()[0].pnl, 0.0);

    RollHost late;
    late.open_units = 10.0;
    late.refuse_fx_roll = true;
    auto late_spec = spec;
    late_spec.identity = {"fx-roll-inside-segment-late", 1};
    run_bars(late, late_spec, curve, bars);
    REQUIRE(late.margin_calls.size() == 1);
    CHECK(late.margin_calls[0].cursor.point.open_ms == T + 2 * kMinute);
    REQUIRE(late.rows().size() == 1);
    near(late.rows()[0].exit_price, 99.0);        // a bar later, through the level
    near(late.rows()[0].pnl, -20.0);           // (99 - 100) * 10 * 2.0
}

// ── 4. No curve, or a curve that never steps the rate: no such point ─────
void runs_without_a_rate_step_are_never_offered_the_point() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    auto spec = roll_spec("fx-roll-none");
    spec.margin = model();

    RollHost bare;
    bare.open_units = 10.0;
    run_bars(bare, spec, std::nullopt, bars);
    CHECK(bare.offered(NativeMarginCheckKind::FxRoll).empty());
    CHECK(!bare.offered(NativeMarginCheckKind::BarOpen).empty());
    CHECK(bare.margin_calls.empty());
    near(bare.physical_position().signed_units, 10.0);

    // A curve point that restates the running rate is a point, not a step.
    RollHost level;
    level.open_units = 10.0;
    auto level_spec = spec;
    level_spec.identity = {"fx-roll-level-curve", 1};
    run_bars(level, level_spec, NativeFxCurve{{T + 2 * kMinute}, {1.0}}, bars);
    CHECK(level.offered(NativeMarginCheckKind::FxRoll).empty());
    CHECK(level.margin_calls.empty());

    // The two runs took the same fills and offered the same points.
    CHECK(level.fills.size() == bare.fills.size());
    CHECK(level.points.size() == bare.points.size());

    // A curve with no margin model is not a margin run at all.
    RollHost unmodelled;
    unmodelled.open_units = 10.0;
    auto plain = roll_spec("fx-roll-no-model");
    run_bars(unmodelled, plain, NativeFxCurve{{T + 2 * kMinute}, {2.5}}, bars);
    CHECK(unmodelled.points.empty());
    CHECK(unmodelled.margin_calls.empty());
    near(unmodelled.physical_position().signed_units, 10.0);
}

// ── 5. The short side, and the mark-resting check ────────────────────────
// A SHORT 10 @ 100: equity(100, fx) = 1000, required = 500 * fx, the same
// breach at 2.5. Flatten closes the whole book at the unchanged price, under
// both resting checks, on the opening print that follows the roll.
void short_side_and_mark_check() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    for (const auto check : {NativeLiquidationCheck::PathAdverseExtreme,
                             NativeLiquidationCheck::PathAdverseExtremeMark}) {
        auto spec = roll_spec(check == NativeLiquidationCheck::PathAdverseExtreme
                                  ? "fx-roll-short-level" : "fx-roll-short-mark");
        spec.margin = model(NativeLiquidationSizing::Flatten, check);
        RollHost host;
        host.open_units = -10.0;
        run_bars(host, spec, curve, bars);
        REQUIRE(host.offered(NativeMarginCheckKind::FxRoll).size() == 1);
        REQUIRE(host.margin_calls.size() == 1);
        CHECK(host.margin_calls[0].side == no::Side::Short);
        CHECK(host.margin_calls[0].cursor.point.effective_time_ms == T + 2 * kMinute);
        CHECK(host.margin_calls[0].cursor.point.path_phase == NativePathPhase::Open);
        near(host.margin_calls[0].position_before, -10.0);
        near(host.margin_calls[0].position_after, 0.0);
        REQUIRE(host.rows().size() == 1);
        near(host.rows()[0].exit_price, 100.0);
    }
}

// ── 6. CalculationOnly measures at its calculation alone ─────────────────
// It is not offered the roll; bar 1's calculation, which already converts at
// 2.5, is where that model sees the same breach.
void calculation_only_is_not_offered_the_roll() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    auto spec = roll_spec("fx-roll-calculation-only");
    spec.margin = model(NativeLiquidationSizing::RestoreMinimum,
                        NativeLiquidationCheck::CalculationOnly);
    RollHost host;
    host.open_units = 10.0;
    run_bars(host, spec, curve, bars);
    CHECK(host.offered(NativeMarginCheckKind::FxRoll).empty());
    REQUIRE(host.margin_calls.size() == 1);
    CHECK(host.margin_calls[0].cursor.point.open_ms == T + kMinute);
    CHECK(host.margin_calls[0].cursor.point.provenance == NativePriceProvenance::CurrentExecution
          || host.margin_calls[0].cursor.point.path_phase == NativePathPhase::None);
    near(host.margin_calls[0].units, 2.0);
}

// ── 7. A roll inside a script bar's lower-timeframe walk ─────────────────
// A 3-minute script over retained 1-minute bars. The entry fills at script
// bar 0's close (T+3m). The curve steps at T+4m, the second sub-bar of script
// bar 1: a point no script-bar open can reach.
void roll_inside_a_lower_timeframe_walk() {
    std::vector<Bar> lower;
    for (int index = 0; index < 9; ++index) lower.push_back(calm(index));
    const NativeFxCurve curve{{T + 4 * kMinute}, {2.5}};
    auto spec = roll_spec("fx-roll-lower", "3");
    spec.margin = model();
    IntrabarPath::lower_tf path;
    path.bars = lower;
    path.tf = "1";
    path.samples = 4;
    spec.intrabar.value = path;

    RollHost host;
    host.open_units = 10.0;
    begin(host, spec, curve);
    host.run(lower.data(), static_cast<int>(lower.size()), "1", "3", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);

    const auto rolls = host.offered(NativeMarginCheckKind::FxRoll);
    REQUIRE(rolls.size() == 1);
    CHECK(rolls[0].cursor.point.open_ms == T + 3 * kMinute);            // script bar 1
    CHECK(rolls[0].cursor.point.effective_time_ms == T + 4 * kMinute);  // its 2nd sub-bar
    CHECK(rolls[0].mark == 100.0);
    REQUIRE(host.margin_calls.size() == 1);
    CHECK(host.margin_calls[0].cursor.point.open_ms == T + 3 * kMinute);
    CHECK(host.margin_calls[0].cursor.point.effective_time_ms == T + 4 * kMinute);
    near(host.margin_calls[0].units, 2.0);
    near(host.physical_position().signed_units, 8.0);
}

// ── 8. FP6 stream twin ───────────────────────────────────────────────────
// The same bars, the same declared curve, once as a batch and once through
// stream_begin / stream_push_bar / stream_end. Every offered check point,
// every requirement the kernel measured, every receipt, every closed row and
// the book that is left are the batch's, whether the roll lands in the warmup
// or in realtime.
void stream_bars(RollHost& host, const NativeRunSpec& spec, const NativeFxCurve& curve,
                 const std::vector<Bar>& bars, int n_warmup) {
    begin(host, spec, curve);
    REQUIRE(host.stream_begin(bars.data(), n_warmup, "1", "1"));
    CHECK(host.last_error().empty());
    for (std::size_t index = static_cast<std::size_t>(n_warmup); index < bars.size(); ++index) {
        REQUIRE(host.stream_push_bar(bars[index]));
    }
    REQUIRE(host.stream_end(false));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

void check_twin(const RollHost& stream, const RollHost& batch) {
    REQUIRE(stream.points.size() == batch.points.size());
    for (std::size_t i = 0; i < batch.points.size(); ++i) {
        const auto& got = stream.points[i];
        const auto& want = batch.points[i];
        CHECK(got.kind == want.kind);
        CHECK(got.cursor.point.ordinal == want.cursor.point.ordinal);
        CHECK(got.cursor.point.open_ms == want.cursor.point.open_ms);
        CHECK(got.cursor.point.effective_time_ms == want.cursor.point.effective_time_ms);
        CHECK(got.cursor.point.path_phase == want.cursor.point.path_phase);
        CHECK(got.cursor.t == want.cursor.t);
        CHECK(got.mark == want.mark);
        CHECK(got.position.signed_units == want.position.signed_units);
        CHECK(got.liquidation_resting == want.liquidation_resting);
    }
    REQUIRE(stream.views.size() == batch.views.size());
    for (std::size_t i = 0; i < batch.views.size(); ++i) {
        CHECK(stream.views[i].kind == batch.views[i].kind);
        CHECK(stream.views[i].mark == batch.views[i].mark);
        CHECK(stream.views[i].equity == batch.views[i].equity);
        CHECK(stream.views[i].required == batch.views[i].required);
    }
    REQUIRE(stream.margin_calls.size() == batch.margin_calls.size());
    for (std::size_t i = 0; i < batch.margin_calls.size(); ++i) {
        const auto& got = stream.margin_calls[i];
        const auto& want = batch.margin_calls[i];
        CHECK(got.cursor.point.ordinal == want.cursor.point.ordinal);
        CHECK(got.cursor.point.effective_time_ms == want.cursor.point.effective_time_ms);
        CHECK(got.cursor.point.path_phase == want.cursor.point.path_phase);
        CHECK(got.units == want.units);
        CHECK(got.mark == want.mark);
        CHECK(got.equity == want.equity);
        CHECK(got.required == want.required);
        CHECK(got.liquidation_price == want.liquidation_price);
        CHECK(got.position_before == want.position_before);
        CHECK(got.position_after == want.position_after);
    }
    REQUIRE(stream.rows().size() == batch.rows().size());
    for (std::size_t i = 0; i < batch.rows().size(); ++i) {
        CHECK(stream.rows()[i].exit_time == batch.rows()[i].exit_time);
        CHECK(stream.rows()[i].exit_price == batch.rows()[i].exit_price);
        CHECK(stream.rows()[i].qty == batch.rows()[i].qty);
        CHECK(stream.rows()[i].pnl == batch.rows()[i].pnl);
    }
    CHECK(stream.fills.size() == batch.fills.size());
    CHECK(stream.physical_position().signed_units == batch.physical_position().signed_units);
    CHECK(stream.native_marked_equity(100.0) == batch.native_marked_equity(100.0));
}

void stream_twin() {
    struct Row {
        const char* key;
        std::vector<Bar> bars;
        NativeFxCurve curve;
        NativePathOrder order;
        NativeLiquidationSizing sizing;
        std::size_t calls;
        std::size_t rolls;
    };
    const std::vector<Row> rows = {
        {"fx-roll-twin-flat", {calm(0), calm(1), calm(2), calm(3)},
         NativeFxCurve{{T + 2 * kMinute}, {2.5}}, NativePathOrder::Auto,
         NativeLiquidationSizing::RestoreMinimum, 1, 1},
        {"fx-roll-twin-segment",
         {calm(0), ohlc(1, 100.0, 104.0, 98.0, 99.0), calm(2, 99.0), calm(3, 99.0)},
         NativeFxCurve{{T + 2 * kMinute}, {2.0}}, NativePathOrder::LowFirst,
         NativeLiquidationSizing::Flatten, 1, 1},
        // Two steps: up into the call, then back down. The second roll is a
        // check point too, and withdraws nothing it should keep.
        {"fx-roll-twin-two-steps", {calm(0), calm(1), calm(2), calm(3), calm(4)},
         NativeFxCurve{{T + 2 * kMinute, T + 4 * kMinute}, {2.5, 1.0}}, NativePathOrder::Auto,
         NativeLiquidationSizing::RestoreMinimum, 1, 2},
    };
    for (const auto& row : rows) {
        auto spec = roll_spec(row.key);
        spec.path_order = row.order;
        spec.margin = model(row.sizing);
        RollHost batch;
        batch.open_units = 10.0;
        run_bars(batch, spec, row.curve, row.bars);
        REQUIRE(batch.margin_calls.size() == row.calls);
        REQUIRE(batch.offered(NativeMarginCheckKind::FxRoll).size() == row.rolls);
        // n_warmup = 1: the entry and every roll are realtime. n_warmup = 3:
        // the first roll (and its call) happen inside the warmup replay.
        for (const int n_warmup : {1, 3}) {
            RollHost stream;
            stream.open_units = 10.0;
            stream_bars(stream, spec, row.curve, row.bars, n_warmup);
            check_twin(stream, batch);
        }
    }
}

}  // namespace

int main() {
    test("rate step breaches with prices unchanged", rate_step_breaches_with_prices_unchanged);
    test("suppressed roll is inert", suppressed_roll_is_inert_until_the_next_bar_open);
    test("rolled level inside the remaining segment",
         rolled_level_is_reached_inside_the_remaining_segment);
    test("no rate step, no point", runs_without_a_rate_step_are_never_offered_the_point);
    test("short side and mark check", short_side_and_mark_check);
    test("CalculationOnly is not offered the roll", calculation_only_is_not_offered_the_roll);
    test("roll inside a lower-timeframe walk", roll_inside_a_lower_timeframe_walk);
    test("FP6 stream twin", stream_twin);
    std::printf("N6 FX roll: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
