// pineforge::source::PineVwma of <pineforge/source/pine_ta_length.hpp>:
// ta.vwma(source, length) as ta.sma(source * volume, length) /
// ta.sma(volume, length), two windows that skip an na input separately.
// The kernel's ta::VWMA keeps complete pairs and is unchanged; its answers
// on the same bars are asserted beside the adapter's. All inputs are invented.
#include <pineforge/source/pine_ta_length.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

using pineforge::na;
namespace ta = pineforge::ta;
using pineforge::source::PineVwma;

int failures = 0;

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// Bit identity; any two NaNs count as the same na.
void same(const char* row, int bar, double actual, double expected) {
    const bool equal = std::isnan(expected) ? std::isnan(actual)
                                            : (!std::isnan(actual) && bits(actual) == bits(expected));
    std::printf("%s %s bar %d: got %.17g, want %.17g\n",
                equal ? "PASS" : "FAIL", row, bar, actual, expected);
    if (!equal) ++failures;
}

struct Bar {
    double source;
    double volume;
};

// The same expression written with the two calls it is defined by.
struct TwoSmas {
    ta::SMA product;
    ta::SMA volume;
    explicit TwoSmas(int length) : product(length), volume(length) {}
    double compute(double source, double vol) {
        const double product_mean = product.compute(source * vol);
        const double volume_mean = volume.compute(vol);
        return product_mean / volume_mean;
    }
};

// A source that is na on one bar while the volume is not. Length 2.
//   products  2  6  na  10  12  14      volumes  1  1  3  1  1  1
// Bar 2: the product window still holds {2, 6}, mean 4; the volume window
// is {1, 3}, mean 2. Bar 3: products {6, 10}, mean 8; volumes {3, 1}, mean 2.
void missing_source() {
    const double missing = na<double>();
    const Bar bars[] = {{2, 1}, {6, 1}, {missing, 3}, {10, 1}, {12, 1}, {14, 1}};
    const double expected[] = {missing, 4, 2, 4, 11, 13};
    // Complete pairs only: bar 2 enters neither window, bar 3 is {6, 10} over {1, 1}.
    const double pairs[] = {missing, 4, missing, 8, 11, 13};
    PineVwma vwma(2);
    TwoSmas spelled(2);
    ta::VWMA kernel(2);
    for (int bar = 0; bar < 6; ++bar) {
        const double value = vwma.compute(bars[bar].source, bars[bar].volume);
        same("missing-source", bar, value, expected[bar]);
        same("missing-source two-sma", bar, value,
             spelled.compute(bars[bar].source, bars[bar].volume));
        same("missing-source kernel-pairs", bar,
             kernel.compute(bars[bar].source, bars[bar].volume), pairs[bar]);
    }
}

// An na volume is missing in both windows: both hold, and stay aligned.
//   products  2  6  na  10      volumes  1  1  na  1
void missing_volume() {
    const double missing = na<double>();
    const Bar bars[] = {{2, 1}, {6, 1}, {8, missing}, {10, 1}};
    const double expected[] = {missing, 4, 4, 8};
    PineVwma vwma(2);
    for (int bar = 0; bar < 4; ++bar)
        same("missing-volume", bar, vwma.compute(bars[bar].source, bars[bar].volume),
             expected[bar]);
}

// No na anywhere: the value of the two ta.sma calls on every bar.
void all_valid() {
    for (const int length : {1, 3, 14}) {
        PineVwma vwma(length);
        TwoSmas spelled(length);
        for (int bar = 0; bar < 120; ++bar) {
            const double source = 48.25 + ((13 * bar) % 19) * 0.37 - bar * 0.021;
            const double volume = 310.5 + ((7 * bar) % 11) * 42.75;
            same("all-valid", bar, vwma.compute(source, volume),
                 spelled.compute(source, volume));
        }
    }
}

// A zero volume mean answers na: with products of zero (0 / 0), and with a
// held product mean over a volume window that has become all zero.
void zero_volume_mean() {
    const double missing = na<double>();
    PineVwma flat(2);
    same("zero-volume", 0, flat.compute(5, 0), missing);
    same("zero-volume", 1, flat.compute(7, 0), missing);

    PineVwma held(2);
    held.compute(2, 1);
    same("zero-volume held", 1, held.compute(6, 1), 4);
    same("zero-volume held", 2, held.compute(missing, 0), 8);   // 4 / ((1 + 0) / 2)
    same("zero-volume held", 3, held.compute(missing, 0), missing);
}

// Replacing the current bar replays each window from its own previous bar.
void replacement() {
    const double missing = na<double>();
    PineVwma vwma(2);
    vwma.compute(2, 1);
    vwma.compute(6, 1);
    same("replacement", 2, vwma.compute(missing, 3), 2);     // 4 / 2
    same("replacement", 2, vwma.recompute(10, 3), 9);        // (6 + 30) / 2 over (1 + 3) / 2
    same("replacement", 2, vwma.recompute(missing, 3), 2);   // back to the held mean
    same("replacement", 3, vwma.compute(10, 1), 4);          // (6 + 10) / 2 over (3 + 1) / 2
}

}  // namespace

int main() {
    missing_source();
    missing_volume();
    all_valid();
    zero_volume_mean();
    replacement();
    std::printf("test_pine_vwma: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
