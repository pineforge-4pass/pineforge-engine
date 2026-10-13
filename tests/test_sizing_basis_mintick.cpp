/*
 * test_sizing_basis_mintick.cpp — default percent-of-equity / cash market
 * orders size on the mintick-rounded signal close, with equity marked there.
 * Pine's signal calculations still read the raw close. Pinned against
 * TradingView behaviour (recorded tapes are kept outside this repository).
 *
 * The decimal-tick fixtures below are hand-designed around a base of 64:
 * binary midpoint rounding down/up, flat and reversal admission, an on-tick
 * control, and slippage applied after rounding. Larger invented midpoints
 * also check the helper. The quarter-tick and 100-price margin-cascade
 * controls retain their separate rounding and chronological-exit checks.
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

// A. Invented midpoint: 64.035 / 0.01 = 6403.499999999999, so the
// floor(p / tick + 0.5) rounds DOWN to 64.03 without an epsilon. Rounded sizing gives
// floor(640000 / 64.03) = 9995, versus floor(640000 / 64.035) = 9994 raw.
// The zero-gap fill costs 9995 * 64.03 = 639979.85 <= 640000; it must fill.
// A2/A3 separately exercise the admission failures caused by rounding UP.
void test_sub_penny_close_rounds_down_sizes_on_tick() {
    std::printf("-- A: invented DOWN midpoint sizes on 64.03 -> 9995, fills --\n");
    Probe eng(640000.0, 0.01, 0);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 64.00, 64.10, 63.50, 64.035),   // S: basis 64.03, qty 9995
        mk_bar(2000, 64.03, 64.10, 64.00, 64.05),    // fill @ open 64.03
        mk_bar(3000, 64.05, 64.10, 63.90, 64.05),    // close_all
        mk_bar(4000, 64.05, 64.10, 63.90, 64.05),    // exit @ 64.05
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 64.03, 1e-12);
        CHECK_NEAR(eng.frozen_qty[0], 9995.0, 1e-9);   // pre-fix: 9994
    }
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        const Trade& t = eng.all_trades()[0];
        CHECK(t.is_long);
        CHECK_NEAR(t.entry_price, 64.03, 1e-9);
        CHECK_NEAR(t.qty, 9995.0, 1e-9);
        CHECK_NEAR(t.exit_price, 64.05, 1e-9);
    }
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// A2. Invented midpoint: 64.025 / 0.01 = 6402.5 rounds UP to 64.03.
// Rounded qty = floor(640000 / 64.03) = 9995; its 639979.85 notional fits.
// Raw qty = floor(640000 / 64.025) = 9996, whose rounded fill would cost
// 9996 * 64.03 = 640043.88 > 640000. Thus a raw divisor would incorrectly
// trigger true-flat gap rejection even though the open equals the basis.
void test_sub_penny_close_rounds_up_fills_instead_of_gap_reject() {
    std::printf("-- A2: 64.025 close (-> 64.03) fills, no phantom gap-reject --\n");
    Probe eng(640000.0, 0.01, 0);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 64.00, 64.10, 63.50, 64.025),   // S: basis 64.03, qty 9995
        mk_bar(2000, 64.03, 64.20, 63.90, 64.10),    // fill @ open 64.03 (no gap)
        mk_bar(3000, 64.10, 64.20, 64.05, 64.10),
        mk_bar(4000, 64.10, 64.20, 64.05, 64.10),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 64.03, 1e-12);
        CHECK_NEAR(eng.frozen_qty[0], 9995.0, 1e-9);   // pre-fix: 9996
    }
    CHECK(eng.trade_count() == 1);                      // pre-fix: 0 (dropped)
    if (eng.trade_count() == 1) {
        const Trade& t = eng.all_trades()[0];
        CHECK(t.is_long);
        CHECK_NEAR(t.entry_price, 64.03, 1e-9);
        CHECK_NEAR(t.qty, 9995.0, 1e-9);
    }
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// A3. Reversal admission at the same UP midpoint. Entry at 64.00 uses
// floor(640000 / 64.00) = 10000 shares, leaving no uninvested cash.
// Signal close 64.025 marks at 64.03: E = 640000 + 0.03 * 10000 = 640300;
// short qty = floor(640300 / 64.03) = 10000 and required funds = 640300.
// A raw mark instead gives E = 640000 + 0.025 * 10000 = 640250 while
// retaining qty 10000; required 640300 exceeds it, declining the reversal.
// The bars close the long at 64.03 and the short at the invented 64.10.
void test_sub_penny_reversal_admitted_on_float_guard_arm() {
    std::printf("-- A3: sub-penny reversal admitted on the float-guard arm --\n");
    Probe eng(640000.0, 0.01, 0);
    eng.script = "LS.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 64.00, 64.25, 63.50, 64.00),
        mk_bar(2000, 64.00, 64.10, 63.50, 64.025),
        mk_bar(3000, 64.03, 64.20, 63.90, 64.10),
        mk_bar(4000, 64.10, 64.20, 64.05, 64.10),
        mk_bar(5000, 64.10, 64.20, 64.05, 64.10),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_sell.size() == 2);
    if (eng.basis_sell.size() == 2) {
        CHECK_NEAR(eng.basis_sell[1], 64.03, 1e-12);
        CHECK_NEAR(eng.frozen_qty[1], 10000.0, 1e-9);
    }
    CHECK(eng.trade_count() == 2);                      // pre-fix: 1 (declined)
    if (eng.trade_count() == 2) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK(t0.is_long);
        CHECK_NEAR(t0.entry_price, 64.00, 1e-9);
        CHECK_NEAR(t0.qty, 10000.0, 1e-9);
        CHECK_NEAR(t0.exit_price, 64.03, 1e-9);
        const Trade& t1 = eng.all_trades()[1];
        CHECK(!t1.is_long);
        CHECK_NEAR(t1.entry_price, 64.03, 1e-9);
        CHECK_NEAR(t1.qty, 10000.0, 1e-9);
        CHECK_NEAR(t1.exit_price, 64.10, 1e-9);
    }
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// B. On-tick control: 64.03 stays the sizing basis to within one ulp.
// floor(640000 / 64.03) = 9995 shares on either the raw or rounded basis.
void test_penny_close_unchanged() {
    std::printf("-- B: penny close unchanged --\n");
    Probe eng(640000.0, 0.01, 0);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 64.00, 64.10, 63.50, 64.03),
        mk_bar(2000, 64.03, 64.10, 64.00, 64.05),
        mk_bar(3000, 64.05, 64.10, 63.90, 64.05),
        mk_bar(4000, 64.05, 64.10, 63.90, 64.05),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 64.03, 1e-12);
        CHECK_NEAR(eng.basis_sell[0], 64.03, 1e-12);
        CHECK_NEAR(eng.frozen_qty[0], 9995.0, 1e-9);
    }
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK_NEAR(eng.all_trades()[0].qty, 9995.0, 1e-9);
        CHECK_NEAR(eng.all_trades()[0].entry_price, 64.03, 1e-9);
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

// D. Round 64.035 DOWN to 64.03, then apply two 0.01 slippage ticks:
// buy basis/fill = 64.05, sell basis = 64.01,
// buy qty = floor(640000 / 64.05) = 9992. Its notional is 639987.60.
// A raw slipped divisor would be 64.055, giving only 9991 shares.
void test_slippage_added_after_rounding() {
    std::printf("-- D: slippage ticks added after rounding --\n");
    Probe eng(640000.0, 0.01, 2);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 64.00, 64.10, 63.50, 64.035),
        mk_bar(2000, 64.03, 64.10, 64.00, 64.05),
        mk_bar(3000, 64.05, 64.10, 63.90, 64.05),
        mk_bar(4000, 64.05, 64.10, 63.90, 64.05),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.basis_buy.size() == 1);
    if (!eng.basis_buy.empty()) {
        CHECK_NEAR(eng.basis_buy[0], 64.05, 1e-12);      // raw slipped buy basis: 64.055
        CHECK_NEAR(eng.basis_sell[0], 64.01, 1e-12);     // raw slipped sell basis: 64.015
        CHECK_NEAR(eng.frozen_qty[0], 9992.0, 1e-9);   // pre-fix: 9991
    }
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK_NEAR(eng.all_trades()[0].entry_price, 64.05, 1e-9);
        CHECK_NEAR(eng.all_trades()[0].qty, 9992.0, 1e-9);
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

// F. Invented decimal midpoints at two scales plus a fixed-point check.
void test_helper_values() {
    std::printf("-- F: round_to_mintick on invented midpoints --\n");
    Probe eng(640000.0, 0.01, 0);
    CHECK_NEAR(eng.nearest(64.035), 64.03, 1e-12);        // 6403.499999999999 -> 64.03
    CHECK_NEAR(eng.nearest(64.025), 64.03, 1e-12);        // 6402.5 -> 64.03
    CHECK_NEAR(eng.nearest(512.035), 512.03, 1e-12);    // 51203.49999999999 -> 512.03
    CHECK_NEAR(eng.nearest(1024.045), 1024.05, 1e-12);    // 102404.5 -> 1024.05
    CHECK(eng.nearest(64.03) == eng.nearest(eng.nearest(64.03)));   // idempotent
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
    test_helper_values();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
