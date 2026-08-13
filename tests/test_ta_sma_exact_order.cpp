// test_ta_sma_exact_order — ta.sma must be a FUNCTION OF ITS WINDOW.
//
// Pine defines ta.sma(src, length) as the mean of the last `length` values.
// That makes it a pure function of the window: two bars whose windows hold the
// same values must emit the same number, to the last bit, no matter what the
// series did before.
//
// An incremental running sum (`running_sum += src - popped`) breaks that. The
// emitted value becomes a function of every value ever accumulated, and a
// periodic exact re-summation only re-PHASES the drift instead of removing it.
//
// The failure is observable at the signal layer through the stochRSI shape
//
//     k = ta.sma(stoch, 3)
//     d = ta.sma(k, 3)
//
// Whenever the source plateaus, the three k-windows feeding d are rotations of
// a single multiset, so the s_{t-2} term cancels identically and
//
//     9 * (k_t - d_t) = 2*s_t + s_{t-1} - 2*s_{t-3} - s_{t-4}
//
// which is EXACTLY ZERO whenever s_t == s_{t-3} and s_{t-1} == s_{t-4}. stochRSI
// hits that constantly: the stoch value saturates at exactly 100.0 or 0.0
// (x/x -> 1.0 -> *100 is exact in IEEE-754) every time rsi is its own window
// extremum. ta.crossover / ta.crossunder are STRICT inequalities, so at such a
// bar no correct implementation at any precision may fire. Running-sum drift
// breaks the tie at +-1 ULP and manufactures crossings the mathematics forbids.
//
// These tests pin the invariant directly: equal windows -> bitwise equal
// output, and chained SMA(3) rotation ties stay exactly tied.

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
    return a == b;  // bit-exact: ULP tolerance would hide the drift entirely
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
// 1. Equal windows must produce bitwise-equal output regardless of history.
//
// Two SMA(3) instances are driven with DIFFERENT prefixes (one with values
// large enough to shred the low bits of a running accumulator), then converged
// onto the same final window. Pine semantics say they must agree exactly.
// ---------------------------------------------------------------------------
static void test_window_is_the_only_state() {
    ta::SMA a(3), b(3);

    // Prefix A: quiet.
    for (int i = 0; i < 40; ++i) a.compute(1.0);
    // Prefix B: a huge value first, so an incremental sum carries absorption
    // error that a window re-sum would never produce.
    b.compute(1.0e16);
    for (int i = 0; i < 39; ++i) b.compute(1.0);

    // Converge both onto the identical window {0.1, 0.2, 0.3}.
    double va = na<double>(), vb = na<double>();
    const double win[3] = {0.1, 0.2, 0.3};
    for (int i = 0; i < 3; ++i) { va = a.compute(win[i]); vb = b.compute(win[i]); }

    CHECK_EXACT(va, vb, "SMA(3): identical windows, different history -> equal");
    // And it must equal a fresh instance fed only that window.
    ta::SMA c(3);
    double vc = na<double>();
    for (int i = 0; i < 3; ++i) vc = c.compute(win[i]);
    CHECK_EXACT(va, vc, "SMA(3): matches a freshly-seeded instance");
}

// ---------------------------------------------------------------------------
// 2. The stochRSI tie: chained SMA(3) over a saturating plateau.
//
// Feed a stoch-like series that saturates at exactly 100.0 with one arbitrary
// unsaturated value threaded through, forming the rotation pattern
//     [.., a, b, x, a, b]
// At the final bar 9*(k-d) == 0 exactly, so k must equal d BITWISE and neither
// ta.crossover nor ta.crossunder may fire.
// ---------------------------------------------------------------------------
static void test_chained_sma3_rotation_tie_is_exact() {
    // A long saturated run first, so any running accumulator has drifted.
    std::vector<double> series;
    for (int i = 0; i < 300; ++i) series.push_back(100.0);
    // Rotation window: s[t-4]=100, s[t-3]=100, s[t-2]=x, s[t-1]=100, s[t]=100.
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
    CHECK_EXACT(k, d, "stochRSI rotation tie: k == d bitwise");
    CHECK_TRUE(!(k > d), "rotation tie: ta.crossover must not fire (k > d false)");
    CHECK_TRUE(!(k < d), "rotation tie: ta.crossunder must not fire (k < d false)");

    // Same shape on the 0.0 saturation side.
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
    CHECK_EXACT(kk, dd, "stochRSI rotation tie (0.0 side): k == d bitwise");
    CHECK_TRUE(!(kk > dd) && !(kk < dd), "0.0-side tie: no cross may fire");
}

// ---------------------------------------------------------------------------
// 3. A fully saturated plateau must hold k == d == the plateau level exactly,
//    at EVERY bar of the plateau -- not merely at a resync boundary.
//
// This is the case that made the engine's 256-bar resync observable: the
// spurious crossing appeared or vanished depending on where the plateau fell
// relative to (bar_count & 255) == 0.
// ---------------------------------------------------------------------------
static void test_saturated_plateau_never_crosses() {
    ta::SMA k_sma(3), d_sma(3);
    double k = na<double>(), d = na<double>();
    int crossings = 0;
    // Long enough to sweep several resync phases (256-bar period).
    for (int i = 0; i < 1200; ++i) {
        // Alternate 0.0 / 100.0 plateaus long enough for k and d to settle.
        double s = ((i / 37) % 2 == 0) ? 100.0 : 0.0;
        double pk = k, pd = d;
        k = k_sma.compute(s);
        d = d_sma.compute(k);
        if (is_na(k) || is_na(d) || is_na(pk) || is_na(pd)) continue;
        // Inside a settled plateau (k, d and the previous pair all at the same
        // saturation level) no crossing may occur.
        bool settled = (k == d) && (pk == pd);
        if (settled) continue;
        // Count real sign changes; these are legitimate plateau transitions.
        if ((k > d && pk <= pd) || (d > k && pd <= pk)) crossings++;
    }
    // 1200 bars / 37-bar plateaus ~ 32 transitions; the exact count is not the
    // point -- the point is that it is FINITE and driven by transitions, not by
    // hundreds of ULP flips inside flat plateaus.
    CHECK_TRUE(crossings < 80,
               "saturated plateaus: crossings driven by transitions, not ULP noise");
    std::printf("  (plateau sweep: %d crossings over 1200 bars)\n", crossings);
}

// ---------------------------------------------------------------------------
// 4. Ordinary SMA values are unchanged: the fix must not move real arithmetic.
// ---------------------------------------------------------------------------
static void test_plain_sma_values_unchanged() {
    ta::SMA s(3);
    CHECK_TRUE(is_na(s.compute(1.0)), "SMA(3) bar0 na");
    CHECK_TRUE(is_na(s.compute(2.0)), "SMA(3) bar1 na");
    CHECK_EXACT(s.compute(3.0), 2.0, "SMA(3) seed = (1+2+3)/3");
    CHECK_EXACT(s.compute(4.0), 3.0, "SMA(3) = (2+3+4)/3");
    CHECK_EXACT(s.compute(5.0), 4.0, "SMA(3) = (3+4+5)/3");

    // A long window still uses the incremental path; check it still tracks.
    ta::SMA big(50);
    double last = na<double>();
    for (int i = 1; i <= 50; ++i) last = big.compute((double)i);
    CHECK_EXACT(last, 25.5, "SMA(50) seed = mean(1..50)");
}

int main() {
    test_window_is_the_only_state();
    test_chained_sma3_rotation_tie_is_exact();
    test_saturated_plateau_never_crosses();
    test_plain_sma_values_unchanged();

    std::printf("test_ta_sma_exact_order: passed=%d failed=%d\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
