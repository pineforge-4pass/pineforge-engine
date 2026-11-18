// test_ta_sma_exact_order -- ta.sma is TradingView's sliding-window sum,
// residue and all (re-pinned 2026-09-05, round 7).
//
// Round 6 pinned the opposite: "ta.sma is a function of its window", an
// exact-order re-sum so that chained SMA(3) rotation ties stayed exact and
// ta.crossover could never fire on them. The round-7 family-D diagnosis
// (the stochRSI probe, BTC/EURUSD/F@15) showed that TradingView DOES
// fire ta.crossover on exactly those algebraic ties -- on ~half of the flat
// descending ones, never on ascending ones -- decided by the sign of its own
// k - d residue, and the synthetic tapes (scratchpad r7/pins/sma-pulse-*,
// sma-ident-*; see tests/test_ta_sma_window_residual.cpp) pinned where that
// residue comes from: a Kahan-compensated RUNNING sum whose ring subtracts the
// compensated addend n bars later, re-summed newest-first only on the bar
// whose incoming value swallows the compensation term (window_sum.hpp).
//
// So the invariants below are the ones the tapes support:
//   1. the emitted value is a deterministic function of the whole series
//      (two instances fed the same history agree bit-for-bit), NOT of the
//      window alone -- the residue of what left the window is carried;
//   2. an all-zero window after a pulse reads the carried residue, with the
//      tape's exact values;
//   3. the rotation-tie shape after a settled plateau is an exact tie under
//      this arithmetic (TradingView's own value on a live series depends on
//      the state carried into the plateau -- the round-6 pin-sma-tie2 tape
//      has 179 exact ties and 257 residual-decided ones out of 436);
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
    return a == b;  // bit-exact: ULP tolerance would hide the residue entirely
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
// 1. Deterministic in the series; the window is NOT the only state.
// ---------------------------------------------------------------------------
static void test_series_is_the_state() {
    // Two instances, identical histories -> bitwise-equal output throughout.
    ta::SMA a(3), b(3);
    const double series[] = {0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.2, 0.3, 0.0, 0.0, 0.0, 0.7, 0.1, 0.3, 0.7, 0.0, 0.0};
    for (double v : series) {
        CHECK_EXACT(a.compute(v), b.compute(v), "SMA(3): identical histories agree bit-for-bit");
    }
    // A different prefix leaves a different residue on the SAME final window
    // (pinned: sma-pulse-n3 bar 40 reads (0.2 + 2^-55) / 3 because block 0's
    // residue 2^-55 rides into block 1's pulse).
    ta::SMA fresh(3), after_pulse(3);
    double vf = na<double>(), vp = na<double>();
    const double prefix[] = {0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.0};   // leaves S = 2^-55
    for (double v : prefix) after_pulse.compute(v);
    const double win[3] = {0.2, 0.0, 0.0};
    // the fresh instance sees only the window's newest value after two zeros
    fresh.compute(0.0); fresh.compute(0.0);
    vf = fresh.compute(0.2);
    for (double v : win) vp = after_pulse.compute(v);
    CHECK_EXACT(vf, 0.2 / 3, "fresh SMA(3) of {0, 0, 0.2} = 0.2 / 3");
    CHECK_EXACT(vp, 0.20000000000000004 / 3, "after a pulse the same window carries the residue: (0.2 + 2^-55) / 3");
}

// ---------------------------------------------------------------------------
// 2. An all-zero window reads the carried residue (tape sma-pulse-n3, block 0:
//    bars 6..39 encode qty 10000009 = round(s * 1e18) with s = 2^-55 / 3).
// ---------------------------------------------------------------------------
static void test_zero_window_carries_residue() {
    ta::SMA sma(3);
    sma.compute(0.1);
    sma.compute(0.3);
    std::vector<double> z;
    for (int i = 0; i < 38; ++i) z.push_back(sma.compute(0.0));   // bars 2..39
    // bar 2: (0.1 + 0.3 + 0) with the zero addend folding c: 0.4 - 2^-54 -> /3
    CHECK_EXACT(z[0], 0.13333333333333333, "bar 2");
    CHECK_EXACT(z[1], 0.09999999999999998, "bar 3: 0.29999999999999993 / 3");
    CHECK_EXACT(z[2], 0.0, "bar 4: exact 0");
    CHECK_EXACT(z[3] * 3, std::ldexp(1.0, -54), "bar 5: 2^-54");
    for (int i = 4; i < 38; ++i) {
        CHECK_EXACT(z[(std::size_t)i] * 3, std::ldexp(1.0, -55), "bars 6..39: 2^-55, not 0");
    }
}

// ---------------------------------------------------------------------------
// 3. The stochRSI rotation tie after a settled plateau.
//
// Feed a stoch-like series saturated at exactly 100.0 (or 0.0) with one
// unsaturated value threaded through, forming [.., a, b, x, a, b]. Under the
// fitted arithmetic the plateau's sums are exact, so k == d bitwise here and
// neither cross fires. (Whether TradingView's live k - d is exactly 0 on such
// a bar depends on the residue carried into the plateau; the arithmetic that
// decides it is the one pinned in test_ta_sma_window_residual.)
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
