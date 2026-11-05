// Selected-capacity script_bars bound.
//
// WHAT IT PINS. The caller's script_bars limit counts the script buckets opened by the supplied primary
// data: every bucket whose first row the stream carries, including pre-roll, pre-T rows and an unsealed
// final bucket. Each bucket uses the native script interval, anchored to the session when one is set.
// This is neither the planner's sealed or fed count (S2 and S3) nor a UTC-hour count (S9).
// used + requested > limit must throw selected_script_bars_limit_exceeded before the crossing record is
// materialized; used + requested == limit passes. An absent limit means no limit; explicit 0 is finite.
// Source and auxiliary roles never charge this bound, even when Source carries the same primary bytes.
// S1 covers full buckets; S2 an unsealed tail; S3 pre-roll trimming; S4 gaps and coarser input; S5 roles;
// S6 and S6b absence; S7 zero; S8 early stopping; S9 session anchoring; S10 equal input/script timeframes.
// Each counting scenario admits N opened buckets at limit N, refuses at N-1, accepts an absent bound
// without refusal, and admits a much larger finite limit. S2, S3 and S10 also test the smaller planner count.
//
// HOW THE EXPECTED VALUES ARE MADE. Every expected count is arithmetic on timestamps written into this
// file's own CSV text. A row's bucket key is anchor + floor((ts - anchor) / length); a bucket opens when
// its key differs from the preceding row's. The timestamp helpers and Grid supply the independent counts.
// The existing planner (plan_selected_primary) only establishes that fixtures are valid selected requests
// and demonstrates where sealed/fed counts differ from opened counts; it does not supply expected counts.
// For a refusal at limit L, the crossing bucket is number L + 1, so limit = L, used = L before the
// attempted increment, and requested = 1. All three arguments are canonical unsigned decimal text, with
// sorted keys and no whitespace. The typed code name and English message come from the failure catalog.
// The crossing row's index is computed from the same timestamp keys, independently of driver output.
// The sink must hold precisely the primary rows preceding that index, in their original order.
// CSV records vary every OHLCV field and use exact binary fractions within the reader's valid grammar.
// In S9, seven session-anchored buckets span eight UTC hours; in S10 each minute row opens one bucket.
//
// WHAT IT DOES NOT PIN. Early refusal is observed through the materialization sink: tails of 2, 30 and
// 200 days must leave exactly the rows before the crossing row in the sink. For the 30- and 200-day tails,
// S8 also requires the primary stream position when the refusal escapes to be under a tenth of the file.
// These observations establish that the long tail is not read; they do not measure internal buffers.
// This test installs no allocation hook and does not exercise the production entry's observer binding.
// It calls run_driver with an observer table supplied by this test. Unneeded slots remain null.
// The script_bars slot holds an observer that observes nothing and enforces nothing. It stands for the
// guard that does not exist yet and must be replaced by that guard's observer when it is implemented,
// with every assertion unchanged. The stand-in is local to this file and is separate from product symbols.
// No default observer, weak symbol, registration fallback or stub is supplied through product code.
// In S6b, a separate inert source_rows observer makes a finite source_rows bound require and read Source
// while script_bars is absent. It neither counts nor refuses; it only keeps the driver active for that case.
// The driver invokes an observer only for a present bound, so S6 and S6b test absence through the driver,
// rather than a branch in the script_bars observer. With all bounds absent, Completed or
// NotAdmitted / NoRequestedFields is acceptable; neither may refuse, and NoRequestedFields delivers no rows.
//
// WHAT FAILS IF THE GUARD IS WRONG. Counting sealed buckets accepts S2 at N-1 because its tail is unsealed.
// Counting fed buckets accepts S3 below N. Counting UTC hours accepts S9 below 8 and refuses its limit 7.
// Charging Source or auxiliary records refuses S5 at N. Counting rows or elapsed time gives wrong S4 results.
// Ignoring the limit never refuses. Refusing after delivering the crossing row, or after reading the
// whole input, fails S8. Treating explicit 0 as absent admits S7 instead of refusing its first record.
// Assuming several rows per bucket, a timeframe ratio or a nominal slot count instead of the native key
// fails S10, where input and script timeframes are equal and every row opens a bucket.
// A refusal with a different typed code, noncanonical arguments or different English fails expect_refused.
// A setup failure, missing symbol or MissingObserver answer cannot establish enforcement of the limit.
// The refusal must propagate unchanged out of the driver; returning ObserverFailure instead makes the
// result NotAdmitted and fails the test. Unrelated typed failures, I/O errors and invariant failures escape.
// Only Primary records reach the materialization sink; Source and auxiliary records must stay separate.
// With the stand-in observer, the real reader, native calendar and sink run, but no script_bars limit is
// enforced: every finite-bound call is admitted and the sink receives every supplied primary row.
// Each expected refusal therefore fails at CHECK(got.result == seam::Result::Refused) in expect_refused.
// S1 through S5 and S7 through S10 reach that assertion; S6 also reaches it at limit 299.
// There are 15 such refusal checks, including the extra smaller-count cases and S8's three tail lengths.
// Exact-limit, larger-limit and absent-bound calls should still pass with this observer.
// Failure output identifies the scenario, file, assertion and line; describe() prints the driver's answer
// and the number of rows received by the sink. A bound violation must fail the refusal assertion,
// rather than being mistaken for a build, link or setup error.
// These failures demonstrate that the limit is not enforced by this tree. After the guard is supplied,
// the same assertions establish its typed refusal and exact arguments, equality behaviour and early stop.
// Leaving the stand-in in the script_bars slot still fails at the same refusal assertion.
// The Source observer used by S6b remains inert so that no other capacity check masks script_bars absence.
// S5 supplies separate stream objects for Primary, Source and Auxiliary, keeping role isolation explicit.
// S4 counts actual opened keys across gaps rather than deriving a bucket count from row spacing.
// S8's limit 2 allows the first 120 minute rows; the next row opens the third hourly bucket.
// Its stream-position assertion runs only after an actual refusal; an admitted result fails earlier.
// S7's explicit zero expects limit = 0, used = 0 and requested = 1, with an empty sink.
// S6's 300 one-row buckets test that absence is not an implicit finite default.
// S10 also refuses at the smaller fed count, proving that trimming does not reduce the charged count.
// All expected values and assertions are independent of the stand-in's lack of enforcement.

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
    std::optional<std::string> source;  // the same bytes as the primary: one input, two roles
    std::vector<std::string> auxiliary;
};

enum class Result { Admitted, Refused, NotAdmitted };

struct Outcome {
    Result result = Result::Admitted;
    std::string code_name, args_json, what;  // Refused: the carried typed failure (canonical args)
    std::string not_admitted;                // NotAdmitted: the driver's reason, e.g. "MissingObserver"
    std::string detail;                      // the driver's message, for the failure text only
    std::vector<pineforge::Bar> delivered;   // what the materialization sink received, in order
    // Bytes of the primary stream the driver consumed: its coverage when it returns, the stream position when
    // the refusal escapes it.
    std::optional<std::uint64_t> consumed_primary_bytes;
};

// The observer table supplied by this test to run_driver. Only the slots needed by each call are set:
// script_bars and, in S6b, source_rows. The driver calls an observer only for a present bound.
// The callbacks are defined here, separate from the product archive.
namespace red_form {  // observer for a tree without the script_bars guard
// This observer observes nothing and enforces nothing: it stands for the guard that does not exist yet.
// Replace it in the table with that guard's observer when implemented, keeping every assertion unchanged.
// It is local to this file and has a distinct name from the product's observe_script_bars.
void pre_guard_observer(sc::State&, const sc::Limits&, const sc::RecordEvent&) {}
}  // namespace red_form

namespace carrier {  // inert Source reader for the absent-bound scenario
// S6b only: an inert source_rows observer. A finite source_rows bound makes the driver require and read a
// Source stream, so the driver does real work while script_bars is absent. This callback only enables that
// read: it counts nothing and refuses nothing. It is supplied by this test and requires no other capacity
// observer implementation.
void inert_source_rows(sc::State&, const sc::Limits&, const sc::RecordEvent&) {}
}  // namespace carrier

sc::ObserverTable table_under_test(bool with_source_rows) {
    sc::ObserverTable table;  // every slot null
    table.script_bars = &red_form::pre_guard_observer;  // stands for the script_bars guard not yet implemented
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
// still does real driver work). Catches ONLY the script_bars typed refusal; every other exception (another
// typed code, an I/O or format failure, an invariant) propagates and fails the test.
// The typed refusal propagates out of the driver unchanged, including its coded arguments and English.
// ObserverFailure and SinkFailure are reserved and never returned. The exception reserve() throws is
// coded<std::runtime_error> with the code and the canonical
// args. If a later driver returned ObserverFailure instead, the result below would read NotAdmitted /
// ObserverFailure and the test would fail loudly instead of passing.
Outcome admit(const Request& request, const Streams& streams, std::optional<std::uint64_t> script_bars,
              std::optional<std::uint64_t> carrier_source_rows = std::nullopt) {
    Outcome out;
    std::istringstream primary(streams.primary);
    std::unique_ptr<std::istringstream> source;
    std::vector<std::unique_ptr<std::istringstream>> auxiliary;
    std::vector<sc::StreamInput> inputs;
    inputs.emplace_back(sc::Role::Primary, primary, "primary");
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
    } catch (const std::runtime_error& error) {
        const pineforge::RunFailureValue failure = pineforge::classify_run_failure(error);
        const std::string name = pineforge::run_failure_code_name(failure.code);
        if (name != kCodeName) throw;
        out.result = Result::Refused;
        out.code_name = name;
        out.args_json = failure.args ? *failure.args : std::string();
        out.what = error.what();
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
                          std::uint32_t preroll, const char* input_tf, const char* script_tf) {
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
    request.feed_tolerant = true;
    return request;
}

seam::Request hourly_request(std::int64_t t_hours, std::int64_t e_hours, std::uint32_t preroll) {
    return utc_request(kBase + t_hours * kHour, kBase + e_hours * kHour, kBase, preroll, "1", "60");
}

constexpr Grid kHourGrid{kBase, kHour};

void describe(const seam::Outcome& got, const char* wanted) {
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
    if (ran) CHECK(delivered_prefix(got, c.rows, c.rows.size()));
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
    expect_refused(c, plan.supplied_script_bars);  // the sealed count is NOT the bound
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
        // The refusal happens AT the crossing record, so a tail of 30 or 200 days is never read: measured on
        // the stream itself (its position when the refusal escaped), and only on an actual refusal (the
        // missing guard already fails at expect_refused). A bounded reader has consumed a small fraction of a
        // file this long, not the file.
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
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
