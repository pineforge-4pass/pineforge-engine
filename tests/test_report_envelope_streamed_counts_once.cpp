// B2-02: BASE adds the held output/event/hash buffers after their records
// have already advanced the completed sink receipt.
#include "report_modes_fix_fixture.hpp"

using namespace report_modes_fix;

int main() {
    return test_main("test_report_envelope_streamed_counts_once", [] {
        Host host;
        host.emit_outputs = true;
        require(host.set_outputs_enabled(true), "outputs enable refused");
        host.set_broker_state_hash_recording(true);
        configure(host);
        const auto path = sink_path("counts-once");
        install_sink(host, path);
        require(host.set_report_mode_v1(ReportMode::STREAMED) == PF_REPORT_MODE_ACCEPTED,
                "STREAMED refused");
        const auto bars = tape();
        run(host, bars);
        require(host.open_lots() == 0, "count fixture must finish flat");

        for (int read = 0; read < 2; ++read) {
            Envelope result;
            require(host.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                    "envelope read refused");
            const auto got = receipt(host);
            require(got.complete == 1 && result.value.metadata.complete == 1,
                    "count fixture did not complete");
            require(got.count_o == 12 && got.count_v == 12 && got.count_h == 12,
                    "fixture did not emit one output, event and hash per bar");
            require(result.value.metadata.output_rows.logical_count == got.count_o,
                    "output rows counted twice after finish");
            require(result.value.metadata.output_events.logical_count == got.count_v,
                    "output events counted twice after finish");
            require(result.value.metadata.broker_state_hash.logical_count == got.count_h,
                    "broker hashes counted twice after finish");
        }
        require_trailer(read_sink(path));
    });
}
