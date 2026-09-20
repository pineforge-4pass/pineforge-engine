#include <cmath>
#include <cstdio>
#include <limits>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta_compare_band.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) < tol;
}

static void test_dmi_first_bar_returns_na() {
    std::printf("test_dmi_first_bar_returns_na\n");
    ta::DMI dmi(1, 1);
    ta::DMIResult r = dmi.compute(100.0, 95.0, 100.0);
    CHECK(is_na(r.diplus));
    CHECK(is_na(r.diminus));
    CHECK(is_na(r.adx));
}

// Bar0 then Bar1 where TR with prev_close (100) differs from TR using current close (150).
// Buggy TR used |H-C| and |L-C| with C=150 -> max TR 60; Pine TR -> 25 -> DI+ = 60 vs 25.
static void test_dmi_true_range_uses_prev_close() {
    std::printf("test_dmi_true_range_uses_prev_close\n");
    ta::DMI dmi(1, 1);
    dmi.compute(100.0, 95.0, 100.0);
    ta::DMIResult r = dmi.compute(115.0, 90.0, 150.0);
    CHECK(near(r.diplus, 60.0, 1e-9));
    CHECK(near(r.diminus, 0.0, 1e-9));
    CHECK(near(r.adx, 100.0, 1e-9));
}

static void test_dmi_recompute_prev_close_consistent() {
    std::printf("test_dmi_recompute_prev_close_consistent\n");
    ta::DMI a(1, 1);
    ta::DMI b(1, 1);
    a.compute(100.0, 95.0, 100.0);
    b.compute(100.0, 95.0, 100.0);
    ta::DMIResult r1 = a.compute(115.0, 90.0, 150.0);
    b.compute(115.0, 90.0, 150.0);
    ta::DMIResult r2 = b.recompute(115.0, 90.0, 150.0);
    CHECK(near(r1.diplus, r2.diplus));
    CHECK(near(r1.diminus, r2.diminus));
    CHECK(near(r1.adx, r2.adx));
}

// --- Pine's tolerant relational inside ta.dmi (round 7 family K) ----------
//
// TradingView's `up > down` / `down > up` / `up > 0` inside ta.dmi are Pine
// float relationals: |a - b| <= 1e-10 is EQUAL (inclusive, absolute), so an
// exact-decimal tie books NEITHER directional movement. Pinned on NYSE:F 15m
// (`lab tv` f15-plusdm-perbar-0714, one qty-encoded lot per bar over
// 2025-07-14..18; f15-dmi-internals-0718 / f15-wma-adx-sense-0718 for the
// smoothed terms): the 07-15 19:30Z bar (prev 19:15Z h 11.565 l 11.54;
// 19:30Z h 11.58 l 11.525) has up = 11.58 - 11.565 and down = 11.54 - 11.525,
// both 0.015 in decimal and 1.78e-15 apart as doubles; TV's plusDM is 0
// (qty 1 = round(0 * 1e6) + 1) where the strict compare booked 0.015, and
// TV's rma(plusDM, 18) at 07-18 14:15Z is 0.006907 (strict model 0.006939),
// +DI 16.92562 (17.00417), ADX 24.22236 (23.571) — the bar that fires the
// boztilkiserhan ADX > 24 short on TV only.
static void test_dmi_exact_decimal_tie_books_neither_dm() {
    std::printf("test_dmi_exact_decimal_tie_books_neither_dm\n");
    // di_length 1: rma(1) is the value itself, so +DI / -DI read the bar's
    // own plusDM / minusDM over its true range.
    ta::DMI dmi(1, 1);
    dmi.compute(11.565, 11.54, 11.545);            // 2025-07-15 19:15Z
    ta::DMIResult r = dmi.compute(11.58, 11.525, 11.575);  // 19:30Z
    // The strict compare hands 0.015 to one side (here up > down in IEEE:
    // 11.58 - 11.565 = 0.015000000000000568 > 11.54 - 11.525 =
    // 0.014999999999998792); Pine's tolerant compare books a tie.
    CHECK((11.58 - 11.565) != (11.54 - 11.525));
    CHECK((r.diplus == 0.0));
    CHECK((r.diminus == 0.0));
    CHECK((r.adx == 0.0));
}

static void test_dmi_clear_difference_is_unchanged() {
    std::printf("test_dmi_clear_difference_is_unchanged\n");
    // Control: up = 0.025, down = 0.015 -> plusDM 0.025, minusDM 0; TR =
    // max(11.59 - 11.525, |11.59 - 11.545|, |11.525 - 11.545|) = 0.065.
    ta::DMI dmi(1, 1);
    dmi.compute(11.565, 11.54, 11.545);
    ta::DMIResult r = dmi.compute(11.59, 11.525, 11.575);
    CHECK(near(r.diplus, 100.0 * (11.59 - 11.565) / (11.59 - 11.525), 1e-9));
    CHECK((r.diminus == 0.0));
    CHECK(near(r.adx, 100.0, 1e-9));
    // And the mirror: down clearly larger -> minusDM only.
    ta::DMI dmi2(1, 1);
    dmi2.compute(11.565, 11.54, 11.545);
    ta::DMIResult r2 = dmi2.compute(11.575, 11.51, 11.52);
    CHECK((r2.diplus == 0.0));
    CHECK(near(r2.diminus, 100.0 * (11.54 - 11.51) / (11.575 - 11.51), 1e-9));
}

static void test_dmi_up_within_band_of_zero_is_not_up() {
    std::printf("test_dmi_up_within_band_of_zero_is_not_up\n");
    // `up > 0` is tolerant too: a 5e-11 up-move (sub-band) is no movement,
    // and with down = 0 the up/down pair is a tie as well -> both 0.
    ta::DMI dmi(1, 1);
    dmi.compute(10.0, 9.0, 9.5);
    ta::DMIResult r = dmi.compute(10.0 + 5e-11, 9.0, 9.5);
    CHECK((10.0 + 5e-11) - 10.0 > 0.0);   // strictly positive in IEEE
    CHECK((r.diplus == 0.0));
    CHECK((r.diminus == 0.0));
    // 1.5e-10 is outside the band: booked.
    ta::DMI dmi2(1, 1);
    dmi2.compute(10.0, 9.0, 9.5);
    ta::DMIResult r2 = dmi2.compute(10.0 + 1.5e-10, 9.0, 9.5);
    CHECK(r2.diplus > 0.0);
    CHECK((r2.diminus == 0.0));
}

// The band itself, replayed from the two `lab tv` sensor tapes on NYSE:F 15m
// 2025-07-10..18 (scratchpad/r7/pins/f15-eps-abs / f15-eps-rel, qty = 1000 +
// 100*(a>b) + 10*(a==b) + (a<b) + 10000*k): 20/20 decisions.
static void test_float_compare_band_replays_the_sensor_tapes() {
    std::printf("test_float_compare_band_replays_the_sensor_tapes\n");
    auto code = [](double a, double b) {
        return 100 * (float_band_gt(a, b) ? 1 : 0)
             + 10 * (float_band_eq(a, b) ? 1 : 0)
             + (float_band_lt(a, b) ? 1 : 0);
    };
    // abs: a = m + d, b = m.
    const double m = 11.5;
    CHECK(code(m + 1e-8, m) == 100);     // k0
    CHECK(code(m + 1e-9, m) == 100);     // k1
    CHECK(code(m + 1e-10, m) == 100);    // k2: the double diff is 1e-10 + 1 ulp
    CHECK(code(m + 1e-11, m) == 10);     // k3
    CHECK(code(m + 1e-12, m) == 10);     // k4
    CHECK(code(m + 1e-13, m) == 10);     // k5
    CHECK(code(m + 1e-14, m) == 10);     // k6
    CHECK(code(1.0 + 1e-10, 1.0) == 100);        // k7 (diff 1.0000000827e-10)
    CHECK(code(1000.0 + 1e-10, 1000.0) == 100);  // k8 (1.00044e-10)
    CHECK(code(100000.0 + 1e-10, 100000.0) == 100);  // k9 (1.0186e-10)
    // rel: a fixed relative diff 1e-11 is absolute-banded, not relative.
    CHECK(code(0.01 * (1 + 1e-11), 0.01) == 10);         // k0 diff 1e-13
    CHECK(code(1.0 * (1 + 1e-11), 1.0) == 10);           // k1 diff 1e-11
    CHECK(code(100.0 * (1 + 1e-11), 100.0) == 100);      // k2 diff 1e-9
    CHECK(code(10000.0 * (1 + 1e-11), 10000.0) == 100);  // k3
    CHECK(code(1000000.0 * (1 + 1e-11), 1000000.0) == 100);  // k4
    CHECK(code(11.58 - 11.565, 11.54 - 11.525) == 10);   // k5: the DMI tie
    CHECK(code(0.1 + 0.2, 0.3) == 10);                   // k6
    CHECK(code(1e-10, 0.0) == 10);                       // k7: inclusive edge
    CHECK(code(1.1e-10, 0.0) == 100);                    // k8
    CHECK(code(0.9e-10, 0.0) == 10);                     // k9
    // na makes every relational false, != included.
    CHECK(!float_band_gt(na<double>(), 1.0));
    CHECK(!float_band_lt(1.0, na<double>()));
    CHECK(!float_band_eq(na<double>(), na<double>()));
    CHECK(!float_band_ne(na<double>(), 1.0));
    CHECK(float_band_ge(1.0 + 1e-11, 1.0) && float_band_le(1.0 + 1e-11, 1.0));
    CHECK(float_band_ne(1.0 + 1.1e-10, 1.0));
    // Infinities compare by value.
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(float_band_eq(inf, inf) && float_band_gt(inf, 1.0) && float_band_lt(-inf, inf));
}

int main() {
    test_dmi_first_bar_returns_na();
    test_dmi_true_range_uses_prev_close();
    test_dmi_recompute_prev_close_consistent();
    test_dmi_exact_decimal_tie_books_neither_dm();
    test_dmi_clear_difference_is_unchanged();
    test_dmi_up_within_band_of_zero_is_not_up();
    test_float_compare_band_replays_the_sensor_tapes();
    std::printf("dmi_parity: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
