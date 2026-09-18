// R4-D L10ar: a priced ENTRY (strategy.entry with limit= or stop=) fills on
// the same bar and at the same price as the legacy owner (ab9714be).
//
// Pinned rows (window-mode lane replays against the owner tree):
//   bnf6082-initial-balance-breakout-strategy on BINANCE:BTCUSDT
//   (process_orders_on_close, slippage 1, cash-per-contract 2.50, mintick 0.01):
//     #10 Entry long 2025-05-01 14:30 @96368.00 q=1, Exit long 15:00 @97091.57,
//         net 718.57 — the pure LIMIT entry placed by the 14:30 close is already
//         marketable against that close and fills there (pine_fills.cpp:7604-7646,
//         8022-8043), not at the 14:45 open.
//   ahtisham-ee-decoded-volatility-expansion-ahtisham on OANDA:EURUSD
//   (percent_of_equity 100, qty_step 0.01, mintick 1e-05):
//     #64 Entry long 2025-06-10 00:15 @1.14357 q=8773.54, Exit long 00:30
//         @1.14257 ('Fakeout'), net -8.77354 — the STOP entry placed at the 00:00
//         close is touched by the 00:15 high on the tick-quantized bar.
//
// Bars are embedded literals copied out of the lane feeds; this test must never
// open corpus files or absolute paths at runtime.
#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
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

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

// ---------------------------------------------------------------------------
// bnf6082: 2025-05-01 (UTC) rows of ohlcv_BINANCE-BTCUSDT_15m.csv.
// Indices 0-3 form the initial balance (09:30-10:30 ET), index 4 is the IB
// lock / breakout bar that places the limit entry and fills it at its close.
std::vector<Bar> bnf6082_btcusdt_bars() {
    return {
        mk(1746106200000LL, 96642.01, 96650.93, 95925.23, 96163.07),  // 0: 13:30
        mk(1746107100000LL, 96163.08, 96256.00, 95769.66, 95809.52),  // 1: 13:45
        mk(1746108000000LL, 95809.52, 96240.47, 95809.52, 96224.00),  // 2: 14:00
        mk(1746108900000LL, 96223.99, 96459.04, 96223.99, 96404.01),  // 3: 14:15
        mk(1746109800000LL, 96404.01, 96689.70, 96173.91, 96368.00),  // 4: 14:30 (entry fill)
        mk(1746110700000LL, 96368.00, 96908.28, 96328.86, 96827.99),  // 5: 14:45
        mk(1746111600000LL, 96828.00, 97421.00, 96751.30, 97384.98),  // 6: 15:00 (TP fill)
        mk(1746112500000LL, 97384.97, 97424.02, 96971.23, 97126.00),  // 7: 15:15
    };
}

class Bnf6082BtcUsdtHost : public source::PineStrategyHost {
public:
    Bnf6082BtcUsdtHost() {
        source::PineStrategyConfig c;
        c.process_orders_on_close = true;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
        c.commission_value = 2.5;
        c.slippage = 1;
        c.pyramiding = 0;
        configure_pine_strategy(c);
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        syminfo_.ticker = "BINANCE:BTCUSDT";
        syminfo_.tickerid = "BINANCE:BTCUSDT";
        syminfo_.type = "crypto";
        syminfo_.timezone = "UTC";
        syminfo_.session = "24x7";
        set_syminfo_metadata("qty_step", 1e-05);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i < 4) {
            ib_high_ = std::max(ib_high_, bar.high);
            ib_low_ = std::min(ib_low_, bar.low);
            return;
        }
        // The strategy's IB lock bar: the breakout above the IB high places
        // the retracement limit entry and its bracket on the same close.
        if (i == 4 && live_position_size() == 0.0 && bar.high > ib_high_) {
            const double range = ib_high_ - ib_low_;
            const double entry_price = ib_high_ - range * (25.0 / 100.0);
            const double stop_price = ib_high_ - range * (60.0 / 100.0);
            const double tp1_price = ib_high_ + range * 0.50;
            strategy_entry("Long", true, entry_price);
            strategy_exit("Long TP", "Long", tp1_price, stop_price, kNaN, kNaN, kNaN,
                          100.0, "TP");
        }
    }

private:
    double ib_high_ = -std::numeric_limits<double>::infinity();
    double ib_low_ = std::numeric_limits<double>::infinity();
};

void test_bnf6082_limit_entry_fills_at_placement_close() {
    Bnf6082BtcUsdtHost host;
    const auto bars = bnf6082_btcusdt_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("bnf6082: %s\n", host.last_error().c_str());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() < 1) {
        std::printf("bnf6082: got trade_count=%d (expected 1)\n", host.trade_count());
        return;
    }
    const Trade& t = host.get_trade(0);
    CHECK(t.is_long);
    CHECK(t.entry_id == "Long");
    CHECK(t.exit_id == "Long TP");
    CHECK(near(t.entry_price, 96368.00, 1e-6));
    CHECK(near(t.exit_price, 97091.57, 1e-6));
    CHECK(near(t.qty, 1.0, 1e-9));
    CHECK(near(t.pnl, 718.57, 1e-6));
    CHECK(t.entry_time == bars[4].timestamp);
    CHECK(t.exit_time == bars[6].timestamp);
    if (t.entry_time != bars[4].timestamp) {
        std::printf("bnf6082: entry_time=%lld (expected %lld) entry_price=%.6f\n",
                    static_cast<long long>(t.entry_time),
                    static_cast<long long>(bars[4].timestamp), t.entry_price);
    }
}

// ---------------------------------------------------------------------------
// ahtisham: 2025-06-09/10 (UTC) rows of ohlcv_OANDA-EURUSD_15m.csv. The 20 bars
// before index 21 form the consolidation zone read by the 00:15 exit.
std::vector<Bar> ahtisham_eurusd_bars() {
    return {
        mk(1749496500000LL, 1.14282, 1.14290, 1.14262, 1.14266),  //  0: 19:15
        mk(1749497400000LL, 1.14267, 1.14271, 1.14233, 1.14239),  //  1: 19:30
        mk(1749498300000LL, 1.14238, 1.14258, 1.14225, 1.14231),  //  2: 19:45
        mk(1749499200000LL, 1.14229, 1.14271, 1.14227, 1.14250),  //  3: 20:00
        mk(1749500100000LL, 1.14249, 1.14251, 1.14202, 1.14218),  //  4: 20:15
        mk(1749501000000LL, 1.14217, 1.14218, 1.14188, 1.14197),  //  5: 20:30
        mk(1749501900000LL, 1.14198, 1.14208, 1.14186, 1.14208),  //  6: 20:45
        mk(1749502800000LL, 1.14224, 1.14241, 1.14202, 1.14214),  //  7: 21:00
        mk(1749503700000LL, 1.14216, 1.14216, 1.14214, 1.14216),  //  8: 21:15
        mk(1749504600000LL, 1.14216, 1.14221, 1.14214, 1.14214),  //  9: 21:30
        mk(1749505500000LL, 1.14215, 1.14221, 1.14201, 1.14210),  // 10: 21:45
        mk(1749506400000LL, 1.14218, 1.14238, 1.14208, 1.14233),  // 11: 22:00
        mk(1749507300000LL, 1.14232, 1.14234, 1.14220, 1.14222),  // 12: 22:15
        mk(1749508200000LL, 1.14221, 1.14223, 1.14203, 1.14206),  // 13: 22:30
        mk(1749509100000LL, 1.14206, 1.14231, 1.14204, 1.14231),  // 14: 22:45
        mk(1749510000000LL, 1.14232, 1.14232, 1.14203, 1.14218),  // 15: 23:00
        mk(1749510900000LL, 1.14216, 1.14255, 1.14214, 1.14254),  // 16: 23:15
        mk(1749511800000LL, 1.14254, 1.14268, 1.14247, 1.14248),  // 17: 23:30
        mk(1749512700000LL, 1.14246, 1.14286, 1.14242, 1.14264),  // 18: 23:45
        mk(1749513600000LL, 1.14264, 1.14328, 1.14242, 1.14324),  // 19: 00:00 (stop placement)
        mk(1749514500000LL, 1.14323, 1.14357, 1.14308, 1.14326),  // 20: 00:15 (stop entry fill)
        mk(1749515400000LL, 1.14328, 1.14330, 1.14218, 1.14232),  // 21: 00:30 (Fakeout exit)
        mk(1749516300000LL, 1.14229, 1.14272, 1.14222, 1.14244),  // 22: 00:45
    };
}

class AhtishamEurUsdHost : public source::PineStrategyHost {
public:
    AhtishamEurUsdHost() {
        source::PineStrategyConfig c;
        // The owner's equity after its first 63 trades (10000 + 33.158239).
        c.initial_capital = 10033.158239;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 100.0;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        configure_pine_strategy(c);
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 1e-05;
        syminfo_mintick_ = 1e-05;
        syminfo_.ticker = "OANDA:EURUSD";
        syminfo_.tickerid = "OANDA:EURUSD";
        syminfo_.type = "forex";
        syminfo_.timezone = "America/New_York";
        syminfo_.session = "1700-1700";
        set_syminfo_metadata("qty_step", 0.01);
    }

    void on_source_bar(const Bar& bar) override {
        highs_.push_back(bar.high);
        lows_.push_back(bar.low);
        const int i = pine_bar_index();
        // buyStopLevel / sellStopLevel on the owner's 00:00 bar: the 20-bar
        // zone of high[1]/low[1] widened by 1.5 * ta.atr(14) (ATR carried from
        // the full lane history, not reproducible from the embedded rows).
        if (i == 19 && live_position_size() == 0.0) {
            strategy_entry("Long", true, kNaN, 1.143561767149223, kNaN, "EXPANSION UP");
            strategy_entry("Short", false, kNaN, 1.141268232850777, kNaN, "EXPANSION DOWN");
        }
        if (i >= 20 && live_position_size() > 0.0) {
            const double zone_high = *std::max_element(highs_.end() - 21, highs_.end() - 1);
            const double zone_low = *std::min_element(lows_.end() - 21, lows_.end() - 1);
            const double long_sl = (zone_high + zone_low) / 2.0;
            strategy_exit("L-Exit", "Long", 1.146505994201407, long_sl, kNaN, kNaN, kNaN,
                          100.0, "Fakeout");
        }
    }

private:
    std::vector<double> highs_;
    std::vector<double> lows_;
};

void test_ahtisham_stop_entry_fills_on_touch_bar() {
    AhtishamEurUsdHost host;
    const auto bars = ahtisham_eurusd_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() < 1) {
        std::printf("ahtisham: got trade_count=%d (expected 1)\n", host.trade_count());
        return;
    }
    const Trade& t = host.get_trade(0);
    CHECK(t.is_long);
    CHECK(t.entry_id == "Long");
    CHECK(t.exit_id == "L-Exit");
    CHECK(near(t.entry_price, 1.14357, 1e-9));
    CHECK(near(t.exit_price, 1.14257, 1e-9));
    CHECK(near(t.qty, 8773.54, 1e-6));
    CHECK(near(t.pnl, -8.77354, 1e-6));
    CHECK(t.entry_time == bars[20].timestamp);
    CHECK(t.exit_time == bars[21].timestamp);
}

}  // namespace

int main() {
    test_bnf6082_limit_entry_fills_at_placement_close();
    test_ahtisham_stop_entry_fills_on_touch_bar();
    std::printf("test_l10ar_priced_entry_fill: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
