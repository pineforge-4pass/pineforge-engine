// R4-D L10ap: On the switched route, with calc_on_order_fills = true, the
// re-entry the script issues on the bar of a fill executes at the price and on
// the bar the legacy owner (ab9714be) uses, including three chained fills on
// one bar.
//
// Pins owner rows #2-#4 of the
// zz-pop-officialjackofalltrades-aureate-market-architecture-strategy-joat
// shape on data-BINANCE-BTCUSDT: pyramiding 0, percent_of_equity 10,
// commission 0.01%, calc_on_order_fills, strategy.entry("Long") whenever flat
// and strategy.exit("Long Risk", "Long", limit, stop) whenever long.
//
// 2025-04-02 15:45 (O 87178.63 H 87287.29 L 86660.00 C 86757.67, high
// first): the carried marketable LIMIT exits at the open 87178.63; the
// first-open recalculation's re-entry refills at that same open; the bracket
// born from that refill is held through O->H and gap-fills at H 87287.29; the
// next re-entry waits for the next extreme and fills at L 86660.00.
//
// Bars are embedded from the BINANCE:BTCUSDT 15m lane feed — this test must
// never open corpus files (CI has no corpus checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

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

constexpr std::int64_t k1530 = 1743607800000LL;
constexpr std::int64_t k1545 = 1743608700000LL;
constexpr std::int64_t k1645 = 1743612300000LL;

// 2025-04-02 15:15 .. 17:00 UTC (15m bars).
std::vector<Bar> btc_bars() {
    return {
        mk(1743606900000LL, 86200.00, 86622.10, 86190.00, 86556.16, 686.11729),   // 0: entry call
        mk(k1530, 86556.16, 87333.00, 86550.00, 87178.63, 1955.54958),            // 1
        mk(k1545, 87178.63, 87287.29, 86660.00, 86757.67, 2135.27209),            // 2: chained bar
        mk(1743609600000LL, 86757.67, 86941.84, 86589.58, 86886.34, 1113.49513),  // 3
        mk(1743610500000LL, 86886.34, 87029.13, 86785.54, 87012.56, 553.10133),   // 4
        mk(1743611400000LL, 87012.57, 87110.57, 86796.50, 87019.65, 361.61821),   // 5
        mk(k1645, 87019.65, 87221.99, 86915.91, 87065.18, 520.92729),             // 6
        mk(1743613200000LL, 87065.19, 87333.00, 86955.38, 87223.99, 925.53902),   // 7
    };
}

class ChainedCoofHost : public source::PineStrategyHost {
public:
    ChainedCoofHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 100000.0;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 10.0;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.01;
        c.pyramiding = 0;
        c.slippage = 0;
        c.process_orders_on_close = false;
        c.calc_on_order_fills = true;
        configure_pine_strategy(c);
        set_syminfo_metadata("BTCUSDT", 0.01);
        set_syminfo_metadata("qty_step", 1e-5);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        const double position = live_position_size();
        // The owner script's stale activeTarget (87144.23) and a far stop.
        if (i <= 2 && position == 0.0) strategy_entry("Long", true);
        if (position > 0.0) strategy_exit("Long Risk", "Long", 87144.23, 80000.0);
    }
};

void test_chained_fills_on_one_bar() {
    std::printf("test_chained_fills_on_one_bar\n");
    ChainedCoofHost host;
    const auto bars = btc_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 4);
    if (host.trade_count() != 4) return;
    for (int t = 0; t < 4; ++t) {
        const auto& trade = host.get_trade(t);
        CHECK(trade.entry_id == "Long");
        CHECK(trade.exit_id == "Long Risk");
        CHECK(trade.is_long);
    }

    // Seed trade: opens at the 15:30 open, its limit fills on the L->H leg,
    // and the re-entry waits for the next extreme (owner row #2 entry).
    const auto& seed = host.get_trade(0);
    CHECK(near(seed.entry_price, 86556.16));
    CHECK(seed.entry_time == k1530);
    CHECK(near(seed.exit_price, 87144.23));
    CHECK(seed.exit_time == k1530);

    // Owner row #2: entry at the 15:30 high, the marketable limit born there
    // holds to the next bar and exits at the 15:45 open 87178.63.
    const auto& row2 = host.get_trade(1);
    CHECK(near(row2.entry_price, 87333.00));
    CHECK(row2.entry_time == k1530);
    CHECK(near(row2.exit_price, 87178.63));
    CHECK(row2.exit_time == k1545);

    // Owner row #3: the re-entry refills at that exit's price on the same
    // bar, and its bracket exits at the high 87287.29 on the same bar.
    const auto& row3 = host.get_trade(2);
    CHECK(near(row3.entry_price, 87178.63));
    CHECK(row3.entry_time == k1545);
    CHECK(near(row3.exit_price, 87287.29));
    CHECK(row3.exit_time == k1545);

    // Owner row #4: the third entry on that bar fills at its low 86660.00;
    // its bracket, born on the terminal leg, rests until 16:45.
    const auto& row4 = host.get_trade(3);
    CHECK(near(row4.entry_price, 86660.00));
    CHECK(row4.entry_time == k1545);
    CHECK(near(row4.exit_price, 87144.23));
    CHECK(row4.exit_time == k1645);

    CHECK(near(host.live_position_size(), 0.0));
}

}  // namespace

int main() {
    test_chained_fills_on_one_bar();
    std::printf("test_l10ap_coof_chained_fill_price: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
