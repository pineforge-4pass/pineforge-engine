// R4-D L10p: 3-way bracket exit set once at entry fires on the entry bar itself
// (bracket-exit-three-way-set-once-entry-01 trade #539).
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { \
        ++passed; \
    } else { \
        ++failed; \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
    } \
} while (0)

bool near(double a, double b, double eps = 1e-4) {
    return std::abs(a - b) < eps;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

class ThreeWaySetOnceHost : public source::PineStrategyHost {
public:
    ThreeWaySetOnceHost() {
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1000000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 1;
        cfg.commission_type = static_cast<int>(CommissionType::PERCENT);
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.process_orders_on_close = false;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("L", true, kNaN, kNaN, 1.0, "entry long");
            strategy_exit("LX", "L", bar.close * 1.02, bar.close * 0.99, 20.0,
                          kNaN, kNaN, 100.0, "3-way set once");
        }
    }
};

std::vector<Bar> trade539_bars() {
    return {
        // 2025-12-25 08:00 UTC
        mk(1766649600000LL, 2940.83, 2946.0, 2937.0, 2937.0, 14299.513),
        // 2025-12-25 08:15 UTC (high is exact 2937.2)
        mk(1766650500000LL, 2937.0, 2937.2, 2916.05, 2922.06, 114856.186),
        // 2025-12-25 08:30 UTC
        mk(1766651400000LL, 2922.07, 2927.0, 2919.69, 2926.25, 21869.148),
    };
}

class Trade10Host : public source::PineStrategyHost {
public:
    Trade10Host() {
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1000000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 1;
        cfg.commission_type = static_cast<int>(CommissionType::PERCENT);
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.process_orders_on_close = false;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("S", false, kNaN, kNaN, 1.0, "entry short");
            strategy_exit("SX", "S", bar.close * 0.98, bar.close * 1.01, 20.0,
                          kNaN, kNaN, 100.0, "3-way set once");
        }
    }
};

std::vector<Bar> trade10_bars() {
    return {
        // 2025-04-04 20:00 UTC
        mk(1743796800000LL, 1809.03, 1812.71, 1807.8, 1809.55, 26413.663),
        // 2025-04-04 20:15 UTC
        mk(1743797700000LL, 1809.55, 1810.48, 1806.43, 1809.49, 24009.874),
        // 2025-04-04 20:30 UTC
        mk(1743798600000LL, 1809.49, 1819.36, 1808.89, 1818.24, 30706.899),
    };
}

}  // namespace

int main() {
    {
        ThreeWaySetOnceHost host;
        const auto bars = trade539_bars();
        host.run(bars.data(), static_cast<int>(bars.size()));

        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() == 1) {
            const auto& t = host.get_trade(0);
            CHECK(t.is_long);
            CHECK(t.entry_time == 1766650500000LL);
            CHECK(near(t.entry_price, 2937.00));
            CHECK(t.exit_time == 1766650500000LL);
            CHECK(near(t.exit_price, 2937.20));
            CHECK(near(t.pnl, 0.20));
            CHECK(near(t.max_runup, 0.20));
            CHECK(near(t.max_drawdown, 0.00));
        }
    }
    {
        Trade10Host host;
        const auto bars = trade10_bars();
        host.run(bars.data(), static_cast<int>(bars.size()));

        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() == 1) {
            const auto& t = host.get_trade(0);
            CHECK(!t.is_long);
            CHECK(t.entry_time == 1743797700000LL);
            CHECK(near(t.entry_price, 1809.55));
            CHECK(t.exit_time == 1743797700000LL);
            CHECK(near(t.exit_price, 1809.35));
            CHECK(near(t.pnl, 0.20));
            CHECK(near(t.max_runup, 0.20));
            CHECK(near(t.max_drawdown, 0.93));
        }
    }

    std::printf("test_l10p_three_way_entry_bar_exit: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
