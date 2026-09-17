// R4-D L10t: On the switched route a partial-quantity exit (strategy.exit /
// strategy.close with qty_percent, or a rounded exit whose remainder rounds to
// nothing) leaves NO residual open lot, exactly like the legacy owner (ab9714be).
//
// Pins owner literals for the three population probe reproductions:
// 1) zz-pop-officialjackofalltrades-concordance-execution-mandate-joat:
//    two 50% exit legs ("Long TP1", "Long TP2") leave no residual lot.
// 2) zz-pop-francescodimichele-gold-ai-strategy-v2-0:
//    full bracket exit ("EXIT_L") leaves no zero-qty residual lot.
// 3) zz-pop-p181342x-china-a-share-long-trend-resonance-system-clr-system:
//    two 30% partial closes followed by strategy.close("LONG") close the
//    remainder cleanly without opening an opposing short position or leaving dust.
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

bool near(double a, double b, double tol = 1e-4) {
    return std::abs(a - b) <= tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

// ---------------------------------------------------------------------------
// Scenario 1: zz-pop-officialjackofalltrades (Two 50% exit legs)
// Entry long @ 2584.91, q=3.88714072.
// Leg 1 ("Long TP1", qty_percent=50) limit 2638.33 -> fills 1.94357036
// Leg 2 ("Long TP2", qty_percent=50) limit 2691.77 -> fills 1.94357036
// Exactly 2 trades, position is flat, second entry can fill under pyramiding=0.
// ---------------------------------------------------------------------------
std::vector<Bar> joat_bars() {
    return {
        // 2025-06-09 19:15 UTC to 23:45 UTC
        mk(1749496500000LL, 2579.06, 2588.00, 2577.87, 2584.89, 38240.800),  // 0: Entry call bar
        mk(1749497400000LL, 2584.89, 2589.33, 2577.00, 2577.51, 43144.426),  // 1: Entry fill @ 2584.91
        mk(1749502800000LL, 2587.95, 2626.00, 2587.94, 2618.08, 234135.326), // 2: Rally begins
        mk(1749503700000LL, 2618.09, 2639.88, 2616.37, 2638.66, 124317.149), // 3: TP1 touch (H=2639.88 >= 2638.33)
        mk(1749510900000LL, 2658.70, 2665.00, 2656.68, 2661.66, 57876.663),  // 4: Consolidation
        mk(1749511800000LL, 2661.66, 2692.65, 2660.66, 2690.54, 156347.832), // 5: TP2 touch (H=2692.65 >= 2691.77)
        mk(1749512700000LL, 2690.65, 2693.20, 2678.00, 2679.12, 87146.695),  // 6: Flat bar
        mk(1749513600000LL, 2679.13, 2694.00, 2677.00, 2690.12, 117409.644), // 7: Second entry signal
        mk(1749514500000LL, 2690.12, 2719.66, 2687.51, 2716.91, 294332.263), // 8: Second entry fill
    };
}

class JoatHost : public source::PineStrategyHost {
public:
    JoatHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 3.88714072;
        c.pyramiding = 0;
        c.process_orders_on_close = false;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.05;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("Long", true);
            strategy_exit("Long TP1", "Long", 2638.33, 2550.0, kNaN, kNaN, kNaN, 50.0);
            strategy_exit("Long TP2", "Long", 2691.77, 2550.0, kNaN, kNaN, kNaN, 50.0);
        }
        if (i == 7 && live_position_size() == 0.0) {
            // Second entry succeeds because first position left NO residual lot
            strategy_entry("Long2", true);
        }
    }
};

void test_joat_two_fifty_percent_legs() {
    JoatHost host;
    const auto bars = joat_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    // Initial entry produced exactly 2 closed trades (TP1 and TP2)
    CHECK(host.trade_count() == 2);
    if (host.trade_count() >= 2) {
        const auto& t0 = host.get_trade(0);
        const auto& t1 = host.get_trade(1);
        CHECK(t0.entry_id == "Long");
        CHECK(t0.exit_id == "Long TP1");
        CHECK(near(t0.entry_price, 2584.89));
        CHECK(near(t0.exit_price, 2638.33));
        CHECK(near(t0.qty, 1.94357036));

        CHECK(t1.entry_id == "Long");
        CHECK(t1.exit_id == "Long TP2");
        CHECK(near(t1.entry_price, 2584.89));
        CHECK(near(t1.exit_price, 2691.77));
        CHECK(near(t1.qty, 1.94357036));
    }
    // And second entry successfully opened from flat
    CHECK(near(host.live_position_size(), 3.88714072));
}

// ---------------------------------------------------------------------------
// Scenario 2: zz-pop-francescodimichele (Full bracket exit no zero residual)
// Entry long @ 1563.07, q=0.32055283.
// Exit bracket "EXIT_L" limit 1579.92, stop 1548.24 -> fills 0.32055283.
// Position is flat, next entry can enter without being blocked by a 1e-16 lot.
// ---------------------------------------------------------------------------
std::vector<Bar> francesco_bars() {
    return {
        // 2025-04-11 12:30 UTC to 14:15 UTC
        mk(1744374600000LL, 1559.02, 1573.00, 1558.45, 1563.05, 114851.487), // 0: Signal bar
        mk(1744375500000LL, 1563.04, 1563.81, 1553.59, 1557.40, 70518.982),  // 1: Entry fill @ 1563.04
        mk(1744376400000LL, 1557.40, 1559.30, 1551.00, 1552.00, 74474.363),  // 2: In-trade
        mk(1744377300000LL, 1552.00, 1559.14, 1548.60, 1557.85, 70292.223),  // 3: In-trade
        mk(1744378200000LL, 1557.85, 1580.78, 1553.39, 1570.20, 255175.489), // 4: Exit touch (H=1580.78 >= 1579.92)
        mk(1744379100000LL, 1570.20, 1578.24, 1567.33, 1569.80, 102513.930), // 5: Next entry call bar
        mk(1744380000000LL, 1569.80, 1569.80, 1556.00, 1563.78, 161266.025), // 6: Next entry fill bar
    };
}

class FrancescoHost : public source::PineStrategyHost {
public:
    FrancescoHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 0.32055283;
        c.pyramiding = 0;
        c.process_orders_on_close = false;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("LONG", true);
            strategy_exit("EXIT_L", "LONG", 1579.92, 1548.24);
        }
        if (i == 4 && live_position_size() > 0.0) {
            // Re-issue bracket while in position
            strategy_exit("EXIT_L", "LONG", 1579.92, 1548.24);
        }
        if (i == 5 && live_position_size() == 0.0) {
            // Next entry succeeds because position completely flattened
            strategy_entry("LONG2", true);
        }
    }
};

void test_francesco_full_exit_bracket_no_zero_residual() {
    FrancescoHost host;
    const auto bars = francesco_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() >= 1) {
        const auto& t0 = host.get_trade(0);
        CHECK(t0.entry_id == "LONG");
        CHECK(t0.exit_id == "EXIT_L");
        CHECK(near(t0.entry_price, 1563.04));
        CHECK(near(t0.exit_price, 1579.92));
        CHECK(near(t0.qty, 0.32055283));
    }
    CHECK(near(host.live_position_size(), 0.32055283));
}

// ---------------------------------------------------------------------------
// Scenario 3: zz-pop-p181342x (Percent closes then strategy.close)
// Entry long @ 2705.78, q=412.21866683.
// strategy.close("LONG", qty_percent=30) -> closes 123.66560005
// strategy.close("LONG", qty_percent=30) -> closes 123.66560005
// strategy.close("LONG") -> closes remaining 164.88746673
// Exactly 3 trades, position is flat, NO opposing short position opened.
// ---------------------------------------------------------------------------
std::vector<Bar> p181342x_bars() {
    return {
        // 2025-06-10 10:45 UTC to 11:30 UTC
        mk(1749552300000LL, 2683.17, 2694.61, 2682.70, 2694.59, 33281.470),  // 0: Entry call bar
        mk(1749553200000LL, 2694.60, 2708.99, 2687.58, 2705.78, 92063.192),  // 1: Entry fill @ 2705.78 (POOC)
        mk(1749554100000LL, 2705.77, 2762.98, 2705.77, 2759.51, 482928.929), // 2: Three closes executed
        mk(1749555000000LL, 2759.51, 2798.72, 2753.45, 2788.79, 357102.885), // 3: Flat bar
    };
}

class P181342xHost : public source::PineStrategyHost {
public:
    P181342xHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 10000000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 412.21866683;
        c.pyramiding = 0;
        c.process_orders_on_close = true;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("LONG", true);
        }
        if (i == 2 && live_position_size() > 0.0) {
            strategy_close("LONG", "", kNaN, 30.0, false, 1);
            strategy_close("LONG", "", kNaN, 30.0, false, 2);
            strategy_close("LONG", "", kNaN, kNaN, false, 3);
        }
    }
};

void test_p181342x_percent_closes_then_full_close() {
    P181342xHost host;
    const auto bars = p181342x_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    CHECK(host.trade_count() == 3);
    if (host.trade_count() >= 3) {
        const auto& t0 = host.get_trade(0);
        const auto& t1 = host.get_trade(1);
        const auto& t2 = host.get_trade(2);
        CHECK(t0.is_long);
        CHECK(t1.is_long);
        CHECK(t2.is_long);
        CHECK(near(t0.qty, 123.66560005));
        CHECK(near(t1.qty, 123.66560005));
        CHECK(near(t2.qty, 164.88746673));
    }
}

}  // namespace

int main() {
    test_joat_two_fifty_percent_legs();
    test_francesco_full_exit_bracket_no_zero_residual();
    test_p181342x_percent_closes_then_full_close();
    std::printf("test_l10t_partial_exit_residual: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
