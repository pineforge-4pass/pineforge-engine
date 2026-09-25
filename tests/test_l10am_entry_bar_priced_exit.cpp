// R4-D L10am: Priced exit fills on the same bar and at the same price as owner
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

// ---------------------------------------------------------------------------
// Probe 1: Traderhayz Prison Escape Breakout Strategy (ETH 15m)
// Trade #1:
// On bar 0 (2025-05-21 15:00 UTC), strategy places market long entry and exit
// stop=2562.25, limit=2614.01.
// On bar 1 (2025-05-21 15:15 UTC): O 2564.01, H 2576.96, L 2560.83, C 2569.84.
// Entry long fills at open 2564.01.
// Exit long stop fills on the same entry bar at 2562.25.
// Owner literals: entry 2564.01, exit 2562.25, qty 1.0, pnl -1.76.
// ---------------------------------------------------------------------------
class TraderhayzHost : public source::PineStrategyHost {
public:
    TraderhayzHost() {
        attach_pine_execution_adapter();
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
            strategy_entry("Long", true, kNaN, kNaN, 1.0, "");
            strategy_exit("Exit", "", 2614.01, 2562.25, kNaN, kNaN, kNaN, 100.0, "", kNaN, "", kNaN, kNaN);
        }
    }
};

std::vector<Bar> traderhayz_bars() {
    return {
        // 0: 2025-05-21 15:00 UTC (signal bar)
        mk(1747839600000LL, 2579.33, 2581.96, 2555.8, 2564.01, 236828.33),
        // 1: 2025-05-21 15:15 UTC (entry + exit bar)
        mk(1747840500000LL, 2564.01, 2576.96, 2560.83, 2569.84, 107427.887),
        // 2: 2025-05-21 15:30 UTC
        mk(1747841400000LL, 2569.84, 2576.31, 2541.65, 2559.13, 185452.685),
    };
}

void test_traderhayz_entry_bar_stop() {
    TraderhayzHost host;
    const auto bars = traderhayz_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& t = host.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_time == 1747840500000LL);
        CHECK(near(t.entry_price, 2564.01));
        CHECK(t.exit_time == 1747840500000LL);
        CHECK(near(t.exit_price, 2562.25));
        CHECK(near(t.qty, 1.0));
        CHECK(near(t.pnl, -1.76));
    }
}

// ---------------------------------------------------------------------------
// Probe 2: Vasudevshenoy Manoj Betrayed Me (OANDA:XAUUSD 15m)
// Trade #18:
// On bar 0 (2025-04-16 16:45 UTC), strategy places stop buy entry @ 3327.38
// and exit limit @ 3337.46, stop @ 3320.00.
// The buy stop fills @ 3327.38 at bar 0's close: under process_orders_on_close
// the placing close (3327.38) already reaches it. TradingView's own trade 18
// enters at 2025-04-16 16:45 UTC, Duration 6 bars; the owner (ab9714be) filled
// it a bar later, on bar 1 (2025-04-16 17:00 UTC), and so did this engine until
// R5 lane PAR-ORDERS, which moved the pin to TradingView's bar.
// On bar 6 (2025-04-16 18:15 UTC): O 3332.235, H 3338.895, L 3331.735, C 3338.375.
// Exit long limit fills @ 3337.46.
// Owner literals: entry 3327.38, exit 3337.46, qty 1.0, pnl 10.08.
// ---------------------------------------------------------------------------
class VasudevshenoyHost : public source::PineStrategyHost {
public:
    VasudevshenoyHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1000000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 0;
        cfg.commission_type = static_cast<int>(CommissionType::PERCENT);
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.process_orders_on_close = true;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("XAUUSD", 0.001);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("Buy", true, kNaN, 3327.38, 1.0, "");
        }
        // The script re-issues the bracket on every bar, as the corpus script
        // does, so each bar's body re-prices the same named exit instance.
        strategy_exit("Buy Exit", "Buy", 3337.46, 3320.0, kNaN, kNaN, kNaN, 100.0, "", kNaN, "", kNaN, kNaN);
    }
};

std::vector<Bar> vasudevshenoy_bars() {
    return {
        // 0: 2025-04-16 16:45 UTC (signal bar; the stop fills at its close)
        mk(1744821900000LL, 3322.59, 3327.38, 3322.34, 3327.38),
        // 1: 2025-04-16 17:00 UTC (the owner's entry bar)
        mk(1744822800000LL, 3327.355, 3328.865, 3325.475, 3326.87),
        // 2: 2025-04-16 17:15 UTC
        mk(1744823700000LL, 3326.855, 3330.53, 3326.675, 3330.105),
        // 3: 2025-04-16 17:30 UTC
        mk(1744824600000LL, 3330.085, 3332.905, 3324.385, 3330.26),
        // 4: 2025-04-16 17:45 UTC
        mk(1744825500000LL, 3329.86, 3329.86, 3326.1, 3328.255),
        // 5: 2025-04-16 18:00 UTC
        mk(1744826400000LL, 3328.215, 3333.125, 3326.42, 3332.255),
        // 6: 2025-04-16 18:15 UTC (exit bar, limit fills at 3337.46)
        mk(1744827300000LL, 3332.235, 3338.895, 3331.735, 3338.375),
    };
}

void test_vasudevshenoy_priced_exit() {
    VasudevshenoyHost host;
    const auto bars = vasudevshenoy_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& t = host.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_time == 1744821900000LL);
        CHECK(near(t.entry_price, 3327.38));
        CHECK(t.exit_time == 1744827300000LL);
        CHECK(near(t.exit_price, 3337.46));
        CHECK(near(t.qty, 1.0));
        CHECK(near(t.pnl, 10.08));
    }
}

// ---------------------------------------------------------------------------
// Probe 3: Yuri Garcia Narrow State Strategy (ETH 15m)
// Trade #38:
// Short entry fills at open 1791.96 on bar 1 (2025-04-27 14:15 UTC).
// Stop loss at 1805.78 fills on the same entry bar.
// Owner literals: entry 1791.96, exit 1805.78, pnl -154.426062.
// ---------------------------------------------------------------------------
class YgilsHost : public source::PineStrategyHost {
public:
    YgilsHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1000000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        cfg.default_qty_value = 2.0;
        cfg.pyramiding = 0;
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
            strategy_entry("SHORT", false);
        }
        strategy_exit("TP SL LONG", "LONG", 1820.0, 1780.0, kNaN, kNaN, kNaN, 100.0, "", kNaN, "", kNaN, kNaN);
        strategy_exit("TP SL SHORT", "SHORT", 1764.32, 1805.78, kNaN, kNaN, kNaN, 100.0, "", kNaN, "", kNaN, kNaN);
    }
};

std::vector<Bar> ygils_bars() {
    return {
        // 0: 2025-04-27 14:00 UTC (signal bar)
        mk(1745762400000LL, 1796.93, 1798.18, 1791.95, 1791.95, 28989.101),
        // 1: 2025-04-27 14:15 UTC (entry + exit bar)
        mk(1745763300000LL, 1791.96, 1807.96, 1791.96, 1798.36, 110984.772),
        // 2: 2025-04-27 14:30 UTC
        mk(1745764200000LL, 1798.35, 1801.12, 1796.0, 1798.66, 19875.038),
    };
}

void test_ygils_entry_bar_stop() {
    YgilsHost host;
    const auto bars = ygils_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& t = host.get_trade(0);
        CHECK(!t.is_long);
        CHECK(near(t.entry_price, 1791.96));
        CHECK(near(t.exit_price, 1805.78));
    }
}

}  // namespace

int main() {
    test_vasudevshenoy_priced_exit();
    test_traderhayz_entry_bar_stop();
    test_ygils_entry_bar_stop();
    std::printf("test_l10am_entry_bar_priced_exit: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
