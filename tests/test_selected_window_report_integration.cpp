// Selected-window report capture and assembly on the native host (the pure
// helper report::build_selected_equity_curve and the observation-count
// overload of the equity statistics).
//
// Native-host fixtures driven only from outside: a bare NativeStrategyHost with
// a recording report presentation, the private bridges behind the selected-window
// and observer C exports, a batch run() and fill_report(). Every expected value
// below is a literal worked by hand from the tape given (and, for the metrics, from
// the report arithmetic: returns anchor first, exposure over the M observations,
// drawdown from the running peak, buy-and-hold from the first window input's open
// to the last close); none is read back from the code under test.
//
// The tape: five one-minute rows, a pre-roll price of 50 and a window price of
// 100 / 110 / 104.5, passthrough 1m -> 1m, 24x7 UTC, a capital of 1000 and
// KernelRecorded so the engine itself records one equity observation per script
// bar:
//
//   m0  O50  H50  L50  C50     pre-roll
//   m1  O50  H50  L50  C50     pre-roll
//   m2  O100 H100 L100 C100    window bar 1 (T): the host buys one unit at its close
//   m3  O100 H110 L100 C110    window bar 2: the buy fills at its open, 100
//   m4  O110 H110 L104.5 C104.5 window bar 3
//
// With T = m2 the engine's observations are m2: 1000 (flat), m3: 1000 + (110 - 100)
// = 1010, m4: 1000 + (104.5 - 100) = 1004.5; the position is open at the last two.
//
// It is registered in tests/CMakeLists.txt with the other selected-window rows.
#include <pineforge/execution_observer.h>
#include <pineforge/native_host.hpp>
#include <pineforge/native_toolkit.hpp>
#include <pineforge/selected_window.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
// The private bridges behind the four C exports, declared exactly as
// src/c_abi.cpp declares them; the native consumer owns every check.
int native_set_execution_observer_v1(BacktestEngine*, const pf_execution_observer_v1*) noexcept;
int native_execution_observation_v1(const BacktestEngine*, pf_execution_observation_v1*) noexcept;
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
constexpr double kCapital = 1000.0;

// The numbers pf_execution_observation_v1 carries (execution_observer.h).
constexpr std::uint32_t kPhaseSealed = 3;
constexpr std::uint32_t kPhaseResults = 4;
constexpr std::uint32_t kFaultExecution = 1;
constexpr std::uint32_t kFaultAfterExecution = 2;
constexpr std::uint32_t kOutcomeFailed = 2;
constexpr std::uint32_t kOutcomeResultsOpen = 4;

bool near(double actual, double expected, double tolerance) {
    return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
}

// A bare host that records its presentation calls, optionally buys one unit at the
// close of the first script bar labelled at or after `enter_from_ms`, and can edit
// or refuse the presented report.
struct ReportHost final : NativeStrategyHost {
    std::int64_t enter_from_ms = -1;
    bool entered = false;
    bool throw_in_present = false;
    bool shrink_presented_curve = false;
    mutable int present_calls = 0;

    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        if (enter_from_ms >= 0 && !entered && context.script_bar_open_ms >= enter_from_ms) {
            entered = true;
            (void)submit({no::Transact{1.0}, "entry", ""});
        }
    }
    void present_report(ReportC* report) const override {
        ++present_calls;
        if (throw_in_present) throw std::runtime_error("report fixture: presentation refused");
        if (shrink_presented_curve && report->equity_curve_len > 1) report->equity_curve_len = 1;
    }
};

NativeRunSpec spec_of(const char* key, std::uint64_t run_number = 1,
                      NativeReportPolicy policy = NativeReportPolicy::KernelRecorded) {
    NativeRunSpec s;
    s.identity = {key, run_number};
    s.input_tf = "1";
    s.script_tf = "1";
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
    s.report_policy = policy;
    return s;
}

const Bar kTape[5] = {
    {50.0, 50.0, 50.0, 50.0, 1.0, kT0},
    {50.0, 50.0, 50.0, 50.0, 1.0, kT0 + 1 * kMinute},
    {100.0, 100.0, 100.0, 100.0, 1.0, kT0 + 2 * kMinute},
    {100.0, 110.0, 100.0, 110.0, 1.0, kT0 + 3 * kMinute},
    {110.0, 110.0, 104.5, 104.5, 1.0, kT0 + 4 * kMinute},
};

// The first `count` rows of the tape.
std::vector<Bar> tape(std::size_t count) {
    return std::vector<Bar>(kTape, kTape + count);
}

// Rows at the given minutes, all at one flat price.
std::vector<Bar> flat_rows(const std::vector<int>& minutes, double price) {
    std::vector<Bar> rows;
    for (const int minute : minutes) {
        rows.push_back(Bar{price, price, price, price, 1.0, kT0 + minute * kMinute});
    }
    return rows;
}

// The window [kT0 + start_minute, kT0 + 1000 minutes): T is start_minute.
pf_selected_window_config_v1 window_at(int start_minute) {
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof window);
    window.version = 1;
    window.start_ms = kT0 + start_minute * kMinute;
    window.end_ms = kT0 + 1000 * kMinute;
    return window;
}

// Configures `host`, registers the observer (when given), sets or clears the
// window and runs the batch -- the order the selected-window harness uses.
void run_host(ReportHost& host, const NativeRunSpec& spec, const std::vector<Bar>& rows,
              const pf_selected_window_config_v1* window,
              const pf_execution_observer_v1* observer = nullptr) {
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    if (observer != nullptr) CHECK(native_set_execution_observer_v1(&host, observer) == 0);
    CHECK(native_set_selected_window_v1(&host, window) == 0);
    host.run(rows.data(), static_cast<int>(rows.size()));
}

// A report read, freed on every exit.
struct Report {
    ReportC raw{};
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
    ~Report() { BacktestEngine::free_report(&raw); }
};

pf_selected_window_counts_v1 blank_counts() {
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = static_cast<std::uint32_t>(sizeof counts);
    counts.version = 1;
    return counts;
}

pf_execution_observation_v1 observation_of(const ReportHost& host) {
    pf_execution_observation_v1 seen{};
    seen.struct_size = static_cast<std::uint32_t>(sizeof seen);
    seen.version = 1;
    CHECK(native_execution_observation_v1(&host, &seen) == 0);
    return seen;
}

struct Point {
    std::int64_t time_ms;
    double equity;
    double open_profit;
};

// The report's curve, point for point: length, then each time exactly and each
// money value to a tolerance far below any amount in these tapes.
void check_curve(const ReportC& report, const std::vector<Point>& expected) {
    CHECK(report.equity_curve_len == static_cast<std::int64_t>(expected.size()));
    if (report.equity_curve == nullptr
        || report.equity_curve_len != static_cast<std::int64_t>(expected.size())) {
        CHECK(report.equity_curve != nullptr);
        return;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(report.equity_curve[i].time_ms == expected[i].time_ms);
        CHECK(near(report.equity_curve[i].equity, expected[i].equity, 1e-9));
        CHECK(near(report.equity_curve[i].open_profit, expected[i].open_profit, 1e-9));
    }
}

// Nothing of a run is readable: the empty report, every array null, every count
// zero.
void check_empty_report(const ReportC& report) {
    CHECK(report.trades == nullptr);
    CHECK(report.trades_len == 0);
    CHECK(report.equity_curve == nullptr);
    CHECK(report.equity_curve_len == 0);
    CHECK(report.script_bars_processed == 0);
}

// A selected run whose host never trades: a flat account, so every observation
// is the initial capital. Rows m0..m4 at the pre-roll price, T = m2: three window
// bars, M = 3. The curve is the anchor at T and then the three window
// observations in the order they were recorded -- the two pre-roll observations
// (m0, m1) are in the engine's own record but not in the report.
void preroll_and_window_has_no_pre_t_equity() {
    std::printf("preroll_and_window_has_no_pre_t_equity\n");
    ReportHost host;
    const pf_selected_window_config_v1 window = window_at(2);
    run_host(host, spec_of("wsr-flat"), flat_rows({0, 1, 2, 3, 4}, 50.0), &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);

    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    CHECK(counts.preroll_script_bars == 2);
    CHECK(counts.window_script_bars == 3);

    Report report;
    host.fill_report(&report.raw);
    check_curve(report.raw, {{kT0 + 2 * kMinute, kCapital, 0.0},
                             {kT0 + 2 * kMinute, kCapital, 0.0},
                             {kT0 + 3 * kMinute, kCapital, 0.0},
                             {kT0 + 4 * kMinute, kCapital, 0.0}});
    // The first window bar stands beside the anchor at the same time T, and no
    // point of the report is stamped before T.
    for (std::int64_t i = 0; i < report.raw.equity_curve_len && report.raw.equity_curve; ++i) {
        CHECK(report.raw.equity_curve[i].time_ms >= kT0 + 2 * kMinute);
    }
    // The scalars still describe everything the run was fed, pre-roll included.
    CHECK(report.raw.input_bars_processed == 5);
    CHECK(report.raw.script_bars_processed == 5);
    // A flat account: nothing in the market, nothing open, no drawdown.
    CHECK(near(report.raw.metrics.equity.time_in_market_pct, 0.0, 1e-12));
    CHECK(near(report.raw.metrics.equity.open_pl, 0.0, 1e-12));
    CHECK(near(report.raw.metrics.equity.max_equity_drawdown, 0.0, 1e-12));
    CHECK(host.present_calls == 1);
}

// T is m2 but the first window row is m4: the anchor stays at T with the initial
// capital, and the span of the curve runs from the anchor, not from the first bar.
void anchor_at_t_with_the_first_bar_later() {
    std::printf("anchor_at_t_with_the_first_bar_later\n");
    ReportHost host;
    const pf_selected_window_config_v1 window = window_at(2);
    run_host(host, spec_of("wsr-gap"), flat_rows({0, 1, 4, 5}, 50.0), &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);

    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    CHECK(counts.preroll_script_bars == 2);
    CHECK(counts.window_script_bars == 2);
    CHECK(counts.window_input_bars == 2);

    Report report;
    host.fill_report(&report.raw);
    check_curve(report.raw, {{kT0 + 2 * kMinute, kCapital, 0.0},
                             {kT0 + 4 * kMinute, kCapital, 0.0},
                             {kT0 + 5 * kMinute, kCapital, 0.0}});
}

// Every row precedes T: no window bar was ever calculated. The curve is the anchor
// alone, the window counts are zero, and the statistics keep their undefined
// domains -- buy-and-hold has no window price to start from (the pre-roll price of
// 50 is not borrowed: borrowing it would read 0 %, not undefined), and exposure
// has no observation to divide by.
void no_window_preroll_only_is_the_anchor_alone() {
    std::printf("no_window_preroll_only_is_the_anchor_alone\n");
    ReportHost host;
    const pf_selected_window_config_v1 window = window_at(5);
    run_host(host, spec_of("wsr-empty"), flat_rows({0, 1}, 50.0), &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);

    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    CHECK(counts.window_script_bars == 0);
    CHECK(counts.window_input_bars == 0);
    CHECK(counts.preroll_script_bars == 2);
    CHECK(counts.preroll_input_bars == 2);

    Report report;
    host.fill_report(&report.raw);
    check_curve(report.raw, {{kT0 + 5 * kMinute, kCapital, 0.0}});
    CHECK(report.raw.trades_len == 0);
    CHECK(std::isnan(report.raw.metrics.equity.time_in_market_pct));
    CHECK(std::isnan(report.raw.metrics.equity.buy_hold_return_pct));
    CHECK(std::isnan(report.raw.metrics.equity.buy_hold_return));
    CHECK(near(report.raw.metrics.equity.open_pl, 0.0, 1e-12));
    CHECK(report.raw.input_bars_processed == 2);
}

// One, two and three window observations on the trading tape (T = m2), by hand.
//
// M = 1 (rows m0..m2): the curve is [1000 @T anchor, 1000 @m2]; no position yet, so
//   exposure 0 / 1 = 0 %, open P&L 0, buy-and-hold from the window's first open
//   (100) to its last close (100) is 0 %, and two points are too few for the
//   per-bar return statistics (n >= 3 is required): sharpe_bar is undefined.
// M = 2 (m0..m3): [1000, 1000, 1010]; the buy fills at m3's open, 100, and the close
//   is 110: open P&L +10, equity 1010. One observation in the market of M = 2 is
//   50 % (an anchor-counted denominator would say 1 / 3). The curve never falls, so
//   drawdown 0 and, because a new peak resets the trough, run-up 0. Buy-and-hold:
//   110 / 100 - 1 = 10 %, and 10 % of the 1000 capital is 100.
// M = 3 (m0..m4): [1000, 1000, 1010, 1004.5]; m4 closes at 104.5 with the position
//   still open at 100: open P&L +4.5, equity 1004.5. Two observations in the market
//   of M = 3: 2 / 3 = 66.67 % (anchor-counted: 2 / 4 = 50 %). Drawdown from the peak
//   1010 to 1004.5 is 5.5, 5.5 / 1010 = 0.5446 %, and no run-up. Buy-and-hold:
//   104.5 / 100 - 1 = 4.5 %, 4.5 % of 1000 is 45.
void one_two_three_observations_by_hand() {
    std::printf("one_two_three_observations_by_hand\n");
    const std::int64_t t = kT0 + 2 * kMinute;
    {
        ReportHost host;
        host.enter_from_ms = t;
        const pf_selected_window_config_v1 window = window_at(2);
        run_host(host, spec_of("wsr-m1"), tape(3), &window);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
        CHECK(counts.window_script_bars == 1);
        Report report;
        host.fill_report(&report.raw);
        check_curve(report.raw, {{t, kCapital, 0.0}, {t, kCapital, 0.0}});
        const pf_equity_stats_t& e = report.raw.metrics.equity;
        CHECK(near(e.time_in_market_pct, 0.0, 1e-12));
        CHECK(near(e.open_pl, 0.0, 1e-12));
        CHECK(near(e.buy_hold_return_pct, 0.0, 1e-9));
        CHECK(std::isnan(e.sharpe_bar));
    }
    {
        ReportHost host;
        host.enter_from_ms = t;
        const pf_selected_window_config_v1 window = window_at(2);
        run_host(host, spec_of("wsr-m2"), tape(4), &window);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
        CHECK(counts.window_script_bars == 2);
        Report report;
        host.fill_report(&report.raw);
        check_curve(report.raw, {{t, kCapital, 0.0},
                                 {t, kCapital, 0.0},
                                 {kT0 + 3 * kMinute, 1010.0, 10.0}});
        const pf_equity_stats_t& e = report.raw.metrics.equity;
        CHECK(near(e.time_in_market_pct, 50.0, 1e-9));
        CHECK(near(e.open_pl, 10.0, 1e-9));
        CHECK(near(e.max_equity_drawdown, 0.0, 1e-12));
        CHECK(near(e.max_equity_runup, 0.0, 1e-12));
        CHECK(near(e.buy_hold_return_pct, 10.0, 1e-9));
        CHECK(near(e.buy_hold_return, 100.0, 1e-9));
    }
    {
        ReportHost host;
        host.enter_from_ms = t;
        const pf_selected_window_config_v1 window = window_at(2);
        run_host(host, spec_of("wsr-m3"), tape(5), &window);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
        CHECK(counts.window_script_bars == 3);
        Report report;
        host.fill_report(&report.raw);
        check_curve(report.raw, {{t, kCapital, 0.0},
                                 {t, kCapital, 0.0},
                                 {kT0 + 3 * kMinute, 1010.0, 10.0},
                                 {kT0 + 4 * kMinute, 1004.5, 4.5}});
        const pf_equity_stats_t& e = report.raw.metrics.equity;
        CHECK(near(e.time_in_market_pct, 200.0 / 3.0, 1e-9));
        CHECK(near(e.open_pl, 4.5, 1e-9));
        CHECK(near(e.max_equity_drawdown, 5.5, 1e-9));
        CHECK(near(e.max_equity_drawdown_pct, 5.5 / 1010.0 * 100.0, 1e-9));
        CHECK(near(e.max_equity_runup, 0.0, 1e-12));
        CHECK(near(e.buy_hold_return_pct, 4.5, 1e-9));
        CHECK(near(e.buy_hold_return, 45.0, 1e-9));
    }
}

// Reading the report again never presents again: the host's hook ran once, in the
// capture, and every read is its own independently owned copy of that capture.
void repeated_reads_run_one_presentation() {
    std::printf("repeated_reads_run_one_presentation\n");
    ReportHost host;
    host.enter_from_ms = kT0 + 2 * kMinute;
    const pf_selected_window_config_v1 window = window_at(2);
    run_host(host, spec_of("wsr-reads"), tape(5), &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.present_calls == 1);

    Report first;
    Report second;
    Report third;
    host.fill_report(&first.raw);
    host.fill_report(&second.raw);
    host.fill_report(&third.raw);
    CHECK(host.present_calls == 1);
    CHECK(first.raw.equity_curve != nullptr);
    CHECK(first.raw.equity_curve != second.raw.equity_curve);
    CHECK(second.raw.equity_curve != third.raw.equity_curve);
    const std::vector<Point> expected = {{kT0 + 2 * kMinute, kCapital, 0.0},
                                         {kT0 + 2 * kMinute, kCapital, 0.0},
                                         {kT0 + 3 * kMinute, 1010.0, 10.0},
                                         {kT0 + 4 * kMinute, 1004.5, 4.5}};
    check_curve(first.raw, expected);
    check_curve(second.raw, expected);
    check_curve(third.raw, expected);
    CHECK(near(first.raw.metrics.equity.time_in_market_pct, 200.0 / 3.0, 1e-9));
    CHECK(near(third.raw.metrics.equity.time_in_market_pct, 200.0 / 3.0, 1e-9));
    // The counts do not depend on any read.
    pf_selected_window_counts_v1 counts = blank_counts();
    CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
    CHECK(counts.window_script_bars == 3);
}

// The host's presentation may leave the report's own length behind (here it cuts
// the curve to one point): the length the reader sees is what it left, but the
// metrics keep the capture's own M + 1 points and M observations, so they are the
// ones of the unedited run (the M = 3 values above).
void presented_curve_edit_does_not_move_metrics() {
    std::printf("presented_curve_edit_does_not_move_metrics\n");
    ReportHost host;
    host.enter_from_ms = kT0 + 2 * kMinute;
    host.shrink_presented_curve = true;
    const pf_selected_window_config_v1 window = window_at(2);
    run_host(host, spec_of("wsr-edit"), tape(5), &window);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    Report report;
    host.fill_report(&report.raw);
    CHECK(host.present_calls == 1);
    CHECK(report.raw.equity_curve_len == 1);
    const pf_equity_stats_t& e = report.raw.metrics.equity;
    CHECK(near(e.time_in_market_pct, 200.0 / 3.0, 1e-9));
    CHECK(near(e.open_pl, 4.5, 1e-9));
    CHECK(near(e.max_equity_drawdown, 5.5, 1e-9));
    CHECK(near(e.buy_hold_return_pct, 4.5, 1e-9));
}

// A capture that fails owns the attempt: the run is Failed, its generation is
// Sealed with an execution fault and no results, the counts are unavailable and the
// report read is the empty report -- and the host's hook is not run again by that
// read.
void capture_failures_own_the_attempt() {
    std::printf("capture_failures_own_the_attempt\n");
    const auto failed_without_results = [](ReportHost& host, std::uint32_t fault) {
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        pf_selected_window_counts_v1 counts = blank_counts();
        counts.fed_input_bars = 4242;  // an unavailable getter leaves this alone
        CHECK(native_selected_window_counts_v1(&host, &counts) == -2);
        CHECK(counts.fed_input_bars == 4242);
        const pf_execution_observation_v1 seen = observation_of(host);
        CHECK(seen.phase == kPhaseSealed);
        CHECK(seen.fault_stage == fault);
        CHECK(seen.attempt_outcome == kOutcomeFailed);
        Report report;
        host.fill_report(&report.raw);
        check_empty_report(report.raw);
    };
    {
        // The host's own presentation refuses: the hook ran once, in the capture.
        ReportHost host;
        host.throw_in_present = true;
        const pf_selected_window_config_v1 window = window_at(2);
        run_host(host, spec_of("wsr-throw"), flat_rows({0, 1, 2, 3, 4}, 50.0), &window);
        failed_without_results(host, kFaultExecution);
        CHECK(host.present_calls == 1);
    }
    {
        // The engine recorded no observation (HostRecorded, and this host records
        // none) while three window bars were calculated: the two records disagree,
        // refused before the hook and never accepted as an empty window.
        ReportHost host;
        const pf_selected_window_config_v1 window = window_at(2);
        run_host(host, spec_of("wsr-mismatch", 1, NativeReportPolicy::HostRecorded),
                 flat_rows({0, 1, 2, 3, 4}, 50.0), &window);
        failed_without_results(host, kFaultExecution);
        CHECK(host.present_calls == 0);
        CHECK(host.last_error().find("native selected report") != std::string::npos);
    }
    {
        // A position entered at m0's close fills at m1's open, before T = m3: a
        // carry-in, a failure and not a curve with the early fill filtered out.
        ReportHost host;
        host.enter_from_ms = kT0;
        const pf_selected_window_config_v1 window = window_at(3);
        run_host(host, spec_of("wsr-carry"), tape(5), &window);
        failed_without_results(host, kFaultExecution);
        CHECK(host.present_calls == 0);
    }
}

struct ObserverProbe {
    const BacktestEngine* engine = nullptr;
    int returns = 0;
    int calls = 0;
    int counts_inside = 99;
    int report_inside_empty = 0;
};

int observer_before_results(void* context, const pf_execution_boundary_v1*,
                            pf_boundary_receipt_v1* receipt) {
    ObserverProbe* probe = static_cast<ObserverProbe*>(context);
    ++probe->calls;
    pf_selected_window_counts_v1 counts = blank_counts();
    probe->counts_inside = native_selected_window_counts_v1(probe->engine, &counts);
    receipt->frame_bytes = 0;
    receipt->handed_bytes = 0;
    receipt->export_requested = 0;
    return probe->returns;
}

// The observer runs after the capture and the seal and before the results open: it
// cannot read the counts, and what it returns decides the attempt. A refusal leaves
// the hook run once, the generation Sealed with an after-execution fault, the counts
// unavailable and the report empty; an acceptance opens the counts and the report.
void observer_failure_and_counts_during_observer() {
    std::printf("observer_failure_and_counts_during_observer\n");
    {
        ReportHost host;
        ObserverProbe probe;
        probe.engine = &host;
        pf_execution_observer_v1 observer{};
        observer.struct_size = static_cast<std::uint32_t>(sizeof observer);
        observer.version = 1;
        observer.context = &probe;
        observer.before_results = &observer_before_results;
        const pf_selected_window_config_v1 window = window_at(2);
        run_host(host, spec_of("wsr-observer-ok"), flat_rows({0, 1, 2, 3, 4}, 50.0), &window,
                 &observer);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(probe.calls == 1);
        CHECK(probe.counts_inside == -2);
        CHECK(host.present_calls == 1);
        const pf_execution_observation_v1 seen = observation_of(host);
        CHECK(seen.phase == kPhaseResults);
        CHECK(seen.attempt_outcome == kOutcomeResultsOpen);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == 0);
        CHECK(counts.window_script_bars == 3);
        Report report;
        host.fill_report(&report.raw);
        CHECK(report.raw.equity_curve_len == 4);
        CHECK(host.present_calls == 1);
    }
    {
        ReportHost host;
        ObserverProbe probe;
        probe.engine = &host;
        probe.returns = 7;
        pf_execution_observer_v1 observer{};
        observer.struct_size = static_cast<std::uint32_t>(sizeof observer);
        observer.version = 1;
        observer.context = &probe;
        observer.before_results = &observer_before_results;
        const pf_selected_window_config_v1 window = window_at(2);
        run_host(host, spec_of("wsr-observer-refuse"), flat_rows({0, 1, 2, 3, 4}, 50.0),
                 &window, &observer);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(probe.calls == 1);
        CHECK(host.present_calls == 1);
        const pf_execution_observation_v1 seen = observation_of(host);
        CHECK(seen.phase == kPhaseSealed);
        CHECK(seen.fault_stage == kFaultAfterExecution);
        CHECK(seen.attempt_outcome == kOutcomeFailed);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == -2);
        Report report;
        host.fill_report(&report.raw);
        check_empty_report(report.raw);
        CHECK(host.present_calls == 1);
    }
}

// OFF is the ordinary report, untouched: no anchor, the engine's whole recorded
// curve with the pre-roll points in it, exposure over every point, and buy-and-hold
// from the run's first open (50, the pre-roll price) to its last close -- the same
// whether or not an observer is registered. Over the tape with the buy at m2:
// curve 1000, 1000, 1000, 1010, 1004.5; two of five points in the market is 40 %;
// 104.5 / 50 - 1 = 109 %, and 109 % of 1000 is 1090. A plain OFF read presents on
// every read (the hook is part of the ordinary report); an observer-only OFF
// presents once, in its capture.
void off_and_observer_only_off_are_unchanged() {
    std::printf("off_and_observer_only_off_are_unchanged\n");
    const std::vector<Point> whole = {{kT0, kCapital, 0.0},
                                      {kT0 + 1 * kMinute, kCapital, 0.0},
                                      {kT0 + 2 * kMinute, kCapital, 0.0},
                                      {kT0 + 3 * kMinute, 1010.0, 10.0},
                                      {kT0 + 4 * kMinute, 1004.5, 4.5}};
    const auto check_whole = [&whole](const ReportC& report) {
        check_curve(report, whole);
        const pf_equity_stats_t& e = report.metrics.equity;
        CHECK(near(e.time_in_market_pct, 40.0, 1e-9));
        CHECK(near(e.open_pl, 4.5, 1e-9));
        CHECK(near(e.max_equity_drawdown, 5.5, 1e-9));
        CHECK(near(e.buy_hold_return_pct, 109.0, 1e-9));
        CHECK(near(e.buy_hold_return, 1090.0, 1e-9));
        CHECK(report.input_bars_processed == 5);
        CHECK(report.script_bars_processed == 5);
    };
    {
        ReportHost host;
        host.enter_from_ms = kT0 + 2 * kMinute;
        run_host(host, spec_of("wsr-off"), tape(5), nullptr);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == -2);
        Report first;
        Report second;
        host.fill_report(&first.raw);
        host.fill_report(&second.raw);
        check_whole(first.raw);
        check_whole(second.raw);
        CHECK(host.present_calls == 2);
    }
    {
        ReportHost host;
        host.enter_from_ms = kT0 + 2 * kMinute;
        ObserverProbe probe;
        probe.engine = &host;
        pf_execution_observer_v1 observer{};
        observer.struct_size = static_cast<std::uint32_t>(sizeof observer);
        observer.version = 1;
        observer.context = &probe;
        observer.before_results = &observer_before_results;
        run_host(host, spec_of("wsr-off-observed"), tape(5), nullptr, &observer);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(probe.calls == 1);
        pf_selected_window_counts_v1 counts = blank_counts();
        CHECK(native_selected_window_counts_v1(&host, &counts) == -2);
        Report first;
        Report second;
        host.fill_report(&first.raw);
        host.fill_report(&second.raw);
        check_whole(first.raw);
        check_whole(second.raw);
        CHECK(host.present_calls == 1);
    }
}

}  // namespace

int main() {
    try {
        preroll_and_window_has_no_pre_t_equity();
        anchor_at_t_with_the_first_bar_later();
        no_window_preroll_only_is_the_anchor_alone();
        one_two_three_observations_by_hand();
        repeated_reads_run_one_presentation();
        presented_curve_edit_does_not_move_metrics();
        capture_failures_own_the_attempt();
        observer_failure_and_counts_during_observer();
        off_and_observer_only_off_are_unchanged();
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
