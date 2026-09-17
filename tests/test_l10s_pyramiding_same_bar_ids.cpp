// R4-D L10s: With pyramiding=1, two strategy.entry calls with different ids
// that both fire on the same bar while flat admit only ONE lot, matching
// ab9714be legacy owner (corpus scenario mtf-roll-state-60-240-d-minimal-01:
// trade #77 Entry long @1618.67 / Exit @1608.90 once, never twice).
// Bars are embedded from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv —
// this test must never open corpus files (CI has no corpus checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-4) {
    return std::abs(a - b) <= tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg(int pyr) {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyr;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

// Bars from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv:
// 2025-04-10 04:45 UTC to 2025-04-10 06:45 UTC
std::vector<Bar> sample_bars() {
    return {
        mk(1744260300000LL, 1614.60, 1620.00, 1613.83, 1618.68, 51703.598),  // 0: 04:45 call bar (flat)
        mk(1744261200000LL, 1618.67, 1623.33, 1615.76, 1622.15, 47453.600),  // 1: 05:00 entry fill @1618.67
        mk(1744262100000LL, 1622.15, 1625.48, 1619.30, 1623.48, 56619.446),  // 2: 05:15
        mk(1744263000000LL, 1623.47, 1624.91, 1617.55, 1619.02, 38925.067),  // 3: 05:30
        mk(1744263900000LL, 1619.02, 1619.99, 1613.58, 1616.39, 47160.450),  // 4: 05:45
        mk(1744264800000LL, 1616.40, 1622.18, 1615.13, 1619.10, 39319.511),  // 5: 06:00
        mk(1744265700000LL, 1619.09, 1619.99, 1610.05, 1611.73, 55503.545),  // 6: 06:15
        mk(1744266600000LL, 1611.73, 1612.50, 1605.61, 1608.91, 60878.670),  // 7: 06:30 close_all() called
        mk(1744267500000LL, 1608.90, 1616.50, 1607.44, 1615.54, 41165.902),  // 8: 06:45 exit fill @1608.90
    };
}

class MtfRollPyramidingHost : public source::PineStrategyHost {
public:
    explicit MtfRollPyramidingHost(int pyr) {
        configure_pine_strategy(cfg(pyr));
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        // Bar 0 (04:45): both entry conditions fire while flat
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("R60", true, kNaN, kNaN, 1.0, "60 up with 240 state");
            strategy_entry("D", true, kNaN, kNaN, 1.0, "daily high break");
        }
        // Bar 7 (06:30): exit condition fires
        if (i == 7 && live_position_size() > 0.0) {
            strategy_close_all();
        }
    }
};

void test_pyramiding_1_admits_single_lot() {
    MtfRollPyramidingHost host(1);
    const auto bars = sample_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    CHECK(host.trade_count() == 1);

    if (host.trade_count() >= 1) {
        const auto& t0 = host.get_trade(0);
        CHECK(t0.is_long == true);
        CHECK(t0.entry_id == "R60");
        CHECK(t0.entry_time == 1744261200000LL);
        CHECK(near(t0.entry_price, 1618.67));
        CHECK(t0.exit_time == 1744267500000LL);
        CHECK(near(t0.exit_price, 1608.90));
        CHECK(near(t0.qty, 1.0));
        CHECK(near(t0.pnl, -9.77));
        CHECK(near(t0.max_runup, 6.81, 1e-2));
        CHECK(near(t0.max_drawdown, 13.06, 1e-2));
    }
}

void test_pyramiding_2_admits_both_lots() {
    MtfRollPyramidingHost host(2);
    const auto bars = sample_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    CHECK(host.trade_count() == 2);

    if (host.trade_count() >= 2) {
        const auto& t0 = host.get_trade(0);
        const auto& t1 = host.get_trade(1);
        CHECK(t0.entry_id == "R60");
        CHECK(t1.entry_id == "D");
        CHECK(near(t0.entry_price, 1618.67));
        CHECK(near(t1.entry_price, 1618.67));
        CHECK(near(t0.exit_price, 1608.90));
        CHECK(near(t1.exit_price, 1608.90));
    }
}

}  // namespace

int main() {
    test_pyramiding_1_admits_single_lot();
    test_pyramiding_2_admits_both_lots();
    std::printf("test_l10s_pyramiding_same_bar_ids: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
