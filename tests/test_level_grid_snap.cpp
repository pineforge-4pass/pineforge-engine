/*
 * test_level_grid_snap.cpp — round 8 family T (NYSE:F@15), order-level
 * price-grid snap:
 *
 * TradingView stores a resting stop / limit LEVEL on the symbol's price grid
 * (multiples of 1 / pricescale) whenever it lies within 0.01 / pricescale^2 of
 * a grid price, and tests / fills it as that grid price; outside the band the
 * level is sub-tick and takes the directional snap (round 6). The engine used
 * to compare the RAW level against the tick-quantized bar, so a level carrying
 * the residue of avg_price +/- k * mintick (9.99 + 0.05 = 10.040000000000001)
 * did not fill on a bar whose quantized extreme EQUALS it (h 10.04, or h 10.035
 * -> 10.04) and the exit landed bars later at the same price — 148 of the 179
 * exit-time mismatches on the F@15 lane.
 *
 * Every case is a `lab tv` tape (ws-report-v1, rangeProof covered;
 * scratchpad famT/pins, 2026-09-05) replayed on the registry feed's own bars
 * (feed-f-15-chart 80f404ae, ES1! 15m, OANDA:EURUSD 15m); the pinned rule is
 * campaign note "PINNED (resting stop/limit level snaps to the price grid
 * within 0.01/pricescale^2, round 8 family T)".
 *
 *   famT-lim-<d>   NYSE:F long entered 04-25 14:30 ET open (9.995 -> 9.99);
 *                  sell limit 10.04 + d; the 14:45 bar is o = h = 10.04.
 *                  d <= 1e-6 (incl. 9.99 + 0.05): fills 14:45 @10.04.
 *                  d >= 1.2e-6: fills 15:00 (h 10.05) @10.05 (ceil).
 *   famT-stp-<d>   long entered 07-11 14:30 open 11.83; sell stop 11.81 - d;
 *                  15:00 l = 11.81. d <= 1e-6 (incl. 11.86 - 0.05): 15:00
 *                  @11.81. d >= 3e-6: skips 15:00 and 15:15 (l 11.805 ->
 *                  11.81), fills 15:30 (l 11.78) @11.80 (floor).
 *   famT-blim-<d>  short entered 10-06 10:45 open 12.71; buy limit 12.58 - d;
 *                  11:15 l = 12.58. 1e-6: 11:15 @12.58. 1e-5: 11:30 @12.57.
 *   famT-es-lim-<d> CME_MINI:ES1! (mintick 0.25, pricescale 100) long
 *                  entered 14:30Z open 5489.25; sell limit 5513.75 + d; 14:45
 *                  h = 5513.75. 5e-7: 14:45 @5513.75. 1e-6 / 1e-5 / 2.5e-5:
 *                  15:00 @5514.00 — the band is 1e-6 in price, not 1e-4 ticks.
 *   famT-eu-lim-<d> OANDA:EURUSD (pricescale 1e5) long entered 13:15Z open
 *                  1.13466; sell limit 1.13556 + d; 13:45 h = 1.13556.
 *                  1e-13 / 1e-12: 13:45 @1.13556. 2e-12 / 1e-11 / 1e-9:
 *                  14:00 @1.13557.
 *   famT-ps-*      syminfo.pricescale / round(1/mintick) qty-encoded: F 100 /
 *                  100, ES1! 100 / 4, EURUSD 100000 / 100000.

 *   famTb-236-repro / famTb-eth-{lim,stp,blim,bstp} (round 9, r9-famT-b,
 *                  BINANCE:ETHUSDT.P 15m, 2026-09-06): the corpus probe
 *                  analyzer-parity-stop-limit-timing-01 TV #236 level
 *                  3760.480000607087 fills @3760.48; the ETH band ladders —
 *                  see the ETH section below.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.12f, expected %.12f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = 0;
    return b;
}
static int64_t ts(int i) { return 1000 * (i + 1); }

namespace {

// NYSE:F 15m, 2025-04-25 ET (feed-f-15-chart 80f404ae): 14:15 14:30 14:45 15:00
const Bar kF0425_1415 = mk_bar(10.01, 10.01, 9.99, 10.0);
const Bar kF0425_1430 = mk_bar(9.995, 10.045, 9.995, 10.045);
const Bar kF0425_1445 = mk_bar(10.04, 10.04, 10.015, 10.025);
const Bar kF0425_1500 = mk_bar(10.021, 10.05, 10.021, 10.035);
// NYSE:F 15m, 2025-07-11 ET: 14:15 .. 15:30
const Bar kF0711_1415 = mk_bar(11.83, 11.83, 11.82, 11.83);
const Bar kF0711_1430 = mk_bar(11.83, 11.86, 11.825, 11.855);
const Bar kF0711_1445 = mk_bar(11.855, 11.86, 11.84, 11.855);
const Bar kF0711_1500 = mk_bar(11.855, 11.855, 11.81, 11.815);
const Bar kF0711_1515 = mk_bar(11.815, 11.82, 11.805, 11.805);
const Bar kF0711_1530 = mk_bar(11.805, 11.81, 11.78, 11.785);
// NYSE:F 15m, 2025-10-06 ET: 10:30 .. 11:30
const Bar kF1006_1030 = mk_bar(12.65, 12.71, 12.64, 12.7);
const Bar kF1006_1045 = mk_bar(12.71, 12.71, 12.64, 12.645);
const Bar kF1006_1100 = mk_bar(12.65, 12.67, 12.635, 12.635);
const Bar kF1006_1115 = mk_bar(12.635, 12.655, 12.58, 12.59);
const Bar kF1006_1130 = mk_bar(12.59, 12.6, 12.57, 12.58);
// NYSE:F 15m, 2025-04-24 ET (masayanfx-scalping TV #56): 09:45 .. 10:15
const Bar kF0424_0945 = mk_bar(9.955, 10.015, 9.94, 10.0);
const Bar kF0424_1000 = mk_bar(9.99, 10.06, 9.985, 10.0);
const Bar kF0424_1015 = mk_bar(9.995, 10.035, 9.97, 10.0);
// CME_MINI:ES1! 15m, 2025-04-25 UTC: 14:15 .. 15:00
const Bar kES_1415 = mk_bar(5519.25, 5522.0, 5482.25, 5489.0);
const Bar kES_1430 = mk_bar(5489.25, 5512.0, 5480.25, 5511.0);
const Bar kES_1445 = mk_bar(5510.75, 5513.75, 5498.25, 5508.25);
const Bar kES_1500 = mk_bar(5508.0, 5517.0, 5501.75, 5515.25);
// OANDA:EURUSD 15m, 2025-04-25 UTC: 13:00 .. 14:00
const Bar kEU_1300 = mk_bar(1.13524, 1.13538, 1.13452, 1.13467);
const Bar kEU_1315 = mk_bar(1.13466, 1.1351, 1.13338, 1.13405);
const Bar kEU_1330 = mk_bar(1.13404, 1.13418, 1.1333, 1.13406);
const Bar kEU_1345 = mk_bar(1.13407, 1.13556, 1.1339, 1.13536);
const Bar kEU_1400 = mk_bar(1.13536, 1.13717, 1.13526, 1.13647);

// BINANCE:ETHUSDT.P 15m (feed 27b62431096e), 2025-07-26 UTC: 00:00 .. 02:00
// (round 9 r9-famT-b: the corpus hard-lane probe's one-tick knock-on)
const Bar kETH0726_0000 = mk_bar(3724.18, 3725.69, 3710.21, 3713.05);
const Bar kETH0726_0015 = mk_bar(3713.05, 3727.64, 3712.0, 3726.67);
const Bar kETH0726_0030 = mk_bar(3726.68, 3727.12, 3719.29, 3719.55);
const Bar kETH0726_0045 = mk_bar(3719.56, 3725.0, 3712.82, 3724.75);
const Bar kETH0726_0100 = mk_bar(3724.76, 3725.28, 3715.62, 3721.05);
const Bar kETH0726_0115 = mk_bar(3721.06, 3721.49, 3713.02, 3718.58);
const Bar kETH0726_0130 = mk_bar(3718.58, 3721.23, 3708.11, 3708.48);
const Bar kETH0726_0145 = mk_bar(3708.48, 3714.39, 3694.56, 3701.34);
const Bar kETH0726_0200 = mk_bar(3701.34, 3714.8, 3701.33, 3714.79);
// BINANCE:ETHUSDT.P 15m, 2025-07-27 UTC: 00:00 .. 01:15 (corpus #236's bars)
const Bar kETH0727_0000 = mk_bar(3740.0, 3741.44, 3728.36, 3734.78);
const Bar kETH0727_0015 = mk_bar(3734.78, 3743.66, 3731.2, 3740.15);
const Bar kETH0727_0030 = mk_bar(3740.15, 3751.5, 3738.04, 3743.06);
const Bar kETH0727_0045 = mk_bar(3743.05, 3754.81, 3738.83, 3754.81);
const Bar kETH0727_0100 = mk_bar(3754.8, 3767.77, 3748.64, 3766.39);
const Bar kETH0727_0115 = mk_bar(3766.39, 3771.68, 3761.57, 3764.28);

std::vector<Bar> series(std::initializer_list<Bar> bars) {
    std::vector<Bar> out;
    int i = 0;
    for (const Bar& b : bars) {
        Bar c = b;
        c.timestamp = ts(i++);
        out.push_back(c);
    }
    return out;
}

// The tapes' strategy(): fixed qty, no commission / slippage, one position.
// Script chars (indexed by bar_index_): 'L' / 'S' market entry, '.' nothing;
// while in a position strategy.exit("X", "E", limit = exit_limit_, stop =
// exit_stop_) is re-issued every bar.
class Probe : public BacktestEngine {
public:
    explicit Probe(double mintick, double qty = 100.0) {
        initial_capital_ = 1000000000.0;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = mintick;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = qty;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        margin_call_enabled_ = false;
    }
    std::string script;
    double exit_stop_ = kNaN;
    double exit_limit_ = kNaN;
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ >= 0 && bar_index_ < (int)script.size()) {
            switch (script[bar_index_]) {
                case 'L': strategy_entry("E", true); break;
                case 'S': strategy_entry("E", false); break;
                default: break;
            }
        }
        if (position_side_ != PositionSide::FLAT
            && (std::isfinite(exit_stop_) || std::isfinite(exit_limit_))) {
            strategy_exit("X", "E", exit_limit_, exit_stop_);
        }
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::syminfo_mintick_;
    double grid(double p) const { return level_on_price_grid(p); }
    int decimals() const { return price_grid_decimals(); }
};

void expect_single_trade(const Probe& eng, bool is_long,
                         int entry_bar, double entry_px,
                         int exit_bar, double exit_px, double qty) {
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() != 1) return;
    const Trade& t = eng.get_trade(0);
    CHECK(t.is_long == is_long);
    CHECK(t.entry_time == ts(entry_bar));
    CHECK_NEAR(t.entry_price, entry_px, 1e-9);
    CHECK(t.exit_time == ts(exit_bar));
    CHECK_NEAR(t.exit_price, exit_px, 1e-9);
    CHECK_NEAR(t.qty, qty, 1e-9);
    if (t.exit_time != ts(exit_bar) || std::fabs(t.exit_price - exit_px) > 1e-9) {
        std::printf("        got exit bar %lld @%.8f (expected bar %d @%.8f)\n",
                    (long long)(t.exit_time / 1000 - 1), t.exit_price,
                    exit_bar, exit_px);
    }
}

void expect_open_position(const Probe& eng, bool is_long) {
    CHECK(eng.trade_count() == 0);
    CHECK(eng.position_side_ == (is_long ? PositionSide::LONG : PositionSide::SHORT));
    if (eng.trade_count() != 0) {
        const Trade& t = eng.get_trade(0);
        std::printf("        got an exit at bar %lld @%.8f (expected none)\n",
                    (long long)(t.exit_time / 1000 - 1), t.exit_price);
    }
}

// --- the snap itself ---------------------------------------------------------
void test_level_on_price_grid() {
    std::printf("-- level_on_price_grid: 0.01/pricescale^2 band, exact-decimal grid value --\n");
    Probe f(0.01);
    CHECK(f.decimals() == 2);
    CHECK(f.grid(9.99 + 0.05) == 10.04);            // the natural residue
    CHECK(f.grid(11.86 - 0.05) == 11.81);
    CHECK(f.grid(10.04 + 1e-6) == 10.04);           // famT-lim-1e-6 IN
    CHECK(f.grid(10.04 + 9e-7) == 10.04);
    CHECK(f.grid(10.04 + 1.2e-6) == 10.04 + 1.2e-6); // famT-lim-1.2e-6 OUT
    CHECK(f.grid(11.81 - 1e-6) == 11.81);           // famT-stp-1e-6 IN
    CHECK(f.grid(11.81 - 3e-6) == 11.81 - 3e-6);    // famT-stp-3e-6 OUT
    CHECK(f.grid(12.58 - 1e-6) == 12.58);           // famT-blim-1e-6 IN
    CHECK(f.grid(12.58 - 1e-5) == 12.58 - 1e-5);    // famT-blim-1e-5 OUT
    CHECK(f.grid(14.035) == 14.035);                // a genuine sub-tick level is untouched
    CHECK(f.grid(10.04) == 10.04);                  // on-grid is a fixed point
    CHECK(f.grid(1404.0 * 0.01) == 14.04);          // k*mintick residue -> the literal double
    CHECK(std::isnan(f.grid(kNaN)));
    Probe es(0.25);
    CHECK(es.decimals() == 2);                      // pricescale 100, not 1/0.25
    CHECK(es.grid(5513.75 + 5e-7) == 5513.75);      // famT-es-lim-5e-7 IN
    CHECK(es.grid(5513.75 + 1e-6) == 5513.75 + 1e-6);   // famT-es-lim-1e-6 OUT
    CHECK(es.grid(5513.75 + 2.5e-5) == 5513.75 + 2.5e-5); // 1e-4 ticks is NOT the band
    CHECK(es.grid(5513.76) == 5513.76);             // on the price grid, off the tick grid: kept
    Probe eu(0.00001);
    CHECK(eu.decimals() == 5);
    CHECK(eu.grid(1.13556 + 1e-13) == 1.13556);     // famT-eu-lim-1e-13 IN
    CHECK(eu.grid(1.13556 + 1e-12) == 1.13556);     // famT-eu-lim-1e-12 IN (FP boundary)
    CHECK(eu.grid(1.13556 + 2e-12) == 1.13556 + 2e-12); // famT-eu-lim-2e-12 OUT
    CHECK(eu.grid(1.13556 + 1e-11) == 1.13556 + 1e-11); // OUT
    CHECK(eu.grid(1.13556 + 1e-9) == 1.13556 + 1e-9);   // OUT
    Probe none(0.0);
    CHECK(none.decimals() == -1);
    CHECK(none.grid(10.04 + 1e-6) == 10.04 + 1e-6); // no tick, no grid
    Probe bin(0.0078125);                            // 1/128 = 0.0078125: 7 decimals,
    CHECK(bin.decimals() == 7);                     // pricescale 1e7 -> band 1e-16:
    CHECK(bin.grid(13.7451) == 13.7451);            // effectively no snap
    CHECK(bin.grid(13.7451 + 1e-9) == 13.7451 + 1e-9);
}

// --- NYSE:F sell limit on the o = h = 10.04 bar (famT-lim-*) ----------------
void test_f_sell_limit_band() {
    std::printf("-- NYSE:F sell limit 10.04 + d on o=h=10.04: d <= 1e-6 fills there @10.04, 1.2e-6 fills next @10.05 --\n");
    // bars: 0 14:15 (signal) 1 14:30 (fill 9.995 -> 9.99) 2 14:45 (o=h=10.04) 3 15:00 (h 10.05)
    for (double d : {0.0, 1e-12, 1e-9, 5e-7, 9e-7, 1e-6}) {
        Probe eng(0.01);
        eng.script = "L...";
        eng.exit_limit_ = 10.04 + d;
        auto bars = series({kF0425_1415, kF0425_1430, kF0425_1445, kF0425_1500});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 9.99, 2, 10.04, 100.0);
    }
    {   // the natural residue of masayanfx-scalping's longLimit
        Probe eng(0.01);
        eng.script = "L...";
        eng.exit_limit_ = 9.99 + 0.05;
        auto bars = series({kF0425_1415, kF0425_1430, kF0425_1445, kF0425_1500});
        eng.run(bars.data(), (int)bars.size());
        // pre-fix: exit bar 3 @10.04 (TV #70 04-25: TV 14:45, engine 15:00)
        expect_single_trade(eng, true, 1, 9.99, 2, 10.04, 100.0);
    }
    for (double d : {1.2e-6, 2e-6, 1e-5, 1e-4}) {
        Probe eng(0.01);
        eng.script = "L...";
        eng.exit_limit_ = 10.04 + d;
        auto bars = series({kF0425_1415, kF0425_1430, kF0425_1445, kF0425_1500});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 9.99, 3, 10.05, 100.0);
    }
}

// --- NYSE:F sell stop on the l = 11.81 bar (famT-stp-*) ----------------------
void test_f_sell_stop_band() {
    std::printf("-- NYSE:F sell stop 11.81 - d: d <= 1e-6 fills 15:00 @11.81, 3e-6 skips 15:00/15:15, fills 15:30 @11.80 --\n");
    // bars: 0 14:15 1 14:30 (fill 11.83) 2 14:45 3 15:00 (l 11.81) 4 15:15 (l 11.805) 5 15:30 (l 11.78)
    for (double stop : {11.81, 11.86 - 0.05, 11.81 - 1e-9, 11.81 - 1e-6}) {
        Probe eng(0.01);
        eng.script = "L.....";
        eng.exit_stop_ = stop;
        auto bars = series({kF0711_1415, kF0711_1430, kF0711_1445, kF0711_1500, kF0711_1515, kF0711_1530});
        eng.run(bars.data(), (int)bars.size());
        // pre-fix: 11.86 - 0.05 = 11.809999999999999 exited bar 5 @11.81
        // (drakkhon TV #27: TV 15:00, engine 15:30).
        expect_single_trade(eng, true, 1, 11.83, 3, 11.81, 100.0);
    }
    for (double stop : {11.81 - 3e-6, 11.81 - 1e-5, 11.81 - 1e-4}) {
        Probe eng(0.01);
        eng.script = "L.....";
        eng.exit_stop_ = stop;
        auto bars = series({kF0711_1415, kF0711_1430, kF0711_1445, kF0711_1500, kF0711_1515, kF0711_1530});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 11.83, 5, 11.80, 100.0);
    }
}

// --- NYSE:F buy limit (short) on the l = 12.58 bar (famT-blim-*) -------------
void test_f_buy_limit_band() {
    std::printf("-- NYSE:F buy limit 12.58 - d: 1e-6 fills 11:15 @12.58, 1e-5 fills 11:30 @12.57 --\n");
    // bars: 0 10:30 1 10:45 (fill 12.71) 2 11:00 3 11:15 (l 12.58) 4 11:30 (l 12.57)
    for (double d : {0.0, 1e-6}) {
        Probe eng(0.01);
        eng.script = "S....";
        eng.exit_limit_ = 12.58 - d;
        auto bars = series({kF1006_1030, kF1006_1045, kF1006_1100, kF1006_1115, kF1006_1130});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 12.71, 3, 12.58, 100.0);
    }
    {
        Probe eng(0.01);
        eng.script = "S....";
        eng.exit_limit_ = 12.58 - 1e-5;
        auto bars = series({kF1006_1030, kF1006_1045, kF1006_1100, kF1006_1115, kF1006_1130});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 12.71, 4, 12.57, 100.0);
    }
}

// --- the probe defect itself: masayanfx-scalping TV #56 ----------------------
void test_masayanfx_56_limit_on_quantized_high() {
    std::printf("-- masayanfx TV #56: limit 9.99+0.05 fills on the 10:15 bar (h 10.035 -> 10.04) --\n");
    // bars: 0 09:45 (signal) 1 10:00 (fill 9.99) 2 10:15 (h 10.035)
    Probe eng(0.01);
    eng.script = "L..";
    eng.exit_limit_ = 9.99 + 0.05;
    eng.exit_stop_ = 9.99 - 0.04;
    auto bars = series({kF0424_0945, kF0424_1000, kF0424_1015});
    eng.run(bars.data(), (int)bars.size());
    // pre-fix: no fill on bar 2 (10.04 vs 10.040000000000001); TV exits
    // 04-24 10:15 @10.04, the engine 11:45.
    expect_single_trade(eng, true, 1, 9.99, 2, 10.04, 100.0);
}

// --- CME_MINI:ES1!: the band is 1e-6 in price, not 1e-4 ticks ---------------
void test_es_band_is_price_not_ticks() {
    std::printf("-- ES1! (tick 0.25) sell limit 5513.75 + d: 5e-7 fills 14:45 @5513.75; 1e-6 / 2.5e-5 fill 15:00 @5514 --\n");
    // bars: 0 14:15 1 14:30 (fill 5489.25) 2 14:45 (h 5513.75) 3 15:00 (h 5517)
    for (double d : {0.0, 5e-7}) {
        Probe eng(0.25, 1.0);
        eng.script = "L...";
        eng.exit_limit_ = 5513.75 + d;
        auto bars = series({kES_1415, kES_1430, kES_1445, kES_1500});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 5489.25, 2, 5513.75, 1.0);
    }
    for (double d : {1e-6, 1e-5, 2.5e-5, 5e-5}) {
        Probe eng(0.25, 1.0);
        eng.script = "L...";
        eng.exit_limit_ = 5513.75 + d;
        auto bars = series({kES_1415, kES_1430, kES_1445, kES_1500});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 5489.25, 3, 5514.0, 1.0);
    }
}

// --- OANDA:EURUSD: pricescale 1e5 -> band 1e-12 ------------------------------
void test_eurusd_band() {
    std::printf("-- EURUSD sell limit 1.13556 + d: 1e-13 / 1e-12 fill 13:45 @1.13556; 2e-12 / 1e-11 / 1e-9 fill 14:00 @1.13557 --\n");
    // bars: 0 13:00 1 13:15 (fill 1.13466) 2 13:30 3 13:45 (h 1.13556) 4 14:00 (h 1.13717)
    for (double d : {0.0, 1e-13, 1e-12}) {
        Probe eng(0.00001);
        eng.script = "L....";
        eng.exit_limit_ = 1.13556 + d;
        auto bars = series({kEU_1300, kEU_1315, kEU_1330, kEU_1345, kEU_1400});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 1.13466, 3, 1.13556, 100.0);
    }
    for (double d : {2e-12, 1e-11, 1e-9, 1e-6}) {
        Probe eng(0.00001);
        eng.script = "L....";
        eng.exit_limit_ = 1.13556 + d;
        auto bars = series({kEU_1300, kEU_1315, kEU_1330, kEU_1345, kEU_1400});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 1.13466, 4, 1.13557, 100.0);
    }
}

// --- BINANCE:ETHUSDT.P (round 9, r9-famT-b): the hard-lane corpus tick ------
//
// corpus/validation/analyzer-parity-stop-limit-timing-01, TV #236: long at
// the 2025-07-27 00:15Z open (3734.78), strategy.exit limit = close +
// 2 * ta.atr(14) = 3734.78 + 2 * 12.850000303543267 = 3760.480000607087 —
// 6.07e-7 above the grid, INSIDE the band. TV stores 3760.48 and fills the
// 01:00Z bar (h 3767.77) at exactly 3760.48 (lab tv famTb-236-repro: the
// qty-encoded exit price reads 3760.48, the level 3760.48000061); the
// pre-famT engine ceil-snapped the raw level and filled 3760.49. Pinned by
// four ETH sensor ladders (famTb-eth-lim / -stp / -blim / -bstp, ws-report-v1
// rangeProof covered, 2026-09-06): on a pricescale-100 symbol at ~3700 a
// level exactly 1e-6 off the grid is OUT (3767.77 + 1e-6 does not fill on h
// 3767.77; 3694.56 - 1e-6 does not fill on l 3694.56) — the same double
// arithmetic that puts F 10.04 + 1e-6 IN and ES 5513.75 + 1e-6 OUT — and an
// out-of-band level crossed intrabar fills at the directional tick (sell
// limit / buy stop ceil, sell stop / buy limit floor), never at the raw level.
void test_eth_corpus_236_atr_level_in_band() {
    std::printf("-- ETH corpus #236: limit 3734.78 + 2*ATR = 3760.480000607087 fills 01:00Z @3760.48 (not 3760.49) --\n");
    // bars: 0 00:00 (signal, c 3734.78) 1 00:15 (fill 3734.78) 2 00:30 3 00:45 4 01:00 (h 3767.77) 5 01:15
    const double atr14 = 12.850000303543267;   // TV ta.atr(14) at the 00:00Z bar
    for (double level : {3734.78 + atr14 * 2.0, 3760.480000607087, 3760.48 + 0.000000607}) {
        Probe eng(0.01, 1.0);
        eng.script = "L.....";
        eng.exit_limit_ = level;
        eng.exit_stop_ = 3734.78 - atr14;      // 3721.93: never reached
        auto bars = series({kETH0727_0000, kETH0727_0015, kETH0727_0030, kETH0727_0045, kETH0727_0100, kETH0727_0115});
        eng.run(bars.data(), (int)bars.size());
        // pre-famT: bar 4 @3760.49 (baseline b6696da's row; TV's is 3760.48)
        expect_single_trade(eng, true, 1, 3734.78, 4, 3760.48, 1.0);
    }
    {   // 1.2e-6 is outside the band: the bar crosses it, ceil -> 3760.49
        Probe eng(0.01, 1.0);
        eng.script = "L.....";
        eng.exit_limit_ = 3760.48 + 0.0000012;
        auto bars = series({kETH0727_0000, kETH0727_0015, kETH0727_0030, kETH0727_0045, kETH0727_0100, kETH0727_0115});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 3734.78, 4, 3760.49, 1.0);
    }
}

void test_eth_sell_limit_band() {
    std::printf("-- ETH sell limit 3767.77 + d on h 3767.77: d <= 5e-7 fills 01:00 @3767.77; d >= 1e-6 fills 01:15 @3767.78 (ceil) --\n");
    for (double d : {0.0, 0.0000005}) {
        Probe eng(0.01, 1.0);
        eng.script = "L.....";
        eng.exit_limit_ = 3767.77 + d;
        auto bars = series({kETH0727_0000, kETH0727_0015, kETH0727_0030, kETH0727_0045, kETH0727_0100, kETH0727_0115});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 3734.78, 4, 3767.77, 1.0);
    }
    for (double d : {0.000001, 0.0000012, 0.000002, 0.00001, 0.004}) {
        Probe eng(0.01, 1.0);
        eng.script = "L.....";
        eng.exit_limit_ = 3767.77 + d;
        auto bars = series({kETH0727_0000, kETH0727_0015, kETH0727_0030, kETH0727_0045, kETH0727_0100, kETH0727_0115});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 3734.78, 5, 3767.78, 1.0);
    }
}

void test_eth_sell_stop_band() {
    std::printf("-- ETH sell stop 3694.56 - d on l 3694.56: d <= 5e-7 fills 01:45 @3694.56; d >= 1e-6 no fill; 3700.004 -> @3700.00 (floor) --\n");
    // bars: 0 00:00 (signal) 1 00:15 (fill 3713.05) 2 00:30 3 00:45 4 01:00 5 01:15 6 01:30 7 01:45 (o 3708.48 l 3694.56) 8 02:00
    auto eth0726 = [] { return series({kETH0726_0000, kETH0726_0015, kETH0726_0030, kETH0726_0045, kETH0726_0100,
                                       kETH0726_0115, kETH0726_0130, kETH0726_0145, kETH0726_0200}); };
    for (double d : {0.0, 0.0000005}) {
        Probe eng(0.01, 1.0);
        eng.script = "L........";
        eng.exit_stop_ = 3694.56 - d;
        auto bars = eth0726();
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 3713.05, 7, 3694.56, 1.0);
    }
    for (double d : {0.000001, 0.0000012}) {
        Probe eng(0.01, 1.0);
        eng.script = "L........";
        eng.exit_stop_ = 3694.56 - d;
        auto bars = eth0726();
        eng.run(bars.data(), (int)bars.size());
        expect_open_position(eng, true);
    }
    for (double stop : {3700.004, 3700.0 + 0.000000607}) {
        Probe eng(0.01, 1.0);
        eng.script = "L........";
        eng.exit_stop_ = stop;
        auto bars = eth0726();
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 3713.05, 7, 3700.0, 1.0);
    }
}

void test_eth_buy_limit_band() {
    std::printf("-- ETH buy limit (short) 3694.56 - d on l 3694.56: 0 fills 01:45 @3694.56; 1e-6 / 1.2e-6 no fill; 3700.004 -> @3700.00 (floor) --\n");
    auto eth0726 = [] { return series({kETH0726_0000, kETH0726_0015, kETH0726_0030, kETH0726_0045, kETH0726_0100,
                                       kETH0726_0115, kETH0726_0130, kETH0726_0145, kETH0726_0200}); };
    {
        Probe eng(0.01, 1.0);
        eng.script = "S........";
        eng.exit_limit_ = 3694.56;
        auto bars = eth0726();
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 3713.05, 7, 3694.56, 1.0);
    }
    for (double d : {0.000001, 0.0000012}) {
        Probe eng(0.01, 1.0);
        eng.script = "S........";
        eng.exit_limit_ = 3694.56 - d;
        auto bars = eth0726();
        eng.run(bars.data(), (int)bars.size());
        expect_open_position(eng, false);
    }
    for (double limit : {3700.004, 3700.0 - 0.000000607}) {
        Probe eng(0.01, 1.0);
        eng.script = "S........";
        eng.exit_limit_ = limit;
        auto bars = eth0726();
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 3713.05, 7, 3700.0, 1.0);
    }
}

void test_eth_buy_stop_band() {
    std::printf("-- ETH buy stop (short) 3767.77 + d on h 3767.77: 0 fills 01:00 @3767.77; 1e-6 / 1.2e-6 / 1e-5 fill 01:15 @3767.78; 3760.004 -> @3760.01 (ceil) --\n");
    auto eth0727 = [] { return series({kETH0727_0000, kETH0727_0015, kETH0727_0030, kETH0727_0045, kETH0727_0100, kETH0727_0115}); };
    {
        Probe eng(0.01, 1.0);
        eng.script = "S.....";
        eng.exit_stop_ = 3767.77;
        auto bars = eth0727();
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 3734.78, 4, 3767.77, 1.0);
    }
    for (double d : {0.000001, 0.0000012, 0.00001}) {
        Probe eng(0.01, 1.0);
        eng.script = "S.....";
        eng.exit_stop_ = 3767.77 + d;
        auto bars = eth0727();
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 3734.78, 5, 3767.78, 1.0);
    }
    for (double stop : {3760.004, 3760.0 + 0.000000607}) {
        Probe eng(0.01, 1.0);
        eng.script = "S.....";
        eng.exit_stop_ = stop;
        auto bars = eth0727();
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 3734.78, 4, stop > 3760.001 ? 3760.01 : 3760.0, 1.0);
    }
}

}  // namespace

int main() {
    std::printf("--- level_grid_snap (round 8 family T) ---\n");
    test_level_on_price_grid();
    test_f_sell_limit_band();
    test_f_sell_stop_band();
    test_f_buy_limit_band();
    test_masayanfx_56_limit_on_quantized_high();
    test_es_band_is_price_not_ticks();
    test_eurusd_band();
    test_eth_corpus_236_atr_level_in_band();
    test_eth_sell_limit_band();
    test_eth_sell_stop_band();
    test_eth_buy_limit_band();
    test_eth_buy_stop_band();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
