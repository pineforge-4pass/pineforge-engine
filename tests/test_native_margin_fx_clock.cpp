/*
 * test_native_margin_fx_clock.cpp — R5 follow-up lane E3: a margin check point
 * converts at ITS OWN cursor's instant, on the run's declared FX curve.
 *
 * Every term of the maintenance test is FX-bearing — the requirement
 * Q * P * pv * fx * m, the marked equity it is compared with, and the level
 * solved from the two — so the rate has to be the one the curve has in force
 * at the point being checked. BacktestEngine's own accessor converts at the
 * PRESENTED bar clock, which is a different instant whenever the walk has
 * moved on: an applied fill drained at its script bar's calculation is
 * presented that bar's close coordinate while its cursor still stands at the
 * opening print it filled on. Lane P4 hit exactly that writing the C fx-roll
 * scenario ("required 1250 at cursor time 300000, where the curve's own step
 * was at 600000") and moved its step one bar later to dodge it; lane N6
 * disclosed the same asymmetry on the tick route.
 *
 * The rest of the kernel already converts at the cursor — a Sized request
 * freezes at `account_currency_fx_at(point->decision.coordinate
 * .effective_time_ms)`, an execution term records
 * `active_fx = account_currency_fx_at(cursor.point.effective_time_ms)` — so
 * this TU pins the margin model onto the same rule, for every check kind and
 * in batch and stream alike.
 *
 * R5 lane E21 holds the opening gate to the same rule (section 8): its
 * placement site, run when a Sized{SizeTime::AtAcceptance} is accepted, read
 * the presented clock while its freeze converted at the acceptance point.
 *
 * No Pine twin lives here on purpose: the TU reaches no source header, so the
 * kernel-only build (PINEFORGE_BUILD_SOURCE_LAYER=OFF) runs every row.
 *
 * Hand arithmetic (point_value = 1, no fee, capital 1000, maintenance 0.5,
 * no initial requirement — the maintenance-only model):
 *   equity(P, fx)   = 1000 + dir * (P - entry) * Q * fx
 *   required(P, fx) = Q * P * fx * 0.5
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

// The entry fills on the OPENING PRINT of the bar after the one it was
// submitted on (no AfterCalculation close execution), which is the geometry
// whose applied check is drained at that bar's calculation — the one place
// the presented clock stands ahead of the cursor.
NativeRunSpec clock_spec(const char* key, const char* script = "1") {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = script;
    s.tickerid = "TEST:FXCLOCK";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

// Maintenance-only: zero per-side initial keeps the opening admission out of
// the way, so every number this TU reads is a check point's own.
NativeMarginModel model(NativeLiquidationCheck check = NativeLiquidationCheck::PathAdverseExtreme) {
    NativeMarginModel m;
    m.initial_long = 0.0;
    m.initial_short = 0.0;
    m.maintenance_long = 0.5;
    m.maintenance_short = 0.5;
    m.sizing = NativeLiquidationSizing::RestoreMinimum;
    m.check = check;
    return m;
}

// One offered check point, with the engine clock that was standing while it
// was offered. The clock is the whole point of this TU, so it is recorded
// beside every point rather than inferred.
struct Offered {
    NativeMarginCheckPoint point;
    std::int64_t presented_ms = 0;
};

struct ClockHost final : Host {
    double open_units = 0.0;
    int bars = 0;
    std::vector<Offered> points;
    std::vector<NativeMarginRequirementView> views;
    // Beside each measured view: BacktestEngine::marked_equity() at the same
    // mark, read at the same moment — the engine's own accessor, converting at
    // its presented clock — and that clock itself.
    std::vector<double> accessor_equity;
    std::vector<std::int64_t> view_presented_ms;
    std::vector<no::MarginCallEvent> margin_calls;

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (bars++ == 0 && open_units != 0.0) (void)put(*this, tx(open_units, "entry"));
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        margin_calls.push_back(event);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        const_cast<ClockHost&>(*this).points.push_back(Offered{point, current_bar_.timestamp});
        return true;
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        const_cast<ClockHost&>(*this).views.push_back(view);
        const_cast<ClockHost&>(*this).accessor_equity.push_back(marked_equity(view.mark));
        const_cast<ClockHost&>(*this).view_presented_ms.push_back(current_bar_.timestamp);
        return std::nullopt;
    }

    // The curve's own lookup, so a row states the rate it expects instead of
    // restating the curve by hand.
    double rate_at(std::int64_t at_ms) const { return account_currency_fx_at(at_ms); }

    std::vector<Offered> offered(NativeMarginCheckKind kind) const {
        std::vector<Offered> out;
        for (const auto& row : points) if (row.point.kind == kind) out.push_back(row);
        return out;
    }
    std::vector<NativeMarginRequirementView> measured(NativeMarginCheckKind kind) const {
        std::vector<NativeMarginRequirementView> out;
        for (const auto& view : views) if (view.kind == kind) out.push_back(view);
        return out;
    }
};

void begin(Host& host, const NativeRunSpec& spec, const std::optional<NativeFxCurve>& curve) {
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    if (curve) {
        REQUIRE(host.configure_native_fx_curve(*curve).status == NativeSetupStatus::Applied);
    }
}

void run_bars(Host& host, const NativeRunSpec& spec,
              const std::optional<NativeFxCurve>& curve, const std::vector<Bar>& bars) {
    begin(host, spec, curve);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

void stream_bars(Host& host, const NativeRunSpec& spec, const NativeFxCurve& curve,
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

// ── 1. P4's geometry: the step AT the entry bar's presented clock ────────
// Four flat bars at 100, LONG 15 filling on bar 1's OPENING print (T+1m).
// The curve steps to 2.5 at T+2m — bar 1's close coordinate, which is the
// clock the engine is presenting when that fill's applied check is drained.
//   at the cursor T+1m : required = 15 * 100 * 1.0 * 0.5 =  750 <= 1000, quiet
//   at the clock T+2m  : required = 15 * 100 * 2.5 * 0.5 = 1875 >  1000, a call
// The rate in force at T+1m is 1.0, so the fill is quiet and the FIRST point
// that may see the step is the roll at T+2m, which is where the breach is
// measured and the liquidation rested. (15 rather than 10 units on purpose: a
// book exactly at the money solves level 0, which withdraws before the model
// measures anything at all — see tests/test_native_margin_fx_roll.cpp §1.)
void applied_check_converts_at_its_own_fill() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    auto spec = clock_spec("fx-clock-applied");
    spec.margin = model();

    ClockHost host;
    host.open_units = 15.0;
    run_bars(host, spec, curve, bars);

    // The asymmetry itself: the entry's applied check stands one script bar
    // behind the clock the engine is presenting.
    const auto applied = host.offered(NativeMarginCheckKind::AfterApplied);
    REQUIRE(!applied.empty());
    CHECK(applied[0].point.cursor.point.effective_time_ms == T + kMinute);
    CHECK(applied[0].presented_ms == T + 2 * kMinute);
    CHECK(applied[0].point.position.signed_units == 15.0);
    CHECK(applied[0].point.mark == 100.0);
    // ... and the rate it converts at is the cursor's, not the clock's.
    near(host.rate_at(applied[0].point.cursor.point.effective_time_ms), 1.0);
    near(host.rate_at(applied[0].presented_ms), 2.5);

    const auto applied_views = host.measured(NativeMarginCheckKind::AfterApplied);
    REQUIRE(!applied_views.empty());
    near(applied_views[0].equity, 1000.0);
    near(applied_views[0].required, 750.0);          // 15 * 100 * 1.0 * 0.5

    // Nothing was called at the entry: the step is still ahead of it.
    for (const auto& call : host.margin_calls) CHECK(call.cursor.point.effective_time_ms >= T + 2 * kMinute);

    // The roll is where the step is measured, with the new rate.
    const auto rolls = host.offered(NativeMarginCheckKind::FxRoll);
    REQUIRE(rolls.size() == 1);
    CHECK(rolls[0].point.cursor.point.effective_time_ms == T + 2 * kMinute);
    CHECK(rolls[0].point.cursor.point.path_phase == NativePathPhase::Close);
    const auto roll_views = host.measured(NativeMarginCheckKind::FxRoll);
    REQUIRE(roll_views.size() == 1);
    near(roll_views[0].equity, 1000.0);
    near(roll_views[0].required, 1875.0);            // 15 * 100 * 2.5 * 0.5

    // One call, at the roll's own instant, restoring the minimum:
    // (1875 - 1000) / (100 * 2.5 * 0.5) = 7 units.
    REQUIRE(host.margin_calls.size() == 1);
    const auto& call = host.margin_calls[0];
    CHECK(call.cursor.point.effective_time_ms == T + 2 * kMinute);
    CHECK(call.side == no::Side::Long);
    near(call.units, 7.0);
    near(call.mark, 100.0);
    near(call.equity, 1000.0);
    near(call.required, 1000.0);                     // 8 * 100 * 2.5 * 0.5
    near(call.position_before, 15.0);
    near(call.position_after, 8.0);
    near(host.physical_position().signed_units, 8.0);
    REQUIRE(host.rows().size() == 1);
    near(host.rows()[0].qty, 7.0);
    near(host.rows()[0].exit_price, 100.0);
    near(host.rows()[0].pnl, 0.0);
}

// ── 2. Both terms, not just the requirement ─────────────────────────────
// The same geometry with a price that moves, so the marked equity is
// FX-bearing too. LONG 15 fills at bar 1's open (100); bar 1 walks down to 90,
// so the applied check's adverse mark is 90:
//   at the cursor T+1m (1.0): equity = 1000 - 150 =  850, required =  675, quiet
//   at the clock  T+2m (2.5): equity = 1000 - 375 =  625, required = 1687.5, call
// Reading the clock would call a margin the account never owed.
void both_terms_convert_at_the_cursor() {
    const std::vector<Bar> bars = {calm(0), ohlc(1, 100.0, 100.0, 90.0, 90.0),
                                   calm(2, 90.0), calm(3, 90.0)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    auto spec = clock_spec("fx-clock-both-terms");
    spec.path_order = NativePathOrder::LowFirst;
    spec.margin = model();

    ClockHost host;
    host.open_units = 15.0;
    run_bars(host, spec, curve, bars);

    const auto applied = host.offered(NativeMarginCheckKind::AfterApplied);
    REQUIRE(!applied.empty());
    CHECK(applied[0].point.cursor.point.effective_time_ms == T + kMinute);
    CHECK(applied[0].presented_ms == T + 2 * kMinute);
    CHECK(applied[0].point.mark == 90.0);            // the adverse price still ahead

    const auto applied_views = host.measured(NativeMarginCheckKind::AfterApplied);
    REQUIRE(!applied_views.empty());
    near(applied_views[0].mark, 90.0);
    near(applied_views[0].equity, 850.0);            // 1000 + (90 - 100) * 15 * 1.0
    near(applied_views[0].required, 675.0);          // 15 * 90 * 1.0 * 0.5
    // No call was made at the entry's own bar.
    for (const auto& call : host.margin_calls) CHECK(call.cursor.point.effective_time_ms >= T + 2 * kMinute);
}

// ── 3. Every kind, on its own cursor ────────────────────────────────────
// Whatever the kind, the requirement a view carries is the held book at the
// view's own mark and the rate the curve has in force at the view's cursor.
// A CalculationOnly model contributes the Calculation kind; the path model
// contributes BarOpen, AfterApplied and FxRoll.
void every_kind_converts_at_its_own_cursor() {
    struct Row {
        const char* key;
        double units;
        NativeLiquidationCheck check;
        std::vector<Bar> bars;
        NativeFxCurve curve;
    };
    const std::vector<Row> rows = {
        {"fx-clock-kinds-long", 10.0, NativeLiquidationCheck::PathAdverseExtreme,
         {calm(0), calm(1), calm(2), calm(3), calm(4)},
         NativeFxCurve{{T + 2 * kMinute, T + 4 * kMinute}, {1.4, 1.0}}},
        {"fx-clock-kinds-short", -10.0, NativeLiquidationCheck::PathAdverseExtreme,
         {calm(0), ohlc(1, 100.0, 104.0, 100.0, 103.0), calm(2, 103.0), calm(3, 103.0)},
         NativeFxCurve{{T + kMinute, T + 3 * kMinute}, {1.2, 1.5}}},
        {"fx-clock-kinds-calc", 10.0, NativeLiquidationCheck::CalculationOnly,
         {calm(0), calm(1), calm(2), calm(3)},
         NativeFxCurve{{T + 2 * kMinute}, {1.6}}},
    };
    for (const auto& row : rows) {
        auto spec = clock_spec(row.key);
        spec.margin = model(row.check);
        ClockHost host;
        host.open_units = row.units;
        run_bars(host, spec, row.curve, row.bars);
        REQUIRE(!host.views.empty());
        REQUIRE(host.accessor_equity.size() == host.views.size());
        for (std::size_t i = 0; i < host.views.size(); ++i) {
            const auto& view = host.views[i];
            const double held = std::abs(view.position.signed_units);
            const double fx = host.rate_at(view.cursor.point.effective_time_ms);
            near(view.required, held * view.mark * fx * 0.5);
            // Wherever the presented clock's rate IS the cursor's, the
            // rate-explicit equity is BacktestEngine::marked_equity()'s own
            // number, bit for bit: the rule only ever moves a point whose two
            // clocks disagree.
            if (host.rate_at(host.view_presented_ms[i]) == fx) {
                CHECK(view.equity == host.accessor_equity[i]);
            }
        }
    }
}

// ── 4. The receipt is the check's own money ─────────────────────────────
// A margin call's receipt states the requirement of the book it left behind;
// that number is measured at the fill's cursor too, not at whatever bar the
// engine went on to present.
void the_receipt_converts_at_the_fill_it_booked() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    auto spec = clock_spec("fx-clock-receipt");
    spec.margin = model();

    ClockHost host;
    host.open_units = 10.0;
    run_bars(host, spec, curve, bars);
    REQUIRE(host.margin_calls.size() == 1);
    const auto& call = host.margin_calls[0];
    const double fx = host.rate_at(call.cursor.point.effective_time_ms);
    near(fx, 2.5);
    near(call.required, std::abs(call.position_after) * call.mark * fx * 0.5);
    near(call.equity, 1000.0);
}

// ── 5. A run with no curve is untouched ─────────────────────────────────
// account_currency_fx_at answers the run's scalar rate at every instant when
// no curve is declared, so cursor and clock are the same number and every
// check point measures exactly what it always did.
void a_run_without_a_curve_is_untouched() {
    // LONG 15: a book whose maintenance line is a price the run can reach, so
    // every check point solves a level and measures a requirement.
    const std::vector<Bar> bars = {calm(0), ohlc(1, 100.0, 100.0, 90.0, 90.0),
                                   calm(2, 90.0), calm(3, 90.0)};
    for (const double account_fx : {1.0, 3.0}) {
        auto spec = clock_spec(account_fx == 1.0 ? "fx-clock-bare-1" : "fx-clock-bare-3");
        spec.account_fx = account_fx;
        spec.path_order = NativePathOrder::LowFirst;
        spec.margin = model();
        ClockHost host;
        host.open_units = 15.0;
        run_bars(host, spec, std::nullopt, bars);
        REQUIRE(!host.views.empty());
        for (const auto& row : host.points) {
            near(host.rate_at(row.point.cursor.point.effective_time_ms), account_fx);
            near(host.rate_at(row.presented_ms), account_fx);
        }
        for (const auto& view : host.views) {
            const double held = std::abs(view.position.signed_units);
            near(view.required, held * view.mark * account_fx * 0.5);
        }
        // The rate-explicit equity the kernel now computes IS
        // BacktestEngine::marked_equity(), bit for bit, wherever the cursor's
        // rate and the presented clock's agree — which is every instant of a
        // run that declares no curve. (The model's basis is the default
        // MarkedEquity, so margin_equity() adds nothing on top.)
        REQUIRE(host.accessor_equity.size() == host.views.size());
        for (std::size_t i = 0; i < host.views.size(); ++i) {
            CHECK(host.views[i].equity == host.accessor_equity[i]);
        }
        CHECK(host.offered(NativeMarginCheckKind::FxRoll).empty());
    }
}

// ── 6. Batch and stream take the same instants ──────────────────────────
// A confirmed-bar stream walks the same points as the batch of the same bars,
// so the clock rule cannot be a batch-only rule. Every offered point, the
// clock standing at it, every requirement, every receipt and every closed row
// match, whether the step lands in the warmup replay or in realtime.
void stream_twin() {
    struct Row {
        const char* key;
        double units;
        std::vector<Bar> bars;
        NativeFxCurve curve;
        NativePathOrder order;
    };
    const std::vector<Row> rows = {
        {"fx-clock-twin-applied", 10.0, {calm(0), calm(1), calm(2), calm(3)},
         NativeFxCurve{{T + 2 * kMinute}, {2.5}}, NativePathOrder::Auto},
        {"fx-clock-twin-both-terms", 10.0,
         {calm(0), ohlc(1, 100.0, 100.0, 90.0, 90.0), calm(2, 90.0), calm(3, 90.0)},
         NativeFxCurve{{T + 2 * kMinute}, {2.5}}, NativePathOrder::LowFirst},
        {"fx-clock-twin-two-steps", -10.0,
         {calm(0), calm(1), calm(2), calm(3), calm(4)},
         NativeFxCurve{{T + 2 * kMinute, T + 4 * kMinute}, {2.5, 1.0}}, NativePathOrder::Auto},
    };
    for (const auto& row : rows) {
        auto spec = clock_spec(row.key);
        spec.path_order = row.order;
        spec.margin = model();
        ClockHost batch;
        batch.open_units = row.units;
        run_bars(batch, spec, row.curve, row.bars);
        REQUIRE(!batch.points.empty());
        for (const int n_warmup : {1, 3}) {
            ClockHost stream;
            stream.open_units = row.units;
            stream_bars(stream, spec, row.curve, row.bars, n_warmup);
            REQUIRE(stream.points.size() == batch.points.size());
            for (std::size_t i = 0; i < batch.points.size(); ++i) {
                CHECK(stream.points[i].point.kind == batch.points[i].point.kind);
                CHECK(stream.points[i].point.cursor.point.effective_time_ms
                      == batch.points[i].point.cursor.point.effective_time_ms);
                CHECK(stream.points[i].presented_ms == batch.points[i].presented_ms);
                CHECK(stream.points[i].point.mark == batch.points[i].point.mark);
                CHECK(stream.points[i].point.position.signed_units
                      == batch.points[i].point.position.signed_units);
            }
            REQUIRE(stream.views.size() == batch.views.size());
            for (std::size_t i = 0; i < batch.views.size(); ++i) {
                CHECK(stream.views[i].kind == batch.views[i].kind);
                CHECK(stream.views[i].equity == batch.views[i].equity);
                CHECK(stream.views[i].required == batch.views[i].required);
            }
            REQUIRE(stream.margin_calls.size() == batch.margin_calls.size());
            for (std::size_t i = 0; i < batch.margin_calls.size(); ++i) {
                CHECK(stream.margin_calls[i].cursor.point.effective_time_ms
                      == batch.margin_calls[i].cursor.point.effective_time_ms);
                CHECK(stream.margin_calls[i].units == batch.margin_calls[i].units);
                CHECK(stream.margin_calls[i].required == batch.margin_calls[i].required);
                CHECK(stream.margin_calls[i].equity == batch.margin_calls[i].equity);
            }
            REQUIRE(stream.rows().size() == batch.rows().size());
            for (std::size_t i = 0; i < batch.rows().size(); ++i) {
                CHECK(stream.rows()[i].exit_price == batch.rows()[i].exit_price);
                CHECK(stream.rows()[i].qty == batch.rows()[i].qty);
                CHECK(stream.rows()[i].pnl == batch.rows()[i].pnl);
            }
            CHECK(stream.physical_position().signed_units
                  == batch.physical_position().signed_units);
        }
    }
}

// ── 7. The tick route keeps its refusal ─────────────────────────────────
// E3 moves the margin model onto the cursor's rate; it does not move
// active_account_currency_fx(), which is what every SCRIPT-VISIBLE conversion
// reads — open_trade_profit(), marked_equity(), the equity curve. On the tick
// route the observation hook runs before its print has moved that clock, so
// those readers would answer on a rate the print has already left behind.
// That is what N6's refusal guards, and it is unrelated to the check point's
// own conversion, so the refusal stays.
void the_tick_route_keeps_its_refusal() {
    auto spec = clock_spec("fx-clock-tick-refusal");
    spec.margin = model();
    const NativeFxCurve curve{{T + kMinute + 30000}, {2.0}};
    const std::vector<Bar> warmup = {calm(0)};

    ClockHost host;
    begin(host, spec, curve);
    REQUIRE(host.stream_begin(warmup.data(), 1, "1", "1"));
    TradeTick tick{};
    tick.timestamp = T + kMinute + 40000;
    tick.price = 110.0;
    tick.quantity = 1.0;
    CHECK(!host.stream_push_tick(tick));
    CHECK(host.last_error() == "a declared native FX curve requires confirmed-bar stream input");
    CHECK(!host.stream_advance_time(T + 2 * kMinute));
    CHECK(host.last_error() == "a declared native FX curve requires confirmed-bar stream input");
}

// ── 8. The opening gate converts at its own point too (R5 lane E21) ─────
// The opening gate is the margin model's other half, and it has two sites.
// The CANDIDATE gate converts at its cursor: consume_matched_request presents
// the cursor's instant before it inspects and admits. The PLACEMENT gate runs
// when a Sized{SizeTime::AtAcceptance} is accepted, against the quantity the
// freeze computed at the acceptance point's rate — and it read the PRESENTED
// clock. In an applied callback drained at its script bar's calculation that
// is the bar's close coordinate, while the acceptance point is the opening
// print the fill landed on. These rows hold both sites to one rule: a gate
// converts at the instant its own point names, for the requirement, the
// marked equity it is compared with and the ticket netted from that equity,
// and the freeze takes its equity basis at the same instant as its rate.
//
// Hand arithmetic (point_value = 1, capital 1000, fraction f):
//   required(fx) = resulting_units * price * fx * f
//   equity(fx)   = 1000 + sum (price - entry) * q * fx - open entry fees - ticket(fx)

// Transact openings at chosen script bars' calculations, and one kernel-sized
// opening submitted from the applied callback of a chosen fill: the frame whose
// coordinate stands behind the clock the engine presents.
struct AdmitHost final : Host {
    std::vector<std::pair<int, double>> entries;  // (script bar, units)
    std::optional<no::Sized> sized;
    std::size_t sized_after = 0;                  // which applied fill, 0-based
    int bars = 0;
    std::size_t applied = 0;
    std::optional<no::SubmitResult> placed;
    std::int64_t acceptance_ms = 0;
    std::int64_t presented_ms = 0;
    std::int64_t presented_after_ms = 0;          // the clock once submit returned

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        for (const auto& entry : entries) {
            if (entry.first == bars) (void)put(*this, tx(entry.second, "entry"));
        }
        ++bars;
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        Host::on_native_applied(event, context);
        if (!sized || placed || applied++ != sized_after) return;
        acceptance_ms = context.coordinate.effective_time_ms;
        presented_ms = current_bar_.timestamp;
        no::Request request;
        request.intent = *sized;
        request.label = "sized";
        placed = submit(request);
        presented_after_ms = current_bar_.timestamp;
    }

    double rate_at(std::int64_t at_ms) const { return account_currency_fx_at(at_ms); }
    std::vector<no::MatchRejectedEvent> sized_rejections() const {
        std::vector<no::MatchRejectedEvent> out;
        for (const auto& event : events<no::MatchRejectedEvent>(*this)) {
            if (event.request().label == "sized") out.push_back(event);
        }
        return out;
    }
    std::optional<no::ExecutionAppliedEvent> sized_fill() const {
        for (const auto& event : events<no::ExecutionAppliedEvent>(*this)) {
            if (event.request().label == "sized") return event;
        }
        return std::nullopt;
    }
};

// Priced at the acceptance point's own print (SizePrice::Resolved).
no::Sized at_acceptance(no::SizeBasis basis) {
    no::Sized sized;
    sized.side = no::Side::Long;
    sized.basis = basis;
    sized.time = no::SizeTime::AtAcceptance;
    return sized;
}

// A model whose per-side initial is positive, so it gates openings itself;
// its maintenance line stays far from every book below.
NativeMarginModel gated_model(double initial) {
    auto m = model();
    m.initial_long = initial;
    m.initial_short = initial;
    m.maintenance_long = 0.1;
    m.maintenance_short = 0.1;
    return m;
}

// P4's geometry again: LONG 15 fills on bar 1's opening print (T+1m), and the
// curve steps to 2.5 at T+2m — the clock presented while that fill's applied
// callback runs. The callback places Sized{CashValue 500}: 500 / (100 * 1.0)
// = 5 frozen units on 15 held.
//   at the point T+1m : 20 * 100 * 1.0 * 0.4 =  800 <= 1000, placed
//   at the clock T+2m : 20 * 100 * 2.5 * 0.4 = 2000 >  1000, PlacementAdmission
// The candidate then matches at T+2m, after the step, and ITS gate refuses the
// same book there with its own reason: each site on its own point's rate.
void placement_gate_step_up(const char* key, bool model_spelling) {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {2.5}};
    auto spec = clock_spec(key);
    if (model_spelling) {
        spec.margin = gated_model(0.4);
    } else {
        spec.initial_margin_fraction = 0.4;
    }

    AdmitHost host;
    host.entries = {{0, 15.0}};
    host.sized = at_acceptance(no::CashValue{500.0});
    run_bars(host, spec, curve, bars);

    REQUIRE(host.placed.has_value());
    CHECK(host.acceptance_ms == T + kMinute);
    CHECK(host.presented_ms == T + 2 * kMinute);
    near(host.rate_at(host.acceptance_ms), 1.0);
    near(host.rate_at(host.presented_ms), 2.5);
    CHECK(host.placed->status == no::SubmitStatus::Accepted);
    CHECK(!host.placed->reason.has_value());
    // The point's instant is the gate's alone: the callback still stands on
    // the clock it was presented once submit has returned.
    CHECK(host.presented_after_ms == host.presented_ms);

    const auto rejected = host.sized_rejections();
    REQUIRE(rejected.size() == 1);
    CHECK(rejected[0].reason == no::MatchRejectReason::InitialMargin);
    CHECK(rejected[0].cursor.point.effective_time_ms == T + 2 * kMinute);
    CHECK(!host.sized_fill().has_value());
    near(host.physical_position().signed_units, 15.0);
    CHECK(events<no::MarginCallEvent>(host).empty());
}

void placement_gate_converts_at_its_acceptance_point() {
    placement_gate_step_up("fx-admit-step-up", false);
}

void the_model_spelling_takes_the_same_gate() {
    placement_gate_step_up("fx-admit-step-up-model", true);
}

// The other direction: the clock's rate is LOWER than the point's, so reading
// it admits a book the account cannot fund where the command was accepted.
// LONG 5 fills at T+1m, where the curve already stands at 2.5
// (5 * 100 * 2.5 * 0.4 = 500 <= 1000); it steps back to 1.0 at T+2m.
// Sized{CashValue 1500} freezes 1500 / (100 * 2.5) = 6 units:
//   at the point T+1m : 11 * 100 * 2.5 * 0.4 = 1100 > 1000, PlacementAdmission
//   at the clock T+2m : 11 * 100 * 1.0 * 0.4 =  440, placed and filled
void placement_gate_refuses_what_its_point_cannot_fund() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + kMinute, T + 2 * kMinute}, {2.5, 1.0}};
    auto spec = clock_spec("fx-admit-step-down");
    spec.initial_margin_fraction = 0.4;

    AdmitHost host;
    host.entries = {{0, 5.0}};
    host.sized = at_acceptance(no::CashValue{1500.0});
    run_bars(host, spec, curve, bars);

    REQUIRE(host.placed.has_value());
    near(host.rate_at(host.acceptance_ms), 2.5);
    near(host.rate_at(host.presented_ms), 1.0);
    CHECK(host.placed->status == no::SubmitStatus::Rejected);
    REQUIRE(host.placed->reason.has_value());
    CHECK(*host.placed->reason == no::RequestRejectReason::PlacementAdmission);
    CHECK(!host.placed->handle.has_value());
    CHECK(!host.sized_fill().has_value());
    near(host.physical_position().signed_units, 5.0);
}

// Both terms, not just the requirement. LONG 10 at 100 (T+1m), bar 1 closes
// at 120, LONG 1 at 120 on bar 2's opening print (T+2m) — the acceptance
// point, while the clock presents T+3m. The curve is 2.0 at T+2m and 1.0 from
// T+3m. Sized{CashValue 120} freezes 120 / (120 * 2.0) = 0.5 units:
//   required 11.5 * 120 * 2.0 * 0.5 = 1380
//   equity at the point  1000 + (120 - 100) * 10 * 2.0 = 1400: placed
//   equity at the clock  1000 + (120 - 100) * 10 * 1.0 = 1200 would refuse it
// (This row alone does not move with the defect: the clock read BOTH terms at
// 1.0, 690 against 1200. It fails a fix that moves the requirement only.)
void placement_gate_takes_its_equity_at_its_point() {
    const std::vector<Bar> bars = {calm(0), ohlc(1, 100.0, 120.0, 100.0, 120.0),
                                   calm(2, 120.0), calm(3, 120.0), calm(4, 120.0)};
    const NativeFxCurve curve{{T + 2 * kMinute, T + 3 * kMinute}, {2.0, 1.0}};
    auto spec = clock_spec("fx-admit-equity");
    spec.initial_margin_fraction = 0.5;

    AdmitHost host;
    host.entries = {{0, 10.0}, {1, 1.0}};
    host.sized = at_acceptance(no::CashValue{120.0});
    host.sized_after = 1;
    run_bars(host, spec, curve, bars);

    REQUIRE(host.placed.has_value());
    CHECK(host.acceptance_ms == T + 2 * kMinute);
    CHECK(host.presented_ms == T + 3 * kMinute);
    near(host.rate_at(host.acceptance_ms), 2.0);
    near(host.rate_at(host.presented_ms), 1.0);
    CHECK(host.placed->status == no::SubmitStatus::Accepted);
    const auto fill = host.sized_fill();
    REQUIRE(fill.has_value());
    near(fill->opened_units, 0.5);
    near(host.physical_position().signed_units, 11.5);
}

// The ticket netted from the equity is FX-bearing too under a percent fee.
// LONG 5 at 100 (T+1m, rate 1.0) paid 100 * 5 * 1.0 * 1 % = 5, so the equity is
// 995; the curve steps to 3.0 at T+2m. Sized{CashValue 1450} freezes 14.5 units:
//   required 19.5 * 100 * 1.0 * 0.5 = 975
//   ticket at the point  100 * 14.5 * 1.0 * 1 % = 14.5 -> 995 - 14.5 = 980.5: placed
//   ticket at the clock  100 * 14.5 * 3.0 * 1 % = 43.5 -> 951.5 would refuse it
// The candidate at T+2m converts at 3.0 and refuses: 2925 > 951.5.
void placement_gate_nets_its_ticket_at_its_point() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    const NativeFxCurve curve{{T + 2 * kMinute}, {3.0}};
    auto spec = clock_spec("fx-admit-ticket");
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 1.0;
    spec.initial_margin_fraction = 0.5;

    AdmitHost host;
    host.entries = {{0, 5.0}};
    host.sized = at_acceptance(no::CashValue{1450.0});
    run_bars(host, spec, curve, bars);

    REQUIRE(host.placed.has_value());
    near(host.rate_at(host.acceptance_ms), 1.0);
    near(host.rate_at(host.presented_ms), 3.0);
    CHECK(host.placed->status == no::SubmitStatus::Accepted);
    const auto rejected = host.sized_rejections();
    REQUIRE(rejected.size() == 1);
    CHECK(rejected[0].reason == no::MatchRejectReason::InitialMargin);
    near(host.physical_position().signed_units, 5.0);
}

// The freeze's own equity basis. The same two lots with no margin at all, and
// the curve stepping to 3.0 at T+3m, the clock of the second fill's callback.
// Sized{EquityFraction 0.5}:
//   at the point  (1000 + 20 * 10 * 1.0) * 0.5 = 600 -> 600 / (120 * 1.0) = 5
//   at the clock  (1000 + 20 * 10 * 3.0) * 0.5 = 800 -> 6.67 units, frozen
//                 against the point's rate: an equity of one instant divided
//                 by the rate of another
void the_freeze_takes_its_equity_at_the_same_point() {
    const std::vector<Bar> bars = {calm(0), ohlc(1, 100.0, 120.0, 100.0, 120.0),
                                   calm(2, 120.0), calm(3, 120.0), calm(4, 120.0)};
    const NativeFxCurve curve{{T + 3 * kMinute}, {3.0}};
    auto spec = clock_spec("fx-freeze-equity");

    AdmitHost host;
    host.entries = {{0, 10.0}, {1, 1.0}};
    host.sized = at_acceptance(no::EquityFraction{0.5});
    host.sized_after = 1;
    run_bars(host, spec, curve, bars);

    REQUIRE(host.placed.has_value());
    near(host.rate_at(host.acceptance_ms), 1.0);
    near(host.rate_at(host.presented_ms), 3.0);
    CHECK(host.placed->status == no::SubmitStatus::Accepted);
    const auto fill = host.sized_fill();
    REQUIRE(fill.has_value());
    near(fill->opened_units, 5.0);
    near(host.physical_position().signed_units, 16.0);
}

// With no curve the point and the clock are one rate, so the gate and the
// freeze decide exactly what they always did: 500 / (100 * fx) units on 5
// held owe (5 * 100 * fx + 500) * 0.4 = 200 * fx + 200 <= 1000, placed and filled.
void a_gate_without_a_curve_is_untouched() {
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    for (const double account_fx : {1.0, 2.5}) {
        auto spec = clock_spec(account_fx == 1.0 ? "fx-admit-bare-1" : "fx-admit-bare-25");
        spec.account_fx = account_fx;
        spec.initial_margin_fraction = 0.4;
        AdmitHost host;
        host.entries = {{0, 5.0}};
        host.sized = at_acceptance(no::CashValue{500.0});
        run_bars(host, spec, std::nullopt, bars);
        REQUIRE(host.placed.has_value());
        CHECK(host.acceptance_ms < host.presented_ms);
        near(host.rate_at(host.acceptance_ms), account_fx);
        near(host.rate_at(host.presented_ms), account_fx);
        CHECK(host.placed->status == no::SubmitStatus::Accepted);
        const auto fill = host.sized_fill();
        REQUIRE(fill.has_value());
        near(fill->opened_units, 500.0 / (100.0 * account_fx));
    }
}

// A confirmed-bar stream accepts the command at the same point with the same
// clock standing, so it decides exactly what the batch decides.
void the_stream_gate_decides_what_the_batch_decides() {
    struct Row {
        const char* key;
        double held;
        double cash;
        NativeFxCurve curve;
    };
    const std::vector<Row> rows = {
        {"fx-admit-twin-up", 15.0, 500.0, NativeFxCurve{{T + 2 * kMinute}, {2.5}}},
        {"fx-admit-twin-down", 5.0, 1500.0,
         NativeFxCurve{{T + kMinute, T + 2 * kMinute}, {2.5, 1.0}}},
    };
    const std::vector<Bar> bars = {calm(0), calm(1), calm(2), calm(3)};
    for (const auto& row : rows) {
        auto spec = clock_spec(row.key);
        spec.initial_margin_fraction = 0.4;
        AdmitHost batch;
        batch.entries = {{0, row.held}};
        batch.sized = at_acceptance(no::CashValue{row.cash});
        run_bars(batch, spec, row.curve, bars);
        REQUIRE(batch.placed.has_value());
        for (const int n_warmup : {1, 3}) {
            AdmitHost stream;
            stream.entries = {{0, row.held}};
            stream.sized = at_acceptance(no::CashValue{row.cash});
            stream_bars(stream, spec, row.curve, bars, n_warmup);
            REQUIRE(stream.placed.has_value());
            CHECK(stream.acceptance_ms == batch.acceptance_ms);
            CHECK(stream.presented_ms == batch.presented_ms);
            CHECK(stream.placed->status == batch.placed->status);
            CHECK(stream.placed->reason == batch.placed->reason);
            CHECK(stream.sized_rejections().size() == batch.sized_rejections().size());
            CHECK(stream.sized_fill().has_value() == batch.sized_fill().has_value());
            CHECK(stream.physical_position().signed_units
                  == batch.physical_position().signed_units);
        }
    }
}

}  // namespace

int main() {
    test("applied check converts at its own fill", applied_check_converts_at_its_own_fill);
    test("both terms convert at the cursor", both_terms_convert_at_the_cursor);
    test("every kind converts at its own cursor", every_kind_converts_at_its_own_cursor);
    test("the receipt converts at the fill it booked", the_receipt_converts_at_the_fill_it_booked);
    test("a run without a curve is untouched", a_run_without_a_curve_is_untouched);
    test("batch and stream take the same instants", stream_twin);
    test("the tick route keeps its refusal", the_tick_route_keeps_its_refusal);
    test("placement gate converts at its acceptance point",
         placement_gate_converts_at_its_acceptance_point);
    test("the model spelling takes the same gate", the_model_spelling_takes_the_same_gate);
    test("placement gate refuses what its point cannot fund",
         placement_gate_refuses_what_its_point_cannot_fund);
    test("placement gate takes its equity at its point",
         placement_gate_takes_its_equity_at_its_point);
    test("placement gate nets its ticket at its point",
         placement_gate_nets_its_ticket_at_its_point);
    test("the freeze takes its equity at the same point",
         the_freeze_takes_its_equity_at_the_same_point);
    test("a gate without a curve is untouched", a_gate_without_a_curve_is_untouched);
    test("the stream gate decides what the batch decides",
         the_stream_gate_decides_what_the_batch_decides);
    std::printf("E3 margin FX clock: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
