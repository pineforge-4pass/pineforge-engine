// Selected retained primary count fixtures (pineforge::count_selected_retained_primary).
//
// Every expected value below is derived by hand from the count's contract
// (include/pineforge/selected_window_plan.hpp), the frozen planner semantics and
// the native consumer's grouping and seal rules. The source adapter reads the
// executed count instead (count_selected_executed_primary, from
// PineScheduler::run_begin): the same count with E unchecked and unbounded, held
// to this count's answers by fixture_executed_count_ignores_e below. The native
// observed-counter equality on that route is ROUTES-PINE-TF-PREROLL in
// test_selected_window_routes.cpp, not this file.
//
// Black box through the public call only: rows in, count out. The count reads
// timestamps only, so the prices are inert. Clocks are UTC 24x7 except where a
// row says otherwise, so the expected groups are plain hour/minute arithmetic
// from kBase. One fixture needs the host's tzdata (America/New_York) and says so.
//
// The count is the groups that open before T (each is executed, a last pending
// one by the completion at the accepted T) plus the window groups the consumer's
// seal rules execute: an input that reaches the script interval's next period
// open, the first input of a later group, and at the end of the batch a final
// input that reaches the script interval's last traded close. A final window
// group none of them seals adds nothing. 1m input unless stated.
//
// Every counting fixture also states the planner equality: the rows
// plan_selected_primary keeps (rows from its trim_index), counted here, give
// that plan's fed_script_bars, for every pre-roll request from N = 0 through
// past the available groups (the shortfall). One refusal is not constructed by
// any fixture: a group holding rows on both sides of T cannot arise on aligned
// admitted clocks, so CalendarUnsupported on start_ms is shared code only.

#include <pineforge/selected_window_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using pineforge::Bar;
using pineforge::SelectedPlanRequest;
using pineforge::SelectedPlanStatus;
using pineforge::SelectedPrimaryPlan;
using pineforge::SelectedRetainedCount;
using pineforge::SelectedRetainedRequest;

static_assert(std::is_same<decltype(&pineforge::count_selected_retained_primary),
                           SelectedRetainedCount (*)(const Bar*, std::size_t,
                                                     const SelectedRetainedRequest&)>::value,
              "the decided call signature");
static_assert(std::is_same<decltype(SelectedRetainedRequest::start_ms), std::int64_t>::value,
              "T is a signed 64-bit value");
static_assert(std::is_same<decltype(SelectedRetainedRequest::end_ms), std::int64_t>::value,
              "E is a signed 64-bit value");
static_assert(std::is_same<decltype(SelectedRetainedCount::script_bars), std::uint64_t>::value,
              "the count is unsigned 64-bit");
static_assert(std::is_same<decltype(SelectedRetainedCount::status), SelectedPlanStatus>::value,
              "the count reports the planner's status");

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

SelectedRetainedRequest make_request(std::int64_t start, std::int64_t end, const char* input_tf,
                                     const char* script_tf) {
    SelectedRetainedRequest request{};
    request.start_ms = start;
    request.end_ms = end;
    request.input_tf = input_tf;
    request.script_tf = script_tf;
    request.chart_timezone = "UTC";
    request.engine_timezone = "UTC";
    request.session = "";
    return request;
}

// T = 03:00, E = 06:00 on the hourly script grid: hours 0-2 before T, 3-5 window.
SelectedRetainedRequest six_hour_request() {
    return make_request(kBase + 3 * kHour, kBase + 6 * kHour, "1", "60");
}

std::vector<Bar> full_six_hours() { return span(kBase, kBase + 6 * kHour, kMin); }

SelectedRetainedCount count_of(const std::vector<Bar>& rows, const SelectedRetainedRequest& request) {
    return pineforge::count_selected_retained_primary(rows.data(), rows.size(), request);
}

SelectedRetainedCount executed_of(const std::vector<Bar>& rows,
                                  const SelectedRetainedRequest& request) {
    return pineforge::count_selected_executed_primary(rows.data(), rows.size(), request);
}

void expect_count(const SelectedRetainedCount& counted, std::uint64_t bars) {
    CHECK(counted.status == SelectedPlanStatus::Ok);
    CHECK(counted.option.empty());
    CHECK(counted.script_bars == bars);
}

void expect_refused(const SelectedRetainedCount& counted, SelectedPlanStatus status,
                    const char* option) {
    CHECK(counted.status == status);
    CHECK(counted.option == option);
    CHECK(counted.script_bars == 0);
}

SelectedPlanRequest plan_request_of(const SelectedRetainedRequest& request, std::int64_t fed_start,
                                    std::uint32_t preroll) {
    SelectedPlanRequest plan{};
    plan.start_ms = request.start_ms;
    plan.end_ms = request.end_ms;
    plan.fed_start_ms = fed_start;
    plan.preroll_bars = preroll;
    plan.input_tf = request.input_tf;
    plan.script_tf = request.script_tf;
    plan.chart_timezone = request.chart_timezone;
    plan.engine_timezone = request.engine_timezone;
    plan.session = request.session;
    plan.feed_tolerant = request.feed_tolerant;
    return plan;
}

// The rows plan_selected_primary keeps are the ones a native caller has already
// retained: the count over them is that plan's fed_script_bars.
void expect_equals_plan(const std::vector<Bar>& rows, const SelectedRetainedRequest& request,
                        std::int64_t fed_start, std::uint32_t preroll) {
    const SelectedPrimaryPlan plan = pineforge::plan_selected_primary(
        rows.data(), rows.size(), plan_request_of(request, fed_start, preroll));
    CHECK(plan.status == SelectedPlanStatus::Ok);
    if (plan.status != SelectedPlanStatus::Ok) return;
    const std::size_t skipped = static_cast<std::size_t>(plan.trim_index);
    const SelectedRetainedCount counted = pineforge::count_selected_retained_primary(
        rows.data() + skipped, rows.size() - skipped, request);
    CHECK(counted.status == SelectedPlanStatus::Ok);
    CHECK(counted.option.empty());
    CHECK(counted.script_bars == plan.fed_script_bars);
}

// Every pre-roll request from N = 0 through `last_preroll`, which each fixture
// sets past its available groups so the shortfall is covered too.
void expect_equals_plan_for_n(const std::vector<Bar>& rows, const SelectedRetainedRequest& request,
                              std::int64_t fed_start, std::uint32_t last_preroll) {
    for (std::uint32_t preroll = 0; preroll <= last_preroll; ++preroll) {
        expect_equals_plan(rows, request, fed_start, preroll);
    }
}

void fixture_full_six_hours() {
    scenario = "1m to 60m, six hourly groups";
    const std::vector<Bar> rows = full_six_hours();
    const SelectedRetainedRequest request = six_hour_request();
    expect_count(count_of(rows, request), 6);  // 3 groups before T + 3 window groups
    expect_equals_plan_for_n(rows, request, kBase, 6);

    scenario = "retained rows may begin inside a group";
    const std::vector<Bar> mid = span(kBase + 30 * kMin, kBase + 6 * kHour, kMin);
    expect_count(count_of(mid, request), 6);  // hour 0 is a group of 30 rows, sealed by 00:59
    expect_equals_plan_for_n(mid, request, kBase, 6);

    scenario = "no F boundary: a row below any planner floor is retained prehistory";
    // 23:59 the day before seals its own hour; 00:00 opens a pre-T group that
    // nothing seals and the horizon completes: two groups.
    expect_count(count_of(rows_at({kBase - kMin, kBase}), request), 2);
}

void fixture_all_eight_primary_tokens() {
    // Input 1m over 00:00..15:59, T = 08:00, E = 16:00: every token's grid
    // divides 480 minutes, so each side holds 480 / m groups, all sealed (a
    // window group by its own last minute reaching 16:00 or the next period open).
    static const struct {
        const char* token;
        std::int64_t minutes;
    } kTokens[] = {{"1", 1}, {"3", 3},   {"5", 5},     {"15", 15},
                   {"30", 30}, {"60", 60}, {"120", 120}, {"240", 240}};
    const std::vector<Bar> rows = span(kBase, kBase + 16 * kHour, kMin);  // 960 rows
    for (const bool tolerant : {true, false}) {
        for (const auto& entry : kTokens) {
            scenario = entry.token;
            SelectedRetainedRequest request =
                make_request(kBase + 8 * kHour, kBase + 16 * kHour, "1", entry.token);
            request.feed_tolerant = tolerant;
            const std::uint64_t groups = static_cast<std::uint64_t>(480 / entry.minutes);
            expect_count(count_of(rows, request), 2 * groups);
            expect_equals_plan(rows, request, kBase, 2);
        }
    }
}

void fixture_passthrough() {
    scenario = "input = script (5m), FeedTolerant raw partition and the canonical path";
    const std::vector<Bar> rows = span(kBase, kBase + 60 * kMin, 5 * kMin);  // 12 rows
    for (const bool tolerant : {true, false}) {
        SelectedRetainedRequest request =
            make_request(kBase + 30 * kMin, kBase + 60 * kMin, "5", "5");
        request.feed_tolerant = tolerant;
        expect_count(count_of(rows, request), 12);  // every row is its own sealed bar
        expect_equals_plan_for_n(rows, request, kBase, 8);
    }

    scenario = "5m passthrough on off-grid rows: each row is its own bar";
    const std::vector<Bar> off_grid =
        rows_at({kBase + 1 * kMin, kBase + 7 * kMin, kBase + 31 * kMin, kBase + 44 * kMin});
    for (const bool tolerant : {true, false}) {
        SelectedRetainedRequest request =
            make_request(kBase + 30 * kMin, kBase + 60 * kMin, "5", "5");
        request.feed_tolerant = tolerant;
        expect_count(count_of(off_grid, request), 4);  // two before T, two in the window
        expect_equals_plan_for_n(off_grid, request, kBase, 4);
    }
}

void fixture_missing_last_pre_t_slot() {
    scenario = "1m to 60m: the last pre-T hour misses its final minute";
    // Hour 1 full, hour 2 minutes 00-58 (02:58's next period open is 02:59, short
    // of 03:00, so the hour stays pending until the first window row seals it),
    // hours 3-5 full.
    std::vector<Bar> rows;
    add_span(rows, kBase + kHour, kBase + 2 * kHour, kMin);
    add_span(rows, kBase + 2 * kHour, kBase + 3 * kHour - kMin, kMin);
    add_span(rows, kBase + 3 * kHour, kBase + 6 * kHour, kMin);
    const SelectedRetainedRequest request = six_hour_request();
    expect_count(count_of(rows, request), 5);  // 2 before T + 3 window groups
    expect_equals_plan_for_n(rows, request, kBase, 5);

    scenario = "1m to 240m: the last pre-T slot misses its final minute";
    // [00:00, 04:00) holds minutes 00:00-03:58 only; [04:00, 12:00) is full.
    std::vector<Bar> wide;
    add_span(wide, kBase, kBase + 4 * kHour - kMin, kMin);
    add_span(wide, kBase + 4 * kHour, kBase + 12 * kHour, kMin);
    const SelectedRetainedRequest wide_request =
        make_request(kBase + 4 * kHour, kBase + 12 * kHour, "1", "240");
    expect_count(count_of(wide, wide_request), 3);  // 1 before T + [04,08) + [08,12)
    expect_equals_plan_for_n(wide, wide_request, kBase, 4);
}

void fixture_no_window_pre_t_horizon() {
    scenario = "no window row: a pending pre-T group is completed at the accepted T";
    // Hour 1 full (sealed by its last minute), hour 2 minutes 00-29 (pending
    // when the rows end; T = 03:00 is never reached).
    const std::vector<Bar> rows = span(kBase + kHour, kBase + 2 * kHour + 30 * kMin, kMin);
    const SelectedRetainedRequest request = six_hour_request();
    expect_count(count_of(rows, request), 2);
    expect_equals_plan_for_n(rows, request, kBase, 4);

    scenario = "a lone pending pre-T minute is one group";
    const std::vector<Bar> lone = rows_at({kBase + 2 * kHour + 30 * kMin});
    expect_count(count_of(lone, request), 1);
    expect_equals_plan_for_n(lone, request, kBase, 3);

    scenario = "pre-T groups their own last minute sealed: nothing pending, same count";
    const std::vector<Bar> sealed = span(kBase + kHour, kBase + 3 * kHour, kMin);
    expect_count(count_of(sealed, request), 2);
    expect_equals_plan_for_n(sealed, request, kBase, 4);
}

void fixture_incomplete_window_tail() {
    scenario = "an unsealed window tail adds nothing";
    // Hours 2, 3 and 4 full, hour 5 minutes 00-29 only: one pre-T group and two
    // sealed window groups; hour 5 never seals.
    const std::vector<Bar> rows = span(kBase + 2 * kHour, kBase + 5 * kHour + 30 * kMin, kMin);
    const SelectedRetainedRequest request = six_hour_request();
    expect_count(count_of(rows, request), 3);
    expect_equals_plan_for_n(rows, request, kBase, 3);

    scenario = "a window group that has only just opened is not executed";
    std::vector<Bar> opened = span(kBase + 2 * kHour, kBase + 3 * kHour, kMin);
    opened.push_back(row_at(kBase + 3 * kHour));
    expect_count(count_of(opened, request), 1);
    expect_equals_plan_for_n(opened, request, kBase, 3);
}

void fixture_closing_rule_clipped_session() {
    scenario = "session 0930-1600: the final input reaching the last traded close seals";
    const std::int64_t open = kBase + 9 * kHour + 30 * kMin;
    // 09:30..15:59 every minute: groups open at 09:30, 10:30, ..., 15:30; the
    // 15:30 group's nominal end is 16:30 but trading ends at 16:00.
    std::vector<Bar> rows = span(open, kBase + 16 * kHour, kMin);  // 390 rows
    SelectedRetainedRequest request = make_request(open + 5 * kHour, open + 24 * kHour, "1", "60");
    request.session = "0930-1600";
    expect_count(count_of(rows, request), 7);  // 5 before T; 14:30 by its last minute, 15:30 by the closing rule
    expect_equals_plan_for_n(rows, request, open, 7);

    // Without the 15:59 row the final input stops short of 16:00: unsealed.
    rows.pop_back();
    expect_count(count_of(rows, request), 6);
    expect_equals_plan_for_n(rows, request, open, 7);
}

void fixture_gaps() {
    scenario = "1m to 60m, sparse: a missing hour and partial hours";
    // Hour 1 minutes 00-29 (before T, left pending, sealed by the next group),
    // hour 2 absent (no group), hour 3 full, hour 4 minutes 10-12 (sealed by
    // hour 5's first row, not by its own last minute), hour 5 full.
    std::vector<Bar> rows;
    add_span(rows, kBase + kHour, kBase + kHour + 30 * kMin, kMin);
    add_span(rows, kBase + 3 * kHour, kBase + 4 * kHour, kMin);
    add_span(rows, kBase + 4 * kHour + 10 * kMin, kBase + 4 * kHour + 13 * kMin, kMin);
    add_span(rows, kBase + 5 * kHour, kBase + 6 * kHour, kMin);
    const SelectedRetainedRequest request = six_hour_request();
    expect_count(count_of(rows, request), 4);  // 1 before T + hours 3, 4, 5
    expect_equals_plan_for_n(rows, request, kBase + kHour, 4);

    scenario = "1m to 240m, sparse rows, last group sealed at 15:59";
    // [00,04): 00:00 and 03:59 (sealed by its last minute); [04,08): 05:30
    // (pending, sealed by the next group); [08,12): 09:10 and 09:11 (sealed by
    // 13:00); [12,16): 13:00 and 15:59 (sealed by its last minute).
    const std::vector<Bar> wide = rows_at({kBase, kBase + 3 * kHour + 59 * kMin,
                                           kBase + 5 * kHour + 30 * kMin,
                                           kBase + 9 * kHour + 10 * kMin,
                                           kBase + 9 * kHour + 11 * kMin, kBase + 13 * kHour,
                                           kBase + 15 * kHour + 59 * kMin});
    const SelectedRetainedRequest wide_request =
        make_request(kBase + 8 * kHour, kBase + 16 * kHour, "1", "240");
    expect_count(count_of(wide, wide_request), 4);  // 2 before T + 2 window groups
    expect_equals_plan_for_n(wide, wide_request, kBase, 4);

    scenario = "a gap that spans T: one pre-T group, one window group";
    std::vector<Bar> across;
    add_span(across, kBase + kHour, kBase + 2 * kHour, kMin);
    add_span(across, kBase + 4 * kHour, kBase + 5 * kHour, kMin);
    expect_count(count_of(across, request), 2);
    expect_equals_plan_for_n(across, request, kBase, 3);
}

void fixture_empty_array() {
    scenario = "empty array";
    const SelectedRetainedRequest request = six_hour_request();
    expect_count(pineforge::count_selected_retained_primary(nullptr, 0, request), 0);
    const std::vector<Bar> none;
    expect_count(count_of(none, request), 0);
    expect_equals_plan_for_n(none, request, kBase, 3);
}

void fixture_no_cap_on_retained_prehistory() {
    // 6000 hourly rows before T and three in the window, 60m input and script.
    // The planner caps its pre-roll request at 5000 and keeps the last 5000
    // groups; the count has no request and no cap, and what the planner keeps is
    // counted to the planner's own fed_script_bars.
    scenario = "no 5000 cap on a native caller's retained prehistory";
    constexpr std::int64_t kBefore = 6000;
    std::vector<Bar> rows;
    rows.reserve(static_cast<std::size_t>(kBefore + 3));
    add_span(rows, kBase, kBase + (kBefore + 3) * kHour, kHour);
    for (const bool tolerant : {true, false}) {
        SelectedRetainedRequest request =
            make_request(kBase + kBefore * kHour, kBase + (kBefore + 3) * kHour, "60", "60");
        request.feed_tolerant = tolerant;
        expect_count(count_of(rows, request), 6003);
        expect_equals_plan(rows, request, kBase, 0);
        expect_equals_plan(rows, request, kBase, 1);
        expect_equals_plan(rows, request, kBase, 5000);
    }
}

void fixture_utc_aliases() {
    scenario = "UTC aliases admit pairwise";
    const std::vector<Bar> rows = full_six_hours();
    const char* const names[] = {"", "UTC", "GMT", "Etc/UTC", "Etc/GMT"};
    for (const char* chart : names) {
        for (const char* engine : names) {
            SelectedRetainedRequest request = six_hour_request();
            request.chart_timezone = chart;
            request.engine_timezone = engine;
            expect_count(count_of(rows, request), 6);
        }
    }
}

void fixture_same_non_utc_zone() {
    // Needs the host's tzdata for America/New_York. January is standard time all
    // day, so the hourly grid stays on the UTC hours and the count is the UTC
    // fixture's.
    scenario = "the same non-UTC zone admits (needs tzdata)";
    SelectedRetainedRequest request = six_hour_request();
    request.chart_timezone = "America/New_York";
    request.engine_timezone = "America/New_York";
    expect_count(count_of(full_six_hours(), request), 6);
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
        SelectedRetainedRequest request = six_hour_request();
        request.chart_timezone = pair.chart;
        request.engine_timezone = pair.engine;
        expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::CalendarUnsupported,
                       "timezone");
    }

    scenario = "equal clock tokens do not excuse invalid grammar";
    SelectedRetainedRequest zone = six_hour_request();
    zone.chart_timezone = "Not/AZone";
    zone.engine_timezone = "Not/AZone";
    expect_refused(count_of(full_six_hours(), zone), SelectedPlanStatus::CalendarUnsupported,
                   "engine_timezone");

    SelectedRetainedRequest session = six_hour_request();
    session.session = "garbage";
    expect_refused(count_of(full_six_hours(), session), SelectedPlanStatus::CalendarUnsupported,
                   "session");
    session.session = "2500-0100";
    expect_refused(count_of(full_six_hours(), session), SelectedPlanStatus::CalendarUnsupported,
                   "session");
}

void fixture_request_refusals() {
    scenario = "primary token and input timeframe";
    const char* const bad_scripts[] = {"",   "D",  "1D", "W", "M",   "2",   "7",
                                       "45", "1H", "5S", " 5", "05", "360", "15S"};
    for (const char* token : bad_scripts) {
        SelectedRetainedRequest request = six_hour_request();
        request.script_tf = token;
        expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                       "script_tf");
    }
    const char* const bad_inputs[] = {"", "1H", "01"};
    for (const char* token : bad_inputs) {
        SelectedRetainedRequest request = six_hour_request();
        request.input_tf = token;
        expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                       "input_tf");
    }
    // Valid grammar, a pairing the consumer does not group.
    const char* const bad_pairs[] = {"D", "7", "120"};
    for (const char* token : bad_pairs) {
        SelectedRetainedRequest request = six_hour_request();
        request.input_tf = token;
        expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::CalendarUnsupported,
                       "timeframes");
    }

    scenario = "i53 bounds, order and the row array";
    const std::int64_t over = kI53 + 1;
    SelectedRetainedRequest request = six_hour_request();
    request.start_ms = over;
    expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "start_ms");
    request = six_hour_request();
    request.start_ms = -over;
    expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "start_ms");
    request = six_hour_request();
    request.end_ms = over;
    expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "end_ms");

    request = six_hour_request();
    request.end_ms = request.start_ms;  // T == E
    expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "end_ms");
    request = six_hour_request();
    request.end_ms = request.start_ms - kHour;  // T > E
    expect_refused(count_of(full_six_hours(), request), SelectedPlanStatus::RequestInvalid,
                   "end_ms");

    expect_refused(pineforge::count_selected_retained_primary(nullptr, 3, six_hour_request()),
                   SelectedPlanStatus::RequestInvalid, "rows");
}

void fixture_boundary_refusals() {
    scenario = "T and E must each be the nominal open of the native script interval";
    const std::vector<Bar> rows = full_six_hours();
    SelectedRetainedRequest request = six_hour_request();
    request.start_ms = kBase + 3 * kHour + kMin;
    expect_refused(count_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "start_ms");

    request = six_hour_request();
    request.end_ms = kBase + 6 * kHour + 5 * kMin;
    expect_refused(count_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "end_ms");

    // T is checked before E.
    request = six_hour_request();
    request.start_ms = kBase + 3 * kHour + kMin;
    request.end_ms = kBase + 6 * kHour + 5 * kMin;
    expect_refused(count_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "start_ms");

    // 240m: 03:00 is on the hour grid but not on the 4-hour grid.
    request = make_request(kBase + 3 * kHour, kBase + 8 * kHour, "1", "240");
    expect_refused(count_of(rows, request), SelectedPlanStatus::BoundaryUnaligned, "start_ms");
}

void fixture_row_refusals() {
    scenario = "rows must strictly increase and lie before E";
    const SelectedRetainedRequest request = six_hour_request();

    expect_refused(count_of(rows_at({kBase + 3 * kHour, kBase + 3 * kHour + 2 * kMin,
                                     kBase + 3 * kHour + kMin}),
                            request),
                   SelectedPlanStatus::RowsUnordered, "rows");
    expect_refused(count_of(rows_at({kBase + kHour, kBase + kHour}), request),
                   SelectedPlanStatus::RowsUnordered, "rows");

    expect_refused(count_of(rows_at({kBase + 5 * kHour, kBase + 6 * kHour}), request),
                   SelectedPlanStatus::FeedRangeInvalid, "rows");
    expect_refused(count_of(rows_at({kBase + 6 * kHour + kMin}), request),
                   SelectedPlanStatus::FeedRangeInvalid, "rows");
    // One row short of E is admitted: hour 0's lone row is a pre-T group the
    // horizon completes, 05:59 is a window group sealed by reaching 06:00.
    expect_count(count_of(rows_at({kBase, kBase + 6 * kHour - kMin}), request), 2);
}

void fixture_executed_count_ignores_e() {
    scenario = "the executed count reads T, the clock and the rows, never E";
    const std::vector<Bar> rows = full_six_hours();
    const SelectedRetainedRequest aligned = six_hour_request();
    // Where the retained count admits E, the two agree: 3 groups before T + 3 window groups.
    expect_count(count_of(rows, aligned), 6);
    expect_count(executed_of(rows, aligned), 6);

    // E off the hourly grid: refused by the retained count, the same 6 executed.
    SelectedRetainedRequest unaligned = aligned;
    unaligned.end_ms = kBase + 6 * kHour + 5 * kMin;
    expect_refused(count_of(rows, unaligned), SelectedPlanStatus::BoundaryUnaligned, "end_ms");
    expect_count(executed_of(rows, unaligned), 6);

    // E = 04:00 with rows through 05:59: rows at and after E are refused by the retained
    // count; executed, hours 4 and 5 are window groups each sealed by its last minute.
    SelectedRetainedRequest early = aligned;
    early.end_ms = kBase + 4 * kHour;
    expect_refused(count_of(rows, early), SelectedPlanStatus::FeedRangeInvalid, "rows");
    expect_count(executed_of(rows, early), 6);
    // A row at 06:00 opens hour 6, which nothing seals: it adds no executed bar.
    std::vector<Bar> tail = rows;
    tail.push_back(row_at(kBase + 6 * kHour));
    expect_count(executed_of(tail, early), 6);

    // E unbounded: neither its 53-bit range nor T < E is asked.
    SelectedRetainedRequest unbounded = aligned;
    unbounded.end_ms = std::numeric_limits<std::int64_t>::max();
    expect_refused(count_of(rows, unbounded), SelectedPlanStatus::RequestInvalid, "end_ms");
    expect_count(executed_of(rows, unbounded), 6);
    unbounded.end_ms = unbounded.start_ms;  // T == E
    expect_refused(count_of(rows, unbounded), SelectedPlanStatus::RequestInvalid, "end_ms");
    expect_count(executed_of(rows, unbounded), 6);

    // T, the rows and the array are checked exactly as by the retained count.
    SelectedRetainedRequest off_grid_t = unaligned;
    off_grid_t.start_ms = kBase + 3 * kHour + kMin;
    expect_refused(executed_of(rows, off_grid_t), SelectedPlanStatus::BoundaryUnaligned,
                   "start_ms");
    expect_refused(executed_of(rows_at({kBase + 3 * kHour, kBase + kHour}), unaligned),
                   SelectedPlanStatus::RowsUnordered, "rows");
    expect_refused(pineforge::count_selected_executed_primary(nullptr, 3, unaligned),
                   SelectedPlanStatus::RequestInvalid, "rows");
}

}  // namespace

int main() {
    fixture_full_six_hours();
    fixture_all_eight_primary_tokens();
    fixture_passthrough();
    fixture_missing_last_pre_t_slot();
    fixture_no_window_pre_t_horizon();
    fixture_incomplete_window_tail();
    fixture_closing_rule_clipped_session();
    fixture_gaps();
    fixture_empty_array();
    fixture_no_cap_on_retained_prehistory();
    fixture_utc_aliases();
    fixture_same_non_utc_zone();
    fixture_clock_refusals();
    fixture_request_refusals();
    fixture_boundary_refusals();
    fixture_row_refusals();
    fixture_executed_count_ignores_e();
    if (failures != 0) {
        std::fprintf(stderr, "test_selected_retained_count: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_selected_retained_count: all checks passed\n");
    return 0;
}
