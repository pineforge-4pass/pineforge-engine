/*
 * OBV's first valid bar has no previous close, so its output is na while
 * its cumulative sum remains zero. Later valid bars add or subtract their
 * volume according to the direction of the close.
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

// Independent forward sum over the invented bars; never calls OBV.
static std::vector<double> forward_sum(const std::vector<Bar>& bars) {
    std::vector<double> expected;
    double sum = 0.0;
    double previous = 0.0;
    bool have_previous = false;
    for (const auto& bar : bars) {
        double value = na<double>();
        if (!is_na(bar.close) && !is_na(bar.volume)) {
            if (have_previous) {
                if (bar.close > previous) sum += bar.volume;
                else if (bar.close < previous) sum -= bar.volume;
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
    ta::OBV obv;
    ta::OBV started_with_recompute;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        const double advanced = obv.compute(bar.close, bar.volume);
        const double repeated = obv.recompute(bar.close, bar.volume);
        // This instance starts with recompute before any compute. Its later
        // bars also verify that the initial recompute retained the first close.
        const double from_recompute = i == 0
            ? started_with_recompute.recompute(bar.close, bar.volume)
            : started_with_recompute.compute(bar.close, bar.volume);

        if (is_na(bar.close) || is_na(bar.volume)) {
            CHECK(is_na(advanced) && "ta.obv invalid input must return na");
            CHECK(is_na(repeated) && "ta.obv recompute of invalid input must return na");
            CHECK(is_na(from_recompute) && "ta.obv started with recompute must return na for invalid input");
        } else if (is_na(expected[i])) {
            CHECK(is_na(advanced) && "ta.obv must have no value on its first valid bar");
            CHECK(is_na(repeated) && "ta.obv recompute must have no value on its first valid bar");
            CHECK(is_na(from_recompute) && "ta.obv started with recompute must have no value on its first valid bar");
        } else {
            CHECK(near(advanced, expected[i]) && "ta.obv must match the independent forward sum");
            CHECK(near(repeated, expected[i]) && "ta.obv recompute must match the independent forward sum");
            CHECK(near(from_recompute, expected[i]) && "ta.obv started with recompute must match the independent forward sum");
        }
    }
}

static void check_recompute_replacement(const char* name,
                                       const std::vector<Bar>& history,
                                       const Bar& original,
                                       const Bar& replacement,
                                       const std::vector<Bar>& following) {
    std::printf("%s\n", name);
    auto bars = history;
    bars.push_back(replacement);
    bars.insert(bars.end(), following.begin(), following.end());
    const auto expected = forward_sum(bars);
    ta::OBV revised;
    ta::OBV fresh;
    for (const auto& bar : history) {
        revised.compute(bar.close, bar.volume);
        fresh.compute(bar.close, bar.volume);
    }
    revised.compute(original.close, original.volume);
    const double replaced = revised.recompute(replacement.close, replacement.volume);
    const double repeated = revised.recompute(replacement.close, replacement.volume);
    const double advanced = fresh.compute(replacement.close, replacement.volume);
    CHECK(near(replaced, expected[history.size()]) && "ta.obv changed recompute must match the independent forward sum");
    CHECK(near(repeated, expected[history.size()]) && "ta.obv repeated changed recompute must match the independent forward sum");
    CHECK(near(advanced, expected[history.size()]) && "ta.obv fresh compute must match the independent forward sum");
    CHECK(near(replaced, advanced) && "ta.obv changed recompute must match a fresh compute with the same history");

    std::size_t i = history.size() + 1;
    for (const auto& bar : following) {
        CHECK(near(revised.compute(bar.close, bar.volume), expected[i]) && "ta.obv changed recompute must retain the replacement history");
        CHECK(near(fresh.compute(bar.close, bar.volume), expected[i]) && "ta.obv fresh history must match the independent forward sum");
        ++i;
    }
}

int main() {
    const double missing = std::numeric_limits<double>::quiet_NaN();

    // Different lengths and starting levels; both rise, fall and stay flat.
    check_sequence("test_obv_short_forward_sum", {
        {11.0, 5.0}, {17.0, 9.0}, {14.0, 7.0}, {14.0, 4.0}, {20.0, 3.0},
    });
    check_sequence("test_obv_long_forward_sum", {
        {42.0, 13.0}, {33.0, 6.0}, {33.0, 10.0}, {48.0, 8.0},
        {21.0, 17.0}, {30.0, 4.0}, {18.0, 11.0},
    });

    // The first valid bar, rather than the first call, lacks a value.
    check_sequence("test_obv_first_valid_after_na_closes", {
        {missing, 4.0}, {missing, 9.0},
        {22.0, 0.0}, {11.0, 10.0}, {11.0, 3.0}, {33.0, 8.0},
    });

    // Missing volume returns na without changing state in the engine today;
    // this preserves current behaviour and makes no claim about TradingView.
    // Missing closes also preserve history. Zero volume still records its
    // close, and a zero previous close remains a valid comparison.
    check_sequence("test_obv_invalid_inputs_and_zero_volume", {
        {17.0, missing}, {0.0, 6.0}, {4.0, 9.0}, {6.0, 5.0},
        {missing, 8.0}, {3.0, 4.0}, {21.0, missing}, {5.0, 6.0},
        {9.0, 0.0}, {6.0, 7.0}, {6.0, 0.0}, {12.0, 6.0},
    });

    check_recompute_replacement("test_obv_recompute_changed_inputs",
        {{18.0, 4.0}, {24.0, 9.0}, {21.0, 5.0}},
        {33.0, 13.0}, {23.0, 7.0},
        {{22.0, 11.0}, {22.0, 3.0}, {26.0, 5.0}});
    check_recompute_replacement("test_obv_recompute_replaces_valid_with_missing",
        {{28.0, 4.0}, {35.0, 9.0}, {30.0, 6.0}},
        {42.0, 17.0}, {9.0, missing},
        {{20.0, 8.0}, {26.0, 5.0}});
    check_recompute_replacement("test_obv_recompute_replaces_missing_with_valid",
        {{14.0, 5.0}, {22.0, 6.0}, {18.0, 4.0}},
        {27.0, missing}, {16.0, 11.0},
        {{17.0, 3.0}, {17.0, 5.0}, {12.0, 7.0}});

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
