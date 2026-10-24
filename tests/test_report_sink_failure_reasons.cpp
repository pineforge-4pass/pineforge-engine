// Real I/O failures and the A7 writer translators. No runtime fault hooks.
// Frozen batch-C lacks WriterError::run_failure() and DiskIdTable::set_store.
// Compile fail-before and behavioral red/green evidence belong to the lab.
#include "report_sink_failure_fixture.hpp"
#include "scratch_descriptor_fixture.hpp"
#include "../src/report_scratch.hpp"
#include "../src/report_sink_writer.hpp"

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/selected_window.h>

#include <array>
#include <cstring>

namespace {
using namespace cr150_w1;
namespace detail = pineforge::detail;
namespace rs = pineforge::report_sink;
constexpr auto chunk = detail::ScratchArena::kChunkBytes;
constexpr std::size_t writer_buffer = 64 * 1024;

template<class Operation>
void expect_writer_io(Operation operation, int number, const char* reason,
                      const char* message = nullptr) {
    try {
        operation();
    } catch (const rs::WriterError& error) {
        require(error.reason() == rs::WriterFailure::io && error.error_number() == number,
                "writer retains its I/O error number");
        require(!message || std::strstr(error.what(), message),
                "writer retains the expected failure origin");
        expect_failure(error.run_failure(), reason_args(reason));
        expect_failure(pineforge::classify_run_failure(error), reason_args(reason));
        return;
    }
    throw std::runtime_error("real writer operation unexpectedly succeeded");
}

void scratch_file_failures() {
    Directory directory;
    detail::ScratchFile scratch;
    scratch.set_directory(directory.path + "/missing");
    expect_io([&] { scratch.open("unwritable"); }, ENOENT, "scratch_io");
    scratch.set_directory("/dev/null");
    expect_io([&] { scratch.open("not-a-directory"); }, ENOTDIR, "scratch_io");
    scratch.set_directory(directory.path);
    scratch.set_store(detail::ScratchStore::curve);
    scratch.open("file-size-failure");
    const std::array<char, 2> bytes{{'a', 'b'}};
    {
        FileSizeLimit limit(0);
        expect_io([&] { scratch.ensure_size(bytes.size()); }, EFBIG, "scratch_io");
        expect_io([&] { scratch.write(0, bytes.data(), bytes.size()); }, EFBIG, "scratch_io");
    }
    char byte = 0;
    expect_io([&] { scratch.read(0, &byte, 1); }, EIO, "scratch_io");
}

void id_pair_file_failures() {
    Directory directory;
    detail::IdPairScratchFile file;
    const auto expect_auxiliary_io = [](auto operation, int number) {
        bool caught = false;
        try { operation(); }
        catch (const std::system_error& error) {
            const auto failure = pineforge::classify_run_failure(error);
            require(error.code().value() == number, "auxiliary scratch retains syscall errno");
            require(failure.code == pineforge::RunFailureCode::scratch_io_failed,
                    "id-pair I/O retains scratch_io_failed");
            require(pineforge::run_failure_code_class(failure.code) == pineforge::RunFailureClass::resource
                        && !pineforge::run_failure_code_retryable(failure.code),
                    "auxiliary scratch remains a non-retryable resource failure");
            require(args(failure) == "{}", "auxiliary scratch carries no report-sink reason");
            caught = true;
        }
        require(caught, "real id-pair operation must fail");
    };
    expect_auxiliary_io([&] { file.open(directory.path + "/missing"); }, ENOENT);
    file.open(directory.path);
    char byte = 'x';
    {
        FileSizeLimit limit(0);
        expect_auxiliary_io([&] { file.resize(1); }, EFBIG);
        expect_auxiliary_io([&] { file.write(0, &byte, 1); }, EFBIG);
    }
    expect_auxiliary_io([&] { file.read(0, &byte, 1); }, EIO);
}

void report_scratch_failures() {
    Directory directory;
    expect_io([&] { rs::ScratchFile file(directory.path + "/missing"); }, ENOENT, "scratch_io");
    rs::ScratchCurveWriter curve(directory.path);
    curve.append({1704067200000, 1000.0, 0.0});
    {
        FileSizeLimit limit(0);
        expect_io([&] { curve.flush(); }, EFBIG, "scratch_io");
    }
    // The failed flush retains the complete pending batch for a later retry.
    const auto region = curve.region();
    require(region.point_count == 1, "retry flush lost the original pending point");
    require(::ftruncate(scratch_fixture::descriptor(region), 0) == 0,
            "truncate real report scratch for EOF");
    pf_equity_point_t point{};
    expect_io([&] { rs::read_scratch_points(region, 0, &point, 1); }, EIO, "scratch_io");
    pineforge::metrics::SpillCurve metrics(region);
    expect_io([&] { (void)metrics[0]; }, EIO, "scratch_io");
}

void writer_origins() {
    Directory directory;
    const auto missing = directory.path + "/missing/report";
    const pf_report_sink_v1 bad{sizeof bad, 1, missing.c_str(), "W1", directory.path.c_str()};
    rs::LineWriter unopened(bad);
    expect_writer_io([&] { unopened.write_header({false, false, false, 0, 0}); },
                     ENOENT, "sink_io");
    const auto path = directory.file("file-size.ndjson");
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "W1", directory.path.c_str()};
    rs::LineWriter file(sink);
    file.write_header({false, false, false, 0, 0});
    {
        FileSizeLimit limit(0);
        expect_writer_io([&] { (void)file.write_trailer(); }, EFBIG, "sink_io");
    }
#if defined(__linux__)
    // State::uncached requires a disk-backed regular path sink. /dev/full is
    // a character device: fstat succeeds, then the type guard supplies EINVAL
    // before any write, so the old trailer ENOSPC expectation was unreachable.
    // The regular-file EFBIG case above still exercises a real sink write.
    // Mutation: delete the !S_ISREG guard in src/report_sink_writer.cpp;
    // the origin check must fail even if a later backing check gives EINVAL.
    const pf_report_sink_v1 full{sizeof full, 1, "/dev/full", "W1", directory.path.c_str()};
    rs::LineWriter device(full);
    expect_writer_io([&] { device.write_header({false, false, false, 0, 0}); },
                     EINVAL, "sink_io", "report sink requires a disk-backed regular file: ");
#endif
    Socket socket;
    const pf_report_sink_fd_v1 fd_sink{sizeof fd_sink, 1, socket.writer.value, "W1"};
    rs::LineWriter stream(fd_sink);
    stream.write_header({false, false, false, 0, 0});
    socket.close_peer();
    expect_writer_io([&] { (void)stream.write_trailer(); }, EPIPE, "sink_peer_closed");
    require(::fcntl(socket.writer.value, F_GETFD) >= 0, "writer failure closed the borrowed fd");
}

enum class Mode { quiet, callback, held_equity, selected, arena_failure };

class Host final : public pineforge::NativeStrategyHost {
public:
    Mode mode;
    Socket* socket = nullptr;
    int callbacks = 0;
    mutable int presentations = 0;
    detail::ScratchFile arena_file;

    explicit Host(Mode selected_mode = Mode::quiet) : mode(selected_mode) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"W1-real-failure", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:W1";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 1000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "configure native failure fixture");
        if (mode == Mode::callback) {
            declare_outputs(0, 1);
            require(set_outputs_enabled(true), "enable fixture callback output");
        }
    }
    void on_native_run_begin() override {
        if (mode == Mode::callback) output_run_begin();
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext& context) override {
        ++callbacks;
        if (callbacks == 1 && mode == Mode::held_equity) socket->close_peer();
        if (mode == Mode::callback) {
            socket->close_peer();
            output_bar(context.script_bar_open_ms, context.script_bar_open_ms + 60000);
            // output_event queues; output_events_clear publishes through
            // report_sink_record. The message alone exceeds the writer's
            // 64 KiB io_window (src/report_sink_writer.cpp), forcing socket I/O
            // in this callback, before the sentinel can throw.
            // Mutation: remove write_report_sink_events() from
            // BacktestEngine::output_events_clear in src/engine_report.cpp;
            // the sentinel then yields engine_unclassified_error and fails the code check.
            output_event(0, 1.0, std::string(writer_buffer + 1, 'v'));
            output_events_clear();
            throw std::runtime_error("callback write unexpectedly succeeded");
        }
        if (mode == Mode::arena_failure) {
            arena_file.set_store(detail::ScratchStore::partial_cycles_idx);
            arena_file.open("not-an-identity");
            arena_file.ensure_size(2 * chunk);
            throw std::runtime_error("oversized callback allocation unexpectedly succeeded");
        }
    }
    void present_report(pineforge::ReportC*) const override {
        ++presentations;
        if (mode == Mode::selected) socket->close_peer();
    }
};

void begin_open_translator() {
    Directory directory;
    Host host;
    install_path(host, directory.path + "/missing/report", directory.path);
    run(host, tape(2));
    require(host.callbacks == 0, "open failure did not occur during begin");
    expect_engine_failure(host, reason_args("sink_io"));
}

void begin_peer_translator() {
    Socket socket;
    Host host;
    install_fd(host, socket.writer.value, std::string(writer_buffer + 1, 'h'));
    socket.close_peer();
    run(host, tape(2));
    require(host.callbacks == 0, "header EPIPE reached a script callback");
    expect_engine_failure(host, reason_args("sink_peer_closed"));
}

void arena_backing_admission_translator() {
    // Both sink setters attach the inherited arena before run(). decimal()
    // rejects a non-digit with EINVAL/arena_backing before inspecting any fd;
    // pf_cabi_int records that typed failure on the handle and returns -1.
    // Fresh-host run status stays 0: note_failure_record only stores the refusal.
    // Mutation: remove attach_from_environment() from either sink setter;
    // its now-successful admission must fail the -1 assertion below.
    Environment invalid_fd("PF_SCRATCH_ARENA_FD", "not-a-decimal-fd");
    Directory directory;
    Socket socket;
    const auto path = directory.file("refused-before-open");
    const pf_report_sink_v1 path_sink{sizeof path_sink, 1, path.c_str(), "W1", directory.path.c_str()};
    const pf_report_sink_fd_v1 stream_sink{sizeof stream_sink, 1, socket.writer.value, "W1"};
    for (const bool fd_sink : {false, true}) {
        Host host;
        auto* handle = static_cast<pineforge::BacktestEngine*>(&host);
        require((fd_sink ? strategy_set_report_sink_fd_v1(handle, &stream_sink)
                         : strategy_set_report_sink_v1(handle, &path_sink)) == -1,
                "invalid arena environment must fail sink admission");
        require(host.last_error().find("scratch arena environment fd: ") != std::string::npos,
                "arena admission retains the environment fd failure origin");
        require(host.callbacks == 0 && host.presentations == 0 && !detail::ScratchArena::attached(),
                "backing configuration must fail before scratch and callbacks");
        require(host.last_run_status() == 0, "sink admission must preserve the unstarted run status");
        const auto expected = reason_args("arena_backing");
        expect_failure(pineforge::run_failure_value_of(host), expected);
        require(std::string(pineforge::run_failure_code_of(host)) == "report_sink_failed"
                    && std::string(pineforge::run_failure_args_of(host)) == expected,
                "admission getters preserve the same complete failure");
        pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
        require(host.report_sink_receipt_v1(&receipt) == 0 && !receipt.complete
                    && receipt.sha256_hex[0] == '\0',
                "refused sink admission must not complete a receipt");
        require(::access(path.c_str(), F_OK) == -1 && errno == ENOENT,
                "refused admission opened the sink path");
        require(::fcntl(socket.writer.value, F_GETFD) >= 0,
                "refused admission closed the borrowed fd");
    }
}

void callback_writer_translator() {
    Socket socket;
    Host host(Mode::callback);
    host.socket = &socket;
    install_fd(host, socket.writer.value);
    run(host, tape(2));
    require(host.callbacks == 1 && host.presentations == 0,
            "callback writer failure must interrupt the first body");
    expect_engine_failure(host, reason_args("sink_peer_closed"));
}

void held_equity_translator() {
    Socket socket;
    Host host(Mode::held_equity);
    host.socket = &socket;
    install_fd(host, socket.writer.value);
    const auto bars = tape(4096);
    run(host, bars);
    require(host.callbacks > 1 && host.callbacks < static_cast<int>(bars.size())
                && host.presentations == 0,
            "E buffer must fail during held-point release, before final capture");
    expect_engine_failure(host, reason_args("sink_peer_closed"));
}

void selected_presentation_translator() {
    Socket socket;
    Host host(Mode::selected);
    host.socket = &socket;
    install_fd(host, socket.writer.value);
    const auto bars = tape(4096);
    const pf_selected_window_config_v1 window{sizeof window, 1, bars.front().timestamp,
                                             bars.back().timestamp + 60000};
    require(strategy_set_selected_window_v1(static_cast<pineforge::BacktestEngine*>(&host), &window) == 0,
            "admit selected window for real presentation failure");
    run(host, bars);
    require(host.callbacks == static_cast<int>(bars.size()) && host.presentations == 1,
            "selected writer must fail after exactly one terminal presentation");
    expect_engine_failure(host, reason_args("sink_peer_closed"));
}

void finish_writer_translator(bool peer_closed) {
    Directory directory;
    Socket socket;
    Host host;
    const auto path = directory.file("finish.ndjson");
    if (peer_closed) install_fd(host, socket.writer.value);
    else install_path(host, path, directory.path);
    run(host, tape(2));
    require(host.last_run_status() == 0 && host.last_error().empty(), "finish fixture run failed early");
    pineforge::ReportC report{};
    bool caught = false;
    const auto fill = [&] {
        try {
            host.fill_report(&report);
        } catch (const std::exception& error) {
            caught = true;
            const auto expected = reason_args(peer_closed ? "sink_peer_closed" : "sink_io");
            expect_failure(pineforge::classify_run_failure(error), expected);
            // The fill translator records and rethrows the same typed value.
            expect_engine_failure(host, expected);
        }
    };
    if (peer_closed) {
        socket.close_peer();
        fill();
    } else {
        // Both curve points fit this file extent; the sink header/trailer do
        // not. Scratch succeeds first, then the real sink write fails.
        FileSizeLimit limit(sizeof(pf_equity_point_t) * 2);
        fill();
    }
    pineforge::BacktestEngine::free_report(&report);
    require(caught && report.trades == nullptr && report.equity_curve == nullptr,
            "finish failure must throw and clear its owned output arrays");
}

void arena_failure_through_run_boundary() {
    Directory directory;
    const auto path = directory.file("arena");
    Fd backing(::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
    const auto supplied = 2 * chunk + 37;
    require(backing.value >= 0 && ::ftruncate(backing.value, static_cast<off_t>(supplied)) == 0,
            "create real arena backing");
    detail::ScratchArena::attach(backing.value, supplied);
    pineforge::RunFailureValue captured;
    {
        detail::ScratchFile live, retired;
        live.set_store(detail::ScratchStore::cohort);
        live.open("live");
        live.ensure_size(chunk);
        retired.open("retired");
        retired.ensure_size(chunk);
        retired.clear(); // Peak two chunks; only one remains claimed.
        Host host(Mode::arena_failure);
        run(host, tape(2));
        const auto expected = exhaustion_args(supplied, 2 * chunk, chunk,
                                               2 * chunk, "partial_cycles_idx");
        expect_engine_failure(host, expected);
        captured = pineforge::run_failure_value_of(host);
        host.arena_file.clear();
        live.clear();
        std::uint64_t high = 0, bytes = 0;
        require(pf_report_scratch_arena_stats(&high, &bytes) == 0
                    && high == 2 * chunk && bytes == supplied,
                "getter returns simultaneous high-water after every region is released");
    }
    detail::ScratchArena::detach();
    expect_failure(captured, exhaustion_args(supplied, 2 * chunk, chunk,
                                             2 * chunk, "partial_cycles_idx"));
    std::uint64_t high = 1, bytes = 1;
    require(pf_report_scratch_arena_stats(&high, &bytes) == 0 && high == 0 && bytes == 0,
            "never-attached/detached getter reports zeros");
}

void labelled_disk_table_exhaustion() {
    Directory directory;
    const auto path = directory.file("arena");
    Fd backing(::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
    const auto supplied = chunk + 19;
    require(backing.value >= 0 && ::ftruncate(backing.value, static_cast<off_t>(supplied)) == 0,
            "create table exhaustion backing");
    detail::ScratchArena::attach(backing.value, supplied);
    {
        detail::ScratchFile blocker;
        blocker.open("blocker");
        blocker.ensure_size(chunk);
        for (const auto identity : {std::pair{detail::ScratchStore::cohort, "cohort"},
                                     std::pair{detail::ScratchStore::exit_rank, "exit_rank"}}) {
            detail::DiskIdTable<std::uint64_t> table;
            table.set_store(identity.first);
            bool caught = false;
            try { table.insert("real-node", 7); }
            catch (const detail::ReportScratchIoError& error) {
                caught = true;
                expect_failure(error.run_failure(),
                    exhaustion_args(supplied, chunk, chunk, chunk, identity.second));
            }
            require(caught, "labelled physical table must exhaust at its first real node write");
        }
    }
    detail::ScratchArena::detach();
}

} // namespace

int main() {
    try {
        Environment arena_fd("PF_SCRATCH_ARENA_FD", nullptr);
        Environment arena_bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        Environment window("PF_SCRATCH_WINDOW_BYTES", nullptr);
        require(!detail::ScratchArena::attached(), "failure tests start without an arena");
        scratch_file_failures();
        id_pair_file_failures();
        report_scratch_failures();
        writer_origins();
        begin_open_translator();
        begin_peer_translator();
        arena_backing_admission_translator();
        callback_writer_translator();
        held_equity_translator();
        selected_presentation_translator();
        finish_writer_translator(true);
        finish_writer_translator(false);
        arena_failure_through_run_boundary();
        labelled_disk_table_exhaustion();
        std::puts("report sink reasons: real I/O, writer translators and retained arena args checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_sink_failure_reasons: %s\n", error.what());
        return 1;
    }
}
