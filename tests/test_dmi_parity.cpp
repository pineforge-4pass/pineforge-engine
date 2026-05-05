#include <cmath>
#include <cstdio>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

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

int main() {
    test_dmi_first_bar_returns_na();
    test_dmi_true_range_uses_prev_close();
    test_dmi_recompute_prev_close_consistent();
    std::printf("dmi_parity: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
