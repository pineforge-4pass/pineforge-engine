#include <pineforge/source/pine_ta_length.hpp>

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

int main() {
    constexpr double m = std::numeric_limits<double>::max();
    constexpr double inf = std::numeric_limits<double>::infinity();
    // The spacing below M is U = 2^971; t = 3U/8.
    constexpr double t = 0x1.8p969;
    struct Row {
        const char* name;
        double source;
        int length;
        double expected;
    };
    // Starting with M, adjust the active suffix before inserting the source.
    // An overflowing update retries from the final window's raw sources.
    constexpr Row rows[] = {
        {"opposite_pair", -m, 2, 0.0},             // (M - M) / 2
        {"negative_middle", m, 3, m / 3.0},        // (M - M + M) / 3
        {"negative_first", m, 3, m / 3.0},         // (-M + M + M) / 3
        {"negative_last", -m, 3, m / 3.0},         // (M + M - M) / 3
        {"two_pairs", -m, 4, 0.0},                 // (M + M - M - M) / 4
        {"negative_prefix", m, 3, -m / 3.0},       // (-M - M + M) / 3
        {"rolled_out", 6, 3, 6.0 / 3.0},           // (-M + M + 6) / 3
        // Growing adds the older -M to 6 (S=-M, C=6); inserting M leaves C.
        {"small_remainder", m, 5, 6.0 / 5.0},      // (-M - M + M + 6 + M) / 5
        {"below_midpoint", t, 2, m / 2.0},         // M + 3U/8 rounds to M
        // M + 2t = M + 3U/4 rounds to +infinity before division by 3.
        {"true_overflow", t, 3, inf},
    };
    static_assert(sizeof rows / sizeof rows[0] == 10);
    pineforge::source::SeriesSma sma;
    sma.compute(m, 1);
    // Two maxima overflow before division. Adding the negative maximum
    // recovers M; retaining an infinity uses newest-first IEEE addition.
    const Row exceptional[] = {
        {"first", m, 3, pineforge::na<double>()},
        {"overflow_before_division", m, 2, inf},
        {"cancel_overflow", -m, 3, m / 3.0},
        {"positive_infinity", inf, 1, inf},
        {"opposite_infinities", -inf, 2, pineforge::na<double>()},
        {"one_infinity_left", 3, 2, -inf},
        {"infinities_evicted", 6, 2, 4.5},
    };
    int failed = 0;
    const auto check = [&](auto& site, const auto& cases) {
        for (const Row& row : cases) {
            const double got = site.compute(row.source, row.length);
            if (got == row.expected || (std::isnan(row.expected) && std::isnan(got))) {
                std::printf("PASS %s\n", row.name);
            } else {
                std::printf("FAIL %s: got %a, expected %a\n", row.name, got, row.expected);
                ++failed;
            }
        }
    };
    check(sma, rows);
    pineforge::source::SeriesSma recovered;
    check(recovered, exceptional);

    // Exact recovery rounds 1 + half + speck up. That rounded value cannot
    // be used as a new S: the next window can resume chronological two-sum.
    constexpr double half = 0x1p-53;
    constexpr double speck = std::numeric_limits<double>::denorm_min();
    pineforge::source::SeriesSma sliding;
    for (double x : {m, m, 1.0, half, speck, -m}) sliding.compute(x, 7);
    const Row slide[] = {
        {"rounded_above_tie", -m, 7, (1.0 + 0x1p-52) / 7},
        {"retry_next_window", m, 7, 1.0 / 7},
    };
    // On the next bar the chronological retry succeeds: C rounds 1+h+d
    // to 1, the two maxima cancel twice in S, and the emitted sum is 1.
    check(sliding, slide);

    pineforge::source::SeriesSma fallback;
    for (double x : {m, m, -m, -m, t, t}) fallback.compute(x, 7);
    // The exact M+2t rounds to infinity. Newest first, M absorbs both
    // individual t's, then -M,-M,M,M leave M. Oldest first overflows.
    const Row newest[] = {{"newest_first_fallback", m, 7, m / 7}};
    check(fallback, newest);
    return failed != 0;
}
