// F-02: logical optional counts come from folds after history is retired.
#include "report_modes_fix_fixture.hpp"

using namespace report_modes_fix;

namespace {
class TracedHost final : public Host {
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        trace("summary count", bar.close);
    }
};
}

int main() {
    return test_main("test_report_envelope_optional_summary_counts", [] {
        TracedHost host;
        host.emit_outputs = true;
        configure(host);
        require(host.set_outputs_enabled(true), "outputs refused");
        host.set_trace_enabled(true);
        host.set_broker_state_hash_recording(true);
        require(host.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED,
                "SUMMARY_ONLY refused");
        require(host.set_optional_report_mode(report_sequence::ReportMode::SummaryOnly) == 0,
                "optional summary refused");
        run(host, tape(12));
        for (int read = 0; read < 2; ++read) {
            Envelope result;
            require(host.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                    "summary envelope refused");
            const auto witness = host.optional_sequence_summary();
            require(witness.rows.count == 12 && witness.events.count == 12
                        && witness.hashes.count == 12 && witness.traces.count == 12,
                    "fixture did not fold all twelve optional records");
            const auto& metadata = result.value.metadata;
            require(metadata.output_rows.logical_count
                        == static_cast<std::int64_t>(witness.rows.count),
                    "output logical count used the resident tail");
            require(metadata.output_events.logical_count
                        == static_cast<std::int64_t>(witness.events.count),
                    "event logical count used the empty queue");
            require(metadata.broker_state_hash.logical_count
                        == static_cast<std::int64_t>(witness.hashes.count),
                    "hash logical count used the resident tail");
            require(metadata.trace.logical_count
                        == static_cast<std::int64_t>(witness.traces.count),
                    "trace logical count used the empty buffer");
            require(result.value.report.trace == nullptr
                        && result.value.report.broker_state_hash == nullptr
                        && metadata.output_rows.captured == 0
                        && metadata.output_events.captured == 0
                        && metadata.broker_state_hash.captured == 0
                        && metadata.trace.captured == 0,
                    "summary optional sequences were captured");
        }
    });
}
