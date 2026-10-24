// B2-03: BASE substitutes the resident one-point tail for both a transferred
// selected scratch curve and the equity-summary fold's logical point count.
#include "report_modes_fix_fixture.hpp"
#include <pineforge/selected_window.h>

namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(
    BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
int native_selected_window_counts_v1(
    const BacktestEngine*, pf_selected_window_counts_v1*) noexcept;
}
}

using namespace report_modes_fix;

int main() {
    return test_main("test_report_envelope_equity_logical_count", [] {
        {
            // count_selected_script counts every consumed label >= start_ms;
            // SelectedCaptureLatches validates only [start_ms,end_ms). Feed
            // the two preroll bars and six window bars, with no post-end bars.
            const auto bars = tape(8);
            Host selected;
            configure(selected);
            const pf_selected_window_config_v1 window{
                sizeof window, 1, start_ms + 2 * minute_ms, start_ms + 8 * minute_ms};
            require(native_set_selected_window_v1(&selected, &window) == 0,
                    "selected window refused");
            const auto path = sink_path("selected-equity-count");
            install_sink(selected, path);
            require(selected.set_report_mode_v1(ReportMode::STREAMED)
                        == PF_REPORT_MODE_ACCEPTED, "STREAMED refused");
            run(selected, bars);
            pf_selected_window_counts_v1 counts{};
            counts.struct_size = sizeof counts;
            counts.version = 1;
            require(native_selected_window_counts_v1(&selected, &counts) == 0,
                    "selected fixture has no successful counts");
            require(counts.fed_input_bars == 8 && counts.fed_script_bars == 8
                        && counts.preroll_input_bars == 2 && counts.preroll_script_bars == 2
                        && counts.window_input_bars == 6 && counts.window_script_bars == 6,
                    "selected fixture did not consume two preroll and six window bars");
            Envelope result;
            require(selected.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                    "selected envelope refused");
            const auto got = receipt(selected);
            require(got.complete == 1 && got.count_e == 7,
                    "selected fixture lacks six observations and its anchor");
            require(result.value.metadata.equity_curve.logical_count == got.count_e,
                    "selected logical equity count lost its transferred scratch");
            require(result.value.report.equity_curve == nullptr
                        && result.value.report.equity_curve_len == 0,
                    "selected STREAMED report owns equity");
            require_trailer(read_sink(path));
        }
        {
            const auto bars = tape(12);
            Host summary_host;
            configure(summary_host);
            require(summary_host.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                        == PF_REPORT_MODE_ACCEPTED, "SUMMARY_ONLY refused");
            require(summary_host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                    "equity summary refused");
            run(summary_host, bars);
            require(summary_host.equity_logical_point_count() == 12,
                    "equity fold did not see all twelve bars");
            Envelope result;
            require(summary_host.fill_report_envelope_v1(&result.value)
                        == PF_REPORT_MODE_ACCEPTED, "summary envelope refused");
            require(result.value.metadata.equity_curve.logical_count == 12,
                    "summary envelope counted its held point instead of observations");
            require(result.value.metadata.equity_curve.logical_count
                        == summary_host.equity_logical_point_count(),
                    "envelope differs from equity fold count");
        }
    });
}
