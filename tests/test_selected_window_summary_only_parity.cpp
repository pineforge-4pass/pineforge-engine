// argon F-03: selected/observed capture without a sink needs exact support
// before SUMMARY_ONLY can be admitted or published.
#include "report_modes_fix_fixture.hpp"
#include <pineforge/execution_observer.h>
#include <pineforge/selected_window.h>

namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(
    BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
int native_set_execution_observer_v1(
    BacktestEngine*, const pf_execution_observer_v1*) noexcept;
}
}

using namespace report_modes_fix;

namespace {
class CaptureHost final : public Host {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = calculations++;
        // No carry-in: all three lots enter inside the selected window.
        if (index >= 2 && index < 5) {
            const auto result = submit({no::Transact{1.0}, "window entry", ""});
            require(result.status == no::SubmitStatus::Accepted && result.handle.has_value(),
                    "selected fixture entry refused");
        }
    }
};

int before_results(void* context, const pf_execution_boundary_v1*,
                   pf_boundary_receipt_v1* receipt) {
    ++*static_cast<int*>(context);
    receipt->frame_bytes = 0;
    return 0;
}

void install_control(Host& host, bool selected, int& calls) {
    if (selected) {
        const pf_selected_window_config_v1 window{
            sizeof window, 1, start_ms + 2 * minute_ms, start_ms + 8 * minute_ms};
        require(native_set_selected_window_v1(&host, &window) == 0,
                "selected window refused");
    } else {
        const pf_execution_observer_v1 observer{sizeof observer, 1, &calls, before_results};
        require(native_set_execution_observer_v1(&host, &observer) == 0,
                "execution observer refused");
    }
}

void clear_control(Host& host, bool selected) {
    require((selected ? native_set_selected_window_v1(&host, nullptr)
                      : native_set_execution_observer_v1(&host, nullptr)) == 0,
            "pending control removal refused");
}
}

int main() {
    return test_main("test_selected_window_summary_only_parity", [] {
        // Two preroll plus six selected bars; no post-window labels.
        const auto bars = tape(8);
        for (bool selected : {true, false}) {
            int calls = 0;
            CaptureHost host;
            configure(host);
            install_control(host, selected, calls);
            require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                        == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                    "sinkless selected/observer SUMMARY_ONLY was admitted");
            require(host.set_report_mode_v1(ReportMode::STREAMED) == PF_REPORT_MODE_CONFLICT,
                    "sinkless STREAMED was admitted");
            run(host, bars);
            Envelope full;
            require(host.fill_report_envelope_v1(&full.value) == PF_REPORT_MODE_ACCEPTED,
                    "refusal changed the previous FULL configuration");
            require(full.value.metadata.mode == PF_REPORT_MODE_FULL
                        && full.value.report.total_trades == 3
                        && full.value.report.metrics.all.num_trades == 3,
                    "refusal did not preserve exact FULL trades");
            require(full.value.report.equity_curve != nullptr
                        && full.value.report.equity_curve_len == (selected ? 7 : 8),
                    "refusal did not preserve FULL equity capture");
            require(calls == (selected ? 0 : 1), "observer boundary count changed");
        }
        for (bool selected : {true, false}) {
            int calls = 0;
            CaptureHost late;
            configure(late);
            require(late.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                    "ordinary native SUMMARY_ONLY refused");
            int observer_status = 0;
            if (selected) {
                install_control(late, selected, calls);
            } else {
                const pf_execution_observer_v1 observer{
                    sizeof observer, 1, &calls, before_results};
                observer_status = native_set_execution_observer_v1(&late, &observer);
                // X1 is picked: the setter refuses an observer while the v1 mode
                // is SUMMARY_ONLY. A 0 here means the refusal is absent.
                require(observer_status == -2,
                        "late execution observer must be refused with -2 under X1: status "
                            + std::to_string(observer_status));
            }
            late.run(bars.data(), static_cast<int>(bars.size()));
            if (selected && late.last_run_status() != 0) {
                // The X1 series fails this order closed at the earliest point
                // (begin refuses report_mode_unsupported, or the capture-path
                // backstop raises engine_invariant): the run fails with a
                // typed code and no report is ever published.
                const std::string code = pineforge::run_failure_code_of(late);
                require(code == "report_mode_unsupported" || code == "engine_invariant",
                        "late selected/observer SUMMARY_ONLY failed without the typed code: "
                            + code + " / " + late.last_error());
                Envelope refused;
                const int status = late.fill_report_envelope_v1(&refused.value);
                require(status != PF_REPORT_MODE_ACCEPTED || refused.value.metadata.complete == 0,
                        "failed late capture was certified complete");
                require(refused.value.report.equity_curve == nullptr
                            && refused.value.report.trades == nullptr,
                        "failed late capture published report arrays");
                clear_control(late, selected);
                continue;
            }
            require_completed_run(late, bars);
            require(late.open_lots() == 3, "late capture lacks its three in-window lots");
            if (!selected && observer_status == -2) {
                // X1 refuses registration without binding an observer or
                // changing the ordinary native SUMMARY_ONLY configuration.
                require(calls == 0, "refused late observer was called");
                for (int read = 0; read < 2; ++read) {
                    Envelope result;
                    require(late.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                            "refused late observer prevented ordinary summary publication");
                    require(result.value.metadata.complete == 1
                                && result.value.metadata.mode == PF_REPORT_MODE_SUMMARY_ONLY,
                            "refused late observer changed summary completion or mode");
                    require(result.value.report.total_trades == 3
                                && result.value.report.total_trades
                                    == result.value.report.metrics.all.num_trades,
                            "refused late observer summary lost exact trades");
                    require(result.value.report.trades == nullptr
                                && result.value.report.trades_len == 0
                                && result.value.report.equity_curve == nullptr
                                && result.value.report.equity_curve_len == 0,
                            "refused late observer summary published report arrays");
                    clear_control(late, selected);
                }
                continue;
            }
            // A successfully installed control still makes the read refuse.
            // Removing pending configuration cannot erase the admitted capture.
            for (int read = 0; read < 2; ++read) {
                Envelope result;
                require(late.fill_report_envelope_v1(&result.value)
                            == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                        "late selected/observer capture published an inexact summary");
                require(result.value.metadata.complete == 0
                            && result.value.report.total_trades == 0
                            && result.value.report.equity_curve == nullptr,
                        "refused envelope modified its destination");
                clear_control(late, selected);
            }
        }
    });
}
