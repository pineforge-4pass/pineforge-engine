// Pine's ta.mfi first value (source::PineMfi). TradingView's built-in equals
// its published definition
//   upper = math.sum(volume * (ta.change(src) <= 0 ? 0 : src), length)
//   lower = math.sum(volume * (ta.change(src) >= 0 ? 0 : src), length)
//   mfi   = 100 - 100 / (1 + upper / lower)
// on every bar from its first value (observed on a synthetic script). On the
// first bar the change is na, so both sums take that bar's source * volume,
// and the first value is on bar length - 1. ta::MFI starts one bar later and
// never counts the first bar. All bars below are invented. Every expected
// value is the definition's last line applied to two sums that are written
// out beside it, never a second money-flow loop.

#include <pineforge/na.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using pineforge::is_na;
using pineforge::na;
using pineforge::OwnedStorage;
namespace ta = pineforge::ta;
using pineforge::source::PineMfi;

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const char* label, int bar) {
    if (condition) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL %s (bar %d)\n", label, bar);
    }
}

void check_value(double actual, double expected, const char* label, int bar) {
    if (std::isfinite(actual) && std::fabs(actual - expected) <= 1e-11) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL %s (bar %d): actual=%.17g expected=%.17g\n",
                    label, bar, actual, expected);
    }
}

void check_na(double actual, const char* label, int bar) {
    if (is_na(actual)) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL %s (bar %d): actual=%.17g expected=na\n", label, bar, actual);
    }
}

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// Bit identity; any two NaNs count as the same na.
void check_same(double actual, double expected, const char* label, int bar) {
    const bool same = is_na(expected) ? is_na(actual)
                                      : (!is_na(actual) && bits(actual) == bits(expected));
    if (same) {
        ++passed;
    } else {
        ++failed;
        std::printf("FAIL %s (bar %d): actual=%.17g expected=%.17g\n",
                    label, bar, actual, expected);
    }
}

// The last line of the published definition.
double definition(double upper, double lower) {
    return 100.0 - 100.0 / (1.0 + upper / lower);
}

struct Sample {
    double src;
    double vol;
};

// Length 2, closes 2 4 2 4 2 4. With every volume 1:
//   bar 1: upper = 2 + 4 (the first bar and the rise), lower = 2 (the first
//          bar): 100 - 100 / (1 + 6 / 2) = 75.
//   bars 2..5: one rise of 4 and one fall of 2 in the window: 200 / 3.
// With the first bar's volume 2 the first bar brings 4 to both sums:
//   bar 1: upper = 4 + 4, lower = 4: 200 / 3.
// A value of 100 on bar 1 would be a first bar that counts on neither side;
// na on bar 1 is ta::MFI.
void test_first_value_counts_the_first_bar_on_both_sides() {
    for (double first_volume : {1.0, 2.0}) {
        PineMfi mfi(2);
        const double first_flow = 2.0 * first_volume;
        check_na(mfi.compute(2.0, first_volume), "one bar is not a window of two", 0);
        check_value(mfi.compute(4.0, 1.0), definition(first_flow + 4.0, first_flow),
                    "first value is on bar length - 1 with the first bar on both sides", 1);
        for (int bar = 2; bar < 6; ++bar) {
            check_value(mfi.compute(bar % 2 == 0 ? 2.0 : 4.0, 1.0), definition(4.0, 2.0),
                        "later windows hold one rise and one fall", bar);
        }
    }
    PineMfi plain(2);
    plain.compute(2.0, 1.0);
    check_value(plain.compute(4.0, 1.0), 75.0, "the worked first value", 1);
}

// Length 5. source * volume per bar: 30, 24, 44, 55, 13, 18, 42, 84, 8.
// Direction against the bar before: -, up, down, flat, up, down, up, flat, down.
constexpr Sample kBars[] = {
    {10.0, 3.0}, {12.0, 2.0}, {11.0, 4.0}, {11.0, 5.0}, {13.0, 1.0},
    {9.0, 2.0},  {14.0, 3.0}, {14.0, 6.0}, {8.0, 1.0},
};
constexpr int kBarCount = static_cast<int>(sizeof kBars / sizeof kBars[0]);
constexpr int kLength = 5;

struct Sums {
    double upper;
    double lower;
};

// The two sums of the definition on bars 4..8.
constexpr Sums kSums[] = {
    {30.0 + 24.0 + 13.0, 30.0 + 44.0},  // bar 4: bars 0..4, bar 0 on both sides
    {24.0 + 13.0, 44.0 + 18.0},         // bar 5: bars 1..5
    {13.0 + 42.0, 44.0 + 18.0},         // bar 6: bars 2..6
    {13.0 + 42.0, 18.0},                // bar 7: bars 3..7
    {13.0 + 42.0, 18.0 + 8.0},          // bar 8: bars 4..8
};

void test_first_value_and_the_bars_after_it() {
    PineMfi mfi(kLength);
    for (int bar = 0; bar < kBarCount; ++bar) {
        const double value = mfi.compute(kBars[bar].src, kBars[bar].vol);
        if (bar < kLength - 1) {
            check_na(value, "na before bar length - 1", bar);
        } else {
            const Sums& sums = kSums[bar - (kLength - 1)];
            check_value(value, definition(sums.upper, sums.lower),
                        "the definition from bar length - 1 on", bar);
        }
    }
}

// recompute() replaces the last call, on the adapter's own bar as on the
// others. On bar 4 the bar before closed at 11:
//   (13, 7): a rise of 91: upper = 30 + 24 + 91, lower = 30 + 44.
//   (9, 2):  a fall of 18: upper = 30 + 24,      lower = 30 + 44 + 18.
void test_recompute_replaces_the_last_call() {
    PineMfi mfi(kLength);
    for (int bar = 0; bar < kLength - 1; ++bar) mfi.compute(kBars[bar].src, kBars[bar].vol);
    check_value(mfi.compute(13.0, 1.0), definition(67.0, 74.0), "first value", 4);
    check_value(mfi.recompute(13.0, 7.0), definition(30.0 + 24.0 + 91.0, 30.0 + 44.0),
                "recompute replaces the volume", 4);
    check_value(mfi.recompute(9.0, 2.0), definition(30.0 + 24.0, 30.0 + 44.0 + 18.0),
                "recompute replaces the direction", 4);
    check_value(mfi.recompute(13.0, 1.0), definition(67.0, 74.0),
                "recompute with the bar's own values", 4);
    check_value(mfi.compute(9.0, 2.0), definition(37.0, 62.0),
                "the next bar is not moved by the recomputes", 5);

    // Every bar: a call, a moved recompute, then the bar's own values again.
    // The first bar's recompute replaces the flow both sums start from.
    PineMfi replayed(kLength);
    for (int bar = 0; bar < kBarCount; ++bar) {
        const double first = replayed.compute(kBars[bar].src, kBars[bar].vol);
        replayed.recompute(kBars[bar].src + (bar % 2 ? 3.0 : -3.0), kBars[bar].vol + 2.0);
        const double again = replayed.recompute(kBars[bar].src, kBars[bar].vol);
        check_same(again, first, "a recompute with the bar's own values repeats the call", bar);
        if (bar >= kLength - 1) {
            const Sums& sums = kSums[bar - (kLength - 1)];
            check_value(again, definition(sums.upper, sums.lower),
                        "recomputed bars keep the definition", bar);
        }
    }
}

// Only the length-th call is the adapter's: every other call is ta::MFI's
// answer bit for bit, missing sources and repeated closes included. On the
// length-th call ta::MFI is still warming up.
void test_other_calls_are_the_fixed_length_class() {
    const double missing = na<double>();
    const Sample sequence[] = {
        {10.0, 3.0}, {12.0, 2.0}, {11.0, 4.0}, {missing, 5.0}, {13.0, 1.0},
        {9.0, 2.0},  {9.0, 2.0},  {missing, 1.0}, {14.0, 3.0}, {15.0, 6.0},
        {8.0, 1.0},  {8.0, 0.0},  {7.0, 2.0},
    };
    const int count = static_cast<int>(sizeof sequence / sizeof sequence[0]);
    for (int length : {1, 3, 5}) {
        PineMfi adapter(length);
        ta::MFI fixed(length);
        for (int bar = 0; bar < count; ++bar) {
            const double value = adapter.compute(sequence[bar].src, sequence[bar].vol);
            const double fixed_value = fixed.compute(sequence[bar].src, sequence[bar].vol);
            if (bar != length - 1) {
                check_same(value, fixed_value, "every other call is the fixed-length class", bar);
                continue;
            }
            check_na(fixed_value, "the fixed-length class has no value on bar length - 1", bar);
            if (length == 1) {
                // The window is the first bar alone: 30 on both sides.
                check_value(value, definition(30.0, 30.0), "length 1 starts on the first bar", bar);
            } else if (length == 3) {
                check_value(value, definition(30.0 + 24.0, 30.0 + 44.0),
                            "length 3 starts on bar 2", bar);
            } else {
                // Bar 3 has no source. The adapter keeps ta::MFI's treatment
                // of it: no flow on that bar or on the bar after it, and only
                // the first call counts on both sides. This row is the
                // adapter's rule, not a TradingView observation.
                check_value(value, definition(30.0 + 24.0, 30.0 + 44.0),
                            "a missing source later is not a second first bar", bar);
            }
        }
    }
}

// The adapter's own bar when a sum is na or the lower sum is zero. These
// rows carry ta::MFI's conventions over to that bar; they are not TradingView
// observations.
void test_first_value_edges() {
    const double missing = na<double>();
    PineMfi no_source(2);
    ta::MFI fixed(2);
    no_source.compute(missing, 1.0);
    fixed.compute(missing, 1.0);
    check_na(no_source.compute(4.0, 1.0), "an na first source leaves the first value na", 1);
    fixed.compute(4.0, 1.0);
    check_same(no_source.compute(2.0, 1.0), fixed.compute(2.0, 1.0),
               "the fixed-length class answers from the next bar", 2);

    PineMfi no_volume(2);
    no_volume.compute(2.0, missing);
    check_na(no_volume.compute(4.0, 1.0), "an na first volume leaves the first value na", 1);

    PineMfi zero_then_rise(2);
    zero_then_rise.compute(2.0, 0.0);
    check_value(zero_then_rise.compute(4.0, 1.0), 100.0, "a zero lower sum answers 100", 1);

    PineMfi zero_then_flat(2);
    zero_then_flat.compute(2.0, 0.0);
    check_value(zero_then_flat.compute(2.0, 1.0), 100.0, "two zero sums answer 100", 1);
}

// The shapes of generated code: a reset by assignment, a first-call-bound
// member, a recompute as the first call and the owned-storage observation.
void test_generated_code_shapes() {
    PineMfi member(3);
    member.compute(1.0, 1.0);
    member = PineMfi(2);
    pineforge::source::FirstCallBound<PineMfi> bound;
    PineMfi recomputed_first(2);
    PineMfi fresh(2);
    for (int bar = 0; bar < 6; ++bar) {
        const double close = bar % 2 == 0 ? 2.0 : 4.0;
        const double expected = fresh.compute(close, 1.0);
        check_same(member.compute(close, 1.0), expected, "no state survives a reset", bar);
        check_same(bound.compute([] { return PineMfi(2); }, close, 1.0), expected,
                   "a first-call-bound member", bar);
        check_same(bar == 0 ? recomputed_first.recompute(close, 1.0)
                            : recomputed_first.compute(close, 1.0),
                   expected, "a recompute as the first call", bar);
    }
    check(bound.bound(), "the first call builds the member", 0);
    OwnedStorage observed;
    member.observe_owned_storage(observed);
    bound.observe_owned_storage(observed);
    check(observed.usage().complete(), "owned storage is observed", 0);
}

}  // namespace

int main() {
    test_first_value_counts_the_first_bar_on_both_sides();
    test_first_value_and_the_bars_after_it();
    test_recompute_replaces_the_last_call();
    test_other_calls_are_the_fixed_length_class();
    test_first_value_edges();
    test_generated_code_shapes();
    std::printf("test_pine_mfi_first_flow: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
