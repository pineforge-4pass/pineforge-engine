/*
 * test_ta_ma_warmup_extra.cpp — warmup, na-propagation and sliding-window
 * eviction coverage for the moving-average primitives in
 * src/ta_moving_averages.cpp that were left un-exercised by test_ta.cpp /
 * test_ta_indicators_extras.cpp / test_ta_rma_warmup.cpp.
 *
 * Targeted uncovered lines in src/ta_moving_averages.cpp:
 *   28-29    RMA::compute      na(src) -> na guard
 *   104-105  SMA::compute      periodic exact-sum recompute (bar_count & 255 == 0)
 *   145-146  WMA::compute      na(src) -> na guard
 *   183-184  HMA::compute      na(src) -> na guard
 *   213-217  VWMA::compute     while(size > length) pop_front sv/v eviction
 *   306-310  SMA::recompute    na(src) guard + empty-buffer dispatch to compute
 *   354-355  HMA::recompute    na inner-WMA result -> na guard
 *
 * NDEBUG-PROOF: this test never uses bare assert(). It uses a returning
 * CHECK macro that increments a failure counter; main() returns nonzero if
 * any check fails, so the canonical Release (-DNDEBUG) gate cannot pass it
 * vacuously. Reference values were hand-derived (see comments) and confirmed
 * against an independent Python computation.
 */

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); \
            ++tests_failed;                                                   \
        } else {                                                             \
            ++tests_passed;                                                  \
        }                                                                   \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) <= tol;
}

// --------------------------------------------------------------------
// RMA::compute na-input guard (lines 28-29).
// --------------------------------------------------------------------
// Pine ta.rma propagates na on na input *without* advancing bar_count.
// Verify: an na input mid-warmup returns na and does NOT count toward the
// seed length — the seed therefore lands one real bar later than it would
// have if the na had been counted.
static void test_rma_na_input_guard() {
    std::printf("test_rma_na_input_guard\n");

    // Period 4. Feed three real bars, then an na, then more reals.
    ta::RMA rma(4);
    CHECK(is_na(rma.compute(10.0)));   // bar_count 1
    CHECK(is_na(rma.compute(11.0)));   // bar_count 2
    CHECK(is_na(rma.compute(12.0)));   // bar_count 3
    // na input: returns na, bar_count stays at 3 (the 28-29 early return).
    CHECK(is_na(rma.compute(na<double>())));
    // Because the na did NOT advance bar_count, this real bar is only the
    // 4th counted sample -> seed fires here = mean(10,11,12,13) = 11.5.
    double seed = rma.compute(13.0);
    CHECK(near(seed, (10.0 + 11.0 + 12.0 + 13.0) / 4.0));  // 11.5

    // Control: without the na, the seed would have fired one bar earlier.
    ta::RMA ctrl(4);
    ctrl.compute(10.0);
    ctrl.compute(11.0);
    ctrl.compute(12.0);
    double ctrl_seed = ctrl.compute(13.0);  // 4th counted -> seed
    CHECK(near(ctrl_seed, 11.5));
}

// --------------------------------------------------------------------
// SMA::compute periodic exact-sum self-correction (lines 104-105).
// --------------------------------------------------------------------
// The branch `if ((bar_count & 255) == 0) running_sum = recalculate_exact_sum();`
// only fires once bar_count is a non-zero multiple of 256 AND past the warmup
// window. Feed >256 bars to a period-3 SMA so bar_count reaches 256.
//
// The recompute does not change the mathematical result (it re-derives the
// exact window sum), so the value at bar 256 must equal the mean of the last
// three inputs. We pin that value to confirm the self-correction path leaves
// the result correct rather than corrupting running_sum.
static void test_sma_periodic_exact_sum_recompute() {
    std::printf("test_sma_periodic_exact_sum_recompute\n");

    ta::SMA sma(3);
    auto src = [](int i) -> double { return 1.0 + (double)(i % 50); };

    double v256 = na<double>();
    double v257 = na<double>();
    // bar_count == i+1. We need bar_count to reach 256 -> i == 255, and 257.
    for (int i = 0; i < 260; ++i) {
        double out = sma.compute(src(i));
        if (i == 255) v256 = out;   // bar_count 256 -> recalc branch fires
        if (i == 256) v257 = out;   // bar_count 257 -> ordinary path
        if (i >= 2) {
            CHECK(!is_na(out));     // warmup over after 3 bars
        }
    }

    // bar 256 (i=255): window = src(253), src(254), src(255)
    //   = (253%50)+1, (254%50)+1, (255%50)+1 = 4, 5, 6 -> mean 5.0
    CHECK(near(v256, (src(253) + src(254) + src(255)) / 3.0));
    CHECK(near(v256, 5.0));
    // bar 257 (i=256): window = src(254..256) = 5, 6, 7 -> mean 6.0
    CHECK(near(v257, (src(254) + src(255) + src(256)) / 3.0));
    CHECK(near(v257, 6.0));
}

// --------------------------------------------------------------------
// WMA::compute na-input guard (lines 145-146).
// --------------------------------------------------------------------
static void test_wma_na_input_guard() {
    std::printf("test_wma_na_input_guard\n");

    ta::WMA wma(3);
    CHECK(is_na(wma.compute(10.0)));
    CHECK(is_na(wma.compute(20.0)));
    // na input -> early na return (lines 145-146); buffer is NOT advanced.
    CHECK(is_na(wma.compute(na<double>())));
    // Buffer still holds {10, 20} (na was not pushed), so this is only the
    // 3rd real sample -> window {10,20,30}: weights oldest..newest = 1,2,3.
    //   (10*1 + 20*2 + 30*3) / (1+2+3) = (10 + 40 + 90) / 6 = 140/6.
    double v = wma.compute(30.0);
    CHECK(near(v, 140.0 / 6.0));
}

// --------------------------------------------------------------------
// HMA::compute na-input guard (lines 183-184) and HMA::recompute na-result
// guard (lines 354-355).
// --------------------------------------------------------------------
static void test_hma_na_guards() {
    std::printf("test_hma_na_guards\n");

    // compute na-guard: na input returns na immediately (lines 183-184) and
    // does not advance the inner WMA chain.
    ta::HMA hma(4);
    CHECK(is_na(hma.compute(na<double>())));
    // Subsequent real bars still warm up normally (the na was a no-op): with
    // length 4, wma_full_ needs 4 samples before any non-na, so the first
    // three reals are na.
    CHECK(is_na(hma.compute(1.0)));
    CHECK(is_na(hma.compute(2.0)));
    CHECK(is_na(hma.compute(3.0)));

    // recompute na-result guard (lines 354-355): during warmup the inner
    // wma_half_/wma_full_.recompute return na -> HMA::recompute must return
    // na. Drive a fresh HMA with one compute (so the inner buffers are
    // non-empty and recompute takes the in-place update path, not the
    // empty-buffer dispatch), then recompute while still in warmup.
    ta::HMA hma2(9);
    hma2.compute(1.0);             // inner WMA buffers now non-empty
    double r = hma2.recompute(2.0);  // still far from warmup -> inner na -> na
    CHECK(is_na(r));
}

// --------------------------------------------------------------------
// VWMA::compute sliding-window eviction (lines 213-217).
// --------------------------------------------------------------------
// Feed period+extra bars so the while-loop pops the oldest sv/v samples once
// the buffer exceeds length_. Verify the result reflects ONLY the most recent
// `length_` samples: if pop_front (and the matching sv_sum_/v_sum_ subtraction)
// did not run, stale samples would remain in the sums and the value would be
// wrong.
static void test_vwma_window_eviction() {
    std::printf("test_vwma_window_eviction\n");

    ta::VWMA vwma(3);
    // (src, vol) pairs.
    CHECK(is_na(vwma.compute(10.0, 100.0)));  // size 1 < 3 -> na
    CHECK(is_na(vwma.compute(20.0, 200.0)));  // size 2 < 3 -> na

    // size hits 3 -> first finite value, window {(10,100),(20,200),(30,300)}.
    //   sv = 1000 + 4000 + 9000 = 14000 ; v = 600 -> 14000/600.
    double v3 = vwma.compute(30.0, 300.0);
    CHECK(near(v3, 14000.0 / 600.0));

    // size would be 4 -> while-loop evicts the oldest (10,100). New window
    //   {(20,200),(30,300),(40,400)} : sv = 4000+9000+16000 = 29000 ; v = 900.
    double v4 = vwma.compute(40.0, 400.0);
    CHECK(near(v4, 29000.0 / 900.0));         // 32.2222...
    // Cross-check: if the oldest were NOT evicted the value would instead be
    // the 4-sample mean 30000/1000 = 30.0; assert we are NOT seeing that.
    CHECK(!near(v4, 30000.0 / 1000.0));

    // Another bar -> evicts (20,200). Window {(30,300),(40,400),(50,500)} :
    //   sv = 9000+16000+25000 = 50000 ; v = 1200 -> 41.6666...
    double v5 = vwma.compute(50.0, 500.0);
    CHECK(near(v5, 50000.0 / 1200.0));
    CHECK(!near(v5, (16000.0 + 25000.0 + 9000.0 + 4000.0) / (400.0 + 500.0 + 300.0 + 200.0)));
}

// --------------------------------------------------------------------
// SMA::recompute na-input guard + empty-buffer dispatch (lines 306-310).
// --------------------------------------------------------------------
static void test_sma_recompute_guards() {
    std::printf("test_sma_recompute_guards\n");

    // (lines 306-307) na input to recompute returns na directly.
    ta::SMA sma(3);
    sma.compute(10.0);
    sma.compute(20.0);
    sma.compute(30.0);  // warmed up; value would be 20.0
    CHECK(is_na(sma.recompute(na<double>())));

    // (lines 308-310) empty-buffer recompute must dispatch to compute(). A
    // brand-new SMA with no compute history: recompute behaves exactly like
    // the first compute -> still in warmup -> na.
    ta::SMA fresh(3);
    CHECK(is_na(fresh.recompute(7.0)));
    // The dispatched compute() did push 7.0, so continuing to warm up works.
    CHECK(is_na(fresh.compute(8.0)));
    double v = fresh.compute(9.0);  // window {7,8,9} -> mean 8.0
    CHECK(near(v, 8.0));
}

int main() {
    test_rma_na_input_guard();
    test_sma_periodic_exact_sum_recompute();
    test_wma_na_input_guard();
    test_hma_na_guards();
    test_vwma_window_eviction();
    test_sma_recompute_guards();

    std::printf("\ntest_ta_ma_warmup_extra: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
