// The consumer's schedule group (NativeExecutionConsumer::calc_group_*): its
// identity, its non-interference and its derivation over the consumer's own
// calendar.
//   1. a run that never engages the group keeps the legacy continuation identity
//      exactly: the literal below, measured on the base commit;
//   2. an engaged group is part of native_continuation_hash(), every field of its
//      state moves it, and the consumer's own continuation_hash() never moves;
//   3. the plan and the apply derive over the consumer's calendar: bar ends are
//      the calendar's, the frozen frames T4 c12 and T2 c21 come out as frozen,
//      and one trace cut two ways ends in one state;
//   4. a refusal commits nothing;
//   5. the group belongs to its run: the next begin leaves it disengaged;
//   6. a timebase without time boundaries (a raw-label partition) is planned on
//      data alone: bars and POST rows, no PRE row, no refusal.
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
// The consumer's private group state, for seeding and reading it.
struct NativeExecutionConsumerProbe {
    static native_calc::State& state(NativeExecutionConsumer& consumer) {
        return consumer.calc_group_->state;
    }
};
}  // inline namespace engine_script_run_v19
}  // namespace pineforge

namespace {
using namespace pineforge;
using native_calc::Event;
using native_calc::EventKind;
using native_calc::Finality;
using native_calc::Phase;
using native_calc::Plan;
using native_calc::State;
using native_calc::Status;

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

// The legacy continuation identity of this scenario -- six minute bars, then
// configure_native(spec) and run(bars, 6, "1", "1", false, 4, ENDPOINTS) on the
// Host below -- measured on the base commit fa5f8c5c763eddbaa64a921db49b6603c8a2121e
// (tag v1.5.0-rc.1, tree 5d2cb16965b3aa1ae5fce384b91118b24a78c67d) by box
// i-0c44b43faf99d3373 with a harness (sha256
// 07e7642149f3dcb09feeff1bde1050e415488abe6efdc0a3bbfc9bad8cba28b9) that holds
// none of this group's code: continuation hash 8797707128881238685
// (0x7a17bc057ee3529d) and one trade row.
constexpr std::uint64_t kBaseContinuationHash = 8797707128881238685ULL;
constexpr std::size_t kBaseTradeRows = 1;

class Host final : public NativeStrategyHost {
public:
    int bar = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        // One round trip, so the trade rows the check compares are not empty.
        ++bar;
        if (bar == 1) submit_market({order_action::Transact{1.0}, "e", ""});
        if (bar == 4) submit_market({execution::Flatten{}, "x", ""});
    }
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<Trade>& rows() const { return trades_; }
};

std::vector<Bar> minute_bars(int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        const double open = 100.0 + i;
        bars.push_back({open, open + 2.0, open - 1.0, open + 1.0, 10.0 + i,
                        static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

NativeRunSpec calc_spec() {
    NativeRunSpec spec;
    spec.identity = {"native-calc-identity", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "CALC";
    spec.tickerid = "TEST:CALC";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

// The golden scenario on `host`; true when it configured and ran.
bool run_golden(Host& host, const NativeRunSpec& spec) {
    const auto bars = minute_bars(6);
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) return false;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    return true;
}

constexpr std::int64_t kT0 = 1760000400000LL;
constexpr std::uint32_t kBarClose = native_calc::kCadenceBarClose;
constexpr std::uint32_t kBarCloseAndTick = native_calc::kCadenceBarClose | native_calc::kCadenceTick;

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

struct Perturbation {
    const char* name;
    void (*apply)(State&);
};

// 1 and 2: non-interference and the group's part in the identity.
void test_identity() {
    Host plain;
    CHECK("golden scenario configures", run_golden(plain, calc_spec()));
    CHECK("golden continuation hash", plain.native_continuation_hash() == kBaseContinuationHash);
    CHECK("golden trade rows", plain.rows().size() == kBaseTradeRows);
    CHECK("group starts disengaged", !plain.consumer().calc_group_engaged());

    plain.consumer().calc_group_enable();
    CHECK("group engaged", plain.consumer().calc_group_engaged());
    const std::uint64_t engaged_hash = plain.native_continuation_hash();
    CHECK("an engaged group is part of the identity", engaged_hash != kBaseContinuationHash);
    CHECK("the consumer's own identity never moves",
          plain.consumer().continuation_hash() == kBaseContinuationHash);

    const Perturbation perturbations[] = {
        {"trace_first", [](State& s) { s.trace_first += 1; }},
        {"has_prev_event", [](State& s) { s.has_prev_event = !s.has_prev_event; }},
        {"prev_event_ms", [](State& s) { s.prev_event_ms += 1; }},
        {"has_last_key", [](State& s) { s.has_last_key = !s.has_last_key; }},
        {"last_bar_key", [](State& s) { s.last_bar_key += 1; }},
        {"bar_open", [](State& s) { s.bar_open = !s.bar_open; }},
        {"open_end_ms", [](State& s) { s.open_end_ms += 1; }},
        {"calc_in_bar", [](State& s) { s.calc_in_bar += 1; }},
        {"cursor_ms", [](State& s) { s.cursor_ms += 1; }},
    };
    static_assert(sizeof(perturbations) / sizeof(perturbations[0])
                      == std::tuple_size<decltype(State{}.tie())>::value,
                  "a State field was added or dropped: extend the perturbations above");
    for (const Perturbation& perturbation : perturbations) {
        State& state = NativeExecutionConsumerProbe::state(plain.consumer());
        const State saved = state;
        perturbation.apply(state);
        CHECK(perturbation.name, plain.native_continuation_hash() != engaged_hash);
        state = saved;
        CHECK(perturbation.name, plain.native_continuation_hash() == engaged_hash);
    }
}

// 3: the derivation over the consumer's calendar (24x7, one-minute script bars).
void test_derivation_over_the_calendar() {
    Host host;
    CHECK("scenario configures", run_golden(host, calc_spec()));
    host.consumer().calc_group_enable();
    State& state = NativeExecutionConsumerProbe::state(host.consumer());

    // T4 frame c12: a tick closes bar 20 at the calendar's end of it and opens bar 21.
    state = make_state(12, true, kT0 + 50000, true, 20, true, kT0 + 60000, 0, kT0 + 60000);
    const Event tick{EventKind::Tick, kT0 + 60500};
    const Plan planned = host.consumer().calc_schedule_plan(kBarClose, &tick, 1, true);
    CHECK("c12 plan", planned.status == Status::Ok);
    CHECK("c12 plan", planned.count == 1 && planned.points.size() == 1);
    if (planned.points.size() == 1) {
        const native_calc::Point& row = planned.points[0];
        CHECK("c12 row", row.event_index == 0 && row.event == 12);
        CHECK("c12 row", row.bar_key == 20 && row.calc_ordinal == 0 && row.calc == 0);
        CHECK("c12 row", row.at_ms == kT0 + 60000);
        CHECK("c12 row", row.phase == Phase::Pre && row.finality == Finality::Confirmed);
    }
    const Plan applied = host.consumer().calc_schedule_apply(kBarClose, &tick, 1);
    CHECK("c12 apply is the plan", applied.status == Status::Ok && applied.count == planned.count);
    CHECK("c12 apply commits the state the plan left",
          same_state(state, make_state(13, true, kT0 + 60500, true, 21, true, kT0 + 120000, 0,
                                       kT0 + 120000)));
    CHECK("c12 end is the calendar's", state.open_end_ms == 1760000520000LL);

    // T2 frame c21: one time advance closes bar 31 and the carry-forward bars
    // 32, 33 and 34 the calendar yields for the quiet minutes.
    state = make_state(21, true, 1760000470000LL, true, 31, true, 1760000520000LL, 1,
                       1760000520000LL);
    const Event advance{EventKind::AdvanceTime, 1760000700000LL};
    const Plan c21 = host.consumer().calc_schedule_apply(kBarCloseAndTick, &advance, 1);
    CHECK("c21", c21.status == Status::Ok);
    CHECK("c21", c21.count == 4 && c21.points.size() == 4);
    const std::uint64_t keys[4] = {31, 32, 33, 34};
    const std::uint64_t ordinals[4] = {1, 0, 0, 0};
    const std::int64_t ends[4] = {1760000520000LL, 1760000580000LL, 1760000640000LL,
                                  1760000700000LL};
    std::size_t rows_seen = 0;
    for (std::size_t i = 0; i < c21.points.size() && i < 4; ++i) {
        CHECK("c21 row", c21.points[i].event == 21 && c21.points[i].calc == i);
        CHECK("c21 row", c21.points[i].bar_key == keys[i]);
        CHECK("c21 row", c21.points[i].calc_ordinal == ordinals[i]);
        CHECK("c21 row", c21.points[i].at_ms == ends[i]);
        CHECK("c21 row", c21.points[i].phase == Phase::Pre);
        CHECK("c21 row", c21.points[i].finality == Finality::Confirmed);
        ++rows_seen;
    }
    CHECK("c21 rows seen", rows_seen == 4);
    CHECK("c21 commits", same_state(state, make_state(22, true, 1760000700000LL, true, 34, false,
                                                      0, 0, 1760000700000LL)));

    // One trace, two cuts: T5's e20 and e21 as one frame and as two frames.
    const State s20 = make_state(20, true, 1760000455000LL, true, 30, true, 1760000460000LL, 4,
                                 1760000460000LL);
    const Event e20{EventKind::Tick, 1760000470000LL};
    const Event e21{EventKind::AdvanceTime, 1760000700000LL};
    const Event both[2] = {e20, e21};
    state = s20;
    const Plan frame_b = host.consumer().calc_schedule_apply(kBarCloseAndTick, both, 2);
    const State after_b = state;
    state = s20;
    const Plan frame_a1 = host.consumer().calc_schedule_apply(kBarCloseAndTick, &e20, 1);
    const Plan frame_a2 = host.consumer().calc_schedule_apply(kBarCloseAndTick, &e21, 1);
    CHECK("cuts", frame_b.status == Status::Ok && frame_a1.status == Status::Ok
                      && frame_a2.status == Status::Ok);
    CHECK("cuts", frame_b.points.size() == 6 && frame_a1.points.size() == 2
                      && frame_a2.points.size() == 4);
    CHECK("cuts end in one state", same_state(state, after_b));
    CHECK("trace ordinal moved across the frames of one cut", state.trace_first == 22);
    std::size_t matched = 0;
    if (frame_b.points.size() == 6 && frame_a1.points.size() == 2
        && frame_a2.points.size() == 4) {
        for (std::size_t i = 0; i < 6; ++i) {
            const native_calc::Point& b = frame_b.points[i];
            const native_calc::Point& a = i < 2 ? frame_a1.points[i] : frame_a2.points[i - 2];
            if (a.event == b.event && a.bar_key == b.bar_key
                && a.calc_ordinal == b.calc_ordinal && a.calc == b.calc && a.at_ms == b.at_ms
                && a.phase == b.phase && a.finality == b.finality) {
                ++matched;
            }
        }
    }
    CHECK("cuts plan the same rows", matched == 6);
}

// 4: a refusal commits nothing.
void test_refusals_commit_nothing() {
    Host host;
    CHECK("scenario configures", run_golden(host, calc_spec()));
    host.consumer().calc_group_enable();
    State& state = NativeExecutionConsumerProbe::state(host.consumer());
    const State seed = make_state(12, true, kT0 + 50000, true, 20, true, kT0 + 60000, 3,
                                  kT0 + 60000);
    state = seed;
    const Event tick{EventKind::Tick, kT0 + 60500};
    const Plan bad_cadence = host.consumer().calc_schedule_apply(0, &tick, 1);
    CHECK("bad cadence", bad_cadence.status == Status::BadCadence && bad_cadence.count == 0);
    CHECK("bad cadence commits nothing", same_state(state, seed));
    const Event unknown{static_cast<EventKind>(9), kT0 + 60500};
    const Plan bad_kind = host.consumer().calc_schedule_apply(kBarClose, &unknown, 1);
    CHECK("unknown kind", bad_kind.status == Status::UnknownEventKind && bad_kind.points.empty());
    CHECK("unknown kind commits nothing", same_state(state, seed));

    // The first event of the frame fits; the second needs a key that is gone.
    state = make_state(1, false, 0, true, std::numeric_limits<std::uint64_t>::max() - 1, false, 0,
                       0, kT0);
    const State nearly = state;
    const Event two[2] = {{EventKind::Bar, kT0}, {EventKind::Bar, kT0 + 60000}};
    const Plan exhausted = host.consumer().calc_schedule_apply(kBarClose, two, 2);
    CHECK("exhausted", exhausted.status == Status::BarKeyExhausted && exhausted.points.empty());
    CHECK("exhaustion commits nothing", same_state(state, nearly));
}

// Not engaged: nothing derives and nothing is committed.
void test_not_engaged() {
    Host host;
    CHECK("scenario configures", run_golden(host, calc_spec()));
    const Event tick{EventKind::Tick, kT0};
    CHECK("plan", host.consumer().calc_schedule_plan(kBarClose, &tick, 1, true).status
                      == Status::NotEngaged);
    CHECK("apply", host.consumer().calc_schedule_apply(kBarClose, &tick, 1).status
                       == Status::NotEngaged);
    CHECK("still disengaged", !host.consumer().calc_group_engaged());
}

// 5: the group belongs to the run it was enabled in. A host that engaged it in
// its first run and one that never did hold one identity after their second run.
void test_group_belongs_to_its_run() {
    const NativeRunSpec first = calc_spec();
    NativeRunSpec second = first;
    second.identity.run_number = 2;

    Host engaged_once;
    Host never_engaged;
    CHECK("first runs", run_golden(engaged_once, first) && run_golden(never_engaged, first));
    engaged_once.consumer().calc_group_enable();
    const Event tick{EventKind::Tick, kT0};
    CHECK("engaged in run 1", engaged_once.consumer().calc_schedule_apply(kBarClose, &tick, 1).status
                                  == Status::Ok);
    CHECK("second runs", run_golden(engaged_once, second) && run_golden(never_engaged, second));
    CHECK("disengaged by the next begin", !engaged_once.consumer().calc_group_engaged());
    CHECK("not engaged after the next begin",
          engaged_once.consumer().calc_schedule_plan(kBarClose, &tick, 1, true).status
              == Status::NotEngaged);
    CHECK("one identity again",
          engaged_once.native_continuation_hash() == never_engaged.native_continuation_hash());
}

// 6: a timebase without time boundaries. Frozen contract: c-boundary.md section
// 12 PXR-A2 ("a timebase without time boundaries has no PRE calculations") and
// proof-matrix.md AV-META case (5) ("no PRE calculation exists"). The packet holds
// no vector for this case, so every expectation below is derived from that rule
// alone: each bar event opens a lifecycle and closes it in the same event, a
// POST row confirmed at the event's own time; bar keys 0, 1, 2 in order; nothing
// planned for the ninety minutes between the first two events; no bar end kept
// and no cursor moved. What a tick, a time advance or a quiet interval does on
// such a timebase is not stated by the rule, and is not tested. Both ways to a
// raw-label partition: an undetected timeframe, and tolerant labels over a
// passthrough pairing (the second keeps a valid script timeframe whose calendar
// intervals must not be used).
void test_raw_label_timebase() {
    const Event events[3] = {{EventKind::WarmupBar, kT0},
                             {EventKind::Bar, kT0 + 5400000},
                             {EventKind::Bar, kT0 + 5400001}};
    const std::uint64_t event_numbers[3] = {1, 2, 3};
    const std::uint64_t keys[3] = {0, 1, 2};
    const std::int64_t times[3] = {kT0, kT0 + 5400000, kT0 + 5400001};
    for (const bool undetected : {true, false}) {
        const char* label = undetected ? "undetected timeframe" : "tolerant labels";
        NativeRunSpec spec = calc_spec();
        if (undetected) {
            spec.input_tf.clear();
            spec.script_tf.clear();
            spec.timeframe_undetected = true;
        } else {
            spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        }
        Host host;
        CHECK(label, host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.consumer().calc_group_enable();
        const Plan applied = host.consumer().calc_schedule_apply(kBarClose, events, 3);
        CHECK(label, applied.status == Status::Ok);
        CHECK(label, applied.count == 3 && applied.points.size() == 3);
        for (std::size_t i = 0; i < applied.points.size() && i < 3; ++i) {
            const native_calc::Point& row = applied.points[i];
            CHECK(label, row.event_index == i && row.event == event_numbers[i]);
            CHECK(label, row.bar_key == keys[i] && row.calc_ordinal == 0 && row.calc == 0);
            CHECK(label, row.at_ms == times[i]);
            CHECK(label, row.phase == Phase::Post && row.finality == Finality::Confirmed);
        }
        CHECK(label, same_state(NativeExecutionConsumerProbe::state(host.consumer()),
                                make_state(4, true, kT0 + 5400001, true, 2, false, 0, 0, 0)));
    }
}

}  // namespace

int main() {
    test_identity();
    test_derivation_over_the_calendar();
    test_refusals_commit_nothing();
    test_not_engaged();
    test_group_belongs_to_its_run();
    test_raw_label_timebase();
    std::printf("test_native_calc_identity: %d failure(s) in %d checks\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
