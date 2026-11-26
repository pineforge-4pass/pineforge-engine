// Pine `ta.mom(source, length)` around a missing (na) source bar.
//
// pineforge::source::PineMom reads `source[length]` by position, so an na bar
// keeps its place in the history. Invented source, length 2:
//
//   bar      0    1    2    3    4    5    6
//   source   2    4    na   8    16   10   22
//   ta.mom   na   na   na   4    na   2    6
//
// Bar 3 reads bar 1 (8 - 4). Bar 4 reads bar 2, which is missing. From bar 5
// on the missing bar is more than `length` bars back. ta::Mom keeps a window
// of present samples instead and answers 6 (8 - 2) on bar 3 and 12 (16 - 4)
// on bar 4; its own contract is tests/test_mom_na_recompute.cpp.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/na.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

using pineforge::na;
using pineforge::source::PineMom;

int failures = 0;

// Bit identity; any two NaNs count as the same na.
void expect(const char* row, int bar, const char* call, double got, double want) {
    bool equal = false;
    if (std::isnan(got) || std::isnan(want)) {
        equal = std::isnan(got) && std::isnan(want);
    } else {
        std::uint64_t got_bits = 0;
        std::uint64_t want_bits = 0;
        std::memcpy(&got_bits, &got, sizeof got_bits);
        std::memcpy(&want_bits, &want, sizeof want_bits);
        equal = got_bits == want_bits;
    }
    std::printf("%s %s bar %d %s: got %.17g, want %.17g\n",
                equal ? "PASS" : "FAIL", row, bar, call, got, want);
    if (!equal) ++failures;
}

void expect_true(const char* row, const char* what, bool held) {
    std::printf("%s %s %s\n", held ? "PASS" : "FAIL", row, what);
    if (!held) ++failures;
}

const double kMissing = na<double>();

// One compute() per bar: the missing bar holds position 2.
void ordinary() {
    const double source[] = {2, 4, kMissing, 8, 16, 10, 22};
    const double want[] = {kMissing, kMissing, kMissing, 4, kMissing, 2, 6};
    PineMom mom(2);
    for (int bar = 0; bar < 7; ++bar) {
        expect("ordinary", bar, "compute", mom.compute(source[bar]), want[bar]);
    }
}

// Bar 2 is first missing, then recalculated with a value: the value takes the
// missing bar's position, it does not add one.
void missing_then_value() {
    PineMom mom(2);
    expect("na->value", 0, "compute", mom.compute(2), kMissing);
    expect("na->value", 1, "compute", mom.compute(4), kMissing);
    expect("na->value", 2, "compute", mom.compute(kMissing), kMissing);
    expect("na->value", 2, "recompute", mom.recompute(6), 4);    // 6 - 2
    expect("na->value", 3, "compute", mom.compute(8), 4);        // 8 - 4
    expect("na->value", 4, "compute", mom.compute(16), 10);      // 16 - 6
}

// Bar 2 first has a value, then is recalculated as missing: its position
// stays, holding na.
void value_then_missing() {
    PineMom mom(2);
    expect("value->na", 0, "compute", mom.compute(2), kMissing);
    expect("value->na", 1, "compute", mom.compute(4), kMissing);
    expect("value->na", 2, "compute", mom.compute(6), 4);        // 6 - 2
    expect("value->na", 2, "recompute", mom.recompute(kMissing), kMissing);
    expect("value->na", 3, "compute", mom.compute(8), 4);        // 8 - 4
    expect("value->na", 4, "compute", mom.compute(16), kMissing);
    expect("value->na", 5, "compute", mom.compute(10), 2);       // 10 - 8
}

// Recalculations of the bar after the gap move only that bar's value.
void recalculated_after_gap() {
    PineMom mom(2);
    mom.compute(2);
    mom.compute(4);
    mom.compute(kMissing);
    expect("recalculated", 3, "compute", mom.compute(8), 4);     // 8 - 4
    expect("recalculated", 3, "recompute", mom.recompute(9), 5); // 9 - 4
    expect("recalculated", 3, "recompute", mom.recompute(8), 4);
    expect("recalculated", 4, "compute", mom.compute(16), kMissing);
    expect("recalculated", 5, "compute", mom.compute(10), 2);    // 10 - 8
}

// With no na after the first value the positions are the present samples, so
// every answer is ta::Mom's, bit for bit: two leading na bars, then invented
// values; each bar is computed, recalculated with a moved value and
// recalculated with its own value. Length -1 answers na and length 0 reads
// the current value in both.
void equals_native_without_gap() {
    for (int length : {-1, 0, 1, 2, 5}) {
        PineMom adapter(length);
        pineforge::ta::Mom native(length);
        char row[48];
        std::snprintf(row, sizeof row, "no-gap length %d", length);
        for (int bar = 0; bar < 24; ++bar) {
            const double value =
                bar < 2 ? kMissing : 50.0 + 3.0 * bar + (bar % 4) * 1.5 - (bar % 3) * 2.25;
            expect(row, bar, "compute", adapter.compute(value), native.compute(value));
            expect(row, bar, "recompute moved", adapter.recompute(value + 3.5),
                   native.recompute(value + 3.5));
            expect(row, bar, "recompute", adapter.recompute(value), native.recompute(value));
        }
    }
}

// The history is reported as owned storage: nothing before the first call,
// the three slots of a length-2 call after it.
void owned_storage() {
    PineMom mom(2);
    pineforge::OwnedStorage fresh;
    mom.observe_owned_storage(fresh);
    expect_true("owned storage", "is complete before the first call", fresh.usage().complete());
    expect_true("owned storage", "is empty before the first call",
                fresh.usage().resident_payload_bytes == 0);
    for (double value : {2.0, 4.0, 8.0, 16.0}) mom.compute(value);
    pineforge::OwnedStorage used;
    mom.observe_owned_storage(used);
    expect_true("owned storage", "is complete after four calls", used.usage().complete());
    expect_true("owned storage", "holds at least three slots after four calls",
                used.usage().resident_payload_bytes >= 3 * sizeof(double));
}

}  // namespace

int main() {
    ordinary();
    missing_then_value();
    value_then_missing();
    recalculated_after_gap();
    equals_native_without_gap();
    owned_storage();
    std::printf("%s test_pine_mom_missing_bar: %d failing rows\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures != 0;
}
