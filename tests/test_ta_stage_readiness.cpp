/*
 * test_ta_stage_readiness.cpp — the availability clocks of the composed EMA
 * classes: ta::TSI::ready, ta::KC::basis_ready / range_ready, ta::KCW::ready.
 *
 * One target: on every bar a clock answers what the stage rule gives for the
 * inputs seen so far, whatever the numerical seeding, and it rewinds with
 * recompute. The stage rule is the kernel's own: a stage is ready once its
 * length of defined inputs has reached it, and a stage fed by another stage
 * counts only that stage's ready values. The expected clocks are counted from
 * the inputs alone (ta_stage_clock_fixture.hpp); no expected value is read
 * back from the class under test.
 *
 * Scope. Every check here holds the kernel to its own stated rule or to an
 * input-dependent invariant. None is a TradingView observation: this file
 * asserts no TradingView number and no TradingView first bar. Whether
 * TradingView's ta.tsi, ta.kc and ta.kcw first publish on the bars this rule
 * gives is not measured in this tree, so no first bar of those functions is
 * written down here as an expectation.
 *
 * Rows:
 *   tsi-clock      ready() against the counted two-stage clock, six length
 *                  pairs, plain / gapped / flat sources, both seedings; and,
 *                  under the average seeding, the published value is na
 *                  exactly while the clock is not ready.
 *   tsi-numbers    control: the published value is still, bit for bit, the
 *                  composition of four ta::EMA the class is defined as (the
 *                  clock bookkeeping moved no number).
 *   tsi-recompute  compute -> recompute sequences and a copy carry the clock.
 *   kc-clock       basis_ready() / range_ready() / KCW::ready() against the
 *                  counted clocks, true range and high - low, na inputs, both
 *                  seedings; and, under the average seeding, the middle band
 *                  and the bands are na exactly while their clocks are not
 *                  ready.
 *   kc-recompute   compute -> recompute sequences and a copy carry the clocks.
 *
 * Source-free, so it also runs in the kernel-only profile.
 *
 * NDEBUG-proof: a counting CHECK and a nonzero exit, never a bare assert().
 */

#include "ta_stage_clock_fixture.hpp"

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
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

static const int kBars = 160;

// --------------------------------------------------------------------------
// TSI
// --------------------------------------------------------------------------

// The class as it is defined: the short EMA of the long EMA of the change,
// over the same of its absolute value, from four independent ta::EMA.
struct TsiComposition {
    ta::EMA long_stage;
    ta::EMA short_stage;
    ta::EMA abs_long_stage;
    ta::EMA abs_short_stage;
    double previous = na<double>();

    TsiComposition(int short_len, int long_len)
        : long_stage(long_len), short_stage(short_len),
          abs_long_stage(long_len), abs_short_stage(short_len) {}

    double compute(double src) {
        if (is_na(previous)) {
            previous = src;
            return na<double>();
        }
        const double change = src - previous;
        previous = src;
        const double ds = short_stage.compute(long_stage.compute(change));
        const double ads = abs_short_stage.compute(abs_long_stage.compute(std::abs(change)));
        if (ads == 0) return na<double>();
        return ds / ads;
    }
};

// {short_length, long_length}
static const int kTsiLengths[][2] = {
    {13, 25}, {1, 1}, {2, 5}, {5, 2}, {1, 4}, {4, 1},
};

static void test_tsi_clock() {
    std::printf("tsi-clock\n");
    for (bool average : {false, true}) {
        for (const auto& lengths : kTsiLengths) {
            // The first ready bar of each shape, as the class answered it.
            int first_ready_of[4] = {-1, -1, -1, -1};
            int shape_index = 0;
            for (Shape shape : {Shape::Plain, Shape::Gapped, Shape::Flat, Shape::FlatGapped}) {
                SeedingCase seeding(average);
                ta::TSI tsi(lengths[0], lengths[1]);
                TsiClock clock(lengths[0], lengths[1]);
                int first_ready = -1;
                bool was_ready = false;
                for (int bar = 0; bar < kBars; ++bar) {
                    const double src = source_of(shape, bar);
                    const double value = tsi.compute(src);
                    const bool expected = clock.step(src);
                    if (tsi.ready() && first_ready < 0) first_ready = bar;
                    CHECK(tsi.ready() == expected,
                          "tsi(%d,%d) %s %s bar %d ready %d want %d", lengths[0], lengths[1],
                          shape_name(shape), seeding_name(average), bar, (int)tsi.ready(),
                          (int)expected);
                    // A clock never goes back in an ordinary run.
                    CHECK(!was_ready || tsi.ready(),
                          "tsi(%d,%d) %s %s bar %d the clock went back", lengths[0], lengths[1],
                          shape_name(shape), seeding_name(average), bar);
                    was_ready = tsi.ready();
                    // Under the average seeding a defined change publishes a
                    // value exactly when the clock is ready. (A flat source
                    // has a zero denominator and answers na throughout.)
                    if (average && !is_flat(shape) && clock.change_defined) {
                        CHECK(is_na(value) == !expected,
                              "tsi(%d,%d) %s average bar %d value %.17g ready %d",
                              lengths[0], lengths[1], shape_name(shape), bar, value,
                              (int)expected);
                    }
                    if (is_flat(shape)) {
                        CHECK(is_na(value), "tsi(%d,%d) %s %s bar %d flat value %.17g",
                              lengths[0], lengths[1], shape_name(shape),
                              seeding_name(average), bar, value);
                    }
                }
                // The comparison above is not "never ready" against "never
                // ready": every case becomes ready inside the run, and not on
                // the first bar, which has no change.
                CHECK(first_ready > 0, "tsi(%d,%d) %s %s first ready bar %d", lengths[0],
                      lengths[1], shape_name(shape), seeding_name(average), first_ready);
                first_ready_of[shape_index++] = first_ready;
            }
            // The clock reads the inputs' definedness, not their values: a
            // flat source is ready where the moving one is, and removing
            // sources can only delay it.
            CHECK(first_ready_of[2] == first_ready_of[0] && first_ready_of[3] == first_ready_of[1],
                  "tsi(%d,%d) %s flat first ready (%d %d) moving (%d %d)", lengths[0],
                  lengths[1], seeding_name(average), first_ready_of[2], first_ready_of[3],
                  first_ready_of[0], first_ready_of[1]);
            CHECK(first_ready_of[1] > first_ready_of[0],
                  "tsi(%d,%d) %s gaps did not delay the clock: %d against %d", lengths[0],
                  lengths[1], seeding_name(average), first_ready_of[1], first_ready_of[0]);
        }
    }
}

static void test_tsi_numbers() {
    std::printf("tsi-numbers\n");
    for (bool average : {false, true}) {
        for (const auto& lengths : kTsiLengths) {
            for (Shape shape : {Shape::Plain, Shape::Gapped, Shape::Flat, Shape::FlatGapped}) {
                SeedingCase seeding(average);
                ta::TSI tsi(lengths[0], lengths[1]);
                TsiComposition reference(lengths[0], lengths[1]);
                int values = 0;
                for (int bar = 0; bar < kBars; ++bar) {
                    const double src = source_of(shape, bar);
                    const double got = tsi.compute(src);
                    const double want = reference.compute(src);
                    if (!is_na(want)) ++values;
                    CHECK(same(got, want), "tsi(%d,%d) %s %s bar %d value %.17g want %.17g",
                          lengths[0], lengths[1], shape_name(shape), seeding_name(average),
                          bar, got, want);
                }
                // The comparison is not na against na throughout: a moving
                // source publishes numbers, a flat one never does.
                CHECK((values > 0) == !is_flat(shape), "tsi(%d,%d) %s %s published %d values",
                      lengths[0], lengths[1], shape_name(shape), seeding_name(average),
                      values);
            }
        }
    }
}

static void test_tsi_recompute() {
    std::printf("tsi-recompute\n");
    for (bool average : {false, true}) {
        for (const auto& lengths : kTsiLengths) {
            SeedingCase seeding(average);
            ta::TSI live(lengths[0], lengths[1]);
            ta::TSI committed(lengths[0], lengths[1]);
            TsiClock clock(lengths[0], lengths[1]);
            int copies_before_ready = 0;
            int copies_when_ready = 0;
            for (int bar = 0; bar < kBars; ++bar) {
                const double final_src = source_of(Shape::Gapped, bar);
                // Candidates of one bar: a number, na, another number, then
                // the bar's final source. Only the last one may count.
                live.compute(price(bar) + 9.0);
                live.recompute(na<double>());
                live.recompute(price(bar) - 4.0);
                const double got = live.recompute(final_src);
                const double want = committed.compute(final_src);
                const bool expected = clock.step(final_src);
                CHECK(same(got, want), "tsi(%d,%d) %s bar %d recompute value %.17g want %.17g",
                      lengths[0], lengths[1], seeding_name(average), bar, got, want);
                CHECK(live.ready() == expected && committed.ready() == expected,
                      "tsi(%d,%d) %s bar %d recompute ready %d committed %d want %d",
                      lengths[0], lengths[1], seeding_name(average), bar, (int)live.ready(),
                      (int)committed.ready(), (int)expected);
                // A copy taken on any bar carries the clock and the numbers.
                // Every bar of the first half is copied, so copies are taken
                // on both sides of whatever bar the clock becomes ready on.
                if (bar < kBars / 2) {
                    if (live.ready()) ++copies_when_ready; else ++copies_before_ready;
                    ta::TSI copy = live;
                    ta::TSI original = live;
                    for (int ahead = 1; ahead <= 3; ++ahead) {
                        const double next = price(bar + ahead);
                        const double a = copy.compute(next);
                        const double b = original.compute(next);
                        CHECK(same(a, b) && copy.ready() == original.ready(),
                              "tsi(%d,%d) copy at bar %d diverged %d bars later", lengths[0],
                              lengths[1], bar, ahead);
                    }
                }
            }
            CHECK(copies_before_ready > 0 && copies_when_ready > 0,
                  "tsi(%d,%d) %s copies before / after ready: %d / %d", lengths[0], lengths[1],
                  seeding_name(average), copies_before_ready, copies_when_ready);
        }
    }
}

// --------------------------------------------------------------------------
// KC / KCW
// --------------------------------------------------------------------------

static const int kKcLengths[] = {1, 2, 3, 20};

static void test_kc_clock() {
    std::printf("kc-clock\n");
    for (bool average : {false, true}) {
        for (int length : kKcLengths) {
            for (bool use_true_range : {true, false}) {
                int first_basis_of[2] = {-1, -1};
                int first_range_of[2] = {-1, -1};
                for (bool dirty : {false, true}) {
                    SeedingCase seeding(average);
                    const char* range_name = use_true_range ? "tr" : "hl";
                    const char* input_name = dirty ? "dirty" : "clean";
                    ta::KC kc(length, 1.5, use_true_range);
                    ta::KCW kcw(length, 1.5, use_true_range);
                    KcClock clock(length, use_true_range);
                    int first_basis = -1;
                    int first_range = -1;
                    for (int bar = 0; bar < kBars; ++bar) {
                        const Candle c = candle_of(bar, dirty);
                        const ta::KCResult r = kc.compute(c.src, c.high, c.low, c.close);
                        kcw.compute(c.src, c.high, c.low, c.close);
                        clock.step(c);
                        if (kc.basis_ready() && first_basis < 0) first_basis = bar;
                        if (kc.range_ready() && first_range < 0) first_range = bar;
                        CHECK(kc.basis_ready() == clock.basis_ready()
                                  && kc.range_ready() == clock.range_ready(),
                              "kc(%d,%s) %s %s bar %d clocks (%d %d) want (%d %d)", length,
                              range_name, input_name, seeding_name(average), bar,
                              (int)kc.basis_ready(), (int)kc.range_ready(),
                              (int)clock.basis_ready(), (int)clock.range_ready());
                        CHECK(kcw.ready() == clock.ready(),
                              "kcw(%d,%s) %s %s bar %d ready %d want %d", length, range_name,
                              input_name, seeding_name(average), bar, (int)kcw.ready(),
                              (int)clock.ready());
                        // Under the average seeding a defined input publishes
                        // exactly when its clock is ready.
                        if (average && clock.source_defined) {
                            CHECK(is_na(r.middle) == !clock.basis_ready(),
                                  "kc(%d,%s) %s average bar %d middle %.17g ready %d", length,
                                  range_name, input_name, bar, r.middle,
                                  (int)clock.basis_ready());
                            if (clock.range_defined) {
                                CHECK(is_na(r.upper) == !clock.ready()
                                          && is_na(r.lower) == !clock.ready(),
                                      "kc(%d,%s) %s average bar %d bands (%.17g %.17g) ready %d",
                                      length, range_name, input_name, bar, r.upper, r.lower,
                                      (int)clock.ready());
                            }
                        }
                    }
                    // Both stages become ready inside the run, so the
                    // comparison above is not "never" against "never".
                    CHECK(first_basis >= 0 && first_range >= 0,
                          "kc(%d,%s) %s %s first ready bars (%d %d)", length, range_name,
                          input_name, seeding_name(average), first_basis, first_range);
                    first_basis_of[dirty ? 1 : 0] = first_basis;
                    first_range_of[dirty ? 1 : 0] = first_range;
                }
                // Removing inputs can only delay a stage, and the na sources
                // of the dirty case do delay the basis.
                CHECK(first_basis_of[1] > first_basis_of[0],
                      "kc(%d) na sources did not delay the basis: %d against %d", length,
                      first_basis_of[1], first_basis_of[0]);
                CHECK(first_range_of[1] >= first_range_of[0],
                      "kc(%d) na inputs made the range stage earlier: %d against %d", length,
                      first_range_of[1], first_range_of[0]);
            }
        }
    }
}

static void test_kc_recompute() {
    std::printf("kc-recompute\n");
    for (bool average : {false, true}) {
        for (int length : kKcLengths) {
            for (bool use_true_range : {true, false}) {
                SeedingCase seeding(average);
                const char* range_name = use_true_range ? "tr" : "hl";
                ta::KC live(length, 1.5, use_true_range);
                ta::KCW live_width(length, 1.5, use_true_range);
                KcClock clock(length, use_true_range);
                int copies_before_ready = 0;
                int copies_when_ready = 0;
                for (int bar = 0; bar < kBars; ++bar) {
                    const Candle c = candle_of(bar, true);
                    // Candidates of one bar; only the final one may count.
                    live.compute(price(bar) + 9.0, 300.0, 50.0, 180.0);
                    live.recompute(na<double>(), na<double>(), 40.0, na<double>());
                    live.recompute(c.src, c.high, c.low, c.close);
                    live_width.compute(price(bar) + 9.0, 300.0, 50.0, 180.0);
                    live_width.recompute(na<double>(), na<double>(), 40.0, na<double>());
                    live_width.recompute(c.src, c.high, c.low, c.close);
                    clock.step(c);
                    CHECK(live.basis_ready() == clock.basis_ready()
                              && live.range_ready() == clock.range_ready(),
                          "kc(%d,%s) %s bar %d recompute clocks (%d %d) want (%d %d)", length,
                          range_name, seeding_name(average), bar, (int)live.basis_ready(),
                          (int)live.range_ready(), (int)clock.basis_ready(),
                          (int)clock.range_ready());
                    CHECK(live_width.ready() == clock.ready(),
                          "kcw(%d,%s) %s bar %d recompute ready %d want %d", length, range_name,
                          seeding_name(average), bar, (int)live_width.ready(),
                          (int)clock.ready());
                    // Every bar of the first half is copied, so copies are
                    // taken on both sides of whatever bar the stages become
                    // ready on.
                    if (bar < kBars / 2) {
                        const bool ready = live.basis_ready() && live.range_ready();
                        if (ready) ++copies_when_ready; else ++copies_before_ready;
                        ta::KC copy = live;
                        ta::KC original = live;
                        for (int ahead = 1; ahead <= 3; ++ahead) {
                            const Candle n = candle_of(bar + ahead, false);
                            const ta::KCResult a = copy.compute(n.src, n.high, n.low, n.close);
                            const ta::KCResult b =
                                original.compute(n.src, n.high, n.low, n.close);
                            CHECK(same(a.middle, b.middle) && same(a.upper, b.upper)
                                      && same(a.lower, b.lower)
                                      && copy.basis_ready() == original.basis_ready()
                                      && copy.range_ready() == original.range_ready(),
                                  "kc(%d) copy at bar %d diverged %d bars later", length, bar,
                                  ahead);
                        }
                    }
                }
                CHECK(copies_before_ready > 0 && copies_when_ready > 0,
                      "kc(%d,%s) %s copies before / after ready: %d / %d", length, range_name,
                      seeding_name(average), copies_before_ready, copies_when_ready);
            }
        }
    }
}

int main(int argc, char** argv) {
    // A row name runs that row alone; no argument runs them all.
    const std::string row = argc == 2 ? argv[1] : "";
    const char* rows[] = {"tsi-clock", "tsi-numbers", "tsi-recompute", "kc-clock",
                          "kc-recompute"};
    bool known = row.empty();
    for (const char* name : rows) known = known || row == name;
    if (argc > 2 || !known) {
        std::fprintf(stderr, "unknown stage-readiness row\n");
        return 2;
    }
    if (row.empty() || row == "tsi-clock") test_tsi_clock();
    if (row.empty() || row == "tsi-numbers") test_tsi_numbers();
    if (row.empty() || row == "tsi-recompute") test_tsi_recompute();
    if (row.empty() || row == "kc-clock") test_kc_clock();
    if (row.empty() || row == "kc-recompute") test_kc_recompute();
    std::printf("test_ta_stage_readiness: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 && tests_passed > 0 ? 0 : 1;
}
