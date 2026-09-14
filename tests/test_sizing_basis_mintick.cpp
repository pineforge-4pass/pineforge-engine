/*
 * test_sizing_basis_mintick.cpp — TradingView's broker sizes DEFAULT (qty=na)
 * percent_of_equity / cash market orders on the MINTICK-ROUNDED signal close:
 *
 *   basis(S) = round_to_mintick(close(S))            (census form, no epsilon)
 *   qty      = floor_step( E / basis(S) ),  E marked at basis(S) as well
 *
 * Pine's own signal path (ta.*, crossovers) keeps reading the RAW feed close;
 * only the broker's sizing snapshot and the fill are on-tick. Evidence (the
 * replay this file pins): 674/674 NYSE:F and 832/832 NASDAQ:AAPL reversals of
 * taro-s-c-c-ma-simplified-2-color fit the rounded basis while the raw close
 * fits 476/674 on F; drgunjan-F trade 1 is TV qty 10460 = floor(100000/9.56)
 * on a 9.565 close where the raw divisor gave 10454; and the raw basis
 * DECLINED entries TV filled whenever an x.xx5 close rounded UP at the fill
 * (qty floored on the lower raw price times the higher rounded fill overshot
 * the sizing equity by ~qty*mintick/2 and tripped the true-flat gap-reject /
 * reversal float-guard arms): 463/463 missing taro-F entries predicted, 0
 * counterexamples; 26/26 drgunjan-F and 6/6 mazi-F missing entries sit on
 * sub-penny signal closes.
 *
 * Pins:
 *   A.  9.565 close, mintick 0.01, capital 100000, pct 100, next open 9.56:
 *       frozen qty == 10460 (drgunjan-F trade 1) and the entry FILLS.
 *   A2. 9.585 close (rounds UP to 9.59), next open 9.59: the entry FILLS with
 *       qty 10427. Pre-fix: qty 10432 floored on 9.585, 10432*9.59 = 100042.88
 *       > 100000 -> the true-flat zero-commission gap-reject arm DROPPED it on
 *       a zero-gap open (the 463/463 mechanism).
 *   A3. The reversal twin of A2: a held long, short signal on a 9.585 close,
 *       reversal fills at 9.59. Pre-fix the equity mark at the raw 9.585 left
 *       free_funds 100894.71 against a required 10526*9.59 = 100944.34 and the
 *       float-guard reversal arm DECLINED; on-tick the mark is 9.59, free
 *       funds 100947.34, admitted.
 *   B.  A penny close (9.56) is unchanged in every quantity and verdict:
 *       basis == close to within one ulp (double(0.01) is inexact, so
 *       round_to_mintick is not a bit-for-bit identity on decimal ticks —
 *       the ulp is absorbed by apply_qty_step's 1e-6 nudge and the
 *       admission float guards), qty 10460, fills.
 *   C.  mintick 0.25 (futures): a 5000.125 close sizes on 5000.25 (nearest,
 *       floor(x/tick + 0.5)); capital 100003 floors to 19 lots where the raw
 *       basis gave 20.
 *   D.  Slippage ticks are added AFTER rounding: 9.565 with slippage 2 sizes
 *       a buy on 9.58 and a sell on 9.54 (pre-fix 9.585 / 9.545).
 *   E1. SHORT margin-call cascade marks at the ROUNDED high (medium evidence:
 *       32 vs 0 reproduced slices on the F tape): a sub-tick excursion the
 *       on-tick ledger cannot see (high 100.004 on a short at liq 100.00)
 *       fires NO slice; pre-fix the raw mark produced a phantom one.
 *   E2. The slice quantity at a 105.005 high is the 105.01-marked 3.8167794
 *       (pre-fix 3.8131518 from the raw mark); the fill price was already
 *       105.01 both ways (bar_fill_price, finding-446).
 *   E3. The chronological copy of the cascade (margin_call_slice_before_
 *       priced_exit, the slice taken BEFORE a same-bar priced exit) marks at
 *       the same rounded high: E1's 100.004 excursion with a TP limit
 *       resting on the bar fires NO slice and the TP closes the full lot
 *       (pre-fix that path marked raw and fired a phantom 0.0016 slice only
 *       when an exit happened to be resting); the on-tick control high
 *       100.01 with the same TP slices 4*(10 - 999.9/100.01) @ 100.01 first
 *       and the TP closes the remainder, proving the path is live.
 *   F.  Sanity on the helper itself: round_to_mintick on the drgunjan close
 *       and idempotence on its own output.
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

namespace {

// Scripted stock-shaped probe: whole-share lots (qty_step 1), zero
// commission, 1x margin both sides, margin-call emulation OFF so the sizing
// basis and the fill-time admission arms are the only mechanisms in play.
// The feed is deliberately NOT on-tick — that is the point of the file.
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double capital, double mintick, int slippage_ticks) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        qty_step_ = 1.0;
        process_orders_on_close_ = false;
        slippage_ = slippage_ticks;
        set_syminfo_mintick(mintick);
        margin_call_enabled_ = false;
    }
    // 'L' = default-sized long entry, 'S' = default-sized short entry,
    // 'C' = close all, '.' = nothing. Every placement also records the
    // frozen basis / qty the broker snapshot took on that bar.
    std::string script;
    std::vector<double> basis_buy, basis_sell, frozen_qty;
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        const char a = script[bar_index_];
        if (a == 'L' || a == 'S') {
            basis_buy.push_back(frozen_sizing_price(/*is_buy=*/true));
            basis_sell.push_back(frozen_sizing_price(/*is_buy=*/false));
            frozen_qty.push_back(frozen_default_market_qty(a == 'L'));
        }
        switch (a) {
            case 'L': strategy_entry("L", true); break;
            case 'S': strategy_entry("S", false); break;
            case 'C': strategy_close_all(); break;
            default: break;
        }
    }
    double nearest(double p) const { return round_to_mintick(p); }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    const std::vector<Trade>& all_trades() const { return trades_; }
};

// A. drgunjan-F trade 1. The signal bar closes at 9.565; 9.565 / 0.01 lands
//    at 956.49999... in binary, so the census form rounds it DOWN to 9.56
//    (the same way 228.765 -> 228.76). TV sized floor(100000 / 9.56) =
//    10460; the raw divisor gave floor(100000 / 9.565) = 10454. The next bar
//    opens exactly on the rounded close, so the fill is 9.56 and the frozen
//    lot is affordable on both bases (10460 * 9.56 = 99997.60 <= 100000).
//
//    Pre-fix expectation (kept for the record): qty 10454 @ 9.56, filled.
//    This bar shape does not reach the decline arms — a DOWN-rounded close
//    leaves qty * fill below equity; A2/A3 cover the UP-rounded shape that
//    declined.
void test_sub_penny_close_rounds_down_sizes_on_tick() {
    std::printf("-- A: 9.565 close sizes on 9.56 -> 10460, fills --\n");
    Probe eng(100000.0, 0.01, 0);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 9.50, 9.60, 9.45, 9.565),   // S: basis 9.56, qty 10460
        mk_bar(2000, 9.56, 9.60, 9.50, 9.58),    // fill @ open 9.56
        mk_bar(3000, 9.58, 9.60, 9.55, 9.58),    // close_all
        mk_bar(4000, 9.58, 9.60, 9.55, 9.58),    // exit @ 9.58
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 9.56, 1e-12);
        CHECK_NEAR(eng.frozen_qty[0], 10460.0, 1e-9);   // pre-fix: 10454
    }
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        const Trade& t = eng.all_trades()[0];
        CHECK(t.is_long);
        CHECK_NEAR(t.entry_price, 9.56, 1e-9);
        CHECK_NEAR(t.qty, 10460.0, 1e-9);
        CHECK_NEAR(t.exit_price, 9.58, 1e-9);
    }
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// A2. The declining shape. 9.585 / 0.01 = 958.5000... rounds UP to 9.59.
//     On-tick: qty = floor(100000 / 9.59) = 10427, fill @ 9.59 ->
//     10427 * 9.59 = 99994.93 <= 100000, admitted (flat arm prices at the
//     sizing notional; gap-reject sees a zero gap).
//     Pre-fix: qty = floor(100000 / 9.585) = 10432 and the fill rounded to
//     9.59 -> 10432 * 9.59 = 100042.88 > 100000 + float_guard, so the
//     true-flat zero-commission gap-reject arm silently DROPPED the entry
//     (position FLAT, no trade row) although the open did not gap at all.
void test_sub_penny_close_rounds_up_fills_instead_of_gap_reject() {
    std::printf("-- A2: 9.585 close (-> 9.59) fills, no phantom gap-reject --\n");
    Probe eng(100000.0, 0.01, 0);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 9.50, 9.60, 9.45, 9.585),   // S: basis 9.59, qty 10427
        mk_bar(2000, 9.59, 9.62, 9.55, 9.60),    // fill @ open 9.59 (no gap)
        mk_bar(3000, 9.60, 9.62, 9.58, 9.60),
        mk_bar(4000, 9.60, 9.62, 9.58, 9.60),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 9.59, 1e-12);
        CHECK_NEAR(eng.frozen_qty[0], 10427.0, 1e-9);   // pre-fix: 10432
    }
    CHECK(eng.trade_count() == 1);                      // pre-fix: 0 (dropped)
    if (eng.trade_count() == 1) {
        const Trade& t = eng.all_trades()[0];
        CHECK(t.is_long);
        CHECK_NEAR(t.entry_price, 9.59, 1e-9);
        CHECK_NEAR(t.qty, 10427.0, 1e-9);
    }
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// A3. Reversal twin (design-reversal-admission-float-guard arm).
//   bar0  9.50/9.55/9.45/9.50   on_bar: L — basis 9.50, qty floor(100000/9.5)
//                               = 10526
//   bar1  9.50/9.60/9.45/9.585  long fills @ 9.50 x 10526. on_bar: S — the
//                               SIGNAL bar. On-tick: E = 100000 +
//                               (9.59 - 9.50) * 10526 = 100947.34, qty =
//                               floor(100947.34 / 9.59) = 10526.
//   bar2  9.59/9.62/9.55/9.60   reversal @ 9.59: long closes (+947.34),
//                               short admission on the float-guard arm:
//                               required 10526 * 9.59 = 100944.34 <=
//                               free_funds 100947.34 -> ADMITTED.
//                               Pre-fix: E marked at the raw 9.585 =
//                               100894.71 (same qty 10526), required
//                               100944.34 > 100894.71 + 1e-7 -> DECLINED,
//                               the close leg suppressed, the long held.
//   bar3  close_all; bar4 exit.
void test_sub_penny_reversal_admitted_on_float_guard_arm() {
    std::printf("-- A3: sub-penny reversal admitted on the float-guard arm --\n");
    Probe eng(100000.0, 0.01, 0);
    eng.script = "LS.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 9.50, 9.55, 9.45, 9.50),
        mk_bar(2000, 9.50, 9.60, 9.45, 9.585),
        mk_bar(3000, 9.59, 9.62, 9.55, 9.60),
        mk_bar(4000, 9.60, 9.62, 9.58, 9.60),
        mk_bar(5000, 9.60, 9.62, 9.58, 9.60),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_sell.size() == 2);
    if (eng.basis_sell.size() == 2) {
        CHECK_NEAR(eng.basis_sell[1], 9.59, 1e-12);
        CHECK_NEAR(eng.frozen_qty[1], 10526.0, 1e-9);
    }
    CHECK(eng.trade_count() == 2);                      // pre-fix: 1 (declined)
    if (eng.trade_count() == 2) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK(t0.is_long);
        CHECK_NEAR(t0.entry_price, 9.50, 1e-9);
        CHECK_NEAR(t0.qty, 10526.0, 1e-9);
        CHECK_NEAR(t0.exit_price, 9.59, 1e-9);
        const Trade& t1 = eng.all_trades()[1];
        CHECK(!t1.is_long);
        CHECK_NEAR(t1.entry_price, 9.59, 1e-9);
        CHECK_NEAR(t1.qty, 10526.0, 1e-9);
        CHECK_NEAR(t1.exit_price, 9.60, 1e-9);
    }
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// B. A penny close is unchanged: the basis IS the close (to within one ulp —
//    double(0.01) is inexact, so round_to_mintick is not bit-for-bit on a
//    decimal tick; the 1e-12 tolerance below is the honest statement) and
//    the quantity is the pre-fix number.
void test_penny_close_unchanged() {
    std::printf("-- B: penny close unchanged --\n");
    Probe eng(100000.0, 0.01, 0);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 9.50, 9.60, 9.45, 9.56),
        mk_bar(2000, 9.56, 9.60, 9.50, 9.58),
        mk_bar(3000, 9.58, 9.60, 9.55, 9.58),
        mk_bar(4000, 9.58, 9.60, 9.55, 9.58),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 9.56, 1e-12);
        CHECK_NEAR(eng.basis_sell[0], 9.56, 1e-12);
        CHECK_NEAR(eng.frozen_qty[0], 10460.0, 1e-9);
    }
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK_NEAR(eng.all_trades()[0].qty, 10460.0, 1e-9);
        CHECK_NEAR(eng.all_trades()[0].entry_price, 9.56, 1e-9);
    }
}

// C. Futures tick 0.25: 5000.125 / 0.25 = 20000.5 -> floor(20001.0) ->
//    5000.25 (nearest, up on this exact binary midpoint). Capital 100003
//    discriminates the divisor: floor(100003 / 5000.25) = 19 on-tick,
//    floor(100003 / 5000.125) = 20 on the raw close.
void test_quarter_tick_basis() {
    std::printf("-- C: mintick 0.25 sizes on 5000.25 --\n");
    Probe eng(100003.0, 0.25, 0);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 4999.00, 5001.00, 4998.00, 5000.125),
        mk_bar(2000, 5000.25, 5002.00, 4999.00, 5001.00),
        mk_bar(3000, 5001.00, 5002.00, 4999.00, 5001.00),
        mk_bar(4000, 5001.00, 5002.00, 4999.00, 5001.00),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK_NEAR(eng.nearest(5000.125), 5000.25, 1e-9);
    CHECK_NEAR(eng.nearest(4999.875), 5000.00, 1e-9);
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 5000.25, 1e-9);
        CHECK_NEAR(eng.frozen_qty[0], 19.0, 1e-9);      // pre-fix: 20
    }
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK_NEAR(eng.all_trades()[0].qty, 19.0, 1e-9);
        CHECK_NEAR(eng.all_trades()[0].entry_price, 5000.25, 1e-9);
    }
}

// D. Slippage ticks ride on the ROUNDED basis: 9.565 -> 9.56, then +/-2
//    ticks -> 9.58 (buy) / 9.54 (sell). Pre-fix: 9.585 / 9.545. The frozen
//    buy qty is floor(100000 / 9.58) = 10438 and the slipped fill at the
//    9.56 open is 9.58, so the lot is affordable and fills.
void test_slippage_added_after_rounding() {
    std::printf("-- D: slippage ticks added after rounding --\n");
    Probe eng(100000.0, 0.01, 2);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 9.50, 9.60, 9.45, 9.565),
        mk_bar(2000, 9.56, 9.60, 9.50, 9.58),
        mk_bar(3000, 9.58, 9.60, 9.55, 9.58),
        mk_bar(4000, 9.58, 9.60, 9.55, 9.58),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 9.58, 1e-12);      // pre-fix: 9.585
        CHECK_NEAR(eng.basis_sell[0], 9.54, 1e-12);     // pre-fix: 9.545
        CHECK_NEAR(eng.frozen_qty[0], 10438.0, 1e-9);   // pre-fix: 10432
    }
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK_NEAR(eng.all_trades()[0].entry_price, 9.58, 1e-9);
        CHECK_NEAR(eng.all_trades()[0].qty, 10438.0, 1e-9);
    }
}

// Short-cascade probe, the test_margin_call.cpp "A" shape: 1000 capital, 100%
// short at 1x fills on the bar-0 close (POC), never exits; the adverse
// cascade is the only mechanism (continuous lots, qty_step 0).
class ShortCascadeProbe : public pineforge::source::PineStrategyHost {
public:
    ShortCascadeProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        qty_step_ = 0.0;
        set_syminfo_mintick(0.01);
    }
    // E3: a take-profit limit armed on bar 1 (position live) so it RESTS on
    // bar 2 and routes the deficit test through the chronological hook.
    double tp_limit = kNaN;
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("S", false, kNaN, kNaN, kNaN);
        if (bar_index_ == 1 && std::isfinite(tp_limit)) {
            strategy_exit("X", "S", tp_limit, kNaN, kNaN, kNaN, kNaN,
                          100.0, "", kNaN, "");
        }
    }
    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double trade_size(int i) const { return closed_trade_size(i); }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
};

// E1. Short 10 @ 100 (liq 100.00). bar1 prints a high of 100.004: the
//     rounded mark is 100.00, equity 1000 == required 1000, NO slice. The
//     raw mark (pre-fix) read equity 999.96 < required 1000.04 and produced
//     a phantom 4x(0.0004) slice the on-tick ledger cannot hold.
void test_short_cascade_ignores_sub_tick_excursion() {
    std::printf("-- E1: short cascade ignores a sub-tick excursion --\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,   99.0, 100.0),   // short 10 @ 100
        mk_bar(2000, 100.0, 100.004, 99.5,  99.9),   // high rounds to 100.00
        mk_bar(3000,  99.9, 100.0,   99.0,  99.5),
    };
    ShortCascadeProbe eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 0);                      // pre-fix: 1 phantom
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_qty_, 10.0, 1e-9);
}

// E2. Same shape, bar1 high 105.005 (rounds UP to 105.01). Slice at the
//     rounded mark: q_min = 20 - 2000/105.01 = 0.95419, x4 = 3.8167794.
//     Pre-fix (raw 105.005 mark): 3.8131518. The fill price was 105.01 on
//     both bases (bar_fill_price rounds the raw extreme, finding-446).
void test_short_cascade_slices_at_rounded_high() {
    std::printf("-- E2: short cascade slice sized at the rounded high --\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,    99.0, 100.0),
        mk_bar(2000, 100.0, 105.005,  99.5, 104.0),
        mk_bar(3000, 104.0, 104.0,   103.0, 103.5),
    };
    ShortCascadeProbe eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() >= 1);
    if (eng.trade_count() >= 1) {
        CHECK(eng.exit_comment(0) == std::string("Margin call"));
        CHECK_NEAR(eng.exit_price(0), 105.01, 1e-9);
        CHECK_NEAR(eng.trade_size(0), 4.0 * (20.0 - 2000.0 / 105.01), 1e-6);
        // pre-fix: 4.0 * (20.0 - 2000.0 / 105.005) = 3.8131518
    }
}

// E3. The chronological copy of the cascade takes the same mark. Bars:
//   bar0  100/100/99/100      short 10 @ 100 (POC fill at the close)
//   bar1  100/100/99.5/99.9   on_bar arms a TP limit at 99.60 (rests)
//   bar2  100/H/99.5/99.9     HIGH-first (|H - 100| < 0.5): O -> H -> L -> C,
//                             the adverse high precedes the TP fill on the
//                             H -> L leg, so margin_call_slice_before_priced_
//                             exit asks the deficit question at H BEFORE the
//                             TP fills.
// (a) H = 100.004, the E1 excursion: the rounded mark is 100.00, equity
//     1000 == required 1000, NO slice; the TP closes the full 10 @ 99.60.
//     Pre-fix this path marked at the raw 100.004 (equity 999.96 <
//     required 1000.04) and fired a 4 * 0.0004 = 0.0016 phantom slice —
//     but ONLY because an exit was resting: the end-of-bar cascade (E1)
//     already read the rounded high. The ledger cannot depend on that.
// (b) H = 100.01, on-tick control: equity 999.9 < required 1000.1, q_min =
//     10 - 999.9 / 100.01 = 0.0019998, slice 4x = 0.0079992 @ 100.01 first
//     (continuous lots), then the TP closes the 9.9920008 remainder. The
//     path is live; (a) is silent because of the mark, not eligibility.
void test_short_chronological_slice_marks_at_rounded_high() {
    std::printf("-- E3: chronological pre-exit slice marks at the rounded high --\n");
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0, 100.0,   99.0, 100.0),
            mk_bar(2000, 100.0, 100.0,   99.5,  99.9),
            mk_bar(3000, 100.0, 100.004, 99.5,  99.9),
        };
        ShortCascadeProbe eng;
        eng.tp_limit = 99.60;
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trade_count() == 1);                  // pre-fix: 2 (phantom slice + TP)
        if (eng.trade_count() >= 1) {
            CHECK(eng.exit_comment(0) != std::string("Margin call"));
            CHECK_NEAR(eng.exit_price(0), 99.60, 1e-9);
            CHECK_NEAR(eng.trade_size(0), 10.0, 1e-9);
        }
        CHECK(eng.position_side_ == PositionSide::FLAT);
    }
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0, 100.0,  99.0, 100.0),
            mk_bar(2000, 100.0, 100.0,  99.5,  99.9),
            mk_bar(3000, 100.0, 100.01, 99.5,  99.9),
        };
        ShortCascadeProbe eng;
        eng.tp_limit = 99.60;
        eng.run(bars.data(), (int)bars.size());
        const double q_min = 10.0 - (1000.0 - 0.01 * 10.0) / 100.01;
        CHECK(eng.trade_count() == 2);
        if (eng.trade_count() == 2) {
            CHECK(eng.exit_comment(0) == std::string("Margin call"));
            CHECK_NEAR(eng.exit_price(0), 100.01, 1e-9);
            CHECK_NEAR(eng.trade_size(0), 4.0 * q_min, 1e-9);
            CHECK(eng.exit_comment(1) != std::string("Margin call"));
            CHECK_NEAR(eng.exit_price(1), 99.60, 1e-9);
            CHECK_NEAR(eng.trade_size(1), 10.0 - 4.0 * q_min, 1e-9);
        }
        CHECK(eng.position_side_ == PositionSide::FLAT);
    }
}

// F. The helper on the census closes: the rounding this file depends on.
void test_helper_census_values() {
    std::printf("-- F: round_to_mintick on the census closes --\n");
    Probe eng(100000.0, 0.01, 0);
    CHECK_NEAR(eng.nearest(9.565), 9.56, 1e-12);        // down (binary quotient)
    CHECK_NEAR(eng.nearest(9.585), 9.59, 1e-12);        // up
    CHECK_NEAR(eng.nearest(228.765), 228.76, 1e-12);    // the AAPL down case
    CHECK_NEAR(eng.nearest(214.385), 214.39, 1e-12);    // the AAPL up case
    CHECK(eng.nearest(9.56) == eng.nearest(eng.nearest(9.56)));   // idempotent
}

}  // namespace

int main() {
    std::printf("--- sizing_basis_mintick ---\n");
    test_sub_penny_close_rounds_down_sizes_on_tick();
    test_sub_penny_close_rounds_up_fills_instead_of_gap_reject();
    test_sub_penny_reversal_admitted_on_float_guard_arm();
    test_penny_close_unchanged();
    test_quarter_tick_basis();
    test_slippage_added_after_rounding();
    test_short_cascade_ignores_sub_tick_excursion();
    test_short_cascade_slices_at_rounded_high();
    test_short_chronological_slice_marks_at_rounded_high();
    test_helper_census_values();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
