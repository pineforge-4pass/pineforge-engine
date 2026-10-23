// A-2/B2-06: the contract requires refusal before execution for a presenter
// without an exact non-FULL adapter. BASE accepts both of these mode setters.
#include "report_terminal_quote_fix_fixture.hpp"

using namespace report_modes_fix;

int main() {
    return test_main("test_terminal_quote_report_modes", [] {
        const auto bars = quote_tape();
        for (bool long_side : {false, true}) {
            QuoteHost full(long_side), omitted(long_side), streamed(long_side);
            require(full.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                    "Pine presenter FULL mode refused");
            run_quote(full, bars);
            Report reference;
            full.fill_report(&reference.value);
            require_quote_rows(full, reference.value);

            require(omitted.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                        == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                    "whole-array Pine presenter accepted SUMMARY_ONLY without an adapter");
            require(omitted.native_state().kind == NativeLifecycleKind::Unconfigured,
                    "SUMMARY_ONLY refusal consumed the unbound Pine host");
            run_quote(omitted, bars);
            Envelope unchanged;
            require(omitted.fill_report_envelope_v1(&unchanged.value) == PF_REPORT_MODE_ACCEPTED,
                    "refused setter changed FULL configuration");
            require(unchanged.value.metadata.mode == PF_REPORT_MODE_FULL,
                    "refused SUMMARY_ONLY setter was not atomic");
            require_quote_rows(omitted, unchanged.value.report);
            require(same_bits(unchanged.value.report.net_profit, reference.value.net_profit),
                    "refused setter changed FULL net-profit bits");
            require(std::memcmp(&unchanged.value.report.metrics, &reference.value.metrics,
                                sizeof(pf_metrics_t)) == 0,
                    "refused setter changed FULL metric bits");
            const auto& last = reference.value.equity_curve[reference.value.equity_curve_len - 1];
            const auto& unchanged_last = unchanged.value.report.equity_curve[
                unchanged.value.report.equity_curve_len - 1];
            require(std::memcmp(&unchanged_last, &last, sizeof last) == 0,
                    "refused setter changed FULL terminal equity bits");

            const auto path = sink_path(long_side ? "quote-long" : "quote-short");
            install_sink(streamed, path);
            require(streamed.set_report_mode_v1(ReportMode::STREAMED) == PF_REPORT_MODE_CONFLICT,
                    "whole-array Pine presenter accepted STREAMED without an adapter");
            require(streamed.native_state().kind == NativeLifecycleKind::Unconfigured,
                    "STREAMED refusal consumed the unbound Pine host");
            // The rejected explicit mode preserves the already supported
            // legacy FULL sink route and its presented open-row view.
            run_quote(streamed, bars);
            Report legacy;
            streamed.fill_report(&legacy.value);
            const auto got = receipt(streamed);
            const auto provenance = diagnostics(streamed);
            require(got.complete == 1 && got.count_t == reference.value.total_trades,
                    "refused setter changed legacy sink completion");
            require(same_bits(legacy.value.net_profit, reference.value.net_profit),
                    "legacy terminal-quote sink lost open PnL");
            require(std::memcmp(&legacy.value.metrics, &reference.value.metrics,
                                sizeof(pf_metrics_t)) == 0,
                    "refused setter changed legacy FULL sink metric bits");
            require(provenance.complete == 1
                        && std::memcmp(&provenance.last, &last, sizeof last) == 0,
                    "legacy terminal-quote sink changed the last equity point");
            require_trailer(read_sink(path));
        }
        require_non_presenter_mode_parity(ReportMode::SUMMARY_ONLY, "quote-no-presenter-summary");
        require_non_presenter_mode_parity(ReportMode::STREAMED, "quote-no-presenter-streamed");
    });
}
