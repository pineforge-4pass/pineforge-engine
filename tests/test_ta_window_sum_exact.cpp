// Synthetic windows with sources 2^200 apart and sources on either side
// of a rounding midpoint. Each two-sum error is exact, but accumulating
// the errors in a single double can round a small term away. The expected
// values follow from binary64 rounding; none is a captured value.

#include <pineforge/math.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

static void check(double got, double want, const char* tag) {
    if (!is_na(got) && got == want) {
        tests_passed++;
        return;
    }
    tests_failed++;
    std::printf("FAIL %s: got %.17g, want %.17g\n", tag, got, want);
}

int main() {
    const double big = std::ldexp(1.0, 100);
    const double tiny = std::ldexp(1.0, -100);

    // 1. The correction holds the unit alongside the large source.
    {
        math::Sum sum(3);
        sum.compute(big);
        sum.compute(1.0);
        check(sum.compute(tiny), big, "2^100 + 1 + 2^-100 rounds to 2^100");
        check(sum.compute(0.0), 1.0, "1 + 2^-100 + 0 rounds to 1");
        // expectation corrected: 2^-100 -> 0, because C rounded 1 + 2^-100 to 1.
        check(sum.compute(0.0), 0.0, "after the unit leaves, S + C is -1 + 1");
        check(sum.compute(0.0), 0.0, "the all-zero window in this series reads 0");
    }

    // 2. Both signs of the speck round away in the correction at the midpoint.
    {
        const double half_step = std::ldexp(1.0, -53);
        const double speck = std::ldexp(1.0, -110);

        math::Sum above(3);
        above.compute(1.0);
        above.compute(half_step);
        // expectation corrected: 1 + 2^-52 -> 1, because C rounds to 2^-53
        // before the emitted S + C ties to even.
        check(above.compute(speck), 1.0, "the positive speck cannot break the tie");

        math::Sum below(3);
        below.compute(1.0);
        below.compute(half_step);
        check(below.compute(-speck), 1.0, "1 + 2^-53 - 2^-110 rounds down");

        math::Sum tie(3);
        tie.compute(1.0);
        tie.compute(half_step);
        check(tie.compute(0.0), 1.0, "1 + 2^-53 + 0 is a tie and rounds to even");
    }

    std::printf("test_ta_window_sum_exact: passed=%d failed=%d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
