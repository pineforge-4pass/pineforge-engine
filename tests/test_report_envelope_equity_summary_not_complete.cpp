// F-01: a completed execution cannot certify pending second-pass ratios.
#include "report_modes_fix_fixture.hpp"
#include <pineforge/metrics_fold.hpp>
#include <cmath>

using namespace report_modes_fix;

int main() {
    return test_main("test_report_envelope_equity_summary_not_complete", [] {
        auto bars = tape(12);
        // Distinct months and both signs of returns exercise all four ratios.
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const double price = 100.0 + static_cast<double>((i * 7) % 11) * 0.5;
            bars[i] = {price, price + 0.5, price - 0.5, price + 0.125, 1.0,
                       start_ms + static_cast<std::int64_t>(i) * 32 * 86400000LL};
        }
        Host full, candidate;
        full.hold_lots = candidate.hold_lots = 3;
        configure(full);
        configure(candidate);
        require(candidate.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                    == PF_REPORT_MODE_ACCEPTED, "SUMMARY_ONLY refused");
        require(candidate.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                "equity summary refused");
        run(full, bars);
        run(candidate, bars);
        Report oracle;
        full.fill_report(&oracle.value);
        require(oracle.value.total_trades == 3 && oracle.value.equity_curve_len == 12,
                "FULL oracle lacks the three lots or twelve points");
        const auto& ratios = oracle.value.metrics.equity;
        require(std::isfinite(ratios.sharpe_bar) && std::isfinite(ratios.sortino_bar)
                    && std::isfinite(ratios.sharpe_monthly)
                    && std::isfinite(ratios.sortino_monthly),
                "FULL fixture did not produce finite bar and monthly ratios");

        Envelope pending;
        require(candidate.fill_report_envelope_v1(&pending.value) == PF_REPORT_MODE_ACCEPTED,
                "pending envelope refused");
        const auto* first = candidate.equity_summary_snapshot();
        require(first && first->needs_second_pass() && !candidate.equity_summary_ratios_ready(),
                "fixture has no pending second pass");
        require(std::isnan(pending.value.report.metrics.equity.sharpe_bar)
                    && std::isnan(pending.value.report.metrics.equity.sortino_bar),
                "uninstalled bar ratios unexpectedly exist");
        require(pending.value.metadata.complete == 0,
                "pending equity ratios were certified complete");

        metrics::EquityMetricsSecondPass second(*first);
        for (std::int64_t i = 0; i < oracle.value.equity_curve_len; ++i) {
            const auto& point = oracle.value.equity_curve[i];
            bool exposed = false;
            for (int row = 0; row < oracle.value.trades_len; ++row)
                exposed = exposed || point.time_ms >= oracle.value.trades[row].entry_time;
            second.fold(point, metrics::equity_month_key(point.time_ms, "UTC"), true, exposed);
        }
        require(candidate.install_equity_ratio_result(second.finish()),
                "ordered FULL points did not match the summary snapshot");
        for (int read = 0; read < 2; ++read) {
            Envelope complete;
            require(candidate.fill_report_envelope_v1(&complete.value) == PF_REPORT_MODE_ACCEPTED,
                    "installed envelope refused");
            require(candidate.equity_summary_ratios_ready() && complete.value.metadata.complete == 1,
                    "installed ratios did not complete the envelope");
            const auto& installed = complete.value.report.metrics.equity;
            require(std::isfinite(installed.sharpe_bar) && std::isfinite(installed.sortino_bar)
                        && std::isfinite(installed.sharpe_monthly)
                        && std::isfinite(installed.sortino_monthly),
                    "installed ratios are not finite");
            require(std::memcmp(&complete.value.report.metrics, &oracle.value.metrics,
                                sizeof(pf_metrics_t)) == 0,
                    "installed summary metric bits differ from FULL");
        }
    });
}
