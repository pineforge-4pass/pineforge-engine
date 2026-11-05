// Selected-capacity script_bars bound.
//
// The bound counts script buckets opened by supplied Primary rows across all Primary streams in a run.
// A bucket opens when its native script-bucket key differs from the preceding Primary row's key.
// Pre-roll, rows later trimmed and an unsealed final bucket all count. Source and Auxiliary rows do not.
// The product guard under test is observe_script_bars, called through this test's run_driver observer table.
// S14 calls the observer directly to check its invariant refusal.
// A row opening bucket limit + 1 is refused before it reaches the materialization sink. Equality passes;
// an absent bound imposes no limit, while explicit zero refuses the first bucket.
//
// Expected values come from this test's own oracles over timestamps written into its CSV text.
// Grid computes keys from the fixture anchor and interval length; opened_buckets counts key changes,
// and crossing_index locates the first row exceeding the bound, independently of driver output.
// The planner establishes fixture validity and shows where sealed or fed counts differ from opened counts.
// Each refusal checks selected_script_bars_limit_exceeded, class resource, non-retryability and the exact
// catalog English. Arguments are canonical unsigned decimal text: limit and used equal the bound before
// the attempted reservation, and requested is one. The sink holds exactly the rows before the crossing.
// CSV records vary every OHLCV field and use exact binary fractions within the reader's valid grammar.
//
// S1 checks complete buckets and equality at the bound.
// S2 counts an unsealed final bucket.
// S3 counts supplied pre-roll buckets even when the planner trims them.
// S4 counts opened keys across gaps with coarser input.
// S5 keeps Primary, Source and Auxiliary charges separate using distinct streams.
// S6 checks absence over many buckets; S6b keeps the driver active with a finite Source bound.
// S7 checks that explicit zero refuses the first row with an empty sink.
// S8 checks refusal before the crossing row reaches the sink and before a long tail is read.
// S9 uses session-anchored script buckets.
// S10 checks that equal input and script timeframes open one bucket per row.
// S11 preserves the last bucket across two Primary streams.
// S12 checks strict-feed hourly buckets.
// S13 checks strict-feed buckets with equal input and script timeframes.
// S14 checks typed refusal of a Primary row without a script bucket and unchanged State.
// Each counting scenario admits its opened count, refuses one below it, accepts an absent bound and
// admits a larger finite bound. S3 and S10 also refuse at the planner's smaller fed count.
//
// S8 observes early stopping through the sink and stream position without measuring internal buffers.
// This test exercises run_driver with the product guard in its own table, independently of the production
// entry's observer binding. Other slots remain null except for S6b's inert Source observer, which makes
// the driver read Source while script_bars is absent. The driver invokes observers only for present bounds.
// With all bounds absent, Completed or NotAdmitted / NoRequestedFields is acceptable; neither may refuse.
// NoRequestedFields delivers no rows. Typed failures propagate unchanged; setup and unrelated failures
// remain distinguishable from a capacity refusal.

#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window_plan.hpp>

#include "selected_capacity_fixture.hpp"
#include "../src/selected_capacity_driver.hpp"  // selected-capacity driver

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fixture = selected_capacity_fixture;
namespace sc = pineforge::detail::selected_capacity;

constexpr const char* kCodeName = "selected_script_bars_limit_exceeded";
constexpr const char* kEnglish = "Selected script-bar capacity exceeded.";  // the catalog's text

// ---- ADAPTER SEAM: interface to the selected-capacity driver ----------------------------------------------
namespace seam {

using Request = pineforge::SelectedPlanRequest;  // the selected request, exactly as the driver takes it

struct Streams {  // role-labelled streams: the CSV text of each role (fixture::make_csv output)
    std::string primary;
    std::vector<std::string> primary_more;
    std::optional<std::string> source;  // the same bytes as the primary: one input, two roles
    std::vector<std::string> auxiliary;
};

enum class Result { Admitted, Refused, NotAdmitted };

struct Outcome {
    Result result = Result::Admitted;
    std::string code_name, args_json, what;  // Refused: the carried typed failure (canonical args)
    std::optional<pineforge::RunFailureClass> failure_class;
    std::optional<bool> retryable;
    std::string not_admitted;                // NotAdmitted: the driver's reason, e.g. "MissingObserver"
    std::string detail;                      // the driver's message, for the failure text only
    std::vector<pineforge::Bar> delivered;   // what the materialization sink received, in order
    std::optional<sc::State> state;         // State committed by a completed driver run
    // Bytes of the primary stream the driver consumed: its coverage when it returns, the stream position when
    // the refusal escapes it.
    std::optional<std::uint64_t> consumed_primary_bytes;
};

// The observer table supplied by this test to run_driver. Only the slots needed by each call are set:
// script_bars and, in S6b, source_rows. The driver calls an observer only for a present bound.
// The script_bars slot binds the product guard; the Source callback below only enables S6b's stream read.

namespace carrier {  // inert Source reader for the absent-bound scenario
// S6b only: an inert source_rows observer. A finite source_rows bound makes the driver require and read a
// Source stream, so the driver does real work while script_bars is absent. This callback only enables that
// read: it counts nothing and refuses nothing. It is supplied by this test and requires no other capacity
// observer implementation.
void inert_source_rows(sc::State&, const sc::Limits&, const sc::RecordEvent&) {}
}  // namespace carrier

sc::ObserverTable table_under_test(bool with_source_rows) {
    sc::ObserverTable table;  // every slot null
    table.script_bars = &sc::observe_script_bars;
    if (with_source_rows) table.source_rows = &carrier::inert_source_rows;
    return table;
}

// The sink receives only Primary records, forming the materialized primary array.
// Source and Auxiliary records never reach it.
void collect(void* context, const pineforge::Bar& bar) {
    static_cast<std::vector<pineforge::Bar>*>(context)->push_back(bar);
}

std::string reason_text(sc::DriverReason reason) {
    switch (reason) {
        case sc::DriverReason::None: return "None";
        case sc::DriverReason::NoRequestedFields: return "NoRequestedFields";
        case sc::DriverReason::MissingObserver: return "MissingObserver";
        case sc::DriverReason::MissingSink: return "MissingSink";
        case sc::DriverReason::MissingInput: return "MissingInput";
        case sc::DriverReason::DuplicateInput: return "DuplicateInput";
        case sc::DriverReason::InvalidInput: return "InvalidInput";
        case sc::DriverReason::NoData: return "NoData";
        case sc::DriverReason::RequestInvalid: return "RequestInvalid";
        case sc::DriverReason::CalendarUnsupported: return "CalendarUnsupported";
        case sc::DriverReason::BoundaryUnaligned: return "BoundaryUnaligned";
        case sc::DriverReason::FeedRangeInvalid: return "FeedRangeInvalid";
        case sc::DriverReason::RowsUnordered: return "RowsUnordered";
        case sc::DriverReason::CsvColumns: return "CsvColumns";
        case sc::DriverReason::CsvValue: return "CsvValue";
        case sc::DriverReason::CsvUtf8: return "CsvUtf8";
        case sc::DriverReason::CsvSyntax: return "CsvSyntax";
        case sc::DriverReason::ObserverFailure: return "ObserverFailure";
        case sc::DriverReason::SinkFailure: return "SinkFailure";
        case sc::DriverReason::InternalError: return "InternalError";
        case sc::DriverReason::InputReadFailure: return "InputReadFailure";
    }
    return "UnknownReason";
}

// admit(): run the real driver over `streams` with ONLY script_bars limited (absent when nullopt); when
// `carrier_source_rows` is set, source_rows is ALSO finite (so that a run whose script_bars bound is absent
// still does real driver work). Catches every typed run failure and records its code, canonical arguments,
// class, retryability and English; non-typed exceptions propagate and fail the test.
// A typed failure propagates out of the driver unchanged, including its coded arguments and English.
// ObserverFailure and SinkFailure are reserved and never returned. The exception reserve() throws is
// coded<std::runtime_error> with the code and the canonical
// args. If a later driver returned ObserverFailure instead, the result below would read NotAdmitted /
// ObserverFailure and the test would fail loudly instead of passing.
Outcome admit(const Request& request, const Streams& streams, std::optional<std::uint64_t> script_bars,
              std::optional<std::uint64_t> carrier_source_rows = std::nullopt) {
    Outcome out;
    std::istringstream primary(streams.primary);
    std::vector<std::unique_ptr<std::istringstream>> primary_more;
    std::vector<std::string> primary_labels;
    std::unique_ptr<std::istringstream> source;
    std::vector<std::unique_ptr<std::istringstream>> auxiliary;
    std::vector<sc::StreamInput> inputs;
    inputs.emplace_back(sc::Role::Primary, primary, "primary");
    primary_more.reserve(streams.primary_more.size());
    primary_labels.reserve(streams.primary_more.size());
    for (std::size_t i = 0; i < streams.primary_more.size(); ++i) {
        primary_more.push_back(std::make_unique<std::istringstream>(streams.primary_more[i]));
        primary_labels.push_back("primary-more-" + std::to_string(i + 1));
        inputs.emplace_back(sc::Role::Primary, *primary_more.back(), primary_labels.back().c_str());
    }
    if (streams.source) {
        source = std::make_unique<std::istringstream>(*streams.source);  // its own stream object
        inputs.emplace_back(sc::Role::Source, *source, "source");
    }
    for (const std::string& text : streams.auxiliary) {
        auxiliary.push_back(std::make_unique<std::istringstream>(text));
        inputs.emplace_back(sc::Role::Auxiliary, *auxiliary.back(), "auxiliary");
    }
    sc::Limits limits;  // every bound absent
    if (script_bars) limits.script_bars = sc::Bound{true, *script_bars};
    if (carrier_source_rows) limits.source_rows = sc::Bound{true, *carrier_source_rows};
    try {
        const sc::DriverResult result =
            sc::run_driver(inputs.data(), inputs.size(), request, limits,
                           table_under_test(carrier_source_rows.has_value()), &collect, &out.delivered);
        out.consumed_primary_bytes = result.coverage.primary_bytes;
        out.detail = result.message;
        out.state = result.state;
        if (result.status == sc::DriverStatus::Completed) {
            out.result = Result::Admitted;
        } else {
            out.result = Result::NotAdmitted;
            out.not_admitted = reason_text(result.reason);
            switch (result.status) {
                case sc::DriverStatus::FormatError: out.not_admitted += " (FormatError)"; break;
                case sc::DriverStatus::IoError: out.not_admitted += " (IoError)"; break;
                case sc::DriverStatus::InputError: out.not_admitted += " (InputError)"; break;
                case sc::DriverStatus::CalendarError: out.not_admitted += " (CalendarError)"; break;
                case sc::DriverStatus::NotAdmitted:
                case sc::DriverStatus::Completed: break;
            }
        }
    } catch (const pineforge::coded<std::runtime_error>& error) {
        const pineforge::RunFailureValue failure = error.run_failure();
        const std::string name = pineforge::run_failure_code_name(failure.code);
        out.result = Result::Refused;
        out.code_name = name;
        out.args_json = failure.args ? *failure.args : std::string();
        out.what = error.what();
        out.failure_class = pineforge::run_failure_code_class(failure.code);
        out.retryable = pineforge::run_failure_code_retryable(failure.code);
        // The refusal escapes the driver, so no DriverResult carries coverage: read how far the driver got into
        // the primary stream from the stream itself (the position of the next unread byte; -1 only if the
        // stream failed, which a refusal does not cause).
        const std::istringstream::pos_type position = primary.tellg();
        if (position != std::istringstream::pos_type(-1)) {
            out.consumed_primary_bytes = static_cast<std::uint64_t>(static_cast<std::streamoff>(position));
        }
    }
    return out;
}

}  // namespace seam

// ---- checks ---------------------------------------------------------------------------------------------
int failures = 0;
int setup_failures = 0;
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
constexpr std::int64_t kDay = 24 * kHour;
constexpr std::int64_t kBase = 1'704'067'200'000;  // UTC midnight on a Monday

using Stamps = std::vector<std::int64_t>;

// Every `step` from `from` (inclusive) to `until` (exclusive).
Stamps span(std::int64_t from, std::int64_t until, std::int64_t step) {
    Stamps stamps;
    for (std::int64_t at = from; at < until; at += step) stamps.push_back(at);
    return stamps;
}

// Rows at the given minutes after kBase.
Stamps at_minutes(std::initializer_list<std::int64_t> minutes) {
    Stamps stamps;
    for (const std::int64_t m : minutes) stamps.push_back(kBase + m * kMin);
    return stamps;
}

// A script grid: buckets of `length` ms, one of which opens at `anchor`. Every row of a scenario is at or
// after its anchor, so the division below is a floor.
struct Grid {
    std::int64_t anchor;
    std::int64_t length;
    std::int64_t key(std::int64_t at) const { return anchor + ((at - anchor) / length) * length; }
};

// ORACLE 1: the number of buckets the strictly increasing timestamps open (a bucket opens when its key
// differs from the preceding row's).
std::uint64_t opened_buckets(const Stamps& stamps, const Grid& grid) {
    std::uint64_t opened = 0;
    bool have = false;
    std::int64_t last = 0;
    for (const std::int64_t at : stamps) {
        const std::int64_t key = grid.key(at);
        if (!have || key != last) {
            ++opened;
            last = key;
            have = true;
        }
    }
    return opened;
}

// ORACLE 2: the index of the first row that would open bucket number limit + 1 (the row a refusal must
// stop at); stamps.size() when the rows open at most `limit` buckets.
std::size_t crossing_index(const Stamps& stamps, const Grid& grid, std::uint64_t limit) {
    std::uint64_t opened = 0;
    bool have = false;
    std::int64_t last = 0;
    for (std::size_t i = 0; i < stamps.size(); ++i) {
        const std::int64_t key = grid.key(stamps[i]);
        if (!have || key != last) {
            if (opened == limit) return i;
            ++opened;
            last = key;
            have = true;
        }
    }
    return stamps.size();
}

// The canonical args of a refusal at `limit`: used (before the attempted delta) equals the limit, requested
// is 1; all three are canonical unsigned decimal text, keys sorted, no whitespace.
std::string expected_args(std::uint64_t limit) {
    const std::string n = std::to_string(limit);
    return "{\"limit\":\"" + n + "\",\"requested\":\"1\",\"used\":\"" + n + "\"}";
}

// A deterministic, varied OHLCV record for row number `i`: every field changes from row to row (no repeated
// seed row), every value is an exact binary fraction (the CSV text is short and exact), and
// low <= open, close <= high always holds (the reader's grammar).
fixture::CsvRecord record_at(std::size_t i, std::int64_t at) {
    const double open = 100.0 + static_cast<double>(i % 7) * 0.25;
    const double high = open + 1.0 + static_cast<double>(i % 3) * 0.5;
    const double low = open - 1.0 - static_cast<double>(i % 5) * 0.125;
    const double close = open + (static_cast<double>(i % 4) - 1.5) * 0.25;
    const double volume = 10.0 + static_cast<double>(i % 11);
    return fixture::make_record(at, open, high, low, close, volume);
}

std::string csv_of(const Stamps& stamps) {
    std::vector<fixture::CsvRecord> records;
    records.reserve(stamps.size());
    for (std::size_t i = 0; i < stamps.size(); ++i) records.push_back(record_at(i, stamps[i]));
    return fixture::make_csv(records);
}

struct Case {
    seam::Request request;
    Stamps rows;  // the primary data timestamps
    Grid grid{0, 1};
    seam::Streams streams;
};

Case make_case(const Stamps& rows, const Grid& grid, const seam::Request& request) {
    Case c;
    c.request = request;
    c.rows = rows;
    c.grid = grid;
    c.streams.primary = csv_of(rows);
    return c;
}

// A request on the UTC 24x7 clock with the caller's boundaries.
seam::Request utc_request(std::int64_t start_ms, std::int64_t end_ms, std::int64_t fed_start_ms,
                          std::uint32_t preroll, const char* input_tf, const char* script_tf,
                          bool feed_tolerant = true) {
    seam::Request request;
    request.start_ms = start_ms;
    request.end_ms = end_ms;
    request.fed_start_ms = fed_start_ms;
    request.preroll_bars = preroll;
    request.input_tf = input_tf;
    request.script_tf = script_tf;
    request.chart_timezone = "UTC";
    request.engine_timezone = "UTC";
    request.session = "";
    request.feed_tolerant = feed_tolerant;
    return request;
}

seam::Request hourly_request(std::int64_t t_hours, std::int64_t e_hours, std::uint32_t preroll) {
    return utc_request(kBase + t_hours * kHour, kBase + e_hours * kHour, kBase, preroll, "1", "60");
}

constexpr Grid kHourGrid{kBase, kHour};

void describe(const seam::Outcome& got, const char* wanted) {
    if (got.result == seam::Result::NotAdmitted) ++setup_failures;
    const char* what = got.result == seam::Result::Admitted      ? "Admitted"
                       : got.result == seam::Result::Refused     ? "Refused"
                                                                  : "NotAdmitted";
    std::fprintf(stderr, "  [%s] wanted %s, driver answered %s%s%s%s%s, sink received %zu row(s)\n", scenario,
                 wanted, what, got.not_admitted.empty() ? "" : " / ", got.not_admitted.c_str(),
                 got.detail.empty() ? "" : " / ", got.detail.c_str(), got.delivered.size());
}

// The sink received exactly the first `count` rows of the primary stream, in order.
bool delivered_prefix(const seam::Outcome& got, const Stamps& rows, std::size_t count) {
    if (got.delivered.size() != count || count > rows.size()) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (got.delivered[i].timestamp != rows[i]) return false;
    }
    return true;
}

void expect_admitted(const Case& c, std::optional<std::uint64_t> limit,
                     std::optional<std::uint64_t> carrier_source_rows = std::nullopt) {
    const seam::Outcome got = seam::admit(c.request, c.streams, limit, carrier_source_rows);
    CHECK(got.result == seam::Result::Admitted);
    if (got.result != seam::Result::Admitted) {
        describe(got, "Admitted");
        return;
    }
    const std::uint64_t expected_script_bars =
        limit ? opened_buckets(c.rows, c.grid) : std::uint64_t{0};
    CHECK(got.state.has_value());
    if (got.state) CHECK(got.state->script_bars == expected_script_bars);
    CHECK(delivered_prefix(got, c.rows, c.rows.size()));  // every record of an admitted stream is delivered
}

// All bounds absent: nothing is limited. The driver may run the stream (Completed, every row delivered) or
// answer that no field was requested (NotAdmitted / NoRequestedFields, nothing read or delivered): both are
// "no limit"; a refusal or any other answer is a failure.
void expect_no_limit_all_absent(const Case& c) {
    const seam::Outcome got = seam::admit(c.request, c.streams, std::nullopt);
    const bool ran = got.result == seam::Result::Admitted;
    const bool none_requested =
        got.result == seam::Result::NotAdmitted && got.not_admitted == "NoRequestedFields";
    CHECK(ran || none_requested);
    if (!ran && !none_requested) {
        describe(got, "Admitted or NoRequestedFields");
        return;
    }
    if (ran) {
        CHECK(got.state.has_value());
        if (got.state) CHECK(got.state->script_bars == 0);
        CHECK(delivered_prefix(got, c.rows, c.rows.size()));
    }
    if (none_requested) CHECK(got.delivered.empty());
}

seam::Outcome expect_refused(const Case& c, std::uint64_t limit) {
    seam::Outcome got = seam::admit(c.request, c.streams, limit);
    CHECK(got.result == seam::Result::Refused);
    if (got.result != seam::Result::Refused) {
        describe(got, "Refused");
        return got;
    }
    CHECK(got.code_name == kCodeName);
    CHECK(got.failure_class && *got.failure_class == pineforge::RunFailureClass::resource);
    CHECK(got.retryable && !*got.retryable);
    CHECK(got.args_json == expected_args(limit));
    CHECK(got.what == kEnglish);
    // The sink never saw the crossing record, and saw every record before it.
    CHECK(delivered_prefix(got, c.rows, crossing_index(c.rows, c.grid, limit)));
    return got;
}

// The ladder every counting scenario climbs: N passes, N-1 refuses, all-absent is no limit, a far larger
// limit passes.
void ladder(const Case& c) {
    const std::uint64_t opened = opened_buckets(c.rows, c.grid);
    CHECK(opened >= 2);  // fixture sanity: N-1 is a real bound and not an explicit zero
    expect_admitted(c, opened);
    expect_refused(c, opened - 1);
    expect_no_limit_all_absent(c);
    expect_admitted(c, opened + 1000);
}

std::vector<pineforge::Bar> as_bars(const Stamps& stamps) {
    std::vector<pineforge::Bar> bars;
    bars.reserve(stamps.size());
    for (std::size_t i = 0; i < stamps.size(); ++i) {
        const fixture::CsvRecord r = record_at(i, stamps[i]);
        bars.push_back(pineforge::Bar{r.open, r.high, r.low, r.close, r.volume, r.timestamp});
    }
    return bars;
}

pineforge::SelectedPrimaryPlan plan_of(const Stamps& stamps, const seam::Request& request) {
    const std::vector<pineforge::Bar> rows = as_bars(stamps);
    return pineforge::plan_selected_primary(rows.data(), rows.size(), request);
}

// ---- scenarios --------------------------------------------------------------------------------------------

void s1_six_full_hours() {
    scenario = "S1 six full hours";
    const Stamps rows = span(kBase, kBase + 6 * kHour, kMin);  // 360 rows
    const Case c = make_case(rows, kHourGrid, hourly_request(3, 6, 3));
    const std::uint64_t opened = opened_buckets(rows, kHourGrid);
    CHECK(opened == 6);  // hours 0..5, each opened by its first minute
    // fixture validity, against the existing planner: every bucket is sealed here, so its supplied count is
    // the same number (3 before T + 3 sealed groups in the selected range)
    const pineforge::SelectedPrimaryPlan plan = plan_of(rows, c.request);
    CHECK(plan.status == pineforge::SelectedPlanStatus::Ok);
    CHECK(plan.supplied_script_bars == opened);
    ladder(c);
}

void s2_partial_final_bucket() {
    scenario = "S2 partial final bucket";
    const Stamps rows = span(kBase, kBase + 7 * kHour + 30 * kMin, kMin);  // minutes 0..449, 450 rows
    const Case c = make_case(rows, kHourGrid, hourly_request(3, 8, 3));
    const std::uint64_t opened = opened_buckets(rows, kHourGrid);
    CHECK(opened == 8);  // hour 7 holds 30 rows and is never sealed, but it IS opened
    const pineforge::SelectedPrimaryPlan plan = plan_of(rows, c.request);
    CHECK(plan.status == pineforge::SelectedPlanStatus::Ok);
    CHECK(plan.supplied_script_bars < opened);  // the sealed count (7) misses the unsealed tail
    ladder(c);
}

void s3_preroll_trim() {
    scenario = "S3 pre-roll trim";
    const Stamps rows = span(kBase, kBase + 10 * kHour, kMin);  // ten full hours, 600 rows
    const Case c = make_case(rows, kHourGrid, hourly_request(5, 10, 2));  // keep two pre-roll buckets, trim three
    const std::uint64_t opened = opened_buckets(rows, kHourGrid);
    CHECK(opened == 10);
    const pineforge::SelectedPrimaryPlan plan = plan_of(rows, c.request);
    CHECK(plan.status == pineforge::SelectedPlanStatus::Ok);
    CHECK(plan.trimmed_script_bars == 3 && plan.fed_script_bars < opened);  // fed = 2 kept + 5 in range = 7
    ladder(c);
    expect_refused(c, plan.fed_script_bars);  // the fed count is NOT the bound
}

void s4_gaps_coarse_input() {
    scenario = "S4 gaps, input 5 script 15";
    const Stamps rows = at_minutes({0, 5, 10, 15, 30, 35, 60, 65, 70, 75, 120});
    const seam::Request request =
        utc_request(kBase + 30 * kMin, kBase + 135 * kMin, kBase, 5, "5", "15");
    const Grid grid{kBase, 15 * kMin};
    const Case c = make_case(rows, grid, request);
    CHECK(opened_buckets(rows, grid) == 6);  // keys 0, 15, 30, 60, 75, 120 min: by key, not by spacing
    CHECK(plan_of(rows, request).status == pineforge::SelectedPlanStatus::Ok);
    ladder(c);
}

void s5_other_roles_do_not_charge() {
    scenario = "S5 other roles";
    // E is 48 h so that an auxiliary stream of 40 hourly rows stays inside [F, E) like every other feed,
    // while the primary stream ends after six hours (a short primary feed is a valid input).
    const Stamps rows = span(kBase, kBase + 6 * kHour, kMin);
    Case c = make_case(rows, kHourGrid, hourly_request(3, 48, 3));
    // The same bytes in a second role, as a SEPARATE stream object (admit() builds one istringstream per
    // input), so the case holds under both readings of "duplicate" (a second descriptor of one role, or one
    // stream object presented twice). Exactly one Primary, one Source and one Auxiliary stream are supplied.
    c.streams.source = c.streams.primary;
    Stamps wide;                           // an auxiliary stream with MORE distinct buckets than the primary
    for (std::int64_t h = 0; h < 40; ++h) wide.push_back(kBase + h * kHour);
    c.streams.auxiliary.push_back(csv_of(wide));
    CHECK(opened_buckets(rows, kHourGrid) == 6 && opened_buckets(wide, kHourGrid) == 40);
    CHECK(plan_of(rows, c.request).status == pineforge::SelectedPlanStatus::Ok);
    ladder(c);  // a guard that also counted the auxiliary (or Source) role would refuse at limit 6
}

void s6_absent_is_no_limit() {
    scenario = "S6 absent limit over 300 buckets";
    Stamps rows;  // one row per hour: every row opens a bucket
    for (std::int64_t h = 0; h < 300; ++h) rows.push_back(kBase + h * kHour);
    Case c = make_case(rows, kHourGrid, hourly_request(150, 300, 5));
    CHECK(opened_buckets(rows, kHourGrid) == 300);
    CHECK(plan_of(rows, c.request).status == pineforge::SelectedPlanStatus::Ok);  // fixture validity
    expect_no_limit_all_absent(c);
    expect_admitted(c, 300);
    expect_refused(c, 299);
    // ABSENT WHILE THE DRIVER IS ACTIVE: script_bars absent, source_rows finite and huge, observed by this
    // file's own inert observer (carrier::inert_source_rows). The driver does real work (it requires and reads
    // the Source stream) and must still deliver all 300 primary rows: no script_bars refusal and no script_bars
    // demand on the stream.
    scenario = "S6b absent script_bars next to a finite source_rows";
    c.streams.source = c.streams.primary;
    expect_admitted(c, std::nullopt, UINT64_MAX);
}

void s7_explicit_zero_is_finite() {
    scenario = "S7 explicit zero";
    const Stamps rows = span(kBase, kBase + 6 * kHour, kMin);
    const Case c = make_case(rows, kHourGrid, hourly_request(3, 6, 3));
    CHECK(crossing_index(rows, kHourGrid, 0) == 0);
    CHECK(plan_of(rows, c.request).status == pineforge::SelectedPlanStatus::Ok);  // fixture validity
    expect_refused(c, 0);  // (limit, used, requested) = (0, 0, 1) at the first record, sink empty
}

void s8_refusal_stops_at_the_crossing_record() {
    scenario = "S8 refusal stops at the crossing record, whatever follows";
    // limit 2: hours 0 and 1 open buckets 1 and 2 (120 rows); hour 2's first row would open the third.
    // The tail behind the crossing is 2, 30 and 200 days of minute rows.
    for (const std::int64_t days : {std::int64_t{2}, std::int64_t{30}, std::int64_t{200}}) {
        const Stamps rows = span(kBase, kBase + days * kDay, kMin);
        const Case c = make_case(rows, kHourGrid, hourly_request(24, days * 24, 5));
        CHECK(crossing_index(rows, kHourGrid, 2) == 120);  // fixture sanity
        CHECK(plan_of(rows, c.request).status == pineforge::SelectedPlanStatus::Ok);  // fixture validity
        const seam::Outcome got = expect_refused(c, 2);    // sink holds exactly the first 120 rows
        // On refusal, the stream position shows that only a small fraction of the long tail was read.
        // expect_refused checks the typed failure and the exact sink prefix before this position check.
        if (got.result == seam::Result::Refused && days >= 30) {
            CHECK(got.consumed_primary_bytes.has_value());
            if (got.consumed_primary_bytes) {
                CHECK(*got.consumed_primary_bytes < c.streams.primary.size() / 10);
            }
        }
    }
}

void s9_session_anchored_buckets() {
    scenario = "S9 session 0930-1630 anchors the hourly buckets at the open";
    const std::int64_t open = kBase + 9 * kHour + 30 * kMin;
    const Stamps rows = span(open, kBase + 16 * kHour + 30 * kMin, kMin);  // session minutes 0..419, 420 rows
    seam::Request request = utc_request(open + 3 * kHour,   // three hours after session open, the fourth bucket
                                        open + 24 * kHour,  // the next day's session open
                                        open, 3, "1", "60");
    request.session = "0930-1630";
    const Grid anchored{open, kHour};
    const Case c = make_case(rows, anchored, request);
    CHECK(opened_buckets(rows, anchored) == 7);   // bucket opens at session hours 0, 1, 2, 3, 4, 5, 6
    CHECK(opened_buckets(rows, kHourGrid) == 8);  // a UTC-hour guard would see eight (hours 9..16)
    CHECK(plan_of(rows, request).status == pineforge::SelectedPlanStatus::Ok);
    ladder(c);
}

void s10_equal_timeframes() {
    scenario = "S10 input = script = 1 minute: every row opens a bucket";
    // The common production chart is the 1-minute one: the input and script timeframes are equal (the native
    // pairing is Passthrough, FeedTolerant), a bucket is a single row, and the number of opened buckets is the
    // number of rows. This takes the driver's and the planner's raw-partition path, not the aggregating one.
    const Stamps rows = span(kBase, kBase + 3 * kHour, kMin);  // 180 rows, minutes 0..179
    const seam::Request request = utc_request(kBase + kHour, kBase + 3 * kHour, kBase, 5, "1", "1");
    const Grid minutes{kBase, kMin};
    const Case c = make_case(rows, minutes, request);
    CHECK(opened_buckets(rows, minutes) == 180);
    const pineforge::SelectedPrimaryPlan plan = plan_of(rows, request);
    CHECK(plan.status == pineforge::SelectedPlanStatus::Ok);
    CHECK(plan.fed_script_bars < opened_buckets(rows, minutes));  // the pre-roll beyond the kept 5 is trimmed
    ladder(c);
    expect_refused(c, plan.fed_script_bars);  // the fed count is NOT the bound
}

void s11_one_hour_split_over_two_primary_streams() {
    scenario = "S11 one hour split over two Primary streams";
    const Stamps first = span(kBase, kBase + 90 * kMin, kMin);
    const Stamps second = span(kBase + 90 * kMin, kBase + 3 * kHour, kMin);
    Stamps rows = first;
    rows.insert(rows.end(), second.begin(), second.end());
    const seam::Request request = hourly_request(1, 3, 1);
    Case c = make_case(rows, kHourGrid, request);
    c.streams.primary = csv_of(first);
    c.streams.primary_more.push_back(csv_of(second));
    CHECK(opened_buckets(rows, kHourGrid) == 3);
    CHECK(plan_of(rows, request).status == pineforge::SelectedPlanStatus::Ok);
    // The split must preserve the previous bucket across the Primary stream boundary. csv_of numbers the
    // records of each stream from zero, so the second stream's OHLCV values restart the pattern of the first;
    // only the timestamps matter to this scenario.
    ladder(c);
}

void s12_strict_feed_hourly() {
    scenario = "S12 strict feed hourly";
    const Stamps rows = span(kBase, kBase + 6 * kHour, kMin);
    const seam::Request request =
        utc_request(kBase + 3 * kHour, kBase + 6 * kHour, kBase, 3, "1", "60", false);
    const Case c = make_case(rows, kHourGrid, request);
    CHECK(opened_buckets(rows, kHourGrid) == 6);
    CHECK(plan_of(rows, request).status == pineforge::SelectedPlanStatus::Ok);
    ladder(c);
}

void s13_strict_feed_equal_timeframes() {
    scenario = "S13 strict feed equal timeframes";
    const Stamps rows = span(kBase, kBase + 3 * kHour, kMin);
    const seam::Request request =
        utc_request(kBase + kHour, kBase + 3 * kHour, kBase, 5, "1", "1", false);
    const Grid minutes{kBase, kMin};
    const Case c = make_case(rows, minutes, request);
    CHECK(opened_buckets(rows, minutes) == 180);
    CHECK(plan_of(rows, request).status == pineforge::SelectedPlanStatus::Ok);
    ladder(c);
}

void s14_primary_row_without_a_script_bucket_is_refused() {
    sc::Limits finite;
    finite.script_bars = sc::Bound{true, 8};
    sc::Limits absent;
    sc::State before;
    before.source_rows = 2;
    before.primary_rows = 5;
    before.script_bars = 3;
    before.auxiliary_rows = 7;
    before.bytes = 11;
    before.has_last_primary_bucket = true;
    before.last_primary_bucket = kBase;
    sc::RecordEvent event;
    event.role = sc::Role::Primary;
    event.timestamp_ms = kBase + 5 * kMin;
    event.has_script_bucket = false;
    event.script_bucket_key = kBase + kHour;

    const auto check_unchanged = [](const sc::State& actual, const sc::State& expected) {
        CHECK(actual.source_rows == expected.source_rows);
        CHECK(actual.primary_rows == expected.primary_rows);
        CHECK(actual.script_bars == expected.script_bars);
        CHECK(actual.auxiliary_rows == expected.auxiliary_rows);
        CHECK(actual.bytes == expected.bytes);
        CHECK(actual.has_last_primary_bucket == expected.has_last_primary_bucket);
        CHECK(actual.last_primary_bucket == expected.last_primary_bucket);
    };
    const auto expect_invariant = [&](sc::State& state, const sc::Limits& limits) {
        const sc::State initial = state;
        bool refused = false;
        try {
            sc::observe_script_bars(state, limits, event);
        } catch (const pineforge::coded<std::logic_error>& error) {
            refused = true;
            const pineforge::RunFailureValue& failure = error.run_failure();
            CHECK(std::string(pineforge::run_failure_code_name(failure.code)) == "engine_invariant");
            CHECK(pineforge::run_failure_code_class(failure.code)
                  == pineforge::RunFailureClass::engine_fault);
            CHECK(!pineforge::run_failure_code_retryable(failure.code));
            CHECK((failure.args ? *failure.args : std::string("{}")) == "{}");
            CHECK(std::string(error.what())
                  == "script-bar observer got a Primary row without a script bucket");
        }
        CHECK(refused && "Primary row without a script bucket must throw a coded logic_error");
        check_unchanged(state, initial);
    };

    scenario = "S14 Primary without a script bucket, finite bound";
    sc::State finite_state = before;
    expect_invariant(finite_state, finite);

    scenario = "S14 Primary without a script bucket, absent bound";
    sc::State absent_state = before;
    expect_invariant(absent_state, absent);

    scenario = "S14 Primary with a script bucket counts once";
    sc::RecordEvent bucketful = event;
    bucketful.has_script_bucket = true;
    sc::State counted = before;
    sc::observe_script_bars(counted, finite, bucketful);
    CHECK(counted.script_bars == 4);
    CHECK(counted.last_primary_bucket == bucketful.script_bucket_key);
    CHECK(counted.has_last_primary_bucket);
    CHECK(counted.source_rows == before.source_rows);
    CHECK(counted.primary_rows == before.primary_rows);
    CHECK(counted.auxiliary_rows == before.auxiliary_rows);
    CHECK(counted.bytes == before.bytes);

    scenario = "S14 Primary with a script bucket at saturation";
    sc::State saturated_before = before;
    saturated_before.script_bars = 8;
    sc::State saturated = saturated_before;
    bool capacity_refused = false;
    try {
        sc::observe_script_bars(saturated, finite, bucketful);
    } catch (const pineforge::coded<std::runtime_error>& error) {
        capacity_refused = true;
        const pineforge::RunFailureValue& failure = error.run_failure();
        CHECK(std::string(pineforge::run_failure_code_name(failure.code))
              == "selected_script_bars_limit_exceeded");
    }
    CHECK(capacity_refused && "A new script bucket at the bound must throw a coded capacity failure");
    check_unchanged(saturated, saturated_before);

    scenario = "S14 Primary without a script bucket at saturation";
    expect_invariant(saturated, finite);

    for (const sc::Role role : {sc::Role::Source, sc::Role::Auxiliary}) {
        scenario = role == sc::Role::Source ? "S14 Source without a script bucket"
                                          : "S14 Auxiliary without a script bucket";
        sc::RecordEvent other = event;
        other.role = role;
        sc::State unchanged = before;
        sc::observe_script_bars(unchanged, finite, other);
        check_unchanged(unchanged, before);
    }
}

}  // namespace

int main() {
    s1_six_full_hours();
    s2_partial_final_bucket();
    s3_preroll_trim();
    s4_gaps_coarse_input();
    s5_other_roles_do_not_charge();
    s6_absent_is_no_limit();
    s7_explicit_zero_is_finite();
    s8_refusal_stops_at_the_crossing_record();
    s9_session_anchored_buckets();
    s10_equal_timeframes();
    s11_one_hour_split_over_two_primary_streams();
    s12_strict_feed_hourly();
    s13_strict_feed_equal_timeframes();
    s14_primary_row_without_a_script_bucket_is_refused();
    if (setup_failures != 0) {
        std::fprintf(stderr, "NEEDS setup %d NotAdmitted answer(s)\n", setup_failures);
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
