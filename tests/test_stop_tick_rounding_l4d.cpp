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
 * test_stop_tick_rounding.cpp — round 6, design-stop-tick-rounding:
 * TradingView's broker emulator tests a resting stop / limit against the
 * bar's OHLC QUANTIZED to the tick (nearest, floor(p / mintick + 0.5)) while
 * the order LEVEL stays raw; the fill keeps its directional / limit-or-better
 * snap. The engine used to compare the RAW bar prices, so a sell-stop at
 * 13.74624 fired on a 13.745 low that TradingView (low -> 13.75) walks past.
 *
 * Every case below is a `lab tv` tape on NYSE:F 1D (mintick 0.01, sub-penny
 * prints; scratchpad/r6/pins/stopround-*, 2026-09-04) replayed on the feed's
 * own bars (tape times are UTC+8 evenings of the same trading day):
 *
 *   stopround-xs-L-{1374624,137451,137449,13745}  long sell-stop, entry
 *       01-28 @13.88: every one SKIPS the 02-02 bar (low 13.745) and fills
 *       02-03 @13.74.
 *   stopround-xs-L-133449   long sell-stop 13.3449, entry 01-22 @13.78:
 *       fills 01-26 (low 13.3448) @13.34 — so the level is NOT floored
 *       before the compare (13.34 vs 13.3448 would not fire) and the bar is
 *       NOT raw (02-02 would fire): only the quantized low explains both.
 *   stopround-xs-S-{140349,1403505,140352}  short buy-stop, entry 01-30
 *       @13.91: all fill 02-03 (high 14.0351 -> 14.04) @14.04.
 *   stopround-xs-S-140351   short buy-stop 14.0351, entry 02-19 @13.77:
 *       fills 02-20 (high 14.035 -> 14.04) @14.04.
 *   stopround-xs-S-13225-high  short buy-stop 13.225, entry 12-08 @13.07:
 *       skips 12-09 (high 13.2202 -> 13.22), fills 12-10 @13.23 — the
 *       high rounds to NEAREST, not up.
 *   stopround-xs-L-13776-open  long sell-stop 13.776, entry 02-19 @13.77:
 *       the 02-20 open 13.775 (-> 13.78) is NOT a gap; fills at the level
 *       13.77, not at the open.
 *   stopround-xl-L-{140349,1403505,140352}  long sell-limit -> 02-03 @14.04;
 *   stopround-xl-S-{137451,137449} short buy-limit -> 02-03 @13.74;
 *   stopround-xl-S-133449  short buy-limit -> 01-26 @13.34.
 *   stopround-es-L-{140349,1403505,140352} / stopround-eo-L-1403505
 *       strategy.entry / strategy.order long stop placed 01-30 -> fill
 *       02-03 @14.04; stopround-es-S-{137451,137449} short stop -> 02-03
 *       @13.74.
 *   stopround-el-L-{137451,137449} long limit entry placed 01-30 -> 02-03
 *       @13.74; stopround-el-S-{1403505,140352} short limit entry -> 02-03
 *       @14.04.
 *   stopround-xt-L-trail  long entry 02-19 @13.77, trail_points 20 /
 *       trail_offset 3: exits 02-23 at the open 13.98 — the raw-extreme
 *       trail behaviour the engine already had (a quantized 14.04 best
 *       would have filled 02-20 @14.01). The trail is NOT quantized.
 *   stopround-ohlc-{0,1}  Pine's own low / high (encoded in the trade qty)
 *       are the raw prints 13.745 / 13.3448 / 14.0351 — the feed's values —
 *       so the quantization is the broker's, not the data's.
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

// NYSE:F daily bars (registry feed e3dd3a88e85b, UTC-day labels).
const Bar kDec05 = mk_bar(0, 13.15, 13.28, 13.0, 13.03);
const Bar kDec08 = mk_bar(0, 13.07, 13.16, 12.945, 13.14);
const Bar kDec09 = mk_bar(0, 13.13, 13.2202, 13.06, 13.08);
const Bar kDec10 = mk_bar(0, 13.08, 13.42, 13.07, 13.41);
const Bar kJan21 = mk_bar(0, 13.405, 13.77, 13.405, 13.77);
const Bar kJan22 = mk_bar(0, 13.78, 13.84, 13.7, 13.71);
const Bar kJan23 = mk_bar(0, 13.7, 13.7, 13.55, 13.56);
const Bar kJan26 = mk_bar(0, 13.56, 13.655, 13.3448, 13.44);
const Bar kJan27 = mk_bar(0, 13.64, 13.945, 13.51, 13.93);
const Bar kJan28 = mk_bar(0, 13.88, 13.89, 13.76, 13.82);
const Bar kJan29 = mk_bar(0, 13.89, 14.09, 13.795, 14.0);
const Bar kJan30 = mk_bar(0, 13.91, 13.98, 13.79, 13.88);
const Bar kFeb02 = mk_bar(0, 13.86, 13.895, 13.745, 13.81);
const Bar kFeb03 = mk_bar(0, 13.82, 14.0351, 13.61, 13.73);
const Bar kFeb04 = mk_bar(0, 13.72, 14.0, 13.69, 13.82);
const Bar kFeb05 = mk_bar(0, 13.75, 13.82, 13.53, 13.72);
const Bar kFeb18 = mk_bar(0, 14.11, 14.14, 13.805, 13.85);
const Bar kFeb19 = mk_bar(0, 13.77, 13.945, 13.69, 13.78);
const Bar kFeb20 = mk_bar(0, 13.775, 14.035, 13.72, 14.01);
const Bar kFeb23 = mk_bar(0, 13.98, 14.04, 13.57, 13.64);
const Bar kFeb24 = mk_bar(0, 13.77, 14.325, 13.73, 14.2);

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

// NYSE:F — pointvalue 1, mintick 0.01, whole shares, fixed 100 shares, no
// commission / slippage, one position at a time (the pins' strategy()).
// Script chars (indexed by bar_index_), all on entry id "E":
//   'L' / 'S'  market entry long / short
//   'e'        strategy.entry(stop = entry_level_)   in entry_long_ direction
//   'm'        strategy.entry(limit = entry_level_)
//   'o'        strategy.order(stop = entry_level_)
//   'C'        strategy.close_all()
//   '.'        nothing
// While a position is open the bracket strategy.exit("X", "E", limit =
// exit_limit_, stop = exit_stop_, trail_points_, trail_offset_) is re-issued
// every bar, exactly like the pins' `if strategy.position_size != 0`.
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
    CHECK(eng.grid(13.745) == 13.75);      // 1374.5 exact -> up
    CHECK(eng.grid(14.035) == 14.04);
    CHECK(eng.grid(13.3448) == 13.34);
    CHECK(eng.grid(14.0351) == 14.04);
    CHECK(eng.grid(13.2202) == 13.22);     // nearest, not ceil
    CHECK(eng.grid(13.775) == 13.78);
    CHECK(eng.grid(13.61) == 13.61);       // on-grid input is a fixed point
    CHECK(eng.grid(14.04) == 14.04);
    CHECK(std::isnan(eng.grid(kNaN)));
    // Every grid point is the double its decimal literal parses to, so an
    // on-grid level compares equal bit-for-bit (k * 0.01 would give
    // 14.040000000000001 for k = 1404).
    CHECK(eng.grid(1404.0 * 0.01) == 14.04);
    // The finding-446 binary-midpoint artifacts are preserved: 228.765 sits
    // just under its midpoint and rounds DOWN, 214.385 sits on it and rounds
    // up (the census the fill rounding was fitted to).
    CHECK(eng.grid(228.765) == 228.76);
    CHECK(eng.grid(214.385) == 214.39);
    // No tick, no quantization.
    eng.syminfo_mintick_ = 0.0;
    CHECK(eng.grid(13.745) == 13.745);
    // A binary tick (1/128) has an integral inverse and takes the k / 128
    // branch: 13.7451 / 0.0078125 = 1759.37 -> k = 1759.
    eng.syminfo_mintick_ = 0.0078125;
    CHECK(eng.grid(13.7451) == 1759.0 * 0.0078125);
    // A tick whose inverse is not integral (2.5 -> 0.4) falls back to
    // k * mintick: 13.7451 / 2.5 = 5.498 -> k = 5.
    eng.syminfo_mintick_ = 2.5;
    CHECK(eng.grid(13.7451) == 5.0 * 2.5);
    CHECK(eng.grid(13.75) == 6.0 * 2.5);        // 5.5 exact -> half up -> 15
}

// --- (a) long sell-stop -----------------------------------------------------
void test_long_sell_stop_skips_subtick_low() {
    std::printf("-- long sell-stop: 13.745 low -> 13.75 is not a touch (xs-L-*) --\n");
    // bars: 0 Jan27 (signal) 1 Jan28 (fill 13.88) 2 Jan29 3 Jan30 4 Feb02 5 Feb03
    for (double stop : {13.74624, 13.7451, 13.7449, 13.745}) {
        Probe eng;
        eng.script = "L.....";
        eng.exit_stop_ = stop;
        auto bars = series({kJan27, kJan28, kJan29, kJan30, kFeb02, kFeb03});
        eng.run(bars.data(), (int)bars.size());
        // pre-fix: 13.74624 / 13.7451 exited on Feb02 (bar 4) — the
        // jayentriken NYSE:F trade-1 defect.
        expect_single_trade(eng, true, 1, 13.88, 5, 13.74);
    }
}

void test_long_sell_stop_fires_on_rounded_down_low() {
    std::printf("-- long sell-stop 13.3449: 13.3448 low -> 13.34 IS a touch (xs-L-133449) --\n");
    // bars: 0 Jan21 (signal) 1 Jan22 (fill 13.78) 2 Jan23 3 Jan26 (low 13.3448)
    Probe eng;
    eng.script = "L...";
    eng.exit_stop_ = 13.3449;
    auto bars = series({kJan21, kJan22, kJan23, kJan26});
    eng.run(bars.data(), (int)bars.size());
    expect_single_trade(eng, true, 1, 13.78, 3, 13.34);
}

void test_long_sell_stop_open_is_quantized() {
    std::printf("-- long sell-stop 13.776: 13.775 open -> 13.78 is no gap (xs-L-13776-open) --\n");
    // bars: 0 Feb18 (signal) 1 Feb19 (fill 13.77) 2 Feb20 (open 13.775, low 13.72)
    Probe eng;
    eng.script = "L..";
    eng.exit_stop_ = 13.776;
    auto bars = series({kFeb18, kFeb19, kFeb20});
    eng.run(bars.data(), (int)bars.size());
    // Level fill 13.77 (floor of 13.776), not the gap fill at the open 13.78.
    expect_single_trade(eng, true, 1, 13.77, 2, 13.77);
}

// --- (b) short buy-stop -----------------------------------------------------
void test_short_buy_stop_fires_on_rounded_up_high() {
    std::printf("-- short buy-stop: 14.0351 high -> 14.04 touches 14.0352 (xs-S-*) --\n");
    // bars: 0 Jan29 (signal) 1 Jan30 (fill 13.91) 2 Feb02 3 Feb03 (high 14.0351)
    for (double stop : {14.0349, 14.03505, 14.0352}) {
        Probe eng;
        eng.script = "S...";
        eng.exit_stop_ = stop;
        auto bars = series({kJan29, kJan30, kFeb02, kFeb03});
        eng.run(bars.data(), (int)bars.size());
        // pre-fix: 14.0352 > 14.0351 never fired here.
        expect_single_trade(eng, false, 1, 13.91, 3, 14.04);
    }
    // The exact half-tick high 14.035 -> 14.04 touches 14.0351 too
    // (xs-S-140351): bars 0 Feb18 (signal) 1 Feb19 (fill 13.77) 2 Feb20.
    {
        Probe eng;
        eng.script = "S..";
        eng.exit_stop_ = 14.0351;
        auto bars = series({kFeb18, kFeb19, kFeb20});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 13.77, 2, 14.04);
    }
}

void test_short_buy_stop_high_rounds_nearest_not_up() {
    std::printf("-- short buy-stop 13.225: 13.2202 high -> 13.22 is no touch (xs-S-13225-high) --\n");
    // bars: 0 Dec05 (signal) 1 Dec08 (fill 13.07) 2 Dec09 (high 13.2202) 3 Dec10
    Probe eng;
    eng.script = "S...";
    eng.exit_stop_ = 13.225;
    auto bars = series({kDec05, kDec08, kDec09, kDec10});
    eng.run(bars.data(), (int)bars.size());
    expect_single_trade(eng, false, 1, 13.07, 3, 13.23);
}

// --- (c) strategy.exit(limit=) ---------------------------------------------
void test_exit_limits() {
    std::printf("-- exit limits: sell-limit on the 14.0351 high, buy-limit on the 13.745 / 13.3448 lows (xl-*) --\n");
    for (double limit : {14.0349, 14.03505, 14.0352}) {
        Probe eng;
        eng.script = "L...";
        eng.exit_limit_ = limit;
        auto bars = series({kJan29, kJan30, kFeb02, kFeb03});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 1, 13.91, 3, 14.04);
    }
    for (double limit : {13.7451, 13.7449}) {
        Probe eng;
        eng.script = "S.....";
        eng.exit_limit_ = limit;
        auto bars = series({kJan27, kJan28, kJan29, kJan30, kFeb02, kFeb03});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 13.88, 5, 13.74);
    }
    {
        Probe eng;
        eng.script = "S...";
        eng.exit_limit_ = 13.3449;
        auto bars = series({kJan21, kJan22, kJan23, kJan26});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 1, 13.78, 3, 13.34);
    }
}

// --- (d) stop / limit entries ----------------------------------------------
// bars: 0 Jan30 (placed) 1 Feb02 2 Feb03 3 Feb04 (close_all) 4 Feb05 (exit 13.75)
void test_stop_entries() {
    std::printf("-- entry stops: long on the 14.0351 high, short on the 13.745 low (es-*, eo-*) --\n");
    for (double level : {14.0349, 14.03505, 14.0352}) {
        for (char kind : {'e', 'o'}) {
            Probe eng;
            eng.script = std::string(1, kind) + "..C.";
            eng.entry_long_ = true;
            eng.entry_level_ = level;
            auto bars = series({kJan30, kFeb02, kFeb03, kFeb04, kFeb05});
            eng.run(bars.data(), (int)bars.size());
            expect_single_trade(eng, true, 2, 14.04, 4, 13.75);
        }
    }
    for (double level : {13.7451, 13.7449}) {
        for (char kind : {'e', 'o'}) {
            Probe eng;
            eng.script = std::string(1, kind) + "..C.";
            eng.entry_long_ = false;
            eng.entry_level_ = level;
            auto bars = series({kJan30, kFeb02, kFeb03, kFeb04, kFeb05});
            eng.run(bars.data(), (int)bars.size());
            // pre-fix: 13.7451 filled on Feb02 (bar 1).
            expect_single_trade(eng, false, 2, 13.74, 4, 13.75);
        }
    }
}

void test_limit_entries() {
    std::printf("-- entry limits: long on the 13.745 low, short on the 14.0351 high (el-*) --\n");
    for (double level : {13.7451, 13.7449}) {
        Probe eng;
        eng.script = "m..C.";
        eng.entry_long_ = true;
        eng.entry_level_ = level;
        auto bars = series({kJan30, kFeb02, kFeb03, kFeb04, kFeb05});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, true, 2, 13.74, 4, 13.75);
    }
    for (double level : {14.03505, 14.0352}) {
        Probe eng;
        eng.script = "m..C.";
        eng.entry_long_ = false;
        eng.entry_level_ = level;
        auto bars = series({kJan30, kFeb02, kFeb03, kFeb04, kFeb05});
        eng.run(bars.data(), (int)bars.size());
        expect_single_trade(eng, false, 2, 14.04, 4, 13.75);
    }
}

// --- trail: not quantized (raw extremes, unchanged) -------------------------
void test_trail_keeps_raw_path() {
    std::printf("-- trail 20/3 over the 14.035 high: exits at the next open 13.98 (xt-L-trail) --\n");
    // bars: 0 Feb18 (signal) 1 Feb19 (fill 13.77) 2 Feb20 (high 14.035) 3 Feb23 (open 13.98)
    Probe eng;
    eng.script = "L...";
    eng.trail_points_ = 20.0;
    eng.trail_offset_ = 3.0;
    auto bars = series({kFeb18, kFeb19, kFeb20, kFeb23});
    eng.run(bars.data(), (int)bars.size());
    // raw best 14.035 - 0.03 = 14.005 > close 14.01 on Feb20: no fill; the
    // Feb23 open 13.98 gaps through -> 13.98. (A quantized best 14.04 would
    // have filled Feb20 @14.01, which TradingView does not do.)
    expect_single_trade(eng, true, 1, 13.77, 3, 13.98);
}

// --- leg order: the raw bar's, on every path coordinate ---------------------
void test_leg_order_is_the_raw_bars() {
    std::printf("-- leg order: quantization flips the O->H / O->L proximity tie, the cursor stays in the raw bar's order --\n");
    // Raw bar: O 13.7749 H 13.7846 L 13.7649 C 13.775 — |H-O| 0.0097 <
    // |O-L| 0.0100, so the raw path is O -> H -> L -> C (high first).
    // Tick twin: O 13.77 H 13.78 L 13.76 C 13.78 — a 0.01 / 0.01 tie, which
    // bar_path_uses_high_first resolves LOW first. A long stop entry at
    // 13.7751 fires on the raw first leg (tick high 13.78 >= 13.7751, tick
    // open 13.77 is no gap) and fills at ceil -> 13.78; its same-bar bracket
    // stop 13.766 then sits on the H -> L leg (tick 13.78 -> 13.76) and
    // fills at floor -> 13.76 on the SAME bar. Had the entry's path cursor
    // been taken in the twin's own (low-first) order it would read 1.755 —
    // past the H waypoint of the raw walk — and the bracket would miss the
    // H -> L leg entirely (no exit this bar), a regression the pre-round-6
    // raw walk never had.
    const Bar flip = mk_bar(0, 13.7749, 13.7846, 13.7649, 13.775);
    Probe probe;
    CHECK(probe.grid(flip.open) == 13.77);
    CHECK(probe.grid(flip.high) == 13.78);
    CHECK(probe.grid(flip.low) == 13.76);
    CHECK(probe.grid(flip.close) == 13.78);

    const Bar placement = mk_bar(0, 13.70, 13.75, 13.65, 13.72);
    const Bar after = mk_bar(0, 13.80, 13.85, 13.79, 13.84);
    Probe eng;
    eng.script = "e..";
    eng.entry_long_ = true;
    eng.entry_level_ = 13.7751;
    eng.exit_stop_ = 13.766;
    eng.arm_exit_flat_ = true;
    auto bars = series({placement, flip, after});
    eng.run(bars.data(), (int)bars.size());
    expect_single_trade(eng, true, 1, 13.78, 1, 13.76);
}

// --- control: the quantization is the tick's doing --------------------------
void test_no_tick_no_quantization() {
    std::printf("-- control: mintick 0 keeps the raw compare (13.7451 fires on the 13.745 low) --\n");
    Probe eng;
    eng.syminfo_mintick_ = 0.0;
    eng.script = "L.....";
    eng.exit_stop_ = 13.7451;
    auto bars = series({kJan27, kJan28, kJan29, kJan30, kFeb02, kFeb03});
    eng.run(bars.data(), (int)bars.size());
    // No tick: raw compare AND raw fill (no snap either).
    expect_single_trade(eng, true, 1, 13.88, 4, 13.7451);
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
