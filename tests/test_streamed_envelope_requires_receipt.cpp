// F-03: a completed execution alone cannot certify STREAMED publication.
#include "report_modes_fix_fixture.hpp"

using namespace report_modes_fix;

int main() {
    return test_main("test_streamed_envelope_requires_receipt", [] {
        const auto bars = tape(12);
        {
            Host intact;
            configure(intact);
            const auto path = sink_path("intact-receipt");
            install_sink(intact, path);
            require(intact.set_report_mode_v1(ReportMode::STREAMED) == PF_REPORT_MODE_ACCEPTED,
                    "intact STREAMED refused");
            run(intact, bars);
            Envelope complete;
            require(intact.fill_report_envelope_v1(&complete.value) == PF_REPORT_MODE_ACCEPTED,
                    "intact envelope refused");
            require(complete.value.metadata.complete == 1 && receipt(intact).complete == 1,
                    "intact sink did not certify a complete receipt");
            require_trailer(read_sink(path));
        }
        Host replaced;
        configure(replaced);
        install_sink(replaced, sink_path("replaced-receipt"));
        require(replaced.set_report_mode_v1(ReportMode::STREAMED) == PF_REPORT_MODE_ACCEPTED,
                "STREAMED refused");
        run(replaced, bars);
        require(receipt(replaced).complete == 0, "sink completed before first report read");
        const auto other = sink_path("replacement-receipt");
        install_sink(replaced, other);
        for (int read = 0; read < 2; ++read) {
            Envelope incomplete;
            require(replaced.fill_report_envelope_v1(&incomplete.value) == PF_REPORT_MODE_ACCEPTED,
                    "replacement envelope refused");
            require(receipt(replaced).complete == 0, "replacement invented a sink receipt");
            require(incomplete.value.metadata.complete == 0,
                    "STREAMED envelope certified a missing receipt");
            require(replaced.calculations == 12, "report read reran callbacks");
        }
    });
}
