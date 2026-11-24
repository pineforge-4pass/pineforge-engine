#include <pineforge/source/pine_ta_length.hpp>

#include <cstdio>
#include <limits>

int main() {
    constexpr double m = std::numeric_limits<double>::max();
    // The spacing below M is U = 2^971; t = 3U/8.
    constexpr double t = 0x1.8p969;
    struct Row {
        const char* name;
        double source;
        int length;
        double expected;
    };
    // Starting with M, each call selects the indicated suffix. Reduce its
    // sum in real arithmetic, round to binary64, then divide by its length.
    constexpr Row rows[] = {
        {"opposite_pair", -m, 2, 0.0},             // (M - M) / 2
        {"negative_middle", m, 3, m / 3.0},        // (M - M + M) / 3
        {"negative_first", m, 3, m / 3.0},         // (-M + M + M) / 3
        {"negative_last", -m, 3, m / 3.0},         // (M + M - M) / 3
        {"two_pairs", -m, 4, 0.0},                 // (M + M - M - M) / 4
        {"negative_prefix", m, 3, -m / 3.0},       // (-M - M + M) / 3
        {"rolled_out", 6, 3, 6.0 / 3.0},           // (-M + M + 6) / 3
        {"small_remainder", m, 5, 6.0 / 5.0},      // (-M - M + M + 6 + M) / 5
        {"below_midpoint", t, 2, m / 2.0},         // M + 3U/8 rounds to M
        // M + 2t = M + 3U/4 rounds to +infinity before division by 3.
        {"true_overflow", t, 3, std::numeric_limits<double>::infinity()},
    };
    static_assert(sizeof rows / sizeof rows[0] == 10);
    pineforge::source::SeriesSma sma;
    sma.compute(m, 1);
    int failed = 0;
    for (const Row& row : rows) {
        const double got = sma.compute(row.source, row.length);
        if (got == row.expected) {
            std::printf("PASS %s\n", row.name);
        } else {
            std::printf("FAIL %s: got %a, expected %a\n", row.name, got, row.expected);
            ++failed;
        }
    }
    return failed != 0;
}
