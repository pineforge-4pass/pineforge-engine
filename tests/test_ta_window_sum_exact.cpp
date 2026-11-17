// test_ta_window_sum_exact -- the sliding-window sum behind ta.sma and
// math.sum is the exact sum of the sources in the window, rounded once, also
// when that sum needs more than two doubles.
//
// The window carries its sum in two doubles and adds or removes a source
// without error. When two doubles can not hold the exact sum, it sums the
// window from its own sources instead. This test drives that second path,
// which ordinary price series reach rarely: sources 2^200 apart, and sums that
// sit exactly on, just above and just below the midpoint of two doubles.
//
// Every expected value is a power of two or 0, so each one follows from the
// definition "exact sum, rounded to nearest, ties to even" without a
// calculation. They state the arithmetic's own property. They are not a
// TradingView observation: no capture holds sources this far apart.

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

    // 1. A source is not lost while a far larger one shares its window, and
    //    nothing is left of the large one after it has gone.
    {
        math::Sum sum(3);
        sum.compute(big);
        sum.compute(1.0);
        check(sum.compute(tiny), big, "2^100 + 1 + 2^-100 rounds to 2^100");
        check(sum.compute(0.0), 1.0, "1 + 2^-100 + 0 rounds to 1");
        check(sum.compute(0.0), tiny, "2^-100 + 0 + 0 is 2^-100");
        check(sum.compute(0.0), 0.0, "an all-zero window reads 0");
    }

    // 2. The midpoint of 1 and the next double: a third source far below
    //    decides the rounding, and with none the tie goes to the even double.
    {
        const double half_step = std::ldexp(1.0, -53);
        const double speck = std::ldexp(1.0, -110);
        const double next_after_one = 1.0 + std::ldexp(1.0, -52);

        math::Sum above(3);
        above.compute(1.0);
        above.compute(half_step);
        check(above.compute(speck), next_after_one, "1 + 2^-53 + 2^-110 rounds up");

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
