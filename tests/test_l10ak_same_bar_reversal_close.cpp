// R4-D L10ak: on the native route a strategy.close issued on the SAME bar as
// the reversal entry it is paired with is bound to the position cycle it was
// issued in: it closes only the lot that existed at issue time and never
// consumes any part of the reversal's fresh lot.  The legacy book Removes
// every EXIT that was created against a position the moment the book goes
// flat (ab9714be src/source/pine_fills.cpp:7461-7468) and keeps it Removed
// on every later bar while its ``from_entry`` has not filled in the CURRENT
// cycle (ab9714be src/source/pine_fills.cpp:7667-7675, 7672-7675), so the
// closed cycle's brackets can neither fill against the new lot on its own
// entry bar nor be revived against a later reuse of the same entry id.
// Without that rule one bracket leg of the retired cycle rests through the
// flat, fills against the reversal lot on the entry bar at that bar's print,
// and splits the reversal into two half lots — the first booked as a
// same-bar zero-PnL round trip.
//
// Both orderings the population probes use are pinned: the close issued
// before the reversal entry (shurben5 / ut-bot shape) and the entry issued
// first, where the stale close belongs to an earlier cycle than the live
// position and stays Refused (lane W27a, ab9714be
// src/source/pine_fills.cpp:7449-7459).
//
// Bars are literals (a 15m EURUSD-shaped path: a signal bar, a gap bar and a
// flip bar) and the facts mirror the zz-pop-shurben5-tradingview-bot-goat
// probe — mintick 0.00001, qty_step 0.01, initial_capital 10000,
// default_qty_type=strategy.percent_of_equity, default_qty_value=100,
// pyramiding=0, process_orders_on_close=false, commission percent 0,
// slippage 0, margin_long/short 10 — with that script's dollar targets
// converted to ticks (SL $1 / TP1 $1 half / TP2 $3 / trail lag $1 follow
// $0.5 at mintick 1e-5).  No corpus file is opened at runtime.
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;
const char* current_case = "";

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL [%s] %s:%d %s\n", current_case, __FILE__, \
                                 __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 10000;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 0;
    c.process_orders_on_close = false;
    c.calc_on_order_fills = false;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = 0.0;
    c.slippage = 0;
    c.margin_long = 10;
    c.margin_short = 10;
    return c;
}

// The probe script re-issues all four bracket exits on every bar and flips on
// a signal bar: strategy.close of the held side paired with strategy.entry of
// the opposite side.  ``entry_first_on_flip`` selects the ordering of the
// second (flip) pair only; the seeding pair at bar 0 and the signal pair at
// bar 4 are close-first in both cases.
class ReversalHost : public source::PineStrategyHost {
public:
    explicit ReversalHost(bool entry_first_on_flip)
        : entry_first_on_flip_(entry_first_on_flip) {
        configure_pine_strategy(cfg());
        set_syminfo_mintick(0.00001);
        set_syminfo_metadata("qty_step", 0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {                                   // seed: flat -> long
            strategy_close("Short");
            strategy_entry("Long", true);
        } else if (i == 4) {                            // signal: long -> short
            strategy_close("Long");
            strategy_entry("Short", false);
        } else if (i == 8) {                            // flip: short -> long
            if (entry_first_on_flip_) {
                strategy_entry("Long2", true);
                strategy_close("Short");
            } else {
                strategy_close("Short");
                strategy_entry("Long2", true);
            }
        }
        // SL $1 / TP1 $1 (half) / TP2 $3 / trail lag $1 follow $0.5, in ticks
        // at mintick 1e-5.
        strategy_exit("TP1 Long", "Long", kNaN, kNaN, kNaN, kNaN, kNaN, 50.0,
                      "", kNaN, "", 100000.0, kNaN);
        strategy_exit("Exit Long", "Long", kNaN, kNaN, 100000.0, 50000.0, kNaN,
                      100.0, "", kNaN, "", 300000.0, 100000.0);
        strategy_exit("TP1 Short", "Short", kNaN, kNaN, kNaN, kNaN, kNaN, 50.0,
                      "", kNaN, "", 100000.0, kNaN);
        strategy_exit("Exit Short", "Short", kNaN, kNaN, 100000.0, 50000.0, kNaN,
                      100.0, "", kNaN, "", 300000.0, 100000.0);
    }

private:
    bool entry_first_on_flip_;
};

std::vector<Bar> eurusd_15m() {
    const std::int64_t t0 = 1743427800000;
    const double ohlc[11][4] = {
        {1.07900, 1.07950, 1.07850, 1.07920},
        {1.08000, 1.08100, 1.07950, 1.08050},
        {1.08050, 1.08150, 1.08000, 1.08100},
        {1.08100, 1.08200, 1.08050, 1.08150},
        {1.08150, 1.08250, 1.08100, 1.08200},
        {1.09000, 1.09100, 1.08900, 1.09050},
        {1.09050, 1.09150, 1.09000, 1.09100},
        {1.09100, 1.09200, 1.09050, 1.09150},
        {1.09150, 1.09250, 1.09100, 1.09200},
        {1.08600, 1.08700, 1.08500, 1.08650},
        {1.08650, 1.08750, 1.08600, 1.08700},
    };
    std::vector<Bar> v;
    for (int i = 0; i < 11; ++i)
        v.push_back(mk(t0 + static_cast<std::int64_t>(i) * 900000,
                       ohlc[i][0], ohlc[i][1], ohlc[i][2], ohlc[i][3]));
    return v;
}

void run_case(const char* label, bool entry_first_on_flip,
              const std::string& flip_exit_id) {
    current_case = label;
    ReversalHost host(entry_first_on_flip);
    const auto bars = eurusd_15m();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());

    // Two rows only.  The removed phantom is a third row: half of the
    // reversal's short lot entered AND exited on bar 5 at 1.09000 with
    // Net PnL 0.000000 from a bracket of the retired long cycle
    // (exit_from_bracket, xid "Exit Short"), leaving the other half to run.
    CHECK(host.trade_count() == 2);
    if (host.trade_count() != 2) return;

    int short_rows = 0;
    for (int t = 0; t < host.trade_count(); ++t) {
        const auto& row = host.get_trade(t);
        // No lot may open and close on its own entry bar.
        CHECK(row.exit_bar_index != row.entry_bar_index);
        CHECK(!(row.entry_time == row.exit_time && near(row.pnl, 0.0)));
        CHECK(!row.exit_from_bracket);
        if (row.entry_id == "Short") ++short_rows;
    }
    // ONE short lot, of the full entry quantity — not two half lots.
    CHECK(short_rows == 1);

    // #1 Entry long bar 1 @1.08000 -> Exit long bar 5 @1.09000 (the paired
    // strategy.close("Long") of the signal bar), qty 9266.12, PnL +92.6612.
    const auto& t1 = host.get_trade(0);
    CHECK(t1.entry_id == "Long");
    CHECK(t1.is_long);
    CHECK(t1.entry_time == 1743428700000);
    CHECK(t1.entry_bar_index == 1);
    CHECK(near(t1.entry_price, 1.08000));
    CHECK(t1.exit_time == 1743432300000);
    CHECK(t1.exit_bar_index == 5);
    CHECK(near(t1.exit_price, 1.09000));
    CHECK(near(t1.qty, 9266.12, 1e-8));
    CHECK(near(t1.pnl, 92.6612, 1e-4));
    CHECK(near(t1.pnl_pct, 0.925926, 1e-5));
    CHECK(t1.exit_id == "__close__Long");
    CHECK(near(t1.max_runup, 92.6612, 1e-3));
    CHECK(near(t1.max_drawdown, 4.633060, 1e-3));

    // #2 Entry short bar 5 @1.09000, the FULL 9259.27 lot, survives its entry
    // bar and exits on bar 9 @1.08600 with PnL +37.037080.  Its exit id tells
    // the two orderings apart: the close-first flip exits through the flip
    // bar's own strategy.close("Short"), the entry-first flip through the
    // reversal entry that replaced it — the stale close stays Refused there.
    const auto& t2 = host.get_trade(1);
    CHECK(t2.entry_id == "Short");
    CHECK(!t2.is_long);
    CHECK(t2.entry_time == 1743432300000);
    CHECK(t2.entry_bar_index == 5);
    CHECK(near(t2.entry_price, 1.09000));
    CHECK(t2.exit_time == 1743435900000);
    CHECK(t2.exit_bar_index == 9);
    CHECK(near(t2.exit_price, 1.08600));
    CHECK(near(t2.qty, 9259.27, 1e-8));
    CHECK(near(t2.pnl, 37.037080, 1e-4));
    CHECK(near(t2.pnl_pct, 0.366972, 1e-5));
    CHECK(t2.exit_id == flip_exit_id);
    CHECK(near(t2.max_runup, 37.037080, 1e-3));
    CHECK(near(t2.max_drawdown, 23.148175, 1e-3));

    // The flip's freshly opened long lot stays OPEN: no bracket of the retired
    // short cycle and no stale close may touch it.
    CHECK(host.live_position_size() > 0.0);
}

}  // namespace

int main() {
    run_case("close-then-reversal", /*entry_first_on_flip=*/false,
             "__close__Short");
    run_case("reversal-then-stale-close", /*entry_first_on_flip=*/true,
             "Long2");
    std::printf("test_l10ak_same_bar_reversal_close: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
