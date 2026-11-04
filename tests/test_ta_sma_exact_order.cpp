// SMA: round the exact sum of window binary64 inputs once, then divide.
// the earlier pins disagree with TradingView's current values in the last binary digit; whether TradingView changed since 2026-09-05 is not known; the earlier pins are kept as history
// data/sma-oracles/history/pin-2026-09-05-capture-unconfirmed/.
// That date labels the source pin; the original export date is unconfirmed.

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <vector>
#include <iterator>
#include "sma_window_reference.hpp"

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
// 1. Identical windows agree independently of departed prefixes.
// ---------------------------------------------------------------------------
static void test_window_is_the_state() {
    // Two instances, identical histories -> bitwise-equal output throughout.
    ta::SMA a(3), b(3);
    const double series[] = {0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.2, 0.3, 0.0, 0.0, 0.0, 0.7, 0.1, 0.3, 0.7, 0.0, 0.0};
    const std::vector<double> inputs(std::begin(series), std::end(series));
    for (int i = 0; i < static_cast<int>(inputs.size()); ++i) {
        const double want = sma_test::dyadic_mean(inputs, i, 3, 56);
        CHECK_EXACT(a.compute(inputs[i]), want, "SMA(3): independent window mean A");
        CHECK_EXACT(b.compute(inputs[i]), want, "SMA(3): independent window mean B");
    }
    // The same final values in a different order and after a different
    // prefix have the same exact sum. The sole nonzero term is binary64 0.2.
    ta::SMA fresh(3), after_pulse(3);
    double vf = na<double>(), vp = na<double>();
    const double prefix[] = {0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.0};   // historical pulse prefix
    for (double v : prefix) after_pulse.compute(v);
    const double win[3] = {0.2, 0.0, 0.0};
    // the fresh instance sees only the window's newest value after two zeros
    fresh.compute(0.0); fresh.compute(0.0);
    vf = fresh.compute(0.2);
    for (double v : win) vp = after_pulse.compute(v);
    CHECK_EXACT(vf, 0.2 / 3, "fresh SMA(3) of {0, 0, 0.2} = 0.2 / 3");
    CHECK_EXACT(vp, 0.2 / 3, "departed pulse cannot change the final window");
}

// ---------------------------------------------------------------------------
// 2. Departed values contribute nothing to an all-zero window.
// ---------------------------------------------------------------------------
static void test_zero_window_has_no_residue() {
    ta::SMA sma(3);
    sma.compute(0.1);
    sma.compute(0.3);
    std::vector<double> z;
    for (int i = 0; i < 38; ++i) z.push_back(sma.compute(0.0));   // bars 2..39
    // The binary64 sum 0.1 + 0.3 is a tie that rounds to binary64 0.4.
    CHECK_EXACT(z[0], 0.4 / 3, "bar 2: round sum, then divide");
    CHECK_EXACT(z[1], 0.3 / 3, "bar 3: only binary64 0.3 remains");
    CHECK_EXACT(z[2], 0.0, "bar 4: exact 0");
    CHECK_EXACT(z[3], 0.0, "bar 5: exact 0");
    for (int i = 4; i < 38; ++i) {
        CHECK_EXACT(z[(std::size_t)i], 0.0, "bars 6..39: zero window");
    }
}

// ---------------------------------------------------------------------------
// 3. The stochRSI rotation tie after a settled plateau.
//
// The chained means use independently accumulated dyadic input windows.
// the earlier pins disagree with TradingView's current values in the last binary digit; whether TradingView changed since 2026-09-05 is not known; the earlier pins are kept as history
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
    const double want_k = sma_test::dyadic_mean(series, static_cast<int>(series.size()) - 1, 3, 49);
    CHECK_EXACT(k, want_k, "rotation tie: independently derived k");
    CHECK_EXACT(d, (want_k * 3.0) / 3.0, "rotation tie: three equal rounded k inputs");
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
    const double want_kk = sma_test::dyadic_mean(zseries, static_cast<int>(zseries.size()) - 1, 3, 49);
    CHECK_EXACT(kk, want_kk, "rotation tie (0.0 side): independently derived k");
    CHECK_EXACT(dd, (want_kk * 3.0) / 3.0, "rotation tie (0.0 side): derived d");
    CHECK_EXACT(kk, dd, "rotation tie after a settled 0-plateau: k == d bitwise");
}

// ---------------------------------------------------------------------------
// 4. Alternating saturated plateaus: crossings are driven by transitions.
// ---------------------------------------------------------------------------
static void test_saturated_plateau_sweep() {
    ta::SMA k_sma(3), d_sma(3);
    double k = na<double>(), d = na<double>();
    int crossings = 0;
    std::vector<double> inputs, expected_k;
    for (int i = 0; i < 1200; ++i) {
        double s = ((i / 37) % 2 == 0) ? 100.0 : 0.0;
        double pk = k, pd = d;
        k = k_sma.compute(s);
        d = d_sma.compute(k);
        inputs.push_back(s);
        expected_k.push_back(sma_test::dyadic_mean(inputs, i, 3, 49));
        CHECK_EXACT(k, expected_k.back(), "plateau: independent k window");
        const double want_d = i < 4 ? na<double>() :
            sma_test::dyadic_mean(expected_k, i, 3, 49);
        CHECK_EXACT(d, want_d, "plateau: independent d window");
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
    test_window_is_the_state();
    test_zero_window_has_no_residue();
    test_chained_sma3_rotation_tie();
    test_saturated_plateau_sweep();
    test_plain_sma_values_unchanged();

    std::printf("test_ta_sma_exact_order: passed=%d failed=%d\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
