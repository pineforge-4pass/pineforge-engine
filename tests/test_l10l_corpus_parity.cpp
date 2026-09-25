// R4-D L10l: Switched route parity for calc_on_order_fills bracket,
// intraday-risk cap gatekeeper, and OCA-reduce bracket.
// Pins the earliest divergent trades with legacy owner literals.
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
// Scenario 1: bracket-rivet-calc-on-fill-01 (Trade #29)
// process_orders_on_close=true, calc_on_order_fills=true
// Entry long on 2025-04-22 07:15 fills at close (1590.00 + 0.01 = 1590.01).
// On 2025-04-22 07:30 (O 1590, H 1623.50, L 1589.83, C 1611.92):
// Strategy places exit limit=1608.77, stop=1575.51. Under POOC, because
// limit 1608.77 is marketable against close 1611.92, it executes immediately
// at the close: 1611.92!
// Owner literals: entry=1590.01, exit=1611.92, qty=2, pnl=40.618070.
// ---------------------------------------------------------------------------
class RivetCoofHost : public source::PineStrategyHost {
public:
    RivetCoofHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 2.0;
        c.pyramiding = 0;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.05;
        c.slippage = 1;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        c.process_orders_on_close = true;
        c.calc_on_order_fills = true;
        configure_pine_strategy(c);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 1) {
            strategy_entry("Rivet Long", true, kNaN, kNaN, 2.0, "");
        } else if (i == 2) {
            if (signed_position_size() > 0.0) {
                strategy_exit("Rivet Attached Risk", "Rivet Long", 1608.77071972, 1575.51308022);
            }
        }
    }
};

void test_coof_pooc_marketable_bracket_exit() {
    std::printf("test_coof_pooc_marketable_bracket_exit\n");
    std::vector<Bar> bars = {
        mk(1745305200000LL, 1582.97, 1584.93, 1581.68, 1582.51, 46105.368),
        mk(1745306100000LL, 1582.50, 1590.56, 1581.96, 1590.00, 55379.892),
        mk(1745307000000LL, 1590.00, 1623.50, 1589.83, 1611.92, 436529.843),
        mk(1745307900000LL, 1611.93, 1620.00, 1611.49, 1618.60, 159069.481),
    };
    RivetCoofHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto t = host.get_trade(0);
        CHECK(t.is_long);
        CHECK(near(t.entry_price, 1590.01));
        CHECK(near(t.exit_price, 1611.92));
        CHECK(near(t.qty, 2.0));
        CHECK(near(t.pnl, 40.618070));
    }
}

// ---------------------------------------------------------------------------
// Scenario 2: cap-gatekeeper-intraday-risk-01 (Trade #147)
// process_orders_on_close=true, max_intraday_filled_orders(1)
// Entry long fills at 01:15 close (3930.68 + 0.01 slippage = 3930.69).
// On 06:00 (O 3929.85, H 3934.29, L 3926.36, C 3932.01):
// Intraday cap triggers CloseNow at high 3934.29, which pays 1-step slippage
// (3934.29 - 0.01 = 3934.28).
// Owner literals: entry=3930.69, exit=3934.28, qty=2, pnl=-0.684970.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Scenario 2: cap-gatekeeper-intraday-risk-01 (Trade #258)
// process_orders_on_close=true, max_intraday_filled_orders(1)
// Entry long fills at 2026-03-06 08:00 close (2078.78 + 0.01 slippage = 2078.79).
// Max intraday fills cap is reached, so cap triggers CloseNow at close 2078.78.
// The synthetic close pays 1-step slippage: 2078.78 - 0.01 = 2078.77!
// Owner literals: entry=2078.79, exit=2078.77, qty=2, pnl=-4.197560.
// ---------------------------------------------------------------------------
class GatekeeperCapHost : public source::PineStrategyHost {
public:
    GatekeeperCapHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 2.0;
        c.pyramiding = 0;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.05;
        c.slippage = 1;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        c.process_orders_on_close = true;
        c.calc_on_order_fills = false;
        configure_pine_strategy(c);
        set_pine_risk_max_intraday_filled_orders(1);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 1) {
            strategy_entry("Gatekeeper Long", true, kNaN, kNaN, 2.0, "");
        }
    }
};

void test_intraday_cap_close_now_slippage() {
    std::printf("test_intraday_cap_close_now_slippage\n");
    std::vector<Bar> bars = {
        mk(1772783100000LL, 2083.22, 2086.57, 2080.44, 2081.79), // 07:45
        mk(1772784000000LL, 2081.79, 2081.79, 2077.50, 2078.78), // 08:00
    };
    GatekeeperCapHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto t = host.get_trade(0);
        CHECK(t.is_long);
        CHECK(near(t.entry_price, 2078.79));
        CHECK(near(t.exit_price, 2078.77));
        CHECK(near(t.qty, 2.0));
        CHECK(near(t.pnl, -4.197560));
    }
}

// ---------------------------------------------------------------------------
// Scenario 3: bracket-tp-sl-oca-reduce-isolate-01 (Trade #117)
// process_orders_on_close=false, calc_on_order_fills=false
// Entry short at 23:15 open (1585.22).
// At close of 23:15, BracketTP (limit 1585.12) and BracketSL (stop 1585.32) placed.
// At 23:30 open (1583.34), BracketTP gap-fills at open 1583.34.
// Owner literals: entry=1585.22, exit=1583.34, pnl=1.880000, fav=3.370000, adv=-2.080000.
// ---------------------------------------------------------------------------
class OcaReduceGapHost : public source::PineStrategyHost {
public:
    OcaReduceGapHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 1;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.0;
        c.slippage = 0;
        c.process_orders_on_close = false;
        c.calc_on_order_fills = false;
        configure_pine_strategy(c);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("S", false, kNaN, kNaN, 1.0, "");
        } else if (i == 1) {
            strategy_order("BracketTP", true, 1.0, 1585.12, kNaN, "bracket97a", 2);
            strategy_order("BracketSL", true, 1.0, kNaN, 1585.32, "bracket97a", 2);
        }
    }
};

void test_oca_reduce_open_gap_excursion() {
    std::printf("test_oca_reduce_open_gap_excursion\n");
    std::vector<Bar> bars = {
        mk(1745190000000LL, 1581.30, 1586.00, 1581.30, 1585.23, 29667.191),
        mk(1745190900000LL, 1585.22, 1587.30, 1582.89, 1583.35, 26593.292),
        mk(1745191800000LL, 1583.34, 1584.84, 1581.85, 1583.79, 16393.876),
    };
    OcaReduceGapHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto t = host.get_trade(0);
        CHECK(!t.is_long);
        CHECK(near(t.entry_price, 1585.22));
        CHECK(near(t.exit_price, 1583.34));
        CHECK(near(t.qty, 1.0));
        CHECK(near(t.pnl, 1.880000));
        // expectation corrected (R5 lane H-THIN, E19: the kernel samples the
        // Pine host's lots; the host-owned model is gone): favorable 3.37 ->
        // 2.33, TradingView's own number (corpus tape
        // bracket-tp-sl-oca-reduce-isolate-01 trade #117).
        CHECK(near(t.max_runup, 2.330000));
        CHECK(near(t.max_drawdown, 2.080000));
    }
}

} // namespace

int main() {
    test_coof_pooc_marketable_bracket_exit();
    test_intraday_cap_close_now_slippage();
    test_oca_reduce_open_gap_excursion();

    std::printf("test_l10l_corpus_parity: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
