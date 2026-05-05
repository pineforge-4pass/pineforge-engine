#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pineforge;

static constexpr double EPS = 1e-10;

static int tests_passed = 0;
static int tests_failed = 0;

static bool eq(double a, double b, double eps = EPS) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::abs(a - b) < eps;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        ++tests_failed; \
    } else { \
        ++tests_passed; \
    } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    double _a = (a); \
    double _b = (b); \
    if (!eq(_a, _b)) { \
        std::printf("FAIL: %s (line %d): %.12f != %.12f\n", msg, __LINE__, _a, _b); \
        ++tests_failed; \
    } else { \
        ++tests_passed; \
    } \
} while (0)

static double true_range_with_prev_close(double high, double low, double prev_close) {
    if (is_na(prev_close)) return na<double>();
    return std::max(high - low,
                    std::max(std::abs(high - prev_close), std::abs(low - prev_close)));
}

static void test_kc_uses_ema_of_true_range() {
    std::printf("test_kc_uses_ema_of_true_range\n");

    const int len = 3;
    const double mult = 1.0;

    ta::KC kc(len, mult);
    ta::EMA basis_ref(len);
    ta::EMA span_ema_ref(len);

    // Construct bars so TR sequence (after first bar) is [2,3,4,5].
    const std::vector<double> src   = {100, 100, 100, 100, 100};
    const std::vector<double> high  = {1,   2,   3,   4,   5};
    const std::vector<double> low   = {0,   0,   0,   0,   0};
    const std::vector<double> close = {0,   0,   0,   0,   0};

    double prev_close = na<double>();
    ta::KCResult out{};
    for (size_t i = 0; i < src.size(); ++i) {
        out = kc.compute(src[i], high[i], low[i], close[i]);

        double basis = basis_ref.compute(src[i]);
        double span = true_range_with_prev_close(high[i], low[i], prev_close);
        double range = span_ema_ref.compute(span);
        prev_close = close[i];

        if (is_na(basis) || is_na(range)) {
            CHECK(is_na(out.middle), "KC middle should be na during warmup");
            CHECK(is_na(out.upper), "KC upper should be na during warmup");
            CHECK(is_na(out.lower), "KC lower should be na during warmup");
            continue;
        }

        CHECK_EQ(out.middle, basis, "KC middle should equal EMA(src)");
        CHECK_EQ(out.upper, basis + range * mult, "KC upper should use EMA(TR)");
        CHECK_EQ(out.lower, basis - range * mult, "KC lower should use EMA(TR)");
    }

    // With len=3 and TRs [2,3,4,5] (first bar na), EMA(TR) uses Pine seeding
    // (first non-na) and ends at 4.125 on the last bar.
    // Basis is constant 100.0.
    CHECK_EQ(out.middle, 100.0, "final KC middle");
    CHECK_EQ(out.upper, 104.125, "final KC upper");
    CHECK_EQ(out.lower, 95.875, "final KC lower");
}

static void test_kc_recompute_matches_fresh_compute() {
    std::printf("test_kc_recompute_matches_fresh_compute\n");

    const int len = 3;
    const double mult = 1.5;

    ta::KC kc1(len, mult);
    ta::KC kc2(len, mult);

    const std::vector<double> src   = {100, 101, 102, 103, 104};
    const std::vector<double> high  = {1,   2,   3,   4,   5};
    const std::vector<double> low   = {0,   0,   0,   0,   0};
    const std::vector<double> close = {0,   0,   0,   0,   0};

    for (int i = 0; i < 4; ++i) {
        kc1.compute(src[i], high[i], low[i], close[i]);
        kc2.compute(src[i], high[i], low[i], close[i]);
    }

    // Compute with bar A then recompute with bar B.
    kc1.compute(src[4], high[4], low[4], close[4]);
    ta::KCResult r1 = kc1.recompute(src[4], 9.0, low[4], close[4]);

    // Fresh compute with bar B.
    ta::KCResult r2 = kc2.compute(src[4], 9.0, low[4], close[4]);

    CHECK_EQ(r1.middle, r2.middle, "KC recompute middle == fresh");
    CHECK_EQ(r1.upper, r2.upper, "KC recompute upper == fresh");
    CHECK_EQ(r1.lower, r2.lower, "KC recompute lower == fresh");
}

int main() {
    test_kc_uses_ema_of_true_range();
    test_kc_recompute_matches_fresh_compute();

    std::printf("kc: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
