// R5 follow-up lane E16: a trail stop that stands a whole number of price
// ticks from a best on the ladder IS the ladder point it names.
//
// The kernel's trailing stop is `best -/+ offset`, and when the offset is
// spelled in ticks (native_order::TrailTicks) acceptance resolves it to
// `ticks * NativeRunSpec::price_tick` (WorkingRequestCore::resolve_tick_spellings).
// Both steps are exact arithmetic on inexact numbers, and the subtraction can
// land one binary64 ULP off the ladder point the count names:
//
//     11.44 - 5 * 0.01 = 11.389999999999998792   (0x1.6c7ae147ae147p+3)
//     the ladder point  = 11.390000000000000568  (0x1.6c7ae147ae148p+3)
//
// A print that IS 11.39 — the spelling a decimal feed parses to — then does
// not reach a stop the run declared five ticks under 11.44, and the leg rides
// on. The mirror is a buy trail, whose stop lands one ULP ABOVE the point:
// 11.39 + 5 * 0.01 = 11.440000000000001 against 11.44.
//
// This is not a quantization of the stop (design native-feature-parity.md
// row B2: the trail stop and the running best stay on the raw path) and not a
// tolerance in the comparison. It is the LEVEL: a stop `ticks` ticks from a
// best that is itself a ladder point is the ladder point that many ticks
// away, derived by index arithmetic the way every other ladder decision in
// this kernel is (native_matching::grid_exact_index, R7's exactness ruling).
// A sub-tick best, a fractional-tick offset and a run that declares no price
// tick keep the raw subtraction they have always had.
//
// Measured by TradingView's own tape first: R5 lane E14
// (tests/fixtures/trail_activation_tick_reach/e14-f-long-shallow-next, the
// seventh trade) is exactly the 11.44 / 5-tick case — TradingView books the
// exit at 11.39 on the bar whose low is 11.39, the engine missed it by that
// ULP and ran to the timeout. E14 pinned the row as a recorded divergence;
// this unit is the kernel-only reproduction, batch and stream.
//
// Fail-before, on this lane's base e9ad37dd (the whole unit compiles — the
// defect is in the arithmetic, not the surface):
//   sell trail level: actual=11.389999999999998792 expected=11.390000000000001
//   sell trail (batch): rows=0 exit=0 position=1
//   buy  trail level: actual=11.440000000000001 expected=11.4399999999999995
//   buy  trail (batch): rows=0 exit=0 position=-1
#include "native_current_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

using namespace r4_test;

namespace {

Bar bar_at(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, T + static_cast<std::int64_t>(index) * 60000};
}

// The two ladder points this unit is about, in the spelling a decimal feed
// parses to. Both are also k * tick exactly, so there is one binary64 value
// per point here and the level assertions below are bit comparisons.
constexpr double kBest = 11.44;
constexpr double kStop = 11.39;
constexpr double kTicks = 5.0;

struct Case {
    std::vector<Bar> bars;
    double entry_units = 1.0;            // signed: the lot the trail closes
    std::optional<double> best_seed;
    std::optional<double> offset_ticks;  // the TrailTicks spelling
    double offset_price = 0.0;           // used when offset_ticks is absent
    double price_tick = 0.01;
    int submit_calculation = 2;          // 1-based calculation the trail is sent on
};

struct Outcome {
    std::size_t rows = 0;
    double exit_price = 0.0;
    double position = 0.0;
    // The kernel's own readout of the live stop (NativeStrategyHost::trail_state),
    // taken at every calculation after the leg armed: the LEVEL under test.
    double level = std::numeric_limits<double>::quiet_NaN();
    double best = std::numeric_limits<double>::quiet_NaN();
};

struct Driver {
    const Case* c = nullptr;
    Outcome* out = nullptr;
    std::optional<no::RequestHandle> leg;
};

void drive(Host& host, Driver& driver) {
    host.calculation = [&driver](Host& h) {
        const Case& c = *driver.c;
        if (h.calculations == 1) put(h, tx(c.entry_units, "open"));
        if (h.calculations == c.submit_calculation) {
            no::Request leg{no::Reduce{no::ExplicitUnits{std::abs(c.entry_units)}},
                            "trail", ""};
            no::Trail trail;
            if (c.offset_ticks) {
                trail.ticks = no::TrailTicks{*c.offset_ticks};
            } else {
                trail.offset = c.offset_price;
            }
            trail.best_seed = c.best_seed;
            leg.trigger = trail;
            driver.leg = put(h, leg);
            return;
        }
        if (!driver.leg) return;
        if (const auto state = h.trail_state(*driver.leg)) {
            if (state->activated) {
                driver.out->level = state->current_level;
                driver.out->best = state->best_price;
            }
        }
    };
}

NativeRunSpec case_spec(const char* key, const Case& c) {
    NativeRunSpec s = spec(key);
    s.price_tick = c.price_tick;
    return s;
}

Outcome run_batch(const char* key, const Case& c) {
    Host host;
    Outcome out;
    Driver driver{&c, &out, std::nullopt};
    drive(host, driver);
    REQUIRE(host.configure_native(case_spec(key, c)).status == NativeSetupStatus::Applied);
    host.run(c.bars.data(), static_cast<int>(c.bars.size()));
    completed(host);
    out.position = host.physical_position().signed_units;
    out.rows = host.rows().size();
    if (!host.rows().empty()) out.exit_price = host.rows().back().exit_price;
    return out;
}

// The same case through stream_begin + stream_push_bar + stream_end: one
// warmup bar, then every later bar pushed live.
Outcome run_stream(const char* key, const Case& c) {
    Host host;
    Outcome out;
    Driver driver{&c, &out, std::nullopt};
    drive(host, driver);
    REQUIRE(host.configure_native(case_spec(key, c)).status == NativeSetupStatus::Applied);
    REQUIRE(host.stream_begin(c.bars.data(), 1, "1", "1"));
    for (std::size_t i = 1; i < c.bars.size(); ++i) CHECK(host.stream_push_bar(c.bars[i]));
    CHECK(host.stream_end(false));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    out.position = host.physical_position().signed_units;
    out.rows = host.rows().size();
    if (!host.rows().empty()) out.exit_price = host.rows().back().exit_price;
    return out;
}

void report(const char* label, const Outcome& out) {
    std::printf("  %-34s rows=%zu exit=%.17g position=%.17g best=%.17g level=%.17g\n",
                label, out.rows, out.exit_price, out.position, out.best, out.level);
}

// A bit comparison: these are levels, and one ULP is the whole question.
void exactly(double actual, double expected) {
    if (actual != expected)
        std::printf("   actual=%.17g expected=%.17g\n", actual, expected);
    CHECK(actual == expected);
}

// ── the sell trail: the stop lands one ULP UNDER its ladder point ────────
//
// Bar 0 opens the long at its close. The leg is sent on bar 1's calculation
// already armed and rides from bar 2, whose prints all stay under the seed,
// so the best is the seed 11.44 and the stop is five ticks under it. The
// print bar's low is exactly 11.39.
Case sell_case(bool reaches) {
    Case c;
    c.bars = {
        bar_at(0, 11.40, 11.40, 11.40, 11.40),
        bar_at(1, 11.40, 11.42, 11.40, 11.41),
        reaches ? bar_at(2, 11.42, 11.43, kStop, 11.40)
                : bar_at(2, 11.42, 11.43, 11.41, 11.42),
        bar_at(3, 11.40, 11.40, 11.40, 11.40),
    };
    c.best_seed = kBest;
    c.offset_ticks = kTicks;
    return c;
}

// ── the buy trail: the mirror, one ULP OVER ──────────────────────────────
//
// A short lot, the seed the low the position already reached, and a print bar
// whose high is exactly 11.44 = 11.39 + five ticks.
Case buy_case(bool reaches) {
    Case c;
    c.bars = {
        bar_at(0, 11.40, 11.40, 11.40, 11.40),
        bar_at(1, 11.40, 11.40, 11.39, 11.39),
        reaches ? bar_at(2, 11.40, kBest, 11.39, 11.42)
                : bar_at(2, 11.40, 11.43, 11.39, 11.42),
        bar_at(3, 11.42, 11.42, 11.42, 11.42),
    };
    c.entry_units = -1.0;
    c.best_seed = kStop;
    c.offset_ticks = kTicks;
    return c;
}

void a_sell_trails_stop_is_the_ladder_point() {
    const Case level_only = sell_case(false);
    const auto resting = run_batch("e16-sell", level_only);
    report("sell trail, never reached", resting);
    exactly(resting.best, kBest);
    exactly(resting.level, kStop);
    CHECK(resting.rows == 0);
    CHECK(resting.position == 1.0);

    // The raw subtraction the level used to be, and the ULP that separates
    // them: this is the whole defect, stated in the numbers.
    const double raw = kBest - kTicks * 0.01;
    std::printf("  raw best - ticks * tick = %.17g, ladder point = %.17g, ULP apart = %d\n",
                raw, kStop, static_cast<int>(std::nextafter(raw, kStop) == kStop));
    CHECK(raw < kStop);
    CHECK(std::nextafter(raw, kStop) == kStop);

    const Case reaching = sell_case(true);
    const auto batch = run_batch("e16-sell", reaching);
    report("sell trail (batch)", batch);
    CHECK(batch.rows == 1);
    CHECK(batch.position == 0.0);
    if (batch.rows == 1) exactly(batch.exit_price, kStop);

    const auto stream = run_stream("e16-sell", reaching);
    report("sell trail (stream)", stream);
    CHECK(stream.rows == batch.rows);
    CHECK(stream.position == batch.position);
    exactly(stream.exit_price, batch.exit_price);
}

void b_buy_trails_stop_is_the_ladder_point() {
    const Case level_only = buy_case(false);
    const auto resting = run_batch("e16-buy", level_only);
    report("buy trail, never reached", resting);
    exactly(resting.best, kStop);
    exactly(resting.level, kBest);
    CHECK(resting.rows == 0);
    CHECK(resting.position == -1.0);

    const double raw = kStop + kTicks * 0.01;
    std::printf("  raw best + ticks * tick = %.17g, ladder point = %.17g, ULP apart = %d\n",
                raw, kBest, static_cast<int>(std::nextafter(raw, kBest) == kBest));
    CHECK(raw > kBest);
    CHECK(std::nextafter(raw, kBest) == kBest);

    const Case reaching = buy_case(true);
    const auto batch = run_batch("e16-buy", reaching);
    report("buy trail (batch)", batch);
    CHECK(batch.rows == 1);
    CHECK(batch.position == 0.0);
    if (batch.rows == 1) exactly(batch.exit_price, kBest);

    const auto stream = run_stream("e16-buy", reaching);
    report("buy trail (stream)", stream);
    CHECK(stream.rows == batch.rows);
    CHECK(stream.position == batch.position);
    exactly(stream.exit_price, batch.exit_price);
}

// ── the price-spelled twin ──────────────────────────────────────────────
// design native-feature-parity.md row TR2: "a tick-spelled trail and its
// price-spelled equal behave identically bar for bar". A host that writes the
// distance 0.05 on a 0.01 ladder has named five ticks just as much as
// TrailTicks{5} has, and acceptance erases the difference anyway.
void c_the_price_spelled_twin_is_the_same_level() {
    Case priced = sell_case(true);
    priced.offset_ticks.reset();
    priced.offset_price = kTicks * 0.01;
    const auto out = run_batch("e16-sell", priced);
    report("sell trail, price-spelled 0.05", out);
    CHECK(out.rows == 1);
    if (out.rows == 1) exactly(out.exit_price, kStop);

    Case priced_resting = sell_case(false);
    priced_resting.offset_ticks.reset();
    priced_resting.offset_price = kTicks * 0.01;
    const auto resting = run_batch("e16-sell", priced_resting);
    report("price-spelled, never reached", resting);
    exactly(resting.level, kStop);
}

// ── the best the ride found itself ──────────────────────────────────────
// The rule is about the best, not about where it came from: with no seed at
// all the trail's own first print is 11.44 and the stop is the same point.
void d_an_observed_best_is_a_ladder_point_too() {
    Case observed = sell_case(false);
    observed.best_seed.reset();
    observed.bars[2] = bar_at(2, kBest, kBest, 11.41, 11.42);
    const auto out = run_batch("e16-sell", observed);
    report("observed best 11.44, no seed", out);
    exactly(out.best, kBest);
    exactly(out.level, kStop);
}

// ── off the ladder nothing moves ────────────────────────────────────────
// Three shapes keep the raw subtraction, bit for bit: a best inside a tick
// cell, an offset that is not a whole number of ticks, and a run that
// declares no price tick at all. The first two are the sub-tick trails lane
// E5's tapes are about; the third is the kernel with no ladder to name.
void e_off_the_ladder_the_stop_stays_raw() {
    Case sub_tick = sell_case(false);
    sub_tick.best_seed = 11.445;
    const auto inside_a_cell = run_batch("e16-sub-tick", sub_tick);
    report("sub-tick best 11.445", inside_a_cell);
    exactly(inside_a_cell.best, 11.445);
    exactly(inside_a_cell.level, 11.445 - kTicks * 0.01);

    Case half_tick = sell_case(false);
    half_tick.offset_ticks = 0.5;
    // Half a tick under the seed is 11.435, which bar 2's own low would
    // reach; hold the ride flat at the best instead so the level is read
    // while the leg is still live.
    half_tick.bars[2] = bar_at(2, kBest, kBest, kBest, kBest);
    const auto fractional = run_batch("e16-half-tick", half_tick);
    report("half-tick offset", fractional);
    exactly(fractional.best, kBest);
    exactly(fractional.level, kBest - 0.5 * 0.01);

    Case no_ladder = sell_case(false);
    no_ladder.price_tick = 0.0;
    no_ladder.offset_ticks.reset();   // a tick spelling needs a positive tick
    no_ladder.offset_price = kTicks * 0.01;
    const auto unticked = run_batch("e16-no-tick", no_ladder);
    report("no price tick declared", unticked);
    exactly(unticked.best, kBest);
    exactly(unticked.level, kBest - kTicks * 0.01);
    CHECK(unticked.level != kStop);
}

}  // namespace

int main() {
    test("a sell trail's stop is the ladder point", a_sell_trails_stop_is_the_ladder_point);
    test("a buy trail's stop is the ladder point", b_buy_trails_stop_is_the_ladder_point);
    test("the price-spelled twin is the same level", c_the_price_spelled_twin_is_the_same_level);
    test("an observed best is a ladder point too", d_an_observed_best_is_a_ladder_point_too);
    test("off the ladder the stop stays raw", e_off_the_ladder_the_stop_stays_raw);
    std::printf("\n%s native trail stop ladder: %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
