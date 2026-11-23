// test_ta_sma_exact_order -- pulse, plateau and rotation observations for
// fixed SMA. These cases exercise the November compensated sum.
//
// History, kept because the assertions below were written against it.
// Round 6 pinned "ta.sma is a function of its window", an exact-order re-sum
// so that chained SMA(3) rotation ties stayed exact and ta.crossover could
// never fire on them. Round 7 (2026-09-05, the stochRSI probe,
// BTC/EURUSD/F@15) pinned the opposite: TradingView DID fire ta.crossover on
// those algebraic ties -- on ~half of the flat descending ones, never on
// ascending ones -- decided by the sign of its own k - d residue, and the
// synthetic pulse and identity tapes (see
// tests/test_ta_sma_window_residual.cpp) pinned where that residue came from:
// a Kahan-compensated RUNNING sum whose ring subtracted the compensated addend
// n bars later. The captures of 2026-11-16 and 2026-11-17 read the window's
// own sum again on the measured bars: the earlier residue is absent there.
// This does not establish an exact sum for every input. Each changed pin cites
// its capture where it stands.
//
// So the invariants below are the ones the newest captures support:
//   1. the emitted value is a deterministic function of the series (two
//      instances fed the same history agree bit-for-bit), and the measured
//      prefix leaves nothing behind in the selected window;
//   2. the all-zero windows after this pulse read 0;
//   3. the rotation-tie shape after a settled plateau is an exact tie (in
//      September TradingView's own value on a live series depended on the
//      state carried into the plateau -- one round-6 tape has
//      179 exact ties and 257 residual-decided ones out of 436);
//   4. ordinary small-integer windows are unchanged (their sums are exact).

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

static bool exact_eq(double a, double b) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return a == b && std::signbit(a) == std::signbit(b);
}

#define CHECK_EXACT(actual, expected, tag) do {                               \
    double _a = (actual);                                                     \
    double _e = (expected);                                                   \
    if (exact_eq(_a, _e)) { tests_passed++; }                                 \
    else {                                                                    \
        tests_failed++;                                                       \
        std::printf("FAIL %s: got %.17g want %.17g (diff %.3g)\n",            \
                    (tag), _a, _e, _a - _e);                                  \
    }                                                                         \
} while (0)

#define CHECK_TRUE(cond, tag) do {                                            \
    if (cond) { tests_passed++; }                                             \
    else { tests_failed++; std::printf("FAIL %s\n", (tag)); }                 \
} while (0)

// ---------------------------------------------------------------------------
// 1. Deterministic in the series; the selected prefix leaves no residue on
//    this observation.
// ---------------------------------------------------------------------------
static void test_series_is_the_state() {
    // Two instances, identical histories -> bitwise-equal output throughout.
    ta::SMA a(3), b(3);
    const double series[] = {0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.2, 0.3, 0.0, 0.0, 0.0, 0.7, 0.1, 0.3, 0.7, 0.0, 0.0};
    for (double v : series) {
        CHECK_EXACT(a.compute(v), b.compute(v), "SMA(3): identical histories agree bit-for-bit");
    }
    // The same three values in a different order read the same value here.
    // (Until the capture of 2026-11-17 this line was pinned the other way: the
    // round-7 pulse tape for n = 3 read (0.2 + 2^-55) / 3 on its bar 40, a
    // residue of block 0 riding into block 1's pulse.)
    ta::SMA fresh(3), after_pulse(3);
    double vf = na<double>(), vp = na<double>();
    const double prefix[] = {0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (double v : prefix) after_pulse.compute(v);
    const double win[3] = {0.2, 0.0, 0.0};
    // the fresh instance sees only the window's newest value after two zeros
    fresh.compute(0.0); fresh.compute(0.0);
    vf = fresh.compute(0.2);
    for (double v : win) vp = after_pulse.compute(v);
    CHECK_EXACT(vf, 0.2 / 3, "fresh SMA(3) of {0, 0, 0.2} = 0.2 / 3");
    // expectation corrected: 0.20000000000000004 / 3 -> 0.2 / 3, because
    // TradingView's sliding-window sum changed between two captures and the
    // newest capture wins. These ten bars (0.1, 0.3, five zeros, 0.2, 0, 0;
    // length 3) were captured as exact doubles on 2026-11-17: on the tenth bar
    // ta.sma reads 0x1.1111111111111p-4, and the capture's own reference
    // ((x + x[1]) + x[2]) / 3 reads the same double (their difference is an
    // exact zero). The expected value is that reference, 0.2 / 3. It is
    // TradingView's double; it was not taken from this library's output.
    //   OLD    the round-7 pulse tapes of 2026-09-05 (no digest recorded)
    //   TODAY  the ten-bar capture of 2026-11-17 (the capture evidence is kept
    //          privately)
    // Re-pin rule: this line holds TradingView's double on that bar. A later
    // capture of the same ten bars that reads another double replaces it: by
    // the reference expression while ta.sma equals the capture's own reference,
    // by the captured double otherwise, each time with a new "expectation
    // corrected" note that cites both captures by date.
    CHECK_EXACT(vp, 0.2 / 3, "after a pulse the same window reads 0.2 / 3: nothing of the prefix is left");
}

// ---------------------------------------------------------------------------
// 2. An all-zero window reads 0 after a pulse has left it. Until the capture of
//    2026-11-16 it read the carried residue (pulse tape for n = 3, block 0:
//    bars 6..39 encoded qty 10000009 = round(s * 1e18) with s = 2^-55 / 3).
// ---------------------------------------------------------------------------
static void test_zero_window_carries_residue() {
    ta::SMA sma(3);
    sma.compute(0.1);
    sma.compute(0.3);
    std::vector<double> z;
    for (int i = 0; i < 38; ++i) z.push_back(sma.compute(0.0));   // bars 2..39
    // Both rules round to the captured SMA on bar 2.
    CHECK_EXACT(z[0], 0.13333333333333333, "bar 2");
    // expectation corrected on bars 3, 5 and 6..39 below, because TradingView's
    // sliding-window sum changed between two captures of this series (it is bars
    // 0..39 of the n = 3 pulse series) and the newest capture wins.
    //   OLD    the round-7 pulse tapes of 2026-09-05 (no digest recorded)
    //   TODAY  the pulse capture of 2026-11-16 (codes of bars 2..45; the capture
    //          evidence is kept privately)
    // expectation corrected: 0.09999999999999998 -> 0.3 / 3, because TradingView's
    // pulse code of bar 3 is 50000000 today (49999999 in September): its value is
    // the reference ((x + x[1]) + x[2]) / 3 = 0.3 / 3, and either neighbouring
    // double would read 49999999 or 50000001.
    CHECK_EXACT(z[1], 0.3 / 3, "bar 3: the window (0.3, 0, 0) reads 0.3 / 3");
    CHECK_EXACT(z[2], 0.0, "bar 4: exact 0");
    // expectation corrected: 2^-54 / 3 -> 0, because TradingView's pulse code of
    // bar 5 is 10000000 today (10000019 in September): the smallest sum these
    // sources can leave behind, 2^-56, would read 10000005.
    CHECK_EXACT(z[3], 0.0, "bar 5: an all-zero window reads 0");
    for (int i = 4; i < 38; ++i) {
        // expectation corrected: 2^-55 / 3 -> 0, because TradingView's pulse codes
        // of bars 6..39 are 10000000 today (10000009 in September).
        CHECK_EXACT(z[(std::size_t)i], 0.0, "bars 6..39: an all-zero window reads 0");
    }
}

// ---------------------------------------------------------------------------
// 3. The stochRSI rotation tie after a settled plateau.
//
// Feed a stoch-like series saturated at exactly 100.0 (or 0.0) with one
// unsaturated value threaded through, forming [.., a, b, x, a, b]. The
// plateau's sums are exact, so k == d bitwise here and neither cross fires.
// (In September TradingView's live k - d on such a bar depended on the residue
// carried into the plateau. In the captures of 2026-11-17 its k
// and d read exactly 100 and exactly 0 on such plateaus; the two tie-cell
// tests replay those bars.)
// ---------------------------------------------------------------------------
static void test_chained_sma3_rotation_tie() {
    std::vector<double> series;
    for (int i = 0; i < 300; ++i) series.push_back(100.0);
    const double x = 37.912345678901234;
    series.push_back(100.0);
    series.push_back(100.0);
    series.push_back(x);
    series.push_back(100.0);
    series.push_back(100.0);

    ta::SMA k_sma(3), d_sma(3);
    double k = na<double>(), d = na<double>();
    for (double s : series) {
        k = k_sma.compute(s);
        d = d_sma.compute(k);
    }
    CHECK_EXACT(k, 79.30411522630041, "rotation tie: k");
    CHECK_EXACT(k, d, "rotation tie after a settled 100-plateau: k == d bitwise");
    CHECK_TRUE(!(k > d) && !(k < d), "rotation tie: no cross fires");

    std::vector<double> zseries;
    for (int i = 0; i < 300; ++i) zseries.push_back(0.0);
    zseries.push_back(0.0);
    zseries.push_back(0.0);
    zseries.push_back(x);
    zseries.push_back(0.0);
    zseries.push_back(0.0);

    ta::SMA k0(3), d0(3);
    double kk = na<double>(), dd = na<double>();
    for (double s : zseries) { kk = k0.compute(s); dd = d0.compute(kk); }
    CHECK_EXACT(kk, 12.637448559633745, "rotation tie (0.0 side): k");
    CHECK_EXACT(kk, dd, "rotation tie after a settled 0-plateau: k == d bitwise");
}

// ---------------------------------------------------------------------------
// 4. Alternating saturated plateaus: crossings are driven by transitions.
// ---------------------------------------------------------------------------
static void test_saturated_plateau_sweep() {
    ta::SMA k_sma(3), d_sma(3);
    double k = na<double>(), d = na<double>();
    int crossings = 0;
    for (int i = 0; i < 1200; ++i) {
        double s = ((i / 37) % 2 == 0) ? 100.0 : 0.0;
        double pk = k, pd = d;
        k = k_sma.compute(s);
        d = d_sma.compute(k);
        if (is_na(k) || is_na(d) || is_na(pk) || is_na(pd)) continue;
        bool settled = (k == d) && (pk == pd);
        if (settled) continue;
        if ((k > d && pk <= pd) || (d > k && pd <= pk)) crossings++;
    }
    CHECK_TRUE(crossings == 32, "alternating 37-bar plateaus over 1200 bars: 32 crossings (the transitions)");
    std::printf("  (plateau sweep: %d crossings over 1200 bars)\n", crossings);
}

// ---------------------------------------------------------------------------
// 5. Ordinary small-integer windows are unchanged: their sums are exact.
// ---------------------------------------------------------------------------
static void test_plain_sma_values_unchanged() {
    ta::SMA s(3);
    CHECK_TRUE(is_na(s.compute(1.0)), "SMA(3) bar0 na");
    CHECK_TRUE(is_na(s.compute(2.0)), "SMA(3) bar1 na");
    CHECK_EXACT(s.compute(3.0), 2.0, "SMA(3) seed = (1+2+3)/3");
    CHECK_EXACT(s.compute(4.0), 3.0, "SMA(3) = (2+3+4)/3");
    CHECK_EXACT(s.compute(5.0), 4.0, "SMA(3) = (3+4+5)/3");

    ta::SMA big(50);
    double last = na<double>();
    for (int i = 1; i <= 50; ++i) last = big.compute((double)i);
    CHECK_EXACT(last, 25.5, "SMA(50) seed = mean(1..50)");
    for (int i = 51; i <= 200; ++i) last = big.compute((double)i);
    CHECK_EXACT(last, 175.5, "SMA(50) of 151..200 = 175.5 (integer sums stay exact)");
}

int main() {
    test_series_is_the_state();
    test_zero_window_carries_residue();
    test_chained_sma3_rotation_tie();
    test_saturated_plateau_sweep();
    test_plain_sma_values_unchanged();

    std::printf("test_ta_sma_exact_order: passed=%d failed=%d\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
