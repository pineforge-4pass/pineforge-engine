// test_ta_pivot_na_left_window -- ta::PivotHigh / ta::PivotLow when bars to the
// left of the candidate hold no value yet.
//
// A source that is na during its own warm-up (an oscillator, a moving average)
// has its first values within `leftbars` of its na bars. A candidate among them
// has na on some or all of its left bars. Every number here is invented.
//
// HYPOTHESIS. This file pins one reading of that corner: a left bar without a
// value does not compete. The candidate is a pivot when no left VALUE is beyond
// it and every right bar is a value on the near side of it. The reading rests on
// a difference seen against a reference chart, whose first signal needs such a
// pivot; it is not yet confirmed by a printed value, and a second reading (the
// script's own call running on every bar) fits the same difference.
//
// The value that decides it: ta.pivothigh(ta.rsi(close, 14), 5, 5) on bar 19 of
// a chart (bars counted from 0) whose RSI value on bar 14, its first, is higher
// than its next five. A number there confirms this reading. na refutes it, and
// then this file and the change it tests are to be withdrawn.
//
// Red before the change to the left-side loops of PivotHigh / PivotLow: the
// checks named "target" (the old loops rejected a candidate as soon as one left
// bar was na). The checks named "unchanged" hold before and after it; they mark
// what the change leaves alone: a left value beyond the candidate, an na
// candidate, an na right bar, and a window that is not complete yet.

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cstddef>
#include <cstdio>
#include <vector>

namespace ta = pineforge::ta;
using pineforge::is_na;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
    if (ok) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("FAIL %s\n", what);
}

bool same(double got, double want) {
    if (is_na(want)) return is_na(got);
    return !is_na(got) && got == want;
}

const double kNa = pineforge::na<double>();

template <class Pivot>
std::vector<double> run_pivot(Pivot pivot, const std::vector<double>& source) {
    std::vector<double> out;
    out.reserve(source.size());
    for (double value : source) out.push_back(pivot.compute(value));
    return out;
}

void print_values(const char* name, const std::vector<double>& values) {
    std::printf("  %s:", name);
    for (double value : values) {
        if (is_na(value)) {
            std::printf(" na");
        } else {
            std::printf(" %.17g", value);
        }
    }
    std::printf("\n");
}

// The first value of the source is the highest of its neighbourhood and all
// three bars to its left are na.
void high_with_an_all_na_left_window() {
    const std::vector<double> source = {kNa,  kNa,  kNa,   kNa,  61.5, 58.25,
                                        57.0, 59.0, 64.75, 60.5, 58.0};
    const std::vector<double> out = run_pivot(ta::PivotHigh(3, 2), source);
    print_values("high, all-na left window", out);

    for (std::size_t bar = 0; bar < 5; ++bar) {
        check(is_na(out[bar]), "unchanged, high: no result before six bars have been seen");
    }
    check(is_na(out[5]), "unchanged, high: an na candidate (bar 3) is no pivot");
    check(same(out[6], 61.5),
          "target, high: bar 4 (61.5) with three na left bars is a pivot, reported on bar 6");
    check(is_na(out[7]), "unchanged, high: bar 5 (58.25) has the higher 61.5 on its left");
    check(is_na(out[8]), "unchanged, high: bar 6 (57.0) has the higher 61.5 on its left");
    check(is_na(out[9]), "unchanged, high: bar 7 (59.0) has the higher 61.5 on its left");
    check(same(out[10], 64.75),
          "unchanged, high: an ordinary pivot (bar 8, 64.75) is reported on bar 10");
}

// One left bar is a value on the allowed side, the other two are na.
void low_with_a_partly_na_left_window() {
    const std::vector<double> source = {kNa, kNa, 34.0, 27.5, 31.25, 30.0};
    const std::vector<double> out = run_pivot(ta::PivotLow(3, 2), source);
    print_values("low, partly-na left window", out);

    for (std::size_t bar = 0; bar < 5; ++bar) {
        check(is_na(out[bar]), "unchanged, low: no result before six bars have been seen");
    }
    check(same(out[5], 27.5),
          "target, low: bar 3 (27.5) with left bars na, na, 34.0 is a pivot, reported on bar 5");
}

// The mirrors of the two shapes above.
void mirrors() {
    const std::vector<double> lows = {kNa,  kNa,  kNa,   kNa,  38.5, 41.75,
                                      43.0, 41.0, 35.25, 39.5, 42.0};
    const std::vector<double> low_out = run_pivot(ta::PivotLow(3, 2), lows);
    print_values("low, all-na left window", low_out);
    check(is_na(low_out[5]), "unchanged, low: an na candidate (bar 3) is no pivot");
    check(same(low_out[6], 38.5),
          "target, low: bar 4 (38.5) with three na left bars is a pivot, reported on bar 6");
    check(is_na(low_out[7]) && is_na(low_out[8]) && is_na(low_out[9]),
          "unchanged, low: bars 5 to 7 have the lower 38.5 on their left");
    check(same(low_out[10], 35.25),
          "unchanged, low: an ordinary pivot (bar 8, 35.25) is reported on bar 10");

    const std::vector<double> highs = {kNa, kNa, 66.0, 72.5, 68.75, 70.0};
    const std::vector<double> high_out = run_pivot(ta::PivotHigh(3, 2), highs);
    print_values("high, partly-na left window", high_out);
    check(same(high_out[5], 72.5),
          "target, high: bar 3 (72.5) with left bars na, na, 66.0 is a pivot, reported on bar 5");
}

// What the change leaves alone.
void unchanged_rejections() {
    const std::vector<double> higher_left = {kNa, kNa, 70.0, 61.5, 58.25, 57.0};
    check(is_na(run_pivot(ta::PivotHigh(3, 2), higher_left)[5]),
          "unchanged, high: a left value above the candidate (70.0 over 61.5) rejects it");
    const std::vector<double> lower_left = {kNa, kNa, 20.0, 27.5, 31.25, 30.0};
    check(is_na(run_pivot(ta::PivotLow(3, 2), lower_left)[5]),
          "unchanged, low: a left value below the candidate (20.0 under 27.5) rejects it");

    const std::vector<double> na_right_high = {50.0, 51.0, 52.0, 61.5, kNa, 57.0};
    check(is_na(run_pivot(ta::PivotHigh(3, 2), na_right_high)[5]),
          "unchanged, high: an na right bar rejects the candidate");
    const std::vector<double> na_right_low = {40.0, 39.0, 38.0, 27.5, kNa, 30.0};
    check(is_na(run_pivot(ta::PivotLow(3, 2), na_right_low)[5]),
          "unchanged, low: an na right bar rejects the candidate");

    // A source with values from its first bar: the candidate on bar 0 has no bars
    // to its left at all, and no result comes before six bars have been seen.
    const std::vector<double> from_first_bar = {61.5, 58.25, 57.0, 56.0, 55.0};
    const std::vector<double> short_out = run_pivot(ta::PivotHigh(3, 2), from_first_bar);
    bool all_na = true;
    for (double value : short_out) all_na = all_na && is_na(value);
    check(all_na, "unchanged, high: no result while fewer than left + right + 1 bars have been seen");
}

// recompute() replaces the current bar's value and must answer as compute() does.
void recompute_agrees() {
    ta::PivotHigh high(3, 2);
    for (double value : {kNa, kNa, kNa, kNa, 61.5, 58.25}) high.compute(value);
    check(same(high.compute(57.0), 61.5), "target, high: compute on the confirming bar reports 61.5");
    check(same(high.recompute(57.0), 61.5), "target, high: recompute with the same value reports 61.5");
    check(is_na(high.recompute(62.0)),
          "unchanged, high: recompute with a right value above the candidate reports na");
    check(same(high.recompute(56.5), 61.5),
          "target, high: recompute back below the candidate reports 61.5");

    ta::PivotLow low(3, 2);
    for (double value : {kNa, kNa, kNa, kNa, 38.5, 41.75}) low.compute(value);
    check(same(low.compute(43.0), 38.5), "target, low: compute on the confirming bar reports 38.5");
    check(same(low.recompute(43.0), 38.5), "target, low: recompute with the same value reports 38.5");
    check(is_na(low.recompute(38.0)),
          "unchanged, low: recompute with a right value below the candidate reports na");
    check(same(low.recompute(43.5), 38.5),
          "target, low: recompute back above the candidate reports 38.5");
}

// The shape the difference was seen in: the first value of an RSI. A 3-bar RSI
// has its first value on bar 3. After three rises it is exactly 100, after three
// falls exactly 0, and the two bars that follow move it inside the range.
void first_value_of_an_rsi() {
    {
        ta::RSI rsi(3);
        ta::PivotHigh pivot(2, 2);
        std::vector<double> values;
        std::vector<double> out;
        for (double close : {50.0, 51.0, 52.5, 54.0, 53.0, 52.0}) {
            values.push_back(rsi.compute(close));
            out.push_back(pivot.compute(values.back()));
        }
        print_values("rsi after three rises", values);
        print_values("its pivot high", out);
        check(is_na(values[0]) && is_na(values[1]) && is_na(values[2]),
              "premise, rsi high: bars 0 to 2 are na");
        check(same(values[3], 100.0), "premise, rsi high: the first value, on bar 3, is 100");
        check(!is_na(values[4]) && values[4] < 100.0 && !is_na(values[5]) && values[5] < 100.0,
              "premise, rsi high: bars 4 and 5 are below 100");
        check(same(out[5], 100.0),
              "target, rsi high: the first value is a pivot high, reported on bar 5");
    }
    {
        ta::RSI rsi(3);
        ta::PivotLow pivot(2, 2);
        std::vector<double> values;
        std::vector<double> out;
        for (double close : {50.0, 49.0, 47.5, 46.0, 47.0, 48.0}) {
            values.push_back(rsi.compute(close));
            out.push_back(pivot.compute(values.back()));
        }
        print_values("rsi after three falls", values);
        print_values("its pivot low", out);
        check(is_na(values[0]) && is_na(values[1]) && is_na(values[2]),
              "premise, rsi low: bars 0 to 2 are na");
        check(same(values[3], 0.0), "premise, rsi low: the first value, on bar 3, is 0");
        check(!is_na(values[4]) && values[4] > 0.0 && !is_na(values[5]) && values[5] > 0.0,
              "premise, rsi low: bars 4 and 5 are above 0");
        check(same(out[5], 0.0),
              "target, rsi low: the first value is a pivot low, reported on bar 5");
    }
}

}  // namespace

int main() {
    high_with_an_all_na_left_window();
    low_with_a_partly_na_left_window();
    mirrors();
    unchanged_rejections();
    recompute_agrees();
    first_value_of_an_rsi();
    std::printf("test_ta_pivot_na_left_window: passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
