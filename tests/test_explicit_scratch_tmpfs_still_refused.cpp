// Negative control: this is the same native script with an explicit tmpfs
// scratch directory. A disk-backed sink isolates scratch_io from sink_io.
#include "default_scratch_tmpfs_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    require_filesystems();
    // An unlinked disk file avoids leaving a failed sink artifact behind. The
    // lab host's /var/tmp (overlayfs) has no O_TMPFILE, so create and unlink.
    char sink_name[] = "/var/tmp/s4e-explicit-refused-XXXXXX";
    Fd sink(::mkstemp(sink_name));
    require(sink.value >= 0, "lab /var/tmp must support an unlinked disk sink");
    ::unlink(sink_name);
    const auto path = std::string("/proc/self/fd/") + std::to_string(sink.value);
    std::printf("Explicit sink follows only its owned descriptor: %s\n", path.c_str());
    CreationWatch watch;
    const auto before = descriptor_count();
    {
        Host host;
        const pf_report_sink_v1 descriptor{
            sizeof descriptor, 1, path.c_str(), "s4e-explicit-tmpfs", "/dev/shm"};
        require(host.set_report_sink_v1(&descriptor) == 0, "explicit scratch sink setter failed");
        run(host);
        require(host.last_run_status() != 0, "explicit tmpfs scratch unexpectedly succeeded");
        require(std::string(pineforge::run_failure_code_of(host)) == "report_sink_failed"
                    && std::string(pineforge::run_failure_args_of(host))
                        == "{\"reason\":\"scratch_io\"}",
                "explicit tmpfs refusal lost report_sink_failed/scratch_io: " + host.last_error());
        require(host.last_error().find("scratch requires disk backing") != std::string::npos,
                "explicit tmpfs refusal lost its established disk-backing diagnostic");
        pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
        require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 0,
                "refused explicit tmpfs run published a complete receipt");
    }
    bool refused = false;
    try {
        pineforge::detail::ScratchFile file;
        file.bound_cache();
        file.set_directory("/dev/shm");
        file.open("pineforge-s4e-explicit");
    } catch (const pineforge::detail::ReportScratchIoError& error) {
        refused = true;
        require(error.code() == std::error_code(ENODEV, std::generic_category()),
                "explicit bounded scratch changed its established ENODEV refusal");
        require(pineforge::classify_run_failure(error).code
                    == pineforge::RunFailureCode::report_sink_failed,
                "explicit bounded scratch changed its established exception code");
    }
    require(refused, "explicit bounded tmpfs scratch was redirected to a default directory");
    watch.collect();
    watch.require_removed();
    require(descriptor_count() == before, "explicit refusal leaked an unlinked scratch descriptor");
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_explicit_scratch_tmpfs_still_refused", row);
}
