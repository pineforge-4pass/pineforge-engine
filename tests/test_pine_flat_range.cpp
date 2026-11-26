// ta.wvad and ta.iii on a bar whose high equals its low, through the Pine
// adapter classes of <pineforge/source/pine_ta_flat_range.hpp>.
//
// The Pine expressions of the two built-ins divide by high - low with no
// branch for a flat bar: 0 / 0 there, which Pine reads as na. A bar with a
// range whose numerator is zero is a genuine 0. The kernel classes ta::WVAD
// and ta::III answer 0 in both cases.
//
// Red-first control: built without the adapter header, `bound::Wvad` and
// `bound::Iii` are the kernel classes, which is what a code generator that
// does not select the adapter binds for the two built-ins. The flat-bar rows
// then fail with the kernel's 0.
//
// All inputs are invented. Every expected value is exact in binary.
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#if __has_include(<pineforge/source/pine_ta_flat_range.hpp>)
#include <pineforge/source/pine_ta_flat_range.hpp>
namespace bound {
using Wvad = pineforge::source::PineWvad;
using Iii = pineforge::source::PineIii;
}  // namespace bound
#else
namespace bound {
using Wvad = pineforge::ta::WVAD;
using Iii = pineforge::ta::III;
}  // namespace bound
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

const double kMissing = pineforge::na<double>();

struct Bar {
    double open, high, low, close, volume;
};

void expect(const char* row, std::size_t bar, double actual, double expected) {
    const bool ok = pineforge::is_na(expected)
                        ? pineforge::is_na(actual)
                        : (!pineforge::is_na(actual) && actual == expected);
    if (ok) return;
    std::printf("  FAIL  %s bar %zu: got %.17g, expected %.17g\n", row, bar, actual, expected);
    ++failures;
}

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

double wvad(bound::Wvad& object, const Bar& bar, bool again) {
    return again ? object.recompute(bar.open, bar.high, bar.low, bar.close, bar.volume)
                 : object.compute(bar.open, bar.high, bar.low, bar.close, bar.volume);
}

double iii(bound::Iii& object, const Bar& bar, bool again) {
    return again ? object.recompute(bar.high, bar.low, bar.close, bar.volume)
                 : object.compute(bar.high, bar.low, bar.close, bar.volume);
}

// Bars 1 and 4 are flat (bar 4 with a volume of zero too): na. Bar 2 has a
// range and a zero numerator: a genuine 0, between the two na readings. Every
// bar is evaluated twice (compute, then recompute).
void flat_bars() {
    std::printf("wvad_flat_bar\n");
    // (close - open) / (high - low) * volume: 2 / 4 * 8, 0 / 0, 0 / 4 * 8,
    // -4 / 4 * 8, 0 / 0.
    const std::vector<Bar> wvad_bars{
        {4, 6, 2, 6, 8}, {5, 5, 5, 5, 9}, {4, 6, 2, 4, 8}, {6, 6, 2, 2, 8}, {7, 7, 7, 7, 0}};
    const std::vector<double> wvad_expected{4.0, kMissing, 0.0, -8.0, kMissing};
    bound::Wvad wvad_object;
    for (std::size_t i = 0; i < wvad_bars.size(); ++i) {
        expect("wvad_flat_bar", i, wvad(wvad_object, wvad_bars[i], false), wvad_expected[i]);
        expect("wvad_flat_bar (again)", i, wvad(wvad_object, wvad_bars[i], true),
               wvad_expected[i]);
    }

    std::printf("iii_flat_bar\n");
    // (2 * close - high - low) / (high - low) * volume: 4 / 4 * 8, 0 / 0,
    // 0 / 4 * 8, -4 / 4 * 8, 0 / 0. The open is not an input.
    const std::vector<Bar> iii_bars{
        {6, 6, 2, 6, 8}, {5, 5, 5, 5, 9}, {4, 6, 2, 4, 8}, {2, 6, 2, 2, 8}, {7, 7, 7, 7, 0}};
    const std::vector<double> iii_expected{8.0, kMissing, 0.0, -8.0, kMissing};
    bound::Iii iii_object;
    for (std::size_t i = 0; i < iii_bars.size(); ++i) {
        expect("iii_flat_bar", i, iii(iii_object, iii_bars[i], false), iii_expected[i]);
        expect("iii_flat_bar (again)", i, iii(iii_object, iii_bars[i], true), iii_expected[i]);
    }
}

// A missing input is na, on a bar with a range and on a flat bar.
void missing_inputs() {
    std::printf("missing_inputs\n");
    bound::Wvad wvad_object;
    bound::Iii iii_object;
    const Bar no_volume{4, 6, 2, 6, kMissing};
    const Bar flat_no_volume{5, 5, 5, 5, kMissing};
    expect("wvad_missing_volume", 0, wvad(wvad_object, no_volume, false), kMissing);
    expect("wvad_flat_missing_volume", 0, wvad(wvad_object, flat_no_volume, false), kMissing);
    expect("iii_missing_volume", 0, iii(iii_object, no_volume, false), kMissing);
    expect("iii_flat_missing_volume", 0, iii(iii_object, flat_no_volume, false), kMissing);
}

// Bars with a range: the adapter's values are the kernel class's, bit for bit.
void ordinary_bars() {
    std::printf("ordinary_bars_match_kernel\n");
    bound::Wvad wvad_object;
    bound::Iii iii_object;
    pineforge::ta::WVAD wvad_kernel;
    pineforge::ta::III iii_kernel;
    for (int i = 0; i < 48; ++i) {
        const double base = 50.0 + (i % 7) * 1.25 - (i % 4) * 0.75 + 0.125 * i;
        const Bar bar{base, base + 1.5 + (i % 3) * 0.5, base - 0.75 - (i % 5) * 0.25,
                      base + ((i % 2) ? 0.5 : -0.5), 100.0 + ((i * 37) % 11) * 10.0};
        const bool again = i % 3 == 2;
        const double wvad_actual = wvad(wvad_object, bar, again);
        const double wvad_reference =
            again ? wvad_kernel.recompute(bar.open, bar.high, bar.low, bar.close, bar.volume)
                  : wvad_kernel.compute(bar.open, bar.high, bar.low, bar.close, bar.volume);
        if (bits(wvad_actual) != bits(wvad_reference)) {
            std::printf("  FAIL  wvad_ordinary bar %d: got %.17g, kernel %.17g\n", i, wvad_actual,
                        wvad_reference);
            ++failures;
        }
        const double iii_actual = iii(iii_object, bar, again);
        const double iii_reference =
            again ? iii_kernel.recompute(bar.high, bar.low, bar.close, bar.volume)
                  : iii_kernel.compute(bar.high, bar.low, bar.close, bar.volume);
        if (bits(iii_actual) != bits(iii_reference)) {
            std::printf("  FAIL  iii_ordinary bar %d: got %.17g, kernel %.17g\n", i, iii_actual,
                        iii_reference);
            ++failures;
        }
    }
}

}  // namespace

int main() {
    flat_bars();
    missing_inputs();
    ordinary_bars();
    if (failures != 0) {
        std::printf("FAILED: %d\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
