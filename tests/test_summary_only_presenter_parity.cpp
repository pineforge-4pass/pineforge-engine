// B2-06: refusal is determined before a Pine host binds its run spec, and
// remains atomic even when the separate equity-summary setter is configured.
#include "report_terminal_quote_fix_fixture.hpp"

using namespace report_modes_fix;

int main() {
    return test_main("test_summary_only_presenter_parity", [] {
        QuoteHost host(true);
        require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                "equity-summary setup refused");
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY)
                    == PF_REPORT_MODE_REPLAY_UNAVAILABLE,
                "presenter host accepted SUMMARY_ONLY with a one-point equity view");
        require(host.native_state().kind == NativeLifecycleKind::Unconfigured,
                "mode refusal consumed the unbound presenter host");
        // Restore the independent equity setting for the FULL parity witness.
        require(host.set_equity_report_mode(ReportMode::FULL) == 0,
                "equity mode restore refused");
        const auto bars = quote_tape();
        QuoteHost full(true);
        run_quote(full, bars);
        Report reference;
        full.fill_report(&reference.value);
        require_quote_rows(full, reference.value);
        run_quote(host, bars);
        Envelope result;
        require(host.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                "FULL envelope refused after mode rejection");
        require(result.value.metadata.complete == 1
                    && result.value.metadata.mode == PF_REPORT_MODE_FULL,
                "mode refusal changed the completed FULL envelope");
        require_quote_rows(host, result.value.report);
        require(same_bits(result.value.report.net_profit, reference.value.net_profit),
                "mode refusal changed FULL net-profit bits");
        require(std::memcmp(&result.value.report.metrics, &reference.value.metrics,
                            sizeof(pf_metrics_t)) == 0,
                "mode refusal changed FULL metric bits");
        const auto& last = reference.value.equity_curve[reference.value.equity_curve_len - 1];
        const auto& actual_last = result.value.report.equity_curve[
            result.value.report.equity_curve_len - 1];
        require(std::memcmp(&actual_last, &last, sizeof last) == 0,
                "mode refusal changed FULL terminal equity bits");
        require_non_presenter_mode_parity<SealedPrefixHost>(
            ReportMode::SUMMARY_ONLY, "summary-no-presenter-summary");
        require_non_presenter_mode_parity<SealedPrefixHost>(
            ReportMode::STREAMED, "summary-no-presenter-streamed");
    });
}
