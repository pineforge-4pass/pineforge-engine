// R4-D L10m: parity tests for partial exits and OCA-isolated brackets.
// Pin earliest divergent trades with legacy owner literals by replaying embedded bars.
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

#define CHECK(x) do {                                                           \
    if (x) {                                                                   \
        ++passed;                                                              \
    } else {                                                                   \
        ++failed;                                                              \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);               \
    }                                                                          \
} while (0)

bool near(double a, double b, double tol = 1e-4) {
    return std::abs(a - b) <= tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

// -----------------------------------------------------------------------------
// Scenario 1: bracket-partial-exit-qty-percent-01
// Partial exit (50%) + remaining exit (100%) sharing an entry.
// On bar 2026-02-17 02:00, both HALF_TP (limit 2005.54) and REST_SL (stop 1987.54)
// fill. Same-bar exit sibling trades order by script command_sequence (HALF_TP
// first as trade 641, REST_SL second as trade 642).
// -----------------------------------------------------------------------------
class BracketPartialExitHost : public source::PineStrategyHost {
public:
    BracketPartialExitHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 2.0;
        c.pyramiding = 1;
        c.process_orders_on_close = false;
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("L", true, kNaN, kNaN, 2.0, "two lots");
        }
        if (live_position_size() > 0.0) {
            const double entry = position_avg_price();
            strategy_exit("HALF_TP", "L", entry * 1.003, kNaN, kNaN, kNaN, kNaN, 50.0, "half tp");
            strategy_exit("REST_SL", "L", kNaN, entry * 0.994, kNaN, kNaN, kNaN, 100.0, "rest stop");
        }
    }
};

void test_bracket_partial_exit_qty_percent() {
    BracketPartialExitHost host;
    // Bars copied from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv (2026-02-17 01:15 to 02:00)
    const std::vector<Bar> bars = {
        mk(1771290900000LL, 2000.68, 2003.62, 1997.47, 1999.53),
        mk(1771291800000LL, 1999.54, 2007.91, 1998.12, 2001.59),
        mk(1771292700000LL, 2001.59, 2002.46, 1989.97, 1990.69),
        mk(1771293600000LL, 1990.7, 2008.64, 1977.31, 2002.9),
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() >= 2) {
        const auto& t0 = host.get_trade(0);
        CHECK(t0.exit_id == "HALF_TP");
        CHECK(near(t0.entry_price, 1999.54));
        CHECK(near(t0.exit_price, 2005.54));
        CHECK(near(t0.qty, 1.0));
        CHECK(near(t0.pnl, 6.0));
        CHECK(near(t0.max_runup, 8.37));
        CHECK(near(t0.max_drawdown, 22.23));

        const auto& t1 = host.get_trade(1);
        CHECK(t1.exit_id == "REST_SL");
        CHECK(near(t1.entry_price, 1999.54));
        CHECK(near(t1.exit_price, 1987.54));
        CHECK(near(t1.qty, 1.0));
        CHECK(near(t1.pnl, -12.0));
        CHECK(near(t1.max_runup, 8.37));
        CHECK(near(t1.max_drawdown, 12.0));
    }
}

// -----------------------------------------------------------------------------
// Scenario 2: oca-multi-bracket-isolation-01
// Two strategy.exit calls with explicit qty=1 attached to qty=2 entry with
// distinct oca_names ("GRP_A", "GRP_B").
// On bar 2025-05-02 12:30, GRP_A stop hits and cancels GRP_A limit, while
// GRP_B limit continues running and fills later in the same bar.
// -----------------------------------------------------------------------------
class OcaMultiBracketHost : public source::PineStrategyHost {
public:
    OcaMultiBracketHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 2.0;
        c.pyramiding = 1;
        c.process_orders_on_close = false;
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("L", true, kNaN, kNaN, 2.0, "entry");
        }
        if (live_position_size() > 0.0) {
            const double entry = position_avg_price();
            strategy_exit("X_A", "L", entry + 5.45, entry - 5.45, kNaN, kNaN, kNaN, kNaN,
                          "", 1.0, "GRP_A");
            strategy_exit("X_B", "L", entry + 10.89, entry - 10.89, kNaN, kNaN, kNaN, kNaN,
                          "", 1.0, "GRP_B");
        }
    }
};

void test_oca_multi_bracket_isolation() {
    OcaMultiBracketHost host;
    // Bars copied from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv (2025-05-02 11:30 to 12:30)
    const std::vector<Bar> bars = {
        mk(1746185400000LL, 1831.59, 1832.39, 1829.50, 1831.35),
        mk(1746186300000LL, 1831.34, 1833.34, 1829.52, 1832.39),
        mk(1746187200000LL, 1832.39, 1835.98, 1829.46, 1830.19),
        mk(1746188100000LL, 1830.18, 1832.70, 1828.00, 1832.41),
        mk(1746189000000LL, 1832.40, 1847.11, 1822.13, 1832.99),
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() >= 2) {
        const auto& t0 = host.get_trade(0);
        CHECK(t0.exit_id == "X_A");
        CHECK(near(t0.entry_price, 1831.34));
        CHECK(near(t0.exit_price, 1825.89));
        CHECK(near(t0.qty, 1.0));
        CHECK(near(t0.pnl, -5.45));
        CHECK(near(t0.max_runup, 4.64));
        CHECK(near(t0.max_drawdown, 5.45));

        const auto& t1 = host.get_trade(1);
        CHECK(t1.exit_id == "X_B");
        CHECK(near(t1.entry_price, 1831.34));
        CHECK(near(t1.exit_price, 1842.23));
        CHECK(near(t1.qty, 1.0));
        CHECK(near(t1.pnl, 10.89));
        CHECK(near(t1.max_runup, 10.89));
        CHECK(near(t1.max_drawdown, 9.21));

        CHECK(near(host.live_position_size(), 0.0));
    }
}

} // namespace

int main() {
    test_bracket_partial_exit_qty_percent();
    test_oca_multi_bracket_isolation();
    std::printf("test_l10m_corpus_parity: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
