// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

/*
 * test_level_grid_snap.cpp — resting stop / limit price-grid snap.
 *
 * Pinned against TradingView behaviour (recorded tapes are kept outside this
 * repository): a level within 0.01 / pricescale^2 of a decimal grid price
 * snaps before touch testing. Outside that band it keeps its sub-tick value
 * and fills at the directional tick. Decimal grid and tick grid can differ.
 *
 * Hand-designed cent-tick bars isolate sell limits, sell stops, buy limits,
 * and a quantized high. Arithmetic residues sit just beyond/below an exact
 * touch; outside-band controls must wait for a later bar one tick farther.
 * Synthetic timestamps are 1000 * (index + 1). The separate quarter-tick
 * and five-decimal controls retain their price-scale boundary checks.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

// Invented sell-limit path: entry midpoint prints 64.03; bar 2 has
// O = H = 64.07, and bar 3 reaches the next tick 64.08.
const Bar kSellLimitSignal = mk_bar(64.00, 64.25, 63.75, 64.03);
const Bar kSellLimitEntry = mk_bar(64.035, 64.20, 64.00, 64.06);
const Bar kSellLimitTouch = mk_bar(64.07, 64.07, 64.02, 64.04);
const Bar kSellLimitBeyond = mk_bar(64.04, 64.08, 64.02, 64.06);
// Invented sell-stop path: lows touch 80.06, then 80.055 prints 80.06;
// only bar 5 reaches 80.05. Its open prints above the outside-band stop.
const Bar kSellStopSignal = mk_bar(80.50, 80.75, 80.25, 80.50);
const Bar kSellStopEntry = mk_bar(80.50, 80.80, 80.30, 80.60);
const Bar kSellStopHold = mk_bar(80.60, 80.70, 80.20, 80.30);
const Bar kSellStopTouch = mk_bar(80.30, 80.40, 80.06, 80.08);
const Bar kSellStopQuantizedTouch = mk_bar(80.08, 80.12, 80.055, 80.055);
const Bar kSellStopBeyond = mk_bar(80.055, 80.08, 79.90, 79.95);
// Invented buy-limit path: short entry 96.80, first low 96.40 on bar 3,
// next low 96.39 on bar 4. Earlier lows are strictly above either limit.
const Bar kBuyLimitSignal = mk_bar(96.80, 97.00, 96.60, 96.80);
const Bar kBuyLimitEntry = mk_bar(96.80, 97.00, 96.65, 96.70);
const Bar kBuyLimitHold = mk_bar(96.70, 96.90, 96.50, 96.60);
const Bar kBuyLimitTouch = mk_bar(96.60, 96.75, 96.40, 96.50);
const Bar kBuyLimitBeyond = mk_bar(96.50, 96.70, 96.39, 96.45);
// Invented quantized-high path: raw high 64.065 prints 64.07. The low
// 64.02 stays above stop 64.01, so only the limit can close the trade.
const Bar kQuantizedSignal = mk_bar(64.00, 64.25, 63.75, 64.03);
const Bar kQuantizedEntry = mk_bar(64.035, 64.20, 64.00, 64.04);
const Bar kQuantizedTouch = mk_bar(64.035, 64.065, 64.02, 64.05);
// Hand-designed quarter-tick path: entry 8190.00, exact high 8192.75
// on bar 2, then a high beyond the next quarter tick on bar 3.
const Bar kQuarterSignal = mk_bar(8189.00, 8191.00, 8188.00, 8190.00);
const Bar kQuarterEntry = mk_bar(8190.00, 8191.25, 8189.50, 8190.50);
const Bar kQuarterTouch = mk_bar(8191.00, 8192.75, 8190.75, 8192.00);
const Bar kQuarterBeyond = mk_bar(8191.50, 8193.50, 8191.25, 8193.25);
// Hand-designed five-decimal path: earlier highs stay below 1.20005;
// bar 3 touches exactly and bar 4 crosses the next tick 1.20006.
const Bar kDecimalSignal = mk_bar(1.19998, 1.20003, 1.19995, 1.20000);
const Bar kDecimalEntry = mk_bar(1.20000, 1.20003, 1.19997, 1.20001);
const Bar kDecimalHold = mk_bar(1.20001, 1.20004, 1.19999, 1.20002);
const Bar kDecimalTouch = mk_bar(1.20002, 1.20005, 1.20000, 1.20003);
const Bar kDecimalBeyond = mk_bar(1.20002, 1.20010, 1.20001, 1.20008);

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

// Fixed-quantity strategy: fixed qty, no commission / slippage, one position.
// Script chars (indexed by bar_index_): 'L' / 'S' market entry, '.' nothing;
// while in a position strategy.exit("X", "E", limit = exit_limit_, stop =
// exit_stop_) is re-issued every bar.
class Probe : public pineforge::source::PineStrategyHost {
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
    void on_source_bar(const Bar& /*bar*/) override {
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

// --- the snap itself ---------------------------------------------------------
void test_level_on_price_grid() {
    std::printf("-- level_on_price_grid: 0.01/pricescale^2 band, exact-decimal grid value --\n");
    Probe f(0.01);
    CHECK(f.decimals() == 2);
    CHECK(f.grid(64.03 + 0.04) == 64.07);            // the natural residue
    CHECK(f.grid(80.07 - 0.01) == 80.06);
    CHECK(f.grid(64.07 + 1e-6) == 64.07);           // lvl-lim-1e-6 IN
    CHECK(f.grid(64.07 + 9e-7) == 64.07);
    CHECK(f.grid(64.07 + 1.2e-6) == 64.07 + 1.2e-6); // lvl-lim-1.2e-6 OUT
    CHECK(f.grid(80.06 - 1e-6) == 80.06);           // lvl-stp-1e-6 IN
    CHECK(f.grid(80.06 - 3e-6) == 80.06 - 3e-6);    // lvl-stp-3e-6 OUT
    CHECK(f.grid(96.40 - 1e-6) == 96.40);           // lvl-blim-1e-6 IN
    CHECK(f.grid(96.40 - 1e-5) == 96.40 - 1e-5);    // lvl-blim-1e-5 OUT
    CHECK(f.grid(20.085) == 20.085);                // a genuine sub-tick level is untouched
    CHECK(f.grid(64.07) == 64.07);                  // on-grid is a fixed point
    CHECK(f.grid(2008.0 * 0.01) == 20.08);          // k*mintick residue -> the literal double
    CHECK(std::isnan(f.grid(kNaN)));
    Probe es(0.25);
    CHECK(es.decimals() == 2);                      // pricescale 100, not 1/0.25
    CHECK(es.grid(8192.75 + 5e-7) == 8192.75);      // lvl-q-lim-5e-7 IN
    CHECK(es.grid(8192.75 + 1e-6) == 8192.75 + 1e-6);   // lvl-q-lim-1e-6 OUT
    CHECK(es.grid(8192.75 + 2.5e-5) == 8192.75 + 2.5e-5); // 1e-4 ticks is NOT the band
    CHECK(es.grid(8192.76) == 8192.76);             // on the price grid, off the tick grid: kept
    Probe eu(0.00001);
    CHECK(eu.decimals() == 5);
    CHECK(eu.grid(1.20005 + 1e-13) == 1.20005);     // lvl-d-lim-1e-13 IN
    CHECK(eu.grid(1.20005 + 1e-12) == 1.20005);     // lvl-d-lim-1e-12 IN (FP boundary)
    CHECK(eu.grid(1.20005 + 2e-12) == 1.20005 + 2e-12); // lvl-d-lim-2e-12 OUT
    CHECK(eu.grid(1.20005 + 1e-11) == 1.20005 + 1e-11); // OUT
    CHECK(eu.grid(1.20005 + 1e-9) == 1.20005 + 1e-9);   // OUT
    Probe none(0.0);
    CHECK(none.decimals() == -1);
    CHECK(none.grid(64.07 + 1e-6) == 64.07 + 1e-6); // no tick, no grid
    Probe bin(0.0078125);                            // 1/128 = 0.0078125: 7 decimals,
    CHECK(bin.decimals() == 7);                     // pricescale 1e7 -> band 1e-16:
    CHECK(bin.grid(13.7451) == 13.7451);            // effectively no snap
    CHECK(bin.grid(13.7451 + 1e-9) == 13.7451 + 1e-9);
}

// --- synthetic sell limit at an exact O = H touch ---------------------------
void test_case_sell_limit_band() {
    std::printf("-- synthetic sell limit: inside band bar 2 @64.07, outside band bar 3 @64.08 --\n");
    // Entry 64.035 prints 64.03; band = 0.01 / 100^2 = 1e-6.
    for (double d : {0.0, 1e-12, 1e-9, 5e-7, 9e-7, 1e-6}) {
        Probe eng(0.01);
        eng.script = "L...";
        eng.exit_limit_ = 64.07 + d;
        auto bars = series({kSellLimitSignal, kSellLimitEntry, kSellLimitTouch, kSellLimitBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 64.03, 2, 64.07, 100.0);
    }
    {   // 64.03 + 0.04 = 64.07000000000001: above the exact touch.
        Probe eng(0.01);
        eng.script = "L...";
        eng.exit_limit_ = 64.03 + 0.04;
        auto bars = series({kSellLimitSignal, kSellLimitEntry, kSellLimitTouch, kSellLimitBeyond});
        eng.run(bars.data(), (int)bars.size());
        // Snapping the residue makes bar 2 marketable at its open 64.07.
        expect_single_trade(eng, true, 1, 64.03, 2, 64.07, 100.0);
    }
    for (double d : {1.2e-6, 2e-6, 1e-5, 1e-4}) {
        Probe eng(0.01);
        eng.script = "L...";
        eng.exit_limit_ = 64.07 + d;
        auto bars = series({kSellLimitSignal, kSellLimitEntry, kSellLimitTouch, kSellLimitBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 64.03, 3, 64.08, 100.0); // ceil_tick(64.07 + d)
    }
}

// --- synthetic sell stop at exact and quantized low touches -----------------
void test_case_sell_stop_band() {
    std::printf("-- synthetic sell stop: inside band bar 3 @80.06, outside band bar 5 @80.05 --\n");
    // Lows on bars 3/4 print 80.06; bar 5 crosses floor_tick(stop) = 80.05.
    for (double stop : {80.06, 80.07 - 0.01, 80.06 - 1e-9, 80.06 - 1e-6}) {
        Probe eng(0.01);
        eng.script = "L.....";
        eng.exit_stop_ = stop;
        auto bars = series({kSellStopSignal, kSellStopEntry, kSellStopHold, kSellStopTouch, kSellStopQuantizedTouch, kSellStopBeyond});
        eng.run(bars.data(), (int)bars.size());
        // 80.07 - 0.01 = 80.05999999999999; snapping admits the bar-3 touch.
        expect_single_trade(eng, true, 1, 80.50, 3, 80.06, 100.0);
    }
    for (double stop : {80.06 - 3e-6, 80.06 - 1e-5, 80.06 - 1e-4}) {
        Probe eng(0.01);
        eng.script = "L.....";
        eng.exit_stop_ = stop;
        auto bars = series({kSellStopSignal, kSellStopEntry, kSellStopHold, kSellStopTouch, kSellStopQuantizedTouch, kSellStopBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 80.50, 5, 80.05, 100.0); // floor_tick(80.06 - d)
    }
}

// --- synthetic buy limit for a short position -------------------------------
void test_case_buy_limit_band() {
    std::printf("-- synthetic buy limit: inside band bar 3 @96.40, outside band bar 4 @96.39 --\n");
    // Inside band: 96.40 touch on bar 3. Outside: floor_tick(limit) = 96.39.
    for (double d : {0.0, 1e-6}) {
        Probe eng(0.01);
        eng.script = "S....";
        eng.exit_limit_ = 96.40 - d;
        auto bars = series({kBuyLimitSignal, kBuyLimitEntry, kBuyLimitHold, kBuyLimitTouch, kBuyLimitBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 96.80, 3, 96.40, 100.0);
    }
    {
        Probe eng(0.01);
        eng.script = "S....";
        eng.exit_limit_ = 96.40 - 1e-5;
        auto bars = series({kBuyLimitSignal, kBuyLimitEntry, kBuyLimitHold, kBuyLimitTouch, kBuyLimitBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 96.80, 4, 96.39, 100.0); // floor_tick(96.40 - 1e-5)
    }
}

// --- limit residue versus a tick-quantized high -----------------------------
void test_limit_on_quantized_high() {
    std::printf("-- synthetic limit residue fills at the quantized high on bar 2 --\n");
    // Entry 64.035 prints 64.03; high 64.065 prints 64.07 on bar 2.
    Probe eng(0.01);
    eng.script = "L..";
    eng.exit_limit_ = 64.03 + 0.04;
    eng.exit_stop_ = 64.03 - 0.02;
    auto bars = series({kQuantizedSignal, kQuantizedEntry, kQuantizedTouch});
    eng.run(bars.data(), (int)bars.size());
    // 64.03 + 0.04 exceeds literal 64.07 by one ulp; grid snapping
    // removes that residue before comparing with the quantized high.
    expect_single_trade(eng, true, 1, 64.03, 2, 64.07, 100.0);
}

// --- quarter tick: the band is 1e-6 in price, not 1e-4 ticks ---------------
void test_es_band_is_price_not_ticks() {
    std::printf("-- synthetic quarter-tick sell limit: inside band bar 2, outside band bar 3 --\n");
    // Entry on bar 1 is 8190.00. For inside-band offsets, snap to the
    // exact bar-2 high 8192.75; outside-band ceil_tick(8192.75 + d) = 8193.00.
    // At this invented base, adding 1e-6 leaves 1.0000003385e-6 in binary,
    // just outside the 0.01 / 100^2 = 1e-6 price band.
    for (double d : {0.0, 5e-7}) {
        Probe eng(0.25, 1.0);
        eng.script = "L...";
        eng.exit_limit_ = 8192.75 + d;
        auto bars = series({kQuarterSignal, kQuarterEntry, kQuarterTouch, kQuarterBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 8190.00, 2, 8192.75, 1.0);
    }
    for (double d : {1e-6, 1e-5, 2.5e-5, 5e-5}) {
        Probe eng(0.25, 1.0);
        eng.script = "L...";
        eng.exit_limit_ = 8192.75 + d;
        auto bars = series({kQuarterSignal, kQuarterEntry, kQuarterTouch, kQuarterBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 8190.00, 3, 8193.0, 1.0);
    }
}

// --- five-decimal tick: pricescale 1e5 -> band 1e-12 ------------------------------
void test_eurusd_band() {
    std::printf("-- synthetic five-decimal sell limit: inside band bar 3, outside band bar 4 --\n");
    // Entry on bar 1 is 1.20000. Inside-band offsets snap to the exact
    // bar-3 high 1.20005; outside-band ceil_tick(1.20005 + d) = 1.20006.
    // Adding 1e-12 leaves 1.0000889006e-12 in binary (the engine's slack admits
    // this FP boundary of the 0.01 / 100000^2 = 1e-12 band); adding 2e-12 is outside it.
    for (double d : {0.0, 1e-13, 1e-12}) {
        Probe eng(0.00001);
        eng.script = "L....";
        eng.exit_limit_ = 1.20005 + d;
        auto bars = series({kDecimalSignal, kDecimalEntry, kDecimalHold, kDecimalTouch, kDecimalBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 1.20000, 3, 1.20005, 100.0);
    }
    for (double d : {2e-12, 1e-11, 1e-9, 1e-6}) {
        Probe eng(0.00001);
        eng.script = "L....";
        eng.exit_limit_ = 1.20005 + d;
        auto bars = series({kDecimalSignal, kDecimalEntry, kDecimalHold, kDecimalTouch, kDecimalBeyond});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 1.20000, 4, 1.20006, 100.0);
    }
}

}  // namespace

int main() {
    std::printf("--- level_grid_snap (round 8 family T) ---\n");
    test_level_on_price_grid();
    test_case_sell_limit_band();
    test_case_sell_stop_band();
    test_case_buy_limit_band();
    test_limit_on_quantized_high();
    test_es_band_is_price_not_ticks();
    test_eurusd_band();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
