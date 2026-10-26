// The C++ fd consumer's native run/fill_report failure boundary.
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>

#include "fd_flag_diagnostics.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr std::int64_t start = INT64_C(1704067200000);
constexpr int bar_count = 4;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_empty_report(const pineforge::ReportC& report) {
    require(report.trades == nullptr && report.trades_len == 0 && report.total_trades == 0
            && report.net_profit == 0, "faulted report published trades or profit");
    require(report.security_diag == nullptr && report.security_diag_len == 0
            && report.trace == nullptr && report.trace_len == 0
            && report.trace_names == nullptr && report.trace_names_len == 0
            && report.equity_curve == nullptr && report.equity_curve_len == 0
            && report.broker_state_hash == nullptr && report.broker_state_hash_len == 0,
            "faulted report published an array");
    require(report.input_bars_processed == 0 && report.script_bars_processed == 0
            && report.security_feeds_total == 0 && report.security_eval_complete_total == 0
            && report.security_eval_partial_total == 0 && report.magnifier_sub_bars_total == 0
            && report.magnifier_sample_ticks_total == 0 && report.input_tf_seconds == 0
            && report.script_tf_seconds == 0 && report.script_tf_ratio == 0
            && report.needs_aggregation == 0 && report.bar_magnifier_enabled == 0,
            "faulted report published diagnostics");
    const pf_trade_stats_t* blocks[]{&report.metrics.all, &report.metrics.longs,
                                    &report.metrics.shorts};
    for (const auto* stats : blocks) {
        require(stats->num_trades == 0 && stats->num_wins == 0 && stats->num_losses == 0
                && stats->num_even == 0 && stats->max_consecutive_wins == 0
                && stats->max_consecutive_losses == 0, "faulted report published trade counts");
        const double values[]{
            stats->percent_profitable, stats->net_profit, stats->net_profit_pct,
            stats->gross_profit, stats->gross_profit_pct, stats->gross_loss, stats->gross_loss_pct,
            stats->profit_factor, stats->avg_trade, stats->avg_trade_pct, stats->avg_win,
            stats->avg_win_pct, stats->avg_loss, stats->avg_loss_pct, stats->ratio_avg_win_avg_loss,
            stats->largest_win, stats->largest_win_pct, stats->largest_loss, stats->largest_loss_pct,
            stats->commission_paid, stats->expectancy, stats->avg_bars_in_trade,
            stats->avg_bars_in_wins, stats->avg_bars_in_losses};
        for (double number : values)
            require(number == 0 && !std::signbit(number), "faulted report published trade metrics");
    }
    const auto& equity = report.metrics.equity;
    const double values[]{
        equity.max_equity_drawdown, equity.max_equity_drawdown_pct,
        equity.max_equity_runup, equity.max_equity_runup_pct,
        equity.buy_hold_return, equity.buy_hold_return_pct,
        equity.sharpe_monthly, equity.sortino_monthly, equity.sharpe_bar, equity.sortino_bar,
        equity.cagr, equity.calmar, equity.recovery_factor, equity.time_in_market_pct, equity.open_pl};
    for (double number : values)
        require(number == 0 && !std::signbit(number), "faulted report published equity metrics");
}

struct Pipe {
    int descriptors[2]{-1, -1};
    Pipe() {
        require(::pipe(descriptors) == 0, "pipe failed");
        const int flags = ::fcntl(descriptors[0], F_GETFL);
        require(flags >= 0 && ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0,
                "nonblocking reader setup failed");
    }
    ~Pipe() {
        for (const int fd : descriptors) if (fd >= 0) ::close(fd);
    }
    std::string drain() const {
        std::string bytes;
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return bytes;
            require(count >= 0, "pipe read failed");
            if (count == 0) return bytes;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
};

class Fixture final : public pineforge::NativeStrategyHost {
public:
    Fixture(int field, double value) : field_(field), value_(value) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"CR-289-cpp-fd", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:CR289";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "native fixture configuration refused");
    }
    pf_strategy_t handle() { return static_cast<pineforge::BacktestEngine*>(this); }
    int callbacks = 0;
    bool injected = false;
    pf_equity_point_t injected_point{};
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (++callbacks != 2) return;
        auto* point = last_equity_point();
        require(point && point->time_ms == start && std::isfinite(point->equity)
                && std::isfinite(point->open_profit), "held native point is missing/invalid");
        // Inject only the held report value. The normal kernel recorder
        // releases it next, before computing/storing this bar's successor.
        if (field_ == 0) point->equity = value_;
        else point->open_profit = value_;
        injected_point = *point;
        injected = true;
    }
private:
    int field_;
    double value_;
};

void fd_run(int field, double value, bool refuses) {
    // Mutation: remove this field's finite_value() call in write_e. A later
    // metric check may still fail, but cannot keep count_e at zero or stop the
    // real engine at its second callback, so each poisoned row turns red.
    Pipe pipe;
    Fixture host(field, value);
    const pf_report_sink_v1 replaced{sizeof replaced, 1,
        "CR-289-replaced-cpp-sink-must-not-open.ndjson", "CR-289", nullptr};
    require(strategy_set_report_sink_v1(host.handle(), &replaced) == 0, "path setup refused");
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.descriptors[1], "CR-289"};
    require(strategy_set_report_sink_fd_v1(host.handle(), &sink) == 0, "fd replacement refused");
    fd_flag_diagnostics::Trace fd_trace(pipe.descriptors[1], "after sink installation");
    const int flags = fd_trace.baseline().flags;
    const int fd_flags = fd_trace.baseline().descriptor_flags;
    std::array<pineforge::Bar, bar_count> bars{};
    for (int i = 0; i < bar_count; ++i)
        bars[static_cast<std::size_t>(i)] =
            {100.0, 100.0, 100.0, 100.0, 1.0, start + i * INT64_C(60000)};
    struct Report {
        pineforge::ReportC value{};
        ~Report() { pineforge::BacktestEngine::free_report(&value); }
    } report;
    bool threw = false;
    int run_status = -1;
    std::string run_error;
    pineforge::RunFailureValue carried;
    const char* exception_boundary = "after run (exception)";
    try {
        host.run(bars.data(), bar_count);
        fd_trace.record("after run");
        run_status = host.last_run_status();
        run_error = host.last_error();
        carried = pineforge::run_failure_value_of(host);
        exception_boundary = "after fill_report (exception)";
        host.fill_report(&report.value);
        fd_trace.record("after fill_report");
    } catch (const std::exception& error) {
        fd_trace.record(exception_boundary);
        threw = true;
        carried = pineforge::classify_run_failure(error);
        // Same boundary as test_report_sink_fd.cpp's execute()/fd_run().
        pineforge::note_run_failure(host, "C++ fd_run", error);
        fd_trace.record("after note_run_failure");
    }
    require(host.injected, "native held point was never injected");
    const double actual = field == 0 ? host.injected_point.equity : host.injected_point.open_profit;
    require(std::isnan(value) ? std::isnan(actual) : actual == value, "native poison changed");
    require(field == 0 ? host.injected_point.open_profit == 0.0
                       : host.injected_point.equity == 10000.0, "companion is not finite");
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    require(strategy_report_sink_receipt_v1(host.handle(), &receipt) == 0, "receipt query failed");
    fd_trace.record("before draining reader");
    const auto bytes = pipe.drain();
    fd_trace.record("after draining reader");
    if (refuses) {
        // run() already retained the execution fault. Report capture must
        // return empty without throwing or rereading the poisoned point.
        require(!threw && run_status == 1 && host.last_run_status() == 1,
                "C++ run/capture changed the execution failure");
        require_empty_report(report.value);
        require(!run_error.empty() && host.last_error() == run_error,
                "report capture changed the original error");
        require(host.callbacks == 2 && receipt.count_e == 0, "writer did not refuse the first E");
        const std::string expected = std::string("{\"field\":\"equity_curve.")
            + (field == 0 ? "equity" : "open_profit") + "\",\"reason\":\"nonfinite\"}";
        require(carried.code == pineforge::RunFailureCode::report_result_invalid
                && carried.args && *carried.args == expected, "capture changed typed failure");
        const char* code = strategy_get_last_error_code(host.handle());
        const char* args = strategy_get_last_error_args(host.handle());
        require(code && std::string(code) == "report_result_invalid"
                && args && std::string(args) == expected, "wrapper changed code/field/reason");
        require(receipt.complete == 0 && receipt.sha256_hex[0] == '\0' && bytes.empty(),
                "failed writer published bytes or completion");
    } else {
        require(!threw && host.last_run_status() == 0 && host.last_error().empty(),
                "finite C++ run failed");
        require(host.callbacks == bar_count && receipt.complete == 1
                && receipt.count_e == bar_count && bytes.find("{\"trailer\":") != std::string::npos,
                "finite C++ fd stream did not complete");
    }
    const auto final_flags = fd_trace.record("final assertion");
    require(flags >= 0 && fd_flags >= 0
            && fd_flag_diagnostics::comparable_status_flags(final_flags.flags)
                == fd_flag_diagnostics::comparable_status_flags(flags)
            && final_flags.descriptor_flags == fd_flags, fd_trace.message("borrowed fd changed"));
}
} // namespace

int main() {
    int failures = 0;
    const double values[] = {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()};
    const char* names[] = {"nan", "posinf", "neginf"};
    for (int field = 0; field < 2; ++field) {
        for (std::size_t i = 0; i < 3; ++i) {
            try {
                fd_run(field, values[i], true);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "C++ fd_run %s-%s: %s\n",
                             field == 0 ? "equity" : "open_profit", names[i], error.what());
            }
        }
        try {
            fd_run(field, field == 0 ? 10000.0 : 0.0, false);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "C++ fd_run finite field %d: %s\n", field, error.what());
        }
    }
    return failures == 0 ? 0 : 1;
}
