/*
 * test_trail_ref_entry_bar_extreme.cpp — round 9 family Z: a trailing exit's
 * running extreme is the position's, from its entry fill on, and a re-issued
 * strategy.exit never restarts it from the issuing bar's close.
 *
 * shurben5-tradingview-bot-goat on BINANCE:ETHUSDT.P 15m (registry feed
 * 27b62431096e; `lab bars`): strategy.exit("TP1 Short", from_entry="Short",
 * qty_percent=50, profit=100t) + strategy.exit("Exit Short", from_entry=
 * "Short", profit=300t, loss=100t, trail_points=100t, trail_offset=50t),
 * both re-issued on EVERY bar (the script calls all four exits
 * unconditionally), percent_of_equity 100, mintick 0.01, qty step 0.0001.
 *
 * TV #663/#664 (2025-12-25 07:15Z, short @2940.36 from flat): the entry bar
 * O 2940.36 H 2940.36 L 2938.71 C 2938.84 fills TP1 @2939.36, arms the trail
 * at that same level and runs its extreme down to the bar's LOW 2938.71; the
 * 07:30Z bar (O 2938.85 H 2939.7) fills "Trail Short" @2939.21 = 2938.71 +
 * 50t. The engine printed 2939.34 = 2938.84 + 50t: the entry bar's CLOSE.
 * TV #943/#944 (2026-04-24 22:15Z, short @2313.82 from flat): entry bar
 * O 2313.82 H 2314.23 L 2311.53 C 2311.85, TP1 @2312.82; the 22:30Z bar
 * (O 2311.85 H 2312.13 L 2305.2) fills "Trail Short" @2312.03 = 2311.53 +
 * 50t on its opening rise. The engine trailed from the close (2312.35, never
 * touched by the 2312.13 high) and rode the bar down to TP2 @2310.82.
 *
 * One rule explains both: TradingView's trailing extreme is the position's
 * best price since the entry fill, walked along every bar's intrabar path,
 * and a strategy.exit re-issued for a from_entry that is already filled
 * MODIFIES the resting order — it does not restart the extreme. The engine
 * restarted it from the issuing bar's close whenever the re-issue found no
 * resting exit under its (id, from_entry): here the flat-armed legs were
 * reconciled at the fill and the TP1 sibling's same-bar fill left the
 * "Exit Short" re-issue with no pending twin to inherit from.
 *
 * Long twins mirror the shape on the same feed (see the tapes named in the
 * family-Z ledger notes; the run function below takes either side).
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "../src/engine_internal.hpp"

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
        const double _a = (a), _b = (b);                                       \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %s  (%.6f vs %.6f)\n", __FILE__, \
                        __LINE__, #a, #b, _a, _b);                             \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b{};
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// BINANCE:ETHUSDT.P 15m, feed 27b62431096e (UTC labels).
const Bar kEth1225_0645 = mk(2939.87, 2942.61, 2939.86, 2941.94, 1766645100000);
const Bar kEth1225_0700 = mk(2941.94, 2942.5, 2940.1, 2940.35, 1766646000000);
const Bar kEth1225_0715 = mk(2940.36, 2940.36, 2938.71, 2938.84, 1766646900000);
const Bar kEth1225_0730 = mk(2938.85, 2939.7, 2937.72, 2938.95, 1766647800000);
const Bar kEth1225_0745 = mk(2938.94, 2941.92, 2938.94, 2940.83, 1766648700000);

const Bar kEth0424_2145 = mk(2313.76, 2316.68, 2312.76, 2313.69, 1777068300000);
const Bar kEth0424_2200 = mk(2313.69, 2314.86, 2312.29, 2313.82, 1777069200000);
const Bar kEth0424_2215 = mk(2313.82, 2314.23, 2311.53, 2311.85, 1777070100000);
const Bar kEth0424_2230 = mk(2311.85, 2312.13, 2305.2, 2308.7, 1777071000000);
const Bar kEth0424_2245 = mk(2308.7, 2311.46, 2308.7, 2310.73, 1777071900000);

// Long twins (`lab tv` tapes famz-trail-L-20251225-{A,B}, -L-20260214-{A,B},
// -L-20260425-{A,B}; A = the probe's four every-bar exits, B = own side only;
// every pair byte-identical in its rows).
const Bar kEth1225_1230 = mk(2926.65, 2928.83, 2924.31, 2925.02, 1766665800000);
const Bar kEth1225_1245 = mk(2925.01, 2926.92, 2924.23, 2926.6, 1766666700000);
const Bar kEth1225_1300 = mk(2926.61, 2927.0, 2923.33, 2924.85, 1766667600000);
const Bar kEth1225_1315 = mk(2924.84, 2925.47, 2918.52, 2922.95, 1766668500000);

const Bar kEth0214_2030 = mk(2083.98, 2087.56, 2082.67, 2086.62, 1771101000000);
const Bar kEth0214_2045 = mk(2086.63, 2088.95, 2086.12, 2088.56, 1771101900000);
const Bar kEth0214_2100 = mk(2088.56, 2098.94, 2088.34, 2094.03, 1771102800000);
const Bar kEth0214_2115 = mk(2094.02, 2096.06, 2083.11, 2083.46, 1771103700000);

const Bar kEth0425_0030 = mk(2310.79, 2313.56, 2310.0, 2312.53, 1777077000000);
const Bar kEth0425_0045 = mk(2312.53, 2314.94, 2312.53, 2314.86, 1777077900000);
const Bar kEth0425_0100 = mk(2314.87, 2317.5, 2314.29, 2316.9, 1777078800000);
const Bar kEth0425_0115 = mk(2316.9, 2319.53, 2316.14, 2316.66, 1777079700000);

// One-shot (omitted-offset) trail re-issued with a NEW activation the
// carried extreme already exceeds (`lab tv` famz-oneshot-A1 / -B1; the
// fast-scalper shape: strategy.exit(stop=close*0.98, trail_points=...)).
const Bar kEth1001_2000 = mk(4332.23, 4335.35, 4314.99, 4321.59, 1759348800000);
const Bar kEth1001_2015 = mk(4321.6, 4331.86, 4310.81, 4325.94, 1759349700000);
const Bar kEth1001_2030 = mk(4325.94, 4327.2, 4323.0, 4326.18, 1759350600000);
const Bar kEth1001_2045 = mk(4326.19, 4333.0, 4323.0, 4332.99, 1759351500000);
const Bar kEth1001_2100 = mk(4333.0, 4333.47, 4321.0, 4323.54, 1759352400000);
const Bar kEth1001_2115 = mk(4323.54, 4323.55, 4313.49, 4317.58, 1759353300000);
const Bar kEth1001_2130 = mk(4317.58, 4317.58, 4297.91, 4307.73, 1759354200000);
const Bar kEth1001_2145 = mk(4307.73, 4313.2, 4284.55, 4302.52, 1759355100000);
const Bar kEth1001_2200 = mk(4302.52, 4327.45, 4302.52, 4316.88, 1759356000000);

const Bar kEth1001_1045 = mk(4287.0, 4289.91, 4277.31, 4280.4, 1759315500000);
const Bar kEth1001_1100 = mk(4280.4, 4295.28, 4280.4, 4295.19, 1759316400000);
const Bar kEth1001_1115 = mk(4295.18, 4303.26, 4293.78, 4296.59, 1759317300000);
const Bar kEth1001_1130 = mk(4296.6, 4300.52, 4289.16, 4292.03, 1759318200000);
const Bar kEth1001_1145 = mk(4292.01, 4298.06, 4290.28, 4295.88, 1759319100000);
const Bar kEth1001_1200 = mk(4295.88, 4301.18, 4289.24, 4295.18, 1759320000000);
const Bar kEth1001_1215 = mk(4295.18, 4312.34, 4287.0, 4306.58, 1759320900000);

// The probe's broker: 10x margin both sides, all-in percent_of_equity,
// 0.0001 lots, mintick 0.01, no commission, market fills at the next open.
class Goat : public pineforge::source::PineStrategyHost {
public:
    explicit Goat(double capital) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0001;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 10.0;
        margin_short_ = 10.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }
    int signal_bar = -1;
    bool signal_long = false;
    // 0: the probe's fixed request; 1: trail_points alternates 100/101t per
    // bar (tapes famz-trail-*-D); 2: trail_offset alternates 50/51t (-E).
    int alternate = 0;

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == signal_bar) {
            if (signal_long) {
                strategy_close("Short", "Flip to Long");
                strategy_entry("Long", true);
            } else {
                strategy_close("Long", "Flip to Short");
                strategy_entry("Short", false);
            }
        }
        // SL $1, TP1 $1 (half), TP2 $3, trail 100t / 50t — every bar.
        const double pts = alternate == 1 ? 100.0 + (bar_index_ % 2) : 100.0;
        const double off = alternate == 2 ? 50.0 + (bar_index_ % 2) : 50.0;
        strategy_exit("TP1 Long", "Long", kNaN, kNaN, kNaN, kNaN, kNaN, 50.0,
                      "", kNaN, "", /*profit_ticks=*/100.0, kNaN);
        strategy_exit("Exit Long", "Long", kNaN, kNaN, /*trail_points=*/pts,
                      /*trail_offset=*/off, kNaN, 100.0, "", kNaN, "",
                      /*profit_ticks=*/300.0, /*loss_ticks=*/100.0);
        strategy_exit("TP1 Short", "Short", kNaN, kNaN, kNaN, kNaN, kNaN, 50.0,
                      "", kNaN, "", /*profit_ticks=*/100.0, kNaN);
        strategy_exit("Exit Short", "Short", kNaN, kNaN, /*trail_points=*/pts,
                      /*trail_offset=*/off, kNaN, 100.0, "", kNaN, "",
                      /*profit_ticks=*/300.0, /*loss_ticks=*/100.0);
    }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
};

void print_trades(const Goat& p) {
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        std::printf("      trade %d: %s entry bar %d @ %.4f qty %.4f exit bar %d @ %.4f pnl %.4f [%s|%s]\n",
                    i, t.is_long ? "long" : "short", t.entry_bar_index,
                    t.entry_price, t.qty, t.exit_bar_index, t.exit_price,
                    t.pnl, t.exit_comment.c_str(), t.exit_id.c_str());
    }
}

void test_short_1225_trails_from_entry_bar_low() {
    std::printf("test_short_1225_trails_from_entry_bar_low\n");
    const std::vector<Bar> bars = {
        kEth1225_0645, kEth1225_0700, kEth1225_0715, kEth1225_0730, kEth1225_0745,
    };
    Goat p(10000.0);
    p.signal_bar = 1;
    p.signal_long = false;
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    CHECK(p.flat());
    if (p.trade_count() == 2) {
        const Trade& tp1 = p.get_trade(0);
        CHECK(!tp1.is_long);
        CHECK(tp1.entry_bar_index == 2);
        CHECK_NEAR(tp1.entry_price, 2940.36, 1e-9);
        CHECK(tp1.exit_bar_index == 2);
        CHECK_NEAR(tp1.exit_price, 2939.36, 1e-9);   // TP1 Short
        const Trade& trail = p.get_trade(1);
        CHECK(!trail.is_long);
        CHECK(trail.exit_bar_index == 3);
        CHECK_NEAR(trail.exit_price, 2939.21, 1e-9); // TV: low 2938.71 + 50t
    }
}

void test_short_0424_trail_fires_before_tp2() {
    std::printf("test_short_0424_trail_fires_before_tp2\n");
    const std::vector<Bar> bars = {
        kEth0424_2145, kEth0424_2200, kEth0424_2215, kEth0424_2230, kEth0424_2245,
    };
    Goat p(10000.0);
    p.signal_bar = 1;
    p.signal_long = false;
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    CHECK(p.flat());
    if (p.trade_count() == 2) {
        const Trade& tp1 = p.get_trade(0);
        CHECK(tp1.entry_bar_index == 2);
        CHECK_NEAR(tp1.entry_price, 2313.82, 1e-9);
        CHECK(tp1.exit_bar_index == 2);
        CHECK_NEAR(tp1.exit_price, 2312.82, 1e-9);   // TP1 Short
        const Trade& trail = p.get_trade(1);
        CHECK(trail.exit_bar_index == 3);
        CHECK_NEAR(trail.exit_price, 2312.03, 1e-9); // TV: low 2311.53 + 50t, on the opening rise
    }
}

// One long twin per shape. Entry at the 12:45Z open 2925.01, TP1 @2926.01 on
// the entry bar (high 2926.92, close 2926.6); the 13:00Z bar opens 2926.61,
// rises first to 2927.0 (|H-O| 0.39 < |L-O| 3.28), and the trail — its
// extreme now 2927.0 — fills on the fall @2926.50 (TV). A close-restarted
// extreme (2926.6 -> 2926.10) or one frozen at the entry bar's high
// (2926.42) both print another price.
void test_long_1225_extreme_walks_the_exit_bar_path() {
    std::printf("test_long_1225_extreme_walks_the_exit_bar_path\n");
    const std::vector<Bar> bars = {
        kEth1225_1230, kEth1225_1245, kEth1225_1300, kEth1225_1315,
    };
    Goat p(10000.0);
    p.signal_bar = 0;
    p.signal_long = true;
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    CHECK(p.flat());
    if (p.trade_count() == 2) {
        const Trade& tp1 = p.get_trade(0);
        CHECK(tp1.is_long);
        CHECK(tp1.entry_bar_index == 1);
        CHECK_NEAR(tp1.entry_price, 2925.01, 1e-9);
        CHECK(tp1.exit_bar_index == 1);
        CHECK_NEAR(tp1.exit_price, 2926.01, 1e-9);   // TP1 Long
        const Trade& trail = p.get_trade(1);
        CHECK(trail.exit_bar_index == 2);
        CHECK_NEAR(trail.exit_price, 2926.50, 1e-9); // TV: exit-bar high 2927.0 - 50t
    }
}

// The TP2-vs-trail mirror of the 04-24 short: entry 20:45Z @2086.63, TP1
// @2087.63, entry-bar high 2088.95 / close 2088.56; the 21:00Z bar opens
// 2088.56, dips first to 2088.34 (|L-O| 0.22 < |H-O| 10.38) through the
// trail 2088.45 = 2088.95 - 50t (TV), then runs to 2098.94. The restarted
// extreme (2088.56 -> 2088.06) is never touched and the engine rode the
// bar up to TP2 @2089.63.
void test_long_0214_trail_fires_before_tp2() {
    std::printf("test_long_0214_trail_fires_before_tp2\n");
    const std::vector<Bar> bars = {
        kEth0214_2030, kEth0214_2045, kEth0214_2100, kEth0214_2115,
    };
    Goat p(10000.0);
    p.signal_bar = 0;
    p.signal_long = true;
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    CHECK(p.flat());
    if (p.trade_count() == 2) {
        const Trade& tp1 = p.get_trade(0);
        CHECK(tp1.exit_bar_index == 1);
        CHECK_NEAR(tp1.exit_price, 2087.63, 1e-9);   // TP1 Long
        const Trade& trail = p.get_trade(1);
        CHECK(trail.exit_bar_index == 2);
        CHECK_NEAR(trail.exit_price, 2088.45, 1e-9); // TV: entry-bar high 2088.95 - 50t
    }
}

// Entry 00:45Z @2312.53, TP1 @2313.53, entry-bar high 2314.94 / close
// 2314.86; the 01:00Z bar opens 2314.87 and dips first to 2314.29
// (|L-O| 0.58 < |H-O| 2.63): TV fills @2314.44 = 2314.94 - 50t; the
// restarted extreme printed 2314.36.
void test_long_0425_trails_from_entry_bar_high() {
    std::printf("test_long_0425_trails_from_entry_bar_high\n");
    const std::vector<Bar> bars = {
        kEth0425_0030, kEth0425_0045, kEth0425_0100, kEth0425_0115,
    };
    Goat p(10000.0);
    p.signal_bar = 0;
    p.signal_long = true;
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    CHECK(p.flat());
    if (p.trade_count() == 2) {
        const Trade& tp1 = p.get_trade(0);
        CHECK(tp1.exit_bar_index == 1);
        CHECK_NEAR(tp1.exit_price, 2313.53, 1e-9);   // TP1 Long
        const Trade& trail = p.get_trade(1);
        CHECK(trail.exit_bar_index == 2);
        CHECK_NEAR(trail.exit_price, 2314.44, 1e-9); // TV: entry-bar high 2314.94 - 50t
    }
}

// A re-issue that moves the activation (trail_points 100 -> 101t at the
// entry bar's close; the tape's bar_index parity puts 101t on the 07:15Z /
// 00:45Z close) restarts the extreme from that close: S 2939.34 = 2938.84 +
// 50t (tape famz-trail-S-20251225-D); L 2314.37 = the 01:00Z open 2314.87
// - 50t, the restarted extreme walking on into the next bar (tape
// famz-trail-L-20260425-D). A re-issue that changes only the offset keeps
// the extreme and applies the new distance: S 2939.22 = 2938.71 + 51t
// (-E), L 2314.43 = 2314.94 - 51t (-E).
void test_changed_points_restart_changed_offset_keeps() {
    std::printf("test_changed_points_restart_changed_offset_keeps\n");
    struct Case { bool is_long; int alternate; double expect; const char* tape; };
    const Case cases[] = {
        {false, 1, 2939.34, "famz-trail-S-20251225-D"},
        {false, 2, 2939.22, "famz-trail-S-20251225-E"},
        {true, 1, 2314.37, "famz-trail-L-20260425-D"},
        {true, 2, 2314.43, "famz-trail-L-20260425-E"},
    };
    // Synthetic parity padding only; retain every actual event bar below.
    Bar short_padding = kEth1225_0645;
    short_padding.timestamp -= 15 * 60 * 1000;
    for (const Case& c : cases) {
        // The tapes' parity: the entry bar's close carries bar_index odd.
        // Short: bars 06:45 (0), 07:00 signal (1), 07:15 entry (2) — pad one
        // bar in front so the entry bar is odd. Long: 00:30 signal (0),
        // 00:45 entry (1).
        std::vector<Bar> bars = c.is_long
            ? std::vector<Bar>{kEth0425_0030, kEth0425_0045, kEth0425_0100, kEth0425_0115}
            : std::vector<Bar>{short_padding, kEth1225_0645, kEth1225_0700, kEth1225_0715,
                               kEth1225_0730, kEth1225_0745};
        Goat p(10000.0);
        p.signal_bar = c.is_long ? 0 : 2;
        p.signal_long = c.is_long;
        p.alternate = c.alternate;
        p.run(bars.data(), (int)bars.size());
        std::printf("    %s\n", c.tape);
        print_trades(p);
        CHECK(p.last_error().empty());
        CHECK(p.trade_count() == 2);
        CHECK(p.flat());
        if (p.trade_count() == 2) {
            const Trade& trail = p.get_trade(1);
            CHECK(trail.exit_bar_index == (c.is_long ? 2 : 4));
            CHECK_NEAR(trail.exit_price, c.expect, 1e-9);
        }
    }
}

// The fast-scalper shape: a fixed-lot long with strategy.exit(stop=
// close*0.98, trail_points=1500t) issued with the entry, re-issued at bar R
// with trail_points=500t. The running high already exceeds entry + 5.00
// and R's close does not: TradingView requires the NEW activation to be
// reached after the re-issue (A1: entry 20:15Z @4321.6, re-issue 21:00Z
// close 4323.54, exit @4326.60 on the 22:00Z bar — not at the 21:15Z open
// 4323.54 the carried-best arming would print; B1: entry 11:15Z @4295.18,
// re-issue 11:45Z, exit @4300.18 = the level on the 12:00Z bar, not its
// open 4295.88).
class OneShot : public pineforge::source::PineStrategyHost {
public:
    OneShot() {
        initial_capital_ = 1'000'000.0;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
    }
    int signal_bar = -1;
    int reissue_bar = -1;
    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == signal_bar) {
            strategy_entry("L", true);
            strategy_exit("x", "L", kNaN, bar.close * 0.98, /*trail_points=*/1500.0);
        }
        if (bar_index_ == reissue_bar) {
            strategy_exit("x", "L", kNaN, bar.close * 0.98, /*trail_points=*/500.0);
        }
    }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
};

void test_one_shot_reissue_needs_its_new_activation() {
    std::printf("test_one_shot_reissue_needs_its_new_activation\n");
    struct Case { std::vector<Bar> bars; int signal; int reissue; int exit_bar; double exit; const char* tape; };
    const Case cases[] = {
        {{kEth1001_2000, kEth1001_2015, kEth1001_2030, kEth1001_2045, kEth1001_2100,
          kEth1001_2115, kEth1001_2130, kEth1001_2145, kEth1001_2200},
         0, 4, 8, 4326.60, "famz-oneshot-A1"},
        {{kEth1001_1045, kEth1001_1100, kEth1001_1115, kEth1001_1130, kEth1001_1145,
          kEth1001_1200, kEth1001_1215},
         1, 4, 5, 4300.18, "famz-oneshot-B1"},
    };
    for (const Case& c : cases) {
        OneShot p;
        p.signal_bar = c.signal;
        p.reissue_bar = c.reissue;
        p.run(c.bars.data(), (int)c.bars.size());
        std::printf("    %s\n", c.tape);
        for (int i = 0; i < p.trade_count(); ++i) {
            const Trade& t = p.get_trade(i);
            std::printf("      trade %d: entry bar %d @ %.2f exit bar %d @ %.2f [%s]\n", i,
                        t.entry_bar_index, t.entry_price, t.exit_bar_index, t.exit_price,
                        t.exit_id.c_str());
        }
        CHECK(p.last_error().empty());
        CHECK(p.trade_count() == 1);
        CHECK(p.flat());
        if (p.trade_count() == 1) {
            const Trade& t = p.get_trade(0);
            CHECK(t.entry_bar_index == c.signal + 1);
            CHECK(t.exit_bar_index == c.exit_bar);
            CHECK_NEAR(t.exit_price, c.exit, 1e-9);
        }
    }
}

}  // namespace

int main() {
    std::printf("--- trail_ref_entry_bar_extreme (round 9 family Z) ---\n");
    test_short_1225_trails_from_entry_bar_low();
    test_short_0424_trail_fires_before_tp2();
    test_long_1225_extreme_walks_the_exit_bar_path();
    test_long_0214_trail_fires_before_tp2();
    test_long_0425_trails_from_entry_bar_high();
    test_changed_points_restart_changed_offset_keeps();
    test_one_shot_reissue_needs_its_new_activation();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
