// R4-D L10ac: a default 100%-of-equity STOP entry that the legacy owner
// (ab9714be) DECLINES because the gap-through fill price cannot be funded, and
// that it then books as ONE lot on the next bar's open, must replay the same
// way on the switched route -- without the deferred pre-open margin slice of
// the declined candidate surviving to liquidate a dust lot on the later bar.
//
// Pinned shape: corpus/validation/zz-pop-ahtisham-ee-decoded-volatility-
// expansion-ahtisham on its lane feed data-OANDA-XAUUSD (mintick 0.001,
// pointvalue 1, no qty_step), owner rows #41-#43:
//   #41 Exit long  2025-05-29 18:15 @3318.115000 q=3.17205539   (flat before #42)
//   #42 Entry long 2025-06-01 22:15 @3307.240000 q=3.18228862
//       Exit  long 2025-06-02 01:45 @3302.770000  q=3.18228862  -> ONE lot
//   #43 Entry long 2025-06-02 06:15 @3332.624000 q=3.15397709
// 2025-06-01 22:15 UTC is the first bar of the Monday session (session
// 1800-1700 America/New_York): the Friday 20:45 close is 3289.7 and the Monday
// 22:00 open gaps up to 3303.415.  The all-in candidate sized off the carried
// close (equity/3289.7 = 3.19945 units) costs 3.19945 * 3303.415 > equity, so
// the owner declines the fill there (ab9714be src/source/pine_fills.cpp:4618
// stop_entry_margin_admission_declines) and never books its adverse-excursion
// slice either -- the excursion is only evaluated AFTER an opening is admitted.
// The re-issued candidate sizes off the 22:00 close (10525.244578/3307.445 =
// 3.18228862) and fills on the 22:15 open (cost 3.18228862 * 3307.24 =
// 10524.59 <= equity) as a single lot.
//
// The same strategy on the corpus ETH-USDT feed DOES get a same-bar residual
// (owner #126 Entry short 2025-06-30 07:00 @2463.00 q=0.11417702, closed again
// @2471.64 on the entry bar, plus main lot #127 q=3.9686425 stopped out
// 2025-06-30 07:45 @2480.96): that opening IS affordable at its fill price
// (equity 10055.984463, cost 4.08281951 * 2463 <= equity) and a short's
// adverse excursion (high) needs units * (2 * high - open) > equity, so the
// liquidation slice stays armed.  The rule is therefore an affordability test
// on the candidate's own fill price, not a symbol/tick-size/quantity-grid
// special case.
//
// Bars are embedded literals copied out of the two feeds
// (pineforge-lab evidence ohlcv_OANDA-XAUUSD_15m.csv rows 96609-96625 and
// corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv) -- this test must never
// open corpus files or absolute paths (CI has no corpus checkout).
#include "l4a_native_route_guard.hpp"

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
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

void dump(const char* tag, const Trade& t) {
    std::printf("%s id=%s/%s %s entry=%.6f@%lld exit=%.6f@%lld qty=%.8f "
                "pnl=%.6f mfe=%.6f mae=%.6f\n",
                tag, t.entry_id.c_str(), t.exit_id.c_str(),
                t.is_long ? "L" : "S", t.entry_price,
                static_cast<long long>(t.entry_time), t.exit_price,
                static_cast<long long>(t.exit_time), t.qty, t.pnl,
                t.max_runup, t.max_drawdown);
}

// The probe's order shape: while flat, a long buy-stop and a short sell-stop
// are re-issued on every bar (levels recomputed each bar); once in position,
// one same-id bracket exit is re-issued every bar.
class ExpansionHost : public source::PineStrategyHost {
public:
    ExpansionHost(double capital, const std::string& ticker, double mintick,
                  std::vector<double> long_stops, std::vector<double> short_stops,
                  double bracket_stop, double bracket_limit) {
        source::PineStrategyConfig c;
        c.initial_capital = capital;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 100.0;
        c.pyramiding = 1;
        c.process_orders_on_close = false;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.0;   // the probe declares no commission
        c.slippage = 0.0;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        configure_pine_strategy(c);
        // set_syminfo_metadata() is a generic key/value map that only honours
        // qty_step / account_currency_fx, so the lane's symbol facts (pointvalue
        // 1, mintick 0.001) are pinned on the engine members directly -- the
        // idiom used by tests/test_aapl15_margin_brackets_l4a.cpp.
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = mintick;
        syminfo_mintick_ = mintick;
        syminfo_.ticker = ticker;
        syminfo_.tickerid = ticker;
        long_stops_ = std::move(long_stops);
        short_stops_ = std::move(short_stops);
        bracket_stop_ = bracket_stop;
        bracket_limit_ = bracket_limit;
    }

    void set_qty_step(double step) { set_syminfo_metadata("qty_step", step); }

    void on_source_bar(const Bar&) override {
        // strategy.position_size is the freeze-aware script view, i.e. exactly
        // what codegen lowers the probe's gate to (generated.cpp:379) and it
        // already carries the current bar's fill.
        const int i = pine_bar_index();
        const double pos = signed_position_size();
        if (pos == 0.0 && i < static_cast<int>(long_stops_.size())) {
            strategy_entry("Long", true, kNaN, long_stops_[i], kNaN,
                           "EXPANSION UP");
            strategy_entry("Short", false, kNaN, short_stops_[i], kNaN,
                           "EXPANSION DOWN");
        } else if (pos > 0.0) {
            strategy_exit("L-Exit", "Long", bracket_limit_, bracket_stop_,
                          kNaN, kNaN, kNaN, 100.0, "Fakeout");
        } else if (pos < 0.0) {
            strategy_exit("S-Exit", "Short", bracket_limit_, bracket_stop_,
                          kNaN, kNaN, kNaN, 100.0, "Fakeout");
        }
    }

private:
    std::vector<double> long_stops_;
    std::vector<double> short_stops_;
    double bracket_stop_ = 0.0;
    double bracket_limit_ = 0.0;
};

// ---------------------------------------------------------------------------
// Unit 1 -- OANDA:XAUUSD lane rows, 2025-05-30 20:45 .. 2025-06-02 01:45 UTC.
// ---------------------------------------------------------------------------
std::vector<Bar> xauusd_bars() {
    return {
        mk(1748637900000LL, 3292.230, 3292.415, 3288.795, 3289.700),   // 0: Fri 20:45
        mk(1748815200000LL, 3303.415, 3309.770, 3303.080, 3307.445),   // 1: Mon 22:00
        mk(1748816100000LL, 3307.240, 3308.770, 3299.955, 3304.050),   // 2: Mon 22:15
        mk(1748817000000LL, 3303.990, 3305.945, 3302.495, 3305.710),   // 3: 22:30
        mk(1748817900000LL, 3305.700, 3315.135, 3305.665, 3313.110),   // 4: 22:45
        mk(1748818800000LL, 3313.080, 3316.745, 3312.570, 3313.815),   // 5: 23:00
        mk(1748819700000LL, 3313.820, 3314.145, 3311.330, 3312.140),   // 6: 23:15
        mk(1748820600000LL, 3312.160, 3313.245, 3310.255, 3311.355),   // 7: 23:30
        mk(1748821500000LL, 3311.350, 3312.485, 3309.425, 3310.220),   // 8: 23:45
        mk(1748822400000LL, 3310.215, 3314.805, 3309.780, 3313.165),   // 9: 00:00
        mk(1748823300000LL, 3313.325, 3315.065, 3311.390, 3312.950),   // 10: 00:15
        mk(1748824200000LL, 3312.935, 3313.625, 3303.315, 3307.465),   // 11: 00:30
        mk(1748825100000LL, 3307.460, 3311.670, 3307.320, 3311.455),   // 12: 00:45
        mk(1748826000000LL, 3311.490, 3313.930, 3310.570, 3312.400),   // 13: 01:00
        mk(1748826900000LL, 3312.385, 3313.530, 3308.205, 3309.495),   // 14: 01:15
        mk(1748827800000LL, 3309.465, 3311.030, 3303.920, 3306.480),   // 15: 01:30
        mk(1748828700000LL, 3306.490, 3307.200, 3301.415, 3303.660),   // 16: 01:45
    };
}

// Equity the lane carries into the Monday session (owner cumulative PnL -23.18
// after #42; the pinned 3.18228862 is 10525.244578 / 3307.445, the 22:00 close
// the fill bar sizes off).
constexpr double kXauEquity = 10525.244578;

// The breakout level stays below both Monday opens (a gap-through fill at the
// open) and above the Friday high; the opposite breakout sell-stop (a sell stop
// triggers BELOW the price) stays under every low of the window, so it rests
// unfillled and only the long breakout is admitted.  The bracket stop is constant,
// unlike the probe's recomputed zone mid; 3302.000 is first traded on the
// 2025-06-02 01:45 bar (lows 3302.495/3303.315/3303.920 ahead of it), whose
// low 3301.415 is the owner's #42 exit bar.
std::vector<double> xauusd_long_stops() { return std::vector<double>(17, 3300.000); }
std::vector<double> xauusd_short_stops() { return std::vector<double>(17, 3295.000); }

void test_declined_gap_entry_books_one_lot() {
    ExpansionHost host(kXauEquity, "OANDA:XAUUSD", 0.001,
                      xauusd_long_stops(), xauusd_short_stops(),
                      /*bracket_stop=*/3302.000, /*bracket_limit=*/3400.000);
    const auto bars = xauusd_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    std::printf("trades=%d\n", host.trade_count());
    for (int i = 0; i < host.trade_count(); ++i) dump("  xau", host.get_trade(i));

    // The owner books ONE lot; the pre-open artifact books a dust lot closed on
    // the entry bar plus a slightly smaller main lot.
    CHECK(host.trade_count() == 1);
    if (host.trade_count() != 1) return;

    const Trade& lot = host.get_trade(0);
    CHECK(lot.is_long);
    CHECK(lot.entry_id == "Long");
    // No deferred margin liquidation of a declined candidate may survive.
    CHECK(lot.exit_id != "__margin_preopen__Long");
    CHECK(lot.exit_id != "__margin_call__");
    CHECK(lot.exit_comment != "Margin call");
    // Owner #42: Entry long 2025-06-01 22:15 @3307.240000 q=3.18228862.
    CHECK(near(lot.entry_price, 3307.240, 1e-9));
    CHECK(lot.entry_time == bars[2].timestamp);
    CHECK(near(lot.qty, 3.18228862, 1e-8));
    // Owner #42's excursion columns are reproduced exactly by the pinned bars:
    // Favorable 30.247653 / Adverse -23.182973 USD.
    CHECK(near(lot.max_runup, 30.247653, 1e-6));
    CHECK(near(lot.max_drawdown, 23.182973, 1e-6));
    // The bracket stop is first traded on the owner's exit bar (the owner's own
    // recomputed mid stopped out at 3302.770000 on the same bar).
    CHECK(lot.exit_time == bars[16].timestamp);
    CHECK(near(lot.exit_price, 3302.000, 1e-9));
    CHECK(near(lot.pnl, (3302.000 - 3307.240) * lot.qty, 1e-6));
}

// ---------------------------------------------------------------------------
// Unit 2 -- the corpus ETH-USDT feed where the owner DOES split (#126/#127).
// ---------------------------------------------------------------------------
std::vector<Bar> eth_bars() {
    return {
        mk(1751265000000LL, 2497.290, 2499.000, 2480.470, 2482.750),  // 0: 06:30
        mk(1751265900000LL, 2482.760, 2483.880, 2453.940, 2463.000),  // 1: 06:45
        mk(1751266800000LL, 2463.000, 2471.640, 2459.480, 2468.700),  // 2: 07:00
        mk(1751267700000LL, 2468.710, 2477.010, 2467.000, 2475.320),  // 3: 07:15
        mk(1751268600000LL, 2475.320, 2476.320, 2471.170, 2474.010),  // 4: 07:30
        mk(1751269500000LL, 2474.000, 2482.740, 2474.000, 2479.630),  // 5: 07:45
    };
}

// Equity the probe carries into the bar (owner cumulative PnL 55.984463 after
// #125).  The sell stop is a downside breakout trigger: the 06:30 level 2450
// stays under the 06:45 low 2453.94 (it rests) and the 06:45 level 2463 is
// already through the 07:00 open, so the opening fills at 2463 exactly as the
// owner booked it; the buy-stop 2490 stays above every high.  The bracket is
// the owner's own mid, 2480.96, first traded on the 07:45 bar.
std::vector<double> eth_long_stops() { return std::vector<double>(6, 2490.000); }
std::vector<double> eth_short_stops() {
    return {2450.000, 2463.000, 2463.000, 2463.000, 2463.000, 2463.000};
}

void test_admitted_all_in_stop_still_splits() {
    ExpansionHost host(10055.984463, "ETHUSDT", 0.01,
                      eth_long_stops(), eth_short_stops(),
                      /*bracket_stop=*/2480.960, /*bracket_limit=*/2400.000);
    host.set_qty_step(0.00000001);
    const auto bars = eth_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    std::printf("trades=%d\n", host.trade_count());
    for (int i = 0; i < host.trade_count(); ++i) dump("  eth", host.get_trade(i));

    // Owner #126/#127: a main lot plus a residual closed again on the entry bar.
    CHECK(host.trade_count() == 2);
    if (host.trade_count() != 2) return;

    const Trade& residual = host.get_trade(0);
    const Trade& main_lot = host.get_trade(1);
    CHECK(!residual.is_long);
    CHECK(residual.entry_id == "Short");
    CHECK(near(residual.entry_price, 2463.000, 1e-9));
    CHECK(residual.entry_time == bars[2].timestamp);
    CHECK(near(residual.exit_price, 2471.640, 1e-6));   // the entry bar's high
    CHECK(residual.exit_time == bars[2].timestamp);     // closed on the entry bar
    // The residual is the margin slice, not the bracket: the owner's own exit
    // level is 2480.96 and never reaches 2471.64.
    CHECK(residual.exit_id != "S-Exit");
    CHECK(near(residual.qty, 0.11417702, 1e-6));
    CHECK(near(residual.max_runup, 0.401903, 1e-6));
    CHECK(near(residual.max_drawdown, 0.986489, 1e-6));

    CHECK(!main_lot.is_long);
    CHECK(main_lot.entry_id == "Short");
    CHECK(near(main_lot.entry_price, 2463.000, 1e-9));
    CHECK(near(main_lot.qty, 3.9686425, 1e-6));
    CHECK(main_lot.qty > residual.qty);
    CHECK(near(residual.qty + main_lot.qty, 4.08281951, 1e-6));
    CHECK(near(main_lot.pnl, (2480.960 - 2463.000) * -main_lot.qty, 1e-6));
    CHECK(main_lot.exit_id == "S-Exit");
    CHECK(near(main_lot.exit_price, 2480.960, 1e-6));
    CHECK(main_lot.exit_time == bars[5].timestamp);
}

}  // namespace

int main() {
    test_declined_gap_entry_books_one_lot();
    test_admitted_all_in_stop_still_splits();
    std::printf("test_l10ac_full_equity_single_lot: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
