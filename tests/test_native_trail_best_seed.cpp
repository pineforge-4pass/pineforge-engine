// R5 follow-up lane E14: native_order::Trail::best_seed — where a trail's
// running best STARTS.
//
// A trailing stop rides a running best. Until this field the kernel had no
// way to say where that ride begins: the best was the arm's own print, which
// is the arm threshold's ladder point when a crossing armed the trail, and
// the first print the trail sees when it was submitted already armed. Both
// are accidents of WHEN the arm happened, not a decision the run made — and
// a host that already knows the level its position reached before the request
// existed had no way to say so. `best_seed` is that level: a floor on the
// start, the favourable one of the seed and the arm print.
//
// Two scenarios, each a sell trail on a long lot with a 0.50 price offset:
//
//   A  already armed at submission (no arm_price): the best would start at
//      the next bar's open, 100.60. Seeded at 101.00 the stop rides at
//      100.50, and the later shallow bar whose low is 100.45 books it. Without
//      the seed the best is the bar-2 high 100.70, the stop 100.20, and that
//      bar never reaches it.
//   B  armed on a crossing (arm_price 100.995, half a tick under a 101.00
//      activation on a 0.01 ladder): the best would start at the threshold
//      itself and then ride the bar's raw high 100.999. Seeded at 101.00 the
//      stop is 100.50 instead of 100.499, and the next bar's low of exactly
//      100.50 books it.
//
// Each is pinned three ways: the control (no seed) keeps its behaviour AND
// the continuation digest the kernel produced before the field existed, the
// seeded run books the exit, and the same pair runs through the streaming
// ingress. A buy trail mirrors the min side, a price-grid row pins that the
// seed is put on the ladder like an observed print, and the acceptance rows
// pin the level's own validity.
//
// Fail-before: compiled against the previous header closure (this lane's base
// 9f0ea24c) this unit stops at
//   error: no member named 'best_seed' in 'pineforge::native_order::Trail'
// and the same bar sequences driven through the unchanged kernel book no exit
// at all (exec/E14-probes/fail_before.cpp: "A control rows=0 ... position=1",
// "B control rows=0 ... position=1").
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

struct Outcome {
    std::size_t rows = 0;
    double exit_price = 0.0;
    double position = 0.0;
    std::uint64_t continuation = 0;
};

struct Case {
    std::vector<Bar> bars;
    double entry_units = 1.0;          // signed: the lot the trail closes
    std::optional<double> arm_price;
    std::optional<double> best_seed;
    int submit_calculation = 1;        // 1-based calculation the trail is sent on
    double offset = 0.5;
    NativePriceGrid grid = NativePriceGrid::None;
};

void drive(Host& host, const Case& c) {
    host.calculation = [&c](Host& h) {
        if (h.calculations == 1) put(h, tx(c.entry_units, "open"));
        if (h.calculations == c.submit_calculation) {
            no::Request leg{no::Reduce{no::ExplicitUnits{std::abs(c.entry_units)}},
                            "trail", ""};
            no::Trail trail;
            trail.offset = c.offset;
            trail.arm_price = c.arm_price;
            trail.best_seed = c.best_seed;
            leg.trigger = trail;
            put(h, leg);
        }
    };
}

NativeRunSpec case_spec(const char* key, const Case& c) {
    NativeRunSpec s = spec(key);
    s.price_grid = c.grid;
    return s;
}

Outcome run_batch(const char* key, const Case& c) {
    Host host;
    drive(host, c);
    REQUIRE(host.configure_native(case_spec(key, c)).status == NativeSetupStatus::Applied);
    host.run(c.bars.data(), static_cast<int>(c.bars.size()));
    completed(host);
    Outcome out;
    out.position = host.physical_position().signed_units;
    out.continuation = host.native_continuation_hash();
    out.rows = host.rows().size();
    if (!host.rows().empty()) out.exit_price = host.rows().back().exit_price;
    return out;
}

// The same case through stream_begin + stream_push_bar + stream_end: one
// warmup bar, then every later bar pushed live.
Outcome run_stream(const char* key, const Case& c) {
    Host host;
    drive(host, c);
    REQUIRE(host.configure_native(case_spec(key, c)).status == NativeSetupStatus::Applied);
    REQUIRE(host.stream_begin(c.bars.data(), 1, "1", "1"));
    for (std::size_t i = 1; i < c.bars.size(); ++i) CHECK(host.stream_push_bar(c.bars[i]));
    CHECK(host.stream_end(false));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    Outcome out;
    out.position = host.physical_position().signed_units;
    out.continuation = host.native_continuation_hash();
    out.rows = host.rows().size();
    if (!host.rows().empty()) out.exit_price = host.rows().back().exit_price;
    return out;
}

void report(const char* label, const Outcome& out) {
    std::printf("  %-26s rows=%zu exit=%.17g position=%.17g continuation=%llu\n",
                label, out.rows, out.exit_price, out.position,
                static_cast<unsigned long long>(out.continuation));
}

// ── A. already armed at submission ──────────────────────────────────────
// Bar 0 opens the lot at its close. Bar 1 is where the host's own level,
// 101.00, is reached; the trail is submitted on bar 2's calculation already
// armed, so the kernel's own start is bar 3's first print.
Case case_a() {
    Case c;
    c.bars = {
        bar_at(0, 100.00, 100.00, 100.00, 100.00),
        bar_at(1, 100.20, 101.00, 100.10, 100.60),
        bar_at(2, 100.60, 100.70, 100.55, 100.60),
        bar_at(3, 100.60, 100.60, 100.45, 100.50),
        bar_at(4, 100.50, 100.50, 100.50, 100.50),
    };
    c.submit_calculation = 2;
    return c;
}

// ── B. armed on a crossing ──────────────────────────────────────────────
// The arm threshold is half a tick under the host's activation. Bar 1's raw
// high stops one thousandth short of the activation, so the kernel's own best
// is 100.999 and its stop 100.499; bar 2's low is exactly 100.50.
Case case_b() {
    Case c;
    c.bars = {
        bar_at(0, 100.00, 100.00, 100.00, 100.00),
        bar_at(1, 100.90, 100.999, 100.90, 100.95),
        bar_at(2, 100.90, 100.90, 100.50, 100.60),
        bar_at(3, 100.60, 100.60, 100.60, 100.60),
    };
    c.arm_price = 100.995;
    return c;
}

// The digests the kernel produced for both controls BEFORE the field existed
// (exec/E14-probes/fail_before.cpp on this lane's base 9f0ea24c). An absent
// seed folds nothing, so these must not move.
constexpr std::uint64_t kControlContinuationA = 7148617029230866814ULL;
constexpr std::uint64_t kControlContinuationB = 1059492877796320724ULL;

void a_already_armed_at_submission() {
    Case control = case_a();
    const auto without = run_batch("e14-a-control", control);
    report("A control", without);
    CHECK(without.rows == 0);
    CHECK(without.position == 1.0);
    CHECK(without.continuation == kControlContinuationA);

    Case seeded = case_a();
    seeded.best_seed = 101.00;
    const auto with = run_batch("e14-a-control", seeded);
    report("A seeded 101.00", with);
    CHECK(with.rows == 1);
    CHECK(with.position == 0.0);
    if (with.rows == 1) near(with.exit_price, 100.50);
    // A seed IS request state: the same run under the same session key must
    // not answer the digest the unseeded one does.
    CHECK(with.continuation != kControlContinuationA);
}

void b_armed_on_a_crossing() {
    Case control = case_b();
    const auto without = run_batch("e14-b-control", control);
    report("B control", without);
    CHECK(without.rows == 0);
    CHECK(without.position == 1.0);
    CHECK(without.continuation == kControlContinuationB);

    Case seeded = case_b();
    seeded.best_seed = 101.00;
    const auto with = run_batch("e14-b-control", seeded);
    report("B seeded 101.00", with);
    CHECK(with.rows == 1);
    CHECK(with.position == 0.0);
    if (with.rows == 1) near(with.exit_price, 100.50);
    CHECK(with.continuation != kControlContinuationB);
}

// Streaming is the same kernel under a different drive, so both halves must
// answer exactly what the batch answered — trades and continuation alike.
void streamed_pair() {
    for (int which = 0; which < 2; ++which) {
        const char* key = which == 0 ? "e14-a-control" : "e14-b-control";
        Case control = which == 0 ? case_a() : case_b();
        Case seeded = control;
        seeded.best_seed = 101.00;

        const auto batch_control = run_batch(key, control);
        const auto stream_control = run_stream(key, control);
        report(which == 0 ? "A control (stream)" : "B control (stream)", stream_control);
        CHECK(stream_control.rows == batch_control.rows);
        CHECK(stream_control.position == batch_control.position);

        const auto batch_seeded = run_batch(key, seeded);
        const auto stream_seeded = run_stream(key, seeded);
        report(which == 0 ? "A seeded (stream)" : "B seeded (stream)", stream_seeded);
        CHECK(stream_seeded.rows == 1);
        if (stream_seeded.rows == 1) near(stream_seeded.exit_price, 100.50);
        CHECK(stream_seeded.position == batch_seeded.position);
        // The continuation digest folds the driving mode, so a stream never
        // answers its batch's value; what it must do is separate the two
        // trails exactly as the batch did.
        CHECK(stream_seeded.continuation != stream_control.continuation);
    }
}

// ── the buy side ────────────────────────────────────────────────────────
// A short lot's trail rides a low, so the seed is a ceiling and the two
// comparisons are the other way round. The mirror of case A about 100.00.
void buy_trail_mirrors_the_min() {
    Case c;
    c.bars = {
        bar_at(0, 100.00, 100.00, 100.00, 100.00),
        bar_at(1, 99.80, 99.90, 99.00, 99.40),
        bar_at(2, 99.40, 99.45, 99.30, 99.40),
        bar_at(3, 99.40, 99.55, 99.40, 99.50),
        bar_at(4, 99.50, 99.50, 99.50, 99.50),
    };
    c.entry_units = -1.0;
    c.submit_calculation = 2;

    const auto without = run_batch("e14-short-control", c);
    report("short control", without);
    CHECK(without.rows == 0);
    CHECK(without.position == -1.0);

    Case seeded = c;
    seeded.best_seed = 99.00;
    const auto with = run_batch("e14-short-control", seeded);
    report("short seeded 99.00", with);
    CHECK(with.rows == 1);
    CHECK(with.position == 0.0);
    if (with.rows == 1) near(with.exit_price, 99.50);
}

// ── the seed on a price grid ────────────────────────────────────────────
// Under QuantizeFillsAndTriggers the running best lives on the ladder (L8b),
// so an off-ladder seed is put on it exactly like an observed print: half-up
// by default. 100.9951 reads 101.00 as a tick, and the stop it produces is
// 100.50, not 100.4951.
void the_seed_goes_on_the_ladder() {
    Case c = case_b();
    c.grid = NativePriceGrid::QuantizeFillsAndTriggers;
    c.best_seed = 100.9951;
    const auto out = run_batch("e14-grid-seeded", c);
    report("grid seeded 100.9951", out);
    CHECK(out.rows == 1);
    if (out.rows == 1) near(out.exit_price, 100.50);
}

// ── acceptance ──────────────────────────────────────────────────────────
void a_seed_is_a_price_level() {
    Host host;
    bool ran = false;
    host.calculation = [&ran](Host& h) {
        if (h.calculations != 1 || ran) return;
        ran = true;
        const auto entry = put(h, tx(1.0, "open"));
        const auto attempt = [&h](std::optional<double> seed) {
            no::Request leg{no::Reduce{no::ExplicitUnits{1.0}}, "trail", ""};
            no::Trail trail;
            trail.offset = 0.5;
            trail.best_seed = seed;
            leg.trigger = trail;
            return h.submit(leg);
        };
        const auto zero = attempt(0.0);
        CHECK(zero.status == no::SubmitStatus::Rejected);
        REQUIRE(zero.reason.has_value());
        CHECK(*zero.reason == no::RequestRejectReason::InvalidTrigger);

        const auto negative = attempt(-1.0);
        CHECK(negative.status == no::SubmitStatus::Rejected);
        REQUIRE(negative.reason.has_value());
        CHECK(*negative.reason == no::RequestRejectReason::InvalidTrigger);

        const auto nan_seed = attempt(std::numeric_limits<double>::quiet_NaN());
        CHECK(nan_seed.status == no::SubmitStatus::Rejected);
        REQUIRE(nan_seed.reason.has_value());
        CHECK(*nan_seed.reason == no::RequestRejectReason::InvalidTrigger);

        const auto good = attempt(101.0);
        CHECK(good.status == no::SubmitStatus::Accepted);

        // An anchored trail carries the placeholder arm and an ABSOLUTE seed:
        // the anchor moves the threshold, never the level the ride starts at.
        no::Request anchored{no::Reduce{no::ExplicitUnits{1.0}}, "anchored", ""};
        no::Trail trail;
        trail.offset = 0.5;
        trail.arm_price = 0.0;
        trail.best_seed = 101.0;
        anchored.trigger = trail;
        anchored.owner = no::WaitForApplied{entry};
        anchored.anchor = no::FromOwnerFill{1.0};
        const auto anchored_out = h.submit(anchored);
        CHECK(anchored_out.status == no::SubmitStatus::Accepted);
        if (anchored_out.status != no::SubmitStatus::Accepted && anchored_out.reason) {
            std::printf("  anchored reject reason=%d\n",
                        static_cast<int>(*anchored_out.reason));
        }
    };
    const std::vector<Bar> bars{bar_at(0, 100.0, 100.0, 100.0, 100.0),
                                bar_at(1, 100.0, 100.0, 100.0, 100.0)};
    REQUIRE(host.configure_native(spec("e14-acceptance")).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(ran);
}

}  // namespace

int main() {
    test("A already armed at submission", a_already_armed_at_submission);
    test("B armed on a crossing", b_armed_on_a_crossing);
    test("streamed pair", streamed_pair);
    test("buy trail mirrors the min", buy_trail_mirrors_the_min);
    test("the seed goes on the ladder", the_seed_goes_on_the_ladder);
    test("a seed is a price level", a_seed_is_a_price_level);
    std::printf("%s trail best seed: %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
