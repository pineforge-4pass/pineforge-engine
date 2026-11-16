/*
 * test_pine_ta_ready_publication.cpp — source::PineEma / PineTsi / PineKc /
 * PineKcw / PineMacd publish a value only once its inputs are ready.
 *
 * One target: on every bar the published value is the kernel class's number
 * where the inputs are ready and na where they are not. "Ready" is counted
 * here from the inputs alone (ta_stage_clock_fixture.hpp); the number is the
 * kernel class's own, fed the same inputs, so no expected value is computed
 * by the class under test and none is a literal.
 *
 * Rows (each over both seedings: the first value, and the average seeding the
 * host raises for a declared series origin):
 *   ema          lengths 14, 1, 2, 3, 20, 200; plain / gapped / flat sources.
 *   ema-nested   an outer EMA(3) of a published EMA.
 *   tsi          (13,25) (1,1) (2,5) (5,2) (1,4) (4,1); a flat source keeps
 *                answering na (zero denominator).
 *   kc           lengths 20, 1, 2, 3; true range and high - low; multipliers
 *                1.5 and 0; na sources, highs and closes; the omitted flag is
 *                the true range.
 *   kcw          the same cases; a zero multiplier publishes the width 0.
 *   macd         control: PineMacd is ta::MACD, value for value.
 *   recompute    compute -> recompute sequences equal one compute of the
 *                final inputs, across every availability boundary.
 *   copy         a copy taken on any bar answers what its original answers,
 *                and neither moves the other.
 *
 * What each check is.
 *
 * TradingView observations kept in this tree, and the only checks bound to
 * them:
 *   - ta.ema without na sources is na for its first length - 1 values
 *     (tests/test_chart_ema_na_warmup.cpp: "na for length-1 values, then an
 *     SMA seed"; tests/test_series_origin_tailh_tape.cpp compares the
 *     declared-origin warm-up with a TradingView tape). Row `ema`, the plain
 *     and flat sources: the first published bar is length - 1.
 *   - ta.macd(close, 12, 26, 9): the histogram is first defined on bar 33
 *     (codegen issue 291, the reporter's TradingView run) and the slow
 *     EMA26 first reads on its 26th bar (src/source/pine_security_eval.cpp,
 *     retained weekly reference). Row `macd`, that one length triple.
 *   - a zero ta.tsi denominator answers na (tests/test_ta_osc_edge.cpp).
 *     Row `tsi`, the flat sources.
 *
 * This implementation's stage rule, held on every bar but NOT a TradingView
 * observation: the per-bar comparison with the counted clocks for ta.tsi,
 * ta.kc and ta.kcw, for ta.ema with na sources, and for the nested EMA. No
 * first bar of those is written here as an expectation. Still to be measured
 * against TradingView, each on the issue 291 matrix:
 *   - which seeding a run without a declared origin has (matrix row 11, the
 *     value on bar 13);
 *   - the first bar and the composition of ta.tsi (row 20);
 *   - the bar a stage becomes ready on when an na source falls inside its
 *     warm-up (rows 17, 26, 34);
 *   - the first bars of ta.kc's middle band and bands and of ta.kcw (rows
 *     29, 33, 37).
 * No early NUMBER is claimed to be TradingView's under the first-value
 * seeding.
 *
 * Controls: under the average seeding the published value is the kernel's on
 * every bar (the kernel is already na exactly while unready); under the first
 * value the withheld kernel values are counted, so a class that withheld
 * nothing, or everything, fails; recompute and copies are input-dependent
 * invariants.
 *
 * NDEBUG-proof: a counting CHECK and a nonzero exit, never a bare assert().
 */

#include "ta_stage_clock_fixture.hpp"

#include <pineforge/na.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <cstdio>
#include <initializer_list>
#include <string>

using namespace pineforge;
using namespace stage_clock_fixture;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr, ...)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            if (tests_failed < 40) {                                           \
                std::printf("  FAIL %s:%d %s -- ", __FILE__, __LINE__, #expr); \
                std::printf(__VA_ARGS__);                                      \
                std::printf("\n");                                             \
            }                                                                  \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

// Plain, Gapped, Flat, FlatGapped: the index of a shape in the arrays below.
static const Shape kShapes[] = {Shape::Plain, Shape::Gapped, Shape::Flat, Shape::FlatGapped};

static double masked(bool ready, double value) { return ready ? value : na<double>(); }

// --------------------------------------------------------------------------
// ta.ema
// --------------------------------------------------------------------------

static const int kEmaLengths[] = {14, 1, 2, 3, 20, 200};
// Long enough for 200 defined sources of the gapped shape.
static const int kEmaBars = 260;

static void test_ema() {
    std::printf("ema\n");
    for (bool average : {false, true}) {
        for (int length : kEmaLengths) {
            int first_of[4] = {-1, -1, -1, -1};
            int shape_index = 0;
            for (Shape shape : kShapes) {
                SeedingCase seeding(average);
                source::PineEma pine(length);
                ta::EMA kernel(length);
                EmaClock clock(length);
                int first = -1;
                int withheld = 0;
                for (int bar = 0; bar < kEmaBars; ++bar) {
                    const double src = source_of(shape, bar);
                    const double got = pine.compute(src);
                    const double raw = kernel.compute(src);
                    const double want = masked(clock.step(src), raw);
                    CHECK(same(got, want), "ema(%d) %s %s bar %d value %.17g want %.17g", length,
                          shape_name(shape), seeding_name(average), bar, got, want);
                    if (average) {
                        CHECK(same(got, raw), "ema(%d) %s average bar %d value %.17g kernel %.17g",
                              length, shape_name(shape), bar, got, raw);
                    }
                    if (!is_na(got) && first < 0) first = bar;
                    if (!is_na(raw) && is_na(got)) ++withheld;
                }
                CHECK(first >= 0, "ema(%d) %s %s never published", length, shape_name(shape),
                      seeding_name(average));
                if (!is_gapped(shape)) {
                    // TradingView's warm-up without na sources: na for the
                    // first length - 1 values. The first-value kernel has a
                    // number on each of them and they are withheld; the
                    // average-seeded kernel has none to withhold.
                    CHECK(first == length - 1, "ema(%d) %s %s first published bar %d", length,
                          shape_name(shape), seeding_name(average), first);
                    CHECK(withheld == (average ? 0 : length - 1),
                          "ema(%d) %s %s withheld %d kernel values", length, shape_name(shape),
                          seeding_name(average), withheld);
                } else {
                    // With na sources only the rule's shape is held: nothing
                    // is withheld from the average-seeded kernel or from a
                    // length of 1, something is otherwise.
                    CHECK((withheld > 0) == (!average && length > 1),
                          "ema(%d) %s %s withheld %d kernel values", length, shape_name(shape),
                          seeding_name(average), withheld);
                }
                first_of[shape_index++] = first;
            }
            // The clock reads definedness, not values, and removing sources
            // can only delay it.
            CHECK(first_of[2] == first_of[0] && first_of[3] == first_of[1],
                  "ema(%d) %s flat first bars (%d %d) moving (%d %d)", length,
                  seeding_name(average), first_of[2], first_of[3], first_of[0], first_of[1]);
            CHECK(first_of[1] > first_of[0], "ema(%d) %s gaps did not delay: %d against %d",
                  length, seeding_name(average), first_of[1], first_of[0]);
        }
    }
}

static void test_ema_nested() {
    std::printf("ema-nested\n");
    const int outer_length = 3;
    for (bool average : {false, true}) {
        for (int length : kEmaLengths) {
            for (Shape shape : kShapes) {
                SeedingCase seeding(average);
                source::PineEma inner(length);
                source::PineEma outer(outer_length);
                ta::EMA kernel_inner(length);
                ta::EMA kernel_outer(outer_length);
                EmaClock inner_clock(length);
                EmaClock outer_clock(outer_length);
                int first = -1;
                int first_inner = -1;
                for (int bar = 0; bar < kEmaBars; ++bar) {
                    const double src = source_of(shape, bar);
                    const double got = outer.compute(inner.compute(src));
                    // The outer stage is fed what the inner one publishes.
                    const double inner_raw = kernel_inner.compute(src);
                    const double inner_want = masked(inner_clock.step(src), inner_raw);
                    const double outer_raw = kernel_outer.compute(inner_want);
                    const double want = masked(outer_clock.step(inner_want), outer_raw);
                    CHECK(same(got, want), "ema(ema(%d),3) %s %s bar %d value %.17g want %.17g",
                          length, shape_name(shape), seeding_name(average), bar, got, want);
                    if (!is_na(inner_want) && first_inner < 0) first_inner = bar;
                    if (!is_na(got) && first < 0) first = bar;
                }
                // The outer call publishes, and only after the inner one: it
                // needs more than one of the inner call's values.
                CHECK(first_inner >= 0 && first > first_inner,
                      "ema(ema(%d),3) %s %s first bars inner %d outer %d", length,
                      shape_name(shape), seeding_name(average), first_inner, first);
            }
        }
    }
}

// --------------------------------------------------------------------------
// ta.tsi
// --------------------------------------------------------------------------

// {short_length, long_length}
static const int kTsiLengths[][2] = {
    {13, 25}, {1, 1}, {2, 5}, {5, 2}, {1, 4}, {4, 1},
};
static const int kBars = 160;

static void test_tsi() {
    std::printf("tsi\n");
    for (bool average : {false, true}) {
        // Kernel values withheld over every length pair of this seeding.
        int withheld_in_row = 0;
        for (const auto& lengths : kTsiLengths) {
            int first_of[4] = {-1, -1, -1, -1};
            int shape_index = 0;
            for (Shape shape : kShapes) {
                SeedingCase seeding(average);
                source::PineTsi pine(lengths[0], lengths[1]);
                ta::TSI kernel(lengths[0], lengths[1]);
                TsiClock clock(lengths[0], lengths[1]);
                int first = -1;
                int withheld = 0;
                for (int bar = 0; bar < kBars; ++bar) {
                    const double src = source_of(shape, bar);
                    const double got = pine.compute(src);
                    const double raw = kernel.compute(src);
                    const double want = masked(clock.step(src), raw);
                    CHECK(same(got, want), "tsi(%d,%d) %s %s bar %d value %.17g want %.17g",
                          lengths[0], lengths[1], shape_name(shape), seeding_name(average),
                          bar, got, want);
                    if (average) {
                        CHECK(same(got, raw),
                              "tsi(%d,%d) %s average bar %d value %.17g kernel %.17g",
                              lengths[0], lengths[1], shape_name(shape), bar, got, raw);
                    }
                    if (!is_na(got) && first < 0) first = bar;
                    if (!is_na(raw) && is_na(got)) ++withheld;
                }
                withheld_in_row += withheld;
                // The average-seeded kernel has nothing to withhold.
                CHECK(!average || withheld == 0,
                      "tsi(%d,%d) %s average withheld %d kernel values", lengths[0], lengths[1],
                      shape_name(shape), withheld);
                // A flat source has a zero denominator: never a number. A
                // moving one publishes inside the run, after its first bar,
                // which has no change.
                if (is_flat(shape)) {
                    CHECK(first == -1, "tsi(%d,%d) %s %s flat source published on bar %d",
                          lengths[0], lengths[1], shape_name(shape), seeding_name(average),
                          first);
                } else {
                    CHECK(first > 0, "tsi(%d,%d) %s %s first published bar %d", lengths[0],
                          lengths[1], shape_name(shape), seeding_name(average), first);
                }
                first_of[shape_index++] = first;
            }
            // Removing sources can only delay the publication.
            CHECK(first_of[1] > first_of[0], "tsi(%d,%d) %s gaps did not delay: %d against %d",
                  lengths[0], lengths[1], seeding_name(average), first_of[1], first_of[0]);
        }
        // The first-value kernel has numbers before the stages are ready; a
        // class that withheld none of them over the whole row is the kernel.
        CHECK((withheld_in_row > 0) == !average, "tsi %s withheld %d kernel values in the row",
              seeding_name(average), withheld_in_row);
    }
}

// --------------------------------------------------------------------------
// ta.kc / ta.kcw
// --------------------------------------------------------------------------

static const int kKcLengths[] = {20, 1, 2, 3};
static const double kMultipliers[] = {1.5, 0.0};

static bool same_result(const ta::KCResult& a, const ta::KCResult& b) {
    return same(a.middle, b.middle) && same(a.upper, b.upper) && same(a.lower, b.lower);
}

static void test_kc() {
    std::printf("kc\n");
    for (bool average : {false, true}) {
        for (int length : kKcLengths) {
            for (bool use_true_range : {true, false}) {
                for (double mult : kMultipliers) {
                    int first_middle_of[2] = {-1, -1};
                    for (bool dirty : {false, true}) {
                        SeedingCase seeding(average);
                        const char* range_name = use_true_range ? "tr" : "hl";
                        const char* input_name = dirty ? "dirty" : "clean";
                        source::PineKc pine(length, mult, use_true_range);
                        source::PineKc omitted(length, mult);
                        source::PineKc spelled(length, mult, true);
                        ta::KC kernel(length, mult, use_true_range);
                        KcClock clock(length, use_true_range);
                        int first_middle = -1;
                        int first_band = -1;
                        int withheld_middle = 0;
                        for (int bar = 0; bar < kBars; ++bar) {
                            const Candle c = candle_of(bar, dirty);
                            const ta::KCResult got = pine.compute(c.src, c.high, c.low, c.close);
                            const ta::KCResult raw =
                                kernel.compute(c.src, c.high, c.low, c.close);
                            clock.step(c);
                            ta::KCResult want;
                            want.middle = masked(clock.basis_ready(), raw.middle);
                            want.upper = masked(clock.ready(), raw.upper);
                            want.lower = masked(clock.ready(), raw.lower);
                            CHECK(same_result(got, want),
                                  "kc(%d,%g,%s) %s %s bar %d (%.17g %.17g %.17g) want "
                                  "(%.17g %.17g %.17g)",
                                  length, mult, range_name, input_name, seeding_name(average),
                                  bar, got.middle, got.upper, got.lower, want.middle,
                                  want.upper, want.lower);
                            if (average) {
                                CHECK(same_result(got, raw),
                                      "kc(%d,%g,%s) %s average bar %d differs from the kernel",
                                      length, mult, range_name, input_name, bar);
                            }
                            // A zero multiplier still waits for the range
                            // stage, then puts both bands on the middle.
                            if (mult == 0.0 && !is_na(got.upper)) {
                                CHECK(same(got.upper, got.middle) && same(got.lower, got.middle),
                                      "kc(%d,0,%s) %s bar %d bands (%.17g %.17g) middle %.17g",
                                      length, range_name, input_name, bar, got.upper,
                                      got.lower, got.middle);
                            }
                            // A band is never published without its middle.
                            CHECK(is_na(got.upper) == is_na(got.lower)
                                      && (is_na(got.upper) || !is_na(got.middle)),
                                  "kc(%d,%g,%s) %s %s bar %d bands (%.17g %.17g) middle %.17g",
                                  length, mult, range_name, input_name, seeding_name(average),
                                  bar, got.upper, got.lower, got.middle);
                            // Omitting the flag is the true range.
                            const ta::KCResult a =
                                omitted.compute(c.src, c.high, c.low, c.close);
                            const ta::KCResult b =
                                spelled.compute(c.src, c.high, c.low, c.close);
                            CHECK(same_result(a, b),
                                  "kc(%d,%g) %s bar %d omitted flag differs from true", length,
                                  mult, input_name, bar);
                            if (!is_na(got.middle) && first_middle < 0) first_middle = bar;
                            if (!is_na(got.upper) && first_band < 0) first_band = bar;
                            if (!is_na(raw.middle) && is_na(got.middle)) ++withheld_middle;
                        }
                        // Both are published inside the run, the bands not
                        // before the middle. Nothing is withheld from the
                        // average-seeded kernel or from a length of 1,
                        // something is otherwise.
                        CHECK(first_middle >= 0 && first_band >= first_middle,
                              "kc(%d,%g,%s) %s %s first bars middle %d bands %d", length, mult,
                              range_name, input_name, seeding_name(average), first_middle,
                              first_band);
                        CHECK((withheld_middle > 0) == (!average && length > 1),
                              "kc(%d,%g,%s) %s %s withheld %d kernel middles", length, mult,
                              range_name, input_name, seeding_name(average), withheld_middle);
                        first_middle_of[dirty ? 1 : 0] = first_middle;
                    }
                    // Removing sources can only delay the middle band.
                    CHECK(first_middle_of[1] > first_middle_of[0],
                          "kc(%d,%g) %s na sources did not delay the middle: %d against %d",
                          length, mult, seeding_name(average), first_middle_of[1],
                          first_middle_of[0]);
                }
            }
        }
    }
}

static void test_kcw() {
    std::printf("kcw\n");
    for (bool average : {false, true}) {
        for (int length : kKcLengths) {
            for (bool use_true_range : {true, false}) {
                for (double mult : kMultipliers) {
                    for (bool dirty : {false, true}) {
                        SeedingCase seeding(average);
                        const char* range_name = use_true_range ? "tr" : "hl";
                        const char* input_name = dirty ? "dirty" : "clean";
                        source::PineKcw pine(length, mult, use_true_range);
                        source::PineKc bands(length, mult, use_true_range);
                        ta::KCW kernel(length, mult, use_true_range);
                        KcClock clock(length, use_true_range);
                        int first = -1;
                        for (int bar = 0; bar < kBars; ++bar) {
                            const Candle c = candle_of(bar, dirty);
                            const double got = pine.compute(c.src, c.high, c.low, c.close);
                            const double raw = kernel.compute(c.src, c.high, c.low, c.close);
                            const ta::KCResult channel =
                                bands.compute(c.src, c.high, c.low, c.close);
                            clock.step(c);
                            const double want = masked(clock.ready(), raw);
                            CHECK(same(got, want),
                                  "kcw(%d,%g,%s) %s %s bar %d value %.17g want %.17g", length,
                                  mult, range_name, input_name, seeding_name(average), bar, got,
                                  want);
                            if (average) {
                                CHECK(same(got, raw),
                                      "kcw(%d,%g,%s) %s average bar %d value %.17g kernel %.17g",
                                      length, mult, range_name, input_name, bar, got, raw);
                            }
                            // A width is never published without the bands of
                            // the same call.
                            CHECK(is_na(got) || !is_na(channel.upper),
                                  "kcw(%d,%g,%s) %s %s bar %d width %.17g without bands", length,
                                  mult, range_name, input_name, seeding_name(average), bar, got);
                            // Coincident bands have the width 0, not a
                            // missing one and not an invented one.
                            if (mult == 0.0 && !is_na(got)) {
                                CHECK(same(got, 0.0), "kcw(%d,0,%s) %s bar %d width %.17g",
                                      length, range_name, input_name, bar, got);
                            }
                            if (!is_na(got) && first < 0) first = bar;
                        }
                        CHECK(first >= 0, "kcw(%d,%g,%s) %s %s never published", length, mult,
                              range_name, input_name, seeding_name(average));
                    }
                }
            }
        }
    }
}

// --------------------------------------------------------------------------
// ta.macd
// --------------------------------------------------------------------------

// {fastlen, slowlen, siglen}
static const int kMacdLengths[][3] = {
    {12, 26, 9}, {2, 5, 3}, {5, 2, 1}, {1, 1, 2}, {1, 4, 1},
};

static bool same_result(const ta::MACDResult& a, const ta::MACDResult& b) {
    return same(a.macd_line, b.macd_line) && same(a.signal_line, b.signal_line)
        && same(a.histogram, b.histogram);
}

static void test_macd() {
    std::printf("macd\n");
    for (bool average : {false, true}) {
        for (const auto& lengths : kMacdLengths) {
            for (Shape shape : kShapes) {
                SeedingCase seeding(average);
                source::PineMacd pine(lengths[0], lengths[1], lengths[2]);
                ta::MACD kernel(lengths[0], lengths[1], lengths[2]);
                int histograms = 0;
                int first_line = -1;
                int first_histogram = -1;
                for (int bar = 0; bar < kBars; ++bar) {
                    const double src = source_of(shape, bar);
                    const ta::MACDResult got = pine.compute(src);
                    const ta::MACDResult raw = kernel.compute(src);
                    if (!is_na(got.histogram)) ++histograms;
                    if (!is_na(got.macd_line) && first_line < 0) first_line = bar;
                    if (!is_na(got.histogram) && first_histogram < 0) first_histogram = bar;
                    CHECK(same_result(got, raw),
                          "macd(%d,%d,%d) %s %s bar %d (%.17g %.17g %.17g) kernel "
                          "(%.17g %.17g %.17g)",
                          lengths[0], lengths[1], lengths[2], shape_name(shape),
                          seeding_name(average), bar, got.macd_line, got.signal_line,
                          got.histogram, raw.macd_line, raw.signal_line, raw.histogram);
                }
                CHECK(histograms > 0, "macd(%d,%d,%d) %s %s never published", lengths[0],
                      lengths[1], lengths[2], shape_name(shape), seeding_name(average));
                // The TradingView observations of these first bars: issue
                // 291's ta.macd(close, 12, 26, 9) histogram is first defined
                // on bar 33, and an EMA26 first reads on its 26th bar in the
                // retained weekly reference. The class a script calls keeps both
                // under either seeding. No other length triple is measured.
                if (shape == Shape::Plain && lengths[0] == 12 && lengths[1] == 26
                    && lengths[2] == 9) {
                    CHECK(first_line == 25 && first_histogram == 33,
                          "macd(12,26,9) %s first line bar %d, first histogram bar %d",
                          seeding_name(average), first_line, first_histogram);
                }
            }
        }
    }
}

// --------------------------------------------------------------------------
// recompute and copies
// --------------------------------------------------------------------------

static void test_recompute() {
    std::printf("recompute\n");
    for (bool average : {false, true}) {
        SeedingCase seeding(average);
        for (int length : {14, 3, 1}) {
            source::PineEma live(length);
            source::PineEma committed(length);
            for (int bar = 0; bar < kBars; ++bar) {
                const double final_src = source_of(Shape::Gapped, bar);
                live.compute(price(bar) + 9.0);
                live.recompute(na<double>());
                live.recompute(price(bar) - 4.0);
                const double got = live.recompute(final_src);
                const double want = committed.compute(final_src);
                CHECK(same(got, want), "ema(%d) %s bar %d recompute %.17g want %.17g", length,
                      seeding_name(average), bar, got, want);
            }
        }
        for (const auto& lengths : kTsiLengths) {
            source::PineTsi live(lengths[0], lengths[1]);
            source::PineTsi committed(lengths[0], lengths[1]);
            for (int bar = 0; bar < kBars; ++bar) {
                const double final_src = source_of(Shape::Gapped, bar);
                live.compute(price(bar) + 9.0);
                live.recompute(na<double>());
                live.recompute(price(bar) - 4.0);
                const double got = live.recompute(final_src);
                const double want = committed.compute(final_src);
                CHECK(same(got, want), "tsi(%d,%d) %s bar %d recompute %.17g want %.17g",
                      lengths[0], lengths[1], seeding_name(average), bar, got, want);
            }
        }
        for (int length : {20, 3, 1}) {
            for (bool use_true_range : {true, false}) {
                source::PineKc live(length, 1.5, use_true_range);
                source::PineKc committed(length, 1.5, use_true_range);
                source::PineKcw live_width(length, 1.5, use_true_range);
                source::PineKcw committed_width(length, 1.5, use_true_range);
                for (int bar = 0; bar < kBars; ++bar) {
                    const Candle c = candle_of(bar, true);
                    live.compute(price(bar) + 9.0, 300.0, 50.0, 180.0);
                    live.recompute(na<double>(), na<double>(), 40.0, na<double>());
                    const ta::KCResult got = live.recompute(c.src, c.high, c.low, c.close);
                    const ta::KCResult want = committed.compute(c.src, c.high, c.low, c.close);
                    CHECK(same_result(got, want), "kc(%d,%s) %s bar %d recompute differs",
                          length, use_true_range ? "tr" : "hl", seeding_name(average), bar);
                    live_width.compute(price(bar) + 9.0, 300.0, 50.0, 180.0);
                    live_width.recompute(na<double>(), na<double>(), 40.0, na<double>());
                    const double got_width =
                        live_width.recompute(c.src, c.high, c.low, c.close);
                    const double want_width =
                        committed_width.compute(c.src, c.high, c.low, c.close);
                    CHECK(same(got_width, want_width),
                          "kcw(%d,%s) %s bar %d recompute %.17g want %.17g", length,
                          use_true_range ? "tr" : "hl", seeding_name(average), bar, got_width,
                          want_width);
                }
            }
        }
        for (const auto& lengths : kMacdLengths) {
            source::PineMacd live(lengths[0], lengths[1], lengths[2]);
            source::PineMacd committed(lengths[0], lengths[1], lengths[2]);
            for (int bar = 0; bar < kBars; ++bar) {
                const double final_src = source_of(Shape::Gapped, bar);
                live.compute(price(bar) + 9.0);
                live.recompute(na<double>());
                live.recompute(price(bar) - 4.0);
                const ta::MACDResult got = live.recompute(final_src);
                const ta::MACDResult want = committed.compute(final_src);
                CHECK(same_result(got, want), "macd(%d,%d,%d) %s bar %d recompute differs",
                      lengths[0], lengths[1], lengths[2], seeding_name(average), bar);
            }
        }
    }
}

static bool equal_out(double a, double b) { return same(a, b); }
static bool equal_out(const ta::KCResult& a, const ta::KCResult& b) { return same_result(a, b); }
static bool equal_out(const ta::MACDResult& a, const ta::MACDResult& b) {
    return same_result(a, b);
}

// The last output of a call to appear.
static bool is_published(double value) { return !is_na(value); }
static bool is_published(const ta::KCResult& value) { return !is_na(value.upper); }
static bool is_published(const ta::MACDResult& value) { return !is_na(value.histogram); }

static const int kCopyBars = 60;

// Before every bar a copy of `original` is taken. The copy and the original
// then compute that bar and must answer the same, so a copy taken before the
// first published bar publishes on the bar its original does. The copy then
// computes a different bar of its own; `untouched`, built like `original`
// and never copied, shows on the next bar that the original did not move.
// `compute(object, bar, bump)` feeds bar `bar` with its inputs raised by
// `bump`. No bar number is expected: the bars before and after the first
// published one are counted from what the original answered.
template <class Object, class Compute>
static void copy_case(const char* name, bool average, Object original, Object untouched,
                      Compute compute) {
    int copies_before_published = 0;
    int copies_after_published = 0;
    bool published = false;
    for (int bar = 0; bar < kCopyBars; ++bar) {
        Object copy = original;
        if (published) ++copies_after_published; else ++copies_before_published;
        const auto from_copy = compute(copy, bar, 0.0);
        const auto from_original = compute(original, bar, 0.0);
        const auto from_untouched = compute(untouched, bar, 0.0);
        CHECK(equal_out(from_copy, from_original), "%s %s bar %d: the copy differs", name,
              seeding_name(average), bar);
        CHECK(equal_out(from_original, from_untouched),
              "%s %s bar %d: the original moved with a copy", name, seeding_name(average), bar);
        published = published || is_published(from_original);
        (void)compute(copy, bar + 1, 50.0);
    }
    CHECK(copies_before_published > 0 && copies_after_published > 0,
          "%s %s copies before / after the first published bar: %d / %d", name,
          seeding_name(average), copies_before_published, copies_after_published);
}

static void test_copy() {
    std::printf("copy\n");
    for (bool average : {false, true}) {
        SeedingCase seeding(average);
        copy_case("ema(14)", average, source::PineEma(14), source::PineEma(14),
                  [](source::PineEma& ema, int bar, double bump) {
                      return ema.compute(price(bar) + bump);
                  });
        copy_case("tsi(2,5)", average, source::PineTsi(2, 5), source::PineTsi(2, 5),
                  [](source::PineTsi& tsi, int bar, double bump) {
                      return tsi.compute(price(bar) + bump);
                  });
        copy_case("kc(3)", average, source::PineKc(3, 1.5), source::PineKc(3, 1.5),
                  [](source::PineKc& kc, int bar, double bump) {
                      const Candle c = candle_of(bar, false);
                      return kc.compute(c.src + bump, c.high + bump, c.low, c.close + bump);
                  });
        copy_case("kcw(3)", average, source::PineKcw(3, 1.5), source::PineKcw(3, 1.5),
                  [](source::PineKcw& kcw, int bar, double bump) {
                      const Candle c = candle_of(bar, false);
                      return kcw.compute(c.src + bump, c.high + bump, c.low, c.close + bump);
                  });
        copy_case("macd(2,5,3)", average, source::PineMacd(2, 5, 3), source::PineMacd(2, 5, 3),
                  [](source::PineMacd& macd, int bar, double bump) {
                      return macd.compute(price(bar) + bump);
                  });
    }
}

int main(int argc, char** argv) {
    // A row name runs that row alone; no argument runs them all.
    const std::string row = argc == 2 ? argv[1] : "";
    const char* rows[] = {"ema", "ema-nested", "tsi", "kc", "kcw", "macd", "recompute", "copy"};
    bool known = row.empty();
    for (const char* name : rows) known = known || row == name;
    if (argc > 2 || !known) {
        std::fprintf(stderr, "unknown ready-publication row\n");
        return 2;
    }
    if (row.empty() || row == "ema") test_ema();
    if (row.empty() || row == "ema-nested") test_ema_nested();
    if (row.empty() || row == "tsi") test_tsi();
    if (row.empty() || row == "kc") test_kc();
    if (row.empty() || row == "kcw") test_kcw();
    if (row.empty() || row == "macd") test_macd();
    if (row.empty() || row == "recompute") test_recompute();
    if (row.empty() || row == "copy") test_copy();
    std::printf("test_pine_ta_ready_publication: %d passed, %d failed\n", tests_passed,
                tests_failed);
    return tests_failed == 0 && tests_passed > 0 ? 0 : 1;
}
