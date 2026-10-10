// Selected primary planner C bridge fixtures (pf_plan_selected_primary_v1).
//
// It is registered in tests/CMakeLists.txt, which also links it against
// pineforge_window_plan.
//
// The C bridge is reached through the shared helper only. The kernel archive is
// linked so the C++ planner can be called in the same process as a reference:
// every C result is compared field by field with the C++ plan of the same
// input, so no calendar, count or boundary logic is repeated here. The frozen
// words (status 0..6, bound, mask bits, flags, layout) are asserted as literals
// from the bridge's frozen layout, never read back from the header under
// test.
//
// Not exercised here, by design of what a black-box fixture can reach: the -2
// (allocation) and -3 (bridge invariant, e.g. an option longer than 31
// characters) returns need fault injection into the planner, and a count that
// does not fit size_t cannot be built on a 64-bit size_t. Those paths are
// reviewed by reading only. SelectedPlanStatus::InternalError (word 6) is not
// reachable from a valid-looking request either; its word is covered by the
// enumerator table below.

#include <pineforge/selected_window_plan.h>
#include <pineforge/selected_window_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

int failures = 0;
const char* scenario = "initialization";

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", scenario, __FILE__,      \
                     __LINE__, #condition);                                    \
    }                                                                          \
} while (0)

// One offset, asserted at compile time and again when the test runs.
#define LAYOUT(type, member, offset)                                           \
    static_assert(offsetof(type, member) == static_cast<std::size_t>(offset),  \
                  #type "::" #member " offset");                               \
    CHECK(offsetof(type, member) == static_cast<std::size_t>(offset))

// The two exported calls, taken through function pointers so that their
// declared signatures are part of what compiles.
using VersionFn = std::uint32_t (*)(void);
using PlanFn = int (*)(const pf_selected_plan_request_v1*, const pf_bar_t*, std::uint64_t,
                       pf_selected_plan_result_v1*);
const VersionFn version_fn = &pf_selected_plan_version;
const PlanFn plan_fn = &pf_plan_selected_primary_v1;

constexpr std::int64_t kMin = 60'000;
constexpr std::int64_t kHour = 60 * kMin;
constexpr std::int64_t kBase = 1'704'067'200'000;  // 2024-01-01T00:00:00Z, a Monday

// ── Layout ───────────────────────────────────────────────────────────────────

static_assert(sizeof(void*) == 8, "the frozen sizes are the 64-bit layout (LP64, LLP64)");
static_assert(std::is_standard_layout<pf_selected_plan_request_v1>::value
                  && std::is_trivially_copyable<pf_selected_plan_request_v1>::value,
              "the request is plain data");
static_assert(std::is_standard_layout<pf_selected_plan_result_v1>::value
                  && std::is_trivially_copyable<pf_selected_plan_result_v1>::value,
              "the result is plain data");
static_assert(sizeof(pf_selected_plan_request_v1) == 80, "request size");
static_assert(alignof(pf_selected_plan_request_v1) == 8, "request alignment");
static_assert(sizeof(pf_selected_plan_result_v1) == 248, "result size");
static_assert(alignof(pf_selected_plan_result_v1) == 8, "result alignment");
static_assert(sizeof(pf_selected_plan_result_v1::option) == 32, "option size");

void fixture_layout() {
    scenario = "layout and version";
    CHECK(sizeof(pf_selected_plan_request_v1) == 80);
    CHECK(alignof(pf_selected_plan_request_v1) == 8);
    CHECK(sizeof(pf_selected_plan_result_v1) == 248);
    CHECK(alignof(pf_selected_plan_result_v1) == 8);
    CHECK(sizeof(pf_selected_plan_result_v1::option) == 32);

    LAYOUT(pf_selected_plan_request_v1, struct_size, 0);
    LAYOUT(pf_selected_plan_request_v1, version, 4);
    LAYOUT(pf_selected_plan_request_v1, start_ms, 8);
    LAYOUT(pf_selected_plan_request_v1, end_ms, 16);
    LAYOUT(pf_selected_plan_request_v1, fed_start_ms, 24);
    LAYOUT(pf_selected_plan_request_v1, preroll_bars, 32);
    LAYOUT(pf_selected_plan_request_v1, feed_tolerant, 36);
    LAYOUT(pf_selected_plan_request_v1, input_tf, 40);
    LAYOUT(pf_selected_plan_request_v1, script_tf, 48);
    LAYOUT(pf_selected_plan_request_v1, chart_timezone, 56);
    LAYOUT(pf_selected_plan_request_v1, engine_timezone, 64);
    LAYOUT(pf_selected_plan_request_v1, session, 72);

    LAYOUT(pf_selected_plan_result_v1, struct_size, 0);
    LAYOUT(pf_selected_plan_result_v1, version, 4);
    LAYOUT(pf_selected_plan_result_v1, status, 8);
    LAYOUT(pf_selected_plan_result_v1, bound, 12);
    LAYOUT(pf_selected_plan_result_v1, value_ms, 16);
    LAYOUT(pf_selected_plan_result_v1, previous_boundary_ms, 24);
    LAYOUT(pf_selected_plan_result_v1, next_boundary_ms, 32);
    LAYOUT(pf_selected_plan_result_v1, supplied_input_bars, 40);
    LAYOUT(pf_selected_plan_result_v1, supplied_script_bars, 48);
    LAYOUT(pf_selected_plan_result_v1, available_script_bars, 56);
    LAYOUT(pf_selected_plan_result_v1, used_script_bars, 64);
    LAYOUT(pf_selected_plan_result_v1, trimmed_script_bars, 72);
    LAYOUT(pf_selected_plan_result_v1, trim_index, 80);
    LAYOUT(pf_selected_plan_result_v1, fed_input_bars, 88);
    LAYOUT(pf_selected_plan_result_v1, fed_script_bars, 96);
    LAYOUT(pf_selected_plan_result_v1, preroll_input_bars, 104);
    LAYOUT(pf_selected_plan_result_v1, window_input_bars, 112);
    LAYOUT(pf_selected_plan_result_v1, window_script_bars, 120);
    LAYOUT(pf_selected_plan_result_v1, trim_start_ms, 128);
    LAYOUT(pf_selected_plan_result_v1, preroll_first_bar_ms, 136);
    LAYOUT(pf_selected_plan_result_v1, preroll_last_bar_ms, 144);
    LAYOUT(pf_selected_plan_result_v1, supplied_first_data_ms, 152);
    LAYOUT(pf_selected_plan_result_v1, supplied_last_data_ms, 160);
    LAYOUT(pf_selected_plan_result_v1, fed_first_data_ms, 168);
    LAYOUT(pf_selected_plan_result_v1, fed_last_data_ms, 176);
    LAYOUT(pf_selected_plan_result_v1, window_first_data_ms, 184);
    LAYOUT(pf_selected_plan_result_v1, window_last_data_ms, 192);
    LAYOUT(pf_selected_plan_result_v1, present_mask, 200);
    LAYOUT(pf_selected_plan_result_v1, shortfall, 204);
    LAYOUT(pf_selected_plan_result_v1, complete_pending_preroll_at_horizon, 208);
    LAYOUT(pf_selected_plan_result_v1, reserved, 212);
    LAYOUT(pf_selected_plan_result_v1, option, 216);

    // The row type the bridge reads in place: five doubles, then the timestamp,
    // identical to pineforge::Bar.
    CHECK(sizeof(pf_bar_t) == 48);
    CHECK(sizeof(pf_bar_t) == sizeof(Bar));
    CHECK(alignof(pf_bar_t) == alignof(Bar));
    LAYOUT(pf_bar_t, open, 0);
    LAYOUT(pf_bar_t, high, 8);
    LAYOUT(pf_bar_t, low, 16);
    LAYOUT(pf_bar_t, close, 24);
    LAYOUT(pf_bar_t, volume, 32);
    LAYOUT(pf_bar_t, timestamp, 40);
    CHECK(offsetof(pf_bar_t, open) == offsetof(Bar, open));
    CHECK(offsetof(pf_bar_t, high) == offsetof(Bar, high));
    CHECK(offsetof(pf_bar_t, low) == offsetof(Bar, low));
    CHECK(offsetof(pf_bar_t, close) == offsetof(Bar, close));
    CHECK(offsetof(pf_bar_t, volume) == offsetof(Bar, volume));
    CHECK(offsetof(pf_bar_t, timestamp) == offsetof(Bar, timestamp));

    // The frozen status words are the C++ enumerators' order, though the bridge
    // maps them explicitly and does not rely on it.
    CHECK(static_cast<int>(SelectedPlanStatus::Ok) == 0);
    CHECK(static_cast<int>(SelectedPlanStatus::RequestInvalid) == 1);
    CHECK(static_cast<int>(SelectedPlanStatus::CalendarUnsupported) == 2);
    CHECK(static_cast<int>(SelectedPlanStatus::BoundaryUnaligned) == 3);
    CHECK(static_cast<int>(SelectedPlanStatus::FeedRangeInvalid) == 4);
    CHECK(static_cast<int>(SelectedPlanStatus::RowsUnordered) == 5);
    CHECK(static_cast<int>(SelectedPlanStatus::InternalError) == 6);

    CHECK(version_fn() == 1u);
}

// ── Inputs ───────────────────────────────────────────────────────────────────

// The rows twice over: as pf_bar_t, which the bridge reads, and as
// pineforge::Bar, which the reference planner reads. Prices differ per row so a
// mixed-up field offset would change the timestamps the bridge sees.
struct Feed {
    std::vector<pf_bar_t> c;
    std::vector<Bar> cpp;

    void add(std::int64_t timestamp) {
        const double seed = 100.0 + static_cast<double>(c.size());
        pf_bar_t bar;
        std::memset(&bar, 0, sizeof bar);
        bar.open = seed;
        bar.high = seed + 2.0;
        bar.low = seed - 2.0;
        bar.close = seed + 1.0;
        bar.volume = 10.0 + seed;
        bar.timestamp = timestamp;
        c.push_back(bar);
        cpp.push_back(Bar{bar.open, bar.high, bar.low, bar.close, bar.volume, bar.timestamp});
    }
};

Feed span(std::int64_t from, std::int64_t until, std::int64_t step) {
    Feed feed;
    for (std::int64_t at = from; at < until; at += step) feed.add(at);
    return feed;
}

Feed rows_at(std::initializer_list<std::int64_t> timestamps) {
    Feed feed;
    for (const std::int64_t at : timestamps) feed.add(at);
    return feed;
}

pf_selected_plan_request_v1 make_request(std::int64_t start, std::int64_t end,
                                         std::int64_t fed_start, std::uint32_t preroll,
                                         const char* input_tf, const char* script_tf) {
    pf_selected_plan_request_v1 request;
    std::memset(&request, 0, sizeof request);
    request.struct_size = static_cast<std::uint32_t>(sizeof request);
    request.version = 1;
    request.start_ms = start;
    request.end_ms = end;
    request.fed_start_ms = fed_start;
    request.preroll_bars = preroll;
    request.feed_tolerant = 1;
    request.input_tf = input_tf;
    request.script_tf = script_tf;
    request.chart_timezone = "UTC";
    request.engine_timezone = "UTC";
    request.session = "";
    return request;
}

// Rows 00:00..05:59 every minute (six hourly groups), T = 03:00, E = 06:00,
// F = 00:00: hours 0-2 are before T, hours 3-5 are the window.
Feed full_six_hours() { return span(kBase, kBase + 6 * kHour, kMin); }

pf_selected_plan_request_v1 full_six_hours_request(std::uint32_t preroll) {
    return make_request(kBase + 3 * kHour, kBase + 6 * kHour, kBase, preroll, "1", "60");
}

SelectedPlanRequest reference_request(const pf_selected_plan_request_v1& c) {
    SelectedPlanRequest request{};
    request.start_ms = c.start_ms;
    request.end_ms = c.end_ms;
    request.fed_start_ms = c.fed_start_ms;
    request.preroll_bars = c.preroll_bars;
    request.input_tf = c.input_tf;
    request.script_tf = c.script_tf;
    request.chart_timezone = c.chart_timezone;
    request.engine_timezone = c.engine_timezone;
    request.session = c.session;
    request.feed_tolerant = c.feed_tolerant != 0;
    return request;
}

// A valid output header over a junk body: a successful call must overwrite all
// of the body, a failing one none of it.
pf_selected_plan_result_v1 sentinel() {
    pf_selected_plan_result_v1 result;
    std::memset(&result, 0xA5, sizeof result);
    result.struct_size = static_cast<std::uint32_t>(sizeof result);
    result.version = 1;
    return result;
}

struct Run {
    int rc = 0;
    pf_selected_plan_result_v1 c;
    SelectedPrimaryPlan plan;
};

Run run_raw(const pf_selected_plan_request_v1& request, const pf_bar_t* c_rows,
            const Bar* cpp_rows, std::size_t count) {
    Run run;
    run.c = sentinel();
    run.rc = plan_fn(&request, c_rows, count, &run.c);
    run.plan = pineforge::plan_selected_primary(cpp_rows, count, reference_request(request));
    return run;
}

Run run_both(const pf_selected_plan_request_v1& request, const Feed& feed) {
    return run_raw(request, feed.c.data(), feed.cpp.data(), feed.c.size());
}

// ── Comparison ───────────────────────────────────────────────────────────────

// The frozen words, spelled as literals.
std::int32_t expected_status_word(SelectedPlanStatus status) {
    switch (status) {
    case SelectedPlanStatus::Ok: return 0;
    case SelectedPlanStatus::RequestInvalid: return 1;
    case SelectedPlanStatus::CalendarUnsupported: return 2;
    case SelectedPlanStatus::BoundaryUnaligned: return 3;
    case SelectedPlanStatus::FeedRangeInvalid: return 4;
    case SelectedPlanStatus::RowsUnordered: return 5;
    case SelectedPlanStatus::InternalError: return 6;
    }
    return -100;
}

// ASCII, NUL-terminated and zero-padded to the 32 bytes.
bool option_is(const pf_selected_plan_result_v1& c, const char* text) {
    const std::size_t length = std::strlen(text);
    if (length >= sizeof c.option) return false;
    if (std::memcmp(c.option, text, length) != 0) return false;
    for (std::size_t at = length; at < sizeof c.option; ++at) {
        if (c.option[at] != '\0') return false;
    }
    return true;
}

// Bit `bit` of the mask is the presence of the timestamp; an absent one is zero.
void expect_optional(const pf_selected_plan_result_v1& c, unsigned bit, std::int64_t value,
                     const std::optional<std::int64_t>& expected) {
    CHECK(((c.present_mask >> bit) & 1u) == (expected.has_value() ? 1u : 0u));
    CHECK(value == expected.value_or(0));
}

// The C result is the C++ plan, field for field.
void expect_maps(const pf_selected_plan_result_v1& c, const SelectedPrimaryPlan& p) {
    CHECK(c.struct_size == sizeof c);
    CHECK(c.version == 1u);
    CHECK(c.status == expected_status_word(p.status));
    CHECK(option_is(c, p.option.c_str()));
    CHECK(c.bound == p.bound);
    CHECK(c.value_ms == p.value_ms);
    CHECK(c.previous_boundary_ms == p.previous_boundary_ms);
    CHECK(c.next_boundary_ms == p.next_boundary_ms);
    CHECK(c.supplied_input_bars == p.supplied_input_bars);
    CHECK(c.supplied_script_bars == p.supplied_script_bars);
    CHECK(c.available_script_bars == p.available_script_bars);
    CHECK(c.used_script_bars == p.used_script_bars);
    CHECK(c.trimmed_script_bars == p.trimmed_script_bars);
    CHECK(c.trim_index == p.trim_index);
    CHECK(c.fed_input_bars == p.fed_input_bars);
    CHECK(c.fed_script_bars == p.fed_script_bars);
    CHECK(c.preroll_input_bars == p.preroll_input_bars);
    CHECK(c.window_input_bars == p.window_input_bars);
    CHECK(c.window_script_bars == p.window_script_bars);
    CHECK(c.trim_start_ms == p.trim_start_ms);
    expect_optional(c, 0, c.preroll_first_bar_ms, p.preroll_first_bar_ms);
    expect_optional(c, 1, c.preroll_last_bar_ms, p.preroll_last_bar_ms);
    expect_optional(c, 2, c.supplied_first_data_ms, p.supplied_first_data_ms);
    expect_optional(c, 3, c.supplied_last_data_ms, p.supplied_last_data_ms);
    expect_optional(c, 4, c.fed_first_data_ms, p.fed_first_data_ms);
    expect_optional(c, 5, c.fed_last_data_ms, p.fed_last_data_ms);
    expect_optional(c, 6, c.window_first_data_ms, p.window_first_data_ms);
    expect_optional(c, 7, c.window_last_data_ms, p.window_last_data_ms);
    CHECK((c.present_mask & ~0xFFu) == 0u);
    CHECK(c.shortfall == (p.shortfall ? 1u : 0u));
    CHECK(c.complete_pending_preroll_at_horizon ==
          (p.complete_pending_preroll_at_horizon ? 1u : 0u));
    CHECK(c.reserved == 0u);
}

// The exact bytes of a planner refusal: a whole result that is zero except the
// header, the status word, the option and the three boundary values.
pf_selected_plan_result_v1 refusal_bytes(std::int32_t status, const char* option,
                                         std::int32_t bound, std::int64_t value,
                                         std::int64_t previous, std::int64_t next) {
    pf_selected_plan_result_v1 result;
    std::memset(&result, 0, sizeof result);
    result.struct_size = static_cast<std::uint32_t>(sizeof result);
    result.version = 1;
    result.status = status;
    result.bound = bound;
    result.value_ms = value;
    result.previous_boundary_ms = previous;
    result.next_boundary_ms = next;
    const std::size_t length = std::strlen(option);
    CHECK(length < sizeof result.option);
    std::memcpy(result.option, option, length);
    return result;
}

// A refusal is a successful C call (return 0) carrying a whole result.
void expect_refusal(const Run& run, std::int32_t status, const char* option,
                    std::int32_t bound = -1, std::int64_t value = 0, std::int64_t previous = 0,
                    std::int64_t next = 0) {
    CHECK(run.rc == 0);
    expect_maps(run.c, run.plan);
    const pf_selected_plan_result_v1 expected =
        refusal_bytes(status, option, bound, value, previous, next);
    CHECK(std::memcmp(&run.c, &expected, sizeof expected) == 0);
    CHECK(run.c.status == status);
    CHECK(option_is(run.c, option));
}

// ── Success ──────────────────────────────────────────────────────────────────

void fixture_success() {
    scenario = "success: 1m to 60m, N=2, all eight timestamps present";
    for (const std::uint32_t tolerant : {1u, 0u}) {
        pf_selected_plan_request_v1 request = full_six_hours_request(2);
        request.feed_tolerant = tolerant;
        const Run run = run_both(request, full_six_hours());
        CHECK(run.rc == 0);
        CHECK(run.c.status == 0);
        expect_maps(run.c, run.plan);  // the junk body is gone, the plan is whole
        if (tolerant != 1u) continue;

        // The planner's own fixture for this input, restated as C words.
        const pf_selected_plan_result_v1& c = run.c;
        CHECK(option_is(c, ""));
        CHECK(c.bound == -1);
        CHECK(c.value_ms == 0);
        CHECK(c.previous_boundary_ms == 0);
        CHECK(c.next_boundary_ms == 0);
        CHECK(c.supplied_input_bars == 360u);
        CHECK(c.supplied_script_bars == 6u);
        CHECK(c.available_script_bars == 3u);
        CHECK(c.used_script_bars == 2u);
        CHECK(c.trimmed_script_bars == 1u);
        CHECK(c.trim_index == 60u);
        CHECK(c.fed_input_bars == 300u);
        CHECK(c.fed_script_bars == 5u);
        CHECK(c.preroll_input_bars == 120u);
        CHECK(c.window_input_bars == 180u);
        CHECK(c.window_script_bars == 3u);
        CHECK(c.trim_start_ms == kBase + kHour);
        CHECK(c.preroll_first_bar_ms == kBase + kHour);
        CHECK(c.preroll_last_bar_ms == kBase + 2 * kHour);
        CHECK(c.supplied_first_data_ms == kBase);
        CHECK(c.supplied_last_data_ms == kBase + 6 * kHour - kMin);
        CHECK(c.fed_first_data_ms == kBase + kHour);
        CHECK(c.fed_last_data_ms == kBase + 6 * kHour - kMin);
        CHECK(c.window_first_data_ms == kBase + 3 * kHour);
        CHECK(c.window_last_data_ms == kBase + 6 * kHour - kMin);
        CHECK(c.present_mask == 0xFFu);
        CHECK(c.shortfall == 0u);
        CHECK(c.complete_pending_preroll_at_horizon == 0u);
        CHECK(c.reserved == 0u);
    }
}

// ── Nullable transport ───────────────────────────────────────────────────────

void fixture_nullable_transport() {
    scenario = "absent timestamps are zero with their bit clear";
    {
        // N = 0 keeps no pre-roll: bits 0 and 1 clear, bits 2..7 set.
        const Run run = run_both(full_six_hours_request(0), full_six_hours());
        CHECK(run.rc == 0);
        CHECK(run.c.status == 0);
        expect_maps(run.c, run.plan);
        CHECK(run.c.used_script_bars == 0u);
        CHECK(run.c.present_mask == 0xFCu);
        CHECK(run.c.preroll_first_bar_ms == 0);
        CHECK(run.c.preroll_last_bar_ms == 0);
        CHECK(run.c.trim_start_ms == kBase + 3 * kHour);  // used 0: T itself
    }
    {
        // An empty feed is valid: no timestamp is present, N stays requested.
        const pf_selected_plan_request_v1 request = full_six_hours_request(2);
        const Run run = run_raw(request, nullptr, nullptr, 0);
        CHECK(run.rc == 0);
        CHECK(run.c.status == 0);
        expect_maps(run.c, run.plan);
        CHECK(run.c.present_mask == 0u);
        CHECK(run.c.supplied_input_bars == 0u);
        CHECK(run.c.available_script_bars == 0u);
        CHECK(run.c.shortfall == 1u);
        CHECK(run.c.supplied_first_data_ms == 0);
        CHECK(run.c.window_last_data_ms == 0);
    }

    scenario = "a present timestamp may be zero, which only its bit tells apart from absent";
    {
        // Rows at 00:00..02:59 from the epoch itself, T = 01:00, E = 03:00,
        // F = 0, N = 1: the kept pre-roll group opens at 0 and the first row is
        // at 0, so four of the eight timestamps are present and zero.
        const Feed feed = span(0, 3 * kHour, kMin);
        const pf_selected_plan_request_v1 request = make_request(kHour, 3 * kHour, 0, 1, "1", "60");
        const Run run = run_both(request, feed);
        CHECK(run.rc == 0);
        CHECK(run.c.status == 0);
        expect_maps(run.c, run.plan);
        CHECK(run.c.present_mask == 0xFFu);
        CHECK(run.c.preroll_first_bar_ms == 0);
        CHECK(run.c.supplied_first_data_ms == 0);
        CHECK(run.c.fed_first_data_ms == 0);
        CHECK(run.c.preroll_last_bar_ms == 0);
        CHECK(run.c.window_first_data_ms == kHour);
        CHECK(run.c.supplied_last_data_ms == 3 * kHour - kMin);
    }
}

// ── Planner refusals ─────────────────────────────────────────────────────────

void fixture_planner_refusals() {
    const Feed rows = full_six_hours();

    scenario = "RequestInvalid (word 1)";
    {
        pf_selected_plan_request_v1 request = full_six_hours_request(2);
        request.input_tf = "";  // an empty timeframe is a planner error, not a bridge one
        expect_refusal(run_both(request, rows), 1, "input_tf");
    }
    expect_refusal(run_both(full_six_hours_request(5001), rows), 1, "preroll_bars", -1, 5001);
    // A NULL row pointer with a positive count is never dereferenced by the
    // bridge: it reaches the planner, which refuses it.
    expect_refusal(run_raw(full_six_hours_request(2), nullptr, nullptr, 3), 1, "rows");

    scenario = "CalendarUnsupported (word 2)";
    {
        pf_selected_plan_request_v1 request = full_six_hours_request(2);
        request.chart_timezone = "UTC";
        request.engine_timezone = "America/New_York";  // refused at the identity comparison
        expect_refusal(run_both(request, rows), 2, "timezone");
    }
    {
        pf_selected_plan_request_v1 request = full_six_hours_request(2);
        request.chart_timezone = "Not/AZone";
        request.engine_timezone = "Not/AZone";  // the longest option name transported
        expect_refusal(run_both(request, rows), 2, "engine_timezone");
    }
    {
        pf_selected_plan_request_v1 request = full_six_hours_request(2);
        request.session = "garbage";
        expect_refusal(run_both(request, rows), 2, "session");
    }

    scenario = "BoundaryUnaligned (word 3)";
    {
        pf_selected_plan_request_v1 request = full_six_hours_request(2);
        request.start_ms = kBase + 3 * kHour + kMin;
        expect_refusal(run_both(request, rows), 3, "start_ms", 0, kBase + 3 * kHour + kMin,
                       kBase + 3 * kHour, kBase + 4 * kHour);
    }

    scenario = "FeedRangeInvalid (word 4)";
    expect_refusal(run_both(full_six_hours_request(2), rows_at({kBase - kMin, kBase})), 4, "rows",
                   2, kBase - kMin, kBase, kBase + 6 * kHour);

    scenario = "RowsUnordered (word 5)";
    expect_refusal(run_both(full_six_hours_request(2), rows_at({kBase + kHour, kBase + kHour})), 5,
                   "rows", -1, kBase + kHour, kBase + kHour);
}

// ── Bridge refusals: return -1 and write nothing ─────────────────────────────

// `out` is the caller's own copy: a negative return must leave it byte-equal.
void expect_negative(int expected, const pf_selected_plan_request_v1* request,
                     const pf_bar_t* rows, std::uint64_t count,
                     pf_selected_plan_result_v1 out) {
    const pf_selected_plan_result_v1 before = out;
    CHECK(plan_fn(request, rows, count, &out) == expected);
    CHECK(std::memcmp(&out, &before, sizeof out) == 0);
}

void fixture_descriptor_refusals() {
    const Feed feed = full_six_hours();
    const pf_bar_t* const rows = feed.c.data();
    const std::uint64_t count = feed.c.size();
    const pf_selected_plan_request_v1 good = full_six_hours_request(2);

    scenario = "the unmodified request is accepted (each refusal below changes one thing)";
    for (const std::uint32_t tolerant : {0u, 1u}) {
        pf_selected_plan_request_v1 request = good;
        request.feed_tolerant = tolerant;
        const Run run = run_both(request, feed);
        CHECK(run.rc == 0);
        CHECK(run.c.status == 0);
    }

    scenario = "request descriptor";
    expect_negative(-1, nullptr, rows, count, sentinel());
    for (const std::uint32_t size : {0u, 79u, 81u, 88u, 0xFFFFFFFFu}) {
        pf_selected_plan_request_v1 request = good;
        request.struct_size = size;
        expect_negative(-1, &request, rows, count, sentinel());
    }
    for (const std::uint32_t version : {0u, 2u, 0xFFFFFFFFu}) {
        pf_selected_plan_request_v1 request = good;
        request.version = version;
        expect_negative(-1, &request, rows, count, sentinel());
    }
    for (const std::uint32_t tolerant : {2u, 0xFFFFFFFFu}) {
        pf_selected_plan_request_v1 request = good;
        request.feed_tolerant = tolerant;
        expect_negative(-1, &request, rows, count, sentinel());
    }

    scenario = "each of the five string pointers must be non-NULL";
    const char* pf_selected_plan_request_v1::* const strings[] = {
        &pf_selected_plan_request_v1::input_tf, &pf_selected_plan_request_v1::script_tf,
        &pf_selected_plan_request_v1::chart_timezone,
        &pf_selected_plan_request_v1::engine_timezone, &pf_selected_plan_request_v1::session};
    for (const auto member : strings) {
        pf_selected_plan_request_v1 request = good;
        request.*member = nullptr;
        expect_negative(-1, &request, rows, count, sentinel());
    }

    scenario = "output descriptor";
    CHECK(plan_fn(&good, rows, count, nullptr) == -1);
    for (const std::uint32_t size : {0u, 247u, 249u, 256u, 0xFFFFFFFFu}) {
        pf_selected_plan_result_v1 out = sentinel();
        out.struct_size = size;
        expect_negative(-1, &good, rows, count, out);
    }
    for (const std::uint32_t version : {0u, 2u, 0xFFFFFFFFu}) {
        pf_selected_plan_result_v1 out = sentinel();
        out.version = version;
        expect_negative(-1, &good, rows, count, out);
    }
    {
        // Both descriptors bad at once is still one -1 and no write.
        pf_selected_plan_request_v1 request = good;
        request.version = 0;
        pf_selected_plan_result_v1 out = sentinel();
        out.version = 0;
        expect_negative(-1, &request, rows, count, out);
    }
}

}  // namespace

int main() {
    fixture_layout();
    fixture_success();
    fixture_nullable_transport();
    fixture_planner_refusals();
    fixture_descriptor_refusals();
    if (failures != 0) {
        std::fprintf(stderr, "test_selected_window_plan_c: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_selected_window_plan_c: all checks passed\n");
    return 0;
}
