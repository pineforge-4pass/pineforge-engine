// Pine-free native example: an account-currency FX curve, and the margin
// check point a step of it produces (NativeFxCurve, NativeMarginCheckKind::
// FxRoll).
//
// A JPY account (3,000,000 JPY) holds 750 shares of a USD-quoted stock on a
// maintenance-only margin model: no opening requirement on the long side,
// a 10 % maintenance requirement, liquidation by Flatten at the solved level.
// Every money figure converts quote -> account at the rate in force, and the
// run declares that rate as a curve rather than one scalar:
//
//   before the first point   150 JPY/USD   (NativeRunSpec::account_fx)
//   at bar 2's open          150           restates the rate: NOT a step
//   at bar 4's open          160           a step: the yen weakens
//
// The requirement carries the rate -- required(P) = Q * P * fx * 10 % -- so the
// step moves the account toward its maintenance line while no price moves.
// The kernel therefore offers the step as a check point of its own, FxRoll,
// immediately before the first driver point that converts at the new rate. A
// curve point stamped at a bar's OPEN activates at the PREVIOUS bar's Close
// coordinate, whose effective time is that same instant: bar 3's Close
// segment, standing at its Low 212.90 with only the Close 213.00 left to walk,
// so the mark (the most adverse price a long still reaches) is 212.90.
//
// The liquidation level is the price where the marked equity meets the
// requirement. With K = 3,000,000 - 23,968.125 (the entry fee, paid at 150):
//
//   L(fx) = (213.05 * 750 * fx - K) / (750 * fx * 0.9)
//   L(150) = 207.3293...     L(160) = 209.1663...
//
// Bar 5 crashes to 208.40: through L(160), not through L(150). With the step
// the kernel liquidates the whole position at L(160); the same run with a
// curve that never steps (only the restating point) offers no FxRoll point at
// all and liquidates nothing. The rate moved the line, not the price.
//
//   c++ -std=c++17 native_fx_roll_strategy.cpp -lpineforge_kernel -o fx_roll
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <vector>

namespace {

namespace no = pineforge::native_order;

constexpr std::int64_t kQuarter = 15LL * 60LL * 1000LL;
constexpr double kUnits = 750.0;
constexpr double kEntryPrice = 213.05;
constexpr double kFeePercent = 0.1;          // 0.1 % of the account notional
constexpr double kCapital = 3000000.0;       // JPY
constexpr double kMaintenance = 0.10;

// The account after the entry: the capital less the entry fee, paid at 150.
constexpr double kBase = kCapital - kUnits * kEntryPrice * 150.0 * kFeePercent / 100.0;

// The hand-solved liquidation level at an account rate.
double level(double fx) {
    return (kEntryPrice * kUnits * fx - kBase) / (kUnits * fx * (1.0 - kMaintenance));
}

bool near(double value, double expected, double tolerance) {
    return std::isfinite(value) && std::fabs(value - expected) <= tolerance;
}

class FxRollExample : public pineforge::NativeStrategyHost {
public:
    // What every check point offered looked like: margin_check_allowed and
    // resolve_margin_requirement are const, so the record is mutable.
    struct Offered {
        pineforge::NativeMarginCheckKind kind = pineforge::NativeMarginCheckKind::BarOpen;
        double units = 0.0;
        std::size_t lots = 0;
        double mark = 0.0;
        std::int64_t time_ms = 0;
        pineforge::NativePathPhase phase = pineforge::NativePathPhase::None;
        int interval_index = -1;
    };
    mutable std::vector<Offered> offered;
    mutable std::optional<pineforge::NativeMarginRequirementView> roll_view;
    std::optional<double> level_bar2;      // native_liquidation_price() at bar 2
    std::optional<double> level_bar4;      // ... and at bar 4, after the step
    std::optional<no::MarginCallEvent> margin_call;

private:
    int bar_ = -1;

    void on_native_run_begin() override { bar_ = -1; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == 0) submit({no::Transact{kUnits}, "entry", "fx-roll"});
        if (bar_ == 2) level_bar2 = native_liquidation_price();
        if (bar_ == 4) level_bar4 = native_liquidation_price();
    }

    bool margin_check_allowed(const pineforge::NativeMarginCheckPoint& point) const override {
        Offered row;
        row.kind = point.kind;
        row.units = point.position.signed_units;
        row.lots = point.position.lot_count;
        row.mark = point.mark;
        row.time_ms = point.cursor.point.effective_time_ms;
        row.phase = point.cursor.point.path_phase;
        row.interval_index = point.cursor.point.interval_index;
        offered.push_back(row);
        return true;
    }

    // The numbers the kernel is about to compare at the roll: both at the NEW
    // rate. nullopt keeps the kernel's own.
    std::optional<pineforge::NativeMarginDecision> resolve_margin_requirement(
            const pineforge::NativeMarginRequirementView& view) const override {
        if (view.kind == pineforge::NativeMarginCheckKind::FxRoll && !roll_view) roll_view = view;
        return std::nullopt;
    }

    void on_native_margin_call(const no::MarginCallEvent& event) override {
        margin_call = event;
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-fx-roll-example";
    spec.identity.run_number = 1;
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "ACME";
    spec.tickerid = "TEST:ACME";
    spec.type = "stock";
    spec.currency = "USD";            // the instrument's quote currency
    spec.basecurrency = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = kCapital;  // the account, in JPY
    spec.point_value = 1.0;
    spec.account_fx = 150.0;          // JPY per USD before the curve's first point
    spec.price_tick = 0.05;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = kFeePercent;     // a percent: 0.1 %
    spec.quantity_grid = 1.0;

    // Maintenance-only on the long side: initial_long 0.0 leaves opening
    // admission to the host. Each side must declare a requirement, so the
    // short side keeps an ordinary one.
    pineforge::NativeMarginModel margin;
    margin.initial_long = 0.0;
    margin.initial_short = 0.5;
    margin.maintenance_long = kMaintenance;
    margin.sizing = pineforge::NativeLiquidationSizing::Flatten;
    margin.check = pineforge::NativeLiquidationCheck::PathAdverseExtreme;
    margin.liquidation_label = "margin-call";
    margin.liquidation_comment = "maintenance 10%";
    spec.margin = margin;
    return spec;
}

// open, high, low, close, volume, timestamp (Unix milliseconds).
const pineforge::Bar kBars[] = {
    {212.95, 213.10, 212.85, 213.02,  900.0, 0 * kQuarter},   // submit 750 shares
    {213.05, 213.20, 212.90, 213.10, 1200.0, 1 * kQuarter},   // filled at 213.05
    {213.10, 213.25, 212.95, 213.15,  900.0, 2 * kQuarter},   // the restating point
    {213.15, 213.30, 212.90, 213.00,  900.0, 3 * kQuarter},   // its Close: the roll
    {213.00, 213.10, 212.75, 212.80, 1200.0, 4 * kQuarter},   // 160 from this open
    {212.60, 212.70, 208.40, 208.90, 3000.0, 5 * kQuarter},   // the crash
    {208.90, 209.20, 208.60, 209.00,  900.0, 6 * kQuarter},
};
constexpr int kBarCount = 7;

pineforge::NativeFxCurve curve(bool step) {
    pineforge::NativeFxCurve fx;
    fx.effective_from_ms.push_back(2 * kQuarter);
    fx.account_per_quote.push_back(150.0);
    if (step) {
        fx.effective_from_ms.push_back(4 * kQuarter);
        fx.account_per_quote.push_back(160.0);
    }
    return fx;
}

std::vector<FxRollExample::Offered> rolls_of(const FxRollExample& host) {
    std::vector<FxRollExample::Offered> rolls;
    for (const auto& row : host.offered) {
        if (row.kind == pineforge::NativeMarginCheckKind::FxRoll) rolls.push_back(row);
    }
    return rolls;
}

// Configure, stage the curve while the host is Ready, run.
void run(FxRollExample& host, bool step) {
    const auto setup = host.configure_native(make_spec());
    assert(setup.status == pineforge::NativeSetupStatus::Applied);
    const auto staged = host.configure_native_fx_curve(curve(step));
    assert(staged.status == pineforge::NativeSetupStatus::Applied);
    host.run(kBars, kBarCount);
    assert(host.native_state().kind == pineforge::NativeLifecycleKind::Completed);
}

}  // namespace

int main() {
    // --- the curve steps 150 -> 160 ------------------------------------------
    FxRollExample stepped;
    run(stepped, true);

    // One FxRoll point: the restating point at bar 2 is not a step.
    const auto rolls = rolls_of(stepped);
    std::printf("FxRoll points: %zu\n", rolls.size());
    assert(rolls.size() == 1);
    const auto& roll = rolls[0];
    std::printf("  at t=%lld (bar 4's open), bar %d's %s: %g units in %zu lot, mark %.2f\n",
                static_cast<long long>(roll.time_ms), roll.interval_index,
                roll.phase == pineforge::NativePathPhase::Close ? "Close" : "?",
                roll.units, roll.lots, roll.mark);
    assert(roll.time_ms == 4 * kQuarter && roll.interval_index == 3
           && roll.phase == pineforge::NativePathPhase::Close);
    assert(roll.units == kUnits && roll.lots == 1 && roll.mark == 212.90);

    // The requirement view at the roll converts at the NEW rate:
    //   equity   = K + (212.90 - 213.05) * 750 * 160 = K - 18,000
    //   required = 750 * 212.90 * 160 * 10 %         = 2,554,800
    // No breach yet: the step moved the line, it did not cross it.
    assert(stepped.roll_view.has_value());
    std::printf("  requirement at the roll: equity %.4f, required %.4f\n",
                stepped.roll_view->equity, stepped.roll_view->required);
    assert(near(stepped.roll_view->equity, kBase - 18000.0, 1e-6));
    assert(near(stepped.roll_view->required, kUnits * 212.90 * 160.0 * kMaintenance, 1e-6));
    assert(stepped.roll_view->equity > stepped.roll_view->required);

    // The level moved with the rate.
    std::printf("liquidation level: bar 2 %.6f (hand %.6f at 150), bar 4 %.6f (hand %.6f at 160)\n",
                stepped.level_bar2.value_or(-1.0), level(150.0),
                stepped.level_bar4.value_or(-1.0), level(160.0));
    assert(stepped.level_bar2 && near(*stepped.level_bar2, level(150.0), 1e-9));
    assert(stepped.level_bar4 && near(*stepped.level_bar4, level(160.0), 1e-9));

    // Bar 5 crosses L(160): the kernel flattens the book at the level, under
    // the model's ticket, and books the loss and the exit fee at 160.
    assert(stepped.margin_call.has_value());
    const auto& call = *stepped.margin_call;
    std::printf("margin call: %g units at %.6f, position %g -> %g, ticket %s\n", call.units,
                call.mark, call.position_before, call.position_after,
                call.request().label.c_str());
    assert(call.units == kUnits && call.position_before == kUnits && call.position_after == 0.0);
    assert(near(call.mark, level(160.0), 1e-9) && call.request().label == "margin-call");
    assert(stepped.trade_count() == 1);
    const auto& trade = stepped.get_trade(0);
    const double entry_fee = kUnits * kEntryPrice * 150.0 * kFeePercent / 100.0;
    const double exit_fee = kUnits * trade.exit_price * 160.0 * kFeePercent / 100.0;
    std::printf("  row: %s -> %s, commission %.4f, pnl %.4f\n", trade.entry_id.c_str(),
                trade.exit_id.c_str(), trade.commission, trade.pnl);
    assert(trade.exit_id == "margin-call" && trade.exit_price == call.mark);
    assert(near(trade.commission, entry_fee + exit_fee, 1e-6));
    assert(near(trade.pnl, (trade.exit_price - kEntryPrice) * kUnits * 160.0 - entry_fee - exit_fee,
                1e-6));

    // --- the same run, the curve never steps ---------------------------------
    FxRollExample flat;
    run(flat, false);
    std::printf("no step: FxRoll points %zu, level at bar 4 %.6f, margin call %s, closed trades %d\n",
                rolls_of(flat).size(), flat.level_bar4.value_or(-1.0),
                flat.margin_call ? "yes" : "none", flat.trade_count());
    assert(rolls_of(flat).empty());
    assert(flat.level_bar4 && near(*flat.level_bar4, level(150.0), 1e-9));
    assert(!flat.margin_call.has_value() && flat.trade_count() == 0);
    assert(flat.physical_position().signed_units == kUnits);

    // The summary line, printed only once every check above has passed.
    std::printf("FxRoll units=%g mark=%.2f  no-step FxRoll points: %zu, margin calls: %d  "
                "closed trades: %d\n",
                roll.units, roll.mark, rolls_of(flat).size(), flat.margin_call ? 1 : 0,
                stepped.trade_count());
    return 0;
}
