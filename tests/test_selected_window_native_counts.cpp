// Native selected-window consumption counts and the accepted pre-roll
// horizon (frozen planner semantics).
//
// Native-host fixtures driven only from outside: a bare NativeStrategyHost that
// records its callbacks, the private bridges behind strategy_set_selected_window_v1
// and strategy_selected_window_counts_v1, and a batch run(). Nothing here reads a
// consumer member. The counts are the native facts of primary input and script
// callbacks that actually completed (include/pineforge/selected_window.h), so
// every expectation below is derived from the rows given and the aligned
// 24x7 UTC clock, never from a planner:
//
//   * fed_input_bars counts accepted real input rows (a quiet carried slot is
//     not one), split on the row's own timestamp against T;
//   * fed_script_bars counts script callbacks that returned, split on the
//     published script label against T;
//   * the pre-roll horizon: the last pending bucket whose label precedes T is
//     sealed once, before the first window input's callback and open (missing
//     pre-T tail rows), or at batch end when no window input follows (no-window
//     run); an incomplete WINDOW bucket is never forced; an OFF run is unchanged.
//
// It is registered in tests/CMakeLists.txt with the other selected-window rows.
//
// The last two fixtures are controls of the generic
// open activation scope, not of the counts: begin_open_activation is refused at an
// Open whose first begin already set the marker, with or without a command in that
// first scope, and leaves the first scope's grants to fill at that Open. They need
// the private consumer header.
#include <pineforge/native_host.hpp>
#include <pineforge/selected_window.h>

// The open activation controls at the end of this file reach the native
// consumer's begin_open_activation the way the other consumer rows do, through
// as_native_consumer(execution_consumer()) inside a host callback.
#include "../src/native_execution_consumer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
// The private bridges behind the two C exports, declared exactly as
// src/c_abi.cpp declares them. The native consumer owns every null, size,
// version and state check, so a native host reaches the exports' behaviour
// without a pf_strategy_t.
int native_set_selected_window_v1(BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
int native_selected_window_counts_v1(const BacktestEngine*,
                                     pf_selected_window_counts_v1*) noexcept;
}  // inline namespace engine_script_run_v19
}  // namespace pineforge

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

enum class Kind { Input, BarOpen, Bar };

struct Event {
    Kind kind;
    std::int64_t ms;  // the input row's timestamp, or the script bar's label
};

// Records the order of the three callbacks a script bar's life consists of. A
// host that sets throw_at_label fails the run at that script bar's calculation.
struct Recorder final : NativeStrategyHost {
    std::vector<Event> events;
    std::int64_t throw_at_label = -1;

    void on_native_input(const Bar& bar, const NativeInputContext&) override {
        events.push_back({Kind::Input, bar.timestamp});
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext& context) override {
        events.push_back({Kind::BarOpen, context.script_bar_open_ms});
    }
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        events.push_back({Kind::Bar, context.script_bar_open_ms});
        if (context.script_bar_open_ms == throw_at_label) {
            throw std::runtime_error("selected-window counts fixture: callback refused");
        }
    }
};

NativeRunSpec spec_of(const char* key, std::uint64_t run_number, const char* script_tf) {
    NativeRunSpec s;
    s.identity = {key, run_number};
    s.input_tf = "1";
    s.script_tf = script_tf;
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    // The kernel records the curve: a selected capture holds the recorded window
    // observations to the window script count, and this recorder keeps none of its
    // own (test_selected_window_report_integration's HostRecorded mismatch row).
    s.report_policy = NativeReportPolicy::KernelRecorded;
    return s;
}

// One-minute rows at kT0 + minute, for first <= minute < last_exclusive, without
// the minute `skip` (a missing input slot).
std::vector<Bar> rows_of(int first, int last_exclusive, int skip = -1) {
    std::vector<Bar> rows;
    for (int minute = first; minute < last_exclusive; ++minute) {
        if (minute == skip) continue;
        const double price = 100.0 + 0.01 * minute;
        rows.push_back(Bar{price, price + 0.05, price - 0.05, price + 0.02, 1.0,
                           kT0 + minute * kMinute});
    }
    return rows;
}

// The window [kT0 + start_minute, kT0 + end_minute): T is start_minute, which
// every case below aligns to its script interval's open.
pf_selected_window_config_v1 window_of(int start_minute, int end_minute) {
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof window);
    window.version = 1;
    window.start_ms = kT0 + start_minute * kMinute;
    window.end_ms = kT0 + end_minute * kMinute;
    return window;
}

pf_selected_window_counts_v1 blank_counts() {
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = static_cast<std::uint32_t>(sizeof counts);
    counts.version = 1;
    return counts;
}

// Every field nonzero: a getter that refuses must leave it exactly so.
pf_selected_window_counts_v1 sentinel_counts() {
    pf_selected_window_counts_v1 counts = blank_counts();
    counts.run_generation = 7001;
    counts.attempt_serial = 7002;
    counts.attempt_generation = 7003;
    counts.fed_input_bars = 7004;
    counts.fed_script_bars = 7005;
    counts.preroll_input_bars = 7006;
    counts.preroll_script_bars = 7007;
    counts.window_input_bars = 7008;
    counts.window_script_bars = 7009;
    return counts;
}

bool same_counts(const pf_selected_window_counts_v1& a, const pf_selected_window_counts_v1& b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

struct Expect {
    std::uint64_t fed_input;
    std::uint64_t preroll_input;
    std::uint64_t window_input;
    std::uint64_t fed_script;
    std::uint64_t preroll_script;
    std::uint64_t window_script;
};

// The expected values, and the wire's two partition equalities of the snapshot
// itself (fed = preroll + window, for input and for script).
void check_counts(const pf_selected_window_counts_v1& counts, const Expect& expect) {
    CHECK(counts.struct_size == sizeof(pf_selected_window_counts_v1));
    CHECK(counts.version == 1);
    CHECK(counts.fed_input_bars == expect.fed_input);
    CHECK(counts.preroll_input_bars == expect.preroll_input);
    CHECK(counts.window_input_bars == expect.window_input);
    CHECK(counts.fed_script_bars == expect.fed_script);
    CHECK(counts.preroll_script_bars == expect.preroll_script);
    CHECK(counts.window_script_bars == expect.window_script);
    CHECK(counts.fed_input_bars == counts.preroll_input_bars + counts.window_input_bars);
    CHECK(counts.fed_script_bars == counts.preroll_script_bars + counts.window_script_bars);
    CHECK(counts.attempt_generation == counts.run_generation);
    CHECK(counts.run_generation >= 1);
    CHECK(counts.attempt_serial >= 1);
}

int first_index(const std::vector<Event>& events, Kind kind, std::int64_t ms) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].kind == kind && events[i].ms == ms) return static_cast<int>(i);
    }
    return -1;
}

int count_of(const std::vector<Event>& events, Kind kind, std::int64_t ms) {
    int n = 0;
    for (const Event& event : events) n += (event.kind == kind && event.ms == ms) ? 1 : 0;
    return n;
}

int count_kind(const std::vector<Event>& events, Kind kind) {
    int n = 0;
    for (const Event& event : events) n += event.kind == kind ? 1 : 0;
    return n;
}

std::vector<std::int64_t> bar_labels(const std::vector<Event>& events) {
    std::vector<std::int64_t> labels;
    for (const Event& event : events) {
        if (event.kind == Kind::Bar) labels.push_back(event.ms);
    }
    return labels;
}

// Configures `host`, sets (or, with null, clears) the window and runs the batch.
void run_selected(Recorder& host, const NativeRunSpec& spec, const std::vector<Bar>& rows,
                  const pf_selected_window_config_v1* window) {
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    CHECK(native_set_selected_window_v1(&host, window) == 0);
    host.events.clear();
    host.run(rows.data(), static_cast<int>(rows.size()));
}

// Passthrough 1m -> 1m: every row seals its own script bar, so there is nothing
// pending and the horizon never fires. T splits rows and labels alike.
void passthrough_counts() {
    std::printf("passthrough_counts\n");
    Recorder host;
    const std::vector<Bar> rows = rows_of(0, 10);
    const pf_selected_window_config_v1 window = window_of(4, 10);
    run_selected(host, spec_of("wnc-passthrough", 1, "1"), rows, &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    check_counts(counts, {10, 4, 6, 10, 4, 6});
    // The counts are the facts the engine's own diagnostic also counted.
    CHECK(static_cast<std::uint64_t>(host.script_bars_processed()) == counts.fed_script_bars);
    CHECK(count_kind(host.events, Kind::Input) == 10);
    CHECK(count_kind(host.events, Kind::Bar) == 10);
}

// Aggregation 1m -> 5m, every bucket full: each fifth row seals its bucket, the
// first two buckets (labels 0 and 5) are pre-roll and the last two are window.
void aggregate_full_buckets() {
    std::printf("aggregate_full_buckets\n");
    Recorder host;
    const std::vector<Bar> rows = rows_of(0, 20);
    const pf_selected_window_config_v1 window = window_of(10, 20);
    run_selected(host, spec_of("wnc-aggregate", 1, "5"), rows, &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    check_counts(counts, {20, 10, 10, 4, 2, 2});
    CHECK(static_cast<std::uint64_t>(host.script_bars_processed()) == counts.fed_script_bars);
    // No bucket was pending anywhere, so each label closed exactly once.
    const std::vector<std::int64_t> expected = {kT0, kT0 + 5 * kMinute, kT0 + 10 * kMinute,
                                                kT0 + 15 * kMinute};
    CHECK(bar_labels(host.events) == expected);
}

// The pre-T bucket [5,10) lost its last row (minute 9), so no own input seals it.
// The first window row (minute 10) must find it already closed: the pre-T
// calculation precedes that row's input callback, and both precede the window
// bar's open. Before the horizon the lazy seal ran after the input callback.
void missing_preroll_tail_then_window_open() {
    std::printf("missing_preroll_tail_then_window_open\n");
    Recorder host;
    const std::vector<Bar> rows = rows_of(0, 20, /*skip=*/9);
    const pf_selected_window_config_v1 window = window_of(10, 20);
    run_selected(host, spec_of("wnc-missing-tail", 1, "5"), rows, &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    check_counts(counts, {19, 9, 10, 4, 2, 2});
    CHECK(static_cast<std::uint64_t>(host.script_bars_processed()) == counts.fed_script_bars);

    const std::int64_t tail_label = kT0 + 5 * kMinute;
    const std::int64_t window_label = kT0 + 10 * kMinute;
    const int tail_bar = first_index(host.events, Kind::Bar, tail_label);
    const int window_input = first_index(host.events, Kind::Input, window_label);
    const int window_open = first_index(host.events, Kind::BarOpen, window_label);
    CHECK(tail_bar >= 0);
    CHECK(window_input >= 0);
    CHECK(window_open >= 0);
    CHECK(tail_bar < window_input);
    CHECK(window_input < window_open);
    // Sealed once: the natural lazy seal that follows finds an empty bucket.
    CHECK(count_of(host.events, Kind::BarOpen, tail_label) == 1);
    CHECK(count_of(host.events, Kind::Bar, tail_label) == 1);
    const std::vector<std::int64_t> expected = {kT0, tail_label, window_label,
                                                kT0 + 15 * kMinute};
    CHECK(bar_labels(host.events) == expected);
}

// No row reaches T: the last pending pre-T bucket ([5,10), minutes 5..7) has no
// later input to seal it, so batch end completes it before the terminal
// capture. The OFF twin over the same rows leaves it unsealed, as before.
void no_window_pending_preroll_horizon() {
    std::printf("no_window_pending_preroll_horizon\n");
    const std::vector<Bar> rows = rows_of(0, 8);
    {
        Recorder host;
        const pf_selected_window_config_v1 window = window_of(10, 20);
        run_selected(host, spec_of("wnc-horizon", 1, "5"), rows, &window);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
        check_counts(counts, {8, 8, 0, 2, 2, 0});
        CHECK(static_cast<std::uint64_t>(host.script_bars_processed()) == counts.fed_script_bars);
        const std::vector<std::int64_t> expected = {kT0, kT0 + 5 * kMinute};
        CHECK(bar_labels(host.events) == expected);
        CHECK(!host.events.empty());
        CHECK(host.events.back().kind == Kind::Bar);
        CHECK(host.events.back().ms == kT0 + 5 * kMinute);
    }
    {
        Recorder off;
        run_selected(off, spec_of("wnc-horizon-off", 1, "5"), rows, nullptr);
        CHECK(off.native_state().kind == NativeLifecycleKind::Completed);
        const std::vector<std::int64_t> expected = {kT0};
        CHECK(bar_labels(off.events) == expected);
        CHECK(off.script_bars_processed() == 1);
        pf_selected_window_counts_v1 counts = sentinel_counts();
        const pf_selected_window_counts_v1 before = counts;
        CHECK(native_selected_window_counts_v1(&off, &counts) == -2);
        CHECK(same_counts(counts, before));
    }
}

// The window's last bucket ([10,15), minutes 10..12) is incomplete: it is left
// unsealed, never forced to a callback or an open, and counted nowhere.
void incomplete_window_tail_stays_unsealed() {
    std::printf("incomplete_window_tail_stays_unsealed\n");
    Recorder host;
    const std::vector<Bar> rows = rows_of(0, 13);
    const pf_selected_window_config_v1 window = window_of(5, 20);
    run_selected(host, spec_of("wnc-window-tail", 1, "5"), rows, &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    check_counts(counts, {13, 5, 8, 2, 1, 1});
    CHECK(static_cast<std::uint64_t>(host.script_bars_processed()) == counts.fed_script_bars);
    CHECK(count_of(host.events, Kind::BarOpen, kT0 + 10 * kMinute) == 0);
    CHECK(count_of(host.events, Kind::Bar, kT0 + 10 * kMinute) == 0);
    const std::vector<std::int64_t> expected = {kT0, kT0 + 5 * kMinute};
    CHECK(bar_labels(host.events) == expected);
}

// T at the first row: no pre-roll rows or labels, and the horizon has nothing
// to complete.
void window_covers_every_row() {
    std::printf("window_covers_every_row\n");
    Recorder host;
    const std::vector<Bar> rows = rows_of(0, 10);
    const pf_selected_window_config_v1 window = window_of(0, 20);
    run_selected(host, spec_of("wnc-all-window", 1, "5"), rows, &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    check_counts(counts, {10, 0, 10, 2, 0, 2});
}

// A run with no window is OFF: no counts, the caller's storage untouched, and
// the callback stream is the unchanged one. A window that was set and then
// cleared before the next begin is OFF as well.
void off_behavior() {
    std::printf("off_behavior\n");
    Recorder host;
    const std::vector<Bar> rows = rows_of(0, 10);
    run_selected(host, spec_of("wnc-off", 1, "1"), rows, nullptr);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.script_bars_processed() == 10);
    CHECK(count_kind(host.events, Kind::Bar) == 10);
    {
        pf_selected_window_counts_v1 counts = sentinel_counts();
        const pf_selected_window_counts_v1 before = counts;
        CHECK(native_selected_window_counts_v1(&host, &counts) == -2);
        CHECK(same_counts(counts, before));
    }

    const pf_selected_window_config_v1 window = window_of(4, 10);
    CHECK(host.configure_native(spec_of("wnc-off", 2, "1")).status == NativeSetupStatus::Applied);
    CHECK(native_set_selected_window_v1(&host, &window) == 0);
    CHECK(native_set_selected_window_v1(&host, nullptr) == 0);
    host.events.clear();
    host.run(rows.data(), static_cast<int>(rows.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(count_kind(host.events, Kind::Bar) == 10);
    {
        pf_selected_window_counts_v1 counts = sentinel_counts();
        const pf_selected_window_counts_v1 before = counts;
        CHECK(native_selected_window_counts_v1(&host, &counts) == -2);
        CHECK(same_counts(counts, before));
    }
}

// A callback that throws fails the run: the getter refuses, the caller's
// storage is untouched, and the host stopped at the throwing script bar.
void failed_callback_refuses_counts() {
    std::printf("failed_callback_refuses_counts\n");
    Recorder host;
    host.throw_at_label = kT0 + 6 * kMinute;
    const std::vector<Bar> rows = rows_of(0, 10);
    const pf_selected_window_config_v1 window = window_of(4, 10);
    run_selected(host, spec_of("wnc-failed", 1, "1"), rows, &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    pf_selected_window_counts_v1 counts = sentinel_counts();
    const pf_selected_window_counts_v1 before = counts;
    CHECK(native_selected_window_counts_v1(&host, &counts) == -2);
    CHECK(same_counts(counts, before));
    CHECK(count_kind(host.events, Kind::Bar) == 7);
}

// A second run on the same handle starts from zero, and its three identities
// are the next generation's: nothing of the first run's counts carries over.
void repeated_runs_reset_identities() {
    std::printf("repeated_runs_reset_identities\n");
    Recorder host;
    const std::vector<Bar> first_rows = rows_of(0, 20);
    const pf_selected_window_config_v1 first_window = window_of(10, 20);
    run_selected(host, spec_of("wnc-repeat", 1, "5"), first_rows, &first_window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 first = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &first) == 0);
    check_counts(first, {20, 10, 10, 4, 2, 2});

    const std::vector<Bar> second_rows = rows_of(0, 12);
    const pf_selected_window_config_v1 second_window = window_of(5, 20);
    run_selected(host, spec_of("wnc-repeat", 2, "5"), second_rows, &second_window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 second = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &second) == 0);
    check_counts(second, {12, 5, 7, 2, 1, 1});
    CHECK(second.run_generation == first.run_generation + 1);
    CHECK(second.attempt_serial == first.attempt_serial + 1);
    CHECK(second.attempt_generation == second.run_generation);
    CHECK(static_cast<std::uint64_t>(host.script_bars_processed()) == second.fed_script_bars);
}

// ---- open activation: one scope per Open ------------------------------------
//
// NativeExecutionConsumer::begin_open_activation opens a capture scope inside one
// PreOpen callback at an Open frame; every request submitted inside it is granted
// its activation at that Open, and the grants stay through the Open's own match
// (match_discrete clears them and the marker together). A second begin at the SAME
// Open -- after end_open_activation, and even when the first scope held no command --
// is refused before anything is written, so the first scope's grants are not
// cleared. A later Open, and a later run, begin normally.
//
// The probe is a buy limit one unit above the Open's price (101 against a bar that
// opens at 100): a Market request born in a PreOpen callback is already eligible at
// that Open through the pre-open birth rule, so only a non-Market request shows
// whether the activation grant survived. Granted, it fills at the Open point
// (applied event cursor phase Open); ungranted it is not eligible at the point it
// was born at and fills, if at all, on a later point of the bar's path. The tape is
// four identical bars (O 100, H 105, L 95, C 100), passthrough 1m, labels m0..m3.

native_order::Request buy_limit(const char* label) {
    no::Request request{no::Transact{1.0}, label, ""};
    request.trigger = no::Limit{101.0};
    return request;
}

std::vector<Bar> activation_rows() {
    std::vector<Bar> rows;
    for (int minute = 0; minute < 4; ++minute) {
        rows.push_back(Bar{100.0, 105.0, 95.0, 100.0, 1.0, kT0 + minute * kMinute});
    }
    return rows;
}

struct ActivationFill {
    std::string label;
    NativePathPhase phase;
    double price;
};

// At every script bar's Open the host begins a scope and ends it. At `scripted_open`
// it then begins a SECOND scope at the same Open and records the answer, whether a
// scope is open after that answer, and (optionally) submits one request inside the
// first scope and one after the second begin.
struct ActivationHost final : NativeStrategyHost {
    std::int64_t scripted_open = -1;
    bool request_in_first_scope = false;
    bool request_after_second_begin = false;
    std::vector<std::pair<std::int64_t, bool>> begins;
    bool scope_open_after_second_begin = false;
    std::vector<ActivationFill> fills;

    void on_native_bar_open(const Bar&, const NativeDecisionContext& context) override {
        NativeExecutionConsumer& activation = as_native_consumer(execution_consumer());
        const std::int64_t label = context.script_bar_open_ms;
        const bool first = activation.begin_open_activation(*this);
        begins.emplace_back(label, first);
        if (label != scripted_open) {
            if (first) activation.end_open_activation();
            return;
        }
        if (first) {
            if (request_in_first_scope) (void)submit(buy_limit("grant-first"));
            activation.end_open_activation();
        }
        const bool second = activation.begin_open_activation(*this);
        begins.emplace_back(label, second);
        scope_open_after_second_begin = activation.open_activation_active();
        if (request_after_second_begin) (void)submit(buy_limit("after-second-begin"));
        if (second) activation.end_open_activation();
    }
    void on_native_applied(const native_order::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        fills.push_back({event.request().label, event.cursor.point.path_phase, event.raw_price});
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

// One run of the script: the begin answers in order (every Open begins; the
// scripted one is refused its second begin and opens no scope), then the fills.
void check_activation_run(const ActivationHost& host, bool with_requests) {
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    const std::vector<std::pair<std::int64_t, bool>> expected_begins = {
        {kT0, true},
        {kT0 + kMinute, true},
        {kT0 + kMinute, false},
        {kT0 + 2 * kMinute, true},
        {kT0 + 3 * kMinute, true},
    };
    CHECK(host.begins == expected_begins);
    CHECK(!host.scope_open_after_second_begin);
    if (!with_requests) {
        CHECK(host.fills.empty());
        return;
    }
    // The first scope's grant is still there at the Open's match: that request fills
    // at the Open point itself, at the Open's price or better than the limit.
    const ActivationFill* granted = nullptr;
    for (const ActivationFill& fill : host.fills) {
        if (fill.label == "grant-first") granted = &fill;
    }
    CHECK(granted != nullptr);
    if (granted != nullptr) {
        CHECK(granted->phase == NativePathPhase::Open);
        CHECK(granted->price >= 100.0 - 1e-9 && granted->price <= 101.0 + 1e-9);
    }
    // The request submitted after the refused begin was born outside any scope, so it
    // holds no grant and never fills at that Open.
    for (const ActivationFill& fill : host.fills) {
        if (fill.label == "after-second-begin") CHECK(fill.phase != NativePathPhase::Open);
    }
}

void run_activation(ActivationHost& host, std::uint64_t run_number) {
    const std::vector<Bar> rows = activation_rows();
    CHECK(host.configure_native(spec_of("wac-repeat", run_number, "1")).status
          == NativeSetupStatus::Applied);
    host.run(rows.data(), static_cast<int>(rows.size()));
}

// First scope submits an eligible request and ends; a second begin at the same Open
// answers false and opens nothing; the first request's grant is kept and it fills at
// that Open; a request submitted after the refusal holds no grant; the later Opens
// begin normally; and a later run on the same handle repeats all of it.
void repeated_begin_at_the_same_open_is_refused() {
    std::printf("repeated_begin_at_the_same_open_is_refused\n");
    ActivationHost host;
    host.scripted_open = kT0 + kMinute;
    host.request_in_first_scope = true;
    host.request_after_second_begin = true;
    run_activation(host, 1);
    check_activation_run(host, true);

    host.begins.clear();
    host.fills.clear();
    host.scope_open_after_second_begin = false;
    run_activation(host, 2);
    check_activation_run(host, true);
}

// A first scope that held no command still set the marker: it cannot be reopened at
// that Open either, and nothing fills. The Opens after it, and a later run, begin
// normally.
void empty_first_scope_cannot_reopen() {
    std::printf("empty_first_scope_cannot_reopen\n");
    ActivationHost host;
    host.scripted_open = kT0 + kMinute;
    run_activation(host, 1);
    check_activation_run(host, false);

    host.begins.clear();
    host.scope_open_after_second_begin = false;
    run_activation(host, 2);
    check_activation_run(host, false);
}

}  // namespace

int main() {
    try {
        repeated_begin_at_the_same_open_is_refused();
        empty_first_scope_cannot_reopen();
        passthrough_counts();
        aggregate_full_buckets();
        missing_preroll_tail_then_window_open();
        no_window_pending_preroll_horizon();
        incomplete_window_tail_stays_unsealed();
        window_covers_every_row();
        off_behavior();
        failed_callback_refuses_counts();
        repeated_runs_reset_identities();
    } catch (const std::exception& e) {
        ++failed;
        std::fprintf(stderr, "uncaught exception: %s\n", e.what());
    } catch (...) {
        ++failed;
        std::fprintf(stderr, "uncaught non-standard exception\n");
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
