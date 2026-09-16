// R4-D L10g: OCA reduce bracket order sequencing and deferred flip across close_all.
// Pins the earliest divergent trades with legacy literals by replaying exact bars.
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
    if (x) { \
        ++passed; \
    } else { \
        ++failed; \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
    } \
} while (0)

bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) < eps;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg(double cap, int qtype, double qval, int pyr, bool pooc) {
    source::PineStrategyConfig c;
    c.initial_capital = cap;
    c.default_qty_type = qtype;
    c.default_qty_value = qval;
    c.pyramiding = pyr;
    c.process_orders_on_close = pooc;
    return c;
}

// ---------------------------------------------------------------------------
// Test 1: bracket-tp-sl-oca-reduce-isolate-01
// On bar 0 (19:45): short 1. At close, entry("L") called, then BracketTP and
// BracketSL (oca.reduce) called.
// On bar 1 (20:00): open 1910.59 > stop 1905.35.
// L executes first (closes short 1, opens long 1).
// BracketSL executes second (enters long 1 as trade #14).
// BracketSL fill reduces BracketTP to 0.
// On bar 2 (20:15): open 1933.11 fills both long exits at TP.
// ---------------------------------------------------------------------------
class OcaReduceHost : public source::PineStrategyHost {
public:
    OcaReduceHost() {
        configure_pine_strategy(cfg(1000000, static_cast<int>(QtyType::FIXED), 1.0, 1, false));
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            // Enter short initially
            strategy_entry("S", false, kNaN, kNaN, 1.0, "init short");
        } else if (i == 1) {
            // MA cross up: enter L, and place bracket for current position
            strategy_entry("L", true, kNaN, kNaN, 1.0, "ma cross up");
            const double pos_qty = std::abs(signed_position_size());
            const double entry_px = position_avg_price();
            const double tp_px = entry_px - 10 * 0.01; // short TP is lower
            const double sl_px = entry_px + 10 * 0.01; // short SL is higher
            strategy_order("BracketTP", true, pos_qty, tp_px, kNaN, "bracket97a", 2);
            strategy_order("BracketSL", true, pos_qty, kNaN, sl_px, "bracket97a", 2);
        } else if (i == 2) {
            // Now in long position (+2), place brackets for long
            const double pos_qty = std::abs(signed_position_size());
            const double entry_px = position_avg_price();
            const double tp_px = entry_px + 10 * 0.01;
            const double sl_px = entry_px - 10 * 0.01;
            strategy_order("BracketTP", false, pos_qty, tp_px, kNaN, "bracket97a", 2);
            strategy_order("BracketSL", false, pos_qty, kNaN, sl_px, "bracket97a", 2);
        }
    }
};

void test_oca_reduce_bracket_isolation() {
    std::printf("test_oca_reduce_bracket_isolation\n");
    std::vector<Bar> bars = {
        mk(1743622200000LL, 1899.69, 1905.90, 1896.53, 1905.26), // bar 0
        mk(1743623100000LL, 1905.25, 1915.28, 1904.98, 1910.56), // bar 1 (19:45)
        mk(1743624000000LL, 1910.59, 1946.03, 1881.00, 1933.11), // bar 2 (20:00)
        mk(1743624900000LL, 1933.11, 1957.97, 1872.48, 1897.82), // bar 3 (20:15)
    };
    OcaReduceHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // Trade 1: short entry from bar 0, closed at bar 2 open
    // Trade 2: long entry L at bar 2 open 1910.59
    // Trade 3: long entry BracketSL at bar 2 open 1910.59
    CHECK(host.trade_count() >= 3);
    if (host.trade_count() >= 3) {
        const auto t2 = host.get_trade(1);
        CHECK(t2.is_long);
        CHECK(near(t2.entry_price, 1910.59));
        CHECK(near(t2.exit_price, 1933.11));

        const auto t3 = host.get_trade(2);
        CHECK(t3.is_long);
        CHECK(near(t3.entry_price, 1910.59));
        CHECK(near(t3.exit_price, 1933.11));
    }
}

// ---------------------------------------------------------------------------
// Test 2: pyramid-deferred-flip-close-all-01 (Trade #157)
// When short 1, strategy.entry("S", stop=1793.76) and strategy.close_all()
// co-queued at bar 21:45.
// At bar 22:00 open (1802.14): close_all closes short position.
// Intrabar: price touches 1793.76, stop entry S fills at 1793.76.
// ---------------------------------------------------------------------------
class DeferredFlipHost : public source::PineStrategyHost {
public:
    DeferredFlipHost() {
        configure_pine_strategy(cfg(1000000, static_cast<int>(QtyType::FIXED), 1.0, 4, false));
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("S_init", false, kNaN, kNaN, 1.0, "init short");
        } else if (i == 1) {
            // Bar 21:45: queue short stop entry AND close_all
            strategy_entry("S", false, kNaN, 1793.76, 1.0, "flip short stop");
            strategy_close("", "session close_all");
        }
    }
};

void test_deferred_flip_stop_preserved_across_close_all() {
    std::printf("test_deferred_flip_stop_preserved_across_close_all\n");
    std::vector<Bar> bars = {
        mk(1745789400000LL, 1802.98, 1806.22, 1802.25, 1805.72), // 21:30
        mk(1745790300000LL, 1805.72, 1806.53, 1801.27, 1802.14), // 21:45
        mk(1745791200000LL, 1802.14, 1802.44, 1788.15, 1789.40), // 22:00
        mk(1745792100000LL, 1789.39, 1793.64, 1787.12, 1788.61), // 22:15
    };
    DeferredFlipHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // Trade 1: S_init closed by close_all at 1802.14
    CHECK(host.trade_count() >= 1);
    if (host.trade_count() >= 1) {
        const auto t1 = host.get_trade(0);
        CHECK(!t1.is_long);
        CHECK(near(t1.exit_price, 1802.14));
    }
    // Stop entry S must have filled at 1793.76 and position must be short 1
    CHECK(near(host.live_position_size(), -1.0));
}

// ---------------------------------------------------------------------------
// Test 3: Same-bar market entry + close_all (Trade #315)
// Position is short 1. On bar N, entry("L", market) AND close_all are called.
// At bar N+1 open: close_all closes short, L enters long 1 from flat.
// L remains open (not scratched).
// ---------------------------------------------------------------------------
class SameBarEntryCloseAllHost : public source::PineStrategyHost {
public:
    SameBarEntryCloseAllHost() {
        configure_pine_strategy(cfg(1000000, static_cast<int>(QtyType::FIXED), 1.0, 4, false));
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("S_prior", false, kNaN, kNaN, 1.0);
        } else if (i == 1) {
            strategy_entry("L", true, kNaN, kNaN, 1.0, "add long market");
            strategy_close("", "session close_all");
        }
    }
};

void test_same_bar_entry_and_close_all() {
    std::printf("test_same_bar_entry_and_close_all\n");
    std::vector<Bar> bars = {
        mk(1748467800000LL, 2630.00, 2635.00, 2628.00, 2634.14),
        mk(1748468700000LL, 2634.14, 2652.00, 2633.00, 2651.21), // 21:45
        mk(1748469600000LL, 2651.21, 2660.00, 2650.00, 2658.00), // 22:00
        mk(1748470500000LL, 2658.00, 2665.00, 2655.00, 2662.00),
    };
    SameBarEntryCloseAllHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // Trade 1: S_prior closed at 2651.21
    CHECK(host.trade_count() >= 1);
    if (host.trade_count() >= 1) {
        const auto t1 = host.get_trade(0);
        CHECK(!t1.is_long);
        CHECK(near(t1.exit_price, 2651.21));
    }
    // Long position L must be active and holding +1
    CHECK(near(host.live_position_size(), 1.0));
}

// ---------------------------------------------------------------------------
// Test 4: pyramid-cash-fractional-commission-01 (Trade #597)
// When position is flat, close_all is a no-op and does not flatten a same-bar
// new entry L.
// ---------------------------------------------------------------------------
class FlatCloseAllEntryHost : public source::PineStrategyHost {
public:
    FlatCloseAllEntryHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000;
        c.default_qty_type = static_cast<int>(QtyType::CASH);
        c.default_qty_value = 50000;
        c.pyramiding = 3;
        c.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
        c.commission_value = 0.05;
        configure_pine_strategy(c);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            // Position is flat. Co-queue entry and close_all
            strategy_entry("L", true, kNaN, kNaN, kNaN, "cash add");
            strategy_close("", "session close");
        }
    }
};

void test_flat_close_all_does_not_flatten_entry() {
    std::printf("test_flat_close_all_does_not_flatten_entry\n");
    std::vector<Bar> bars = {
        mk(1769125500000LL, 2954.80, 2956.10, 2950.00, 2951.21), // 23:45
        mk(1769126400000LL, 2951.21, 2960.00, 2948.00, 2955.00), // 00:00
        mk(1769127300000LL, 2955.00, 2958.00, 2952.00, 2956.00),
    };
    FlatCloseAllEntryHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // L should have entered and remain open (not closed at 00:00)
    CHECK(host.trade_count() == 0);
    CHECK(host.live_position_size() > 0.0);
    // 50000 / 2951.21 = 16.94220337
    CHECK(near(host.live_position_size(), 50000.0 / 2951.21, 1e-4));
}

} // namespace

int main() {
    test_oca_reduce_bracket_isolation();
    test_deferred_flip_stop_preserved_across_close_all();
    test_same_bar_entry_and_close_all();
    test_flat_close_all_does_not_flatten_entry();

    std::printf("test_l10g_oca_reduce_deferred_flip: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
