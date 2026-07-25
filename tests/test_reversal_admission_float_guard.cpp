/*
 * test_reversal_admission_float_guard.cpp —
 * design-reversal-admission-float-guard.
 *
 * The KI-54 fill-time margin admission gate (engine_fills.cpp) compares
 *
 *   required = |frozen_default_qty| * admit_price * pointvalue * fx * margin/100
 *   free     = sizing_equity - held          (held == 0 unless same-direction)
 *   decline iff required > free + epsilon
 *
 * The epsilon used to be widened by ONE WHOLE LOT of notional
 * (qty_step * admit_price * ...) on every arm of that gate. The stated
 * justification was that the frozen quantity was floored to the lot step, so a
 * shortfall smaller than the unspent remainder is "a coin flip on where the
 * floor happened to land", and — decisively — that widening by one lot "keeps
 * every decline that TV's exports actually confirm (their margins exceed a lot
 * of notional)". That last clause is an empirical claim about the ABSENCE of
 * sub-lot ground truth.
 *
 * WHAT FALSIFIED IT (reversal arm only). A scraped 13-month ETHUSDT.P row
 * (chartprime-power-order-blocks-chartprime; percent_of_equity = 100, margin
 * 100, qty_step 0.0001) carries 2,419 TradingView reversal decisions: 94
 * declines and 2,325 admits. 92 of the 94 declines have margins BELOW one lot
 * of notional. On an all-in reversal the whole decision lives inside
 * [0, qty_step * admit_price) by construction — the order spends the entire
 * equity — so the widening does not blunt this arm's gate, it makes it INERT:
 *
 *   epsilon = one lot     ->  2/94 declines caught, balanced accuracy 51.1 %
 *   epsilon = float guard -> 86/94 declines caught, 6/2,325 wrongly cancelled,
 *                            balanced accuracy 95.6 %
 *
 * Board-wide the tightened arm would cancel 18 of 23,785 TradingView-ADMITTED
 * all-in reversals (0.08 %); only 1 of those 18 is above one lot.
 *
 * Scope: the one-lot term is REMOVED ON THE REVERSAL ARM ONLY. The flat-open
 * arm and the same-direction-add arm keep it — each is separately TV-pinned,
 * nothing has falsified their premise, and the "coin flip" argument genuinely
 * does apply to them (they price at the SIZING price the quantity was floored
 * against, so any residual really is floor luck). On a reversal the quantity is
 * floored against the PREVIOUS bar's close while the order fills at THIS bar's
 * open: the overshoot is an observable gap, not floor luck.
 *
 * Pins below (A-D are the reversal arm; E-G are the scope controls that must
 * NOT move):
 *   A. Reversal, adverse gap costing LESS than one lot -> DECLINED (the RED:
 *      base admits this).
 *   B. Reversal, zero gap (required == free exactly) -> ADMITTED. The guard is
 *      a strict `>`; a `>=` would cancel every ordinary flip.
 *   C. Reversal, favourable gap -> ADMITTED.
 *   D. Reversal, adverse gap far above one lot -> DECLINED (unchanged).
 *   E. SCOPE: same-direction add whose shortfall is under one lot -> still
 *      ADMITTED (the add arm keeps the widening), and its above-one-lot
 *      sibling is still DECLINED. This pair is what proves the term is still
 *      load-bearing where it was left in place.
 *   F. SCOPE: flat open on the SAME sub-lot adverse gap as pin A -> ADMITTED.
 *      The exact mirror of A on the flat arm.
 *   G. SCOPE: qty_step == 0 (no lot quantization) is unchanged by definition —
 *      the one-lot term was already zero there, so base and patched must agree.
 *
 * Every fixture uses mintick 0.01 on-tick prices, zero slippage, zero
 * commission and margin_call_enabled_ = false, so the only mechanism under test
 * is the admission comparison itself.
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
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

class Probe : public BacktestEngine {
public:
    Probe(QtyType qty_type, double qty_value, int pyramiding, double step) {
        initial_capital_ = 10000.0;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        commission_value_ = 0.0;
        pyramiding_ = pyramiding;
        qty_step_ = step;
        // All-in probes hold fully-leveraged positions; forced liquidation is
        // not the mechanism under test.
        margin_call_enabled_ = false;
    }
    // 'L' = default long "L", 'A' = default long add "L2",
    // 'S' = default short "S", '.' = nothing.
    std::string script;
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': strategy_entry("L", true); break;
            case 'A': strategy_entry("L2", true); break;
            case 'S': strategy_entry("S", false); break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    using BacktestEngine::qty_step_;
    const std::vector<Trade>& all_trades() const { return trades_; }
};

// Shared reversal fixture. Long all-in fills at 100 on bar 1 and the opposite
// default-sized market entry is queued at that bar's close (also 100), so the
// short is frozen at qty = floor_step(10000 / 100) = 100 with sizing_price 100
// and sizing_equity 10000. `flip_open` is bar 2's open — the price the reversal
// actually books at, and the only free variable.
//
//   required  = 100 * flip_open        free = 10000
//   one lot   = qty_step * flip_open   (1.0 * flip_open with step 1.0)
static void run_reversal(Probe& eng, double flip_open) {
    eng.script = "LS..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),          // L placed (frozen 100)
        mk_bar(2000, 100, 100, 100, 100),          // L fills @100; S placed
        mk_bar(3000, flip_open, flip_open, flip_open, flip_open),
        mk_bar(4000, flip_open, flip_open, flip_open, flip_open),
    };
    eng.run(bars.data(), (int)bars.size());
}

// A. THE RED. Adverse gap 100 -> 100.5 on the flip bar.
//    required = 100 * 100.5 = 10050, free = 10000, shortfall $50.
//    One lot of notional is 1.0 * 100.5 = $100.5, so the shortfall is well
//    INSIDE the old widening: the pre-fix engine admits this flip.
//    TradingView declines it (chartprime: 92 of 94 confirmed declines are
//    sub-lot), so the reversal arm must now decline it too. The close leg is
//    suppressed with the entry, so the LONG survives and no trade row is
//    emitted.
void test_reversal_sub_lot_gap_declined() {
    std::printf("-- A: reversal, sub-lot adverse gap, DECLINED --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1, /*step=*/1.0);
    run_reversal(eng, 100.5);
    CHECK(eng.position_side_ == PositionSide::LONG);   // flip did NOT happen
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK(eng.trade_count() == 0);                     // no close leg either
}

// B. Zero gap: required == free EXACTLY (10000 == 10000). The float guard must
//    admit the tie — the comparison is a strict `>`. Without this pin a
//    `>=` mutation, or a guard that subtracts instead of adds, would cancel
//    every ordinary all-in flip on a non-gapping bar (the most common bar
//    shape there is).
void test_reversal_exact_tie_admitted() {
    std::printf("-- B: reversal, exact required==free tie, ADMITTED --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1, /*step=*/1.0);
    run_reversal(eng, 100.0);
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK(t0.is_long);
        CHECK_NEAR(t0.exit_price, 100.0, 1e-9);
    }
}

// C. Favourable gap 100 -> 99.5: required = 9950 < 10000. Admitted, and the
//    tightened epsilon must not manufacture a decline out of a surplus.
void test_reversal_favourable_gap_admitted() {
    std::printf("-- C: reversal, favourable gap, ADMITTED --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1, /*step=*/1.0);
    run_reversal(eng, 99.5);
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK(eng.trade_count() == 1);
}

// D. Adverse gap 100 -> 103: shortfall $300 against a $103 lot. This already
//    declined before the change; it must still decline. Guards against a
//    mutation that deletes the whole comparison rather than the widening.
void test_reversal_above_lot_gap_still_declined() {
    std::printf("-- D: reversal, gap far above one lot, still DECLINED --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1, /*step=*/1.0);
    run_reversal(eng, 103.0);
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// E. SCOPE CONTROL — the same-direction ADD keeps the one-lot widening.
//
//    pct = 50, pyramiding = 2, qty_step 1.0. The long fills at 100 with
//    qty = floor_1(10000*0.5/100) = 50, then the bar closes at C, which is
//    where the add is sized and where BOTH sides of its comparison are marked:
//
//      equity  = 10000 + 50*(C-100)
//      held    = 50 * C
//      free    = equity - held = 5000                (constant in C)
//      qty_add = floor_1((equity/2) / C)
//      required= qty_add * C
//
//    E.1  C = 106: equity 10300, qty_add = floor_1(5150/106) = floor_1(48.58)
//         = 48, required = 5088. Shortfall $88 against a $106 lot — INSIDE the
//         widening, so the add is ADMITTED. This is the arm's mirror of pin A,
//         and it must NOT move: if the term were dropped everywhere instead of
//         on the reversal arm, this add would flip to declined.
//    E.2  C = 108: equity 10400, qty_add = floor_1(5200/108) = 48,
//         required = 5184. Shortfall $184 against a $108 lot — OUTSIDE the
//         widening, so the add is still DECLINED. Together E.1/E.2 bracket the
//         boundary and prove the term is still load-bearing here.
static void run_same_dir_add(Probe& eng, double signal_close) {
    eng.script = "LA..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),      // L placed (frozen 50)
        // L fills @100 (LONG 50); bar closes at signal_close where L2 is sized
        mk_bar(2000, 100, signal_close, 100, signal_close),
        mk_bar(3000, signal_close, signal_close, signal_close, signal_close),
        mk_bar(4000, signal_close, signal_close, signal_close, signal_close),
    };
    eng.run(bars.data(), (int)bars.size());
}

void test_same_dir_add_keeps_one_lot_slack() {
    std::printf("-- E: same-direction add keeps the one-lot slack --\n");
    {
        std::printf("   E.1 sub-lot shortfall add still ADMITTED\n");
        Probe eng(QtyType::PERCENT_OF_EQUITY, 50.0, 2, /*step=*/1.0);
        run_same_dir_add(eng, 106.0);
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_qty_, 98.0, 1e-9);      // 50 + 48
    }
    {
        std::printf("   E.2 above-lot shortfall add still DECLINED\n");
        Probe eng(QtyType::PERCENT_OF_EQUITY, 50.0, 2, /*step=*/1.0);
        run_same_dir_add(eng, 108.0);
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_qty_, 50.0, 1e-9);      // add dropped
    }
}

// F. SCOPE CONTROL — the FLAT open on pin A's exact gap is unaffected.
//    Frozen qty = floor_1(10000/100) = 100, sizing_price 100, and the bar opens
//    at 100.5 — byte-for-byte the same adverse gap that pin A now declines.
//    Two separate mechanisms must both keep admitting it:
//      * the true-flat zero-commission gap-reject above the KI-54 gate, whose
//        own one-lot slack ($100.5) still covers the $50 gap notional; and
//      * the KI-54 gate itself, which prices a flat open at the SIZING price
//        (required 10000 == free 10000, the floor invariant).
//    So the entry fills, at the gapped open, carrying the frozen quantity.
void test_flat_open_sub_lot_gap_still_admitted() {
    std::printf("-- F: flat open on the same sub-lot gap, ADMITTED --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1, /*step=*/1.0);
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),          // L placed (frozen 100)
        mk_bar(2000, 100.5, 100.5, 100.5, 100.5),  // same gap as pin A
        mk_bar(3000, 100.5, 100.5, 100.5, 100.5),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
}

// G. SCOPE CONTROL — with qty_step 0 the one-lot term was already identically
//    zero, so nothing about this fixture can differ between base and patched.
//    The unquantized reversal declines on any adverse gap beyond the float
//    guard and admits the tie, at BOTH arms.
void test_zero_qty_step_unchanged() {
    std::printf("-- G: qty_step 0 behaviour identical to base --\n");
    {
        Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1, /*step=*/0.0);
        run_reversal(eng, 100.5);
        CHECK(eng.position_side_ == PositionSide::LONG);   // declined
        CHECK(eng.trade_count() == 0);
    }
    {
        Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1, /*step=*/0.0);
        run_reversal(eng, 100.0);
        CHECK(eng.position_side_ == PositionSide::SHORT);  // tie admitted
        CHECK(eng.trade_count() == 1);
    }
}

}  // namespace

int main() {
    std::printf("=== test_reversal_admission_float_guard ===\n");
    test_reversal_sub_lot_gap_declined();
    test_reversal_exact_tie_admitted();
    test_reversal_favourable_gap_admitted();
    test_reversal_above_lot_gap_still_declined();
    test_same_dir_add_keeps_one_lot_slack();
    test_flat_open_sub_lot_gap_still_admitted();
    test_zero_qty_step_unchanged();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
