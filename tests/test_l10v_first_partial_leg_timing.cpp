// R4-D L10v: On the switched route the FIRST partial exit leg of a multi-leg
// strategy.exit (qty_percent legs sharing one stop) fills on the bar where its
// limit level is touched, as the legacy owner (ab9714be) does, instead of one
// bar later at the same price.
//
// Pins owner rows #25-#27 of the
// zz-pop-projectsyndicate-strong-breakout-signals-projectsyndicate shape:
// process_orders_on_close=true, three strategy.exit legs
// ("XS1" qty_percent=40 limit=tTp1, "XS2" qty_percent=50 limit=tTp2,
// "XS3" limit=tTp3; all sharing stop=tStop). The short entry fills at the
// 2025-04-05 12:15 bar's close (1802.82). The 12:30 bar's close (1796.11) is
// already past TP1, so XS1 books on that same bar at the close; XS2 rests and
// fills at its limit on the 12:45 bar; XS3 at 13:00.
//
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

bool near(double a, double b, double tol = 1e-6) {
    return std::abs(a - b) <= tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

// 2025-04-05 12:15 .. 13:15 UTC (15m bars).
std::vector<Bar> projectsyndicate_bars() {
    return {
        mk(1743855300000LL, 1803.64, 1807.08, 1800.34, 1802.82, 13891.514), // 0: entry call bar
        mk(1743856200000LL, 1802.82, 1804.19, 1793.63, 1796.11, 10339.528), // 1: TP1 touched at close
        mk(1743857100000LL, 1796.11, 1797.04, 1791.74, 1797.03, 7291.851),  // 2: TP2 touched (L 1791.74)
        mk(1743858000000LL, 1797.04, 1797.92, 1790.78, 1791.61, 9068.760),  // 3: TP3 touched (L 1790.78)
        mk(1743858900000LL, 1791.61, 1791.96, 1784.25, 1787.96, 10666.187), // 4: flat bar
    };
}

class ProjectsyndicateHost : public source::PineStrategyHost {
public:
    ProjectsyndicateHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 10000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 0.55372982;  // owner's 10% equity entry size
        c.pyramiding = 0;
        c.commission_value = 0.0;
        c.slippage = 0;
        c.process_orders_on_close = true;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("Short", false);
        }
        if (i == 1 && live_position_size() < 0.0) {
            // Owner levels at the 12:15 signal: risk = 3.9333333;
            // tStop = 1806.7533333, tTp1 = 1798.8866667,
            // tTp2 = 1794.9533333, tTp3 = 1791.02.
            strategy_exit("XS1", "Short", 1798.886666666667, 1806.753333333333,
                          kNaN, kNaN, kNaN, 40.0);
            strategy_exit("XS2", "Short", 1794.953333333333, 1806.753333333333,
                          kNaN, kNaN, kNaN, 50.0);
            strategy_exit("XS3", "Short", 1791.02, 1806.753333333333,
                          kNaN, kNaN, kNaN, 100.0);
        }
    }
};

void test_first_partial_leg_fills_on_touch_bar() {
    std::printf("test_first_partial_leg_fills_on_touch_bar\n");
    ProjectsyndicateHost host;
    const auto bars = projectsyndicate_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 3);
    if (host.trade_count() == 3) {
        // Trade #25: XS1 fills on the touch bar (12:30 close 1796.11).
        const auto& t25 = host.get_trade(0);
        CHECK(t25.exit_id == "XS1");
        CHECK(t25.entry_id == "Short");
        CHECK(!t25.is_long);
        CHECK(near(t25.entry_price, 1802.82));
        CHECK(near(t25.exit_price, 1796.11));
        CHECK(near(t25.qty, 0.22149193));
        CHECK(t25.exit_time == 1743856200000LL);
        CHECK(near(t25.pnl, 1.486211, 1e-5));
        CHECK(near(t25.max_runup, 2.035511, 1e-5));
        CHECK(near(t25.max_drawdown, 0.303444, 1e-5));

        // Trade #26: XS2 rests and fills at its limit on the next bar.
        const auto& t26 = host.get_trade(1);
        CHECK(t26.exit_id == "XS2");
        CHECK(near(t26.entry_price, 1802.82));
        CHECK(near(t26.exit_price, 1794.95));
        CHECK(near(t26.qty, 0.27686491));
        CHECK(t26.exit_time == 1743857100000LL);
        CHECK(near(t26.pnl, 2.178927, 1e-5));
        CHECK(near(t26.max_runup, 2.544389, 1e-5));
        CHECK(near(t26.max_drawdown, 0.379305, 1e-5));

        // Trade #27: XS3 fills at its limit two bars after entry.
        const auto& t27 = host.get_trade(2);
        CHECK(t27.exit_id == "XS3");
        CHECK(near(t27.entry_price, 1802.82));
        CHECK(near(t27.exit_price, 1791.02));
        CHECK(near(t27.qty, 0.05537298));
        CHECK(t27.exit_time == 1743858000000LL);
        CHECK(near(t27.pnl, 0.653401, 1e-5));
        CHECK(near(t27.max_runup, 0.653401, 1e-5));
        CHECK(near(t27.max_drawdown, 0.075861, 1e-5));
    }
    CHECK(near(host.live_position_size(), 0.0));
}

}  // namespace

int main() {
    test_first_partial_leg_fills_on_touch_bar();
    std::printf("test_l10v_first_partial_leg_timing: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
