// Pine's ta.rci first value (source::PineRci). TradingView's value is the
// rank correlation of the last `length` values, times 100 (Spearman: the bar
// order against the rank of each value in the window), which is the window
// ta::RCI uses; but TradingView is na on bar length - 1, where that window
// first holds `length` values, and starts on bar `length` (observed on a
// synthetic script). ta::RCI answers one bar earlier. All values below are
// invented. With distinct values the expected number is
//   100 * (1 - 6 * D / (n * (n * n - 1)))
// where D is the sum over the window of (bar order - value rank) squared,
// both counted from 0 at the oldest bar and at the lowest value; D is worked
// out beside each row.

#include <pineforge/na.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using pineforge::is_na;
using pineforge::na;
using pineforge::OwnedStorage;
namespace ta = pineforge::ta;
using pineforge::source::PineRci;

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

// Length 5, n * (n * n - 1) = 120. The windows and their D:
//   bar 4: 11 14 12 17 13, ranks 0 3 1 4 2, D = 0 + 4 + 1 + 1 + 4 = 10:  50
//   bar 5: 14 12 17 13 10, ranks 3 1 4 2 0, D = 9 + 0 + 4 + 1 + 16 = 30: -50
//   bar 6: 12 17 13 10 16, ranks 1 4 2 0 3, D = 1 + 9 + 0 + 9 + 1 = 20:   0
//   bar 7: 17 13 10 16 19, ranks 3 1 0 2 4, D = 9 + 0 + 4 + 1 + 0 = 14:  30
//   bar 8: 13 10 16 19 15, ranks 1 0 3 4 2, D = 1 + 1 + 1 + 1 + 4 = 8:   60
//   bar 9: 10 16 19 15 18, ranks 0 2 4 1 3, D = 0 + 1 + 4 + 4 + 1 = 10:  50
constexpr double kCloses[] = {11.0, 14.0, 12.0, 17.0, 13.0, 10.0, 16.0, 19.0, 15.0, 18.0};
constexpr int kCloseCount = static_cast<int>(sizeof kCloses / sizeof kCloses[0]);
constexpr int kLength = 5;
// Bars 5..9.
constexpr double kExpected[] = {-50.0, 0.0, 30.0, 60.0, 50.0};

void test_first_value_is_on_bar_length() {
    PineRci rci(kLength);
    ta::RCI fixed(kLength);
    for (int bar = 0; bar < kCloseCount; ++bar) {
        const double value = rci.compute(kCloses[bar]);
        const double fixed_value = fixed.compute(kCloses[bar]);
        if (bar < kLength - 1) {
            check_na(value, "na while the window is short", bar);
            check_na(fixed_value, "the fixed-length class is na while the window is short", bar);
        } else if (bar == kLength - 1) {
            check_na(value, "na on bar length - 1", bar);
            check_value(fixed_value, 50.0,
                        "the fixed-length class answers on bar length - 1", bar);
        } else {
            check_value(value, kExpected[bar - kLength], "the rank correlation from bar length on",
                        bar);
            check_same(value, fixed_value, "from bar length on the fixed-length class's value",
                       bar);
        }
    }
}

// The start follows the length. Length 3, n * (n * n - 1) = 24:
//   bar 2: 11 14 12, ranks 0 2 1, D = 0 + 1 + 1 = 2:  50 (withheld)
//   bar 3: 14 12 10, ranks 2 1 0, D = 4 + 0 + 4 = 8: -100
void test_start_follows_the_length() {
    PineRci rci(3);
    ta::RCI fixed(3);
    const double closes[] = {11.0, 14.0, 12.0, 10.0};
    for (int bar = 0; bar < 3; ++bar) {
        check_na(rci.compute(closes[bar]), "na through bar length - 1", bar);
        fixed.compute(closes[bar]);
    }
    const double value = rci.compute(closes[3]);
    check_value(value, -100.0, "first value on bar length", 3);
    check_same(value, fixed.compute(closes[3]), "the fixed-length class's value", 3);
}

// recompute() replaces the last call and never counts a bar twice.
//   bar 4 recomputed with 25: 11 14 12 17 25 would answer 90, still withheld.
//   bar 5 recomputed with 20: 14 12 17 13 20, ranks 2 0 3 1 4,
//                             D = 4 + 1 + 1 + 4 + 0 = 10: 50.
void test_recompute_replaces_the_last_call() {
    const double missing = na<double>();
    PineRci rci(kLength);
    for (int bar = 0; bar < kLength - 1; ++bar) rci.compute(kCloses[bar]);
    check_na(rci.compute(13.0), "na on bar length - 1", 4);
    check_na(rci.recompute(25.0), "a recompute of bar length - 1 stays na", 4);
    check_na(rci.recompute(13.0), "a second recompute of bar length - 1 stays na", 4);
    check_value(rci.compute(10.0), -50.0, "first value", 5);
    check_value(rci.recompute(20.0), 50.0, "recompute replaces the value", 5);
    check_na(rci.recompute(missing), "recompute with na", 5);
    check_value(rci.recompute(10.0), -50.0, "recompute with the bar's own value", 5);
    check_value(rci.compute(16.0), 0.0, "the next bar is not moved by the recomputes", 6);
}

// An na source adds no value to the window, in the adapter as in ta::RCI, so
// the adapter starts one value after ta::RCI on such a series too. This row is
// the adapter's rule, not a TradingView observation.
//   bar 5: 11 14 12 17 13 (bar 2 is na): ta::RCI answers 50, the adapter na
//   bar 6: 14 12 17 13 10: -50
void test_na_source_adds_no_value() {
    const double missing = na<double>();
    const double closes[] = {11.0, 14.0, missing, 12.0, 17.0, 13.0, 10.0};
    PineRci rci(kLength);
    ta::RCI fixed(kLength);
    for (int bar = 0; bar < 5; ++bar) {
        check_na(rci.compute(closes[bar]), "na while the window is short", bar);
        fixed.compute(closes[bar]);
    }
    check_na(rci.compute(closes[5]), "na on the bar that fills the window", 5);
    check_value(fixed.compute(closes[5]), 50.0, "the fixed-length class answers on that bar", 5);
    const double value = rci.compute(closes[6]);
    check_value(value, -50.0, "first value one value later", 6);
    check_same(value, fixed.compute(closes[6]), "the fixed-length class's value", 6);
}

// The shapes of generated code: a reset by assignment, a first-call-bound
// member, a recompute as the first call and the owned-storage observation.
void test_generated_code_shapes() {
    PineRci member(3);
    member.compute(1.0);
    member = PineRci(kLength);
    pineforge::source::FirstCallBound<PineRci> bound;
    PineRci recomputed_first(kLength);
    PineRci fresh(kLength);
    for (int bar = 0; bar < kCloseCount; ++bar) {
        const double expected = fresh.compute(kCloses[bar]);
        check_same(member.compute(kCloses[bar]), expected, "no state survives a reset", bar);
        check_same(bound.compute([] { return PineRci(kLength); }, kCloses[bar]), expected,
                   "a first-call-bound member", bar);
        check_same(bar == 0 ? recomputed_first.recompute(kCloses[bar])
                            : recomputed_first.compute(kCloses[bar]),
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
    test_first_value_is_on_bar_length();
    test_start_follows_the_length();
    test_recompute_replaces_the_last_call();
    test_na_source_adds_no_value();
    test_generated_code_shapes();
    std::printf("test_pine_rci_first_value: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
