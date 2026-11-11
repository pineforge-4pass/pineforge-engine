/*
 * PVT's first valid bar has no previous close, so its output is na while
 * its cumulative sum remains zero. Later valid bars contribute their own
 * volume-weighted relative change; invalid inputs leave that history intact.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) <= tol;
}

struct Bar { double close, volume; };

// Independent forward sum over the invented bars; never calls PVT.
static std::vector<double> forward_sum(const std::vector<Bar>& bars) {
    std::vector<double> expected;
    double sum = 0.0;
    double previous = 0.0;
    bool have_previous = false;
    for (const auto& bar : bars) {
        double value = na<double>();
        if (!is_na(bar.close) && !is_na(bar.volume)) {
            if (have_previous) {
                if (previous != 0.0) {
                    sum += ((bar.close - previous) / previous) * bar.volume;
                }
                value = sum;
            }
            previous = bar.close;
            have_previous = true;
        }
        expected.push_back(value);
    }
    return expected;
}

static void check_sequence(const char* name, const std::vector<Bar>& bars) {
    std::printf("%s\n", name);
    const auto expected = forward_sum(bars);
    ta::PVT pvt;
    ta::PVT started_with_recompute;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        const double advanced = pvt.compute(bar.close, bar.volume);
        const double repeated = pvt.recompute(bar.close, bar.volume);
        // This instance starts with recompute before any compute. Its later
        // bars also verify that the initial recompute retained the first close.
        const double from_recompute = i == 0
            ? started_with_recompute.recompute(bar.close, bar.volume)
            : started_with_recompute.compute(bar.close, bar.volume);

        if (is_na(bar.close) || is_na(bar.volume)) {
            CHECK(is_na(advanced) && "ta.pvt invalid input must return na");
            CHECK(is_na(repeated) && "ta.pvt recompute of invalid input must return na");
            CHECK(is_na(from_recompute) && "ta.pvt started with recompute must return na for invalid input");
        } else if (is_na(expected[i])) {
            CHECK(is_na(advanced) && "ta.pvt must have no value on its first valid bar");
            CHECK(is_na(repeated) && "ta.pvt recompute must have no value on its first valid bar");
            CHECK(is_na(from_recompute) && "ta.pvt started with recompute must have no value on its first valid bar");
        } else {
            CHECK(near(advanced, expected[i]) && "ta.pvt must match the independent forward sum");
            CHECK(near(repeated, expected[i]) && "ta.pvt recompute must match the independent forward sum");
            CHECK(near(from_recompute, expected[i]) && "ta.pvt started with recompute must match the independent forward sum");
        }
    }
}

int main() {
    const double missing = std::numeric_limits<double>::quiet_NaN();

    // Different lengths and starting levels; both rise, fall and stay flat.
    check_sequence("test_pvt_short_forward_sum", {
        {8.0, 5.0}, {12.0, 9.0}, {9.0, 7.0}, {9.0, 4.0}, {15.0, 3.0},
    });
    check_sequence("test_pvt_long_forward_sum", {
        {30.0, 11.0}, {24.0, 5.0}, {36.0, 8.0}, {36.0, 3.0},
        {18.0, 7.0}, {27.0, 4.0}, {9.0, 6.0},
    });

    // The first valid bar, rather than the first call, lacks a value.
    check_sequence("test_pvt_first_valid_after_na_closes", {
        {missing, 4.0}, {missing, 9.0},
        {14.0, 6.0}, {7.0, 10.0}, {7.0, 3.0}, {21.0, 8.0},
    });

    // Missing volume must not seed a previous close. Both kinds of later na
    // preserve a nonzero sum and its previous valid close. A zero previous
    // close skips one term, including when the sum is already nonzero.
    check_sequence("test_pvt_invalid_inputs_and_zero_previous_close", {
        {17.0, missing}, {0.0, 6.0}, {4.0, 9.0}, {6.0, 5.0},
        {missing, 8.0}, {3.0, 4.0}, {21.0, missing},
        {0.0, 7.0}, {9.0, 2.0}, {12.0, 6.0},
    });

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
