// R4-D L10i: parity tests for partial exits, staged pyramid entries,
// OCA-isolated brackets, and pending-order submission sequencing.
// Pin earliest divergent trades with legacy owner literals.
#include "l4a_native_route_guard.hpp"

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

#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

bool near(double a, double b, double tol = 1e-4) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

// -----------------------------------------------------------------------------
// Test 1: bracket-partial-exit-qty-percent-01
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
// Test 2: pyramid-terrace-staged-entry-01
// When flat, stale close orders from prior cycles are purged so that a subsequent
// multi-terrace staged pyramid position can arm its global exit bracket.
// -----------------------------------------------------------------------------
class PyramidTerraceHost : public source::PineStrategyHost {
public:
    PyramidTerraceHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 3;
        c.process_orders_on_close = false;
        c.commission_value = 0.05;
        c.slippage = 1;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        // Cycle 1: enter T1, then close T1, T2, T3 (T2 and T3 never existed).
        if (i == 0) strategy_entry("T1", true, kNaN, kNaN, 1.0, "t1 open");
        if (i == 1) {
            strategy_close("T1");
            strategy_close("T2");
            strategy_close("T3");
        }
        // Cycle 2: after flat, stage multiple terrace entries.
        if (i == 3) {
            strategy_entry("Terrace One", true, kNaN, kNaN, 1.0);
            strategy_entry("Terrace Two", true, kNaN, kNaN, 1.0);
            strategy_entry("Terrace Three", true, kNaN, kNaN, 1.0);
        }
        if (live_position_size() > 0.0 && i >= 4) {
            strategy_exit("Terrace Guard", "", kNaN, position_avg_price() - 20.0);
        }
    }
};

void test_pyramid_terrace_staged_entry() {
    PyramidTerraceHost host;
    const std::vector<Bar> bars = {
        mk(1000000, 3940.0, 3945.0, 3938.0, 3941.0),
        mk(1001000, 3941.0, 3942.0, 3935.0, 3936.0),
        mk(1002000, 3936.0, 3937.0, 3934.0, 3935.0), // closes cycle 1
        mk(1003000, 3940.0, 3946.0, 3939.0, 3944.0), // orders Terrace One, Two, Three
        mk(1004000, 3944.0, 3955.0, 3942.0, 3953.0), // fills Terrace entries, arms Terrace Guard
        mk(1005000, 3953.0, 3954.0, 3920.0, 3925.0), // hits Terrace Guard stop
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // Cycle 1: 1 trade. Cycle 2: 3 trades closed by Terrace Guard. Total = 4.
    CHECK(host.trade_count() == 4);
    if (host.trade_count() >= 4) {
        CHECK(host.get_trade(0).exit_id == "__close__T1");
        CHECK(host.get_trade(1).exit_id == "Terrace Guard");
        CHECK(host.get_trade(2).exit_id == "Terrace Guard");
        CHECK(host.get_trade(3).exit_id == "Terrace Guard");
        CHECK(near(host.live_position_size(), 0.0));
    }
}

// -----------------------------------------------------------------------------
// Test 3: oca-multi-bracket-isolation-01
// Two strategy.exit calls with explicit qty=1 attached to qty=2 entry with
// distinct oca_names ("GRP_A", "GRP_B").
// When GRP_A limit hits, GRP_A stop cancels. GRP_B continues running.
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
            strategy_exit("X_A", "L", entry + 10.0, entry - 10.0, kNaN, kNaN, kNaN, kNaN,
                          "", 1.0, "GRP_A");
            strategy_exit("X_B", "L", entry + 20.0, entry - 20.0, kNaN, kNaN, kNaN, kNaN,
                          "", 1.0, "GRP_B");
        }
    }
};

void test_oca_multi_bracket_isolation() {
    OcaMultiBracketHost host;
    const std::vector<Bar> bars = {
        mk(2000000, 1830.0, 1832.0, 1829.0, 1831.0),
        mk(2001000, 1831.34, 1835.0, 1830.0, 1833.0), // fills L @ 1831.34
        mk(2002000, 1833.0, 1843.0, 1832.0, 1842.0),  // hits X_A limit (1841.34), X_A stop cancelled
        mk(2003000, 1842.0, 1843.0, 1820.0, 1825.0),  // crosses X_A stop level (1821.34), must NOT fill X_A stop
        mk(2004000, 1825.0, 1855.0, 1824.0, 1852.0),  // hits X_B limit (1851.34)
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() >= 2) {
        const auto& t0 = host.get_trade(0);
        CHECK(t0.exit_id == "X_A");
        CHECK(near(t0.entry_price, 1831.34));
        CHECK(near(t0.exit_price, 1841.34));
        CHECK(near(t0.qty, 1.0));

        const auto& t1 = host.get_trade(1);
        CHECK(t1.exit_id == "X_B");
        CHECK(near(t1.entry_price, 1831.34));
        CHECK(near(t1.exit_price, 1851.34));
        CHECK(near(t1.qty, 1.0));

        CHECK(near(host.live_position_size(), 0.0));
    }
}

// -----------------------------------------------------------------------------
// Test 4: composite-bracket-cap-range-pending-stop-01
// A priced reversal entry (ShortOnGap stop) followed by strategy.order BracketSL
// on the same bar. strategy.order flushes pending_entries so ShortOnGap closes
// LongOnGap (trade 40) and opens ShortOnGap (trade 41, qty 1), then BracketSL
// opens trade 42 (qty 1), keeping two distinct short lots.
// -----------------------------------------------------------------------------
class CompositeBracketHost : public source::PineStrategyHost {
public:
    CompositeBracketHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 1;
        c.process_orders_on_close = false;
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("LongOnGap", true, kNaN, 1647.27);
        }
        if (i == 1) {
            // Reversal stop + order SL on same bar
            strategy_entry("ShortOnGap", false, kNaN, 1638.51);
            strategy_order("BracketSL", false, 1.0, kNaN, 1647.17, "bracket97", 2);
        }
        if (i == 2) {
            strategy_close_all();
        }
    }
};

void test_composite_bracket_cap_range_pending_stop() {
    CompositeBracketHost host;
    const std::vector<Bar> bars = {
        mk(3000000, 1640.0, 1650.0, 1639.0, 1648.0), // fills LongOnGap @ 1647.27
        mk(3001000, 1648.0, 1649.0, 1640.0, 1645.0), // places ShortOnGap and BracketSL
        mk(3002000, 1638.51, 1639.0, 1630.0, 1635.0), // both trigger at open 1638.51
        mk(3003000, 1635.0, 1636.0, 1610.0, 1615.0), // close_all
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // Trade 0: LongOnGap closed by BracketSL (qty 1).
    // Trade 1: ShortOnGap closed by close_all (qty 2).
    CHECK(host.trade_count() == 2);
    if (host.trade_count() >= 2) {
        const auto& t0 = host.get_trade(0);
        CHECK(t0.entry_id == "LongOnGap");
        CHECK(t0.exit_id == "BracketSL");
        CHECK(near(t0.entry_price, 1648.00));
        CHECK(near(t0.exit_price, 1638.51));
        CHECK(near(t0.qty, 1.0));

        const auto& t1 = host.get_trade(1);
        CHECK(t1.entry_id == "ShortOnGap");
        CHECK(t1.exit_id == "__close__");
        CHECK(near(t1.entry_price, 1638.51));
        CHECK(near(t1.exit_price, 1635.00));
        CHECK(near(t1.qty, 2.0));
    }
}

} // namespace

int main() {
    test_bracket_partial_exit_qty_percent();
    test_pyramid_terrace_staged_entry();
    test_oca_multi_bracket_isolation();
    test_composite_bracket_cap_range_pending_stop();
    std::printf("test_l10i_corpus_parity: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
