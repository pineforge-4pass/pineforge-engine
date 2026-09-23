// Pine-free native example: the instrument price grid (R5 lanes L8 and L8b).
//
// The instrument trades on a 0.25 ladder; the feed is a composite whose prints
// land between the ticks. NativeRunSpec::price_grid says what the broker model
// does about that, and the same strategy is run on the same six bars under
// each answer:
//   * None (the default): price_tick is only the slippage multiplier, every
//     booked price is the raw modeled one;
//   * QuantizeFills + HalfUp: a fill books on the nearest tick (ties away from
//     zero); a resting trigger is still tested against the raw path;
//   * QuantizeFills + Directional: a market fill and a stop book on the
//     adverse tick, a limit on its own favourable side;
//   * QuantizeFillsAndTriggers + HalfUp: a resting trigger is additionally
//     tested against the tick-quantized path, so the breakout stop at 99.75 is
//     reached by a raw high of 99.65 (its nearest tick is 99.75) and by no
//     other mode.
// A level that already is a ladder price is a fixed point of every mode: the
// 100.75 target books 100.75 four times. A quantizing grid needs a ladder:
// price_grid != None with price_tick == 0 is refused at configure time.
//
//   c++ -std=c++17 native_price_grid_strategy.cpp -lpineforge_kernel -o price_grid
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.
// The Pine adapter never declares this field (it keeps TradingView's per-order
// tick rules on top of None); the grid is a native-host feature by ruling —
// docs/design/native-feature-parity.md §3.6.

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

namespace no = pineforge::native_order;

constexpr double kTick = 0.25;

class PriceGridExample : public pineforge::NativeStrategyHost {
private:
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            // Two units at the market: filled at the next bar's open, 100.10 raw.
            submit({no::Transact{2.0}, "entry", "grid"});
            return;
        }
        if (bars_ == 2) {
            // Both levels are ladder prices, as a host that knows its
            // instrument spells them.
            no::Request target{no::Reduce{no::ExplicitUnits{1.0}}, "target", "grid"};
            target.trigger = no::Limit{100.75};
            submit(target);
            no::Request protect{no::Reduce{no::ExplicitUnits{1.0}}, "protect", "grid"};
            protect.trigger = no::Stop{99.50};
            submit(protect);
            return;
        }
        if (bars_ == 4) {
            // Flat again. A breakout entry one tick above the recent prints.
            no::Request breakout{no::Transact{1.0}, "breakout", "grid"};
            breakout.trigger = no::Stop{99.75};
            submit(breakout);
            return;
        }
        if (bars_ == 5 && physical_position().signed_units > 0.0) {
            submit({no::Flatten{}, "breakout-exit", "grid"});
        }
    }
};

pineforge::NativeRunSpec make_spec(const char* key, pineforge::NativePriceGrid grid,
                                   pineforge::NativeGridRounding rounding) {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = 1;
    // This example reads its whole event record once the run has ended, so
    // it keeps it all: the default, Window, keeps only what a host has not
    // yet acknowledged (native_acknowledge_events).
    spec.event_retention = pineforge::NativeEventRetention::Full;
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "futures";
    spec.currency = "USD";
    spec.basecurrency = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = kTick;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    spec.price_grid = grid;
    spec.grid_rounding = rounding;
    return spec;
}

constexpr std::int64_t kQuarter = 15LL * 60LL * 1000LL;

// open, high, low, close, volume, timestamp (Unix milliseconds). Sub-tick
// prints throughout: 100.10 opens the entry, 100.80 trades through the target,
// 99.40 gaps under the 99.50 stop, 99.65 is the high that only a quantized
// path carries to 99.75, and 99.60 opens the breakout's exit.
const pineforge::Bar kBars[] = {
    {100.00, 100.20,  99.90, 100.10, 10.0, 0 * kQuarter},
    {100.10, 100.30, 100.05, 100.20, 10.0, 1 * kQuarter},
    {100.20, 100.80, 100.15, 100.70, 10.0, 2 * kQuarter},
    { 99.40,  99.45,  99.20,  99.30, 10.0, 3 * kQuarter},
    { 99.45,  99.65,  99.35,  99.60, 10.0, 4 * kQuarter},
    { 99.60,  99.62,  99.30,  99.40, 10.0, 5 * kQuarter},
};
constexpr int kBarCount = 6;

struct Fill {
    std::string label;
    double raw = 0.0;       // the modeled price of the path, never rounded
    double booked = 0.0;    // what the run settled
};

struct Mode {
    const char* name;
    pineforge::NativePriceGrid grid;
    pineforge::NativeGridRounding rounding;
    // Hand-computed from the rounding rules above, never read back.
    std::vector<Fill> expected;
    double expected_net;
};

bool on_ladder(double price) { return price == std::round(price / kTick) * kTick; }

bool run_mode(const Mode& mode, int& closed_trades) {
    PriceGridExample host;
    if (host.configure_native(make_spec(mode.name, mode.grid, mode.rounding)).status
        != pineforge::NativeSetupStatus::Applied) {
        std::cerr << mode.name << " configure: " << host.last_error() << '\n';
        return false;
    }
    host.run(kBars, kBarCount);
    if (host.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << mode.name << " run: " << host.last_error() << '\n';
        return false;
    }

    std::vector<Fill> fills;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&*event.command)) {
            fills.push_back({applied->request().label, applied->raw_price, applied->resolved_price});
        }
    }

    double net = 0.0;
    for (int i = 0; i < host.trade_count(); ++i) net += host.get_trade(i).pnl;

    std::printf("%-34s", mode.name);
    for (const auto& fill : fills) {
        std::printf(" %s %.2f->%.2f", fill.label.c_str(), fill.raw, fill.booked);
    }
    std::printf(" | trades=%d net=%+.2f\n", host.trade_count(), net);

    if (fills.size() != mode.expected.size()) {
        std::cerr << mode.name << ": expected " << mode.expected.size() << " fills, got "
                  << fills.size() << '\n';
        return false;
    }
    for (std::size_t i = 0; i < fills.size(); ++i) {
        const Fill& want = mode.expected[i];
        if (fills[i].label != want.label || fills[i].raw != want.raw
            || fills[i].booked != want.booked) {
            std::cerr << mode.name << ": fill " << i << " expected " << want.label << ' '
                      << want.raw << "->" << want.booked << '\n';
            return false;
        }
        // Under a grid every booked price is a ladder price; the raw modeled
        // price is a fact of the path and is never rounded.
        if (mode.grid != pineforge::NativePriceGrid::None && !on_ladder(fills[i].booked)) {
            std::cerr << mode.name << ": " << fills[i].label << " booked off the ladder\n";
            return false;
        }
    }
    if (std::fabs(net - mode.expected_net) > 1e-9) {
        std::cerr << mode.name << ": expected net " << mode.expected_net << ", got " << net << '\n';
        return false;
    }
    closed_trades += host.trade_count();
    return true;
}

}  // namespace

int main() {
    using pineforge::NativeGridRounding;
    using pineforge::NativePriceGrid;

    // entry: the market fill at bar 1's open, 100.10 — nearest tick 100.00,
    //        adverse (buy) tick 100.25.
    // target: the sell limit at the ladder price 100.75 — its own fixed point.
    // protect: the sell stop at 99.50, gapped through by bar 3's open 99.40 —
    //        nearest tick 99.50, adverse (sell) tick 99.25.
    // breakout: the buy stop at 99.75; bar 4's raw high is 99.65, whose nearest
    //        tick is 99.75 — reached on the quantized path only. Its market exit
    //        at bar 5's open 99.60 books the nearest tick, 99.50.
    const std::vector<Mode> modes = {
        {"None", NativePriceGrid::None, NativeGridRounding::HalfUp,
         {{"entry", 100.10, 100.10}, {"target", 100.75, 100.75}, {"protect", 99.40, 99.40}},
         (100.75 - 100.10) + (99.40 - 100.10)},
        {"QuantizeFills/HalfUp", NativePriceGrid::QuantizeFills, NativeGridRounding::HalfUp,
         {{"entry", 100.10, 100.00}, {"target", 100.75, 100.75}, {"protect", 99.40, 99.50}},
         0.75 - 0.50},
        {"QuantizeFills/Directional", NativePriceGrid::QuantizeFills, NativeGridRounding::Directional,
         {{"entry", 100.10, 100.25}, {"target", 100.75, 100.75}, {"protect", 99.40, 99.25}},
         0.50 - 1.00},
        {"QuantizeFillsAndTriggers/HalfUp", NativePriceGrid::QuantizeFillsAndTriggers,
         NativeGridRounding::HalfUp,
         {{"entry", 100.10, 100.00}, {"target", 100.75, 100.75}, {"protect", 99.40, 99.50},
          {"breakout", 99.75, 99.75}, {"breakout-exit", 99.60, 99.50}},
         0.75 - 0.50 - 0.25},
    };

    int closed_trades = 0;
    for (const auto& mode : modes) {
        if (!run_mode(mode, closed_trades)) return 1;
    }

    // A quantizing grid without a ladder is refused, not silently disabled.
    {
        PriceGridExample host;
        auto spec = make_spec("no-ladder", NativePriceGrid::QuantizeFills, NativeGridRounding::HalfUp);
        spec.price_tick = 0.0;
        const auto setup = host.configure_native(spec);
        const bool refused = setup.status == pineforge::NativeSetupStatus::Failed
            && setup.validation.error == pineforge::NativeRunSpecError::GridRequiresPriceTick
            && setup.validation.field == pineforge::NativeRunSpecField::PriceGrid;
        std::printf("QuantizeFills with price_tick = 0: %s\n",
                    refused ? "refused (GridRequiresPriceTick on PriceGrid)" : "NOT refused");
        if (!refused) return 1;
    }

    std::cout << "closed trades: " << closed_trades << '\n';
    return closed_trades > 0 ? 0 : 1;
}
