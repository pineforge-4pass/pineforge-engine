// X1: refuse unsupported capture and never publish inconsistent trade metrics.
// Public APIs only; the independent FULL run supplies every parity expectation.
#include <pineforge/execution_observer.h>
#include <pineforge/native_host.hpp>
#include <pineforge/report_mode.h>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

constexpr int round_trips = 3;
constexpr int bar_count = 4 * round_trips;
constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t minute_ms = 60000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    static_assert(sizeof result == sizeof value, "binary64 report values required");
    std::memcpy(&result, &value, sizeof result);
    return result;
}

struct EquityField {
    const char* name;
    double pf_equity_stats_t::*member;
};

constexpr std::array<EquityField, 15> equity_fields{{
    {"equity.max_equity_drawdown", &pf_equity_stats_t::max_equity_drawdown},
    {"equity.max_equity_drawdown_pct", &pf_equity_stats_t::max_equity_drawdown_pct},
    {"equity.max_equity_runup", &pf_equity_stats_t::max_equity_runup},
    {"equity.max_equity_runup_pct", &pf_equity_stats_t::max_equity_runup_pct},
    {"equity.buy_hold_return", &pf_equity_stats_t::buy_hold_return},
    {"equity.buy_hold_return_pct", &pf_equity_stats_t::buy_hold_return_pct},
    {"equity.sharpe_monthly", &pf_equity_stats_t::sharpe_monthly},
    {"equity.sortino_monthly", &pf_equity_stats_t::sortino_monthly},
    {"equity.sharpe_bar", &pf_equity_stats_t::sharpe_bar},
    {"equity.sortino_bar", &pf_equity_stats_t::sortino_bar},
    {"equity.cagr", &pf_equity_stats_t::cagr},
    {"equity.calmar", &pf_equity_stats_t::calmar},
    {"equity.recovery_factor", &pf_equity_stats_t::recovery_factor},
    {"equity.time_in_market_pct", &pf_equity_stats_t::time_in_market_pct},
    {"equity.open_pl", &pf_equity_stats_t::open_pl},
}};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

struct Envelope {
    ReportEnvelopeV1 value{};
    Envelope() {
        value.metadata.struct_size = sizeof value.metadata;
        value.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    }
    ~Envelope() { BacktestEngine::free_report(&value.report); }
    Envelope(const Envelope&) = delete;
    Envelope& operator=(const Envelope&) = delete;
};

class Host : public NativeStrategyHost {
public:
    int begins = 0;
    int calculations = 0;
    void on_native_run_begin() override {
        ++begins;
        calculations = 0;
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = calculations++;
        const int cycle = index / 4;
        if (index % 4 == 0) {
            // Long, short, long; differing units keep the total P&L nonzero.
            const double units = (cycle == 1 ? -1.0 : 1.0) * (cycle + 1);
            put({no::Transact{units}, "entry", ""});
        }
        if (index % 4 == 2) put({no::Flatten{}, "exit", ""});
    }
private:
    void put(const no::Request& request) {
        const auto result = submit(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle.has_value(),
                "fixture order refused");
    }
};

class CountMismatchHost final : public Host {
public:
    mutable int presentations = 0;
    mutable int logical_trades_before = 0;
protected:
    bool report_presenter_supports_non_full() const noexcept override { return true; }
    void present_report(ReportC* report) const override {
        ++presentations;
        logical_trades_before = report->total_trades;
        // A deliberately inconsistent bounded presenter: no array, ledger,
        // order, hash or private field is changed by this negative fixture.
        ++report->total_trades;
    }
};

std::array<Bar, bar_count> tape() {
    std::array<Bar, bar_count> bars{};
    for (int index = 0; index < bar_count; ++index) {
        const double price = 100.0 + index;
        bars[static_cast<std::size_t>(index)] =
            {price, price + 0.5, price - 0.5, price + 0.25,
             1.0, start_ms + index * minute_ms};
    }
    return bars;
}

void configure(Host& host) {
    NativeRunSpec spec;
    spec.identity = {"x1-observer-summary-only", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:X1";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    const auto result = host.configure_native(spec);
    require(result.status == NativeSetupStatus::Applied,
            "configure refused: field=" + std::to_string(static_cast<int>(result.validation.field))
                + " error=" + std::to_string(static_cast<int>(result.validation.error)));
}

pf_strategy_t handle(BacktestEngine& host) { return &host; }

struct Observer {
    int calls = 0;
    std::uint64_t generation = 0;
};

int before_results(void* context, const pf_execution_boundary_v1* boundary,
                   pf_boundary_receipt_v1* receipt) {
    auto& observer = *static_cast<Observer*>(context);
    ++observer.calls;
    if (!boundary || !receipt || boundary->struct_size != sizeof *boundary
        || boundary->version != 1 || boundary->run_generation == 0)
        return 1;
    observer.generation = boundary->run_generation;
    receipt->frame_bytes = 0;
    receipt->handed_bytes = 0;
    receipt->export_requested = 0;
    receipt->reserved = 0;
    return 0;
}

int set_observer(Host& host, Observer& observer) {
    const pf_execution_observer_v1 descriptor{
        sizeof descriptor, 1, &observer, before_results};
    return strategy_set_execution_observer_v1(handle(host), &descriptor);
}

void attach(Host& host, Observer& observer) {
    const int status = set_observer(host, observer);
    require(status == 0, "observer registration status=" + std::to_string(status));
}

void refuse_observer(const char* row, Host& host, Observer& observer) {
    const int status = set_observer(host, observer);
    std::printf("%s: observer registration status=%d expected=-2\n", row, status);
    require(status == -2, std::string(row) + ": SUMMARY_ONLY registration must return -2");
    require(host.native_state().kind == NativeLifecycleKind::Ready
                && host.last_run_status() == 0 && host.last_error().empty(),
            std::string(row) + ": registration refusal changed the run outcome");
}

void select_window(Host& host) {
    const pf_selected_window_config_v1 window{
        sizeof window, 1, start_ms, start_ms + bar_count * minute_ms};
    const int status = strategy_set_selected_window_v1(handle(host), &window);
    require(status == 0, "window registration status=" + std::to_string(status));
}

void check_completed(Host& host, const Observer* observer = nullptr) {
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "run status=" + std::to_string(host.last_run_status())
                + " code=" + run_failure_code_of(host) + " error=" + host.last_error());
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "fixture did not complete");
    require(host.begins == 1, "begin callback count=" + std::to_string(host.begins));
    require(host.calculations == bar_count,
            "calculation count=" + std::to_string(host.calculations)
                + " expected=" + std::to_string(bar_count));
    const auto position = host.physical_position();
    require(position.lot_count == 0 && position.signed_units == 0.0,
            "fixture did not finish flat: lots=" + std::to_string(position.lot_count)
                + " units=" + std::to_string(position.signed_units));
    if (observer) {
        pf_execution_observation_v1 observation{};
        observation.struct_size = sizeof observation;
        observation.version = 1;
        const int status = strategy_execution_observation_v1(handle(host), &observation);
        require(status == 0 && observer->calls == 1 && observer->generation != 0
                    && observation.run_generation == observer->generation
                    && observation.boundary_delivered == 1 && observation.phase == 4
                    && observation.fault_stage == 0 && observation.attempt_outcome == 4,
                "observer capture missing: query=" + std::to_string(status)
                    + " calls=" + std::to_string(observer->calls)
                    + " callback_generation=" + std::to_string(observer->generation)
                    + " generation=" + std::to_string(observation.run_generation)
                    + " boundary=" + std::to_string(observation.boundary_delivered)
                    + " phase=" + std::to_string(observation.phase)
                    + " fault=" + std::to_string(observation.fault_stage)
                    + " outcome=" + std::to_string(observation.attempt_outcome));
    }
}

void run(Host& host, const Observer* observer = nullptr) {
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    check_completed(host, observer);
}

void print_report(const char* label, const ReportC& report) {
    std::fprintf(stderr,
        "%s: N=%d total=%d trades_len=%d trades=%p all=%d longs=%d shorts=%d "
        "equity_len=%lld equity=%p\n",
        label, round_trips, report.total_trades, report.trades_len,
        static_cast<const void*>(report.trades), report.metrics.all.num_trades,
        report.metrics.longs.num_trades, report.metrics.shorts.num_trades,
        static_cast<long long>(report.equity_curve_len),
        static_cast<const void*>(report.equity_curve));
    const std::array<double, 4> values{{report.net_profit, report.metrics.all.net_profit,
        report.metrics.longs.net_profit, report.metrics.shorts.net_profit}};
    const std::array<const char*, 4> names{{"net_profit", "all.net_profit",
        "longs.net_profit", "shorts.net_profit"}};
    for (std::size_t index = 0; index < values.size(); ++index)
        std::fprintf(stderr, "%s: %s=%.17g bits=0x%016llx\n",
            label, names[index], values[index],
            static_cast<unsigned long long>(bits(values[index])));
    for (const auto& field : equity_fields) {
        const double value = report.metrics.equity.*field.member;
        std::fprintf(stderr, "%s: %s=%.17g bits=0x%016llx\n",
            label, field.name, value, static_cast<unsigned long long>(bits(value)));
    }
}

struct Checks {
    const char* row;
    bool good = true;
    void check(bool condition, const char* message) {
        if (condition) return;
        good = false;
        std::fprintf(stderr, "%s: %s\n", row, message);
    }
    bool finish(const ReportC& actual, const ReportC& oracle,
                const pf_report_metadata_v1* metadata = nullptr,
                const char* oracle_label = "FULL oracle") const {
        if (!good) {
            print_report(row, actual);
            print_report(oracle_label, oracle);
            if (metadata)
                std::fprintf(stderr,
                    "%s: mode=%u complete=%u equity.logical_count=%lld "
                    "equity.captured=%u equity.external=%u resident=%u\n",
                    row, metadata->mode, metadata->complete,
                    static_cast<long long>(metadata->equity_curve.logical_count),
                    metadata->equity_curve.captured, metadata->equity_curve.external,
                    actual.equity_curve_len != 0 ? 1u : 0u);
        }
        return good;
    }
};

void compare(Checks& checks, const ReportC& actual, const ReportC& oracle) {
    checks.check(actual.total_trades == round_trips, "total_trades != N");
    checks.check(actual.total_trades == oracle.total_trades, "total differs from control");
    checks.check(actual.metrics.all.num_trades == actual.total_trades,
                 "metrics.all.num_trades != total_trades");
    checks.check(actual.metrics.all.num_trades == oracle.metrics.all.num_trades,
                 "all-trade count differs from control");
    checks.check(actual.metrics.longs.num_trades == oracle.metrics.longs.num_trades,
                 "long-trade count differs from control");
    checks.check(actual.metrics.shorts.num_trades == oracle.metrics.shorts.num_trades,
                 "short-trade count differs from control");
    checks.check(static_cast<std::int64_t>(actual.metrics.longs.num_trades)
                     + actual.metrics.shorts.num_trades == actual.metrics.all.num_trades,
                 "long + short count differs from all");
    checks.check(bits(actual.net_profit) == bits(oracle.net_profit),
                 "net_profit bits differ from control");
    checks.check(bits(actual.metrics.all.net_profit) == bits(oracle.metrics.all.net_profit),
                 "all.net_profit bits differ from control");
    checks.check(bits(actual.metrics.longs.net_profit) == bits(oracle.metrics.longs.net_profit),
                 "longs.net_profit bits differ from control");
    checks.check(bits(actual.metrics.shorts.net_profit) == bits(oracle.metrics.shorts.net_profit),
                 "shorts.net_profit bits differ from control");
    checks.check(actual.equity_curve_len >= 0
                     && (actual.equity_curve != nullptr) == (actual.equity_curve_len != 0),
                 "equity pointer and resident length disagree");
}

void compare_equity(Checks& checks, const ReportC& actual, const ReportC& oracle) {
    for (const auto& field : equity_fields)
        checks.check(bits(actual.metrics.equity.*field.member)
                         == bits(oracle.metrics.equity.*field.member),
                     field.name);
    checks.check(actual.equity_curve_len == oracle.equity_curve_len,
                 "equity_curve_len differs from the independent control");
}

bool withheld(const ReportC& report) {
    return report.total_trades == 0 && report.net_profit == 0.0
        && report.metrics.all.num_trades == 0 && report.metrics.longs.num_trades == 0
        && report.metrics.shorts.num_trades == 0
        && !report.trades && report.trades_len == 0
        && !report.equity_curve && report.equity_curve_len == 0
        && !report.security_diag && report.security_diag_len == 0
        && !report.trace && report.trace_len == 0
        && !report.trace_names && report.trace_names_len == 0
        && !report.broker_state_hash && report.broker_state_hash_len == 0;
}

bool failed_closed(const char* row, Host& host, const Observer* observer = nullptr) {
    const int run_status = host.last_run_status();
    const auto state = host.native_state();
    const std::string code = run_failure_code_of(host);
    const std::string args = run_failure_args_of(host);
    pf_execution_observation_v1 observation{};
    observation.struct_size = sizeof observation;
    observation.version = 1;
    const int query = strategy_execution_observation_v1(handle(host), &observation);
    Report report;
    host.fill_report(&report.value);
    std::fprintf(stderr,
        "%s: run_status=%d fill_status=%d code=%s args=%s fill_code=%s fill_args=%s "
        "error=%s lifecycle=%u operation=%u "
        "begins=%d calculations=%d observer_calls=%d query=%d "
        "generation=%llu attempt_generation=%llu boundary=%u phase=%u outcome=%u\n",
        row, run_status, host.last_run_status(), code.c_str(), args.c_str(),
        run_failure_code_of(host), run_failure_args_of(host),
        host.last_error().c_str(),
        static_cast<unsigned>(state.kind), static_cast<unsigned>(state.failure.operation),
        host.begins, host.calculations, observer ? observer->calls : 0, query,
        static_cast<unsigned long long>(observation.run_generation),
        static_cast<unsigned long long>(observation.attempt_generation),
        observation.boundary_delivered, observation.phase, observation.attempt_outcome);
    Checks checks{row};
    checks.check(run_status == 1 && host.last_run_status() == 1 && !host.last_error().empty(),
                 "run did not fail closed");
    checks.check(code == "report_mode_unsupported" || code == "engine_invariant",
                 "run failure must carry report_mode_unsupported or engine_invariant");
    checks.check(code == run_failure_code_of(host) && args == run_failure_args_of(host),
                 "plain fill changed the run failure code or arguments");
    checks.check(state.kind == NativeLifecycleKind::Failed,
                 "refusal did not leave a failed lifecycle");
    checks.check(!observer || observer->calls == 0,
                 "unsupported capture dispatched the observer");
    checks.check(query == 0 && observation.boundary_delivered == 0
                     && observation.phase != 4 && observation.attempt_outcome != 4,
                 "failed capture delivered a boundary or opened successful results");
    checks.check(withheld(report.value), "failed capture published a report");
    if (!checks.good) print_report(row, report.value);
    return checks.good;
}

bool plain(const char* row, Host& host, const ReportC& oracle,
           bool whole_equity = false, const char* oracle_label = "FULL oracle") {
    Report result;
    try {
        host.fill_report(&result.value);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: unexpected fill refusal: %s\n", row, error.what());
        print_report(row, result.value);
        print_report(oracle_label, oracle);
        throw;
    }
    Checks checks{row};
    checks.check(host.last_run_status() == 0 && host.last_error().empty(),
                 "plain fill did not preserve successful run status");
    compare(checks, result.value, oracle);
    if (whole_equity) compare_equity(checks, result.value, oracle);
    return checks.finish(result.value, oracle, nullptr, oracle_label);
}

bool envelope(const char* row, Host& host, const ReportC& oracle,
              ReportMode expected_mode) {
    Envelope result;
    int status = PF_REPORT_MODE_INVALID;
    try {
        status = host.fill_report_envelope_v1(&result.value);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: unexpected envelope refusal: %s\n", row, error.what());
        print_report(row, result.value.report);
        print_report("FULL oracle", oracle);
        throw;
    }
    std::printf("%s: envelope status=%d mode=%u expected_mode=%u complete=%u\n",
                row, status, result.value.metadata.mode,
                static_cast<pf_report_mode_v1_t>(expected_mode), result.value.metadata.complete);
    Checks checks{row};
    checks.check(status == PF_REPORT_MODE_ACCEPTED, "envelope was not accepted");
    checks.check(host.last_run_status() == 0 && host.last_error().empty(),
                 "envelope did not preserve successful run status");
    checks.check(result.value.metadata.mode == static_cast<pf_report_mode_v1_t>(expected_mode),
                 "envelope mode differs from expected mode");
    checks.check(result.value.metadata.complete == 1, "completed run has incomplete envelope");
    compare(checks, result.value.report, oracle);
    if (result.value.metadata.complete == 1)
        checks.check(result.value.metadata.equity_curve.captured
                         == (result.value.report.equity_curve_len != 0 ? 1u : 0u),
                     "complete envelope equity capture flag disagrees with resident curve");
    return checks.finish(result.value.report, oracle, &result.value.metadata);
}

struct Suite {
    int failures = 0;
    template<class Row>
    bool row(const char* name, Row body) {
        bool good = false;
        try {
            good = body();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: exception: %s\n", name, error.what());
        } catch (...) {
            std::fprintf(stderr, "%s: unknown exception\n", name);
        }
        if (!good) ++failures;
        std::printf("%s: %s\n", name, good ? "PASS" : "FAIL");
        return good;
    }
};

bool oracle_run(Report& report) {
    Host host;
    configure(host);
    require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
            "oracle FULL refused");
    run(host);
    host.fill_report(&report.value);
    const auto& value = report.value;
    print_report("FULL oracle", value);
    require(host.last_run_status() == 0 && host.last_error().empty(), "oracle fill failed");
    require(value.total_trades == round_trips && value.trades_len == round_trips
                && value.trades != nullptr, "oracle must own exactly N trades");
    int longs = 0;
    int shorts = 0;
    double net = 0.0;
    for (int index = 0; index < value.trades_len; ++index) {
        const auto& trade = value.trades[index];
        std::fprintf(stderr, "FULL oracle trade[%d]: open_at_end=%d is_long=%d pnl=%.17g\n",
                     index, trade.open_at_end, trade.is_long, trade.pnl);
        require(trade.open_at_end == 0, "oracle contains a range-end mark, not a closed trip");
        if (trade.is_long) ++longs;
        else ++shorts;
        net += trade.pnl;
    }
    std::fprintf(stderr, "FULL oracle row fold: longs=%d shorts=%d net=%.17g bits=0x%016llx\n",
                 longs, shorts, net, static_cast<unsigned long long>(bits(net)));
    require(longs > 0 && shorts > 0 && value.metrics.all.num_trades == round_trips
                && value.metrics.longs.num_trades == longs
                && value.metrics.shorts.num_trades == shorts,
            "oracle trade metrics do not describe both sides of the closed fixture");
    require(std::isfinite(net) && net != 0.0 && bits(net) == bits(value.net_profit)
                && bits(net) == bits(value.metrics.all.net_profit), "oracle P&L fold differs");
    require(value.equity_curve && value.equity_curve_len > 0, "oracle has no resident curve");
    return true;
}

bool legacy_summary_run(Report& report, const ReportC& oracle) {
    Host host;
    configure(host);
    require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
            "legacy control: SUMMARY_ONLY setter refused");
    run(host);
    host.fill_report(&report.value);
    Checks checks{"unobserved legacy SUMMARY_ONLY control"};
    checks.check(host.last_run_status() == 0 && host.last_error().empty(),
                 "legacy control fill failed");
    checks.check(host.equity_report_mode() == ReportMode::SUMMARY_ONLY,
                 "legacy control did not adopt SUMMARY_ONLY");
    checks.check(host.equity_logical_point_count() == oracle.equity_curve_len,
                 "legacy control logical equity count differs from FULL");
    checks.check(!report.value.equity_curve && report.value.equity_curve_len == 0,
                 "legacy control retained a resident equity curve");
    compare(checks, report.value, oracle);
    // These first-pass statistics require no ratio replay. The independent
    // FULL host also validates the legacy control before it judges the defect.
    for (const auto member : std::array<double pf_equity_stats_t::*, 6>{{
             &pf_equity_stats_t::max_equity_drawdown,
             &pf_equity_stats_t::max_equity_runup,
             &pf_equity_stats_t::time_in_market_pct,
             &pf_equity_stats_t::cagr,
             &pf_equity_stats_t::calmar,
             &pf_equity_stats_t::recovery_factor}})
        checks.check(bits(report.value.metrics.equity.*member)
                         == bits(oracle.metrics.equity.*member),
                     "legacy control first-pass equity metric differs from FULL");
    return checks.finish(report.value, oracle);
}

// Every sink directory is a mkdtemp directory under TMPDIR (else /tmp), outside the
// source checkout. A fully green run removes exactly the directories it created;
// a failed run keeps them as evidence.
std::vector<std::string>& created_sink_directories() {
    static std::vector<std::string> directories;
    return directories;
}

std::string sink_directory() {
    const auto* configured = std::getenv("TMPDIR");
    const std::filesystem::path parent = configured && *configured ? configured : "/tmp";
    std::string directory = (parent / "pineforge-x1-observer-XXXXXX").string();
    require(::mkdtemp(directory.data()) != nullptr, "cannot create X1 scratch directory");
    created_sink_directories().push_back(directory);
    return directory;
}

void remove_created_sink_directories() {
    for (const auto& directory : created_sink_directories())
        std::filesystem::remove_all(directory);
    created_sink_directories().clear();
}

bool sink_control(const char* row, ReportMode mode, const ReportC& oracle) {
    Observer observer;
    Host host;
    configure(host);
    // FULL must be selected before the sink; STREAMED requires it first.
    if (mode == ReportMode::FULL)
        require(host.set_report_mode_v1(mode) == PF_REPORT_MODE_ACCEPTED,
                std::string(row) + ": FULL setter refused");
    const auto directory = sink_directory();
    const auto path = directory + "/observed.ndjson";
    const pf_report_sink_v1 sink{
        sizeof sink, 1, path.c_str(), "p2cx-x1", directory.c_str()};
    const int status = host.set_report_sink_v1(&sink);
    require(status == 0, std::string(row) + ": path sink status=" + std::to_string(status));
    if (mode == ReportMode::STREAMED)
        require(host.set_report_mode_v1(mode) == PF_REPORT_MODE_ACCEPTED,
                std::string(row) + ": STREAMED setter refused");
    attach(host, observer);
    run(host, &observer);
    const bool parity = plain(row, host, oracle);
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    const int receipt_status = host.report_sink_receipt_v1(&receipt);
    std::printf("%s: receipt status=%d complete=%u count_t=%lld expected=%d sink=%s\n",
                row, receipt_status, receipt.complete, static_cast<long long>(receipt.count_t),
                oracle.total_trades, path.c_str());
    require(receipt_status == 0 && receipt.complete == 1
                && receipt.count_t == oracle.total_trades,
            std::string(row) + ": sink must publish exactly the oracle's closed trades");
    return parity;
}
} // namespace

int main() {
    Suite suite;
    Report oracle;
    // An invalid FULL oracle is a failed control, not evidence about X1.
    if (!suite.row("FULL oracle control", [&] { return oracle_run(oracle); })) return 1;
    Report legacy_control;
    if (!suite.row("legacy SUMMARY_ONLY control", [&] {
        return legacy_summary_run(legacy_control, oracle.value);
    })) return 1;

    if (!suite.row("A", [&] {
        Observer observer;
        Host host;
        configure(host);
        attach(host, observer);
        const int status = host.set_report_mode_v1(ReportMode::SUMMARY_ONLY);
        std::printf("A: setter status=%d expected=%d\n",
                    status, PF_REPORT_MODE_REPLAY_UNAVAILABLE);
        run(host, &observer);
        const bool full = envelope("A", host, oracle.value, ReportMode::FULL);
        if (status != PF_REPORT_MODE_REPLAY_UNAVAILABLE)
            std::fprintf(stderr, "A: observer-first setter must refuse with REPLAY_UNAVAILABLE\n");
        return status == PF_REPORT_MODE_REPLAY_UNAVAILABLE && full;
    })) return 1;

    for (const auto mode : std::array<ReportMode, 2>{{ReportMode::FULL, ReportMode::STREAMED}}) {
        const char* row = mode == ReportMode::FULL ? "D" : "D/STREAMED";
        if (!suite.row(row, [&] { return sink_control(row, mode, oracle.value); })) return 1;
    }

    suite.row("B1", [&] {
        Observer observer;
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "B1: observer-free SUMMARY_ONLY setter refused");
        refuse_observer("B1", host, observer);
        // Prove the refused registration stayed absent before clearing it:
        // a subsequent FULL run must complete without calling that descriptor.
        require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                "B1: restoring FULL refused");
        run(host);
        require(observer.calls == 0, "B1: refused observer was stored");
        const bool parity = plain("B1", host, oracle.value);
        const bool full = envelope("B1", host, oracle.value, ReportMode::FULL);
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "B1: refused registration left capture configuration behind");
        require(strategy_set_execution_observer_v1(handle(host), nullptr) == 0,
                "B1: clearing in SUMMARY_ONLY must be accepted");
        return parity && full;
    });

    suite.row("C/legacy-then-observer", [&] {
        const char* row = "C/legacy-then-observer";
        Observer observer;
        Host host;
        configure(host);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                std::string(row) + ": legacy SUMMARY_ONLY setter refused");
        refuse_observer(row, host, observer);
        run(host);
        require(observer.calls == 0, std::string(row) + ": refused observer was stored");
        return plain(row, host, legacy_control.value, true,
                     "unobserved legacy SUMMARY_ONLY control");
    });

    suite.row("C/observer-then-legacy", [&] {
        const char* row = "C/observer-then-legacy";
        Observer observer;
        Host host;
        configure(host);
        attach(host, observer);
        const int status = host.set_equity_report_mode(ReportMode::SUMMARY_ONLY);
        std::printf("%s: legacy setter status=%d expected=%d\n",
                    row, status, PF_REPORT_MODE_REPLAY_UNAVAILABLE);
        require(status == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                std::string(row) + ": legacy SUMMARY_ONLY must return exactly -5");
        run(host, &observer);
        require(host.equity_report_mode() == ReportMode::FULL,
                std::string(row) + ": refusal changed the pending equity mode");
        return plain(row, host, oracle.value, true);
    });

    suite.row("C/replacement-after-refusal", [&] {
        const char* row = "C/replacement-after-refusal";
        Observer installed;
        Observer replacement;
        Host host;
        configure(host);
        attach(host, installed);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY)
                    == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                std::string(row) + ": legacy SUMMARY_ONLY must return exactly -5");
        attach(host, replacement);
        run(host, &replacement);
        require(installed.calls == 0, std::string(row) + ": replaced observer was called");
        require(replacement.calls == 1,
                std::string(row) + ": replacement observer was not called once");
        require(host.equity_report_mode() == ReportMode::FULL,
                std::string(row) + ": refusal changed the pending equity mode");
        return plain(row, host, oracle.value, true);
    });

    suite.row("C/legacy-after-clear", [&] {
        const char* row = "C/legacy-after-clear";
        Observer observer;
        Host host;
        configure(host);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                std::string(row) + ": unobserved legacy setter refused");
        require(strategy_set_execution_observer_v1(handle(host), nullptr) == 0,
                std::string(row) + ": clearing with pending legacy SUMMARY_ONLY refused");
        require(host.set_equity_report_mode(ReportMode::FULL) == 0,
                std::string(row) + ": restoring pending FULL refused");
        attach(host, observer);
        require(strategy_set_execution_observer_v1(handle(host), nullptr) == 0,
                std::string(row) + ": installed observer clear refused");
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                std::string(row) + ": legacy setter refused after observer clear");
        require(strategy_set_execution_observer_v1(handle(host), nullptr) == 0,
                std::string(row) + ": repeated clear with pending legacy SUMMARY_ONLY refused");
        run(host);
        require(observer.calls == 0, std::string(row) + ": cleared observer was called");
        return plain(row, host, legacy_control.value, true,
                     "unobserved legacy SUMMARY_ONLY control");
    });

    suite.row("C/legacy-pending-fullswitch", [&] {
        const char* row = "C/legacy-pending-fullswitch";
        Observer observer;
        Host host;
        configure(host);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                std::string(row) + ": legacy SUMMARY_ONLY setter refused");
        run(host);
        require(host.equity_report_mode() == ReportMode::SUMMARY_ONLY,
                std::string(row) + ": fixture did not leave active SUMMARY_ONLY");
        // Pending SUMMARY_ONLY still refuses after completion. Switching the
        // pending mode to FULL must accept despite that leftover active mode.
        const int refused = set_observer(host, observer);
        std::printf("%s: pending SUMMARY_ONLY observer status=%d expected=-2\n", row, refused);
        require(refused == -2, std::string(row) + ": pending legacy mode must return -2");
        require(host.set_equity_report_mode(ReportMode::FULL) == 0,
                std::string(row) + ": pending FULL setter refused");
        attach(host, observer);
        require(host.equity_report_mode() == ReportMode::SUMMARY_ONLY
                    && host.begins == 1 && observer.calls == 0,
                std::string(row) + ": registration changed the completed run");
        return true;
    });

    suite.row("C/legacy-observer-equity-block", [&] {
        const char* row = "C/legacy-observer-equity-block";
        Observer observer;
        Host host;
        configure(host);
        attach(host, observer);
        const int status = host.set_equity_report_mode(ReportMode::SUMMARY_ONLY);
        std::printf("%s: legacy setter status=%d expected=%d\n",
                    row, status, PF_REPORT_MODE_REPLAY_UNAVAILABLE);
        if (status != 0) {
            require(status == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                    std::string(row) + ": refusal must be exactly -5");
            require(host.begins == 0 && host.calculations == 0 && observer.calls == 0
                        && host.last_run_status() == 0 && host.last_error().empty(),
                    std::string(row) + ": refusal executed or failed the run");
            return true;
        }
        // Old behaviour (or m2): reach the defective plain report and compare
        // every equity field to a separate unobserved legacy-summary host.
        // finish prints every value and its bits, including all NaN ratios.
        run(host, &observer);
        Report report;
        host.fill_report(&report.value);
        Checks checks{row};
        checks.check(host.last_run_status() == 0 && host.last_error().empty(),
                     "legacy observed plain fill failed");
        compare(checks, report.value, legacy_control.value);
        compare_equity(checks, report.value, legacy_control.value);
        return checks.finish(report.value, legacy_control.value, nullptr,
                             "unobserved legacy SUMMARY_ONLY control");
    });

    // The public window setter bypasses observer-registration admission.
    // B2 is N5's no-run control: a pending window configures the NEXT run,
    // so the envelope read is ACCEPTED, incomplete, and publishes nothing.
    // B3 independently requires the capture backstop after execution.
    if (!suite.row("B2", [&] {
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "B2: SUMMARY_ONLY setter refused");
        select_window(host);
        Envelope result;
        const int status = host.fill_report_envelope_v1(&result.value);
        std::printf("B2: envelope status=%d complete=%u\n",
                    status, result.value.metadata.complete);
        require(status == PF_REPORT_MODE_ACCEPTED
                    && result.value.metadata.complete == 0 && withheld(result.value.report),
                "B2: a never-run window must be ACCEPTED, complete == 0, report withheld");
        require(host.begins == 0 && host.calculations == 0
                    && host.last_run_status() == 0 && host.last_error().empty(),
                "B2: envelope read executed or failed the run");
        return true;
    })) return 1;

    suite.row("B3", [&] {
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "B3: SUMMARY_ONLY setter refused");
        select_window(host);
        const auto bars = tape();
        host.run(bars.data(), static_cast<int>(bars.size()));
        // Without the X1(2) begin check the capture fails at the trade metrics
        // backstop; with it the run is refused at begin before any callback.
        // Either is the fail-closed shape; each is pinned exactly.
        const bool backstop = std::strcmp(run_failure_code_of(host), "engine_invariant") == 0
            && std::strcmp(run_failure_args_of(host), "{}") == 0
            && host.last_error() == "summary trade metrics differ from the logical trade count";
        const bool begin_refusal =
            std::strcmp(run_failure_code_of(host), "report_mode_unsupported") == 0
            && std::strcmp(run_failure_args_of(host),
                           "{\"contract\":\"pineforge-report/v1\",\"mode\":\"SUMMARY_ONLY\"}") == 0
            && host.begins == 0 && host.calculations == 0;
        require(backstop || begin_refusal,
                "B3: capture failed neither at the exact trade metrics backstop nor at the begin refusal: status="
                    + std::to_string(host.last_run_status())
                    + " code=" + run_failure_code_of(host)
                    + " args=" + run_failure_args_of(host) + " error=" + host.last_error());
        return failed_closed("B3", host);
    });

    suite.row("E", [&] {
        Observer observer;
        CountMismatchHost host;
        configure(host);
        const auto directory = sink_directory();
        const auto path = directory + "/inconsistent-presenter.ndjson";
        const pf_report_sink_v1 sink{
            sizeof sink, 1, path.c_str(), "p2cx-x1", directory.c_str()};
        require(host.set_report_sink_v1(&sink) == 0, "E: path sink setter refused");
        require(host.set_report_mode_v1(ReportMode::STREAMED) == PF_REPORT_MODE_ACCEPTED,
                "E: STREAMED setter refused");
        attach(host, observer);
        const auto bars = tape();
        host.run(bars.data(), static_cast<int>(bars.size()));
        const char* code = run_failure_code_of(host);
        const char* args = run_failure_args_of(host);
        std::fprintf(stderr,
            "E: status=%d code=%s args=%s error=%s begins=%d calculations=%d "
            "presentations=%d original_total=%d oracle_total=%d observer_calls=%d\n",
            host.last_run_status(), code, args, host.last_error().c_str(), host.begins,
            host.calculations, host.presentations, host.logical_trades_before,
            oracle.value.total_trades, observer.calls);
        Checks checks{"E"};
        checks.check(host.begins == 1 && host.calculations == bar_count
                         && host.presentations == 1
                         && host.logical_trades_before == oracle.value.total_trades,
                     "fixture did not reach capture after the oracle's closed trips");
        // Judge run(), before fill_report can detect a later sink count error.
        checks.check(host.last_run_status() == 1
                         && host.native_state().kind == NativeLifecycleKind::Failed,
                     "inconsistent capture succeeded");
        checks.check(std::strcmp(code, "engine_invariant") == 0
                         && std::strcmp(args, "{}") == 0,
                     "capture did not carry the exact invariant code and empty arguments");
        checks.check(host.last_error() == "summary trade metrics differ from the logical trade count",
                     "capture failed outside the metrics backstop");
        checks.check(observer.calls == 0, "inconsistent capture reached the observer boundary");
        return checks.good;
    });

    if (!suite.row("C/observer-first", [&] {
        Observer observer;
        Host host;
        configure(host);
        attach(host, observer);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY)
                    == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                "C/observer-first: legacy SUMMARY_ONLY must return exactly -5");
        run(host, &observer);
        Report report;
        host.fill_report(&report.value);
        Envelope result;
        const int status = host.fill_report_envelope_v1(&result.value);
        std::fprintf(stderr,
            "C/observer-first: run_status=%d code=%s args=%s error=%s observer_calls=%d "
            "equity_mode=%u "
            "envelope_status=%d mode=%u complete=%u logical_trades=%lld oracle_total=%d\n",
            host.last_run_status(), run_failure_code_of(host), run_failure_args_of(host),
            host.last_error().c_str(), observer.calls,
            static_cast<unsigned>(host.equity_report_mode()),
            status, result.value.metadata.mode, result.value.metadata.complete,
            static_cast<long long>(result.value.metadata.trades.logical_count),
            oracle.value.total_trades);
        Checks checks{"C/observer-first"};
        checks.check(host.last_run_status() == 0 && host.last_error().empty(),
                     "report reads did not preserve successful run status");
        checks.check(host.equity_report_mode() == ReportMode::FULL,
                     "refused legacy setter changed the pending equity mode");
        checks.check(status == PF_REPORT_MODE_ACCEPTED,
                     "FULL envelope was not accepted");
        checks.check(result.value.metadata.mode == PF_REPORT_MODE_FULL,
                     "legacy equity setter changed the versioned report mode");
        checks.check(result.value.metadata.complete == 1,
                     "completed FULL run has an incomplete envelope");
        checks.check(result.value.metadata.trades.logical_count == oracle.value.total_trades,
                     "envelope logical trade count differs from FULL");
        // Each read independently matches the entire FULL equity block.
        compare(checks, report.value, oracle.value);
        compare_equity(checks, report.value, oracle.value);
        compare(checks, result.value.report, oracle.value);
        compare_equity(checks, result.value.report, oracle.value);
        checks.check(report.value.trades_len == result.value.report.trades_len
                         && report.value.equity_curve_len == result.value.report.equity_curve_len,
                     "plain and envelope resident lengths disagree");
        checks.check(result.value.metadata.equity_curve.captured
                         == (result.value.report.equity_curve_len != 0 ? 1u : 0u),
                     "FULL envelope equity capture flag disagrees with resident curve");
        const bool plain_good = checks.finish(report.value, oracle.value);
        const bool envelope_good = checks.finish(
            result.value.report, oracle.value, &result.value.metadata);
        return plain_good && envelope_good;
    })) return 1;

    if (!suite.row("C/clear", [&] {
        Observer observer;
        Host host;
        configure(host);
        attach(host, observer);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY)
                    == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                "C/clear: legacy SUMMARY_ONLY must return exactly -5");
        const int status = strategy_set_execution_observer_v1(handle(host), nullptr);
        require(status == 0, "C/clear: SUMMARY_ONLY observer clear status=" + std::to_string(status));
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                "C/clear: legacy SUMMARY_ONLY setter refused after clear");
        run(host);
        require(observer.calls == 0, "C/clear: removed observer was called");
        require(host.equity_report_mode() == ReportMode::SUMMARY_ONLY,
                "C/clear: observer removal changed the pending equity mode");
        return plain("C/clear", host, legacy_control.value, true,
                     "unobserved legacy SUMMARY_ONLY control");
    })) return 1;

    std::printf("test_report_mode_v1_summary_with_observer: %d failed rows\n", suite.failures);
    if (suite.failures == 0) remove_created_sink_directories();
    return suite.failures == 0 ? 0 : 1;
}
