// B2-12 RED: an abort observed immediately before the sink trailer must still
// return the normal sink shape, with every report-owned array released.
#include "report_modes_fix_fixture.hpp"

#include <csignal>

using namespace report_modes_fix;

namespace {

struct Sigterm {
    Sigterm() {
        require(pf_run_abort_install_sigterm_v1() == 0,
                "cannot install SIGTERM abort handler");
    }
    ~Sigterm() { pf_run_abort_restore_sigterm_v1(); }
};

} // namespace

int main() {
    try {
        Host host;
        configure(host);
        host.hold_lots = 3;
        host.emit_outputs = true;
        host.set_broker_state_hash_recording(true);
        const auto path = sink_path("p2-abort-shape");
        install_sink(host, path);
        run(host, tape(8));

        Sigterm signals;
        require(::raise(SIGTERM) == 0, "cannot request report-time abort");
        Report report;
        host.fill_report(&report.value);

        require(report.value.trades == nullptr && report.value.trades_len == 0,
                "aborted sink report retained trades");
        require(report.value.equity_curve == nullptr
                    && report.value.equity_curve_len == 0,
                "aborted sink report retained equity curve");
        require(report.value.broker_state_hash == nullptr
                    && report.value.broker_state_hash_len == 0,
                "aborted sink report retained broker hashes");
        require(report.value.trace == nullptr && report.value.trace_len == 0,
                "aborted sink report retained trace");
        require(report.value.trace_names == nullptr
                    && report.value.trace_names_len == 0,
                "aborted sink report retained trace names");
        require(report.value.security_diag == nullptr
                    && report.value.security_diag_len == 0,
                "aborted sink report retained security diagnostics");
        std::puts("B2-12 sink shape after report-time abort: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "B2-12: %s\n", error.what());
        return 1;
    }
}
