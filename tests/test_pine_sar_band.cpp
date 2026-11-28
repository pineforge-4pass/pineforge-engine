// Pine ta.sar beside a touch: source::PineSar is the parabolic SAR recipe of
// ta::SAR with every relational comparison made the way a Pine script's
// relational operators are. Two finite values at most 1e-10 apart are equal,
// so a projected SAR 6e-11 beyond the bar's low has not passed it. The
// expected rows apply that operator rule to the recipe by hand; they are not
// a recorded tape. ta::SAR, the same recipe with exact C++ comparisons, runs
// beside it on every tape as the witness that the tape sits inside the band
// (and that the kernel class is not changed here), and equals PineSar bit for
// bit on a tape with no pair inside the band.
//
// Only invented bars.

#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pineforge;
using pineforge::source::PineSar;

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const char* label) {
    if (condition) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL %s\n", label);
    }
}

struct Bar {
    double high;
    double low;
    double close;
};

template <class Sar>
std::vector<double> rows(Sar sar, const std::vector<Bar>& bars) {
    std::vector<double> out;
    for (const Bar& bar : bars) out.push_back(sar.compute(bar.high, bar.low, bar.close));
    return out;
}

// na where expected, else the value to 1e-9 (the cases differ by 0.08 or more).
void expect_rows(const std::vector<double>& actual, const std::vector<double>& expected,
                 const char* label) {
    bool ok = actual.size() == expected.size();
    for (std::size_t i = 0; ok && i < expected.size(); ++i) {
        ok = is_na(expected[i]) ? is_na(actual[i])
                                : std::isfinite(actual[i]) &&
                                      std::fabs(actual[i] - expected[i]) <= 1e-9;
    }
    if (ok) {
        ++passed;
        return;
    }
    ++failed;
    std::printf("FAIL %s:", label);
    for (double value : actual) std::printf(" %.17g", value);
    std::printf("\n");
}

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

bool same_bits(double a, double b) {
    return (is_na(a) && is_na(b)) || bits(a) == bits(b);
}

const double kNa = na<double>();

// The long reversal. Bar 1 starts long from the previous low 8 toward its
// high 14: 8 + start * 6. With start 0.50000000001 that is 11.00000000006,
// 6e-11 above the bar's low 11.
//   touch:    no reversal, the line is held at the previous low 8; bar 2 the
//             same; bar 3 holds at the lows 11; bar 4 projects 11 + start * 3
//             = 12.5, clearly above the low, and reverses to 14.
//   passed:   bar 1 reverses to 14; bar 2 projects 12.5 below its high and
//             reverses back, held at 8; bar 3 reverses to 14; bar 4 back to 11.
void test_projection_touching_low_keeps_long() {
    const std::vector<Bar> bars = {
        {12, 8, 10}, {14, 11, 12}, {14, 11, 12}, {14, 11, 12}, {14, 11, 12}};
    const double start = 0.50000000001;
    const double projected = 8.0 + start * (14.0 - 8.0);
    check(projected > 11.0 && projected - 11.0 < 1e-10,
          "long: the projection passes the low by less than 1e-10");

    expect_rows(rows(PineSar(start, 0.01, 0.9), bars), {kNa, 8, 8, 11, 14},
                "long: a touch keeps the long side");
    expect_rows(rows(ta::SAR(start, 0.01, 0.9), bars), {kNa, 14, 8, 14, 11},
                "long: exact class reverses on bar 1");

    // start 0.5: the projection is exactly the low. No rule reverses.
    expect_rows(rows(PineSar(0.5, 0.01, 0.9), bars), {kNa, 8, 8, 11, 14},
                "long control 0.5: an equal projection does not reverse");
    // start 0.500000001: 6e-9 past the low, outside the band of equality.
    // Every rule reverses on bar 1.
    expect_rows(rows(PineSar(0.500000001, 0.01, 0.9), bars), {kNa, 14, 8, 14, 11},
                "long control 0.500000001: a clear pass reverses");
}

// The short reversal: the same tape mirrored about 11. Bar 1 starts short
// from the previous high 14 toward its low 8: 14 - start * 6 =
// 10.99999999994, 6e-11 below the bar's high 11.
void test_projection_touching_high_keeps_short() {
    const std::vector<Bar> bars = {
        {14, 10, 12}, {11, 8, 10}, {11, 8, 10}, {11, 8, 10}, {11, 8, 10}};
    const double start = 0.50000000001;
    const double projected = 14.0 + start * (8.0 - 14.0);
    check(projected < 11.0 && 11.0 - projected < 1e-10,
          "short: the projection passes the high by less than 1e-10");

    expect_rows(rows(PineSar(start, 0.01, 0.9), bars), {kNa, 14, 14, 11, 8},
                "short: a touch keeps the short side");
    expect_rows(rows(ta::SAR(start, 0.01, 0.9), bars), {kNa, 8, 14, 8, 11},
                "short: exact class reverses on bar 1");
}

// The first trend bar reads close > close[1]. Bar 1 closes 4e-11 above bar 0,
// which is not greater: the trend starts short, from the previous high 12
// toward the low 9 (12 - 0.02 * 3 = 11.94, above the bar's high, held at 12;
// bar 2 the same). An exact comparison starts long from the previous low 8.
void test_first_trend_bar_reads_close_against_previous_close() {
    const double close1 = 10.00000000004;
    check(close1 > 10.0 && close1 - 10.0 < 1e-10,
          "start: bar 1 closes above bar 0 by less than 1e-10");
    const std::vector<Bar> bars = {{12, 8, 10}, {11, 9, close1}, {10.5, 8.5, 9}};

    expect_rows(rows(PineSar(0.02, 0.02, 0.2), bars), {kNa, 12, 12},
                "start: an equal close starts short");
    expect_rows(rows(ta::SAR(0.02, 0.02, 0.2), bars), {kNa, 8, 8},
                "start: exact class starts long");
}

// The acceleration steps when the bar sets a new extreme: high > extreme
// (long), low < extreme (short). Bar 2 passes the extreme by 4e-11, which is
// not a new extreme: the acceleration stays 0.02 and bar 3 projects with it.
//   long:  9 + 0.02 * (13 - 9) = 9.08; a step to 0.04 gives 9.16.
//   short: 20 + 0.02 * (16 - 20) = 19.92; a step to 0.04 gives 19.84.
void test_extreme_inside_the_band_does_not_step_acceleration() {
    const double high2 = 13.00000000004;
    check(high2 > 13.0 && high2 - 13.0 < 1e-10,
          "extreme: bar 2's high passes the extreme by less than 1e-10");
    const std::vector<Bar> rising = {
        {11, 9, 10}, {13, 10, 12}, {high2, 12, 12.5}, {13, 12, 12.5}};
    expect_rows(rows(PineSar(0.02, 0.02, 0.2), rising), {kNa, 9, 9, 9.08},
                "extreme long: no step");
    expect_rows(rows(ta::SAR(0.02, 0.02, 0.2), rising), {kNa, 9, 9, 9.16},
                "extreme long: exact class steps");

    const double low2 = 15.99999999996;
    check(low2 < 16.0 && 16.0 - low2 < 1e-10,
          "extreme: bar 2's low passes the extreme by less than 1e-10");
    const std::vector<Bar> falling = {
        {20, 18, 19}, {19, 16, 17}, {17, low2, 16.5}, {17, 16, 16.5}};
    expect_rows(rows(PineSar(0.02, 0.02, 0.2), falling), {kNa, 20, 20, 19.92},
                "extreme short: no step");
    expect_rows(rows(ta::SAR(0.02, 0.02, 0.2), falling), {kNa, 20, 20, 19.84},
                "extreme short: exact class steps");
}

// Away from the band PineSar is ta::SAR, bit for bit: a wandering tape on a
// quarter-point grid (compared prices are equal or a quarter apart) with an
// na bar at the start and one in the middle, reversals both ways and the
// acceleration cap. A bar recalculated after a first tick answers what one
// computation of it answers.
void test_same_as_exact_class_away_from_the_band() {
    std::vector<Bar> bars;
    bars.push_back({kNa, kNa, kNa});
    double mid = 100.0;
    for (int i = 0; i < 64; ++i) {
        mid += (i % 16 < 8 ? 0.75 : -1.0) + (i % 5 == 0 ? 0.5 : 0.0);
        if (i == 30) {
            bars.push_back({kNa, kNa, kNa});
            continue;
        }
        bars.push_back({mid + 0.5 + 0.25 * (i % 3), mid - 0.5 - 0.25 * (i % 4),
                        mid + 0.25 * ((i % 3) - 1)});
    }

    PineSar pine(0.03, 0.017, 0.19);
    PineSar ticked(0.03, 0.017, 0.19);
    ta::SAR exact(0.03, 0.017, 0.19);
    int different = 0;
    int recomputed_different = 0;
    int values = 0;
    double lowest = 0.0;
    double highest = 0.0;
    for (const Bar& bar : bars) {
        const double want = exact.compute(bar.high, bar.low, bar.close);
        const double got = pine.compute(bar.high, bar.low, bar.close);
        ticked.compute(bar.high + 0.25, bar.low - 0.25, bar.close);
        const double again = ticked.recompute(bar.high, bar.low, bar.close);
        if (!same_bits(got, want)) ++different;
        if (!same_bits(again, want)) ++recomputed_different;
        if (!is_na(want)) {
            if (values == 0 || want < lowest) lowest = want;
            if (values == 0 || want > highest) highest = want;
            ++values;
        }
    }
    check(different == 0, "away from the band: every bar equals the exact class");
    check(recomputed_different == 0, "away from the band: a recalculated bar equals it too");
    // The comparison is not over an empty or flat series.
    check(values == 62 && highest - lowest > 4.0, "away from the band: the tape has values");
}

}  // namespace

int main() {
    test_projection_touching_low_keeps_long();
    test_projection_touching_high_keeps_short();
    test_first_trend_bar_reads_close_against_previous_close();
    test_extreme_inside_the_band_does_not_step_acceleration();
    test_same_as_exact_class_away_from_the_band();

    std::printf("pine_sar_band: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
