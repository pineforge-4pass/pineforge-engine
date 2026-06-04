/*
 * test_ta_extremes_edge.cpp — warmup na-guards + sliding-window eviction edges
 * for the range-extreme indicators in src/ta_extremes_volume.cpp.
 *
 * Targets the uncovered guard/eviction lines of Highest, Lowest, PivotHigh,
 * PivotLow, Median, HighestBars, LowestBars:
 *   - "fewer than period bars seen" -> na<double>()  (the size<length guards)
 *   - the sliding window evicting the oldest sample once full (buffer.pop_front
 *     in the `while (size > length)` loops) — proven by asserting that a value
 *     that should have been evicted no longer influences the output.
 *   - na input propagation (is_na(src) -> na out).
 *   - Median's EVEN-count two-middle-average branch, both in compute() and in
 *     recompute() (line 627), with an exact pinned median.
 *
 * NDEBUG-PROOF: uses a self-rolled CHECK that increments a global failure
 * counter and a main() that returns nonzero on any failure — independent of
 * assert()/NDEBUG. Verified non-vacuous by temporarily corrupting an expected
 * value and confirming the test fails, then restoring it.
 */

#include <cmath>
#include <cstdio>
#include <vector>

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

// --- Highest: warmup na, full-window value, eviction, na-input ---
static void test_highest_warmup_evict_na() {
    std::printf("test_highest_warmup_evict_na\n");
    ta::Highest hi(3);

    // Fewer than `length` bars -> na (covers size<length guard).
    CHECK(is_na(hi.compute(5.0)));   // size 1 < 3
    CHECK(is_na(hi.compute(9.0)));   // size 2 < 3

    // Third bar fills the window -> finite max over {5,9,2} = 9.
    double v = hi.compute(2.0);
    CHECK(!is_na(v));
    CHECK(near(v, 9.0));

    // Now slide: feeding three more small values must EVICT the 9 (pop_front).
    // If the window did not evict, max would stay 9.
    CHECK(near(hi.compute(1.0), 9.0));  // window {9,2,1} -> still 9
    CHECK(near(hi.compute(1.0), 2.0));  // window {2,1,1} -> 9 evicted, max 2
    CHECK(near(hi.compute(1.0), 1.0));  // window {1,1,1} -> all evicted, max 1

    // na input -> na out (covers is_na(src) guard) and must not perturb window.
    CHECK(is_na(hi.compute(na<double>())));
    CHECK(near(hi.compute(0.5), 1.0));  // window still {1,1,0.5} -> max 1
}

// --- Lowest: warmup na, full-window value, eviction, na-input ---
static void test_lowest_warmup_evict_na() {
    std::printf("test_lowest_warmup_evict_na\n");
    ta::Lowest lo(3);

    CHECK(is_na(lo.compute(5.0)));   // size 1 < 3
    CHECK(is_na(lo.compute(1.0)));   // size 2 < 3

    double v = lo.compute(9.0);      // window {5,1,9} -> min 1
    CHECK(!is_na(v));
    CHECK(near(v, 1.0));

    // Slide and evict the 1 (the running minimum).
    CHECK(near(lo.compute(8.0), 1.0));   // {1,9,8} -> still 1
    CHECK(near(lo.compute(7.0), 7.0));   // {9,8,7} -> 1 evicted, min 7
    CHECK(near(lo.compute(6.0), 6.0));   // {8,7,6} -> min 6

    CHECK(is_na(lo.compute(na<double>())));  // na in -> na out
    CHECK(near(lo.compute(10.0), 6.0));      // {7,6,10} -> min 6
}

// --- HighestBars: warmup na, offset semantics, eviction ---
static void test_highest_bars_offset_and_evict() {
    std::printf("test_highest_bars_offset_and_evict\n");
    ta::HighestBars hb(4);

    // Warmup: 3 bars < length 4 -> na.
    CHECK(is_na(hb.compute(1.0)));
    CHECK(is_na(hb.compute(2.0)));
    CHECK(is_na(hb.compute(3.0)));

    // 4th bar fills window {1,2,3,7}. Max is the most-recent bar (index 3),
    // offset = 3 - (4-1) = 0.
    CHECK(near(hb.compute(7.0), 0.0));

    // Window {2,3,7,4}: max 7 at index 2, offset = 2 - 3 = -1.
    CHECK(near(hb.compute(4.0), -1.0));
    // Window {3,7,4,5}: max 7 at index 1, offset = 1 - 3 = -2.
    CHECK(near(hb.compute(5.0), -2.0));
    // Window {7,4,5,6}: max 7 at index 0, offset = 0 - 3 = -3.
    CHECK(near(hb.compute(6.0), -3.0));
    // Window {4,5,6,3}: the 7 is EVICTED here (pop_front). New max 6 at idx 2,
    // offset = 2 - 3 = -1. If eviction did not occur, max would still be 7.
    CHECK(near(hb.compute(3.0), -1.0));

    // na in -> na out.
    CHECK(is_na(hb.compute(na<double>())));
}

// --- LowestBars: warmup na, offset semantics, eviction ---
static void test_lowest_bars_offset_and_evict() {
    std::printf("test_lowest_bars_offset_and_evict\n");
    ta::LowestBars lb(4);

    CHECK(is_na(lb.compute(9.0)));
    CHECK(is_na(lb.compute(8.0)));
    CHECK(is_na(lb.compute(7.0)));

    // Window {9,8,7,1}: min at index 3, offset = 3 - 3 = 0.
    CHECK(near(lb.compute(1.0), 0.0));
    // Window {8,7,1,5}: min 1 at index 2, offset = -1.
    CHECK(near(lb.compute(5.0), -1.0));
    // Window {7,1,5,6}: min 1 at index 1, offset = -2.
    CHECK(near(lb.compute(6.0), -2.0));
    // Window {1,5,6,4}: min 1 at index 0, offset = -3.
    CHECK(near(lb.compute(4.0), -3.0));
    // Window {5,6,4,3}: the 1 is EVICTED. New min 3 at index 3, offset = 0.
    CHECK(near(lb.compute(3.0), 0.0));

    CHECK(is_na(lb.compute(na<double>())));
}

// --- Median: warmup na, EVEN-count two-middle average (compute + recompute),
//     eviction, na input. ---
static void test_median_even_and_evict() {
    std::printf("test_median_even_and_evict\n");
    ta::Median med(4);  // EVEN length -> exercises (sorted[n/2-1]+sorted[n/2])/2

    // Warmup: 3 bars < 4 -> na.
    CHECK(is_na(med.compute(10.0)));
    CHECK(is_na(med.compute(30.0)));
    CHECK(is_na(med.compute(20.0)));

    // 4th bar: window {10,30,20,40} -> sorted {10,20,30,40} ->
    // median = (20 + 30) / 2 = 25 (the even-count branch).
    double m = med.compute(40.0);
    CHECK(!is_na(m));
    CHECK(near(m, 25.0));

    // recompute() with empty-check false (buffer non-empty): replaces the last
    // sample (40 -> 50). Window {10,30,20,50} -> sorted {10,20,30,50} ->
    // median = (20 + 30) / 2 = 25. Exercises the recompute even branch (line
    // 627). The two-middle pair is unchanged so the value is still 25, but a
    // single-middle (odd) branch would instead return sorted[2]=30.
    double mr = med.recompute(50.0);
    CHECK(!is_na(mr));
    CHECK(near(mr, 25.0));

    // Slide forward; the original 10 must be EVICTED (pop_front). Window after
    // feeding 60: {30,20,50,60} -> sorted {20,30,50,60} -> median (30+50)/2=40.
    // (recompute above left the window at {10,30,20,50}; compute now pushes 60
    //  and pops the front 10.)
    double m2 = med.compute(60.0);
    CHECK(near(m2, 40.0));

    // na in -> na out (and window unchanged).
    CHECK(is_na(med.compute(na<double>())));
}

// --- Median: recompute on an EMPTY buffer routes to compute (and stays na
//     until warmup), then odd-length sanity to anchor the even-vs-odd contrast.
static void test_median_recompute_empty_and_odd() {
    std::printf("test_median_recompute_empty_and_odd\n");
    {
        ta::Median med(2);
        // recompute() on empty buffer -> compute(): first bar size 1 < 2 -> na.
        CHECK(is_na(med.recompute(5.0)));
        // Second bar: window {5,7} -> sorted {5,7} -> even median (5+7)/2 = 6.
        CHECK(near(med.compute(7.0), 6.0));
    }
    {
        // Odd length -> single middle element (contrast with even branch).
        ta::Median med(3);
        CHECK(is_na(med.compute(10.0)));
        CHECK(is_na(med.compute(30.0)));
        // window {10,30,20} -> sorted {10,20,30} -> middle = 20.
        CHECK(near(med.compute(20.0), 20.0));
    }
}

// --- PivotHigh: warmup na (size<total), then a confirmed pivot, plus the
//     left/right rejection guards (covers the early na returns). ---
static void test_pivot_high_warmup_and_confirm() {
    std::printf("test_pivot_high_warmup_and_confirm\n");
    // left=1, right=1 -> total window size 3. Candidate is at index left=1.
    ta::PivotHigh ph(1, 1);

    // Warmup: fewer than total(3) bars -> na (size<total guard).
    CHECK(is_na(ph.compute(1.0)));  // size 1
    CHECK(is_na(ph.compute(5.0)));  // size 2

    // Window {1,5,2}: candidate 5 > left 1 and > right 2 -> confirmed pivot 5.
    CHECK(near(ph.compute(2.0), 5.0));

    // Window {5,2,4}: candidate 2 has left 5 > 2 -> LEFT guard rejects -> na.
    CHECK(is_na(ph.compute(4.0)));
    // Window {2,4,4}: candidate 4, right bar 4 >= 4 -> RIGHT strict guard -> na.
    CHECK(is_na(ph.compute(4.0)));
    // Window {4,4,3}: candidate 4, left 4 (4 > 4 false, allowed), right 3 < 4
    // -> confirmed pivot 4 (LEFT non-strict equal allowed).
    CHECK(near(ph.compute(3.0), 4.0));
}

// --- PivotLow: mirror of the above (LEFT non-strict, RIGHT strict). ---
static void test_pivot_low_warmup_and_confirm() {
    std::printf("test_pivot_low_warmup_and_confirm\n");
    ta::PivotLow pl(1, 1);  // total 3, candidate at index 1

    CHECK(is_na(pl.compute(9.0)));  // size 1
    CHECK(is_na(pl.compute(2.0)));  // size 2

    // Window {9,2,8}: candidate 2 < left 9 and < right 8 -> confirmed pivot 2.
    CHECK(near(pl.compute(8.0), 2.0));

    // Window {2,8,5}: candidate 8, left 2 < 8 -> LEFT guard rejects -> na.
    CHECK(is_na(pl.compute(5.0)));
    // Window {8,5,5}: candidate 5, right 5 <= 5 -> RIGHT strict guard -> na.
    CHECK(is_na(pl.compute(5.0)));
    // Window {5,5,6}: candidate 5, left 5 (5 < 5 false, allowed), right 6 > 5
    // -> confirmed pivot 5 (LEFT non-strict equal allowed).
    CHECK(near(pl.compute(6.0), 5.0));
}

int main() {
    test_highest_warmup_evict_na();
    test_lowest_warmup_evict_na();
    test_highest_bars_offset_and_evict();
    test_lowest_bars_offset_and_evict();
    test_median_even_and_evict();
    test_median_recompute_empty_and_odd();
    test_pivot_high_warmup_and_confirm();
    test_pivot_low_warmup_and_confirm();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
