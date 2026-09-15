#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

/*
 * test_margin_stop_admission.cpp — margin admission of STOP-ENTRY fills
 * (KI-62 stage 3, pf-probe-ki62-margin-deferral; re-based in round 7 on
 * the 22 lab tv pins of ledger note log-20260905t053924z-15615295, see
 * test_stop_entry_admission.cpp for the tapes).
 *
 * Under margin simulation (margin_long_/margin_short_ > 0) a stop entry is
 * admitted twice: at PLACEMENT — floored qty * tick(close of the call bar) *
 * margin% <= strategy.equity, a rejected call is dropped and never rests —
 * and at the FILL — the same qty * tick(fill price) <= realized equity,
 * where the fill price is the LEVEL on an intrabar touch and the rounded
 * OPEN on a gap-through (KI-62's original "costs the bar open on a touch"
 * premise was refuted by fresh-touch-once: TV fills 890 x 11.23 with the
 * open at 11.29). A declined fill is CANCELLED (not parked) — an arm-once
 * entry silently dies; a Pine-level reissue re-posts and fills at the first
 * admissible bar. Under-margined ADMITTED fills are margin-called at bar end
 * by the existing KI-31 cascade (unchanged). margin=0 is byte-identical.
 *
 * The scenarios below keep their original final-state assertions; the
 * per-bar mechanics noted in each case are the round-7 rule's.
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
        } else { ++tests_passed; }                                            \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a=(a), _b=(b);                                                 \
        if (!(std::fabs(_a-_b) <= (tol))) {                                    \
            std::printf("  FAIL  %s:%d  %s == %.6f, expected %.6f\n",          \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else { ++tests_passed; }                                            \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b; b.open=o; b.high=h; b.low=l; b.close=c; b.volume=1.0; b.timestamp=ts;
    return b;
}

namespace {

// All-in stop-entry probe. Places a stop entry at a fixed level with EXPLICIT
// qty (mirrors the probe's `qty = equity/lvl`), reissued every bar from bar 0
// unless arm_once (place once at bar 0). margin_call OFF so admission is
// isolated from the KI-31 entry-bar nibble.
class StopProbe : public pineforge::source::PineStrategyHost {
public:
    StopProbe(double capital, double ml, double ms, bool mc) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        pyramiding_ = 1;
        margin_long_ = ml; margin_short_ = ms;
        qty_step_ = 0.0;
        set_margin_call_enabled(mc);
    }
    bool is_long = false;     // stop direction
    double level = 100.0;     // stop price
    double qty = 100.0;       // explicit qty
    bool arm_once = false;
    void on_source_bar(const Bar& /*b*/) override {
        if (arm_once && bar_index_ != 0) return;
        if (bar_index_ < 0) return;
        strategy_entry("BO", is_long, kNaN, level, qty);
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_entry_price_;
};

// A1. Marginal SHORT stop reissued every bar; it fills at the first
// admissible gap-down open.
//   short qty 100 @ stop 100 (all-in on $10k, margin_short=100).
//   bar0 close 110: 100*110 = 11000 > 10000 -> REJECTED at placement (dropped).
//   bar1 close 101: 10100 > 10000 -> rejected again (nothing rests to touch
//   the 99 low). bar2 close 98: 9800 <= 10000 -> ACCEPTED. bar3 open 98
//   (<=100, gap-through): fill costed at the rounded open, 9800 -> fill @98.
void test_marginal_short_stop_declined_then_gap_fill() {
    std::printf("-- A1: marginal short stop intrabar-declined, gap-open admitted --\n");
    StopProbe eng(10000.0, /*ml*/100.0, /*ms*/100.0, /*mc*/false);
    eng.is_long=false; eng.level=100.0; eng.qty=100.0;
    std::vector<Bar> bars = {
        mk(1000, 110,110,110,110),        // bar0: placement 100*110 > 10000 -> REJECTED
        mk(2000, 105,106, 99,101),        // bar1: nothing rests; reissue 100*101 > 10000 -> rejected
        mk(3000,  98, 99, 97, 98),        // bar2: reissue 100*98 <= 10000 -> ACCEPTED
        mk(4000,  98, 98, 98, 98),        // bar3: open 98 <= 100 -> fill @98 (9800 admits)
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);   // eventually fills
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK_NEAR(eng.position_entry_price_, 98.0, 1e-9);  // GAP OPEN, not the level 100
}

// A2. ARM-ONCE marginal short stop: rejected at placement (100*110 > 10000)
// and DROPPED -> never reissued -> stays FLAT forever (the SAO NOFILL
// signature; flatten-stop-once / fresh-0919-once pin the drop).
void test_arm_once_declined_stop_nofill() {
    std::printf("-- A2: arm-once declined stop is cancelled (NOFILL) --\n");
    StopProbe eng(10000.0, 100.0, 100.0, false);
    eng.is_long=false; eng.level=100.0; eng.qty=100.0; eng.arm_once=true;
    std::vector<Bar> bars = {
        mk(1000, 110,110,110,110),        // place once: 100*110 > 10000 -> REJECTED, dropped
        mk(2000, 105,106, 99,101),        // nothing rests to touch
        mk(3000,  98, 99, 97, 98),        // open<=stop but NO reissue -> stays flat
        mk(4000,  98, 98, 98, 98),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);    // armed-once order died
    CHECK(eng.trade_count() == 0);
}

// A3. SIDE-SYMMETRIC long stop: a gap-UP open past the buy-stop is DECLINED
// at the fill (100*105 = 10500 > 10000, costed at the rounded open — the
// fresh-gap-once shape) and the order is dropped; the same-bar reissue at
// close 105 is rejected at placement; the bar2 reissue at close 99 is
// accepted and bar3 opens at the level: 100*100 = 10000 <= 10000 -> fill @100.
//   long qty 100 @ stop 100. bar0 close 90: placement 9000 -> accepted.
void test_marginal_long_stop_gap_declined_then_level_fill() {
    std::printf("-- A3: marginal long stop gap-declined, re-touch admitted (symmetric) --\n");
    StopProbe eng(10000.0, 100.0, 100.0, false);
    eng.is_long=true; eng.level=100.0; eng.qty=100.0;
    std::vector<Bar> bars = {
        mk(1000,  90, 90, 90, 90),        // bar0: place (price below buy-stop, pending)
        mk(2000, 105,106,104,105),        // bar1: gap-up open 105>=100 -> fill DECLINED, dropped; reissue rejected
        mk(3000,  99,100.5, 98, 99),      // bar2: nothing rests; reissue at close 99 -> ACCEPTED
        mk(4000, 100,100,100,100),        // bar3: open 100 through the level -> fill @100
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK_NEAR(eng.position_entry_price_, 100.0, 1e-9); // the level, not the gap @105
}

// C1. CONTROL — margin=0: NO fill-time gate. The intrabar touch fills at the
// level exactly as baseline (KI-34 safety: margin-sim-off paths byte-identical).
void test_margin_zero_fills_at_level() {
    std::printf("-- C1: margin=0 stop fills intrabar at level (control) --\n");
    StopProbe eng(10000.0, /*ml*/0.0, /*ms*/0.0, /*mc*/false);
    eng.is_long=false; eng.level=100.0; eng.qty=100.0;
    std::vector<Bar> bars = {
        mk(1000, 110,110,110,110),
        mk(2000, 105,106, 99,101),        // intrabar touch -> fills @100 (no gate)
        mk(3000, 101,101,101,101),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_entry_price_, 100.0, 1e-9);
}

// C2. CONTROL — well-funded stop: required << equity, so the intrabar touch
// admits at the level under margin sim (the gate only bites the marginal case).
//   short qty 1 @ stop 100, margin_short=100: required 1*105 = 105 << 10000.
void test_well_funded_stop_admitted_at_level() {
    std::printf("-- C2: well-funded stop admitted at level under margin sim --\n");
    StopProbe eng(10000.0, 100.0, 100.0, false);
    eng.is_long=false; eng.level=100.0; eng.qty=1.0;
    std::vector<Bar> bars = {
        mk(1000, 110,110,110,110),
        mk(2000, 105,106, 99,101),        // intrabar touch, required 105 << 10000 -> ADMIT @100
        mk(3000, 101,101,101,101),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_entry_price_, 100.0, 1e-9);
}

}  // namespace

int main() {
    std::printf("--- margin_stop_admission (KI-62 stage 3, round-7 basis) ---\n");
    test_marginal_short_stop_declined_then_gap_fill();
    test_arm_once_declined_stop_nofill();
    test_marginal_long_stop_gap_declined_then_level_fill();
    test_margin_zero_fills_at_level();
    test_well_funded_stop_admitted_at_level();
    std::printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
