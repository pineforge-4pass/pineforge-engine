// Selected-window route controls: OFF against selected on the batch and stream routes,
// the failure paths, the single read and the report-only terminal quote, against the
// frozen observer ABI (pineforge/execution_observer.h) and the frozen selected-window ABI
// (pineforge/selected_window.h). The lookback null triple and the 1.3-parity capabilities
// literal of the wire contract are not read by this native-level TU.
//
// Real-system rows driven only from outside, through the public entries a caller
// has: BacktestEngine::run (simple, tf and rich overloads), the stream_begin /
// stream_push_bar / stream_end family, fill_report, the C exports
// strategy_set_selected_window_v1 / strategy_selected_window_counts_v1 /
// strategy_set_execution_observer_v1 / strategy_execution_observation_v1 /
// strategy_state_query_status_v1, the native C host (strategy_native_host_create_v1,
// strategy_configure_native_ext_v1, strategy_native_run_v1 with an on_lot_excursion
// hook) and the Pine source host (PineStrategyHost, KernelRecordedAtHostMarks) with
// its public bool setter set_report_terminal_quote. No private bridge, no consumer
// header, no stub callback and no payload built by hand.
//
// The counters are EXTERNAL. Every host callback, every presentation call and the
// observer note themselves in one test-owned event log (g_log); the checks read that
// log, never a value the implementation computed about itself. The one value taken
// from the implementation is the execution observation's phase seen from INSIDE the
// observer (Sealed, results closed) -- the only externally visible fact that tells
// "observer before the pure final work" from "observer after it".
//
// Rows (stable labels, printed as "ROW <label>"; the table in main() lists them in run
// order): the synthetic controls of the row dispatcher, then the two log predicates that
// the negative controls rely on (shown to reject a deliberately broken log), then the
// batch routes (simple, tf, rich) and the stream end, the failure paths (failed, aborted,
// observer fault, pre-admission refusal), the single-read call counts, the terminal
// quote (preset and late), the Pine product route with a retained pre-roll, and the real
// planner's counts against a real run over the rows the plan keeps.
//
// Verdicts: a row is a PASS only with at least one check, no failed check and no
// exception. The run stops before the first row that is not a PASS, lists every later
// row NOT RUN, prints COMPLETE only when every row ran and passed, and exits 0 only
// then. Every array a positive length promises must be present: a missing array is a
// FAIL and its comparison is never skipped as a pass.
//
// OFF comparison: every OFF-vs-selected row starts the window at the first row
// (N = 0 pre-roll, the window covers every actual bar), so the selected curve is the
// anchor plus the SAME M observations the OFF curve holds. The rows compare the
// presented trades and scalars, the M observations point for point (selected
// points 1..M against OFF points 0..M-1) and the metrics the documented report semantics
// leave equal (exposure over M observations, open P&L, drawdown, buy-and-hold).
// They never assert that the anchorless OFF curve equals the selected curve. The
// pre-roll row (ROUTES-PINE-TF-PREROLL) compares only the source-bar count with an OFF
// run; its counts, curve and trades are worked out from the rows.
//
// Negative controls (what each external check rejects):
//   * observer moved after the pure final work / results open: probe.inside.phase must
//     be Sealed (3) and the counts getter must still answer -2 INSIDE the observer;
//   * an extra post-seal host dispatch (a host callback or a presentation after the
//     observer, including during the single read): host_events_after_observer() must
//     be 0 over the log that runs through the read;
//   * the observer firing zero or several times, or before the last permitted host
//     work: exactly one "observer" event, after every "host." event, and the event
//     just before it is the capture's presentation.
//
// The late terminal-quote row expects the product's bool setter
// set_report_terminal_quote to refuse a post-seal call with no effect. The void routes
// (set_syminfo_metadata quote keys, clear_report_terminal_quote,
// strategy_set_syminfo_metadata) are NOT asserted here; test_selected_quote_seal covers
// them. Every row is expected to pass if the product meets the cited contracts; a wrong
// assumption in an early row stops the later rows, which then list NOT RUN.
//
// Not covered by this TU: the generated C wrapper, the
// native_module publish_report route and the C-ABI stream family (strategy_stream_*).
//
// Source-bound (includes pineforge/source/pine_strategy_host.hpp for the Pine rows), like
// test_report_terminal_quote; the kernel-only reach filter in tests/CMakeLists.txt skips
// it when PINEFORGE_BUILD_SOURCE_LAYER is OFF. Registered by one line in the selected-window
// block of TEST_SOURCES.
#include <pineforge/execution_observer.h>
#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>
#include <pineforge/native_toolkit.hpp>
#include <pineforge/selected_window.h>
#include <pineforge/selected_window_plan.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

// ---- tally -------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
int g_row_checks = 0;
int g_row_failures = 0;
const char* g_row = "setup";
// True only while a synthetic control runs a deliberately failing body through the real
// dispatcher: it silences the console and the sandbox below discards the tallies.
bool g_quiet = false;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        ++g_row_checks;                                                               \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            ++g_row_failures;                                                         \
            if (!g_quiet) std::fprintf(stderr, "CHECK FAIL [%s] %s:%d  %s\n", g_row, __FILE__, \
                         __LINE__, #cond);                                            \
        }                                                                             \
    } while (0)

// ---- row dispatcher -----------------------------------------------------------------
//
// A row is a PASS only when it made at least one check, failed none and threw nothing: a
// row that checked nothing proved nothing and is a FAIL. The sequence stops before the
// first row that is not a PASS; every row after it is recorded NOT RUN and is never
// counted as passed, and the run reads COMPLETE only when every row ran and passed.

enum class RowVerdict { Pass, ZeroChecks, Failed, Threw, NotRun };

const char* verdict_word(RowVerdict verdict) {
    switch (verdict) {
    case RowVerdict::Pass: return "PASS";
    case RowVerdict::ZeroChecks: return "FAIL (no checks)";
    case RowVerdict::Failed: return "FAIL";
    case RowVerdict::Threw: return "FAIL (threw)";
    case RowVerdict::NotRun: return "NOT RUN";
    }
    return "NOT RUN";
}

// The one verdict of a finished row. An exception outranks a failed check, which
// outranks an empty row; only a clean row with checks is a PASS.
RowVerdict judge_row(int checks, int failures, bool threw) {
    if (threw) return RowVerdict::Threw;
    if (failures != 0) return RowVerdict::Failed;
    if (checks <= 0) return RowVerdict::ZeroChecks;
    return RowVerdict::Pass;
}

struct RowSpec {
    const char* label;
    void (*body)();
};

struct RowOutcome {
    std::string label;
    RowVerdict verdict;
    int checks;
    int failures;
};

// Runs one row body under its own tally and judges it; the totals include the row.
RowOutcome execute_row(const RowSpec& row) {
    g_row = row.label;
    g_row_checks = 0;
    g_row_failures = 0;
    if (!g_quiet) std::printf("ROW %s\n", row.label);
    bool threw = false;
    try {
        row.body();
    } catch (const std::exception& e) {
        threw = true;
        if (!g_quiet) std::fprintf(stderr, "ROW EXCEPTION [%s] %s\n", row.label, e.what());
    } catch (...) {
        threw = true;
        if (!g_quiet) {
            std::fprintf(stderr, "ROW EXCEPTION [%s] non-standard exception\n", row.label);
        }
    }
    if (threw) {
        ++g_failures;
        ++g_row_failures;
    }
    const RowVerdict verdict = judge_row(g_row_checks, g_row_failures, threw);
    if (!g_quiet) {
        std::printf("ROW %s: %d checks, %d failures: %s\n", row.label, g_row_checks,
                    g_row_failures, verdict_word(verdict));
    }
    return RowOutcome{row.label, verdict, g_row_checks, g_row_failures};
}

// A synthetic row through the real dispatcher: its deliberate failures and its console
// noise never reach the real tallies. The row that called this keeps its own counts.
RowOutcome execute_synthetic(const RowSpec& row) {
    const int checks = g_checks;
    const int failures = g_failures;
    const int row_checks = g_row_checks;
    const int row_failures = g_row_failures;
    const char* const label = g_row;
    const bool quiet = g_quiet;
    g_quiet = true;
    const RowOutcome outcome = execute_row(row);
    g_quiet = quiet;
    g_checks = checks;
    g_failures = failures;
    g_row_checks = row_checks;
    g_row_failures = row_failures;
    g_row = label;
    return outcome;
}

// Runs `rows` in order through `run_one` and stops before the first row after one that
// is not a PASS: every later row is recorded NOT RUN without being run. True only when
// no row blocked the sequence.
bool run_sequence(const RowSpec* rows, std::size_t count, RowOutcome (*run_one)(const RowSpec&),
                  std::vector<RowOutcome>* outcomes) {
    bool blocked = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (blocked) {
            outcomes->push_back(RowOutcome{rows[i].label, RowVerdict::NotRun, 0, 0});
            continue;
        }
        const RowOutcome outcome = run_one(rows[i]);
        outcomes->push_back(outcome);
        if (outcome.verdict != RowVerdict::Pass) blocked = true;
    }
    return !blocked;
}

// COMPLETE needs exactly the expected rows recorded and every one of them a PASS: a row
// that is missing, NOT RUN or not a PASS keeps the whole run from reading COMPLETE.
bool sequence_complete(const std::vector<RowOutcome>& outcomes, std::size_t expected) {
    if (expected == 0 || outcomes.size() != expected) return false;
    for (const RowOutcome& outcome : outcomes) {
        if (outcome.verdict != RowVerdict::Pass) return false;
    }
    return true;
}

// ---- synthetic checker controls (they run before any real row) --------------------------
//
// They feed the real dispatcher bodies that must NOT read as PASS -- a body that checks
// nothing, a body with a failed check, a body that throws -- and one that must. The
// external counter g_synthetic_runs counts the bodies that really ran, so a sequencer
// that kept going after a bad row is caught by a number it did not compute itself.

int g_synthetic_runs = 0;

void synthetic_pass_row() {
    ++g_synthetic_runs;
    CHECK(g_synthetic_runs > 0);
}

void synthetic_empty_row() { ++g_synthetic_runs; }

void synthetic_failing_row() {
    ++g_synthetic_runs;
    CHECK(g_synthetic_runs < 0);
}

void synthetic_throwing_row() {
    ++g_synthetic_runs;
    CHECK(g_synthetic_runs > 0);
    throw std::runtime_error("synthetic row threw");
}

// The verdict for a zero-check row, a failed row and a throwing row is never PASS, on the
// verdict function and on the dispatcher that applies it.
void row_control_verdicts() {
    CHECK(judge_row(3, 0, false) == RowVerdict::Pass);
    CHECK(judge_row(0, 0, false) == RowVerdict::ZeroChecks);
    CHECK(judge_row(3, 1, false) == RowVerdict::Failed);
    CHECK(judge_row(0, 1, false) == RowVerdict::Failed);
    CHECK(judge_row(3, 0, true) == RowVerdict::Threw);
    CHECK(judge_row(0, 0, true) == RowVerdict::Threw);
    CHECK(judge_row(4, 2, true) == RowVerdict::Threw);

    g_synthetic_runs = 0;
    const RowOutcome passing = execute_synthetic(RowSpec{"synthetic-pass", &synthetic_pass_row});
    CHECK(passing.verdict == RowVerdict::Pass);
    CHECK(passing.checks == 1);
    CHECK(passing.failures == 0);
    const RowOutcome silent = execute_synthetic(RowSpec{"synthetic-empty", &synthetic_empty_row});
    CHECK(silent.verdict == RowVerdict::ZeroChecks);
    CHECK(silent.verdict != RowVerdict::Pass);
    CHECK(silent.checks == 0);
    const RowOutcome failing = execute_synthetic(RowSpec{"synthetic-fail", &synthetic_failing_row});
    CHECK(failing.verdict == RowVerdict::Failed);
    CHECK(failing.verdict != RowVerdict::Pass);
    CHECK(failing.failures == 1);
    const RowOutcome thrown = execute_synthetic(RowSpec{"synthetic-throw", &synthetic_throwing_row});
    CHECK(thrown.verdict == RowVerdict::Threw);
    CHECK(thrown.verdict != RowVerdict::Pass);
    CHECK(g_synthetic_runs == 4);
    // The deliberate failures stayed inside the sandbox: this row's own tally is clean.
    CHECK(g_row_failures == 0);
    CHECK(g_failures == 0);
}

// The real sequencer stops before any row after a zero-check, failed or throwing row, never
// runs it (the external counter), records it NOT RUN, and COMPLETE needs every row a PASS.
void row_control_sequence() {
    std::vector<RowOutcome> ran;

    const RowSpec after_empty[] = {{"seq-pass", &synthetic_pass_row},
                                   {"seq-empty", &synthetic_empty_row},
                                   {"seq-after-1", &synthetic_pass_row},
                                   {"seq-after-2", &synthetic_failing_row}};
    g_synthetic_runs = 0;
    CHECK(!run_sequence(after_empty, 4, &execute_synthetic, &ran));
    CHECK(g_synthetic_runs == 2);
    CHECK(ran.size() == 4);
    if (ran.size() == 4) {
        CHECK(ran[0].verdict == RowVerdict::Pass);
        CHECK(ran[1].verdict == RowVerdict::ZeroChecks);
        CHECK(ran[2].verdict == RowVerdict::NotRun);
        CHECK(ran[3].verdict == RowVerdict::NotRun);
    }
    CHECK(!sequence_complete(ran, 4));

    const RowSpec after_failed[] = {{"seq-fail", &synthetic_failing_row},
                                    {"seq-after", &synthetic_pass_row}};
    g_synthetic_runs = 0;
    ran.clear();
    CHECK(!run_sequence(after_failed, 2, &execute_synthetic, &ran));
    CHECK(g_synthetic_runs == 1);
    CHECK(ran.size() == 2);
    if (ran.size() == 2) {
        CHECK(ran[0].verdict == RowVerdict::Failed);
        CHECK(ran[1].verdict == RowVerdict::NotRun);
    }
    CHECK(!sequence_complete(ran, 2));

    const RowSpec after_thrown[] = {{"seq-throw", &synthetic_throwing_row},
                                    {"seq-after", &synthetic_pass_row}};
    g_synthetic_runs = 0;
    ran.clear();
    CHECK(!run_sequence(after_thrown, 2, &execute_synthetic, &ran));
    CHECK(g_synthetic_runs == 1);
    CHECK(ran.size() == 2);
    if (ran.size() == 2) {
        CHECK(ran[0].verdict == RowVerdict::Threw);
        CHECK(ran[1].verdict == RowVerdict::NotRun);
    }
    CHECK(!sequence_complete(ran, 2));

    const RowSpec all_pass[] = {{"seq-a", &synthetic_pass_row}, {"seq-b", &synthetic_pass_row}};
    g_synthetic_runs = 0;
    ran.clear();
    CHECK(run_sequence(all_pass, 2, &execute_synthetic, &ran));
    CHECK(g_synthetic_runs == 2);
    CHECK(ran.size() == 2);
    CHECK(sequence_complete(ran, 2));
    // A row that is missing from the record, or an empty record, is never COMPLETE.
    CHECK(!sequence_complete(ran, 3));
    CHECK(!sequence_complete(std::vector<RowOutcome>(), 0));
}

// ---- the external event log -----------------------------------------------------

bool is_host_event(const std::string& event) { return event.compare(0, 5, "host.") == 0; }

struct EventLog {
    std::vector<std::string> events;
    void clear() { events.clear(); }
    void note(const std::string& what) { events.push_back(what); }
    int count(const std::string& what) const {
        int n = 0;
        for (const std::string& event : events) n += event == what ? 1 : 0;
        return n;
    }
};
EventLog g_log;

int count_of(const std::vector<std::string>& log, const std::string& what) {
    int n = 0;
    for (const std::string& event : log) n += event == what ? 1 : 0;
    return n;
}

// How many host events follow the observer; -1 unless the observer ran exactly once.
int host_events_after_observer(const std::vector<std::string>& log) {
    int observer = -1;
    int observers = 0;
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i] == "observer") {
            if (observer < 0) observer = static_cast<int>(i);
            ++observers;
        }
    }
    if (observers != 1) return -1;
    int after = 0;
    for (std::size_t i = static_cast<std::size_t>(observer) + 1; i < log.size(); ++i) {
        after += is_host_event(log[i]) ? 1 : 0;
    }
    return after;
}

// The observer ran exactly once, after every host event; when `presentation_last`, the
// event just before it is the capture's presentation (the last permitted host work).
bool observer_follows_last_host_work(const std::vector<std::string>& log,
                                     bool presentation_last) {
    if (host_events_after_observer(log) != 0) return false;
    std::size_t observer = 0;
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i] == "observer") observer = i;
    }
    if (observer == 0) return false;
    if (!presentation_last) return true;
    return log[observer - 1] == "host.present_report";
}

// ---- numbers and shapes --------------------------------------------------------

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;
constexpr double kCapital = 1000.0;
constexpr std::int64_t kPineT0 = 1736121600000LL;  // 2025-01-06 00:00 UTC, 15m aligned

// The numbers pf_execution_observation_v1 carries (execution_observer.h).
constexpr std::uint32_t kPhaseSealed = 3;
constexpr std::uint32_t kPhaseResults = 4;
constexpr std::uint32_t kFaultNone = 0;
constexpr std::uint32_t kFaultExecution = 1;
constexpr std::uint32_t kFaultAfterExecution = 2;
constexpr std::uint32_t kOutcomeRefused = 1;
constexpr std::uint32_t kOutcomeFailed = 2;
constexpr std::uint32_t kOutcomeAborted = 3;
constexpr std::uint32_t kOutcomeResultsOpen = 4;

bool near(double actual, double expected, double tolerance) {
    return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
}

// Five one-minute rows: pre-window price 50, then 100 / 110 / 104.5 (the tape of
// test_selected_window_report_integration). A buy submitted at the close of script bar
// 0 fills at the open of bar 1 (50); a flatten submitted at the close of bar 2 fills at
// the open of bar 3 (100): one closed row.
const Bar kTape[5] = {
    {50.0, 50.0, 50.0, 50.0, 1.0, kT0},
    {50.0, 50.0, 50.0, 50.0, 1.0, kT0 + 1 * kMinute},
    {100.0, 100.0, 100.0, 100.0, 1.0, kT0 + 2 * kMinute},
    {100.0, 110.0, 100.0, 110.0, 1.0, kT0 + 3 * kMinute},
    {110.0, 110.0, 104.5, 104.5, 1.0, kT0 + 4 * kMinute},
};

NativeRunSpec spec_of(const char* key, const char* input_tf, const char* script_tf,
                      std::uint64_t run_number = 1) {
    NativeRunSpec s;
    s.identity = {key, run_number};
    s.input_tf = input_tf;
    s.script_tf = script_tf;
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = kCapital;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.report_policy = NativeReportPolicy::KernelRecorded;
    return s;
}

// The window [start_ms, start_ms + 1000 minutes): T is start_ms.
pf_selected_window_config_v1 window_from(std::int64_t start_ms) {
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof window);
    window.version = 1;
    window.start_ms = start_ms;
    window.end_ms = start_ms + 1000 * kMinute;
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

pf_execution_observation_v1 blank_observation() {
    pf_execution_observation_v1 seen{};
    seen.struct_size = static_cast<std::uint32_t>(sizeof seen);
    seen.version = 1;
    return seen;
}

// The handle every public C export takes for a host object.
pf_strategy_t handle_of(BacktestEngine& engine) { return static_cast<pf_strategy_t>(&engine); }

pf_execution_observation_v1 observation_of(pf_strategy_t handle) {
    pf_execution_observation_v1 seen = blank_observation();
    CHECK(strategy_execution_observation_v1(handle, &seen) == 0);
    return seen;
}

// A ReportC read, freed on every exit.
struct Report {
    ReportC raw{};
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
    ~Report() { BacktestEngine::free_report(&raw); }
};

// A pf_report_t filled by strategy_native_run_v1, freed on every exit.
struct CReport {
    pf_report_t raw;
    CReport() { std::memset(&raw, 0, sizeof raw); }
    CReport(const CReport&) = delete;
    CReport& operator=(const CReport&) = delete;
    ~CReport() { strategy_native_report_free_v1(&raw); }
};

// Nothing of a run is readable: the empty report, every array null, every count zero.
void check_empty_report(const ReportC& report) {
    CHECK(report.trades == nullptr);
    CHECK(report.trades_len == 0);
    CHECK(report.equity_curve == nullptr);
    CHECK(report.equity_curve_len == 0);
    CHECK(report.script_bars_processed == 0);
}

// ---- the observer ----------------------------------------------------------------

struct Probe {
    pf_strategy_t handle = nullptr;
    int rc = 0;
    int calls = 0;
    int observation_rc = 99;
    int counts_inside = 99;
    pf_execution_observation_v1 inside{};
    std::uint64_t boundary_generation = 0;
    std::uint64_t boundary_serial = 0;
};

// Notes itself first, then reads the two getters that are allowed inside the observer,
// fills the receipt's sentinel and answers probe->rc.
int observer_before_results(void* context, const pf_execution_boundary_v1* boundary,
                            pf_boundary_receipt_v1* receipt) {
    Probe* probe = static_cast<Probe*>(context);
    ++probe->calls;
    g_log.note("observer");
    probe->boundary_generation = boundary->run_generation;
    probe->boundary_serial = boundary->attempt_serial;
    probe->inside = blank_observation();
    probe->observation_rc = strategy_execution_observation_v1(probe->handle, &probe->inside);
    pf_selected_window_counts_v1 counts = blank_counts();
    probe->counts_inside = strategy_selected_window_counts_v1(probe->handle, &counts);
    receipt->frame_bytes = 0;
    receipt->handed_bytes = 0;
    receipt->export_requested = 0;
    return probe->rc;
}

pf_execution_observer_v1 observer_of(Probe* probe) {
    pf_execution_observer_v1 observer{};
    observer.struct_size = static_cast<std::uint32_t>(sizeof observer);
    observer.version = 1;
    observer.context = probe;
    observer.before_results = &observer_before_results;
    return observer;
}

// What the observer saw from inside: the generation is Sealed, nothing has failed, the
// attempt does not own results yet, and the counts are still closed. A kernel that ran
// the observer after the results opened (after the pure final work) fails here.
void check_observer_inside(const Probe& probe) {
    CHECK(probe.calls == 1);
    CHECK(probe.observation_rc == 0);
    CHECK(probe.inside.phase == kPhaseSealed);
    CHECK(probe.inside.fault_stage == kFaultNone);
    CHECK(probe.inside.attempt_outcome != kOutcomeResultsOpen);
    CHECK(probe.counts_inside == -2);
    CHECK(probe.boundary_generation == probe.inside.run_generation);
    CHECK(probe.boundary_serial == probe.inside.attempt_serial);
}

// ---- the C++ present host ----------------------------------------------------------

// A bare native host that notes every callback and its presentation into g_log. It buys
// one unit at the close of script bar `enter_bar` and flattens at the close of
// `exit_bar`; throw_label fails the run at that script bar, abort_label requests a
// cooperative abort there.
struct PresentHost final : NativeStrategyHost {
    int enter_bar = -1;
    int exit_bar = -1;
    std::int64_t throw_label = -1;
    std::int64_t abort_label = -1;
    int bars_seen = 0;

    void on_native_run_begin() override { g_log.note("host.run_begin"); }
    void on_native_input(const Bar&, const NativeInputContext&) override {
        g_log.note("host.input");
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        g_log.note("host.bar_open");
    }
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        g_log.note("host.bar");
        const int index = bars_seen++;
        if (index == enter_bar) (void)submit({no::Transact{1.0}, "entry", ""});
        if (index == exit_bar) (void)submit({no::Flatten{}, "exit", ""});
        if (context.script_bar_open_ms == throw_label) {
            throw std::runtime_error("selected routes fixture: callback refused");
        }
        if (context.script_bar_open_ms == abort_label) request_abort();
    }
    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        g_log.note("host.applied");
    }
    void present_report(ReportC*) const override { g_log.note("host.present_report"); }
};

enum class Route { Simple, Tf, Rich, Stream };

struct RouteTape {
    std::vector<Bar> rows;
    std::string input_tf;
    std::string script_tf;
    int warmup = 0;
    int enter_bar = 0;
    int exit_bar = 2;
    std::uint64_t input_bars = 0;
    std::uint64_t script_bars = 0;
};

// simple / rich / stream: the five-row tape, passthrough 1m -> 1m. tf: fifteen 1m rows
// in three 5m buckets priced 50 / 60 / 70 (a buy at the close of bucket 0 fills at
// bucket 1's open, the flatten at its close fills at bucket 2's open: one closed row).
RouteTape tape_for(Route route) {
    RouteTape tape;
    if (route == Route::Tf) {
        tape.input_tf = "1";
        tape.script_tf = "5";
        for (int minute = 0; minute < 15; ++minute) {
            const double price = 50.0 + 10.0 * (minute / 5);
            tape.rows.push_back(Bar{price, price, price, price, 1.0, kT0 + minute * kMinute});
        }
        tape.enter_bar = 0;
        tape.exit_bar = 1;
        tape.input_bars = 15;
        tape.script_bars = 3;
        return tape;
    }
    tape.input_tf = "1";
    tape.script_tf = "1";
    tape.rows.assign(kTape, kTape + 5);
    tape.enter_bar = 0;
    tape.exit_bar = 2;
    tape.input_bars = 5;
    tape.script_bars = 5;
    if (route == Route::Stream) tape.warmup = 2;
    return tape;
}

// Configures `host`, registers the observer (when `probe` is given), sets or clears the
// window (null = OFF) and makes the public entry of `route`. It reads nothing.
void drive(Route route, PresentHost& host, const RouteTape& tape,
           const pf_selected_window_config_v1* window, Probe* probe) {
    host.enter_bar = tape.enter_bar;
    host.exit_bar = tape.exit_bar;
    const NativeRunSpec spec = spec_of("wsrt-route", tape.input_tf.c_str(), tape.script_tf.c_str());
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    pf_execution_observer_v1 observer{};
    if (probe != nullptr) {
        probe->handle = handle_of(host);
        observer = observer_of(probe);
        CHECK(strategy_set_execution_observer_v1(probe->handle, &observer) == 0);
    }
    CHECK(strategy_set_selected_window_v1(handle_of(host), window) == 0);
    const int n = static_cast<int>(tape.rows.size());
    switch (route) {
    case Route::Simple:
        host.run(tape.rows.data(), n);
        break;
    case Route::Tf:
        host.run(tape.rows.data(), n, spec.input_tf, spec.script_tf, false);
        break;
    case Route::Rich: {
        const std::unordered_map<std::string, std::string> inputs;
        const SymInfo syminfo{};
        host.run(tape.rows.data(), n, spec.input_tf, spec.script_tf, inputs, syminfo);
        break;
    }
    case Route::Stream: {
        CHECK(host.stream_begin(tape.rows.data(), tape.warmup, spec.input_tf, spec.script_tf));
        for (std::size_t i = static_cast<std::size_t>(tape.warmup); i < tape.rows.size(); ++i) {
            CHECK(host.stream_push_bar(tape.rows[i]));
        }
        CHECK(host.stream_end(false));
        break;
    }
    }
}

// ---- OFF versus selected --------------------------------------------------------------

// The presented trades and scalars equal, and the selected curve is the anchor (at T, the
// initial capital, no open profit) plus the OFF curve's observations, point for point.
// Works for ReportC and for pf_report_t (the arrays and counts they share).
template <typename R>
void compare_off_selected(const R& off, const R& sel, std::int64_t anchor_time,
                          double anchor_equity) {
    // A positive count promises an array on each side. A missing array is a FAIL of its own
    // and the element comparison stops short of it; it is never skipped as a pass.
    CHECK(off.trades_len >= 0);
    CHECK(sel.trades_len == off.trades_len);
    if (off.trades_len > 0) {
        CHECK(off.trades != nullptr);
        CHECK(sel.trades != nullptr);
    }
    if (off.trades_len > 0 && sel.trades_len == off.trades_len && sel.trades != nullptr
        && off.trades != nullptr) {
        for (int i = 0; i < off.trades_len; ++i) {
            CHECK(sel.trades[i].entry_time == off.trades[i].entry_time);
            CHECK(sel.trades[i].exit_time == off.trades[i].exit_time);
            CHECK(sel.trades[i].entry_price == off.trades[i].entry_price);
            CHECK(sel.trades[i].exit_price == off.trades[i].exit_price);
            CHECK(sel.trades[i].qty == off.trades[i].qty);
            CHECK(sel.trades[i].pnl == off.trades[i].pnl);
            CHECK(sel.trades[i].commission == off.trades[i].commission);
            CHECK(sel.trades[i].is_long == off.trades[i].is_long);
            CHECK(sel.trades[i].max_runup == off.trades[i].max_runup);
            CHECK(sel.trades[i].max_drawdown == off.trades[i].max_drawdown);
            CHECK(sel.trades[i].open_at_end == off.trades[i].open_at_end);
        }
    }
    CHECK(sel.script_bars_processed == off.script_bars_processed);
    // Both curves are expected non-empty (OFF holds M >= 1 observations, the selected curve
    // the anchor plus those M), so both arrays must be present; a missing one is a FAIL and
    // the point comparison stops short of it.
    CHECK(off.equity_curve_len >= 1);
    CHECK(sel.equity_curve_len == off.equity_curve_len + 1);
    CHECK(off.equity_curve != nullptr);
    CHECK(sel.equity_curve != nullptr);
    if (off.equity_curve_len >= 1 && sel.equity_curve_len == off.equity_curve_len + 1
        && sel.equity_curve != nullptr && off.equity_curve != nullptr) {
        CHECK(sel.equity_curve[0].time_ms == anchor_time);
        CHECK(near(sel.equity_curve[0].equity, anchor_equity, 1e-9));
        CHECK(near(sel.equity_curve[0].open_profit, 0.0, 1e-12));
        for (std::int64_t i = 0; i < off.equity_curve_len; ++i) {
            CHECK(sel.equity_curve[i + 1].time_ms == off.equity_curve[i].time_ms);
            CHECK(near(sel.equity_curve[i + 1].equity, off.equity_curve[i].equity, 1e-9));
            CHECK(near(sel.equity_curve[i + 1].open_profit, off.equity_curve[i].open_profit,
                       1e-9));
        }
    }
}

// The scalars and metrics the documented report semantics leave equal when the window starts at
// the first row and the first observation is flat: M observations on both sides, the
// anchor equal to the first observation, buy-and-hold over the same first open and last
// close. The return-statistics block is NOT compared (the selected run has M returns, OFF
// has M - 1).
void compare_metrics(const ReportC& off, const ReportC& sel) {
    CHECK(sel.input_bars_processed == off.input_bars_processed);
    CHECK(sel.net_profit == off.net_profit);
    CHECK(near(sel.metrics.all.net_profit, off.metrics.all.net_profit, 1e-9));
    const pf_equity_stats_t& a = off.metrics.equity;
    const pf_equity_stats_t& b = sel.metrics.equity;
    CHECK(near(b.time_in_market_pct, a.time_in_market_pct, 1e-9));
    CHECK(near(b.open_pl, a.open_pl, 1e-9));
    CHECK(near(b.max_equity_drawdown, a.max_equity_drawdown, 1e-9));
    CHECK(near(b.buy_hold_return_pct, a.buy_hold_return_pct, 1e-9));
    CHECK(near(b.buy_hold_return, a.buy_hold_return, 1e-9));
}

// One C++ route: OFF and selected side by side.
void check_cpp_route(Route route) {
    const RouteTape tape = tape_for(route);
    const std::int64_t first_row = tape.rows.front().timestamp;

    g_log.clear();
    PresentHost off;
    drive(route, off, tape, nullptr, nullptr);
    CHECK(off.native_state().kind == NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 off_counts = sentinel_counts();
    const pf_selected_window_counts_v1 off_counts_before = off_counts;
    CHECK(strategy_selected_window_counts_v1(handle_of(off), &off_counts) == -2);
    CHECK(same_counts(off_counts, off_counts_before));
    Report off_report;
    off.fill_report(&off_report.raw);
    const std::vector<std::string> off_log = g_log.events;

    g_log.clear();
    PresentHost sel;
    Probe probe;
    const pf_selected_window_config_v1 window = window_from(first_row);
    drive(route, sel, tape, &window, &probe);
    CHECK(sel.native_state().kind == NativeLifecycleKind::Completed);
    check_observer_inside(probe);
    const pf_execution_observation_v1 opened = observation_of(handle_of(sel));
    CHECK(opened.phase == kPhaseResults);
    CHECK(opened.fault_stage == kFaultNone);
    CHECK(opened.attempt_outcome == kOutcomeResultsOpen);
    CHECK(opened.boundary_delivered == 1);
    CHECK(opened.run_generation == probe.boundary_generation);
    CHECK(opened.attempt_serial == probe.boundary_serial);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(strategy_selected_window_counts_v1(handle_of(sel), &counts) == 0);
    CHECK(counts.run_generation == opened.run_generation);
    CHECK(counts.attempt_serial == opened.attempt_serial);
    CHECK(counts.attempt_generation == opened.attempt_generation);
    CHECK(counts.fed_input_bars == tape.input_bars);
    CHECK(counts.preroll_input_bars == 0);
    CHECK(counts.window_input_bars == tape.input_bars);
    CHECK(counts.fed_script_bars == tape.script_bars);
    CHECK(counts.preroll_script_bars == 0);
    CHECK(counts.window_script_bars == tape.script_bars);
    Report sel_report;
    sel.fill_report(&sel_report.raw);
    const std::vector<std::string> sel_log = g_log.events;

    // The observer fired once, after the last permitted host work (the capture's
    // presentation) and before the read; no host dispatch follows it, the single read
    // included.
    CHECK(count_of(sel_log, "observer") == 1);
    CHECK(observer_follows_last_host_work(sel_log, true));
    CHECK(host_events_after_observer(sel_log) == 0);
    // One presentation per single-read route, selected and OFF alike.
    CHECK(count_of(sel_log, "host.present_report") == 1);
    CHECK(count_of(off_log, "host.present_report") == 1);
    // The execution itself is the same stream of host callbacks.
    CHECK(count_of(sel_log, "host.run_begin") == count_of(off_log, "host.run_begin"));
    CHECK(count_of(sel_log, "host.input") == count_of(off_log, "host.input"));
    CHECK(count_of(sel_log, "host.bar_open") == count_of(off_log, "host.bar_open"));
    CHECK(count_of(sel_log, "host.bar") == count_of(off_log, "host.bar"));
    CHECK(count_of(sel_log, "host.applied") == count_of(off_log, "host.applied"));
    CHECK(count_of(sel_log, "host.bar") == static_cast<int>(tape.script_bars));
    if (route != Route::Stream) CHECK(off_report.raw.trades_len == 1);

    compare_off_selected(off_report.raw, sel_report.raw, first_row, kCapital);
    compare_metrics(off_report.raw, sel_report.raw);
}

// ---- the native C host ------------------------------------------------------------------

constexpr double kHookFavorable = 7.5;
constexpr double kHookAdverse = 2.5;

struct CState {
    pf_strategy_t handle = nullptr;
    int calculations = 0;
    int excursions = 0;
};

int c_on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    CState* state = static_cast<CState*>(user);
    g_log.note("host.c_bar");
    const int index = state->calculations++;
    pf_native_request_v1 request;
    std::memset(&request, 0, sizeof request);
    request.struct_size = static_cast<std::uint32_t>(sizeof request);
    request.version = PF_NATIVE_API_VERSION;
    if (index == 0) {
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.trigger = PF_NATIVE_TRIGGER_MARKET;
        request.label = "c-entry";
        (void)strategy_native_submit_v1(state->handle, &request, nullptr, nullptr);
    } else if (index == 2) {
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "c-exit";
        (void)strategy_native_submit_v1(state->handle, &request, nullptr, nullptr);
    }
    return 0;
}

int c_on_applied(void*, const pf_native_applied_v1*, const pf_native_decision_v1*) {
    g_log.note("host.c_applied");
    return 0;
}

int c_on_lot_excursion(void* user, const pf_native_lot_excursion_v1*, double* favorable,
                       double* adverse) {
    CState* state = static_cast<CState*>(user);
    ++state->excursions;
    g_log.note("host.c_excursion");
    *favorable = kHookFavorable;
    *adverse = kHookAdverse;
    return PF_NATIVE_ANSWER_PROVIDED;
}

// Creates and configures (KernelRecorded through the spec extension) a native C host.
bool c_host_ready(CState& state) {
    pf_native_callbacks_v1 table;
    std::memset(&table, 0, sizeof table);
    table.struct_size = static_cast<std::uint32_t>(sizeof table);
    table.version = PF_NATIVE_API_VERSION;
    table.user = &state;
    table.on_bar = &c_on_bar;
    table.on_applied = &c_on_applied;
    table.on_lot_excursion = &c_on_lot_excursion;
    state.handle = strategy_native_host_create_v1(&table);
    CHECK(state.handle != nullptr);
    if (state.handle == nullptr) return false;

    pf_native_run_spec_v1 spec;
    std::memset(&spec, 0, sizeof spec);
    spec.struct_size = static_cast<std::uint32_t>(sizeof spec);
    spec.session_key = "wsrt-c-host";
    spec.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = kCapital;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = PF_NATIVE_FEE_CASH_PER_EXECUTION;
    spec.fee_value = 0.0;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    pf_native_run_spec_ext_v1 ext;
    std::memset(&ext, 0, sizeof ext);
    ext.struct_size = static_cast<std::uint32_t>(sizeof ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    const int rc = strategy_configure_native_ext_v1(state.handle, &spec, &ext);
    CHECK(rc == PF_NATIVE_OK);
    return rc == PF_NATIVE_OK;
}

// The native C host with an on_lot_excursion hook: real entry strategy_native_run_v1,
// OFF and selected side by side.
void check_c_host_route() {
    const std::vector<Bar> rows(kTape, kTape + 5);
    const pf_bar_t* bars = reinterpret_cast<const pf_bar_t*>(rows.data());
    const int n = static_cast<int>(rows.size());

    g_log.clear();
    CState off_state;
    if (!c_host_ready(off_state)) return;
    CReport off_report;
    CHECK(strategy_native_run_v1(off_state.handle, bars, n, &off_report.raw) == PF_NATIVE_OK);
    pf_selected_window_counts_v1 off_counts = sentinel_counts();
    const pf_selected_window_counts_v1 off_counts_before = off_counts;
    CHECK(strategy_selected_window_counts_v1(off_state.handle, &off_counts) == -2);
    CHECK(same_counts(off_counts, off_counts_before));
    const std::vector<std::string> off_log = g_log.events;

    g_log.clear();
    CState sel_state;
    if (!c_host_ready(sel_state)) {
        strategy_native_host_free(off_state.handle);
        return;
    }
    Probe probe;
    probe.handle = sel_state.handle;
    const pf_execution_observer_v1 observer = observer_of(&probe);
    CHECK(strategy_set_execution_observer_v1(sel_state.handle, &observer) == 0);
    const pf_selected_window_config_v1 window = window_from(rows.front().timestamp);
    CHECK(strategy_set_selected_window_v1(sel_state.handle, &window) == 0);
    CReport sel_report;
    CHECK(strategy_native_run_v1(sel_state.handle, bars, n, &sel_report.raw) == PF_NATIVE_OK);
    check_observer_inside(probe);
    const pf_execution_observation_v1 opened = observation_of(sel_state.handle);
    CHECK(opened.phase == kPhaseResults);
    CHECK(opened.attempt_outcome == kOutcomeResultsOpen);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(strategy_selected_window_counts_v1(sel_state.handle, &counts) == 0);
    CHECK(counts.fed_script_bars == 5);
    CHECK(counts.window_script_bars == 5);
    CHECK(counts.preroll_script_bars == 0);
    const std::vector<std::string> sel_log = g_log.events;

    CHECK(count_of(sel_log, "observer") == 1);
    CHECK(observer_follows_last_host_work(sel_log, false));
    CHECK(host_events_after_observer(sel_log) == 0);
    // The hook is consulted for the closing evaluation(s) and its answers reach the row; the
    // selected run consults it exactly as often as OFF does, all of it before the observer.
    CHECK(off_state.excursions >= 1);
    CHECK(sel_state.excursions == off_state.excursions);
    CHECK(count_of(sel_log, "host.c_bar") == 5);
    CHECK(count_of(sel_log, "host.c_bar") == count_of(off_log, "host.c_bar"));
    CHECK(count_of(sel_log, "host.c_applied") == count_of(off_log, "host.c_applied"));
    CHECK(off_report.raw.trades_len == 1);
    CHECK(sel_report.raw.trades_len == 1);
    CHECK(off_report.raw.trades != nullptr);
    CHECK(sel_report.raw.trades != nullptr);
    if (off_report.raw.trades_len == 1 && sel_report.raw.trades_len == 1
        && off_report.raw.trades != nullptr && sel_report.raw.trades != nullptr) {
        CHECK(off_report.raw.trades[0].max_runup == kHookFavorable);
        CHECK(off_report.raw.trades[0].max_drawdown == kHookAdverse);
        CHECK(sel_report.raw.trades[0].max_runup == kHookFavorable);
        CHECK(sel_report.raw.trades[0].max_drawdown == kHookAdverse);
    }
    compare_off_selected(off_report.raw, sel_report.raw, rows.front().timestamp, kCapital);

    strategy_native_host_free(sel_state.handle);
    strategy_native_host_free(off_state.handle);
}

// ---- the Pine source host with the terminal quote ---------------------------------------

// The fixture host of test_report_terminal_quote (two entries, a partial close), without
// the broker-state hash recording: the sealed selected run refuses that state query.
class QuoteHost final : public source::PineStrategyHost {
public:
    explicit QuoteHost(bool long_side) : long_side_(long_side) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.1;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        g_log.note("host.source_bar");
        if (bar_index_ == 0 || bar_index_ == 1) strategy_entry("open", long_side_);
        if (bar_index_ == 2) strategy_close("open", {}, 1.0);
    }

private:
    bool long_side_;
};

constexpr double kPineCapital = 10000.0;

std::vector<Bar> pine_feed() {
    std::vector<Bar> bars;
    for (int index = 0; index <= 60; ++index) {
        const double price = 100.0 + index * 0.125;
        bars.push_back({price, price + 0.25, price - 0.25, price + 0.125, 1.0,
                        kPineT0 + index * 60000LL});
    }
    return bars;
}

// Batch, 1m -> 15m with the magnifier: the shape in which the terminal quote is presented.
void pine_run(source::PineStrategyHost& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
}

// A selected Pine run (window at the first bar, observer registered), quote preset.
void pine_selected_run(QuoteHost& host, const std::vector<Bar>& bars, std::int64_t quote_time,
                       Probe& probe, const pf_selected_window_config_v1& window) {
    CHECK(host.set_report_terminal_quote(quote_time, 112.0));
    probe.handle = handle_of(host);
    const pf_execution_observer_v1 observer = observer_of(&probe);
    CHECK(strategy_set_execution_observer_v1(probe.handle, &observer) == 0);
    CHECK(strategy_set_selected_window_v1(probe.handle, &window) == 0);
    pine_run(host, bars);
}

// Rows flagged open_at_end. A positive length over a missing array is a FAIL of its own
// (and counts zero rows, so the caller's expected count fails too), never a skipped loop.
int open_at_end_rows(const ReportC& report) {
    CHECK(report.trades_len >= 0);
    if (report.trades_len > 0) CHECK(report.trades != nullptr);
    int rows = 0;
    if (report.trades_len > 0 && report.trades != nullptr) {
        for (int index = 0; index < report.trades_len; ++index) {
            rows += report.trades[index].open_at_end != 0 ? 1 : 0;
        }
    }
    return rows;
}

// report_terminal_quote_applied reads the last curve point and every row, so a report whose
// positive length has no array behind it is a FAIL here and the product helper is never
// called over it.
bool quote_applied(const QuoteHost& host, const ReportC& report) {
    CHECK(report.equity_curve_len >= 1);
    CHECK(report.equity_curve != nullptr);
    CHECK(report.trades_len >= 0);
    if (report.trades_len > 0) CHECK(report.trades != nullptr);
    if (report.equity_curve_len < 1 || report.equity_curve == nullptr) return false;
    if (report.trades_len > 0 && report.trades == nullptr) return false;
    return host.report_terminal_quote_applied(report);
}

// The first 15-minute open after pine_feed()'s last row (01:00): an E the planner
// admits for that feed (aligned, every row before it).
constexpr std::int64_t kPineAlignedEnd = kPineT0 + 75 * kMinute;

// The Pine host (tf shape): KernelRecordedAtHostMarks, open at the end, terminal
// quote preset before begin; OFF and selected side by side. The window ends either
// 1000 minutes after T (not on a 15-minute open) or at kPineAlignedEnd.
void check_pine_route(bool long_side, bool aligned_end) {
    const std::vector<Bar> bars = pine_feed();
    const std::int64_t quote_time = bars.back().timestamp;

    g_log.clear();
    QuoteHost off(long_side);
    CHECK(off.set_report_terminal_quote(quote_time, 112.0));
    pine_run(off, bars);
    CHECK(off.last_error().empty());
    Report off_report;
    off.fill_report(&off_report.raw);
    const std::vector<std::string> off_log = g_log.events;

    g_log.clear();
    QuoteHost sel(long_side);
    Probe probe;
    pf_selected_window_config_v1 window = window_from(bars.front().timestamp);
    if (aligned_end) window.end_ms = kPineAlignedEnd;
    pine_selected_run(sel, bars, quote_time, probe, window);
    CHECK(sel.last_error().empty());
    check_observer_inside(probe);
    const pf_execution_observation_v1 opened = observation_of(handle_of(sel));
    CHECK(opened.phase == kPhaseResults);
    CHECK(opened.attempt_outcome == kOutcomeResultsOpen);
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(strategy_selected_window_counts_v1(handle_of(sel), &counts) == 0);
    CHECK(counts.preroll_script_bars == 0);
    CHECK(counts.window_script_bars == counts.fed_script_bars);
    // From the rows: 61 one-minute rows from a 15-minute open fill the groups at 00:00,
    // 00:15, 00:30 and 00:45; the 01:00 group holds one row and is never sealed.
    CHECK(counts.fed_script_bars == 4);
    CHECK(counts.fed_input_bars == 61);
    CHECK(counts.window_input_bars == 61);
    Report sel_report;
    sel.fill_report(&sel_report.raw);
    const std::vector<std::string> sel_log = g_log.events;

    CHECK(count_of(sel_log, "observer") == 1);
    CHECK(observer_follows_last_host_work(sel_log, false));
    CHECK(host_events_after_observer(sel_log) == 0);
    CHECK(count_of(sel_log, "host.source_bar") == count_of(off_log, "host.source_bar"));
    CHECK(count_of(sel_log, "host.source_bar") > 0);

    // The terminal quote is presented identically: both reports re-mark the open-at-end
    // rows and the last curve point from the quote preset before begin.
    CHECK(quote_applied(off, off_report.raw));
    CHECK(quote_applied(sel, sel_report.raw));
    CHECK(open_at_end_rows(off_report.raw) == 2);
    CHECK(open_at_end_rows(sel_report.raw) == 2);
    compare_off_selected(off_report.raw, sel_report.raw, bars.front().timestamp, kPineCapital);
    compare_metrics(off_report.raw, sel_report.raw);
}

// ---- the Pine source host with a retained pre-roll --------------------------------------

// A Pine host as the harness runs a transpiled strategy: no report-policy override (the
// adapter projects KernelRecordedAtHostMarks), pyramiding 0 and process_orders_on_close,
// so the last pre-roll evaluation's two explicit opening market entries reach the
// submit-only terminal explicit-market pass of PineStrategyHost::selected_replay_at_open.
// On pine_feed()'s 15-minute chart the script bars are bar_index 0 (00:00) and 1 (00:15)
// before T = 00:30, then 2 (00:30) and 3 (00:45).
class PrerollHost final : public source::PineStrategyHost {
public:
    explicit PrerollHost(bool enter_in_preroll) : enter_in_preroll_(enter_in_preroll) {
        source::PineStrategyConfig config;
        config.initial_capital = kPineCapital;
        config.default_qty_value = 1.0;
        config.pyramiding = 0;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        g_log.note("host.source_bar");
        // The last pre-roll bar: a same-open pair of explicit entries, long then short.
        if (bar_index_ == 1 && enter_in_preroll_) {
            strategy_entry("L", true);
            strategy_entry("S", false);
        }
        // The last window bar closes whatever is open.
        if (bar_index_ == 3) strategy_close_all();
    }

private:
    bool enter_in_preroll_;
};

// The planner-admitted shape on the product route: T = 00:30 (two full 15-minute
// pre-roll groups), E = kPineAlignedEnd (aligned, every row before it), the same
// 1 -> 15 magnifier run as pine_run. The same host without pre-roll orders is the
// control: what the window shows at T comes only from the replay.
void row_pine_tf_preroll() {
    const std::vector<Bar> bars = pine_feed();
    const std::int64_t t_ms = kPineT0 + 30 * kMinute;
    pf_selected_window_config_v1 window = window_from(t_ms);
    window.end_ms = kPineAlignedEnd;

    g_log.clear();
    PrerollHost off(true);
    pine_run(off, bars);
    CHECK(off.last_error().empty());
    const int off_source_bars = count_of(g_log.events, "host.source_bar");
    CHECK(off_source_bars == 4);

    for (const bool enter : {true, false}) {
        g_log.clear();
        PrerollHost sel(enter);
        CHECK(strategy_set_selected_window_v1(handle_of(sel), &window) == 0);
        pine_run(sel, bars);
        CHECK(sel.last_error().empty());
        const pf_execution_observation_v1 opened = observation_of(handle_of(sel));
        CHECK(opened.phase == kPhaseResults);
        CHECK(opened.attempt_outcome == kOutcomeResultsOpen);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(strategy_selected_window_counts_v1(handle_of(sel), &counts) == 0);
        // From the rows: the 00:00 and 00:15 groups (30 rows) are pre-roll, the 00:30 and
        // 00:45 groups are window, and the 01:00 group (one row) is never sealed.
        CHECK(counts.preroll_script_bars == 2);
        CHECK(counts.window_script_bars == 2);
        CHECK(counts.fed_script_bars == counts.preroll_script_bars + counts.window_script_bars);
        CHECK(counts.preroll_input_bars == 30);
        CHECK(counts.window_input_bars == 31);
        CHECK(count_of(g_log.events, "host.source_bar") == off_source_bars);

        Report report;
        sel.fill_report(&report.raw);
        // The anchor at T with the initial capital, then one observation per window script
        // bar, each labelled in [T, E).
        CHECK(report.raw.equity_curve_len
              == static_cast<std::int64_t>(counts.window_script_bars) + 1);
        CHECK(report.raw.equity_curve != nullptr);
        if (report.raw.equity_curve != nullptr && report.raw.equity_curve_len >= 1) {
            CHECK(report.raw.equity_curve[0].time_ms == t_ms);
            CHECK(report.raw.equity_curve[0].equity == kPineCapital);
            std::uint64_t observed = 0;
            for (std::int64_t i = 1; i < report.raw.equity_curve_len; ++i) {
                const std::int64_t at = report.raw.equity_curve[i].time_ms;
                if (at >= t_ms && at < window.end_ms) ++observed;
            }
            CHECK(observed == counts.window_script_bars);
        }

        CHECK(report.raw.trades_len >= 0);
        if (report.raw.trades_len > 0) CHECK(report.raw.trades != nullptr);
        if (!enter) {
            // Nothing was ordered before T and the window's close_all has nothing to close.
            CHECK(report.raw.trades_len == 0);
            continue;
        }
        // The pair is placed once at the window's first Open and fills there (row 30's
        // open, 103.75). The submit-only pass re-submits it in source order with the later
        // opposite call as a full reversal, so the long is closed at that same fill and a
        // short is opened; the short is closed by the last window bar's close_all at that
        // bar's close under process_orders_on_close (row 59's close, 107.5, on the 0.01
        // tick). Every fill of a chart bar is dated at its open (T, and 00:45 for the
        // close). Two net deltas from the flat state would leave no short.
        CHECK(report.raw.trades_len == 2);
        int longs = 0;
        int shorts = 0;
        for (int index = 0; report.raw.trades != nullptr && index < report.raw.trades_len;
             ++index) {
            const TradeC& row = report.raw.trades[index];
            CHECK(row.entry_time == t_ms);
            CHECK(row.entry_price == bars[30].open);
            CHECK(row.open_at_end == 0);
            if (row.is_long) {
                ++longs;
                CHECK(row.exit_time == t_ms);
                CHECK(row.exit_price == bars[30].open);
            } else {
                ++shorts;
                CHECK(row.exit_time == t_ms + 15 * kMinute);
                CHECK(row.exit_price == bars[59].close);
            }
        }
        CHECK(longs == 1);
        CHECK(shorts == 1);
    }
}

// The real planner and a real Pine run over the rows the plan keeps: the equalities the
// harness enforces (the native counts are the plan's, the report's processed counts are
// the plan's fed counts, the curve is the anchor plus the plan's window script bars).
void check_plan_equals_run(const char* script_tf, std::int64_t end_ms, std::uint32_t preroll) {
    const std::vector<Bar> rows = pine_feed();
    const std::int64_t t_ms = kPineT0 + 30 * kMinute;
    pineforge::SelectedPlanRequest request{};
    request.start_ms = t_ms;
    request.end_ms = end_ms;
    request.fed_start_ms = rows.front().timestamp;
    request.preroll_bars = preroll;
    request.input_tf = "1";
    request.script_tf = script_tf;
    request.chart_timezone = "UTC";
    request.engine_timezone = "UTC";
    request.session = "";
    const pineforge::SelectedPrimaryPlan plan =
        pineforge::plan_selected_primary(rows.data(), rows.size(), request);
    CHECK(plan.status == pineforge::SelectedPlanStatus::Ok);
    if (plan.status != pineforge::SelectedPlanStatus::Ok || plan.trim_index > rows.size()) return;
    const std::vector<Bar> fed(rows.begin() + static_cast<std::ptrdiff_t>(plan.trim_index),
                               rows.end());

    PrerollHost host(false);
    pf_selected_window_config_v1 window = window_from(t_ms);
    window.end_ms = end_ms;
    CHECK(strategy_set_selected_window_v1(handle_of(host), &window) == 0);
    host.run(fed.data(), static_cast<int>(fed.size()), "1", script_tf, true);
    CHECK(host.last_error().empty());
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(strategy_selected_window_counts_v1(handle_of(host), &counts) == 0);
    CHECK(counts.fed_input_bars == plan.fed_input_bars);
    CHECK(counts.preroll_input_bars == plan.preroll_input_bars);
    CHECK(counts.window_input_bars == plan.window_input_bars);
    CHECK(counts.fed_script_bars == plan.fed_script_bars);
    CHECK(counts.preroll_script_bars == plan.used_script_bars);
    CHECK(counts.window_script_bars == plan.window_script_bars);
    Report report;
    host.fill_report(&report.raw);
    CHECK(static_cast<std::uint64_t>(report.raw.input_bars_processed) == plan.fed_input_bars);
    CHECK(static_cast<std::uint64_t>(report.raw.script_bars_processed) == plan.fed_script_bars);
    CHECK(static_cast<std::uint64_t>(report.raw.equity_curve_len) == plan.window_script_bars + 1);
}

// 1 -> 15: the two pre-T groups give N = 0, 1, 2 and a shortfall at 3; 1 -> 1: the 30
// pre-T bars give N = 0, 10, 30 and a shortfall at 40. E is the first open after the last
// row on each chart.
void row_plan_equals_run() {
    for (const std::uint32_t preroll : {0u, 1u, 2u, 3u})
        check_plan_equals_run("15", kPineAlignedEnd, preroll);
    for (const std::uint32_t preroll : {0u, 10u, 30u, 40u})
        check_plan_equals_run("1", kPineT0 + 61 * kMinute, preroll);
}

// ---- rows -------------------------------------------------------------------------------

// The two log predicates the negative controls rely on reject a deliberately broken log.
void row_checker_teeth() {
    const std::vector<std::string> good = {"host.run_begin", "host.bar", "host.present_report",
                                           "observer"};
    CHECK(host_events_after_observer(good) == 0);
    CHECK(observer_follows_last_host_work(good, true));
    CHECK(observer_follows_last_host_work(good, false));

    // an extra post-seal host dispatch (a presentation after the observer)
    const std::vector<std::string> extra = {"host.bar", "host.present_report", "observer",
                                            "host.present_report"};
    CHECK(host_events_after_observer(extra) == 1);
    CHECK(!observer_follows_last_host_work(extra, true));
    CHECK(!observer_follows_last_host_work(extra, false));

    // the observer before the last permitted host work
    const std::vector<std::string> early = {"host.bar", "observer", "host.present_report"};
    CHECK(host_events_after_observer(early) == 1);
    CHECK(!observer_follows_last_host_work(early, true));

    // the observer after the presentation but with other work between them
    const std::vector<std::string> gap = {"host.present_report", "host.bar", "observer"};
    CHECK(observer_follows_last_host_work(gap, false));
    CHECK(!observer_follows_last_host_work(gap, true));

    // the observer missing, or fired twice
    const std::vector<std::string> none = {"host.bar", "host.present_report"};
    CHECK(host_events_after_observer(none) == -1);
    CHECK(!observer_follows_last_host_work(none, false));
    const std::vector<std::string> twice = {"host.bar", "observer", "observer"};
    CHECK(host_events_after_observer(twice) == -1);
    CHECK(!observer_follows_last_host_work(twice, false));
}

void row_batch_simple() {
    check_cpp_route(Route::Simple);
    check_c_host_route();
}

void row_batch_tf() {
    check_cpp_route(Route::Tf);
    check_pine_route(false, false);
    check_pine_route(true, false);
    check_pine_route(false, true);
    check_pine_route(true, true);
}

void row_batch_rich() { check_cpp_route(Route::Rich); }

void row_stream_end() { check_cpp_route(Route::Stream); }

// A selected run on the simple tape that fails in the host's callback at the third script
// bar: the generation closes Sealed with an execution fault, no capture, no presentation,
// no successful observer call, the counts and the read stay closed, the first cause stays.
void row_run_failed() {
    const RouteTape tape = tape_for(Route::Simple);
    const std::int64_t bad_label = kT0 + 2 * kMinute;
    const pf_selected_window_config_v1 window = window_from(kT0);

    g_log.clear();
    PresentHost sel;
    sel.throw_label = bad_label;
    Probe probe;
    drive(Route::Simple, sel, tape, &window, &probe);
    CHECK(sel.native_state().kind == NativeLifecycleKind::Failed);
    const std::string first_cause = sel.last_error();
    CHECK(first_cause.find("selected routes fixture: callback refused") != std::string::npos);
    CHECK(probe.calls == 0);
    const pf_execution_observation_v1 seen = observation_of(handle_of(sel));
    CHECK(seen.phase == kPhaseSealed);
    CHECK(seen.fault_stage == kFaultExecution);
    CHECK(seen.attempt_outcome == kOutcomeFailed);
    CHECK(seen.boundary_delivered == 0);
    pf_selected_window_counts_v1 counts = sentinel_counts();
    const pf_selected_window_counts_v1 before = counts;
    CHECK(strategy_selected_window_counts_v1(handle_of(sel), &counts) == -2);
    CHECK(same_counts(counts, before));
    Report report;
    sel.fill_report(&report.raw);
    check_empty_report(report.raw);
    CHECK(sel.last_error() == first_cause);
    CHECK(g_log.count("observer") == 0);
    CHECK(g_log.count("host.present_report") == 0);
    CHECK(host_events_after_observer(g_log.events) == -1);

    // OFF counterpart: the same failure, the same first cause, and the failed attempt
    // keeps its partial rows (the existing rows_current semantics), unlike the closed
    // selected generation.
    g_log.clear();
    PresentHost off;
    off.throw_label = bad_label;
    drive(Route::Simple, off, tape, nullptr, nullptr);
    CHECK(off.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(off.last_error() == first_cause);
    pf_selected_window_counts_v1 off_counts = sentinel_counts();
    const pf_selected_window_counts_v1 off_before = off_counts;
    CHECK(strategy_selected_window_counts_v1(handle_of(off), &off_counts) == -2);
    CHECK(same_counts(off_counts, off_before));
    Report off_report;
    off.fill_report(&off_report.raw);
    CHECK(off_report.raw.equity_curve_len > 0);
    CHECK(off.last_error() == first_cause);
}

// A selected run aborted cooperatively from the host's callback at the third script bar.
void row_run_aborted() {
    const RouteTape tape = tape_for(Route::Simple);
    const std::int64_t abort_label = kT0 + 2 * kMinute;
    const pf_selected_window_config_v1 window = window_from(kT0);

    g_log.clear();
    PresentHost sel;
    sel.abort_label = abort_label;
    Probe probe;
    drive(Route::Simple, sel, tape, &window, &probe);
    CHECK(sel.native_state().kind != NativeLifecycleKind::Completed);
    const std::string first_cause = sel.last_error();
    CHECK(probe.calls == 0);
    const pf_execution_observation_v1 seen = observation_of(handle_of(sel));
    CHECK(seen.phase == kPhaseSealed);
    CHECK(seen.attempt_outcome == kOutcomeAborted);
    CHECK(seen.boundary_delivered == 0);
    pf_selected_window_counts_v1 counts = sentinel_counts();
    const pf_selected_window_counts_v1 before = counts;
    CHECK(strategy_selected_window_counts_v1(handle_of(sel), &counts) == -2);
    CHECK(same_counts(counts, before));
    Report report;
    sel.fill_report(&report.raw);
    check_empty_report(report.raw);
    CHECK(sel.last_error() == first_cause);
    CHECK(g_log.count("observer") == 0);
    CHECK(g_log.count("host.present_report") == 0);

    // OFF counterpart: aborted as well, no counts.
    g_log.clear();
    PresentHost off;
    off.abort_label = abort_label;
    drive(Route::Simple, off, tape, nullptr, nullptr);
    CHECK(off.native_state().kind != NativeLifecycleKind::Completed);
    pf_selected_window_counts_v1 off_counts = sentinel_counts();
    const pf_selected_window_counts_v1 off_before = off_counts;
    CHECK(strategy_selected_window_counts_v1(handle_of(off), &off_counts) == -2);
    CHECK(same_counts(off_counts, off_before));
}

// The observer refuses (non-zero) after the capture: the presentation ran once, before the
// observer; the generation stays Sealed with an after-execution fault and never reaches
// Results; counts and read stay closed; nothing is dispatched after the observer.
void row_observer_fault() {
    const RouteTape tape = tape_for(Route::Simple);
    const pf_selected_window_config_v1 window = window_from(kT0);

    g_log.clear();
    PresentHost sel;
    Probe probe;
    probe.rc = 7;
    drive(Route::Simple, sel, tape, &window, &probe);
    CHECK(sel.native_state().kind == NativeLifecycleKind::Failed);
    const std::string first_cause = sel.last_error();
    CHECK(!first_cause.empty());
    CHECK(probe.calls == 1);
    CHECK(probe.observation_rc == 0);
    CHECK(probe.inside.phase == kPhaseSealed);
    CHECK(probe.counts_inside == -2);
    CHECK(g_log.count("host.present_report") == 1);
    CHECK(observer_follows_last_host_work(g_log.events, true));
    const pf_execution_observation_v1 seen = observation_of(handle_of(sel));
    CHECK(seen.phase == kPhaseSealed);
    CHECK(seen.fault_stage == kFaultAfterExecution);
    CHECK(seen.attempt_outcome == kOutcomeFailed);
    CHECK(seen.phase != kPhaseResults);
    CHECK(seen.attempt_outcome != kOutcomeResultsOpen);
    pf_selected_window_counts_v1 counts = sentinel_counts();
    const pf_selected_window_counts_v1 before = counts;
    CHECK(strategy_selected_window_counts_v1(handle_of(sel), &counts) == -2);
    CHECK(same_counts(counts, before));
    Report report;
    sel.fill_report(&report.raw);
    check_empty_report(report.raw);
    CHECK(sel.last_error() == first_cause);
    CHECK(g_log.count("host.present_report") == 1);
    CHECK(host_events_after_observer(g_log.events) == 0);
}

// A public entry refused before admission mints no generation: the earlier closure stays,
// the earlier Results snapshot is no permission for the refused attempt, the read is
// empty and nothing is dispatched. The OFF counterpart reads empty too (rows_current).
void row_preadmission_refused() {
    const RouteTape tape = tape_for(Route::Simple);
    const int n = static_cast<int>(tape.rows.size());
    const pf_selected_window_config_v1 window = window_from(kT0);

    g_log.clear();
    PresentHost sel;
    Probe probe;
    drive(Route::Simple, sel, tape, &window, &probe);
    CHECK(sel.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(probe.calls == 1);
    const pf_execution_observation_v1 opened = observation_of(handle_of(sel));
    CHECK(opened.phase == kPhaseResults);
    CHECK(opened.attempt_outcome == kOutcomeResultsOpen);
    const int presented = g_log.count("host.present_report");
    const int bars = g_log.count("host.bar");
    const int begins = g_log.count("host.run_begin");
    CHECK(presented == 1);

    // Run again without configure_native: a Completed host refuses before admission.
    sel.run(tape.rows.data(), n);
    CHECK(sel.native_state().kind == NativeLifecycleKind::Completed);
    const pf_execution_observation_v1 refused = observation_of(handle_of(sel));
    CHECK(refused.run_generation == opened.run_generation);
    CHECK(refused.attempt_generation == 0);
    CHECK(refused.attempt_serial > opened.attempt_serial);
    CHECK(refused.attempt_outcome == kOutcomeRefused);
    pf_selected_window_counts_v1 counts = sentinel_counts();
    const pf_selected_window_counts_v1 before = counts;
    CHECK(strategy_selected_window_counts_v1(handle_of(sel), &counts) == -2);
    CHECK(same_counts(counts, before));
    CHECK(strategy_state_query_status_v1(handle_of(sel))
          == PF_STATE_QUERY_SELECTED_WINDOW_AFTER_SEAL_V1);
    Report report;
    sel.fill_report(&report.raw);
    check_empty_report(report.raw);
    CHECK(probe.calls == 1);
    CHECK(g_log.count("host.present_report") == presented);
    CHECK(g_log.count("host.bar") == bars);
    CHECK(g_log.count("host.run_begin") == begins);
    CHECK(host_events_after_observer(g_log.events) == 0);

    // OFF counterpart.
    g_log.clear();
    PresentHost off;
    drive(Route::Simple, off, tape, nullptr, nullptr);
    CHECK(off.native_state().kind == NativeLifecycleKind::Completed);
    Report first;
    off.fill_report(&first.raw);
    CHECK(first.raw.trades_len == 1);
    const int off_presented = g_log.count("host.present_report");
    CHECK(off_presented == 1);
    off.run(tape.rows.data(), n);
    CHECK(off.native_state().kind == NativeLifecycleKind::Completed);
    Report second;
    off.fill_report(&second.raw);
    check_empty_report(second.raw);
    CHECK(g_log.count("host.present_report") == off_presented);
}

// Per COVERED single-read route (C++ simple / tf / rich / stream and strategy_native_run_v1;
// not the generated wrapper, native_module publish_report or the C-ABI stream family),
// selected and OFF present the report the same number of
// times (once), execute the same callbacks, and the selected run dispatches nothing after
// the observer. The C route counts the lot-excursion hook in place of the presentation (a C
// host cannot override present_report).
void row_single_read_callcounts() {
    const Route routes[] = {Route::Simple, Route::Tf, Route::Rich, Route::Stream};
    for (const Route route : routes) {
        const RouteTape tape = tape_for(route);

        g_log.clear();
        PresentHost off;
        drive(route, off, tape, nullptr, nullptr);
        Report off_report;
        off.fill_report(&off_report.raw);
        const std::vector<std::string> off_log = g_log.events;

        g_log.clear();
        PresentHost sel;
        Probe probe;
        const pf_selected_window_config_v1 window = window_from(tape.rows.front().timestamp);
        drive(route, sel, tape, &window, &probe);
        Report sel_report;
        sel.fill_report(&sel_report.raw);
        const std::vector<std::string> sel_log = g_log.events;

        CHECK(count_of(off_log, "host.present_report") == 1);
        CHECK(count_of(sel_log, "host.present_report") == count_of(off_log, "host.present_report"));
        CHECK(count_of(sel_log, "observer") == 1);
        CHECK(host_events_after_observer(sel_log) == 0);
        CHECK(sel_log.size() >= 2 && sel_log[sel_log.size() - 1] == "observer");
        CHECK(count_of(sel_log, "host.input") == count_of(off_log, "host.input"));
        CHECK(count_of(sel_log, "host.bar") == count_of(off_log, "host.bar"));
        CHECK(count_of(sel_log, "host.applied") == count_of(off_log, "host.applied"));
    }

    // The native C route.
    const std::vector<Bar> rows(kTape, kTape + 5);
    const pf_bar_t* bars = reinterpret_cast<const pf_bar_t*>(rows.data());
    const int n = static_cast<int>(rows.size());
    g_log.clear();
    CState off_state;
    if (!c_host_ready(off_state)) return;
    CReport off_report;
    CHECK(strategy_native_run_v1(off_state.handle, bars, n, &off_report.raw) == PF_NATIVE_OK);
    const std::vector<std::string> off_log = g_log.events;

    g_log.clear();
    CState sel_state;
    if (!c_host_ready(sel_state)) {
        strategy_native_host_free(off_state.handle);
        return;
    }
    Probe probe;
    probe.handle = sel_state.handle;
    const pf_execution_observer_v1 observer = observer_of(&probe);
    CHECK(strategy_set_execution_observer_v1(sel_state.handle, &observer) == 0);
    const pf_selected_window_config_v1 window = window_from(rows.front().timestamp);
    CHECK(strategy_set_selected_window_v1(sel_state.handle, &window) == 0);
    CReport sel_report;
    CHECK(strategy_native_run_v1(sel_state.handle, bars, n, &sel_report.raw) == PF_NATIVE_OK);
    const std::vector<std::string> sel_log = g_log.events;
    CHECK(count_of(sel_log, "observer") == 1);
    CHECK(host_events_after_observer(sel_log) == 0);
    CHECK(count_of(sel_log, "host.c_excursion") == count_of(off_log, "host.c_excursion"));
    CHECK(count_of(sel_log, "host.c_bar") == count_of(off_log, "host.c_bar"));
    CHECK(count_of(sel_log, "host.c_applied") == count_of(off_log, "host.c_applied"));
    strategy_native_host_free(sel_state.handle);
    strategy_native_host_free(off_state.handle);
}

// Quote, preset: the terminal quote set BEFORE begin through the public bool setter is accepted
// and presented identically by the OFF and the selected run; the setter still rejects the
// inputs it rejects.
void row_quote_preset() {
    const std::vector<Bar> bars = pine_feed();
    const std::int64_t quote_time = bars.back().timestamp;
    for (const bool long_side : {false, true}) {
        g_log.clear();
        QuoteHost off(long_side);
        CHECK(off.set_report_terminal_quote(quote_time, 112.0));
        CHECK(!off.set_report_terminal_quote(quote_time, std::numeric_limits<double>::quiet_NaN()));
        CHECK(!off.set_report_terminal_quote(-1, 112.0));
        pine_run(off, bars);
        CHECK(off.last_error().empty());
        Report off_report;
        off.fill_report(&off_report.raw);

        g_log.clear();
        QuoteHost sel(long_side);
        CHECK(!sel.set_report_terminal_quote(quote_time, std::numeric_limits<double>::quiet_NaN()));
        CHECK(!sel.set_report_terminal_quote(-1, 112.0));
        Probe probe;
        const pf_selected_window_config_v1 window = window_from(bars.front().timestamp);
        pine_selected_run(sel, bars, quote_time, probe, window);
        CHECK(sel.last_error().empty());
        CHECK(probe.calls == 1);
        Report sel_report;
        sel.fill_report(&sel_report.raw);

        CHECK(quote_applied(off, off_report.raw));
        CHECK(quote_applied(sel, sel_report.raw));
        int marks = 0;
        CHECK(sel_report.raw.trades_len > 0);
        CHECK(sel_report.raw.trades != nullptr);
        if (sel_report.raw.trades_len > 0 && sel_report.raw.trades != nullptr) {
            for (int index = 0; index < sel_report.raw.trades_len; ++index) {
                const TradeC& row = sel_report.raw.trades[index];
                if (row.open_at_end == 0) continue;
                ++marks;
                CHECK(row.exit_time == quote_time);
                CHECK(row.exit_price == 112.0);
            }
        }
        CHECK(marks == 2);
        CHECK(sel_report.raw.equity_curve_len >= 1);
        CHECK(sel_report.raw.equity_curve != nullptr);
        if (sel_report.raw.equity_curve_len >= 1 && sel_report.raw.equity_curve != nullptr) {
            CHECK(sel_report.raw.equity_curve[sel_report.raw.equity_curve_len - 1].time_ms
                  == quote_time);
        }
        compare_off_selected(off_report.raw, sel_report.raw, bars.front().timestamp,
                             kPineCapital);
    }
}

// Quote, late: after the selected seal the public bool setter refuses with no effect (false;
// the next read is byte-for-byte the first one and still carries the preset quote); the
// OFF run, not sealed, still accepts a late quote and the next read presents it. The void
// routes (set_syminfo_metadata, clear_report_terminal_quote, the C metadata export) are
// deliberately not asserted: their post-seal behaviour is an open contract choice.
void row_quote_late_refused() {
    const std::vector<Bar> bars = pine_feed();
    const std::int64_t quote_time = bars.back().timestamp;
    for (const bool long_side : {false, true}) {
        g_log.clear();
        QuoteHost sel(long_side);
        Probe probe;
        const pf_selected_window_config_v1 window = window_from(bars.front().timestamp);
        pine_selected_run(sel, bars, quote_time, probe, window);
        CHECK(sel.last_error().empty());
        CHECK(probe.calls == 1);
        Report first;
        sel.fill_report(&first.raw);
        CHECK(quote_applied(sel, first.raw));
        CHECK(open_at_end_rows(first.raw) == 2);

        CHECK(!sel.set_report_terminal_quote(quote_time, 105.0));
        Report second;
        sel.fill_report(&second.raw);
        CHECK(quote_applied(sel, second.raw));
        // The first read has rows and a curve, so every array on both sides is required; a
        // missing one is a FAIL and the comparison stops short of it, never a skipped pass.
        CHECK(first.raw.trades_len > 0);
        CHECK(second.raw.trades_len == first.raw.trades_len);
        CHECK(first.raw.trades != nullptr);
        CHECK(second.raw.trades != nullptr);
        if (first.raw.trades_len > 0 && second.raw.trades_len == first.raw.trades_len
            && first.raw.trades != nullptr && second.raw.trades != nullptr) {
            for (int index = 0; index < first.raw.trades_len; ++index) {
                CHECK(second.raw.trades[index].exit_time == first.raw.trades[index].exit_time);
                CHECK(second.raw.trades[index].exit_price == first.raw.trades[index].exit_price);
                CHECK(second.raw.trades[index].pnl == first.raw.trades[index].pnl);
                CHECK(second.raw.trades[index].open_at_end == first.raw.trades[index].open_at_end);
            }
        }
        CHECK(first.raw.equity_curve_len >= 1);
        CHECK(second.raw.equity_curve_len == first.raw.equity_curve_len);
        CHECK(first.raw.equity_curve != nullptr);
        CHECK(second.raw.equity_curve != nullptr);
        if (first.raw.equity_curve_len >= 1
            && second.raw.equity_curve_len == first.raw.equity_curve_len
            && first.raw.equity_curve != nullptr && second.raw.equity_curve != nullptr) {
            const std::int64_t last = first.raw.equity_curve_len - 1;
            CHECK(second.raw.equity_curve[last].time_ms == first.raw.equity_curve[last].time_ms);
            CHECK(second.raw.equity_curve[last].equity == first.raw.equity_curve[last].equity);
        }
        CHECK(host_events_after_observer(g_log.events) == 0);

        // OFF counterpart: not sealed, so the late quote is accepted and the next read
        // presents it.
        QuoteHost off(long_side);
        CHECK(off.set_report_terminal_quote(quote_time, 112.0));
        pine_run(off, bars);
        CHECK(off.last_error().empty());
        Report off_first;
        off.fill_report(&off_first.raw);
        CHECK(quote_applied(off, off_first.raw));
        CHECK(off.set_report_terminal_quote(quote_time, 105.0));
        Report off_second;
        off.fill_report(&off_second.raw);
        CHECK(quote_applied(off, off_second.raw));
        int marks = 0;
        CHECK(off_second.raw.trades_len > 0);
        CHECK(off_second.raw.trades != nullptr);
        if (off_second.raw.trades_len > 0 && off_second.raw.trades != nullptr) {
            for (int index = 0; index < off_second.raw.trades_len; ++index) {
                const TradeC& row = off_second.raw.trades[index];
                if (row.open_at_end == 0) continue;
                ++marks;
                CHECK(row.exit_price == 105.0);
            }
        }
        CHECK(marks == 2);
    }
}

}  // namespace

int main() {
    // The three synthetic checker controls come first: a broken
    // dispatcher or log predicate stops the run before any real row. The sequence also
    // stops before the first row that is not a PASS (a failed check, an exception or no
    // check at all); the rows after it are listed NOT RUN and nothing reads COMPLETE.
    static const RowSpec rows[] = {
        {"ROUTES-VERDICT-CONTROL", &row_control_verdicts},
        {"ROUTES-SEQUENCE-CONTROL", &row_control_sequence},
        {"ROUTES-CHECKER-CONTROL", &row_checker_teeth},
        {"ROUTES-BATCH-SIMPLE", &row_batch_simple},
        {"ROUTES-BATCH-TF", &row_batch_tf},
        {"ROUTES-BATCH-RICH", &row_batch_rich},
        {"ROUTES-STREAM-END", &row_stream_end},
        {"ROUTES-RUN-FAILED", &row_run_failed},
        {"ROUTES-RUN-ABORTED", &row_run_aborted},
        {"ROUTES-OBSERVER-FAULT", &row_observer_fault},
        {"ROUTES-PREADMISSION-REFUSED", &row_preadmission_refused},
        {"ROUTES-SINGLE-READ-COUNTS", &row_single_read_callcounts},
        {"ROUTES-QUOTE-PRESET", &row_quote_preset},
        {"ROUTES-QUOTE-LATE-REFUSED", &row_quote_late_refused},
        {"ROUTES-PINE-TF-PREROLL", &row_pine_tf_preroll},
        {"ROUTES-PLAN-EQUALS-RUN", &row_plan_equals_run},
    };
    const std::size_t count = sizeof rows / sizeof rows[0];

    std::vector<RowOutcome> outcomes;
    (void)run_sequence(rows, count, &execute_row, &outcomes);
    const bool complete = sequence_complete(outcomes, count);

    std::printf("---- selected-window routes summary ----\n");
    std::size_t not_run = 0;
    const RowOutcome* first_bad = nullptr;
    for (const RowOutcome& row : outcomes) {
        if (row.verdict == RowVerdict::NotRun) {
            ++not_run;
            std::printf("%-28s %s\n", row.label.c_str(), verdict_word(row.verdict));
            continue;
        }
        if (row.verdict != RowVerdict::Pass && first_bad == nullptr) first_bad = &row;
        std::printf("%-28s %s  (%d checks, %d failures)\n", row.label.c_str(),
                    verdict_word(row.verdict), row.checks, row.failures);
    }
    if (complete) {
        std::printf("COMPLETE: all %zu rows ran and passed\n", count);
    } else if (first_bad != nullptr) {
        std::printf("INCOMPLETE: stopped at %s (%s); %zu of %zu rows not run\n",
                    first_bad->label.c_str(), verdict_word(first_bad->verdict), not_run, count);
    } else {
        std::printf("INCOMPLETE: %zu of %zu rows recorded\n", outcomes.size(), count);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return complete && g_failures == 0 ? 0 : 1;
}
