// R4-D L10al: on the native execution route a reversal (a strategy.entry in the
// opposite direction while in position) books exactly ONE new lot of the size
// the owner computes for the new position -- never "the closed size" plus a
// separate remainder lot.
//
// ab9714be pine_strategy_host.cpp:244/:261 (reset_source_open_position_ledgers_before_book)
// clears the close ledgers (id_unclosed_qty_, close_reserved_qty_, etc.) when a
// position is opened or reversed, so unclosed units of the prior position side
// cannot survive to admit a later close of that side against the new position.
// Without this reset, a subsequent strategy.close of the old side on a later
// bar matched the un-cleared close_logical_units_ ledger, executed an unbound
// Reduce against the new position, and split the reversed position into two
// distinct lots with different exits.
//
// Pinned shapes:
// (1) data-BINANCE-BTCUSDT / sharpstrat-structure-break-chain-sbc-sharpstrat:
//     Initial capital 10000, default_qty_type = percent_of_equity,
//     default_qty_value = 10, process_orders_on_close = true.
//     #1 Exit long 2025-04-02 22:00 @83684.85 q=0.01177773
//     #2 Entry short 2025-04-02 22:00 @83684.85 q=0.01193241 -> ONE lot
// (2) Next-open execution (process_orders_on_close = false) direct reversal.

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

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

// ---------------------------------------------------------------------------
// Host for POOC reversal shape (sharpstrat reproduction)
// ---------------------------------------------------------------------------
class PoocReversalHost : public source::PineStrategyHost {
public:
    PoocReversalHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 10000.0;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 10.0;
        c.pyramiding = 1;
        c.process_orders_on_close = true;
        c.calc_on_order_fills = false;
        c.commission_value = 0.0;
        c.slippage = 0;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        configure_pine_strategy(c);
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("qty_step", 1e-5);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            // Enter Long at bar 0 close (84906.00)
            strategy_entry("Long", true);
        } else if (i == 1) {
            // Reversal at bar 1 close (83684.85)
            strategy_entry("Short", false);
            strategy_close("Long");
        } else if (i == 2) {
            // Repeated close("Long") while holding Short must be dropped
            strategy_close("Long");
        } else if (i == 3) {
            // Close the short position
            strategy_close("Short");
        }
    }
};

// ---------------------------------------------------------------------------
// Host for next-open reversal shape (process_orders_on_close = false)
// ---------------------------------------------------------------------------
class NextOpenReversalHost : public source::PineStrategyHost {
public:
    NextOpenReversalHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 10000.0;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 10.0;
        c.pyramiding = 1;
        c.process_orders_on_close = false;
        c.calc_on_order_fills = false;
        c.commission_value = 0.0;
        c.slippage = 0;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        configure_pine_strategy(c);
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("qty_step", 1e-5);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("Long", true);
        } else if (i == 1) {
            strategy_entry("Short", false);
            strategy_close("Long");
        } else if (i == 2) {
            strategy_close("Long");
        } else if (i == 3) {
            strategy_close("Short");
        }
    }
};

std::vector<Bar> sample_bars() {
    return {
        mk(1743519600000LL, 84195.92, 84910.00, 84182.89, 84906.00), // Bar 0
        mk(1743631200000LL, 84855.98, 84855.98, 83582.43, 83684.85), // Bar 1: reversal
        mk(1743682500000LL, 83292.00, 83477.92, 82400.00, 82566.26), // Bar 2: shortCond fires
        mk(1743750000000LL, 83200.00, 83600.00, 82700.00, 82818.52), // Bar 3: exit short
        mk(1743750900000LL, 82818.52, 82900.00, 82800.00, 82850.00), // Bar 4: flat
    };
}

void test_pooc_reversal_single_lot() {
    PoocReversalHost host;
    const auto bars = sample_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    // Exactly 2 closed trades: #1 Exit long, #2 Exit short.
    // The pre-fix bug created a 3rd trade because close("Long") on bar 2
    // partially closed the short position, creating two short lots.
    CHECK(host.trade_count() == 2);
    if (host.trade_count() < 2) return;

    // Trade 1: Long entry 84906.00, exit 83684.85, qty 0.01177
    const Trade& t1 = host.get_trade(0);
    CHECK(t1.is_long);
    CHECK(t1.entry_id == "Long");
    CHECK(near(t1.entry_price, 84906.00, 1e-2));
    CHECK(near(t1.exit_price, 83684.85, 1e-2));
    CHECK(near(t1.qty, 0.01177773, 1e-5));
    CHECK(near(t1.pnl, -14.382376, 1e-2));

    // Trade 2: Short entry 83684.85, exit 82818.52, qty 0.01193 (ONE full lot)
    // Sized off post-close equity: (10000 - 14.382376) * 10% / 83684.85 = 0.01193241
    const Trade& t2 = host.get_trade(1);
    CHECK(!t2.is_long);
    CHECK(t2.entry_id == "Short");
    CHECK(near(t2.entry_price, 83684.85, 1e-2));
    CHECK(near(t2.exit_price, 82818.52, 1e-2));
    CHECK(near(t2.qty, 0.01193241, 1e-5));
    CHECK(!t2.open_at_end);

    // Book must be flat at the end with 0 lots
    CHECK(host.physical_position().lot_count == 0);
    CHECK(host.physical_position().signed_units == 0.0);
}

void test_next_open_reversal_single_lot() {
    NextOpenReversalHost host;
    const auto bars = sample_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() < 2) return;

    const Trade& t1 = host.get_trade(0);
    CHECK(t1.is_long);
    CHECK(t1.entry_id == "Long");

    const Trade& t2 = host.get_trade(1);
    CHECK(!t2.is_long);
    CHECK(t2.entry_id == "Short");
    CHECK(near(t2.qty, 0.01193241, 1e-4));
    CHECK(!t2.open_at_end);

    CHECK(host.physical_position().lot_count == 0);
    CHECK(host.physical_position().signed_units == 0.0);
}

} // namespace

int main() {
    test_pooc_reversal_single_lot();
    test_next_open_reversal_single_lot();
    std::printf("test_l10al_reversal_single_lot: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
