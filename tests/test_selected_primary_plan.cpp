// Selected primary planner fixtures (pineforge::plan_selected_primary).
//
// Every expected value below is derived by hand from the frozen planner
// semantics and the native consumer's grouping and seal rules. The native
// observed-counter equality and the OFF identity are covered by the consumer's
// own counters, never by this file.
//
// Black box through the public call only: rows in, plan out. The planner reads
// timestamps only, so the prices are inert. Clocks are UTC 24x7 except where a
// row says otherwise, so the expected groups are plain hour/minute arithmetic
// from kBase. Two rows need the host's tzdata (America/New_York) and say so.
//
// Group arithmetic used throughout (1m input unless stated): a group is a run
// of rows under one script interval; an input at the interval's last minute
// reaches the script's next period open and seals it; a later group's first
// row seals a pending one; a final window group neither rule seals is never
// executed and is not counted in window_script_bars, though its rows are in
// window_input_bars.

#include <pineforge/selected_window_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using pineforge::Bar;
using pineforge::SelectedPlanRequest;
using pineforge::SelectedPlanStatus;
using pineforge::SelectedPrimaryPlan;

static_assert(std::is_same<decltype(&pineforge::plan_selected_primary),
                           SelectedPrimaryPlan (*)(const Bar*, std::size_t,
                                                   const SelectedPlanRequest&)>::value,
              "the frozen call signature");

int failures = 0;
const char* scenario = "initialization";

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", scenario, __FILE__,      \
                     __LINE__, #condition);                                    \
    }                                                                          \
} while (0)

constexpr std::int64_t kMin = 60'000;
constexpr std::int64_t kHour = 60 * kMin;
constexpr std::int64_t kBase = 1'704'067'200'000;  // 2024-01-01T00:00:00Z, a Monday
constexpr std::int64_t kI53 = (std::int64_t{1} << 53) - 1;

Bar row_at(std::int64_t timestamp) { return Bar{100.0, 101.0, 99.0, 100.5, 10.0, timestamp}; }

void add_span(std::vector<Bar>& rows, std::int64_t from, std::int64_t until, std::int64_t step) {
    for (std::int64_t at = from; at < until; at += step) rows.push_back(row_at(at));
}

std::vector<Bar> span(std::int64_t from, std::int64_t until, std::int64_t step) {
    std::vector<Bar> rows;
    add_span(rows, from, until, step);
    return rows;
}

std::vector<Bar> rows_at(std::initializer_list<std::int64_t> timestamps) {
    std::vector<Bar> rows;
    for (const std::int64_t at : timestamps) rows.push_back(row_at(at));
    return rows;
}

SelectedPlanRequest make_request(std::int64_t start, std::int64_t end, std::int64_t fed_start,
                                 std::uint32_t preroll, const char* input_tf,
                                 const char* script_tf) {
    SelectedPlanRequest request{};
    request.start_ms = start;
    request.end_ms = end;
    request.fed_start_ms = fed_start;
    request.preroll_bars = preroll;
    request.input_tf = input_tf;
    request.script_tf = script_tf;
    request.chart_timezone = "UTC";
    request.engine_timezone = "UTC";
    request.session = "";
    return request;
}

SelectedPrimaryPlan plan_of(const std::vector<Bar>& rows, const SelectedPlanRequest& request) {
    return pineforge::plan_selected_primary(rows.data(), rows.size(), request);
}

struct Expect {
    std::uint64_t supplied_input = 0, supplied_script = 0, available = 0, used = 0;
    std::uint64_t trimmed = 0, trim_index = 0, fed_input = 0, fed_script = 0;
    std::uint64_t preroll_input = 0, window_input = 0, window_script = 0;
    std::int64_t trim_start = 0;
    std::optional<std::int64_t> preroll_first, preroll_last, supplied_first, supplied_last;
    std::optional<std::int64_t> fed_first, fed_last, window_first, window_last;
    bool shortfall = false;
    bool horizon = false;
};

void expect_ok(const SelectedPrimaryPlan& p, const Expect& x) {
    CHECK(p.status == SelectedPlanStatus::Ok);
    CHECK(p.option.empty());
    CHECK(p.bound == -1);
    CHECK(p.supplied_input_bars == x.supplied_input);
    CHECK(p.supplied_script_bars == x.supplied_script);
    CHECK(p.available_script_bars == x.available);
    CHECK(p.used_script_bars == x.used);
    CHECK(p.trimmed_script_bars == x.trimmed);
    CHECK(p.trim_index == x.trim_index);
    CHECK(p.fed_input_bars == x.fed_input);
    CHECK(p.fed_script_bars == x.fed_script);
    CHECK(p.preroll_input_bars == x.preroll_input);
    CHECK(p.window_input_bars == x.window_input);
    CHECK(p.window_script_bars == x.window_script);
    CHECK(p.trim_start_ms == x.trim_start);
    CHECK(p.preroll_first_bar_ms == x.preroll_first);
    CHECK(p.preroll_last_bar_ms == x.preroll_last);
    CHECK(p.supplied_first_data_ms == x.supplied_first);
    CHECK(p.supplied_last_data_ms == x.supplied_last);
    CHECK(p.fed_first_data_ms == x.fed_first);
    CHECK(p.fed_last_data_ms == x.fed_last);
    CHECK(p.window_first_data_ms == x.window_first);
    CHECK(p.window_last_data_ms == x.window_last);
    CHECK(p.shortfall == x.shortfall);
    CHECK(p.complete_pending_preroll_at_horizon == x.horizon);
    // The frozen equalities, independent of the expected values above.
    CHECK(p.supplied_input_bars == p.trim_index + p.fed_input_bars);
    CHECK(p.supplied_script_bars == p.available_script_bars + p.window_script_bars);
    CHECK(p.fed_input_bars == p.preroll_input_bars + p.window_input_bars);
    CHECK(p.fed_script_bars == p.used_script_bars + p.window_script_bars);
    CHECK(p.available_script_bars == p.used_script_bars + p.trimmed_script_bars);
}

void expect_refused(const SelectedPrimaryPlan& p, SelectedPlanStatus status, const char* option,
                    int bound = -1, std::int64_t value = 0, std::int64_t previous = 0,
                    std::int64_t next = 0) {
    CHECK(p.status == status);
    CHECK(p.option == option);
    CHECK(p.bound == bound);
    CHECK(p.value_ms == value);
    CHECK(p.previous_boundary_ms == previous);
    CHECK(p.next_boundary_ms == next);
    CHECK(p.supplied_input_bars == 0);
    CHECK(p.fed_input_bars == 0);
    CHECK(p.window_script_bars == 0);
    CHECK(!p.shortfall);
    CHECK(!p.complete_pending_preroll_at_horizon);
    CHECK(!p.preroll_first_bar_ms.has_value());
    CHECK(!p.supplied_first_data_ms.has_value());
}

// Rows 00:00..05:59 every minute (six hourly groups), T = 03:00, E = 06:00,
// F = 00:00, N = 2: hours 0-2 are before T, hours 3-5 are the window.
std::vector<Bar> full_six_hours() { return span(kBase, kBase + 6 * kHour, kMin); }

SelectedPlanRequest full_six_hours_request(std::uint32_t preroll) {
    return make_request(kBase + 3 * kHour, kBase + 6 * kHour, kBase, preroll, "1", "60");
}

Expect expect_full_six_hours_n2() {
    Expect x;
    x.supplied_input = 360;
    x.supplied_script = 6;  // 3 available + 3 window
    x.available = 3;
    x.used = 2;
    x.trimmed = 1;
    x.trim_index = 60;  // the first row of hour 1
    x.fed_input = 300;
    x.fed_script = 5;
    x.preroll_input = 120;
    x.window_input = 180;
    x.window_script = 3;
    x.trim_start = kBase + kHour;
    x.preroll_first = kBase + kHour;
    x.preroll_last = kBase + 2 * kHour;
    x.supplied_first = kBase;
    x.supplied_last = kBase + 6 * kHour - kMin;
    x.fed_first = kBase + kHour;
    x.fed_last = kBase + 6 * kHour - kMin;
    x.window_first = kBase + 3 * kHour;
    x.window_last = kBase + 6 * kHour - kMin;
    return x;
}

void fixture_full_1m_to_60m() {
    scenario = "1m to 60m, full span, N=2";
    const SelectedPrimaryPlan p = plan_of(full_six_hours(), full_six_hours_request(2));
    expect_ok(p, expect_full_six_hours_n2());
}

void fixture_n_zero() {
    scenario = "1m to 60m, N=0 keeps no pre-roll";
    const SelectedPrimaryPlan p = plan_of(full_six_hours(), full_six_hours_request(0));
    Expect x;
    x.supplied_input = 360;
    x.supplied_script = 6;
    x.available = 3;
    x.used = 0;
    x.trimmed = 3;
    x.trim_index = 180;  // the first row at T
    x.fed_input = 180;
    x.fed_script = 3;
    x.preroll_input = 0;
    x.window_input = 180;
    x.window_script = 3;
    x.trim_start = kBase + 3 * kHour;  // T itself
    x.supplied_first = kBase;
    x.supplied_last = kBase + 6 * kHour - kMin;
    x.fed_first = kBase + 3 * kHour;
    x.fed_last = kBase + 6 * kHour - kMin;
    x.window_first = kBase + 3 * kHour;
    x.window_last = kBase + 6 * kHour - kMin;
    expect_ok(p, x);
}

void fixture_n_above_available() {
    scenario = "1m to 60m, N=10 above the 3 available groups";
    const SelectedPrimaryPlan p = plan_of(full_six_hours(), full_six_hours_request(10));
    Expect x;
    x.supplied_input = 360;
    x.supplied_script = 6;
    x.available = 3;
    x.used = 3;
    x.trimmed = 0;
    x.trim_index = 0;
    x.fed_input = 360;
    x.fed_script = 6;
    x.preroll_input = 180;
    x.window_input = 180;
    x.window_script = 3;
    x.trim_start = kBase;
    x.preroll_first = kBase;
    x.preroll_last = kBase + 2 * kHour;
    x.supplied_first = kBase;
    x.supplied_last = kBase + 6 * kHour - kMin;
    x.fed_first = kBase;
    x.fed_last = kBase + 6 * kHour - kMin;
    x.window_first = kBase + 3 * kHour;
    x.window_last = kBase + 6 * kHour - kMin;
    x.shortfall = true;  // available < N
    expect_ok(p, x);
}

void fixture_full_1m_to_240m() {
    scenario = "1m to 240m, full span, N=3 above the 1 available group";
    const std::vector<Bar> rows = span(kBase, kBase + 12 * kHour, kMin);  // 720 rows
    const SelectedPlanRequest request =
        make_request(kBase + 4 * kHour, kBase + 12 * kHour, kBase, 3, "1", "240");
    Expect x;
    x.supplied_input = 720;
    x.supplied_script = 3;  // [00,04) before T, [04,08) and [08,12) in the window
    x.available = 1;
    x.used = 1;
    x.trimmed = 0;
    x.trim_index = 0;
    x.fed_input = 720;
    x.fed_script = 3;
    x.preroll_input = 240;
    x.window_input = 480;
    x.window_script = 2;
    x.trim_start = kBase;
    x.preroll_first = kBase;
    x.preroll_last = kBase;
    x.supplied_first = kBase;
    x.supplied_last = kBase + 12 * kHour - kMin;
    x.fed_first = kBase;
    x.fed_last = kBase + 12 * kHour - kMin;
    x.window_first = kBase + 4 * kHour;
    x.window_last = kBase + 12 * kHour - kMin;
    x.shortfall = true;
    expect_ok(plan_of(rows, request), x);
}

void fixture_sparse_1m_to_60m() {
    scenario = "1m to 60m, sparse: a missing hour, partial hours, F above the first slot";
    // Hour 1 minutes 00-29 (before T, left pending, sealed by the next group),
    // hour 2 absent (no group), hour 3 full, hour 4 minutes 10-12 (sealed by
    // hour 5's first row, not by its own last minute), hour 5 full.
    std::vector<Bar> rows;
    add_span(rows, kBase + kHour, kBase + kHour + 30 * kMin, kMin);
    add_span(rows, kBase + 3 * kHour, kBase + 4 * kHour, kMin);
    add_span(rows, kBase + 4 * kHour + 10 * kMin, kBase + 4 * kHour + 13 * kMin, kMin);
    add_span(rows, kBase + 5 * kHour, kBase + 6 * kHour, kMin);
    const SelectedPlanRequest request =
        make_request(kBase + 3 * kHour, kBase + 6 * kHour, kBase + kHour, 2, "1", "60");
    Expect x;
    x.supplied_input = 153;
    x.supplied_script = 4;  // 1 available + 3 window groups
    x.available = 1;
    x.used = 1;
    x.trimmed = 0;
    x.trim_index = 0;
    x.fed_input = 153;
    x.fed_script = 4;
    x.preroll_input = 30;
    x.window_input = 123;
    x.window_script = 3;
    x.trim_start = kBase + kHour;
    x.preroll_first = kBase + kHour;
    x.preroll_last = kBase + kHour;
    x.supplied_first = kBase + kHour;
    x.supplied_last = kBase + 6 * kHour - kMin;
    x.fed_first = kBase + kHour;
    x.fed_last = kBase + 6 * kHour - kMin;
    x.window_first = kBase + 3 * kHour;
    x.window_last = kBase + 6 * kHour - kMin;
    x.shortfall = true;  // 1 available < 2
    expect_ok(plan_of(rows, request), x);
}

void fixture_sparse_1m_to_240m() {
    scenario = "1m to 240m, sparse rows, last group sealed at 15:59, N=1 trims one group";
    // [00,04): 00:00 and 03:59 (sealed by its last minute); [04,08): 05:30
    // (pending, sealed by the next group); [08,12): 09:10 and 09:11 (sealed by
    // 13:00); [12,16): 13:00 and 15:59 (sealed by its last minute).
    const std::vector<Bar> rows = rows_at({kBase, kBase + 3 * kHour + 59 * kMin,
                                           kBase + 5 * kHour + 30 * kMin,
                                           kBase + 9 * kHour + 10 * kMin,
                                           kBase + 9 * kHour + 11 * kMin, kBase + 13 * kHour,
                                           kBase + 15 * kHour + 59 * kMin});
    const SelectedPlanRequest request =
        make_request(kBase + 8 * kHour, kBase + 16 * kHour, kBase, 1, "1", "240");
    Expect x;
    x.supplied_input = 7;
    x.supplied_script = 4;
    x.available = 2;
    x.used = 1;
    x.trimmed = 1;
    x.trim_index = 2;  // the first row of [04,08)
    x.fed_input = 5;
    x.fed_script = 3;
    x.preroll_input = 1;
    x.window_input = 4;
    x.window_script = 2;
    x.trim_start = kBase + 4 * kHour;
    x.preroll_first = kBase + 4 * kHour;
    x.preroll_last = kBase + 4 * kHour;
    x.supplied_first = kBase;
    x.supplied_last = kBase + 15 * kHour + 59 * kMin;
    x.fed_first = kBase + 5 * kHour + 30 * kMin;
    x.fed_last = kBase + 15 * kHour + 59 * kMin;
    x.window_first = kBase + 9 * kHour + 10 * kMin;
    x.window_last = kBase + 15 * kHour + 59 * kMin;
    expect_ok(plan_of(rows, request), x);
}

void fixture_passthrough_raw() {
    scenario = "input = script (5m), FeedTolerant raw partition and the canonical path";
    const std::vector<Bar> rows = span(kBase, kBase + 60 * kMin, 5 * kMin);  // 12 rows
    for (const bool tolerant : {true, false}) {
        SelectedPlanRequest request =
            make_request(kBase + 30 * kMin, kBase + 60 * kMin, kBase, 4, "5", "5");
        request.feed_tolerant = tolerant;
        Expect x;
        x.supplied_input = 12;
        x.supplied_script = 12;  // every row is its own sealed bar
        x.available = 6;
        x.used = 4;
        x.trimmed = 2;
        x.trim_index = 2;
        x.fed_input = 10;
        x.fed_script = 10;
        x.preroll_input = 4;
        x.window_input = 6;
        x.window_script = 6;
        x.trim_start = kBase + 10 * kMin;
        x.preroll_first = kBase + 10 * kMin;
        x.preroll_last = kBase + 25 * kMin;
        x.supplied_first = kBase;
        x.supplied_last = kBase + 55 * kMin;
        x.fed_first = kBase + 10 * kMin;
        x.fed_last = kBase + 55 * kMin;
        x.window_first = kBase + 30 * kMin;
        x.window_last = kBase + 55 * kMin;
        expect_ok(plan_of(rows, request), x);
    }
}

void fixture_off_grid_raw_labels() {
    scenario = "FeedTolerant 5m passthrough on off-grid rows: the raw label is the row's own";
    const std::vector<Bar> rows =
        rows_at({kBase + 1 * kMin, kBase + 7 * kMin, kBase + 31 * kMin, kBase + 44 * kMin});
    const SelectedPlanRequest request =
        make_request(kBase + 30 * kMin, kBase + 60 * kMin, kBase, 1, "5", "5");
    const SelectedPrimaryPlan p = plan_of(rows, request);
    CHECK(p.status == SelectedPlanStatus::Ok);
    CHECK(p.available_script_bars == 2);
    CHECK(p.used_script_bars == 1);
    CHECK(p.trim_index == 1);
    CHECK(p.window_script_bars == 2);
    CHECK(p.window_input_bars == 2);
    CHECK(p.preroll_input_bars == 1);
    CHECK(p.trim_start_ms == kBase + 7 * kMin);
    CHECK(p.preroll_first_bar_ms == std::optional<std::int64_t>(kBase + 7 * kMin));
    CHECK(p.preroll_last_bar_ms == std::optional<std::int64_t>(kBase + 7 * kMin));
    CHECK(p.fed_first_data_ms == std::optional<std::int64_t>(kBase + 7 * kMin));
}

void fixture_empty_feed() {
    scenario = "empty feed";
    const SelectedPlanRequest request = full_six_hours_request(3);
    const SelectedPrimaryPlan p = pineforge::plan_selected_primary(nullptr, 0, request);
    Expect x;
    x.trim_start = kBase + 3 * kHour;  // no kept group: T itself
    x.shortfall = true;                // requested 3, available 0
    expect_ok(p, x);

    const SelectedPrimaryPlan zero =
        pineforge::plan_selected_primary(nullptr, 0, full_six_hours_request(0));
    Expect y;
    y.trim_start = kBase + 3 * kHour;
    expect_ok(zero, y);

    // A non-null array of count 0 is the same empty feed.
    const std::vector<Bar> none;
    expect_ok(plan_of(none, request), x);
}

void fixture_no_window_pending_preroll() {
    scenario = "no window row: a pending pre-T group needs completion at the horizon";
    // Hour 1 full (sealed by its last minute), hour 2 minutes 00-29 (pending
    // when the rows end; T = 03:00 is never reached).
    const std::vector<Bar> rows = span(kBase + kHour, kBase + 2 * kHour + 30 * kMin, kMin);
    Expect x;
    x.supplied_input = 90;
    x.supplied_script = 2;
    x.available = 2;
    x.used = 2;
    x.trimmed = 0;
    x.trim_index = 0;
    x.fed_input = 90;
    x.fed_script = 2;
    x.preroll_input = 90;
    x.window_input = 0;
    x.window_script = 0;
    x.trim_start = kBase + kHour;
    x.preroll_first = kBase + kHour;
    x.preroll_last = kBase + 2 * kHour;
    x.supplied_first = kBase + kHour;
    x.supplied_last = kBase + 2 * kHour + 29 * kMin;
    x.fed_first = kBase + kHour;
    x.fed_last = kBase + 2 * kHour + 29 * kMin;
    x.horizon = true;
    expect_ok(plan_of(rows, full_six_hours_request(2)), x);

    // N=0 keeps no group, so there is nothing to complete.
    const SelectedPrimaryPlan none = plan_of(rows, full_six_hours_request(0));
    CHECK(none.status == SelectedPlanStatus::Ok);
    CHECK(none.used_script_bars == 0);
    CHECK(none.trim_index == 90);
    CHECK(none.fed_input_bars == 0);
    CHECK(none.fed_script_bars == 0);
    CHECK(!none.fed_first_data_ms.has_value());
    CHECK(!none.complete_pending_preroll_at_horizon);

    // A pre-T group its own last minute sealed is not pending.
    const std::vector<Bar> sealed = span(kBase + kHour, kBase + 3 * kHour, kMin);
    const SelectedPrimaryPlan done = plan_of(sealed, full_six_hours_request(2));
    CHECK(done.status == SelectedPlanStatus::Ok);
    CHECK(done.used_script_bars == 2);
    CHECK(done.window_input_bars == 0);
    CHECK(!done.complete_pending_preroll_at_horizon);
}

void fixture_unsealed_window_tail() {
    scenario = "the ordinary unsealed window tail is not an executed window callback";
    // Hours 2, 3 and 4 full, hour 5 minutes 00-29 only. T = 03:00, E = 06:00.
    const std::vector<Bar> rows = span(kBase + 2 * kHour, kBase + 5 * kHour + 30 * kMin, kMin);
    const SelectedPlanRequest request =
        make_request(kBase + 3 * kHour, kBase + 6 * kHour, kBase + 2 * kHour, 1, "1", "60");
    Expect x;
    x.supplied_input = 210;
    x.supplied_script = 3;  // 1 available + 2 executed window bars
    x.available = 1;
    x.used = 1;
    x.trimmed = 0;
    x.trim_index = 0;
    x.fed_input = 210;
    x.fed_script = 3;
    x.preroll_input = 60;
    x.window_input = 150;  // the tail's 30 rows still count as window rows
    x.window_script = 2;   // hour 5 never seals
    x.trim_start = kBase + 2 * kHour;
    x.preroll_first = kBase + 2 * kHour;
    x.preroll_last = kBase + 2 * kHour;
    x.supplied_first = kBase + 2 * kHour;
    x.supplied_last = kBase + 5 * kHour + 29 * kMin;
    x.fed_first = kBase + 2 * kHour;
    x.fed_last = kBase + 5 * kHour + 29 * kMin;
    x.window_first = kBase + 3 * kHour;
    x.window_last = kBase + 5 * kHour + 29 * kMin;
    expect_ok(plan_of(rows, request), x);
}

void fixture_closing_seal_clipped_session() {
    scenario = "session 0930-1600: the final input reaching the last traded close seals";
    const std::int64_t open = kBase + 9 * kHour + 30 * kMin;
    // 09:30..15:59 every minute: groups open at 09:30, 10:30, ..., 15:30; the
    // 15:30 group's nominal end is 16:30 but trading ends at 16:00.
    std::vector<Bar> rows = span(open, kBase + 16 * kHour, kMin);  // 390 rows
    SelectedPlanRequest request =
        make_request(open + 5 * kHour, open + 24 * kHour, open, 1, "1", "60");
    request.session = "0930-1600";
    Expect x;
    x.supplied_input = 390;
    x.supplied_script = 7;  // 5 available + 2 window bars
    x.available = 5;
    x.used = 1;
    x.trimmed = 4;
    x.trim_index = 240;
    x.fed_input = 150;
    x.fed_script = 3;
    x.preroll_input = 60;
    x.window_input = 90;
    x.window_script = 2;  // 14:30 by its last minute, 15:30 by the closing rule
    x.trim_start = open + 4 * kHour;
    x.preroll_first = open + 4 * kHour;
    x.preroll_last = open + 4 * kHour;
    x.supplied_first = open;
    x.supplied_last = kBase + 16 * kHour - kMin;
    x.fed_first = open + 4 * kHour;
    x.fed_last = kBase + 16 * kHour - kMin;
    x.window_first = open + 5 * kHour;
    x.window_last = kBase + 16 * kHour - kMin;
    expect_ok(plan_of(rows, request), x);

    // Without the 15:59 row the final input stops short of 16:00: unsealed.
    rows.pop_back();
    Expect y = x;
    y.supplied_input = 389;
    y.supplied_script = 6;
    y.fed_input = 149;
    y.fed_script = 2;
    y.window_input = 89;
    y.window_script = 1;
    y.supplied_last = kBase + 16 * kHour - 2 * kMin;
    y.fed_last = kBase + 16 * kHour - 2 * kMin;
    y.window_last = kBase + 16 * kHour - 2 * kMin;
    expect_ok(plan_of(rows, request), y);
}

void fixture_all_eight_primary_tokens() {
    // Input 1m over 00:00..15:59, T = 08:00, E = 16:00, N = 2: every token's
    // grid divides 480 minutes, so each hour/minute count is exact.
    static const struct {
        const char* token;
        std::int64_t minutes;
    } kTokens[] = {{"1", 1}, {"3", 3},   {"5", 5},     {"15", 15},
                   {"30", 30}, {"60", 60}, {"120", 120}, {"240", 240}};
    const std::vector<Bar> rows = span(kBase, kBase + 16 * kHour, kMin);  // 960 rows
    for (const bool tolerant : {true, false}) {
        for (const auto& entry : kTokens) {
            scenario = entry.token;
            SelectedPlanRequest request =
                make_request(kBase + 8 * kHour, kBase + 16 * kHour, kBase, 2, "1", entry.token);
            request.feed_tolerant = tolerant;
            const std::int64_t m = entry.minutes;
            const std::uint64_t groups = static_cast<std::uint64_t>(480 / m);
            Expect x;
            x.supplied_input = 960;
            x.supplied_script = 2 * groups;
            x.available = groups;
            x.used = 2;
            x.trimmed = groups - 2;
            x.trim_index = static_cast<std::uint64_t>(480 - 2 * m);
            x.fed_input = static_cast<std::uint64_t>(480 + 2 * m);
            x.fed_script = 2 + groups;
            x.preroll_input = static_cast<std::uint64_t>(2 * m);
            x.window_input = 480;
            x.window_script = groups;
            x.trim_start = kBase + (480 - 2 * m) * kMin;
            x.preroll_first = x.trim_start;
            x.preroll_last = kBase + (480 - m) * kMin;
            x.supplied_first = kBase;
            x.supplied_last = kBase + 16 * kHour - kMin;
            x.fed_first = x.trim_start;
            x.fed_last = x.supplied_last;
            x.window_first = kBase + 8 * kHour;
            x.window_last = x.supplied_last;
            expect_ok(plan_of(rows, request), x);
        }
    }
}

void fixture_utc_aliases() {
    scenario = "UTC aliases admit pairwise";
    const char* const names[] = {"", "UTC", "GMT", "Etc/UTC", "Etc/GMT"};
    for (const char* chart : names) {
        for (const char* engine : names) {
            SelectedPlanRequest request = full_six_hours_request(2);
            request.chart_timezone = chart;
            request.engine_timezone = engine;
            expect_ok(plan_of(full_six_hours(), request), expect_full_six_hours_n2());
        }
    }
}

void fixture_same_non_utc_zone() {
    // Needs the host's tzdata for America/New_York. January is standard time
    // all day, so the hourly grid stays on the UTC hours and the counts are
    // the UTC fixture's.
    scenario = "the same non-UTC zone admits (needs tzdata)";
    SelectedPlanRequest request = full_six_hours_request(2);
    request.chart_timezone = "America/New_York";
    request.engine_timezone = "America/New_York";
    expect_ok(plan_of(full_six_hours(), request), expect_full_six_hours_n2());
}

void fixture_clock_refusals() {
    scenario = "distinct clock identities refuse";
    struct Pair {
        const char* chart;
        const char* engine;
    };
    // Refused at the identity comparison, before any tzdata is read.
    static const Pair kMismatches[] = {
        {"UTC", "America/New_York"}, {"America/New_York", "UTC"},
        {"US/Eastern", "America/New_York"}, {"Etc/UTC", "Asia/Tokyo"},
        {" UTC", "UTC"}, {"utc", "UTC"}, {"UTC+0", "UTC"}, {"Etc/GMT+0", "UTC"}};
    for (const Pair& pair : kMismatches) {
        SelectedPlanRequest request = full_six_hours_request(2);
        request.chart_timezone = pair.chart;
        request.engine_timezone = pair.engine;
        expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::CalendarUnsupported,
                       "timezone");
    }

    scenario = "equal clock tokens do not excuse invalid grammar";
    SelectedPlanRequest zone = full_six_hours_request(2);
    zone.chart_timezone = "Not/AZone";
    zone.engine_timezone = "Not/AZone";
    expect_refused(plan_of(full_six_hours(), zone), SelectedPlanStatus::CalendarUnsupported,
                   "engine_timezone");

    SelectedPlanRequest session = full_six_hours_request(2);
    session.session = "garbage";
    expect_refused(plan_of(full_six_hours(), session), SelectedPlanStatus::CalendarUnsupported,
                   "session");
    session.session = "2500-0100";
    expect_refused(plan_of(full_six_hours(), session), SelectedPlanStatus::CalendarUnsupported,
                   "session");
}

void fixture_request_refusals() {
    scenario = "primary token and input timeframe";
    const char* const bad_scripts[] = {"",   "D",  "1D", "W", "M",   "2",   "7",
                                       "45", "1H", "5S", " 5", "05", "360", "15S"};
    for (const char* token : bad_scripts) {
        SelectedPlanRequest request = full_six_hours_request(2);
        request.script_tf = token;
        expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                       "script_tf");
    }
    const char* const bad_inputs[] = {"", "1H", "01"};
    for (const char* token : bad_inputs) {
        SelectedPlanRequest request = full_six_hours_request(2);
        request.input_tf = token;
        expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                       "input_tf");
    }
    // Valid grammar, a pairing the consumer does not group.
    const char* const bad_pairs[] = {"D", "7", "120"};
    for (const char* token : bad_pairs) {
        SelectedPlanRequest request = full_six_hours_request(2);
        request.input_tf = token;
        expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::CalendarUnsupported,
                       "timeframes");
    }

    scenario = "i53 bounds, order, pre-roll cap and the row array";
    const std::int64_t over = kI53 + 1;
    SelectedPlanRequest request = full_six_hours_request(2);
    request.start_ms = over;
    expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "start_ms", 0, over);
    request = full_six_hours_request(2);
    request.end_ms = over;
    expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid, "end_ms",
                   1, over);
    request = full_six_hours_request(2);
    request.fed_start_ms = -over;
    expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "fed_start_ms", 2, -over);

    request = full_six_hours_request(2);
    request.end_ms = request.start_ms;  // T == E
    expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid, "end_ms",
                   1, request.end_ms, request.start_ms);
    request = full_six_hours_request(2);
    request.end_ms = request.start_ms - kHour;  // T > E
    expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid, "end_ms",
                   1, request.end_ms, request.start_ms);
    request = full_six_hours_request(2);
    request.fed_start_ms = request.start_ms + kHour;  // F > T
    expect_refused(plan_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "fed_start_ms", 2, request.fed_start_ms, 0, request.start_ms);

    expect_refused(plan_of(full_six_hours(), full_six_hours_request(5001)),
                   SelectedPlanStatus::RequestInvalid, "preroll_bars", -1, 5001);
    const SelectedPrimaryPlan cap = pineforge::plan_selected_primary(
        nullptr, 0, full_six_hours_request(5000));
    CHECK(cap.status == SelectedPlanStatus::Ok);
    CHECK(cap.shortfall);

    expect_refused(pineforge::plan_selected_primary(nullptr, 3, full_six_hours_request(2)),
                   SelectedPlanStatus::RequestInvalid, "rows");
}

void fixture_boundary_refusals() {
    scenario = "T, E and F must each be the nominal open of the native script interval";
    const std::vector<Bar> rows = full_six_hours();
    SelectedPlanRequest request = full_six_hours_request(2);
    request.start_ms = kBase + 3 * kHour + kMin;
    expect_refused(plan_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "start_ms", 0,
                   kBase + 3 * kHour + kMin, kBase + 3 * kHour, kBase + 4 * kHour);

    request = full_six_hours_request(2);
    request.end_ms = kBase + 6 * kHour + 5 * kMin;
    expect_refused(plan_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "end_ms", 1,
                   kBase + 6 * kHour + 5 * kMin, kBase + 6 * kHour, kBase + 7 * kHour);

    request = full_six_hours_request(2);
    request.fed_start_ms = kBase + kHour + kMin;
    expect_refused(plan_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "fed_start_ms", 2,
                   kBase + kHour + kMin, kBase + kHour, kBase + 2 * kHour);

    // T is checked before E and F.
    request = full_six_hours_request(2);
    request.start_ms = kBase + 3 * kHour + kMin;
    request.end_ms = kBase + 6 * kHour + 5 * kMin;
    const SelectedPrimaryPlan first = plan_of(rows, request);
    CHECK(first.status == SelectedPlanStatus::BoundaryUnaligned);
    CHECK(first.bound == 0);

    // 240m: 03:00 is on the hour grid but not on the 4-hour grid.
    request = make_request(kBase + 3 * kHour, kBase + 8 * kHour, kBase, 1, "1", "240");
    expect_refused(plan_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "start_ms", 0,
                   kBase + 3 * kHour, kBase, kBase + 4 * kHour);
}

void fixture_row_refusals() {
    scenario = "rows must strictly increase and lie in F <= timestamp < E";
    const SelectedPlanRequest request = full_six_hours_request(2);

    expect_refused(plan_of(rows_at({kBase + 3 * kHour, kBase + 3 * kHour + 2 * kMin,
                                    kBase + 3 * kHour + kMin}),
                           request),
                   SelectedPlanStatus::RowsUnordered, "rows", -1, kBase + 3 * kHour + kMin,
                   kBase + 3 * kHour + 2 * kMin);
    expect_refused(plan_of(rows_at({kBase + kHour, kBase + kHour}), request),
                   SelectedPlanStatus::RowsUnordered, "rows", -1, kBase + kHour, kBase + kHour);

    expect_refused(plan_of(rows_at({kBase - kMin, kBase}), request),
                   SelectedPlanStatus::FeedRangeInvalid, "rows", 2, kBase - kMin, kBase,
                   kBase + 6 * kHour);
    expect_refused(plan_of(rows_at({kBase + 5 * kHour, kBase + 6 * kHour}), request),
                   SelectedPlanStatus::FeedRangeInvalid, "rows", 1, kBase + 6 * kHour, kBase,
                   kBase + 6 * kHour);
    // One row short of E is admitted.
    const SelectedPrimaryPlan edge =
        plan_of(rows_at({kBase, kBase + 6 * kHour - kMin}), request);
    CHECK(edge.status == SelectedPlanStatus::Ok);
    CHECK(edge.supplied_input_bars == 2);
}

}  // namespace

int main() {
    fixture_full_1m_to_60m();
    fixture_n_zero();
    fixture_n_above_available();
    fixture_full_1m_to_240m();
    fixture_sparse_1m_to_60m();
    fixture_sparse_1m_to_240m();
    fixture_passthrough_raw();
    fixture_off_grid_raw_labels();
    fixture_empty_feed();
    fixture_no_window_pending_preroll();
    fixture_unsealed_window_tail();
    fixture_closing_seal_clipped_session();
    fixture_all_eight_primary_tokens();
    fixture_utc_aliases();
    fixture_same_non_utc_zone();
    fixture_clock_refusals();
    fixture_request_refusals();
    fixture_boundary_refusals();
    fixture_row_refusals();
    if (failures != 0) {
        std::fprintf(stderr, "test_selected_primary_plan: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_selected_primary_plan: all checks passed\n");
    return 0;
}
