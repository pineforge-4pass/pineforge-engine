// The pure schedule derivation (src/native_calc_plan.hpp) against the frozen
// named-stream oracle. Every expected row and every expected state below is a
// literal read off the frozen vectors or worked by hand from the contract; none
// is produced by the derivation it judges. Frame names are the frozen frames:
//   * T4 frame c12 (cadence bar close only): one tick closes a bar, exactly one row;
//   * T2 frames c20 and c21, and T5's single frame holding both: one tick that
//     closes a bar and opens the next, then one time advance that closes four;
//   * T3 frames c7, c8, c9: three frames in a row, the trace ordinal moving on;
//   * bar replay partitions a and b: bar keys 0..4 however the trace is cut;
//   * val-bar-key-u64-max and rf-bar-key-exhausted: the ordinal boundary.
#include "../src/native_calc_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <tuple>
#include <vector>

namespace {
using namespace pineforge::native_calc;

int checks = 0;
int failures = 0;

#define CHECK(label, expression)                                                  \
    do {                                                                          \
        ++checks;                                                                 \
        if (!(expression)) {                                                      \
            ++failures;                                                           \
            std::printf("FAIL [%s] line %d: %s\n", label, __LINE__, #expression); \
        }                                                                         \
    } while (false)

constexpr std::int64_t kT0 = 1760000400000LL;
constexpr std::int64_t kMinute = 60000;
constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t kBarCloseOnly = kCadenceBarClose;
constexpr std::uint32_t kBarCloseAndTick = kCadenceBarClose | kCadenceTick;

// Every interval of `period` milliseconds from the epoch yields a bar.
BarClock grid(std::int64_t period) {
    return [period](std::int64_t at, Slot& slot) {
        const std::int64_t open = at - ((at % period) + period) % period;
        slot.end_ms = open + period;
        slot.next_open_ms = open + period;
        slot.tradable = true;
        return true;
    };
}

// A minute grid whose closed time [gap_start, gap_end) yields no bar. With
// `jump` the interval before the gap points past it; without, every closed
// interval is visited and is not tradable.
BarClock gap_grid(std::int64_t gap_start, std::int64_t gap_end, bool jump) {
    return [=](std::int64_t at, Slot& slot) {
        const std::int64_t open = at - ((at % kMinute) + kMinute) % kMinute;
        slot.end_ms = open + kMinute;
        slot.tradable = !(open >= gap_start && open < gap_end);
        slot.next_open_ms = (jump && slot.end_ms == gap_start) ? gap_end : slot.end_ms;
        return true;
    };
}

State make_state(std::uint64_t trace_first, bool has_prev, std::int64_t prev, bool has_key,
                 std::uint64_t key, bool open, std::int64_t end, std::uint64_t in_bar,
                 std::int64_t cursor) {
    State s;
    s.trace_first = trace_first;
    s.has_prev_event = has_prev;
    s.prev_event_ms = prev;
    s.has_last_key = has_key;
    s.last_bar_key = key;
    s.bar_open = open;
    s.open_end_ms = end;
    s.calc_in_bar = in_bar;
    s.cursor_ms = cursor;
    return s;
}

bool same_state(const State& a, const State& b) { return a.tie() == b.tie(); }

struct Want {
    std::uint64_t event_index;
    std::uint64_t event;
    std::uint64_t bar_key;
    std::uint64_t calc_ordinal;
    std::uint64_t calc;
    std::int64_t at_ms;
    Phase phase;
    Finality finality;
};

void expect_rows(const char* label, const Plan& plan, const std::vector<Want>& want) {
    CHECK(label, plan.status == Status::Ok);
    CHECK(label, plan.count == want.size());
    CHECK(label, plan.points.size() == want.size());
    if (plan.points.size() != want.size()) return;
    std::size_t compared = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const Point& p = plan.points[i];
        const Want& w = want[i];
        CHECK(label, p.event_index == w.event_index);
        CHECK(label, p.event == w.event);
        CHECK(label, p.bar_key == w.bar_key);
        CHECK(label, p.calc_ordinal == w.calc_ordinal);
        CHECK(label, p.calc == w.calc);
        CHECK(label, p.at_ms == w.at_ms);
        CHECK(label, p.phase == w.phase);
        CHECK(label, p.finality == w.finality);
        ++compared;
    }
    CHECK(label, compared == want.size());
}

bool same_ignoring_index(const Point& a, const Point& b) {
    return a.event == b.event && a.bar_key == b.bar_key && a.calc_ordinal == b.calc_ordinal
        && a.calc == b.calc && a.at_ms == b.at_ms && a.phase == b.phase
        && a.finality == b.finality;
}

// A refusal plans nothing, counts nothing and hands the state back.
void expect_refusal(const char* label, const Plan& plan, Status why, const State& before) {
    CHECK(label, plan.status == why);
    CHECK(label, plan.status != Status::Ok);
    CHECK(label, plan.count == 0);
    CHECK(label, plan.points.empty());
    CHECK(label, same_state(plan.after, before));
}

constexpr Phase kPre = Phase::Pre;
constexpr Phase kPost = Phase::Post;
constexpr Finality kConfirmed = Finality::Confirmed;
constexpr Finality kProvisional = Finality::Provisional;

// T4 frame c12: the consumer's cadence is bar close only, so the tick of e12
// (T0 + 60500) closes bar 20 at its end, T0 + 60000, and nothing else: its own
// update is not a calculation of this cadence. The tick still opens bar 21.
void test_t4_c12_one_row() {
    const State before = make_state(12, true, kT0 + 50000, true, 20, true, kT0 + 60000, 0,
                                    kT0 + 60000);
    const Event tick{EventKind::Tick, kT0 + 60500};
    const Plan plan = plan_schedule(before, kBarCloseOnly, &tick, 1, grid(kMinute), true);
    expect_rows("t4 c12", plan, {{0, 12, 20, 0, 0, kT0 + 60000, kPre, kConfirmed}});
    CHECK("t4 c12 after",
          same_state(plan.after, make_state(13, true, kT0 + 60500, true, 21, true,
                                            kT0 + 120000, 0, kT0 + 120000)));
}

// T2 frames c20 and c21, T5 partitions a and b. The cadence is bar close and
// tick. Frame c20: the tick of e20 closes bar 30 (its fifth calculation) and its
// own update opens bar 31. Frame c21: the advance to T0 + 300000 closes bar 31 and
// the three carry-forward bars 32, 33 and 34.
void test_t2_t5_partitions() {
    const BarClock clock = grid(kMinute);
    const State s20 = make_state(20, true, 1760000455000LL, true, 30, true, 1760000460000LL, 4,
                                 1760000460000LL);
    const Event e20{EventKind::Tick, 1760000470000LL};
    const Event e21{EventKind::AdvanceTime, 1760000700000LL};
    const Event both[2] = {e20, e21};

    const Plan whole = plan_schedule(s20, kBarCloseAndTick, both, 2, clock, true);
    expect_rows("t5 whole", whole,
                {{0, 20, 30, 4, 0, 1760000460000LL, kPre, kConfirmed},
                 {0, 20, 31, 0, 1, 1760000470000LL, kPost, kProvisional},
                 {1, 21, 31, 1, 0, 1760000520000LL, kPre, kConfirmed},
                 {1, 21, 32, 0, 1, 1760000580000LL, kPre, kConfirmed},
                 {1, 21, 33, 0, 2, 1760000640000LL, kPre, kConfirmed},
                 {1, 21, 34, 0, 3, 1760000700000LL, kPre, kConfirmed}});
    const State s22 = make_state(22, true, 1760000700000LL, true, 34, false, 0, 0,
                                 1760000700000LL);
    CHECK("t5 whole after", same_state(whole.after, s22));

    // Partition a: two frames, the second planned from the state the first left.
    const Plan first = plan_schedule(s20, kBarCloseAndTick, &e20, 1, clock, true);
    expect_rows("t2 c20", first,
                {{0, 20, 30, 4, 0, 1760000460000LL, kPre, kConfirmed},
                 {0, 20, 31, 0, 1, 1760000470000LL, kPost, kProvisional}});
    CHECK("t2 c20 trace advances", first.after.trace_first == 21);
    const Plan second = plan_schedule(first.after, kBarCloseAndTick, &e21, 1, clock, true);
    expect_rows("t2 c21", second,
                {{0, 21, 31, 1, 0, 1760000520000LL, kPre, kConfirmed},
                 {0, 21, 32, 0, 1, 1760000580000LL, kPre, kConfirmed},
                 {0, 21, 33, 0, 2, 1760000640000LL, kPre, kConfirmed},
                 {0, 21, 34, 0, 3, 1760000700000LL, kPre, kConfirmed}});
    CHECK("t2 c21 trace advances", second.after.trace_first == 22);
    CHECK("t5 partitions end in one state", same_state(second.after, whole.after));

    // The same rows in either partition, except the index into their own frame.
    std::size_t matched = 0;
    for (std::size_t i = 0; i < first.points.size() && i < whole.points.size(); ++i) {
        if (same_ignoring_index(first.points[i], whole.points[i])) ++matched;
    }
    for (std::size_t i = 0; i < second.points.size() && i + 2 < whole.points.size(); ++i) {
        if (same_ignoring_index(second.points[i], whole.points[i + 2])) ++matched;
    }
    CHECK("t5 partitions plan the same rows", matched == 6);

    // The count query is the same derivation without collecting the rows.
    const Plan counted = plan_schedule(s20, kBarCloseAndTick, both, 2, clock, false);
    CHECK("count query", counted.status == Status::Ok);
    CHECK("count query", counted.count == 6);
    CHECK("count query", counted.points.empty());
    CHECK("count query", same_state(counted.after, whole.after));
}

// T3 frames c7, c8, c9 in a row: an update, a tick that closes bar 100 and opens
// bar 101, then a time advance that closes bar 101 at exactly the event's time.
// Advance time is not in this cadence, so frame c9 plans only that closure.
void test_t3_three_frames() {
    const BarClock clock = grid(kMinute);
    const State s7 = make_state(7, true, 1760000680000LL, true, 100, true, 1760000700000LL, 3,
                                1760000700000LL);
    const Event e7{EventKind::Tick, 1760000690000LL};
    const Event e8{EventKind::Tick, 1760000702000LL};
    const Event e9{EventKind::AdvanceTime, 1760000760000LL};
    const Plan c7 = plan_schedule(s7, kBarCloseAndTick, &e7, 1, clock, true);
    expect_rows("t3 c7", c7, {{0, 7, 100, 3, 0, 1760000690000LL, kPost, kProvisional}});
    const Plan c8 = plan_schedule(c7.after, kBarCloseAndTick, &e8, 1, clock, true);
    expect_rows("t3 c8", c8,
                {{0, 8, 100, 4, 0, 1760000700000LL, kPre, kConfirmed},
                 {0, 8, 101, 0, 1, 1760000702000LL, kPost, kProvisional}});
    CHECK("t3 c8 update lands on the next bar", c8.points[1].bar_key == c8.points[0].bar_key + 1);
    const Plan c9 = plan_schedule(c8.after, kBarCloseAndTick, &e9, 1, clock, true);
    expect_rows("t3 c9", c9, {{0, 9, 101, 1, 0, 1760000760000LL, kPre, kConfirmed}});
    CHECK("t3 trace after c9", c9.after.trace_first == 10);
    CHECK("t3 bar 101 is closed", !c9.after.bar_open);
}

// Bar replay: three warm-up bars and two bars; bar keys 0..4 in the single trace
// and in partitions a (five frames) and b (frames of one to three events). The
// keys of the live bars are the frozen records av-bar-key-3-live and -4-live.
void test_bar_replay_partitions() {
    const BarClock clock = grid(kMinute);
    const Event trace[5] = {{EventKind::WarmupBar, 1760000220000LL},
                            {EventKind::WarmupBar, 1760000280000LL},
                            {EventKind::WarmupBar, 1760000340000LL},
                            {EventKind::Bar, 1760000400000LL},
                            {EventKind::Bar, 1760000460000LL}};
    const Plan whole = plan_schedule(State{}, kBarCloseOnly, trace, 5, clock, true);
    expect_rows("bar replay whole", whole,
                {{0, 1, 0, 0, 0, 1760000220000LL, kPost, kConfirmed},
                 {1, 2, 1, 0, 0, 1760000280000LL, kPost, kConfirmed},
                 {2, 3, 2, 0, 0, 1760000340000LL, kPost, kConfirmed},
                 {3, 4, 3, 0, 0, 1760000400000LL, kPost, kConfirmed},
                 {4, 5, 4, 0, 0, 1760000460000LL, kPost, kConfirmed}});

    State a;
    std::vector<Point> a_rows;
    for (int i = 0; i < 5; ++i) {
        const Plan step = plan_schedule(a, kBarCloseOnly, &trace[i], 1, clock, true);
        CHECK("bar replay a", step.status == Status::Ok);
        a = step.after;
        a_rows.insert(a_rows.end(), step.points.begin(), step.points.end());
    }
    State b;
    std::vector<Point> b_rows;
    const std::size_t cuts[4] = {0, 3, 4, 5};
    for (int part = 0; part < 3; ++part) {
        const Plan step = plan_schedule(b, kBarCloseOnly, trace + cuts[part],
                                        cuts[part + 1] - cuts[part], clock, true);
        CHECK("bar replay b", step.status == Status::Ok);
        b = step.after;
        b_rows.insert(b_rows.end(), step.points.begin(), step.points.end());
    }
    CHECK("bar replay row counts", a_rows.size() == 5 && b_rows.size() == 5);
    std::size_t matched = 0;
    for (std::size_t i = 0; i < a_rows.size() && i < b_rows.size() && i < whole.points.size();
         ++i) {
        if (same_ignoring_index(a_rows[i], whole.points[i])
            && same_ignoring_index(b_rows[i], whole.points[i])) {
            ++matched;
        }
    }
    CHECK("bar replay partitions plan the same rows", matched == 5);
    CHECK("bar replay a ends where whole ends", same_state(a, whole.after));
    CHECK("bar replay b ends where whole ends", same_state(b, whole.after));
    CHECK("bar replay last key", whole.after.last_bar_key == 4 && whole.after.trace_first == 6);
}

// val-bar-key-u64-max and rf-bar-key-exhausted: the largest ordinal is a legal
// key; a lifecycle is refused only when it would need the one after it.
void test_bar_key_boundary() {
    const BarClock clock = grid(kMinute);
    const Event bar{EventKind::Bar, kT0};

    const State penultimate = make_state(1, false, 0, true, kMax - 1, false, 0, 0, kT0);
    const Plan legal = plan_schedule(penultimate, kBarCloseOnly, &bar, 1, clock, true);
    expect_rows("u64 max key is legal", legal, {{0, 1, kMax, 0, 0, kT0, kPost, kConfirmed}});
    CHECK("u64 max key is legal", legal.points.size() == 1 && legal.points[0].bar_key == 18446744073709551615ULL);

    const State last = make_state(1, false, 0, true, kMax, false, 0, 0, kT0);
    expect_refusal("rf bar key exhausted", plan_schedule(last, kBarCloseOnly, &bar, 1, clock, true),
                   Status::BarKeyExhausted, last);

    // An open bar that already holds the largest key needs no new ordinal.
    const State holding = make_state(1, true, kT0 - 1000, true, kMax, true, kT0 + kMinute, 2,
                                     kT0 + kMinute);
    const Event inside{EventKind::Tick, kT0 + 1000};
    expect_rows("tick inside the bar holding the largest key",
                plan_schedule(holding, kBarCloseAndTick, &inside, 1, clock, true),
                {{0, 1, kMax, 2, 0, kT0 + 1000, kPost, kProvisional}});

    // The refusal is of the whole schedule: the first bar would have fit.
    const Event two[2] = {{EventKind::Bar, kT0}, {EventKind::Bar, kT0 + kMinute}};
    expect_refusal("second lifecycle exhausts the keys",
                   plan_schedule(penultimate, kBarCloseOnly, two, 2, clock, true),
                   Status::BarKeyExhausted, penultimate);

    // A carry-forward bar needs an ordinal too.
    const State quiet = make_state(5, true, kT0, true, kMax, false, 0, 0, kT0 + kMinute);
    const Event later{EventKind::AdvanceTime, kT0 + 5 * kMinute};
    expect_refusal("carry-forward bar exhausts the keys",
                   plan_schedule(quiet, kBarCloseOnly, &later, 1, clock, true),
                   Status::BarKeyExhausted, quiet);
}

// The cadence decides which events calculate; the refusals are not zero rows.
void test_cadence() {
    const BarClock clock = grid(kMinute);
    const Event tick{EventKind::Tick, kT0 + 1000};
    const Event advance{EventKind::AdvanceTime, kT0 + 30000};
    const State open = make_state(9, true, kT0, true, 3, true, kT0 + kMinute, 2, kT0 + kMinute);

    // A tick under a bar-close-only cadence opens a bar and plans no calculation.
    const Plan silent = plan_schedule(State{}, kBarCloseOnly, &tick, 1, clock, true);
    CHECK("tick without tick cadence", silent.status == Status::Ok);
    CHECK("tick without tick cadence", silent.count == 0);
    CHECK("tick without tick cadence", silent.points.empty());
    CHECK("tick without tick cadence", silent.after.has_last_key && silent.after.bar_open);
    CHECK("tick without tick cadence", silent.after.last_bar_key == 0 && silent.after.calc_in_bar == 0);

    // A time advance inside the open bar calculates only on an advance-time cadence.
    const Plan idle = plan_schedule(open, kBarCloseAndTick, &advance, 1, clock, true);
    CHECK("advance without advance cadence", idle.status == Status::Ok);
    CHECK("advance without advance cadence", idle.count == 0 && idle.points.empty());
    CHECK("advance without advance cadence", idle.after.trace_first == 10);
    const Plan updated = plan_schedule(open, kBarCloseOnly | kCadenceAdvanceTime, &advance, 1,
                                       clock, true);
    expect_rows("advance with advance cadence", updated,
                {{0, 9, 3, 2, 0, kT0 + 30000, kPost, kProvisional}});
    CHECK("advance with advance cadence", updated.after.calc_in_bar == 3);

    // No open bar, no calculation, and no bar key: nothing to update.
    const Plan nothing = plan_schedule(State{}, kBarCloseOnly | kCadenceAdvanceTime, &advance,
                                       1, clock, true);
    CHECK("advance with no bar", nothing.status == Status::Ok);
    CHECK("advance with no bar", nothing.count == 0 && nothing.points.empty());
    CHECK("advance with no bar", !nothing.after.has_last_key && !nothing.after.bar_open);
    CHECK("advance with no bar", nothing.after.trace_first == 2);

    // A cadence must contain bar close and only the three known bits.
    expect_refusal("cadence without bar close",
                   plan_schedule(State{}, kCadenceTick, &tick, 1, clock, true),
                   Status::BadCadence, State{});
    expect_refusal("empty cadence", plan_schedule(State{}, 0, &tick, 1, clock, true),
                   Status::BadCadence, State{});
    expect_refusal("cadence with an unknown bit",
                   plan_schedule(State{}, kBarCloseOnly | 8u, &tick, 1, clock, true),
                   Status::BadCadence, State{});
}

// No bar key where no bar exists, and none for time alone unless the timebase
// says a quiet interval yields a bar.
void test_no_bar_no_key() {
    // A closed bar and a tick still inside its interval: nothing to close.
    const State closed = make_state(4, true, kT0 + 60000, true, 6, false, 0, 0, kT0 + 120000);
    const Event tick{EventKind::Tick, kT0 + 130000};
    const Plan plan = plan_schedule(closed, kBarCloseAndTick, &tick, 1, grid(kMinute), true);
    expect_rows("tick after a closed bar", plan,
                {{0, 4, 7, 0, 0, kT0 + 130000, kPost, kProvisional}});

    // Closed time yields no bar: keys run on across it, with and without a jump.
    const std::int64_t gap_start = kT0 + 2 * kMinute;
    const std::int64_t gap_end = kT0 + 4 * kMinute;
    const State before = make_state(2, true, kT0 + 1000, true, 7, true, kT0 + kMinute, 0,
                                    kT0 + kMinute);
    const Event advance{EventKind::AdvanceTime, kT0 + 7 * kMinute};
    const std::vector<Want> rows = {{0, 2, 7, 0, 0, kT0 + 1 * kMinute, kPre, kConfirmed},
                                    {0, 2, 8, 0, 1, kT0 + 2 * kMinute, kPre, kConfirmed},
                                    {0, 2, 9, 0, 2, kT0 + 5 * kMinute, kPre, kConfirmed},
                                    {0, 2, 10, 0, 3, kT0 + 6 * kMinute, kPre, kConfirmed},
                                    {0, 2, 11, 0, 4, kT0 + 7 * kMinute, kPre, kConfirmed}};
    expect_rows("closed time with a jump",
                plan_schedule(before, kBarCloseOnly, &advance, 1,
                              gap_grid(gap_start, gap_end, true), true), rows);
    expect_rows("closed time without a jump",
                plan_schedule(before, kBarCloseOnly, &advance, 1,
                              gap_grid(gap_start, gap_end, false), true), rows);
}

// The end of a bar comes from the timeline and has to come after the event.
void test_derived_end() {
    const Event tick{EventKind::Tick, kT0 + 1000};
    const BarClock ends_at_the_event = [](std::int64_t at, Slot& slot) {
        slot.end_ms = at;
        slot.next_open_ms = at + 1;
        slot.tradable = true;
        return true;
    };
    expect_refusal("end not after the event",
                   plan_schedule(State{}, kBarCloseAndTick, &tick, 1, ends_at_the_event, true),
                   Status::BarEndNotAfterEvent, State{});
    const BarClock no_slot = [](std::int64_t, Slot&) { return false; };
    expect_refusal("no slot at the event",
                   plan_schedule(State{}, kBarCloseAndTick, &tick, 1, no_slot, true),
                   Status::NoBarSlot, State{});

    // A bar's retained end is the one its timeline gave, whatever the period.
    for (const std::int64_t period : {kMinute, 5 * kMinute, 60 * kMinute}) {
        const Plan plan = plan_schedule(State{}, kBarCloseAndTick, &tick, 1, grid(period), true);
        CHECK("retained end follows the timeline", plan.status == Status::Ok);
        CHECK("retained end follows the timeline", plan.after.open_end_ms == kT0 + period);
        CHECK("retained end follows the timeline", plan.after.cursor_ms == kT0 + period);
    }
}

// The window of a Pre position, and the refusal that is not an admission error.
void test_windows() {
    const BarClock clock = grid(kMinute);
    const Event e{EventKind::Tick, 6000};
    // The retained end lies before the previous event: the kernel's own planned
    // position breaks its window.
    const State broken = make_state(3, true, 5000, true, 1, true, 1000, 0, 1000);
    expect_refusal("pre position below the window",
                   plan_schedule(broken, kBarCloseAndTick, &e, 1, clock, true),
                   Status::PositionOutsideWindow, broken);
    // Equality at the lower bound is inside the window.
    const State at_bound = make_state(3, true, 5000, true, 1, true, 5000, 0, 5000);
    const Plan at_the_bound = plan_schedule(at_bound, kBarCloseAndTick, &e, 1, clock, true);
    expect_rows("pre position at the lower bound", at_the_bound,
                {{0, 3, 1, 0, 0, 5000, kPre, kConfirmed},
                 {0, 3, 2, 0, 1, 6000, kPost, kProvisional}});
    // No event before the first: the window is unbounded below, whatever the
    // stale previous time says.
    const State first = make_state(1, false, 5000, true, 1, true, 1000, 0, 1000);
    const Plan unbounded = plan_schedule(first, kBarCloseAndTick, &e, 1, clock, true);
    CHECK("first event window", unbounded.status == Status::Ok);
    CHECK("first event window", unbounded.points.size() == 2 && unbounded.points[0].at_ms == 1000);
}

// A kind outside the registry is refused, even behind a valid event.
void test_refusals() {
    const BarClock clock = grid(kMinute);
    const Event good{EventKind::Tick, kT0};
    for (const std::uint32_t raw : {0u, 5u, 99u}) {
        const Event bad{static_cast<EventKind>(raw), kT0};
        const Event pair[2] = {good, bad};
        expect_refusal("unknown kind", plan_schedule(State{}, kBarCloseAndTick, &bad, 1, clock, true),
                       Status::UnknownEventKind, State{});
        expect_refusal("unknown kind behind a valid event",
                       plan_schedule(State{}, kBarCloseAndTick, pair, 2, clock, true),
                       Status::UnknownEventKind, State{});
    }
    CHECK("known kinds", is_known(EventKind::WarmupBar) && is_known(EventKind::AdvanceTime));
    expect_refusal("null events", plan_schedule(State{}, kBarCloseOnly, nullptr, 1, clock, true),
                   Status::BadArguments, State{});
    const Plan empty = plan_schedule(State{}, kBarCloseOnly, nullptr, 0, clock, true);
    CHECK("empty schedule", empty.status == Status::Ok && empty.count == 0);
    CHECK("empty schedule", same_state(empty.after, State{}));
    const State spent = make_state(kMax, false, 0, false, 0, false, 0, 0, 0);
    expect_refusal("trace ordinal exhausted", plan_schedule(spent, kBarCloseOnly, &good, 1, clock, true),
                   Status::TraceExhausted, spent);
}

// The digest and the field list cover every field of the State.
void test_every_field_counts() {
    static_assert(std::tuple_size<decltype(State{}.tie())>::value == 9,
                  "a State field was added or dropped: extend the perturbations below");
    const State base = make_state(5, true, 100, true, 7, true, 200, 3, 300);
    State changed[9] = {base, base, base, base, base, base, base, base, base};
    const char* names[9] = {"trace_first", "has_prev_event", "prev_event_ms", "has_last_key",
                            "last_bar_key", "bar_open", "open_end_ms", "calc_in_bar", "cursor_ms"};
    changed[0].trace_first += 1;
    changed[1].has_prev_event = false;
    changed[2].prev_event_ms += 1;
    changed[3].has_last_key = false;
    changed[4].last_bar_key += 1;
    changed[5].bar_open = false;
    changed[6].open_end_ms += 1;
    changed[7].calc_in_bar += 1;
    changed[8].cursor_ms += 1;
    for (int i = 0; i < 9; ++i) {
        CHECK(names[i], digest(changed[i]) != digest(base));
        CHECK(names[i], !same_state(changed[i], base));
    }
    CHECK("digest is a function of the state", digest(base) == digest(make_state(5, true, 100, true, 7, true, 200, 3, 300)));

    // The next quiet interval is looked up at the cursor: moving it past bar 32's
    // interval leaves two carry-forward bars where it left three.
    const BarClock clock = grid(kMinute);
    const Event advance{EventKind::AdvanceTime, 1760000700000LL};
    const State at_520 = make_state(21, true, 1760000470000LL, true, 31, true, 1760000520000LL, 1,
                                    1760000520000LL);
    State at_580 = at_520;
    at_580.cursor_ms = 1760000580000LL;
    CHECK("cursor decides the carry-forward bars",
          plan_schedule(at_520, kBarCloseOnly, &advance, 1, clock, false).count == 4);
    CHECK("cursor decides the carry-forward bars",
          plan_schedule(at_580, kBarCloseOnly, &advance, 1, clock, false).count == 3);
}

}  // namespace

int main() {
    test_t4_c12_one_row();
    test_t2_t5_partitions();
    test_t3_three_frames();
    test_bar_replay_partitions();
    test_bar_key_boundary();
    test_cadence();
    test_no_bar_no_key();
    test_derived_end();
    test_windows();
    test_refusals();
    test_every_field_counts();
    std::printf("test_native_calc_plan: %d failure(s) in %d checks\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
