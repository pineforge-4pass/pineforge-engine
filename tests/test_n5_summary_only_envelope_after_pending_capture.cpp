// N5: pending capture for the next run must not hide an ordinary completed report.
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

bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

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

class Host final : public NativeStrategyHost {
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
            // Long, short, long; quantities 1, 2, 3 make the net nonzero.
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
    spec.identity = {"n5-pending-capture", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:N5";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    require(host.configure_native(spec).status == NativeSetupStatus::Applied,
            "native configure refused");
}

void run(Host& host) {
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "fixture run failed: " + host.last_error());
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "fixture run did not complete");
    require(host.begins == 1 && host.calculations == bar_count,
            "fixture did not execute exactly one complete run");
    const auto position = host.physical_position();
    require(position.lot_count == 0 && position.signed_units == 0.0,
            "fixture did not close all three round trips");
}

void select_window(Host& host) {
    const pf_selected_window_config_v1 window{
        sizeof window, 1, start_ms, start_ms + bar_count * minute_ms};
    require(strategy_set_selected_window_v1(&host, &window) == 0,
            "pending selected window refused");
}

struct Observer {
    int calls = 0;
    std::uint64_t generation = 0;
};

int before_results(void* context, const pf_execution_boundary_v1* boundary,
                   pf_boundary_receipt_v1* receipt) {
    if (!boundary || !receipt || boundary->struct_size != sizeof *boundary
        || boundary->version != 1 || boundary->run_generation == 0
        || receipt->struct_size != sizeof *receipt || receipt->version != 1
        || receipt->run_generation != boundary->run_generation)
        return 1;
    auto& observer = *static_cast<Observer*>(context);
    ++observer.calls;
    observer.generation = boundary->run_generation;
    receipt->frame_bytes = 0;
    receipt->handed_bytes = 0;
    receipt->export_requested = 0;
    receipt->reserved = 0;
    return 0;
}

void compare_report(const ReportC& actual, const ReportC& oracle) {
    require(actual.total_trades == oracle.total_trades,
            "report total differs from the FULL oracle");
    require(actual.metrics.all.num_trades == oracle.metrics.all.num_trades
                && actual.metrics.longs.num_trades == oracle.metrics.longs.num_trades
                && actual.metrics.shorts.num_trades == oracle.metrics.shorts.num_trades,
            "report trade metric counts differ from the FULL oracle");
    require(same_bits(actual.net_profit, oracle.net_profit)
                && same_bits(actual.metrics.all.net_profit, oracle.metrics.all.net_profit)
                && same_bits(actual.metrics.longs.net_profit, oracle.metrics.longs.net_profit)
                && same_bits(actual.metrics.shorts.net_profit, oracle.metrics.shorts.net_profit),
            "report net-profit bits differ from the FULL oracle");
}

void check_envelope(const char* row, Host& host, const ReportC& oracle, ReportMode mode) {
    Envelope result;
    const int status = host.fill_report_envelope_v1(&result.value);
    std::printf("%s: envelope status=%d expected=%d complete=%u\n",
                row, status, PF_REPORT_MODE_ACCEPTED, result.value.metadata.complete);
    require(status == PF_REPORT_MODE_ACCEPTED,
            std::string(row) + ": completed envelope refused");
    require(result.value.metadata.complete == 1
                && result.value.metadata.mode == static_cast<pf_report_mode_v1_t>(mode),
            std::string(row) + ": envelope mode or completion changed");
    require(result.value.metadata.trades.logical_count == oracle.total_trades,
            std::string(row) + ": envelope lost logical trades");
    compare_report(result.value.report, oracle);
    if (mode == ReportMode::SUMMARY_ONLY) {
        require(result.value.report.trades == nullptr && result.value.report.trades_len == 0
                    && result.value.report.equity_curve == nullptr
                    && result.value.report.equity_curve_len == 0,
                std::string(row) + ": SUMMARY_ONLY owns omitted arrays");
    } else {
        require(result.value.report.trades != nullptr
                    && result.value.report.trades_len == oracle.trades_len,
                std::string(row) + ": FULL lost its owning trades");
    }
    require(host.last_run_status() == 0 && host.last_error().empty()
                && host.native_state().kind == NativeLifecycleKind::Completed,
            std::string(row) + ": report read changed the completed run outcome");
}

void full_oracle(Report& oracle) {
    Host host;
    configure(host);
    require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
            "FULL oracle setter refused");
    run(host);
    host.fill_report(&oracle.value);
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "FULL oracle read failed");
    const auto& report = oracle.value;
    require(report.total_trades == round_trips && report.trades_len == round_trips
                && report.trades != nullptr && report.metrics.all.num_trades == round_trips
                && report.metrics.longs.num_trades == 2 && report.metrics.shorts.num_trades == 1,
            "FULL oracle lacks three closed trips on both sides");
    double net = 0.0;
    for (int index = 0; index < report.trades_len; ++index) {
        require(report.trades[index].open_at_end == 0,
                "FULL oracle contains a range-end mark instead of a closed trip");
        net += report.trades[index].pnl;
    }
    require(std::isfinite(net) && net != 0.0 && same_bits(net, report.net_profit)
                && same_bits(net, report.metrics.all.net_profit),
            "FULL oracle net-profit bits differ from its closed-row fold");
    std::printf("N5/FULL oracle: PASS\n");
}

void pre_run_capture_control(const ReportC& oracle) {
    Observer observer;
    Host host;
    configure(host);
    const pf_execution_observer_v1 descriptor{
        sizeof descriptor, 1, &observer, before_results};
    require(strategy_set_execution_observer_v1(&host, &descriptor) == 0,
            "row 2: pending execution observer refused");
    require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
            "row 2: next-run capture did not refuse SUMMARY_ONLY with -5");
    run(host);
    pf_execution_observation_v1 observation{};
    observation.struct_size = sizeof observation;
    observation.version = 1;
    require(strategy_execution_observation_v1(&host, &observation) == 0
                && observer.calls == 1 && observer.generation != 0
                && observation.run_generation == observer.generation
                && observation.phase == 4 && observation.fault_stage == 0
                && observation.attempt_outcome == 4 && observation.boundary_delivered == 1,
            "row 2: ordinary observer capture did not open its completed results");
    check_envelope("N5/row2 pre-run observer", host, oracle, ReportMode::FULL);
    require(strategy_set_execution_observer_v1(&host, nullptr) == 0,
            "row 2: clearing pending observer refused");
    require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
            "row 2: clearing pending configuration erased admitted capture");
    std::printf("N5/row2 pre-run observer: PASS\n");
}

void full_pending_control(const ReportC& oracle) {
    Host host;
    configure(host);
    require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
            "row 3: FULL setter refused");
    run(host);
    select_window(host);
    check_envelope("N5/row3 FULL pending window", host, oracle, ReportMode::FULL);
    std::printf("N5/row3 FULL pending window: PASS\n");
}

void summary_admitted_window_control() {
    class ZeroTradeHost final : public NativeStrategyHost {
    public:
        int begins = 0;
        int calculations = 0;

        void on_native_run_begin() override {
            ++begins;
            calculations = 0;
        }

        void on_native_bar(const Bar&, const NativeDecisionContext&) override {
            // Consume the same tape without submitting any entry or exit.
            ++calculations;
        }
    };

    ZeroTradeHost host;
    NativeRunSpec spec;
    spec.identity = {"n5-admitted-window", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:N5";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    require(host.configure_native(spec).status == NativeSetupStatus::Applied,
            "row 4: native configure refused");
    require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
            "row 4: ordinary SUMMARY_ONLY setter refused");
    const pf_selected_window_config_v1 window{
        sizeof window, 1, start_ms, start_ms + bar_count * minute_ms};
    require(strategy_set_selected_window_v1(&host, &window) == 0,
            "row 4: pending selected window refused");

    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    if (host.last_run_status() != 0) {
        // Once the X1(2) begin check is picked, a sinkless SUMMARY_ONLY capture is refused
        // before any bar, so no generation is ever admitted and the admitted half of the
        // predicate has nothing left to guard. Accept exactly that typed refusal, never a
        // different failure, and prove nothing was published.
        require(std::strcmp(run_failure_code_of(host), "report_mode_unsupported") == 0
                    && host.begins == 0 && host.calculations == 0,
                "row 4: zero-trade selected run failed other than by the begin refusal: "
                    + host.last_error());
        Envelope refused;
        require(host.fill_report_envelope_v1(&refused.value) != PF_REPORT_MODE_ACCEPTED
                    || refused.value.metadata.complete == 0,
                "row 4: refused begin was certified complete");
        std::printf("N5/row4 SUMMARY_ONLY admitted window: begin refusal, PASS\n");
        return;
    }
    require(host.last_error().empty()
                && host.native_state().kind == NativeLifecycleKind::Completed,
            "row 4: zero-trade selected run did not complete: " + host.last_error());
    require(host.begins == 1 && host.calculations == bar_count,
            "row 4: zero-trade fixture did not execute the complete tape");
    const auto position = host.physical_position();
    require(position.lot_count == 0 && position.signed_units == 0.0,
            "row 4: zero-trade fixture opened a position");
    Report zero;
    host.fill_report(&zero.value);
    require(zero.value.total_trades == 0 && zero.value.metrics.all.num_trades == 0
                && zero.value.metrics.longs.num_trades == 0
                && zero.value.metrics.shorts.num_trades == 0,
            "row 4: selected fixture did not retain zero trade counts");
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = sizeof counts;
    counts.version = 1;
    require(strategy_selected_window_counts_v1(&host, &counts) == 0
                && counts.run_generation != 0
                && counts.fed_input_bars == bar_count && counts.fed_script_bars == bar_count
                && counts.window_input_bars == bar_count && counts.window_script_bars == bar_count
                && counts.preroll_input_bars == 0 && counts.preroll_script_bars == 0,
            "row 4: selected window was not admitted with completed results");
    require(strategy_set_selected_window_v1(&host, nullptr) == 0,
            "row 4: clearing pending selected window refused");

    for (int read = 0; read < 2; ++read) {
        Envelope result;
        const int status = host.fill_report_envelope_v1(&result.value);
        std::printf("N5/row4 SUMMARY_ONLY admitted window: read=%d status=%d expected=%d complete=%u\n",
                    read + 1, status, PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                    result.value.metadata.complete);
        require(status == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                "row 4: clearing pending window erased admitted SUMMARY_ONLY refusal (-5)");
        const auto& report = result.value.report;
        require(result.value.metadata.complete == 0
                    && report.trades == nullptr && report.trades_len == 0
                    && report.security_diag == nullptr && report.security_diag_len == 0
                    && report.trace == nullptr && report.trace_len == 0
                    && report.trace_names == nullptr && report.trace_names_len == 0
                    && report.equity_curve == nullptr && report.equity_curve_len == 0
                    && report.broker_state_hash == nullptr && report.broker_state_hash_len == 0,
                "row 4: refused envelope completed or exposed report arrays");
        require(host.last_run_status() == 0 && host.last_error().empty()
                    && host.native_state().kind == NativeLifecycleKind::Completed
                    && host.begins == 1 && host.calculations == bar_count,
                "row 4: envelope read changed or re-executed the completed run");
    }
    std::printf("N5/row4 SUMMARY_ONLY admitted window: PASS\n");
}

void summary_after_pending_capture(const ReportC& oracle) {
    Host host;
    configure(host);
    require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
            "row 1: ordinary SUMMARY_ONLY setter refused");
    run(host);
    check_envelope("N5/row1 before pending window", host, oracle, ReportMode::SUMMARY_ONLY);
    // This setter changes only the NEXT generation. No second run begins.
    select_window(host);
    require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
            "row 1: next-run capture must still refuse SUMMARY_ONLY admission");
    for (int read = 0; read < 2; ++read)
        check_envelope("N5/row1 SUMMARY_ONLY pending window",
                       host, oracle, ReportMode::SUMMARY_ONLY);
    require(host.begins == 1 && host.calculations == bar_count,
            "row 1: pending configuration or report read executed another run");
    std::printf("N5/row1 SUMMARY_ONLY pending window: PASS\n");
}
}  // namespace

int main() {
    try {
        Report oracle;
        full_oracle(oracle);
        // A failed control stops before the red row.
        pre_run_capture_control(oracle.value);
        full_pending_control(oracle.value);
        summary_admitted_window_control();
        summary_after_pending_capture(oracle.value);
        std::printf("test_n5_summary_only_envelope_after_pending_capture: PASS\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_n5_summary_only_envelope_after_pending_capture: FAIL: %s\n",
                     error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "test_n5_summary_only_envelope_after_pending_capture: FAIL: unknown exception\n");
        return 1;
    }
}
