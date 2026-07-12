/*
 * test_margin_stop_admission.cpp — STAGE 3: margin fill-time admission gate for
 * STOP-ENTRY fills (pf-probe-ki62-margin-deferral, pinned 99.17%).
 *
 * Under margin simulation (margin_long_/margin_short_ > 0) TV gates every
 * stop-entry fill AT THE FILL MOMENT against the fill bar's OPEN price,
 * side-symmetrically: decline iff qty * open * margin% > available (realized)
 * equity. Admission ignores intrabar extremes (it costs the OPEN, not the fill
 * price / the touched level / the bar high). A declined stop is CANCELLED (not
 * parked) — an arm-once entry silently dies; a Pine-level reissue re-posts and
 * fills at the first admissible bar (a short at the first open<=stop = a
 * price-improved OPEN fill; a long at the first re-touch with open<level).
 * Under-margined ADMITTED fills are margin-called at bar end by the existing
 * KI-31 cascade (unchanged). The :443 created_bar eligibility is NOT touched;
 * the signal-time MARKET-only gate is unaffected; margin=0 is byte-identical.
 *
 * REDs (stash-cycle): A1/A2/A3 are RED on baseline 6abebad (no fill-time
 * gate — every stop fills intrabar at the level) and GREEN with the gate; the
 * controls C1 (margin=0) / C2 (well-funded) pass in BOTH states.
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
class StopProbe : public BacktestEngine {
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
    void on_bar(const Bar& /*b*/) override {
        if (arm_once && bar_index_ != 0) return;
        if (bar_index_ < 0) return;
        strategy_entry("BO", is_long, kNaN, level, qty);
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_entry_price_;
};

// A1. Marginal SHORT stop, INTRABAR trigger -> DECLINED + CANCELLED; the
// reissue fills at the first OPEN<=stop (price-improved gap fill).
//   short qty 100 @ stop 100 (all-in on $10k, margin_short=100).
//   bar1 open 105 (>100), low 99 (<=100): intrabar touch. required 100*105 =
//   10500 > 10000 -> DECLINE (baseline fills SHORT@100 here). bar2 open 98
//   (<=100 gap-down): required 100*98 = 9800 <= 10000 -> ADMIT, fill @98.
void test_marginal_short_stop_declined_then_gap_fill() {
    std::printf("-- A1: marginal short stop intrabar-declined, gap-open admitted --\n");
    StopProbe eng(10000.0, /*ml*/100.0, /*ms*/100.0, /*mc*/false);
    eng.is_long=false; eng.level=100.0; eng.qty=100.0;
    std::vector<Bar> bars = {
        mk(1000, 110,110,110,110),        // bar0: place (price above stop, pending)
        mk(2000, 105,106, 99,101),        // bar1: intrabar touch (open 105>100) -> DECLINE
        mk(3000,  98, 99, 97, 98),        // bar2: gap-down open 98<=100 -> ADMIT @98
        mk(4000,  98, 98, 98, 98),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);   // eventually fills
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK_NEAR(eng.position_entry_price_, 98.0, 1e-9);  // GAP OPEN, not the level 100
}

// A2. ARM-ONCE marginal short stop: declined at the intrabar touch and CANCELLED
// -> never reissued -> stays FLAT forever (the SAO NOFILL signature).
void test_arm_once_declined_stop_nofill() {
    std::printf("-- A2: arm-once declined stop is cancelled (NOFILL) --\n");
    StopProbe eng(10000.0, 100.0, 100.0, false);
    eng.is_long=false; eng.level=100.0; eng.qty=100.0; eng.arm_once=true;
    std::vector<Bar> bars = {
        mk(1000, 110,110,110,110),        // place once
        mk(2000, 105,106, 99,101),        // intrabar touch -> DECLINE + CANCEL
        mk(3000,  98, 99, 97, 98),        // open<=stop but NO reissue -> stays flat
        mk(4000,  98, 98, 98, 98),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);    // armed-once order died
    CHECK(eng.trade_count() == 0);
}

// A3. SIDE-SYMMETRIC long stop: a gap-UP open past the buy-stop is DECLINED
// (required at the high open > equity); the re-touch bar whose open is below the
// level admits at the level.
//   long qty 100 @ stop 100. bar1 open 95 low? no touch. Make bar1 gap up:
//   open 105 (>=100) -> required 100*105=10500 > 10000 -> DECLINE. bar2 opens 99
//   (<100) and highs to 100 -> intrabar touch, required 100*99=9900 <=10000 ->
//   ADMIT at the level 100.
void test_marginal_long_stop_gap_declined_then_level_fill() {
    std::printf("-- A3: marginal long stop gap-declined, re-touch admitted (symmetric) --\n");
    StopProbe eng(10000.0, 100.0, 100.0, false);
    eng.is_long=true; eng.level=100.0; eng.qty=100.0;
    std::vector<Bar> bars = {
        mk(1000,  90, 90, 90, 90),        // bar0: place (price below buy-stop, pending)
        mk(2000, 105,106,104,105),        // bar1: gap-up open 105>=100 -> DECLINE (base fills @105)
        mk(3000,  99,100.5, 98, 99),      // bar2: open 99<100, high 100.5>=100 -> ADMIT @100
        mk(4000, 100,100,100,100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK_NEAR(eng.position_entry_price_, 100.0, 1e-9); // re-touch level, not the gap @105
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
    std::printf("--- margin_stop_admission (KI-62 stage 3) ---\n");
    test_marginal_short_stop_declined_then_gap_fill();
    test_arm_once_declined_stop_nofill();
    test_marginal_long_stop_gap_declined_then_level_fill();
    test_margin_zero_fills_at_level();
    test_well_funded_stop_admitted_at_level();
    std::printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
