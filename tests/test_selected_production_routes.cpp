// Selected-window production-route controls (frozen observer ABI
// pineforge/execution_observer.h, frozen selected-window ABI pineforge/selected_window.h).
// Companion of tests/test_selected_window_routes.cpp, which is not reworked here.
//
// Two production entries that the route fixture did not reach, each driven from outside
// through its real public symbols, selected versus OFF, single read:
//
//   1. The native module entry. include/pineforge/native_module.hpp is the production macro:
//      PINEFORGE_EXPORT_NATIVE_STRATEGY(RouteHost) below expands to the loadable module's
//      real extern "C" exports (strategy_create, strategy_free, run_backtest,
//      run_backtest_full, report_free), and run_backtest / run_backtest_full publish the
//      report through native_module::publish_report (native_module.hpp:173-183), which is
//      what this TU reads. Nothing is re-created locally: the TU calls the exports the macro
//      defined, the only definitions of those names in this executable.
//   2. The C ABI stream family. The exported strategy_stream_begin, strategy_stream_push_bar,
//      strategy_stream_end and strategy_stream_fill_report (src/c_abi.cpp:693-814) are called
//      on a handle; no C++ stream_* member is used for the run.
//
// The host is a custom NativeStrategyHost (RouteHost) that notes every callback and its
// presentation into one test-owned event log (g_log); the checks read that log. The selected
// side registers an observer and a window through the exported strategy_set_execution_
// observer_v1 / strategy_set_selected_window_v1; the OFF side registers neither. The window
// starts at the first row (N = 0), so the selected curve is the anchor plus the SAME M
// observations the OFF curve holds, and the comparison is anchor-aware: trades and scalars
// field by field, selected points 1..M against OFF points 0..M-1, the anchor at T with the
// initial capital, and the metrics the documented report semantics leave equal. The anchorless OFF
// curve is never asserted equal to the selected curve.
//
// Rows (stable labels, printed as "ROW <label>"; the table in main() lists them in run
// order): the synthetic controls of the row dispatcher, the log predicates and the OFF /
// selected comparison, then the module publish with run_backtest, the module publish with
// run_backtest_full (1m to 5m) and the C ABI stream.
//
// Verdicts: a row is a PASS only with at least one check, no failed check and no exception.
// The run stops before the first row that is not a PASS, lists the later rows NOT RUN,
// prints COMPLETE only when every row ran and passed, and exits 0 only then. A report whose
// positive length has no array behind it, a route that was never reached and a missing
// output are FAILs; no comparison is skipped as a pass.
//
// Negative controls (what each external check rejects):
//   * observer moved after the pure final work / results open: the observation taken INSIDE
//     the observer must read phase Sealed and the counts getter must still answer -2;
//   * an extra post-seal host dispatch (a callback or a presentation after the observer,
//     including during the single read): host_events_after_observer() over the log that runs
//     through the read must be 0 and the observer must be the last event of the whole log;
//   * the observer firing zero or several times, or before the capture's presentation;
//   * a selected run presenting a different number of times than OFF (both must be 1).
//
// Not covered by this TU: the codegen-emitted wrapper
// (pineforge_codegen/codegen/emit_top.py _emit_extern_c, a distinct emitted route), the
// terminal-quote rows of tests/test_selected_window_routes.cpp, allocation failure, copies
// and repeated reads. The stream flow uses
// confirmed bars; ticks and advance_time are not exercised.
//
// It includes no pineforge/source header, so it
// is kernel-capable and registers in the kernel-only profile too. Assumptions the rows
// make (a run may falsify them): the first observation of each tape is flat so
// the anchor equals it; the stream route consumes the two warmup rows and the three pushed
// rows as five script bars; strategy_stream_fill_report after strategy_stream_end answers
// from the selected capture; strategy_get_last_error is empty after a clean run.
#include <pineforge/execution_observer.h>
#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>
#include <pineforge/native_module.hpp>
#include <pineforge/native_toolkit.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/selected_window.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
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
// dispatcher: it silences the console, and execute_synthetic discards the tallies.
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
// first row that is not a PASS; every row after it is recorded NOT RUN and is never counted
// as passed, and the run reads COMPLETE only when every row ran and passed.

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

// The one verdict of a finished row. An exception outranks a failed check, which outranks an
// empty row; only a clean row with checks is a PASS.
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

// A synthetic row through the real dispatcher: its deliberate failures and its console noise
// never reach the real tallies. The row that called this keeps its own counts.
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

// Runs `rows` in order through `run_one` and stops before the first row after one that is
// not a PASS: every later row is recorded NOT RUN without being run. True only when no row
// blocked the sequence.
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

// COMPLETE needs exactly the expected rows recorded and every one of them a PASS: a row that
// is missing, NOT RUN or not a PASS keeps the whole run from reading COMPLETE.
bool sequence_complete(const std::vector<RowOutcome>& outcomes, std::size_t expected) {
    if (expected == 0 || outcomes.size() != expected) return false;
    for (const RowOutcome& outcome : outcomes) {
        if (outcome.verdict != RowVerdict::Pass) return false;
    }
    return true;
}

// ---- the external event log -----------------------------------------------------

bool is_host_event(const std::string& event) { return event.compare(0, 5, "host.") == 0; }

struct EventLog {
    std::vector<std::string> events;
    void clear() { events.clear(); }
    void note(const std::string& what) { events.push_back(what); }
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

// The observer ran exactly once, after every host event; when `presentation_last`, the event
// just before it is the capture's presentation (the last permitted host work).
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

// The numbers pf_execution_observation_v1 carries (execution_observer.h).
constexpr std::uint32_t kPhaseSealed = 3;
constexpr std::uint32_t kPhaseResults = 4;
constexpr std::uint32_t kFaultNone = 0;
constexpr std::uint32_t kOutcomeResultsOpen = 4;

bool near(double actual, double expected, double tolerance) {
    return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
}

// Five one-minute rows: price 50, 50, then 100 / 110 / 104.5 (the tape of
// test_selected_window_report_integration). A buy submitted at the close of script bar 0
// fills at the open of bar 1; a flatten submitted at the close of bar 2 fills at the open of
// bar 3: one closed row.
const Bar kTape[5] = {
    {50.0, 50.0, 50.0, 50.0, 1.0, kT0},
    {50.0, 50.0, 50.0, 50.0, 1.0, kT0 + 1 * kMinute},
    {100.0, 100.0, 100.0, 100.0, 1.0, kT0 + 2 * kMinute},
    {100.0, 110.0, 100.0, 110.0, 1.0, kT0 + 3 * kMinute},
    {110.0, 110.0, 104.5, 104.5, 1.0, kT0 + 4 * kMinute},
};

struct RouteTape {
    std::vector<Bar> rows;
    std::string input_tf;
    std::string script_tf;
    int warmup = 0;
    int enter_bar = 0;
    int exit_bar = 2;
    std::uint64_t input_bars = 0;
    std::uint64_t script_bars = 0;
    bool exact_trades = true;  // one closed row expected (batch semantics)
};

// Passthrough 1m -> 1m over the five rows. `stream` splits them into two warmup rows and
// three confirmed pushes (the stream route consumes all five as script bars).
RouteTape simple_tape(bool stream) {
    RouteTape tape;
    tape.input_tf = "1";
    tape.script_tf = "1";
    tape.rows.assign(kTape, kTape + 5);
    tape.enter_bar = 0;
    tape.exit_bar = 2;
    tape.input_bars = 5;
    tape.script_bars = 5;
    if (stream) {
        tape.warmup = 2;
        tape.exact_trades = false;  // the stream fill timing is not asserted, only parity
    }
    return tape;
}

// Fifteen 1m rows in three 5m buckets priced 50 / 60 / 70: a buy at the close of bucket 0
// fills at bucket 1's open, the flatten at its close fills at bucket 2's open.
RouteTape full_tape() {
    RouteTape tape;
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

NativeRunSpec spec_of(const char* key, const char* input_tf, const char* script_tf) {
    NativeRunSpec s;
    s.identity = {key, 1};
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

std::vector<pf_bar_t> to_c_bars(const std::vector<Bar>& rows) {
    std::vector<pf_bar_t> out;
    out.reserve(rows.size());
    for (const Bar& row : rows) {
        const pf_bar_t bar{row.open, row.high, row.low, row.close, row.volume, row.timestamp};
        out.push_back(bar);
    }
    return out;
}

// ---- the observer ----------------------------------------------------------------

struct Probe {
    pf_strategy_t handle = nullptr;
    int calls = 0;
    int observation_rc = 99;
    int counts_inside = 99;
    pf_execution_observation_v1 inside{};
    std::uint64_t boundary_generation = 0;
    std::uint64_t boundary_serial = 0;
};

// Notes itself first, then reads the two getters that are allowed inside the observer, fills
// the receipt's sentinel and answers 0.
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
    return 0;
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
// attempt does not own results yet, and the counts are still closed. A kernel that ran the
// observer after the results opened (after the pure final work) fails here.
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

// ---- the host: a custom NativeStrategyHost, not `final` (the module derives from it) ----

class RouteHost : public NativeStrategyHost {
public:
    int enter_bar = -1;
    int exit_bar = -1;

    void on_native_run_begin() override { g_log.note("host.run_begin"); }
    void on_native_input(const Bar&, const NativeInputContext&) override {
        g_log.note("host.input");
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        g_log.note("host.bar_open");
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        g_log.note("host.bar");
        const int index = bars_seen_++;
        if (index == enter_bar) (void)submit({no::Transact{1.0}, "entry", ""});
        if (index == exit_bar) (void)submit({no::Flatten{}, "exit", ""});
    }
    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        g_log.note("host.applied");
    }
    void present_report(ReportC*) const override { g_log.note("host.present_report"); }

private:
    int bars_seen_ = 0;
};

}  // namespace

// The production module macro: the whole loadable-module C ABI of RouteHost. It defines
// strategy_create, strategy_free, strategy_set_input, strategy_set_override,
// strategy_set_magnifier_volume_weighted, run_backtest, run_backtest_full and report_free at
// global scope; the rows below call those exports.
PINEFORGE_EXPORT_NATIVE_STRATEGY(RouteHost);

namespace {

// ---- reports -------------------------------------------------------------------------

// The strategy handle of a module run, released on every exit through the module's own
// strategy_free export.
struct ModuleHandle {
    pf_strategy_t handle;
    explicit ModuleHandle(pf_strategy_t h) : handle(h) {}
    ModuleHandle(const ModuleHandle&) = delete;
    ModuleHandle& operator=(const ModuleHandle&) = delete;
    ~ModuleHandle() { strategy_free(handle); }
};

// Everything one route flow produced and read, kept as values after its handle is gone.
struct Observed {
    pf_report_t report;
    void (*free_report)(pf_report_t*) = nullptr;
    bool ran = false;
    bool completed = false;
    int run_status = -99;
    std::string error;
    Probe probe;
    pf_execution_observation_v1 opened{};
    int opened_rc = -99;
    pf_selected_window_counts_v1 counts{};
    pf_selected_window_counts_v1 counts_before{};
    int counts_rc = 99;
    std::vector<std::string> log;

    Observed() { std::memset(&report, 0, sizeof report); }
    Observed(const Observed&) = delete;
    Observed& operator=(const Observed&) = delete;
    ~Observed() {
        if (free_report != nullptr) free_report(&report);
    }
};

// What the OFF and the selected run of one route are compared on, by value.
void collect(pf_strategy_t handle, const NativeStrategyHost& native, bool selected,
             Observed& seen) {
    seen.completed = native.native_state().kind == NativeLifecycleKind::Completed;
    seen.run_status = strategy_last_run_status(handle);
    const char* error = strategy_get_last_error(handle);
    seen.error = error != nullptr ? error : "";
    seen.opened = blank_observation();
    seen.opened_rc = strategy_execution_observation_v1(handle, &seen.opened);
    seen.counts = selected ? blank_counts() : sentinel_counts();
    seen.counts_before = seen.counts;
    seen.counts_rc = strategy_selected_window_counts_v1(handle, &seen.counts);
    seen.log = g_log.events;
}

// The selected side registers its observer and its window through the exported setters; the
// OFF side registers neither.
void arm_selection(pf_strategy_t handle, const RouteTape& tape, bool selected, Observed& seen) {
    seen.probe.handle = handle;
    if (!selected) return;
    const pf_execution_observer_v1 observer = observer_of(&seen.probe);
    CHECK(strategy_set_execution_observer_v1(handle, &observer) == 0);
    const pf_selected_window_config_v1 window = window_from(tape.rows.front().timestamp);
    CHECK(strategy_set_selected_window_v1(handle, &window) == 0);
}

// The native module route: the macro's own strategy_create, then run_backtest (simple) or
// run_backtest_full (the timeframe overload), which run the engine and publish the report
// through native_module::publish_report; the report is freed through the macro's report_free.
void drive_module(const RouteTape& tape, bool full, bool selected, Observed& seen) {
    g_log.clear();
    const pf_strategy_t handle = strategy_create(nullptr);
    CHECK(handle != nullptr);
    if (handle == nullptr) return;
    const ModuleHandle owner(handle);
    BacktestEngine* engine = static_cast<BacktestEngine*>(handle);
    RouteHost* host = dynamic_cast<RouteHost*>(engine);
    NativeStrategyHost* native = dynamic_cast<NativeStrategyHost*>(engine);
    CHECK(host != nullptr);
    CHECK(native != nullptr);
    if (host == nullptr || native == nullptr) return;
    host->enter_bar = tape.enter_bar;
    host->exit_bar = tape.exit_bar;
    const NativeRunSpec spec = spec_of("wspr-module", tape.input_tf.c_str(), tape.script_tf.c_str());
    CHECK(native->configure_native(spec).status == NativeSetupStatus::Applied);
    arm_selection(handle, tape, selected, seen);

    std::vector<pf_bar_t> bars = to_c_bars(tape.rows);
    const int n = static_cast<int>(bars.size());
    seen.free_report = &report_free;
    if (full) {
        run_backtest_full(handle, bars.data(), n, tape.input_tf.c_str(), tape.script_tf.c_str(),
                          0, 4, PF_MAGNIFIER_ENDPOINTS, &seen.report);
    } else {
        run_backtest(handle, bars.data(), n, &seen.report);
    }
    seen.ran = true;
    collect(handle, *native, selected, seen);
}

// A stream call that must answer 0; a refusal prints the engine's own text.
void stream_ok(int rc, const char* what, pf_strategy_t handle) {
    CHECK(rc == 0);
    if (rc != 0 && !g_quiet) {
        const char* error = strategy_get_last_error(handle);
        std::fprintf(stderr, "  %s answered %d: %s\n", what, rc, error != nullptr ? error : "<none>");
    }
}

// The C ABI stream route: the exported strategy_stream_begin / strategy_stream_push_bar /
// strategy_stream_end / strategy_stream_fill_report on a handle; the report is the stream
// snapshot after the end and is freed through strategy_native_report_free_v1.
void drive_stream(const RouteTape& tape, bool selected, Observed& seen) {
    g_log.clear();
    RouteHost host;
    host.enter_bar = tape.enter_bar;
    host.exit_bar = tape.exit_bar;
    const NativeRunSpec spec = spec_of("wspr-stream", tape.input_tf.c_str(), tape.script_tf.c_str());
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    BacktestEngine& engine = host;
    const pf_strategy_t handle = handle_of(engine);
    arm_selection(handle, tape, selected, seen);

    std::vector<pf_bar_t> bars = to_c_bars(tape.rows);
    seen.free_report = &strategy_native_report_free_v1;
    stream_ok(strategy_stream_begin(handle, bars.data(), tape.warmup, tape.input_tf.c_str(),
                                    tape.script_tf.c_str()),
              "strategy_stream_begin", handle);
    for (std::size_t i = static_cast<std::size_t>(tape.warmup); i < bars.size(); ++i) {
        stream_ok(strategy_stream_push_bar(handle, &bars[i]), "strategy_stream_push_bar", handle);
    }
    stream_ok(strategy_stream_end(handle, 0), "strategy_stream_end", handle);
    stream_ok(strategy_stream_fill_report(handle, &seen.report), "strategy_stream_fill_report",
              handle);
    seen.ran = true;
    collect(handle, host, selected, seen);
}

// ---- OFF versus selected --------------------------------------------------------------

// The presented trades and scalars equal, and the selected curve is the anchor (at T, the
// initial capital, no open profit) plus the OFF curve's observations, point for point. Works
// for ReportC and pf_report_t (the arrays and counts they share). A positive count promises
// an array on each side: a missing array is a FAIL of its own and the element comparison
// stops short of it, never skipped as a pass.
template <typename R>
void compare_off_selected(const R& off, const R& sel, std::int64_t anchor_time,
                          double anchor_equity) {
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
    // the anchor plus those M), so both arrays must be present.
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

// The scalars and metrics the documented report semantics leave equal when the window starts at the
// first row and the first observation is flat: M observations on both sides, the anchor equal
// to the first observation, buy-and-hold over the same first open and last close. The
// return-statistics block is NOT compared (the selected run has M returns, OFF has M - 1).
template <typename R>
void compare_metrics(const R& off, const R& sel) {
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

// One production route: the OFF flow and the selected flow of the same tape.
void check_single_read(const RouteTape& tape, const Observed& off, const Observed& sel) {
    // Both flows reached their real entry, completed and left no error.
    CHECK(off.ran);
    CHECK(sel.ran);
    CHECK(off.completed);
    CHECK(sel.completed);
    CHECK(off.run_status == 0);
    CHECK(sel.run_status == 0);
    CHECK(off.error.empty());
    CHECK(sel.error.empty());

    // OFF: no counts, caller storage untouched, no observer, one presentation (its one read).
    CHECK(off.counts_rc == -2);
    CHECK(same_counts(off.counts, off.counts_before));
    CHECK(count_of(off.log, "observer") == 0);
    CHECK(count_of(off.log, "host.present_report") == 1);

    // Selected: the observer fired once, inside the sealed generation, and the results opened
    // after it; the identities agree and the counts equal the tape by hand.
    check_observer_inside(sel.probe);
    CHECK(sel.opened_rc == 0);
    CHECK(sel.opened.phase == kPhaseResults);
    CHECK(sel.opened.fault_stage == kFaultNone);
    CHECK(sel.opened.attempt_outcome == kOutcomeResultsOpen);
    CHECK(sel.opened.boundary_delivered == 1);
    CHECK(sel.opened.run_generation == sel.probe.boundary_generation);
    CHECK(sel.opened.attempt_serial == sel.probe.boundary_serial);
    CHECK(sel.counts_rc == 0);
    CHECK(sel.counts.run_generation == sel.opened.run_generation);
    CHECK(sel.counts.attempt_serial == sel.opened.attempt_serial);
    CHECK(sel.counts.attempt_generation == sel.opened.attempt_generation);
    CHECK(sel.counts.fed_input_bars == tape.input_bars);
    CHECK(sel.counts.preroll_input_bars == 0);
    CHECK(sel.counts.window_input_bars == tape.input_bars);
    CHECK(sel.counts.fed_script_bars == tape.script_bars);
    CHECK(sel.counts.preroll_script_bars == 0);
    CHECK(sel.counts.window_script_bars == tape.script_bars);

    // The external counters over the whole flow, the single read included: one observer, after
    // the last permitted host work (the capture's presentation), nothing dispatched after it,
    // and the observer is the last event of the log.
    CHECK(count_of(sel.log, "observer") == 1);
    CHECK(observer_follows_last_host_work(sel.log, true));
    CHECK(host_events_after_observer(sel.log) == 0);
    CHECK(!sel.log.empty() && sel.log.back() == "observer");
    // One presentation per single-read route, selected and OFF alike, and the same execution.
    CHECK(count_of(sel.log, "host.present_report") == 1);
    CHECK(count_of(sel.log, "host.present_report") == count_of(off.log, "host.present_report"));
    CHECK(count_of(sel.log, "host.run_begin") == count_of(off.log, "host.run_begin"));
    CHECK(count_of(sel.log, "host.input") == count_of(off.log, "host.input"));
    CHECK(count_of(sel.log, "host.bar_open") == count_of(off.log, "host.bar_open"));
    CHECK(count_of(sel.log, "host.bar") == count_of(off.log, "host.bar"));
    CHECK(count_of(sel.log, "host.applied") == count_of(off.log, "host.applied"));
    CHECK(count_of(sel.log, "host.bar") == static_cast<int>(tape.script_bars));

    // The real published reports, by hand from the tape: the OFF curve holds one point per
    // script bar and the selected curve the anchor plus the same M.
    CHECK(static_cast<std::uint64_t>(off.report.input_bars_processed) == tape.input_bars);
    CHECK(static_cast<std::uint64_t>(off.report.script_bars_processed) == tape.script_bars);
    CHECK(static_cast<std::uint64_t>(sel.report.input_bars_processed) == tape.input_bars);
    CHECK(static_cast<std::uint64_t>(sel.report.script_bars_processed) == tape.script_bars);
    CHECK(static_cast<std::uint64_t>(off.report.equity_curve_len) == tape.script_bars);
    CHECK(static_cast<std::uint64_t>(sel.report.equity_curve_len) == tape.script_bars + 1);
    if (tape.exact_trades) {
        CHECK(off.report.trades_len == 1);
        CHECK(sel.report.trades_len == 1);
    }
    compare_off_selected(off.report, sel.report, tape.rows.front().timestamp, kCapital);
    compare_metrics(off.report, sel.report);
}

// ---- synthetic checker controls (they run before any real row) --------------------------
//
// They feed the real dispatcher bodies that must NOT read as PASS -- a body that checks
// nothing, a body with a failed check, a body that throws -- and one that must. The external
// counter g_synthetic_runs counts the bodies that really ran, so a sequencer that kept going
// after a bad row is caught by a number it did not compute itself.

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

// The log predicates behind the negative controls accept a good log and reject: a
// presentation after the observer, the observer before the presentation, work between the
// presentation and the observer (strict form), no observer, two observers.
void row_checker_teeth() {
    const std::vector<std::string> good = {"host.run_begin", "host.bar", "host.present_report",
                                           "observer"};
    CHECK(host_events_after_observer(good) == 0);
    CHECK(observer_follows_last_host_work(good, true));
    CHECK(observer_follows_last_host_work(good, false));

    const std::vector<std::string> extra = {"host.bar", "host.present_report", "observer",
                                            "host.present_report"};
    CHECK(host_events_after_observer(extra) == 1);
    CHECK(!observer_follows_last_host_work(extra, true));
    CHECK(!observer_follows_last_host_work(extra, false));

    const std::vector<std::string> early = {"host.bar", "observer", "host.present_report"};
    CHECK(host_events_after_observer(early) == 1);
    CHECK(!observer_follows_last_host_work(early, true));

    const std::vector<std::string> gap = {"host.present_report", "host.bar", "observer"};
    CHECK(observer_follows_last_host_work(gap, false));
    CHECK(!observer_follows_last_host_work(gap, true));

    const std::vector<std::string> none = {"host.bar", "host.present_report"};
    CHECK(host_events_after_observer(none) == -1);
    CHECK(!observer_follows_last_host_work(none, false));
    const std::vector<std::string> twice = {"host.bar", "observer", "observer"};
    CHECK(host_events_after_observer(twice) == -1);
    CHECK(!observer_follows_last_host_work(twice, false));
}

// A flow that never ran, with no report, no log and no observer, must FAIL the route checker
// (a missing output is never a pass); the report comparison must FAIL over a positive length
// with no array behind it and stop short of the missing array (no crash); and a complete
// matching pair of in-memory reports must PASS it.
void synthetic_never_ran_row() {
    const RouteTape tape = simple_tape(false);
    const Observed off;
    const Observed sel;
    check_single_read(tape, off, sel);
}

void synthetic_missing_arrays_row() {
    pf_report_t off;
    pf_report_t sel;
    std::memset(&off, 0, sizeof off);
    std::memset(&sel, 0, sizeof sel);
    off.trades_len = 1;
    off.equity_curve_len = 2;
    sel.trades_len = 1;
    sel.equity_curve_len = 3;
    compare_off_selected(off, sel, kT0, kCapital);
}

void synthetic_matching_pair_row() {
    std::vector<pf_trade_t> off_trades(1);
    std::memset(off_trades.data(), 0, sizeof(pf_trade_t));
    off_trades[0].entry_time = kT0;
    off_trades[0].exit_time = kT0 + kMinute;
    off_trades[0].qty = 1.0;
    off_trades[0].pnl = 10.0;
    std::vector<pf_trade_t> sel_trades = off_trades;
    std::vector<pf_equity_point_t> off_curve(2);
    off_curve[0] = pf_equity_point_t{kT0, kCapital, 0.0};
    off_curve[1] = pf_equity_point_t{kT0 + kMinute, kCapital + 10.0, 10.0};
    std::vector<pf_equity_point_t> sel_curve(3);
    sel_curve[0] = pf_equity_point_t{kT0, kCapital, 0.0};
    sel_curve[1] = off_curve[0];
    sel_curve[2] = off_curve[1];
    pf_report_t off;
    pf_report_t sel;
    std::memset(&off, 0, sizeof off);
    std::memset(&sel, 0, sizeof sel);
    off.trades = off_trades.data();
    off.trades_len = 1;
    off.equity_curve = off_curve.data();
    off.equity_curve_len = 2;
    off.script_bars_processed = 2;
    sel.trades = sel_trades.data();
    sel.trades_len = 1;
    sel.equity_curve = sel_curve.data();
    sel.equity_curve_len = 3;
    sel.script_bars_processed = 2;
    compare_off_selected(off, sel, kT0, kCapital);
}

void row_compare_teeth() {
    const RowOutcome never = execute_synthetic(RowSpec{"synthetic-never-ran", &synthetic_never_ran_row});
    CHECK(never.verdict == RowVerdict::Failed);
    CHECK(never.failures > 0);
    const RowOutcome missing =
        execute_synthetic(RowSpec{"synthetic-missing-arrays", &synthetic_missing_arrays_row});
    CHECK(missing.verdict == RowVerdict::Failed);
    CHECK(missing.failures > 0);
    const RowOutcome matching =
        execute_synthetic(RowSpec{"synthetic-matching-pair", &synthetic_matching_pair_row});
    CHECK(matching.verdict == RowVerdict::Pass);
    CHECK(matching.failures == 0);
    CHECK(matching.checks > 0);
}

// ---- production rows -------------------------------------------------------------------

// The native module entry, simple route: the macro's run_backtest.
void row_module_publish_simple() {
    const RouteTape tape = simple_tape(false);
    Observed off;
    drive_module(tape, false, false, off);
    Observed sel;
    drive_module(tape, false, true, sel);
    check_single_read(tape, off, sel);
}

// The native module entry, timeframe route: the macro's run_backtest_full with 1m -> 5m.
void row_module_publish_full() {
    const RouteTape tape = full_tape();
    Observed off;
    drive_module(tape, true, false, off);
    Observed sel;
    drive_module(tape, true, true, sel);
    check_single_read(tape, off, sel);
}

// The C ABI stream family: the exported strategy_stream_* functions end to end.
void row_c_abi_stream() {
    const RouteTape tape = simple_tape(true);
    Observed off;
    drive_stream(tape, false, off);
    Observed sel;
    drive_stream(tape, true, sel);
    check_single_read(tape, off, sel);
}

}  // namespace

int main() {
    // The synthetic checker controls come first: a broken dispatcher, log
    // predicate or comparison stops the run before any production row. The sequence also
    // stops before the first row that is not a PASS (a failed check, an exception or no check
    // at all); the rows after it are listed NOT RUN and nothing reads COMPLETE.
    static const RowSpec rows[] = {
        {"PRODUCTION-VERDICT-CONTROL", &row_control_verdicts},
        {"PRODUCTION-SEQUENCE-CONTROL", &row_control_sequence},
        {"PRODUCTION-CHECKER-CONTROL", &row_checker_teeth},
        {"PRODUCTION-COMPARE-CONTROL", &row_compare_teeth},
        {"PRODUCTION-MODULE-SIMPLE", &row_module_publish_simple},
        {"PRODUCTION-MODULE-FULL", &row_module_publish_full},
        {"PRODUCTION-C-ABI-STREAM", &row_c_abi_stream},
    };
    const std::size_t count = sizeof rows / sizeof rows[0];

    std::vector<RowOutcome> outcomes;
    (void)run_sequence(rows, count, &execute_row, &outcomes);
    const bool complete = sequence_complete(outcomes, count);

    std::printf("---- selected-window production routes summary ----\n");
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
