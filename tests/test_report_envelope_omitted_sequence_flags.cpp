// REV p2-f3 item 5: report shape alone cannot describe omitted histories.
#include "report_modes_fix_fixture.hpp"

using namespace report_modes_fix;

namespace {

class TracedHost final : public Host {
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        trace("omitted sequence", bar.close);
    }
};

void configure_recording(TracedHost& host) {
    host.emit_outputs = true;
    configure(host);
    require(host.set_outputs_enabled(true), "outputs refused");
    host.set_trace_enabled(true);
    host.set_broker_state_hash_recording(true);
}

void require_flags(const pf_report_sequence_v1& sequence, std::uint32_t captured,
                   std::uint32_t external, const char* name) {
    require(sequence.captured == captured, std::string(name) + ": captured flag");
    require(sequence.external == external, std::string(name) + ": external flag");
}

void require_optional_flags(const pf_report_metadata_v1& metadata,
                            std::uint32_t captured, std::uint32_t external) {
    require_flags(metadata.trace, captured, external, "trace");
    require_flags(metadata.trace_names, captured, external, "trace_names");
    require_flags(metadata.broker_state_hash, captured, external, "broker_state_hash");
    require_flags(metadata.output_rows, captured, external, "output_rows");
    require_flags(metadata.output_events, captured, external, "output_events");
}

void require_counts(const pf_report_metadata_v1& metadata, std::int64_t points,
                    std::int64_t names) {
    require(metadata.trades.logical_count == 0, "flat fixture acquired trades");
    require(metadata.equity_curve.logical_count == points, "equity count changed");
    require(metadata.trace.logical_count == points, "trace count changed");
    require(metadata.trace_names.logical_count == names, "trace-name count changed");
    require(metadata.broker_state_hash.logical_count == points, "hash count changed");
    require(metadata.output_rows.logical_count == points, "output-row count changed");
    require(metadata.output_events.logical_count == points, "output-event count changed");
}

void require_no_optional_arrays(const ReportC& report) {
    require(report.trace == nullptr && report.trace_len == 0
                && report.trace_names == nullptr && report.trace_names_len == 0
                && report.broker_state_hash == nullptr && report.broker_state_hash_len == 0,
            "omitted or external optional arrays are resident");
}

void full_with_equity_omitted() {
    TracedHost host;
    configure_recording(host);
    require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
            "FULL refused");
    require(host.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
            "equity summary refused");
    const auto bars = tape(12);
    run(host, bars);
    require(host.equity_logical_point_count() == 12, "equity fold missed points");
    for (int read = 0; read < 2; ++read) {
        Envelope result;
        require(host.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                "FULL equity-summary envelope refused");
        const auto& metadata = result.value.metadata;
        const auto& report = result.value.report;
        require(metadata.mode == PF_REPORT_MODE_FULL, "equity control changed report mode");
        require_counts(metadata, host.equity_logical_point_count(), 1);
        require(report.equity_curve == nullptr && report.equity_curve_len == 0,
                "equity control retained a full curve");
        require_flags(metadata.equity_curve, 0, 0, "FULL omitted equity");
        const bool full_curve_resident = report.equity_curve != nullptr
            && report.equity_curve_len == metadata.equity_curve.logical_count;
        require((metadata.equity_curve.captured == 1) == full_curve_resident,
                "FULL equity capture disagrees with the resident curve");
        require_flags(metadata.trades, 1, 0, "FULL trades");
        require_optional_flags(metadata, 1, 0);
    }
}

void full_with_optional_omitted() {
    TracedHost host;
    configure_recording(host);
    require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
            "FULL refused");
    require(host.set_optional_report_mode(report_sequence::ReportMode::SummaryOnly) == 0,
            "optional summary refused");
    const auto bars = tape(12);
    run(host, bars);
    for (int read = 0; read < 2; ++read) {
        Envelope result;
        require(host.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                "FULL optional-summary envelope refused");
        const auto witness = host.optional_sequence_summary();
        require(witness.traces.count == 12 && witness.hashes.count == 12
                    && witness.rows.count == 12 && witness.events.count == 12,
                "fixture did not fold all optional records");
        const auto& metadata = result.value.metadata;
        const auto& report = result.value.report;
        require(metadata.mode == PF_REPORT_MODE_FULL && metadata.complete == 1,
                "optional control changed FULL mode or completion");
        require(metadata.trace.logical_count == static_cast<std::int64_t>(witness.traces.count)
                    && metadata.broker_state_hash.logical_count
                        == static_cast<std::int64_t>(witness.hashes.count)
                    && metadata.output_rows.logical_count
                        == static_cast<std::int64_t>(witness.rows.count)
                    && metadata.output_events.logical_count
                        == static_cast<std::int64_t>(witness.events.count),
                "optional logical counts differ from their witnesses");
        // Names are embedded in trace witness leaves; there is no separate
        // name witness or interned name table in optional SummaryOnly.
        // CORE-TOP ruling (N4, 12:10): trace_names.logical_count is the number
        // of names RETAINED (0 when the sequence is not captured), documented
        // in o1-report-contract.md next to the logical_count definition; a
        // distinct-name count would need a growing dictionary.
        require_counts(metadata, 12, 0);
        require_no_optional_arrays(report);
        require(host.output_events_len() == 0, "omitted events are resident");
        require_optional_flags(metadata, 0, 0);
        require_flags(metadata.trades, 1, 0, "FULL trades");
        require_flags(metadata.equity_curve, 1, 0, "FULL equity");
        require(report.equity_curve != nullptr && report.equity_curve_len == 12,
                "optional control omitted equity");
    }
}

void unchanged_mode(ReportMode mode, bool optional_summary = false) {
    TracedHost host;
    configure_recording(host);
    const bool streamed = mode == ReportMode::STREAMED;
    std::string path;
    if (streamed) {
        path = sink_path(optional_summary ? "omitted-flags-streamed-summary"
                                         : "omitted-flags-streamed");
        install_sink(host, path);
    }
    if (optional_summary)
        require(host.set_optional_report_mode(report_sequence::ReportMode::SummaryOnly) == 0,
                "streamed optional summary refused");
    require(host.set_report_mode_v1(mode) == PF_REPORT_MODE_ACCEPTED, "report mode refused");
    const auto bars = tape(12);
    run(host, bars);
    for (int read = 0; read < 2; ++read) {
        Envelope result;
        require(host.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                "control envelope refused");
        const auto& metadata = result.value.metadata;
        const auto& report = result.value.report;
        require(metadata.mode == static_cast<pf_report_mode_v1_t>(mode)
                    && metadata.complete == 1, "control mode or completion changed");
        const std::uint32_t captured = mode == ReportMode::SUMMARY_ONLY ? 0u : 1u;
        const std::uint32_t external = streamed ? 1u : 0u;
        require_flags(metadata.trades, captured, external, "control trades");
        require_flags(metadata.equity_curve, captured, external, "control equity");
        require_optional_flags(metadata, captured, external);
        require_counts(metadata, 12, mode == ReportMode::FULL ? 1 : 0);
        if (mode == ReportMode::FULL) {
            require(report.equity_curve != nullptr && report.equity_curve_len == 12
                        && report.trace != nullptr && report.trace_len == 12
                        && report.trace_names != nullptr && report.trace_names_len == 1
                        && report.broker_state_hash != nullptr
                        && report.broker_state_hash_len == 12
                        && host.output_bars_len() == 12 && host.output_events_len() == 12,
                    "FULL control did not retain enabled histories");
        } else {
            require(report.trades == nullptr && report.trades_len == 0
                        && report.equity_curve == nullptr && report.equity_curve_len == 0,
                    "non-FULL control owns trades or equity");
            require_no_optional_arrays(report);
        }
        if (streamed) {
            const auto got = receipt(host);
            require(got.complete == 1 && got.count_e == 12 && got.count_t == 0
                        && got.count_x == 12 && got.count_h == 12
                        && got.count_o == 12 && got.count_v == 12,
                    "STREAMED receipt does not hold all enabled sequences");
            require(metadata.equity_curve.logical_count == got.count_e
                        && metadata.trace.logical_count == got.count_x
                        && metadata.broker_state_hash.logical_count == got.count_h
                        && metadata.output_rows.logical_count == got.count_o
                        && metadata.output_events.logical_count == got.count_v,
                    "STREAMED logical counts differ from the receipt");
        }
    }
    if (streamed) require_trailer(read_sink(path));
}

} // namespace

int main() {
    return test_main("test_report_envelope_omitted_sequence_flags", [] {
        full_with_equity_omitted();
        full_with_optional_omitted();
        unchanged_mode(ReportMode::FULL);
        unchanged_mode(ReportMode::SUMMARY_ONLY);
        unchanged_mode(ReportMode::STREAMED);
        // A sink still captures complete optional sequences under the
        // retention control; external capture must not become omitted.
        unchanged_mode(ReportMode::STREAMED, true);
    });
}
