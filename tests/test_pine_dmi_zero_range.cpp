// Pine ta.dmi on a bar whose averaged true range is zero.
//
// With diLength 1 the averaged range is the bar's own true range, so a bar
// whose high and low equal the previous close has a zero range and both
// quotients 100 * rma(DM) / rma(TR) are 0 / 0. The Pine reference definition
// wraps each quotient in fixnan: both directional indices keep their last
// values and ADX takes one more step from them. The kernel class ta::DMI
// answers three na on that bar and leaves ADX where it was
// (src/ta_volatility_trend.cpp, the `smoothed_tr == 0.0` return).
//
// PineDmi below is the class a Pine ta.dmi call is built from:
// source::PineDmiReference where the engine has it, ta::DMI before that. The
// second branch is what makes this file fail first: on a tree without
// PineDmiReference the flat bar reads three na and the two bars after it
// carry an ADX that missed one step.
//
// Every price is invented. The expected values follow the reference
// definition as written; no TradingView tape of the built-in on a zero-range
// bar is recorded here.
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/na.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using pineforge::is_na;
using pineforge::na;
using pineforge::OwnedStorage;
namespace ta = pineforge::ta;

namespace {

#ifdef PINEFORGE_HAS_PINE_DMI_REFERENCE
using PineDmi = pineforge::source::PineDmiReference;
#else
using PineDmi = ta::DMI;
#endif

int tests_passed = 0;
int tests_failed = 0;

struct Sample {
    double high, low, close;
};

struct Row {
    Sample bar;
    double diplus, diminus, adx;
};

void record(bool ok, const char* tape, int bar, const char* field, double actual,
            double expected) {
    if (ok) {
        ++tests_passed;
        return;
    }
    ++tests_failed;
    std::printf("  FAIL  %s bar %d %s: got %.17g, expected %.17g\n", tape, bar, field, actual,
                expected);
}

// One field of one bar: na matches na only, a number within 1e-9.
void expect_field(const char* tape, int bar, const char* field, double actual, double expected) {
    const bool ok = is_na(expected) ? is_na(actual)
                                    : (!is_na(actual) && std::fabs(actual - expected) < 1e-9);
    record(ok, tape, bar, field, actual, expected);
}

void expect_row(const char* tape, int bar, const ta::DMIResult& result, const Row& row) {
    expect_field(tape, bar, "+DI", result.diplus, row.diplus);
    expect_field(tape, bar, "-DI", result.diminus, row.diminus);
    expect_field(tape, bar, "ADX", result.adx, row.adx);
}

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// One field of one bar, bit for bit; any two NaNs count as the same na.
void expect_bits(const char* tape, int bar, const char* field, double actual, double expected) {
    const bool ok = is_na(expected) ? is_na(actual)
                                    : (!is_na(actual) && bits(actual) == bits(expected));
    record(ok, tape, bar, field, actual, expected);
}

void expect_same(const char* tape, int bar, const ta::DMIResult& actual,
                 const ta::DMIResult& expected) {
    expect_bits(tape, bar, "+DI", actual.diplus, expected.diplus);
    expect_bits(tape, bar, "-DI", actual.diminus, expected.diminus);
    expect_bits(tape, bar, "ADX", actual.adx, expected.adx);
}

// ta.dmi(1, 2) on seven bars. Lengths: +DI and -DI read the bar's own
// directional movement over its own true range; ADX is the two-bar average
// of DX, seeded by the mean of the first two.
//
//   bar 1  +DM 1 over a range of 2: +DI 50, DX 100, ADX not seeded yet.
//   bar 2  no movement, range 2: both DI 0, DX 0, ADX (100 + 0) / 2 = 50.
//   bar 3  +DM 1 over a range of 2: +DI 50, DX 100, ADX (100 + 50) / 2 = 75.
//   bar 4  high = low = bar 3's close: range 0, both quotients 0 / 0. The
//          held pair is bar 3's (50, 0), DX 100, ADX (100 + 75) / 2 = 87.5.
//   bar 5  -DM 2 over a range of 2: -DI 100, DX 100, ADX (100 + 87.5) / 2.
//   bar 6  no movement, range 2: both DI 0, DX 0, ADX 93.75 / 2.
std::vector<Row> flat_tape() {
    const double none = na<double>();
    return {
        {{10.0, 8.0, 9.0}, none, none, none},
        {{11.0, 9.0, 10.0}, 50.0, 0.0, none},
        {{11.0, 9.0, 10.0}, 0.0, 0.0, 50.0},
        {{12.0, 10.0, 12.0}, 50.0, 0.0, 75.0},
        {{12.0, 12.0, 12.0}, 50.0, 0.0, 87.5},
        {{12.0, 10.0, 11.0}, 0.0, 100.0, 93.75},
        {{12.0, 10.0, 11.0}, 0.0, 0.0, 46.875},
    };
}

void test_flat_bar_holds_both_indices_and_steps_adx() {
    std::printf("test_flat_bar_holds_both_indices_and_steps_adx\n");
    const std::vector<Row> rows = flat_tape();
    PineDmi dmi(1, 2);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Sample& bar = rows[i].bar;
        expect_row("flat", i, dmi.compute(bar.high, bar.low, bar.close), rows[i]);
    }
}

// Bar 4 first arrives as a tick with a range (12 / 9 / 10: -DM 1 over a true
// range of 3, DX 100, ADX (100 + 75) / 2) and closes flat. The replacement
// must find bar 3's pair again, not the tick's, and ADX must step once
// however often the bar is replaced.
void test_flat_close_replaces_a_tick_with_range() {
    std::printf("test_flat_close_replaces_a_tick_with_range\n");
    const std::vector<Row> rows = flat_tape();
    PineDmi dmi(1, 2);
    for (int i = 0; i < 4; ++i) {
        const Sample& bar = rows[i].bar;
        expect_row("replaced", i, dmi.compute(bar.high, bar.low, bar.close), rows[i]);
    }
    const ta::DMIResult tick = dmi.compute(12.0, 9.0, 10.0);
    expect_field("replaced", 4, "tick +DI", tick.diplus, 0.0);
    expect_field("replaced", 4, "tick -DI", tick.diminus, 100.0 * 1.0 / 3.0);
    expect_field("replaced", 4, "tick ADX", tick.adx, 87.5);
    expect_row("replaced", 4, dmi.recompute(12.0, 12.0, 12.0), rows[4]);
    expect_row("replaced", 4, dmi.recompute(12.0, 12.0, 12.0), rows[4]);
    for (int i = 5; i < static_cast<int>(rows.size()); ++i) {
        const Sample& bar = rows[i].bar;
        expect_row("replaced", i, dmi.compute(bar.high, bar.low, bar.close), rows[i]);
    }
}

// The other order: bar 4 first reads flat, then a tick with a range replaces
// it. The flat reading's ADX step must be undone: the tick's DX 100 averages
// with bar 3's 75 (87.5), not with the flat reading's 87.5 (93.75).
void test_tick_with_range_replaces_a_flat_reading() {
    std::printf("test_tick_with_range_replaces_a_flat_reading\n");
    const std::vector<Row> rows = flat_tape();
    PineDmi dmi(1, 2);
    for (int i = 0; i < 5; ++i) {
        const Sample& bar = rows[i].bar;
        expect_row("unflat", i, dmi.compute(bar.high, bar.low, bar.close), rows[i]);
    }
    const ta::DMIResult tick = dmi.recompute(12.0, 9.0, 10.0);
    expect_field("unflat", 4, "tick +DI", tick.diplus, 0.0);
    expect_field("unflat", 4, "tick -DI", tick.diminus, 100.0 * 1.0 / 3.0);
    expect_field("unflat", 4, "tick ADX", tick.adx, 87.5);
}

// Control, the same before and after: flat bars at the start of the series
// have no earlier pair to hold, so they stay na and give ADX no input. Had
// they been read as (0, 0) with DX 0, ADX would be seeded on bar 2 and read
// 50 on bar 3 and 25 on bar 4.
//
//   bars 0-2  9 / 9 / 9: range 0 from bar 1 on, nothing to hold.
//   bar 3     +DM 2 over a range of 2: +DI 100, DX 100, ADX's first input.
//   bar 4     no movement, range 2: both DI 0, DX 0, ADX (100 + 0) / 2 = 50.
void test_leading_flat_bars_hold_nothing() {
    std::printf("test_leading_flat_bars_hold_nothing\n");
    const double none = na<double>();
    const std::vector<Row> rows = {
        {{9.0, 9.0, 9.0}, none, none, none},
        {{9.0, 9.0, 9.0}, none, none, none},
        {{9.0, 9.0, 9.0}, none, none, none},
        {{11.0, 9.0, 10.0}, 100.0, 0.0, none},
        {{11.0, 9.0, 10.0}, 0.0, 0.0, 50.0},
    };
    PineDmi dmi(1, 2);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Sample& bar = rows[i].bar;
        expect_row("leading", i, dmi.compute(bar.high, bar.low, bar.close), rows[i]);
    }
}

// A deterministic sequence in which every bar has a range and every
// directional movement is 0 or at least 0.25: bar i has
// base = 100 + 0.75 i + (i % 7) * 1.25 - (i % 3) * 2,
// high = base + 1.5 + (i % 4) * 0.5, low = base - 1 - (i % 5) * 0.25,
// close = base + 0.5 on odd bars and base - 0.5 on even ones; the high is na
// on every bar with i % 9 == 4.
std::vector<Sample> ranged_bars() {
    std::vector<Sample> out;
    for (int i = 0; i < 60; ++i) {
        const double base = 100.0 + 0.75 * i + (i % 7) * 1.25 - (i % 3) * 2.0;
        Sample bar{base + 1.5 + (i % 4) * 0.5, base - 1.0 - (i % 5) * 0.25,
                   base + ((i % 2) ? 0.5 : -0.5)};
        if (i % 9 == 4) bar.high = na<double>();
        out.push_back(bar);
    }
    return out;
}

// Control, the same before and after: on bars with a range and an ordinary
// sum of the two indices the Pine class answers ta::DMI's values bit for
// bit, through compute, a replacement with a moved high, and the replacement
// with the bar's own values.
void test_bars_with_a_range_answer_the_kernel_values() {
    std::printf("test_bars_with_a_range_answer_the_kernel_values\n");
    const int lengths[][2] = {{1, 1}, {1, 2}, {3, 5}, {14, 14}};
    const std::vector<Sample> seq = ranged_bars();
    for (const auto& pair : lengths) {
        PineDmi pine(pair[0], pair[1]);
        ta::DMI kernel(pair[0], pair[1]);
        for (int i = 0; i < static_cast<int>(seq.size()); ++i) {
            const Sample& bar = seq[i];
            expect_same("ranged", i, pine.compute(bar.high, bar.low, bar.close),
                        kernel.compute(bar.high, bar.low, bar.close));
            expect_same("ranged", i, pine.recompute(bar.high + 3.5, bar.low, bar.close),
                        kernel.recompute(bar.high + 3.5, bar.low, bar.close));
            expect_same("ranged", i, pine.recompute(bar.high, bar.low, bar.close),
                        kernel.recompute(bar.high, bar.low, bar.close));
        }
    }
}

// Control, the same before and after: the shapes generated code uses for a
// ta.dmi member -- a member built from two int lengths, the reset assignment
// `member = Type(lengths)`, a FirstCallBound for a length known on the first
// execution, a replacement as the very first call, and the owned-storage
// observation.
void test_generated_code_shapes() {
    std::printf("test_generated_code_shapes\n");
    const std::vector<Row> rows = flat_tape();
    pineforge::source::FirstCallBound<PineDmi> bound;
    PineDmi member(3, 3);
    member = PineDmi(1, 2);
    PineDmi replaced_first(1, 2);
    PineDmi straight(1, 2);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Sample& bar = rows[i].bar;
        const ta::DMIResult want = straight.compute(bar.high, bar.low, bar.close);
        expect_same("bound", i,
                    bound.compute([] { return PineDmi(1, 2); }, bar.high, bar.low, bar.close),
                    want);
        expect_same("reset", i, member.compute(bar.high, bar.low, bar.close), want);
        const ta::DMIResult again =
            i == 0 ? replaced_first.recompute(bar.high, bar.low, bar.close)
                   : replaced_first.compute(bar.high, bar.low, bar.close);
        expect_same("replaced first", i, again, want);
    }
    OwnedStorage observed;
    bound.observe_owned_storage(observed);
    member.observe_owned_storage(observed);
    const bool complete = observed.usage().complete();
    record(complete, "observed", 0, "complete", complete ? 1.0 : 0.0, 1.0);
}

}  // namespace

int main() {
    test_flat_bar_holds_both_indices_and_steps_adx();
    test_flat_close_replaces_a_tick_with_range();
    test_tick_with_range_replaces_a_flat_reading();
    test_leading_flat_bars_hold_nothing();
    test_bars_with_a_range_answer_the_kernel_values();
    test_generated_code_shapes();
    std::printf("pine_dmi_zero_range: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
