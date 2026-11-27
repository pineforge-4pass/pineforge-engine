// Pine ta.cci on a window whose mean deviation is zero up to rounding:
// pineforge::source::PineCci (<pineforge/source/pine_ta_cci.hpp>).
//
// Target: three identical values of 0.1 have a mean deviation of 2^-56, not
// 0, in double arithmetic. The Pine adapter answers 0 there, as TradingView's
// ta.cci does, and outside the comparison band it answers ta::CCI's own value
// bit for bit. Every input is invented.
#include <pineforge/source/pine_ta_cci.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/ta_compare_band.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using pineforge::kFloatCompareBand;
using pineforge::na;
using pineforge::source::PineCci;
namespace ta = pineforge::ta;

namespace {

int failures = 0;

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

void expect_zero(const char* row, double got) {
    if (!std::isnan(got) && got == 0.0) {
        std::printf("PASS %s: %.17g\n", row, got);
    } else {
        std::printf("FAIL %s: got %.17g, want 0\n", row, got);
        ++failures;
    }
}

void expect_na(const char* row, double got) {
    if (std::isnan(got)) {
        std::printf("PASS %s: na\n", row);
    } else {
        std::printf("FAIL %s: got %.17g, want na\n", row, got);
        ++failures;
    }
}

// Bit identity with the kernel class; any two na values count as the same.
void expect_same(const char* row, double got, double want) {
    const bool same = std::isnan(want) ? std::isnan(got)
                                       : (!std::isnan(got) && bits(got) == bits(want));
    if (same) {
        std::printf("PASS %s: %.17g\n", row, got);
    } else {
        std::printf("FAIL %s: got %.17g, want %.17g\n", row, got, want);
        ++failures;
    }
}

void expect_near(const char* row, double got, double want, double tolerance) {
    if (!std::isnan(got) && std::fabs(got - want) <= tolerance) {
        std::printf("PASS %s: %.17g\n", row, got);
    } else {
        std::printf("FAIL %s: got %.17g, want %.17g within %.3g\n", row, got, want, tolerance);
        ++failures;
    }
}

// The reported window: 0.1 on every bar, length 3.
void flat_tenth() {
    const double tenth = 1.0 / 10.0;

    // Precondition: the window's mean deviation is not exactly zero, so the 0
    // below comes from the band and not from ta::CCI's exact-zero branch.
    ta::Dev dev(3);
    dev.compute(tenth);
    dev.compute(tenth);
    const double deviation = dev.compute(tenth);
    if (!(deviation > 0.0 && deviation <= kFloatCompareBand)) {
        std::printf("NOT COMPARED flat_tenth: the mean deviation of three values of 0.1 is "
                    "%.17g, expected above 0 and at most %.17g\n",
                    deviation, kFloatCompareBand);
        ++failures;
        return;
    }
    std::printf("PASS flat_tenth precondition: mean deviation %.17g\n", deviation);

    PineCci cci(3);
    expect_na("flat_tenth bar 0", cci.compute(tenth));
    expect_na("flat_tenth bar 1", cci.compute(tenth));
    expect_zero("flat_tenth bar 2", cci.compute(tenth));
    expect_zero("flat_tenth bar 2 recomputed", cci.recompute(tenth));
    expect_zero("flat_tenth bar 3", cci.compute(tenth));
    expect_zero("flat_tenth bar 4", cci.compute(tenth));
}

// A window that is flat in double arithmetic too: 0 with or without the band.
void flat_one() {
    PineCci cci(3);
    expect_na("flat_one bar 0", cci.compute(1.0));
    expect_na("flat_one bar 1", cci.compute(1.0));
    expect_zero("flat_one bar 2", cci.compute(1.0));
    expect_zero("flat_one bar 3", cci.compute(1.0));
}

// Outside the band the adapter is ta::CCI: the same calls give the same bits.
// No three consecutive values below are equal.
void ordinary_windows() {
    PineCci adapter(3);
    ta::CCI kernel(3);
    const double missing = na<double>();
    const double sources[] = {1.0, 2.0, 3.0, 3.0, 7.5, missing,
                              4.25, 4.25, 9.0, 0.5, 0.5, 6.0};
    char row[64];
    int bar = 0;
    for (const double src : sources) {
        const double got = adapter.compute(src);
        std::snprintf(row, sizeof row, "ordinary bar %d", bar);
        expect_same(row, got, kernel.compute(src));
        // {1, 2, 3}: mean 2, mean deviation 2/3, (3 - 2) / (0.015 * 2/3) = 100.
        if (bar == 2) expect_near("ordinary bar 2 value", got, 100.0, 1e-9);
        ++bar;
    }
    expect_same("ordinary recomputed", adapter.recompute(8.0), kernel.recompute(8.0));
    expect_same("ordinary recomputed na", adapter.recompute(missing), kernel.recompute(missing));
    expect_same("ordinary after na", adapter.compute(2.5), kernel.compute(2.5));
}

// One member a ten-millionth above the other two. The mean deviation, about
// 4.4e-8, is far outside the band, so the quotient stays: the higher member
// reads about 100 and each of the other two about -50.
void near_flat_outside_band() {
    PineCci adapter(3);
    ta::CCI kernel(3);
    const double low = 0.1;
    const double high = 0.1000001;
    adapter.compute(low);
    kernel.compute(low);
    adapter.compute(low);
    kernel.compute(low);
    const double higher = adapter.compute(high);
    expect_same("near_flat higher member", higher, kernel.compute(high));
    expect_near("near_flat higher member value", higher, 100.0, 1e-3);
    const double lower = adapter.compute(low);
    expect_same("near_flat lower member", lower, kernel.compute(low));
    expect_near("near_flat lower member value", lower, -50.0, 1e-3);
}

// na never enters the window: the third value that is not na completes it.
void na_source() {
    const double tenth = 1.0 / 10.0;
    PineCci cci(3);
    expect_na("na_source bar 0", cci.compute(tenth));
    expect_na("na_source bar 1", cci.compute(tenth));
    expect_na("na_source bar 2", cci.compute(na<double>()));
    expect_zero("na_source bar 3", cci.compute(tenth));
}

// A recalculation replaces the bar's value in both inner windows.
void recompute_replaces() {
    const double tenth = 1.0 / 10.0;
    PineCci adapter(3);
    ta::CCI kernel(3);
    adapter.compute(tenth);
    kernel.compute(tenth);
    adapter.compute(tenth);
    kernel.compute(tenth);
    expect_zero("recompute flat", adapter.compute(tenth));
    kernel.compute(tenth);
    // {0.1, 0.1, 0.4}: mean 0.2, mean deviation 0.4/3,
    // (0.4 - 0.2) / (0.015 * 0.4/3) = 100.
    const double moved = adapter.recompute(0.4);
    expect_same("recompute to 0.4", moved, kernel.recompute(0.4));
    expect_near("recompute to 0.4 value", moved, 100.0, 1e-9);
    expect_zero("recompute back to flat", adapter.recompute(tenth));
}

}  // namespace

int main() {
    flat_tenth();
    flat_one();
    ordinary_windows();
    near_flat_outside_band();
    na_source();
    recompute_replaces();
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures != 0;
}
