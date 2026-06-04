/*
 * test_ta_osc_edge.cpp — edge/guard coverage for src/ta_oscillators.cpp.
 *
 * Targets the na-guards, flat / divide-by-zero arms, window-eviction
 * (pop_front) lines, and cross prev-state false branches that the broader
 * indicator suites (test_ta.cpp / test_ta_indicators_extras.cpp /
 * test_ta_rma_warmup.cpp) leave under-exercised:
 *
 *   RSI    : na-input early return (lines 34-36).
 *   Stoch  : flat-range -> 50.0 midpoint (126-127) + recompute na/flat
 *            arms (561-567).
 *   Change : history window eviction via pop_front (141-143) + recompute
 *            na-prev arm (579-585).
 *   Cross  : na-input -> prev-state-only update, returns false (168-172).
 *   Mom    : na-input early return (207-208).
 *   ROC    : na-input early return (227-229).
 *   CCI    : na-input early return (308-309).
 *   RCI    : na-input early return (346-348).
 *
 * NDEBUG-PROOF: every assertion goes through CHECK(), which increments a
 * file-scope failure counter and is reported by main()'s nonzero return.
 * It does NOT use bare assert(), so it fires identically under -DNDEBUG.
 * Expected numeric values are Pine-correct, derived by hand from the
 * documented behaviour of ta_oscillators.cpp and pinned here.
 */

#include <cmath>
#include <cstdio>
#include <limits>

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int g_fail = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);\
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) <= tol;
}

// ----------------------------------------------------------------------------
// RSI na-input early return (lines 34-36): once past the first bar, an na
// source must short-circuit to na before touching the RMA chain.
// ----------------------------------------------------------------------------
static void test_rsi_na_input() {
    std::printf("test_rsi_na_input\n");
    ta::RSI rsi(3);
    // Bar 0: seed -> na (bar_count==0 branch).
    CHECK(is_na(rsi.compute(10.0)));
    // Subsequent finite bars stay na during warmup (RMA seed not reached).
    CHECK(is_na(rsi.compute(11.0)));
    // na source on a non-first bar hits the `if (is_na(src)) return na` arm
    // *before* bar_count is consulted -> na, no state advance crash.
    CHECK(is_na(rsi.compute(na<double>())));
    // Still produces na (RMA hasn't seeded after the na bar was skipped).
    CHECK(is_na(rsi.compute(12.0)));
    // After enough finite bars RSI eventually becomes finite; a strictly
    // rising tail collapses rma_down to 0 -> RSI == 100 (short-circuit arm).
    double v = std::numeric_limits<double>::quiet_NaN();
    for (int i = 0; i < 10; ++i) v = rsi.compute(20.0 + i);
    CHECK(!is_na(v));
    CHECK(near(v, 100.0));
}

// ----------------------------------------------------------------------------
// Stoch flat-range midpoint (lines 124-129) + na guard (120-122) + recompute
// flat/na arms (561-567).
// ----------------------------------------------------------------------------
static void test_stoch_flat_and_na() {
    std::printf("test_stoch_flat_and_na\n");

    // Warmup: fewer than `length` bars -> na (Highest/Lowest not full).
    ta::Stoch stoch(3);
    CHECK(is_na(stoch.compute(10.0, 11.0, 9.0)));
    CHECK(is_na(stoch.compute(10.0, 12.0, 8.0)));

    // Flat high/low across the whole window -> hi == lo -> range 0 -> 50.0
    // midpoint. Feed a constant high/low (range zero) for `length` bars.
    ta::Stoch flat(3);
    flat.compute(5.0, 5.0, 5.0);
    flat.compute(5.0, 5.0, 5.0);
    CHECK(near(flat.compute(5.0, 5.0, 5.0), 50.0));
    // recompute on the same flat bar also takes the range==0 -> 50.0 arm.
    CHECK(near(flat.recompute(5.0, 5.0, 5.0), 50.0));

    // na source with a valid window -> na (the is_na(src) guard).
    ta::Stoch s2(2);
    s2.compute(10.0, 12.0, 8.0);
    s2.compute(11.0, 13.0, 7.0);
    CHECK(is_na(s2.compute(na<double>(), 14.0, 6.0)));
    // recompute na-src arm.
    CHECK(is_na(s2.recompute(na<double>(), 14.0, 6.0)));

    // Sanity: a real (non-flat) value is computed correctly so the test is
    // not vacuous. window high = max(12,13)=13, low = min(8,7)=7, src=10:
    // (10 - 7) / (13 - 7) * 100 = 50.0  ... pick src=8.5 -> 25.0 to differ.
    ta::Stoch s3(2);
    s3.compute(10.0, 12.0, 8.0);
    double v = s3.compute(8.5, 13.0, 7.0);
    // hi = max(12,13)=13, lo = min(8,7)=7 -> (8.5-7)/(13-7)*100 = 25.0
    CHECK(near(v, 25.0));
    // recompute same bar matches.
    CHECK(near(s3.recompute(8.5, 13.0, 7.0), 25.0));
}

// ----------------------------------------------------------------------------
// Change: window eviction via history.pop_front (lines 140-143) + the na-prev
// guard in compute (151-153) and recompute (579-585).
// ----------------------------------------------------------------------------
static void test_change_window_eviction() {
    std::printf("test_change_window_eviction\n");

    // max_length default 1, length default 1 -> keep = max(1,1)+1 = 2.
    // Feeding a 3rd bar pushes size to 3 > 2 -> pop_front() evicts the oldest.
    ta::Change change(1);
    CHECK(is_na(change.compute(10.0)));        // size 1 <= length 1 -> na
    CHECK(near(change.compute(13.0), 3.0));    // 13 - 10
    // Third bar: history was [10,13]; push 17 -> [10,13,17] size 3 > keep 2
    // -> pop_front removes 10 -> [13,17]; result = 17 - 13 = 4.
    CHECK(near(change.compute(17.0), 4.0));    // exercises pop_front
    // Fourth bar confirms the window keeps sliding: [13,17] push 20 ->
    // [13,17,20] -> pop_front -> [17,20]; 20 - 17 = 3.
    CHECK(near(change.compute(20.0), 3.0));

    // na source -> na (is_na(src) arm in compute).
    CHECK(is_na(change.compute(na<double>())));

    // A larger lookback exercises keep = max(max_length, length)+1 with a
    // deeper history and still evicts. max_length 3, length 2 -> keep = 4.
    ta::Change deep(3);
    CHECK(is_na(deep.compute(1.0, 2)));   // size 1 <= 2 -> na
    CHECK(is_na(deep.compute(2.0, 2)));   // size 2 <= 2 -> na
    CHECK(near(deep.compute(3.0, 2), 2.0));   // 3 - history[0]=1
    CHECK(near(deep.compute(4.0, 2), 2.0));   // 4 - history[1]=2
    // 5th push: [1,2,3,4] push 5 -> size 5 > keep 4 -> pop_front -> [2,3,4,5]
    // idx = 4-1-2 = 1 -> history[1] = 3 -> 5 - 3 = 2.
    CHECK(near(deep.compute(5.0, 2), 2.0));

    // recompute na-prev arm: replace the last bar with na -> na.
    ta::Change rc(3);
    rc.compute(10.0, 1);
    rc.compute(12.0, 1);   // history [10,12], result 2 (not checked)
    CHECK(is_na(rc.recompute(na<double>(), 1)));  // src na -> na (line 581-585)
    // recompute with a finite replacement returns a finite change:
    // history.back() = 20 -> [10,20], length 1 -> idx 0 -> 20 - 10 = 10.
    CHECK(near(rc.recompute(20.0, 1), 10.0));
}

// ----------------------------------------------------------------------------
// Cross: na-input -> only update prev state, return false (lines 168-172),
// plus the false branch where current sign matches the remembered sign.
// ----------------------------------------------------------------------------
static void test_cross_na_and_false_branch() {
    std::printf("test_cross_na_and_false_branch\n");

    ta::Cross cross;
    // na on either operand -> false, prev updated (na-guard arm).
    CHECK(!cross.compute(na<double>(), 1.0));
    CHECK(!cross.compute(1.0, na<double>()));

    // First finite comparison: last_nonzero_sign_ was 0 -> no fire.
    CHECK(!cross.compute(3.0, 2.0));   // a>b -> sign +1, remembered, no fire
    // Same side again -> curr_sign == last_nonzero_sign_ -> false branch.
    CHECK(!cross.compute(4.0, 2.0));   // still +1 -> no cross
    // Opposite side -> fires.
    CHECK(cross.compute(1.0, 2.0));    // sign -1 vs remembered +1 -> cross
    // na again mid-stream -> false, and remembered sign unchanged.
    CHECK(!cross.compute(na<double>(), 2.0));

    // Tie bar (a == b) -> curr_sign 0 -> no fire, remembered sign untouched.
    CHECK(!cross.compute(2.0, 2.0));
    // Returning to the SAME side as the last non-tied sign (-1) -> false.
    CHECK(!cross.compute(1.5, 2.0));
    // Crossing to +1 now fires (opposite of remembered -1).
    CHECK(cross.compute(5.0, 2.0));

    // recompute na arm: replace last bar with na -> false, no crash.
    CHECK(!cross.recompute(na<double>(), 2.0));
}

// ----------------------------------------------------------------------------
// Mom / ROC na-input early returns (lines 207-208 / 227-229) plus warmup na
// and a finite sanity value so the checks are non-vacuous.
// ----------------------------------------------------------------------------
static void test_mom_roc_na() {
    std::printf("test_mom_roc_na\n");

    ta::Mom mom(2);
    CHECK(is_na(mom.compute(10.0)));            // warmup
    CHECK(is_na(mom.compute(na<double>())));    // na-input arm, no buffer push
    CHECK(is_na(mom.compute(11.0)));            // still building window
    // Now have [10,11,12] -> Mom(2) = 12 - buffer.front()=10 = 2.
    CHECK(near(mom.compute(12.0), 2.0));

    ta::ROC roc(2);
    CHECK(is_na(roc.compute(10.0)));            // warmup
    CHECK(is_na(roc.compute(na<double>())));    // na-input arm
    CHECK(is_na(roc.compute(20.0)));            // still building
    // [10,20,40] -> ROC(2) = (40 - 10)/10 * 100 = 300.
    CHECK(near(roc.compute(40.0), 300.0));
}

// ----------------------------------------------------------------------------
// CCI / RCI na-input early returns (lines 308-309 / 346-348) plus warmup na
// and a finite sanity value.
// ----------------------------------------------------------------------------
static void test_cci_rci_na() {
    std::printf("test_cci_rci_na\n");

    ta::CCI cci(3);
    CHECK(is_na(cci.compute(na<double>())));    // na-input arm, no buffer push
    CHECK(is_na(cci.compute(10.0)));            // warmup
    CHECK(is_na(cci.compute(12.0)));            // warmup
    // Window [10,12,14]: mean 12, mean_dev = (2+0+2)/3 = 4/3,
    // cci = (14-12)/(0.015*4/3).
    double v = cci.compute(14.0);
    CHECK(near(v, (14.0 - 12.0) / (0.015 * (4.0 / 3.0)), 1e-9));

    ta::RCI rci(5);
    CHECK(is_na(rci.compute(na<double>())));    // na-input arm, no buffer push
    // Strictly rising series of `length` bars -> Spearman rho 1 -> 100.
    double r = std::numeric_limits<double>::quiet_NaN();
    for (int i = 1; i <= 5; ++i) r = rci.compute(double(i));
    CHECK(near(r, 100.0));
    // na after a full window -> na (guard fires before recomputing rho).
    CHECK(is_na(rci.compute(na<double>())));
}

int main() {
    test_rsi_na_input();
    test_stoch_flat_and_na();
    test_change_window_eviction();
    test_cross_na_and_false_branch();
    test_mom_roc_na();
    test_cci_rci_na();

    if (g_fail) {
        std::fprintf(stderr, "\nta_osc_edge: %d CHECK(s) FAILED\n", g_fail);
    } else {
        std::printf("\nta_osc_edge: all checks passed\n");
    }
    return g_fail ? 1 : 0;
}
