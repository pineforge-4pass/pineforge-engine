// R4-D recon1: behaviours that individual lanes had right and the merged tip
// degraded, pinned on the switched route.
//
// 1. calc_on_order_fills chained fills on a bar whose extremes are on the tick
//    grid but whose n * mintick product sits one binary64 ULP off the decimal
//    print (BTCUSDT 2025-04-22 22:00: H 93354.93, L 92537.93).  The re-entry
//    born by the fill at H waits for the next waypoint and fills at L on the
//    same bar (owner rows #162-#164 of the
//    zz-pop-officialjackofalltrades-aureate-market-architecture-strategy-joat
//    shape on data-BINANCE-BTCUSDT).  ab9714be pine_scheduler.cpp:398-619,
//    pine_fills.cpp:7962-7966, engine.hpp:1207-1210.
// 2. A resting stop that fills on a later bar keeps the host sampler's
//    excursion: the closed row's adverse excursion is the entry commission
//    (XAUUSD 2026-03-08, owner row #1386 of the same shape on
//    data-OANDA-XAUUSD).  RULING A48; ab9714be engine_orders.cpp:319.
// 3. Every flat retires the in-position exits without walking the whole
//    placement history, so a long reversal run stays linear.  ab9714be
//    pine_fills.cpp:7461-7468, pine_orders.cpp:597-608.
//
// Bars are embedded from the BINANCE:BTCUSDT and OANDA:XAUUSD 15m lane feeds
// -- this test must never open corpus files (CI has no corpus checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <chrono>
#include <ctime>
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

// --- 1. chained fills on ULP-off extremes ----------------------------------

constexpr std::int64_t kBtc2145 = 1745358300000LL;
constexpr std::int64_t kBtc2200 = 1745359200000LL;
constexpr std::int64_t kBtc2215 = 1745360100000LL;

// 2025-04-22 21:30 .. 22:30 UTC (15m bars).
std::vector<Bar> btc_bars() {
    return {
        mk(1745357400000LL, 91415.40, 91650.01, 91120.00, 91640.00, 384.72979),   // 0: entry call
        mk(kBtc2145, 91642.21, 93888.00, 91642.21, 93039.99, 4973.619),           // 1
        mk(kBtc2200, 93039.99, 93354.93, 92537.93, 92864.86, 754.31149),          // 2: chained bar
        mk(kBtc2215, 92865.49, 93092.98, 92744.61, 92915.96, 252.03214),          // 3
        mk(1745361000000LL, 92916.24, 93060.00, 92634.97, 92739.13, 424.83487),   // 4
    };
}

source::PineStrategyConfig joat_config() {
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
    return c;
}

class UlpChainHost : public source::PineStrategyHost {
public:
    UlpChainHost() {
        configure_pine_strategy(joat_config());
        set_syminfo_metadata("qty_step", 1e-5);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        const double position = live_position_size();
        // A stale target below the market and a far stop, as the owner script.
        if (i <= 2 && position == 0.0) strategy_entry("Long", true);
        if (position > 0.0) strategy_exit("Long Risk", "Long", 92000.0, 80000.0);
    }
};

void test_chained_fill_on_ulp_off_extremes() {
    std::printf("test_chained_fill_on_ulp_off_extremes\n");
    UlpChainHost host;
    const auto bars = btc_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 4);
    if (host.trade_count() != 4) return;

    // Seed: opens at the 21:45 open, its limit fills on the L->H leg.
    const auto& seed = host.get_trade(0);
    CHECK(near(seed.entry_price, 91642.21));
    CHECK(near(seed.exit_price, 92000.0));
    CHECK(seed.exit_time == kBtc2145);

    // Owner row #162: re-entry at the 21:45 high, its marketable limit holds
    // to the next bar and exits at the 22:00 open.
    const auto& row162 = host.get_trade(1);
    CHECK(near(row162.entry_price, 93888.00));
    CHECK(row162.entry_time == kBtc2145);
    CHECK(near(row162.exit_price, 93039.99));
    CHECK(row162.exit_time == kBtc2200);

    // Owner row #163: refill at that open, bracket gap-fills at the high.
    const auto& row163 = host.get_trade(2);
    CHECK(near(row163.entry_price, 93039.99));
    CHECK(row163.entry_time == kBtc2200);
    CHECK(near(row163.exit_price, 93354.93));
    CHECK(row163.exit_time == kBtc2200);

    // Owner row #164: the re-entry born at the high fills at the low on the
    // same bar, not at the next bar's open; its bracket exits at 22:15.
    const auto& row164 = host.get_trade(3);
    CHECK(near(row164.entry_price, 92537.93));
    CHECK(row164.entry_time == kBtc2200);
    CHECK(near(row164.exit_price, 92865.49));
    CHECK(row164.exit_time == kBtc2215);
}

// --- 2. resting stop keeps the sampled excursion ---------------------------

constexpr std::int64_t kXau2215 = 1773008100000LL;
constexpr std::int64_t kXau2230 = 1773009000000LL;

// 2026-03-08 22:00 .. 22:45 UTC (15m bars).
std::vector<Bar> xau_bars() {
    return {
        mk(1773007200000LL, 5171.950, 5171.950, 5118.530, 5159.265, 13747),  // 0: entry call
        mk(kXau2215, 5159.265, 5160.335, 5127.025, 5156.065, 20274),         // 1: chained bar
        mk(kXau2230, 5156.090, 5164.550, 5106.455, 5125.920, 20417),         // 2: stop-out
        mk(1773009900000LL, 5125.935, 5134.605, 5104.345, 5130.310, 16983),  // 3
    };
}

class RestingStopHost : public source::PineStrategyHost {
public:
    RestingStopHost() {
        configure_pine_strategy(joat_config());
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.001;
        syminfo_mintick_ = 0.001;
        set_syminfo_metadata("qty_step", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        const double position = live_position_size();
        if (i <= 1 && position == 0.0) strategy_entry("Long", true);
        if (position > 0.0) strategy_exit("Long Risk", "Long", 6000.0, 5138.574);
    }
};

void test_resting_stop_keeps_sampled_excursion() {
    std::printf("test_resting_stop_keeps_sampled_excursion\n");
    RestingStopHost host;
    const auto bars = xau_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() != 2) return;

    // Owner row #1385: opens at the 22:15 open, stopped out on the H->L leg.
    const auto& first = host.get_trade(0);
    CHECK(near(first.entry_price, 5159.265));
    CHECK(near(first.exit_price, 5138.574));
    CHECK(first.exit_time == kXau2215);

    // Owner row #1386: the re-entry fills at the 22:15 low; its stop, created
    // on 22:15, rests and fills on 22:30.  The host sampler saw no adverse
    // move below the entry, so the row's drawdown is the entry commission
    // alone -- it is not capped to zero by a fill-based normalization.
    const auto& second = host.get_trade(1);
    CHECK(near(second.entry_price, 5127.025));
    CHECK(second.entry_time == kXau2215);
    CHECK(near(second.exit_price, 5138.574));
    CHECK(second.exit_time == kXau2230);
    const double entry_commission = second.entry_price * second.qty * 0.01 / 100.0;
    CHECK(entry_commission > 0.0);
    CHECK(near(second.max_drawdown, entry_commission, 1e-6));
}

// --- 3. flat retirement stays linear -----------------------------------------

std::vector<Bar> reversal_bars(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    std::int64_t t = 1743536700000LL;
    double px = 2000.0;
    for (int i = 0; i < count; ++i) {
        const double o = px;
        const double h = (i % 2 == 0) ? px + 10.0 : px + 2.0;
        const double l = (i % 2 == 0) ? px - 2.0 : px - 10.0;
        const double c = (i % 2 == 0) ? px + 8.0 : px - 8.0;
        bars.push_back(mk(t, o, h, l, c));
        t += 900000LL;  // 15m
        px = c;
    }
    return bars;
}

class ReversalRetireHost : public source::PineStrategyHost {
public:
    ReversalRetireHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 0;
        c.process_orders_on_close = false;
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        const double pos = live_position_size();
        // Every bar flattens with strategy.close and re-opens the other side,
        // with a far bracket re-issued on each live position.
        if (i % 2 == 0) {
            if (pos < 0.0) strategy_close("Short", "", kNaN, kNaN, false, 1ULL);
            strategy_entry("Long", true);
        } else {
            if (pos > 0.0) strategy_close("Long", "", kNaN, kNaN, false, 2ULL);
            strategy_entry("Short", false);
        }
        if (pos > 0.0) strategy_exit("Long Exit", "Long", 1.0e6, 1.0);
        if (pos < 0.0) strategy_exit("Short Exit", "Short", 1.0, 1.0e6);
    }
};

double run_reversals(int count, int* trades) {
    ReversalRetireHost host;
    const auto bars = reversal_bars(count);
    const std::clock_t started = std::clock();
    host.run(bars.data(), static_cast<int>(bars.size()));
    const double seconds =
        static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
    CHECK(host.last_error().empty());
    CHECK(near(std::abs(host.live_position_size()), 1.0));
    *trades = host.trade_count();
    return seconds;
}

void test_flat_retirement_is_linear() {
    std::printf("test_flat_retirement_is_linear\n");
    // Eight times the bars (and flats) must cost well under 64 times the
    // time: a per-flat walk over the placement history is quadratic.
    constexpr int kShort = 2000;
    constexpr int kLong = 8 * kShort;
    int short_trades = 0;
    int long_trades = 0;
    const double short_seconds = run_reversals(kShort, &short_trades);
    const double long_seconds = run_reversals(kLong, &long_trades);
    CHECK(short_trades >= kShort / 2);
    CHECK(long_trades >= kLong / 2);
    std::printf("  %d bars %.3fs, %d bars %.3fs\n",
                kShort, short_seconds, kLong, long_seconds);
    CHECK(long_seconds < 32.0 * short_seconds + 0.05);
}

}  // namespace

int main() {
    test_chained_fill_on_ulp_off_extremes();
    test_resting_stop_keeps_sampled_excursion();
    test_flat_retirement_is_linear();
    std::printf("test_l10recon1_merge_fill_liveness: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
