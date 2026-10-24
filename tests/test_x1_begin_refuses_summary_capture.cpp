// X1(2): v1 SUMMARY_ONLY capture without a sink must fail at begin, before bars.
// The selected-window setter reaches begin even with the observer setter guard.
// The legacy equity-only setter preserves FULL trade metrics and pending ratios.
#include <pineforge/execution_observer.h>
#include <pineforge/native_host.hpp>
#include <pineforge/report_mode.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

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

struct Report {
    ReportC value{};
    Report() = default;
    ~Report() { BacktestEngine::free_report(&value); }
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

class Host final : public NativeStrategyHost {
public:
    int begins = 0;
    int calculations = 0;

    void on_native_run_begin() override { ++begins; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = calculations++;
        const int cycle = index / 4;
        if (index % 4 == 0) {
            // Same N=3 fixture as p2-x1: long, short, long with 1, 2, 3 units.
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

void configure(Host& host) {
    NativeRunSpec spec;
    spec.identity = {"x1-begin-summary-capture", 1};
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

void run(Host& host) {
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
}

void completed(const Host& host) {
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "run status=" + std::to_string(host.last_run_status())
                + " code=" + run_failure_code_of(host) + " error=" + host.last_error());
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "fixture did not complete");
    require(host.begins == 1 && host.calculations == bar_count,
            "fixture callback counts: begins=" + std::to_string(host.begins)
                + " bars=" + std::to_string(host.calculations));
    const auto position = host.physical_position();
    require(position.lot_count == 0 && position.signed_units == 0.0,
            "fixture did not finish flat");
}

pf_strategy_t handle(BacktestEngine& host) { return &host; }

void select_window(Host& host) {
    const pf_selected_window_config_v1 window{
        sizeof window, 1, start_ms, start_ms + bar_count * minute_ms};
    const int status = strategy_set_selected_window_v1(handle(host), &window);
    require(status == 0, "window registration status=" + std::to_string(status));
}

struct Observer {
    int calls = 0;
};

int before_results(void* context, const pf_execution_boundary_v1* boundary,
                   pf_boundary_receipt_v1* receipt) {
    ++static_cast<Observer*>(context)->calls;
    if (!boundary || !receipt || boundary->struct_size != sizeof *boundary
        || boundary->version != 1 || boundary->run_generation == 0)
        return 1;
    receipt->frame_bytes = 0;
    receipt->handed_bytes = 0;
    receipt->export_requested = 0;
    receipt->reserved = 0;
    return 0;
}

void attach_observer(Host& host, Observer& observer) {
    const pf_execution_observer_v1 descriptor{
        sizeof descriptor, 1, &observer, before_results};
    const int status = strategy_set_execution_observer_v1(handle(host), &descriptor);
    require(status == 0, "observer registration status=" + std::to_string(status));
}

// A captured FULL run (selected window or observer) is not "complete" in the
// envelope sense: only the uncaptured oracle run is required to be. The captured
// controls assert the mode and, in the caller, the oracle's totals.
void full_report(Host& host, Envelope& envelope, bool require_complete = false) {
    require(host.fill_report_envelope_v1(&envelope.value) == PF_REPORT_MODE_ACCEPTED,
            "FULL envelope refused");
    completed(host);
    require(envelope.value.metadata.mode == PF_REPORT_MODE_FULL,
            "FULL envelope has the wrong mode");
    require(!require_complete || envelope.value.metadata.complete == 1,
            "FULL oracle envelope is incomplete");
}

void oracle_run(Envelope& oracle) {
    Host host;
    configure(host);
    require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
            "oracle FULL setter refused");
    run(host);
    completed(host);
    full_report(host, oracle, true);
    const auto& report = oracle.value.report;
    require(report.total_trades == round_trips && report.trades_len == round_trips
                && report.trades != nullptr && report.metrics.all.num_trades == round_trips
                && report.metrics.longs.num_trades == 2 && report.metrics.shorts.num_trades == 1,
            "FULL oracle must contain N=3 closed trips: two long, one short");
    double net = 0.0;
    for (int index = 0; index < round_trips; ++index) {
        const auto& trade = report.trades[index];
        require(trade.open_at_end == 0 && trade.is_long == (index != 1),
                "FULL oracle row is not the expected closed trip");
        net += trade.pnl;
    }
    require(std::isfinite(net) && net != 0.0 && bits(net) == bits(report.net_profit)
                && bits(net) == bits(report.metrics.all.net_profit),
            "FULL oracle P&L differs from its closed-row fold");
}

void compare_totals(const ReportC& actual, const ReportC& oracle) {
    require(actual.total_trades == round_trips && actual.total_trades == oracle.total_trades
                && actual.trades_len == round_trips && actual.trades != nullptr
                && actual.metrics.all.num_trades == oracle.metrics.all.num_trades
                && actual.metrics.longs.num_trades == oracle.metrics.longs.num_trades
                && actual.metrics.shorts.num_trades == oracle.metrics.shorts.num_trades,
            "FULL capture trade totals differ from the independent FULL oracle");
    require(bits(actual.net_profit) == bits(oracle.net_profit)
                && bits(actual.metrics.all.net_profit) == bits(oracle.metrics.all.net_profit)
                && bits(actual.metrics.longs.net_profit) == bits(oracle.metrics.longs.net_profit)
                && bits(actual.metrics.shorts.net_profit) == bits(oracle.metrics.shorts.net_profit),
            "FULL capture P&L differs from the independent FULL oracle");
}

// The legacy equity-mode path is not the v1 envelope mode: a selected window under it
// keeps its released outcome. The window's equity stream is not fed in SUMMARY_ONLY, so
// the run fails closed in the selected-report view with engine_invariant. The control is
// that the begin check of the v1 mode does NOT touch this path (no report_mode_unsupported).
void legacy_window_fails_closed(Host& host) {
    run(host);
    require(host.last_run_status() == 1 && !host.last_error().empty(),
            "legacy SUMMARY_ONLY selected window did not fail the run");
    require(std::strcmp(run_failure_code_of(host), "engine_invariant") == 0,
            std::string("legacy SUMMARY_ONLY selected window must keep engine_invariant, got code=")
                + run_failure_code_of(host) + " error=" + host.last_error());
}

void refused_at_begin(Host& host) {
    run(host);
    const auto state = host.native_state();
    const auto failure = run_failure_value_of(host);
    std::fprintf(stderr,
        "begin refusal: status=%d code=%s args=%s lifecycle=%u operation=%u begins=%d bars=%d\n",
        host.last_run_status(), run_failure_code_of(host), run_failure_args_of(host),
        static_cast<unsigned>(state.kind), static_cast<unsigned>(state.failure.operation),
        host.begins, host.calculations);
    require(host.last_run_status() == 1 && !host.last_error().empty(),
            "SUMMARY_ONLY capture did not fail the run");
    require(state.kind == NativeLifecycleKind::Failed
                && state.failure.operation == NativeFailureOperation::Begin,
            "SUMMARY_ONLY capture failed after begin");
    require(failure.code == RunFailureCode::report_mode_unsupported
                && std::strcmp(run_failure_code_of(host), "report_mode_unsupported") == 0
                && run_failure_code_class(failure.code) == RunFailureClass::unsupported,
            "begin refusal must carry report_mode_unsupported in the unsupported class");
    constexpr const char* expected_args =
        "{\"contract\":\"pineforge-report/v1\",\"mode\":\"SUMMARY_ONLY\"}";
    require(failure.args && *failure.args == expected_args
                && std::strcmp(run_failure_args_of(host), expected_args) == 0,
            "begin refusal arguments must identify SUMMARY_ONLY and pineforge-report/v1");
    require(host.begins == 0 && host.calculations == 0,
            "SUMMARY_ONLY capture ran a host callback before refusing");
    pf_execution_observation_v1 observation{};
    observation.struct_size = sizeof observation;
    observation.version = 1;
    require(strategy_execution_observation_v1(handle(host), &observation) == 0,
            "refused begin observation query failed");
    require(observation.run_generation == 0 && observation.attempt_generation == 0
                && observation.boundary_delivered == 0,
            "SUMMARY_ONLY capture admitted a generation before refusing");
}

template<class Body>
bool row(const char* name, Body body) {
    try {
        body();
        std::printf("%s: PASS\n", name);
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: FAIL: %s\n", name, error.what());
    } catch (...) {
        std::fprintf(stderr, "%s: FAIL: unknown exception\n", name);
    }
    return false;
}
} // namespace

int main() {
    Envelope oracle;
    // Stop immediately if any success control fails.
    if (!row("FULL oracle control", [&] { oracle_run(oracle); })) return 1;
    if (!row("FULL selected-window control", [&] {
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                "FULL setter refused");
        select_window(host);
        run(host);
        completed(host);
        Envelope result;
        full_report(host, result);
        compare_totals(result.value.report, oracle.value.report);
    })) return 1;
    if (!row("FULL observer control", [&] {
        Observer observer;
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                "FULL setter refused");
        attach_observer(host, observer);
        run(host);
        completed(host);
        require(observer.calls == 1, "FULL capture did not dispatch the observer once");
        Envelope result;
        full_report(host, result);
        compare_totals(result.value.report, oracle.value.report);
    })) return 1;
    if (!row("SUMMARY_ONLY no-capture control", [] {
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "uncaptured SUMMARY_ONLY setter refused");
        run(host);
        completed(host);
    })) return 1;
    if (!row("pending SUMMARY_ONLY no-capture control", [] {
        Host host;
        configure(host);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "uncaptured pending SUMMARY_ONLY setter refused");
        run(host);
        completed(host);
    })) return 1;
    // An observer together with the legacy SUMMARY_ONLY mode is refused by the two
    // setters (p2-x1, L6 12:54), so that combination never reaches begin and has no
    // control here; the legacy selected-window controls below cover the begin check's
    // reach (it must not touch the legacy path).

    bool good = row("SUMMARY_ONLY selected-window begin refusal", [] {
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "SUMMARY_ONLY setter refused before capture was configured");
        // This setter has no report-mode guard, including after p2-x1 is picked.
        select_window(host);
        refused_at_begin(host);
    });
    if (!row("legacy SUMMARY_ONLY selected-window control", [&] {
        Host host;
        configure(host);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "pending SUMMARY_ONLY setter refused");
        select_window(host);
        legacy_window_fails_closed(host);
    })) return 1;
    return good ? 0 : 1;
}
