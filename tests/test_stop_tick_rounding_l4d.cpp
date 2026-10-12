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
 * test_stop_tick_rounding.cpp — resting stop / limit touches use OHLC
 * quantized to the nearest tick (floor(p / mintick + 0.5)); order levels
 * stay raw and fills keep their directional / limit-or-better snap.
 * Trailing extremes and bar-leg order retain the raw path.
 *
 * Pinned against TradingView behaviour (recorded tapes are kept outside
 * this repository). Invented bars place sub-tick lows / highs on both sides
 * of a rounding boundary, with separate open-gap and raw-trail controls.
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
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

// Bar index i carries timestamp ts(i) so a trade's entry/exit bar can be
// read back from its entry_time / exit_time.
static int64_t ts(int i) { return 1000 * (i + 1); }

namespace {

// Invented waypoints, grouped by scenario rather than calendar dates.
// A..D: a high just above 79.91 must not touch a 79.915 buy stop.
// E..H: a low 79.8242 rounds down to 79.82 and touches raw level 79.8246.
// I..P: low 80.245 rounds up and misses; a later low / high crosses levels.
// Q..U: an open rounds up, while a raw trailing best must remain unsnapped.
const Bar kA = mk_bar(0, 79.65, 79.85, 79.50, 79.70);
const Bar kB = mk_bar(0, 79.70, 79.82, 79.55, 79.75);
const Bar kC = mk_bar(0, 79.76, 79.9103, 79.65, 79.80);
const Bar kD = mk_bar(0, 79.80, 80.05, 79.72, 80.00);
const Bar kE = mk_bar(0, 80.00, 80.25, 79.95, 80.20);
const Bar kF = mk_bar(0, 80.20, 80.28, 80.00, 80.10);
const Bar kG = mk_bar(0, 80.10, 80.20, 79.90, 80.05);
const Bar kH = mk_bar(0, 80.05, 80.15, 79.8242, 79.95);
const Bar kI = mk_bar(0, 80.35, 80.65, 80.25, 80.60);
const Bar kJ = mk_bar(0, 80.52, 80.62, 80.40, 80.48);
const Bar kK = mk_bar(0, 80.55, 80.75, 80.35, 80.70);
const Bar kL = mk_bar(0, 80.50, 80.60, 80.30, 80.45);
const Bar kM = mk_bar(0, 80.46, 80.60, 80.245, 80.35);
const Bar kN = mk_bar(0, 80.38, 80.6351, 80.10, 80.28);
const Bar kO = mk_bar(0, 80.30, 80.65, 80.20, 80.40);
const Bar kP = mk_bar(0, 80.25, 80.40, 80.05, 80.30);
const Bar kQ = mk_bar(0, 80.70, 80.80, 80.45, 80.50);
const Bar kR = mk_bar(0, 80.41, 80.56, 80.32, 80.44);
const Bar kS = mk_bar(0, 80.415, 80.635, 80.35, 80.61);
const Bar kT = mk_bar(0, 80.58, 80.70, 80.20, 80.30);
const Bar kU = mk_bar(0, 80.45, 80.95, 80.35, 80.80);

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

// Invented instrument: pointvalue 1, mintick 0.01, whole lots, fixed 100,
// no commission / slippage, one position at a time.
// Script chars (indexed by bar_index_), all on entry id "E":
//   'L' / 'S'  market entry long / short
//   'e'        strategy.entry(stop = entry_level_)   in entry_long_ direction
//   'm'        strategy.entry(limit = entry_level_)
//   'o'        strategy.order(stop = entry_level_)
//   'C'        strategy.close_all()
//   '.'        nothing
// While a position is open the bracket strategy.exit("X", "E", limit =
// exit_limit_, stop = exit_stop_, trail_points_, trail_offset_) is re-issued
// every bar, like a script's `if strategy.position_size != 0`.
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe() {
        initial_capital_ = 1000000000.0;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 100.0;
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
    bool entry_long_ = true;
    double entry_level_ = kNaN;
    double exit_stop_ = kNaN;
    double exit_limit_ = kNaN;
    double trail_points_ = kNaN;
    double trail_offset_ = kNaN;
    // Issue the bracket while flat as well, so it rests next to its stop
    // entry and is live on the entry's own fill bar (same-bar bracket).
    bool arm_exit_flat_ = false;

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ >= 0 && bar_index_ < (int)script.size()) {
            switch (script[bar_index_]) {
                case 'L': strategy_entry("E", true); break;
                case 'S': strategy_entry("E", false); break;
                case 'e': strategy_entry("E", entry_long_, kNaN, entry_level_); break;
                case 'm': strategy_entry("E", entry_long_, entry_level_, kNaN); break;
                case 'o': strategy_order("E", entry_long_, kNaN, kNaN, entry_level_); break;
                case 'C': strategy_close_all(); break;
                default: break;
            }
        }
        const bool armed = std::isfinite(exit_stop_) || std::isfinite(exit_limit_)
            || std::isfinite(trail_points_);
        if (armed && (arm_exit_flat_ || position_side_ != PositionSide::FLAT)) {
            strategy_exit("X", "E", exit_limit_, exit_stop_,
                          trail_points_, trail_offset_);
        }
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::syminfo_mintick_;
    double grid(double p) const { return tick_grid_price(p); }
};

// One closed trade: entered on bar entry_bar at entry_px, exited on bar
// exit_bar at exit_px.
void expect_single_trade(const Probe& eng, bool is_long,
                         int entry_bar, double entry_px,
                         int exit_bar, double exit_px) {
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() != 1) return;
    const Trade& t = eng.get_trade(0);
    CHECK(t.is_long == is_long);
    CHECK(t.entry_time == ts(entry_bar));
    CHECK_NEAR(t.entry_price, entry_px, 1e-9);
    CHECK(t.exit_time == ts(exit_bar));
    CHECK_NEAR(t.exit_price, exit_px, 1e-9);
    CHECK_NEAR(t.qty, 100.0, 1e-9);
    if (t.exit_time != ts(exit_bar) || std::fabs(t.exit_price - exit_px) > 1e-9) {
        std::printf("        got exit bar %lld @%.5f (expected bar %d @%.5f)\n",
                    (long long)(t.exit_time / 1000 - 1), t.exit_price,
                    exit_bar, exit_px);
    }
}

// --- the quantization itself ---------------------------------------------
void test_tick_grid_price() {
    std::printf("-- tick_grid_price: nearest tick, half up, literal-exact --\n");
    Probe eng;
    CHECK(eng.grid(80.245) == 80.25);      // 8024.5 exact -> up
    CHECK(eng.grid(80.635) == 80.64);
    CHECK(eng.grid(79.8242) == 79.82);
    CHECK(eng.grid(80.6351) == 80.64);
    CHECK(eng.grid(79.9103) == 79.91);     // nearest, not ceil
    CHECK(eng.grid(80.415) == 80.42);
    CHECK(eng.grid(80.10) == 80.10);       // on-grid input is a fixed point
    CHECK(eng.grid(80.64) == 80.64);
    CHECK(std::isnan(eng.grid(kNaN)));
    // Every grid point is the double its decimal literal parses to, so an
    // on-grid level compares equal bit-for-bit (8007 * 0.01 gives
    // 80.07000000000001, while 8007 / 100 parses as the literal 80.07).
    CHECK(eng.grid(8007.0 * 0.01) == 80.07);
    // Invented binary-midpoint controls: 80.005 / 0.01 is just below
    // 8000.5 (rounds down); 80.015 / 0.01 is exactly 8001.5 (rounds up).
    CHECK(eng.grid(80.005) == 80.00);
    CHECK(eng.grid(80.015) == 80.02);
    // No tick, no quantization.
    eng.syminfo_mintick_ = 0.0;
    CHECK(eng.grid(80.245) == 80.245);
    // Binary tick: 82.4147 * 128 = 10549.0816 -> k = 10549.
    eng.syminfo_mintick_ = 0.0078125;
    CHECK(eng.grid(82.4147) == 10549.0 * 0.0078125);
    // Non-integral inverse: 82.4147 / 2.5 = 32.96588 -> k = 33.
    eng.syminfo_mintick_ = 2.5;
    CHECK(eng.grid(82.4147) == 33.0 * 2.5);
    CHECK(eng.grid(83.75) == 34.0 * 2.5);       // 33.5 exact -> half up -> 85
}

// --- (a) long sell-stop -----------------------------------------------------
void test_long_sell_stop_skips_subtick_low() {
    std::printf("-- long sell-stop: 80.245 low -> 80.25 is not a touch (boundary control) --\n");
    // bars: 0 I (signal) 1 J (fill 80.52) 2 K 3 L 4 M 5 N
    for (double stop : {80.2468, 80.2452, 80.2447, 80.245}) {
        Probe eng;
        eng.script = "L.....";
        eng.exit_stop_ = stop;
        auto bars = series({kI, kJ, kK, kL, kM, kN});
        eng.run(bars.data(), (int)bars.size());
        // M low 80.245 -> 80.25 > every stop; N low 80.10 touches.
        // Entry = J open 80.52; each raw level floors to exit 80.24.
        expect_single_trade(eng, true, 1, 80.52, 5, 80.24);
    }
}

void test_long_sell_stop_fires_on_rounded_down_low() {
    std::printf("-- long sell-stop 79.8246: 79.8242 low -> 79.82 IS a touch (boundary control) --\n");
    // bars: 0 E (signal) 1 F (fill 80.20) 2 G 3 H (low 79.8242)
    Probe eng;
    eng.script = "L...";
    eng.exit_stop_ = 79.8246;
    auto bars = series({kE, kF, kG, kH});
    eng.run(bars.data(), (int)bars.size());
    // Entry = F open 80.20; floor(79.8246 / 0.01) / 100 = 79.82.
    // Entry F.open = 80.20; H.low rounds to 79.82, level floors to 79.82.
    expect_single_trade(eng, true, 1, 80.20, 3, 79.82);
}

void test_long_sell_stop_open_is_quantized() {
    std::printf("-- long sell-stop 80.416: 80.415 open -> 80.42 is no gap (boundary control) --\n");
    // bars: 0 Q (signal) 1 R (fill 80.41) 2 S (open 80.415, low 80.35)
    Probe eng;
    eng.script = "L..";
    eng.exit_stop_ = 80.416;
    auto bars = series({kQ, kR, kS});
    eng.run(bars.data(), (int)bars.size());
    // Level fill 80.41 (floor of 80.416), not the gap fill at the open 80.42.
    // R.open = 80.41; S.open rounds above level; floor(80.416*100)/100 = 80.41.
    expect_single_trade(eng, true, 1, 80.41, 2, 80.41);
}

// --- (b) short buy-stop -----------------------------------------------------
void test_short_buy_stop_fires_on_rounded_up_high() {
    std::printf("-- short buy-stop: 80.6351 high -> 80.64 touches 80.6353 (boundary control) --\n");
    // bars: 0 K (signal) 1 L (fill 80.50) 2 M 3 N (high 80.6351)
    for (double stop : {80.6347, 80.63508, 80.6353}) {
        Probe eng;
        eng.script = "S...";
        eng.exit_stop_ = stop;
        auto bars = series({kK, kL, kM, kN});
        eng.run(bars.data(), (int)bars.size());
        // pre-fix: 80.6353 > 80.6351 never fired here.
        // L.open = 80.50; N.high rounds to 80.64; ceil(stop*100)/100 = 80.64.
        expect_single_trade(eng, false, 1, 80.50, 3, 80.64);
    }
    // The exact half-tick high 80.635 -> 80.64 touches 80.6351 too
    // (boundary control): bars 0 Q (signal) 1 R (fill 80.41) 2 S.
    {
        Probe eng;
        eng.script = "S..";
        eng.exit_stop_ = 80.6351;
        auto bars = series({kQ, kR, kS});
        eng.run(bars.data(), (int)bars.size());
        // R.open = 80.41; S.high rounds to 80.64; ceil(80.6351*100)/100 = 80.64.
        expect_single_trade(eng, false, 1, 80.41, 2, 80.64);
    }
}

void test_short_buy_stop_high_rounds_nearest_not_up() {
    std::printf("-- short buy-stop 79.915: 79.9103 high -> 79.91 is no touch (boundary control) --\n");
    // bars: 0 A (signal) 1 B (fill 79.70) 2 C (high 79.9103) 3 D
    Probe eng;
    eng.script = "S...";
    eng.exit_stop_ = 79.915;
    auto bars = series({kA, kB, kC, kD});
    eng.run(bars.data(), (int)bars.size());
    // B.open = 79.70; C.high rounds below level; D touches, ceil(79.915*100)/100 = 79.92.
    expect_single_trade(eng, false, 1, 79.70, 3, 79.92);
}

// --- (c) strategy.exit(limit=) ---------------------------------------------
void test_exit_limits() {
    std::printf("-- exit limits: sell-limit on the 80.6351 high, buy-limit on the 80.245 / 79.8242 lows (boundary control) --\n");
    for (double limit : {80.6347, 80.63508, 80.6353}) {
        Probe eng;
        eng.script = "L...";
        eng.exit_limit_ = limit;
        auto bars = series({kK, kL, kM, kN});
        eng.run(bars.data(), (int)bars.size());
        // L.open = 80.50; N.high rounds to 80.64, satisfying each sell-limit at its ceil.
        expect_single_trade(eng, true, 1, 80.50, 3, 80.64);
    }
    for (double limit : {80.2452, 80.2447}) {
        Probe eng;
        eng.script = "S.....";
        eng.exit_limit_ = limit;
        auto bars = series({kI, kJ, kK, kL, kM, kN});
        eng.run(bars.data(), (int)bars.size());
        // J.open = 80.52; M.low rounds above the buy-limit; N touches, floor(limit*100)/100 = 80.24.
        expect_single_trade(eng, false, 1, 80.52, 5, 80.24);
    }
    {
        Probe eng;
        eng.script = "S...";
        eng.exit_limit_ = 79.8246;
        auto bars = series({kE, kF, kG, kH});
        eng.run(bars.data(), (int)bars.size());
        // Entry = F open 80.20; the buy-limit floor gives 79.82.
        // F.open = 80.20; H.low and the buy-limit floor both give 79.82.
        expect_single_trade(eng, false, 1, 80.20, 3, 79.82);
    }
}

// --- (d) stop / limit entries ----------------------------------------------
// bars: 0 L (placed) 1 M 2 N 3 O (close_all) 4 P (exit 80.25)
void test_stop_entries() {
    std::printf("-- entry stops: long on the 80.6351 high, short on the 80.245 low (boundary control) --\n");
    for (double level : {80.6347, 80.63508, 80.6353}) {
        for (char kind : {'e', 'o'}) {
            Probe eng;
            eng.script = std::string(1, kind) + "..C.";
            eng.entry_long_ = true;
            eng.entry_level_ = level;
            auto bars = series({kL, kM, kN, kO, kP});
            eng.run(bars.data(), (int)bars.size());
            // M.high misses; N.high touches -> ceil(level*100)/100 = 80.64; close fills P.open = 80.25.
            expect_single_trade(eng, true, 2, 80.64, 4, 80.25);
        }
    }
    for (double level : {80.2452, 80.2447}) {
        for (char kind : {'e', 'o'}) {
            Probe eng;
            eng.script = std::string(1, kind) + "..C.";
            eng.entry_long_ = false;
            eng.entry_level_ = level;
            auto bars = series({kL, kM, kN, kO, kP});
            eng.run(bars.data(), (int)bars.size());
            // pre-fix: 80.2452 filled on M (bar 1).
            // M.low misses; N.low touches -> floor(level*100)/100 = 80.24; close fills P.open = 80.25.
            expect_single_trade(eng, false, 2, 80.24, 4, 80.25);
        }
    }
}

void test_limit_entries() {
    std::printf("-- entry limits: long on the 80.245 low, short on the 80.6351 high (boundary control) --\n");
    for (double level : {80.2452, 80.2447}) {
        Probe eng;
        eng.script = "m..C.";
        eng.entry_long_ = true;
        eng.entry_level_ = level;
        auto bars = series({kL, kM, kN, kO, kP});
        eng.run(bars.data(), (int)bars.size());
        // N.low first touches the buy-limit; floor(level*100)/100 = 80.24; close fills P.open = 80.25.
        expect_single_trade(eng, true, 2, 80.24, 4, 80.25);
    }
    for (double level : {80.63508, 80.6353}) {
        Probe eng;
        eng.script = "m..C.";
        eng.entry_long_ = false;
        eng.entry_level_ = level;
        auto bars = series({kL, kM, kN, kO, kP});
        eng.run(bars.data(), (int)bars.size());
        // N.high first touches the sell-limit; ceil(level*100)/100 = 80.64; close fills P.open = 80.25.
        expect_single_trade(eng, false, 2, 80.64, 4, 80.25);
    }
}

// --- trail: not quantized (raw extremes, unchanged) -------------------------
void test_trail_keeps_raw_path() {
    std::printf("-- trail 20/3 over the 80.635 high: exits at the next open 80.58 (boundary control) --\n");
    // bars: 0 Q (signal) 1 R (fill 80.41) 2 S (high 80.635) 3 T (open 80.58)
    Probe eng;
    eng.script = "L...";
    eng.trail_points_ = 20.0;
    eng.trail_offset_ = 3.0;
    auto bars = series({kQ, kR, kS, kT});
    eng.run(bars.data(), (int)bars.size());
    // raw best 80.635 - 0.03 = 80.605 < close 80.61 on S: no fill; the
    // T open 80.58 gaps through -> 80.58. (A quantized best 80.64 would
    // have filled S @80.61, which TradingView does not do.)
    expect_single_trade(eng, true, 1, 80.41, 3, 80.58);
}

// --- leg order: the raw bar's, on every path coordinate ---------------------
void test_leg_order_is_the_raw_bars() {
    std::printf("-- leg order: quantization flips the O->H / O->L proximity tie, the cursor stays in the raw bar's order --\n");
    // Raw distances: H-O = 0.0096 < O-L = 0.0100, hence HIGH first.
    // Tick twin: O 82.42 H 82.43 L 82.41 C 82.43. Its exactly equal
    // distances choose LOW first. Entry and bracket must share the RAW
    // leg order: entry ceil(82.4252 / 0.01) / 100 = 82.43 on O->H,
    // then stop floor(82.4162 / 0.01) / 100 = 82.41 on H->L, same bar.
    // Taking the entry cursor from the low-first twin skips that retrace.
    const Bar flip = mk_bar(0, 82.4248, 82.4344, 82.4148, 82.425);
    Probe probe;
    CHECK(probe.grid(flip.open) == 82.42);
    CHECK(probe.grid(flip.high) == 82.43);
    CHECK(probe.grid(flip.low) == 82.41);
    CHECK(probe.grid(flip.close) == 82.43);

    const Bar placement = mk_bar(0, 82.30, 82.40, 82.20, 82.35);
    const Bar after = mk_bar(0, 82.50, 82.65, 82.45, 82.60);
    Probe eng;
    eng.script = "e..";
    eng.entry_long_ = true;
    eng.entry_level_ = 82.4252;
    eng.exit_stop_ = 82.4162;
    eng.arm_exit_flat_ = true;
    auto bars = series({placement, flip, after});
    eng.run(bars.data(), (int)bars.size());
    // Same raw H->L leg: entry ceil(82.4252*100)/100 = 82.43; exit floor(82.4162*100)/100 = 82.41.
    expect_single_trade(eng, true, 1, 82.43, 1, 82.41);
}

// --- control: the quantization is the tick's doing --------------------------
void test_no_tick_no_quantization() {
    std::printf("-- control: mintick 0 keeps the raw compare (80.2452 fires on the 80.245 low) --\n");
    Probe eng;
    eng.syminfo_mintick_ = 0.0;
    eng.script = "L.....";
    eng.exit_stop_ = 80.2452;
    auto bars = series({kI, kJ, kK, kL, kM, kN});
    eng.run(bars.data(), (int)bars.size());
    // No tick: raw compare AND raw fill (no snap either).
    // J.open = 80.52; M.raw_low = 80.245 < raw stop; no tick means unsnapped fill 80.2452.
    expect_single_trade(eng, true, 1, 80.52, 4, 80.2452);
}

}  // namespace

int main() {
    std::printf("--- stop_tick_rounding ---\n");
    test_tick_grid_price();
    test_long_sell_stop_skips_subtick_low();
    test_long_sell_stop_fires_on_rounded_down_low();
    test_long_sell_stop_open_is_quantized();
    test_short_buy_stop_fires_on_rounded_up_high();
    test_short_buy_stop_high_rounds_nearest_not_up();
    test_exit_limits();
    test_stop_entries();
    test_limit_entries();
    test_trail_keeps_raw_path();
    test_leg_order_is_the_raw_bars();
    test_no_tick_no_quantization();
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
