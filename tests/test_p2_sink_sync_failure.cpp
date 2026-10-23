// B2-07: fail the real final fdatasync after its pwrite/ftruncate succeeded.
#include "p2_sink_writer_fixture.hpp"
#include "run_sigterm_abort_fixture.hpp"
#include "../src/sha256.hpp"

namespace {
std::string active_path;
bool inject_sync_error = false;
bool saw_complete_trailer = false;
}

// Compile the actual writer with a single syscall seam, following
// test_report_nonfinite_equity_writer.cpp. No production fault knob is needed.
int p2_sink_fdatasync(int fd) {
    const auto bytes = p2_sink::read_file(active_path);
    if (inject_sync_error && bytes.find("{\"trailer\":") != std::string::npos) {
        p2_sink::require(!bytes.empty() && bytes.back() == '\n',
                         "injection did not follow successful logical ftruncate");
        saw_complete_trailer = true;
        errno = EIO;
        return -1;
    }
    return ::fdatasync(fd);
}

#define fdatasync p2_sink_fdatasync
#include "../src/report_sink_writer.cpp"
#undef fdatasync

namespace {
void engine_receipt_failure() {
    p2_sink::Directory directory;
    p2_sink::require(::setenv("TMPDIR", directory.path.c_str(), 1) == 0,
                     "keep engine scratch under scratch-s4e");
    active_path = directory.file("engine-sync-failure.ndjson");
    q7_fixture::Host host;
    host.configure("1", "1");
    const pf_report_sink_v1 sink{sizeof sink, 1, active_path.c_str(), "B2-07-engine",
                                 directory.path.c_str()};
    p2_sink::require(host.set_report_sink_v1(&sink) == 0, "install engine sync-failure sink");
    const pineforge::Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067200000)},
        {100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067260000)},
    };
    host.run(bars, 2);
    p2_sink::require(host.last_run_status() == 0 && host.callbacks == 2,
                     "engine sync-failure fixture did not finish its input");
    inject_sync_error = true;
    saw_complete_trailer = false;
    pineforge::ReportC report{};
    bool failed = false;
    try {
        host.fill_report(&report);
    } catch (const std::exception& error) {
        failed = pineforge::classify_run_failure(error).code
            == pineforge::RunFailureCode::report_sink_failed;
    }
    pineforge::BacktestEngine::free_report(&report);
    inject_sync_error = false;
    p2_sink::require(failed && saw_complete_trailer && host.last_run_status() != 0,
                     "engine capture did not reach the injected final-sync failure");
    p2_sink::require(std::string(pineforge::run_failure_code_of(host)) == "report_sink_failed",
                     "engine lost the typed sink failure");
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    p2_sink::require(host.report_sink_receipt_v1(&receipt) == 0
                         && receipt.complete == 0 && receipt.sha256_hex[0] == '\0',
                     "failed final sync published a completed engine receipt");
    const auto bytes = p2_sink::read_file(active_path);
    p2_sink::require(bytes.find("[\"E\",") != std::string::npos && bytes.back() == '\n',
                     "engine rollback lost its complete data prefix");
    p2_sink::require(bytes.find("{\"trailer\":") == std::string::npos,
                     "incomplete engine receipt disagrees with the trailer on disk");
}

void sync_failure() {
    p2_sink::Directory directory;
    active_path = directory.file("sync-failure.ndjson");
    const pf_report_sink_v1 sink{sizeof sink, 1, active_path.c_str(), "B2-07", nullptr};
    p2_sink::rs::LineWriter writer(sink);
    writer.write_header({false, false, false, 0, 0});
    writer.write_e({0, 10000.0, 0.0});
    inject_sync_error = true;
    saw_complete_trailer = false;
    p2_sink::expect_io([&] { (void)writer.write_trailer(); }, EIO);
    inject_sync_error = false;
    p2_sink::require(saw_complete_trailer, "final-sync fault was never reached");
    const auto bytes = p2_sink::read_file(active_path);
    p2_sink::require(bytes.find("[\"E\",0,10000.0,0.0]\n") != std::string::npos,
                     "rollback removed a completed data record");
    p2_sink::require(bytes.find("{\"trailer\":") == std::string::npos,
                     "failed final sync left a reader-acceptable trailer");
    p2_sink::require(!bytes.empty() && bytes.back() == '\n',
                     "rollback did not preserve a whole-record prefix");
    p2_sink::require(writer.diagnostics().complete == 0,
                     "failed sync completed writer provenance");
    bool poisoned = false;
    try { (void)writer.write_trailer(); }
    catch (const p2_sink::rs::WriterError& error) {
        poisoned = error.reason() == p2_sink::rs::WriterFailure::invalid_state;
    }
    p2_sink::require(poisoned && p2_sink::read_file(active_path) == bytes,
                     "failed writer could republish a trailer");
}

void successful_sync() {
    p2_sink::Directory directory;
    active_path = directory.file("sync-success.ndjson");
    const pf_report_sink_v1 sink{sizeof sink, 1, active_path.c_str(), "B2-07", nullptr};
    p2_sink::rs::LineWriter writer(sink);
    writer.write_header({false, false, false, 0, 0});
    writer.write_e({0, 10000.0, 0.0});
    const auto trailer = writer.write_trailer();
    const auto bytes = p2_sink::read_file(active_path);
    const auto start = bytes.find("{\"trailer\":");
    p2_sink::require(start != std::string::npos && bytes.back() == '\n',
                     "successful final sync lost its trailer");
    pineforge::report_sink::Sha256 hash;
    hash.update(std::string_view(bytes.data(), start));
    p2_sink::require(trailer.count_e == 1 && trailer.count_t == 0
                         && hash.hex_digest() == trailer.sha256_hex
                         && writer.diagnostics().complete == 1,
                     "successful sync changed counts, digest or completion");
}
}

int main() {
    return p2_sink::test_main("B2-07 final sync", [] {
        engine_receipt_failure();
        sync_failure();
        successful_sync();
    });
}
