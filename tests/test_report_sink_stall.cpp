#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include "../src/report_sink_writer.hpp"
#include "../src/resource_clock.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace pineforge;
// One payload spans two 64 KiB writer windows. The progressing fixture
// derives its row count from the measured kernel capacity below.
constexpr std::size_t window_bytes = 64 * 1024;
constexpr std::size_t payload_bytes = 2 * window_bytes;
// The operational stall policy is 30 s; the injected rows require no real
// wait. A real blocked row gets 2*30 s + 1 s setup, even under instrumentation.
constexpr std::int64_t stall_ms = 30 * 1000;
constexpr unsigned timeout_seconds = (2 * stall_ms + 999) / 1000 + 1;
std::int64_t wall = 0;
int reader_fd = -1, wakes = 0;
bool progressing = false, interrupted = false, drain_all = false;
std::string consumed;

void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}

int clock_read(clockid_t kind, timespec* out) {
    const auto ms = kind == CLOCK_MONOTONIC ? wall : 0;
    out->tv_sec = ms / 1000;
    out->tv_nsec = (ms % 1000) * 1000000;
    return 0;
}

int poll_wait(pollfd* fds, nfds_t count, int timeout) {
    require(timeout > 0 && timeout <= stall_ms, "poll must have a finite stall timeout");
    ++wakes;
    const int ready = ::poll(fds, count, 0);
    if (ready != 0) return ready;
    wall += timeout;
    if (progressing) {
        // Consume exactly ONE byte, below PIPE_BUF and POLLOUT's free-space
        // threshold, on every wake. Queue depth must reset the stall timer.
        if (drain_all) {
            std::array<char, window_bytes> bytes{};
            const auto count_read = ::read(reader_fd, bytes.data(), bytes.size());
            require(count_read > 0, "fast reader consumes pending bytes");
            consumed.append(bytes.data(), static_cast<std::size_t>(count_read));
        } else {
            char byte;
            require(::read(reader_fd, &byte, 1) == 1, "slow reader consumes a byte");
            consumed += byte;
        }
    }
    if (interrupted) { errno = EINTR; return -1; }
    return 0;
}

struct Scope {
    Scope(bool slow, bool signals, bool fast = false) {
        wall = wakes = 0;
        progressing = slow; interrupted = signals; consumed.clear();
        drain_all = fast;
        resource_clock::read_clock = clock_read;
        resource_clock::wait_poll = poll_wait;
    }
    ~Scope() {
        resource_clock::read_clock = nullptr;
        resource_clock::wait_poll = nullptr;
    }
};

struct Pipe {
    int read_fd = -1, write_fd = -1, flags = 0;
    std::size_t capacity = 0;
    explicit Pipe(bool nonblocking = true, bool socket = false) {
        int fds[2];
        require((socket ? ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) : ::pipe(fds)) == 0,
                "create sink transport");
        read_fd = fds[0]; write_fd = fds[1]; reader_fd = read_fd;
        const int initial = ::fcntl(write_fd, F_GETFL);
        require(initial >= 0 && ::fcntl(write_fd, F_SETFL, initial | O_NONBLOCK) == 0,
                "prepare full sink transport");
        if (socket) {
            const int send_capacity = PIPE_BUF;
            require(::setsockopt(write_fd, SOL_SOCKET, SO_SNDBUF, &send_capacity, sizeof send_capacity) == 0,
                    "bound socket buffering");
        }
        // Discover and fill the actual kernel capacity, rather than assume a
        // platform-specific pipe/socket size.
        std::array<char, PIPE_BUF> bytes{};
        for (;;) {
            const auto written = ::write(write_fd, bytes.data(), bytes.size());
            if (written > 0) { capacity += static_cast<std::size_t>(written); continue; }
            if (written < 0 && errno == EINTR) continue;
            require(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
                    "fill sink through actual backpressure");
            break;
        }
        require(::fcntl(write_fd, F_SETFL, nonblocking ? initial | O_NONBLOCK : initial) == 0,
                "choose supplied sink flags");
        flags = ::fcntl(write_fd, F_GETFL);
    }
    ~Pipe() {
        if (read_fd >= 0) ::close(read_fd);
        if (write_fd >= 0) ::close(write_fd);
    }
    void empty() {
        int pending = 0;
        require(::ioctl(read_fd, FIONREAD, &pending) == 0, "measure fixture queue");
        std::array<char, window_bytes> buffer{};
        while (pending > 0) {
            const auto count = ::read(read_fd, buffer.data(),
                                     std::min<std::size_t>(pending, buffer.size()));
            require(count > 0, "empty fixture prefix");
            pending -= static_cast<int>(count);
        }
    }
    std::string finish() {
        ::close(write_fd); write_fd = -1;
        std::string result = consumed;
        std::array<char, window_bytes> buffer{};
        for (;;) {
            const auto count = ::read(read_fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            require(count >= 0, "drain sink output");
            if (!count) return result;
            result.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
};

const report_sink::RecordSet records{false, true, false, 0, 0};

void emit(report_sink::LineWriter& writer, std::size_t rows = 1) {
    writer.write_header(records);
    const pf_trace_entry_t trace{0, 0, 0, 1.0};
    const std::string payload(payload_bytes, 'x');
    for (std::size_t i = 0; i < rows; ++i) writer.write_x(trace, payload);
    writer.write_trailer();
}

void blocked_row(bool nonblocking, bool signals, bool socket, bool real_time) {
    Scope scope(false, signals);
    if (real_time) {
        resource_clock::read_clock = nullptr;
        resource_clock::wait_poll = nullptr;
    }
    Pipe pipe(nonblocking, socket);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "stall-test"};
    {
        report_sink::LineWriter writer(sink);
        bool stopped = false;
        try { emit(writer); }
        catch (const report_sink::WriterError& error) {
            stopped = true;
            require(error.error_number() == ETIMEDOUT, "stall retains timeout errno");
            const auto failure = classify_run_failure(error);
            require(failure.code == RunFailureCode::report_sink_failed && failure.args
                        && *failure.args == "{\"reason\":\"sink_stalled\"}",
                    "blocked reader must produce sink_stalled");
        }
        require(stopped, "blocked reader must stop within the stall budget");
        require(writer.diagnostics().complete == 0, "stalled writer must have no success diagnostics");
        bool refused = false;
        try { writer.write_trailer(); }
        catch (const std::exception&) { refused = true; }
        require(refused, "stalled writer cannot publish a later success trailer");
    }
    require(::fcntl(pipe.write_fd, F_GETFL) == pipe.flags,
            "stall cleanup must preserve the borrowed descriptor and flags");
    if (!real_time)
        require(wall >= stall_ms && wall <= 2 * stall_ms,
                "stall expiry must be bounded even with repeated EINTR");
}

std::string progressing_wire(bool fast, bool socket = false) {
    Scope scope(true, false, fast);
    Pipe pipe(true, socket);
    pipe.empty();
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "stall-test"};
    {
        report_sink::LineWriter writer(sink);
        // Report payload > actual pipe capacity + one full payload, on every
        // kernel: rows=floor(capacity/payload_bytes)+2. Each row stays below
        // the independent 1 MiB line limit.
        emit(writer, pipe.capacity / payload_bytes + 2);
        require(writer.diagnostics().complete == 1, "progressing reader must complete");
    }
    require(fast || (wall > 2 * stall_ms && wakes > 2),
            "slow-reader witness must exceed a total-time stall mutant");
    const auto wire = pipe.finish();
    require(wire.find("\"trailer\"") != std::string::npos
                && wire.find(std::string(payload_bytes, 'x')) != std::string::npos,
            "large progressing report must retain payload and success trailer");
    return wire;
}

void slow_row(bool socket = false) {
    const auto expected = progressing_wire(true, socket);
    require(progressing_wire(false, socket) == expected,
            "slow-reader payload, counts and digest must equal the fast reader byte for byte");
}

struct Host final : NativeStrategyHost {
    Host() {
        NativeRunSpec spec;
        spec.identity = {"sink-budget-test", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:SINK";
        spec.timezone = "UTC"; spec.session = "24x7";
        spec.initial_capital = 10000;
        spec.point_value = spec.account_fx = 1; spec.price_tick = 0.01;
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == NativeSetupStatus::Applied, "configure sink host");
        set_trace_enabled(true);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        trace(std::string(payload_bytes, 'x'), 1);
    }
};

void engine_row() {
    Scope scope(false, false);
    Pipe pipe;
    Host host;
    auto handle = static_cast<BacktestEngine*>(&host);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "stall-test"};
    require(strategy_set_report_sink_fd_v1(handle, &sink) == 0, "install blocked engine sink");
    const Bar bar{100, 101, 99, 100, 1, INT64_C(1704067200000)};
    try {
        host.run(&bar, 1, "1", "1", false, 4, MagnifierDistribution::ENDPOINTS);
        ReportC report{};
        try { host.fill_report(&report); }
        catch (...) { BacktestEngine::free_report(&report); throw; }
        BacktestEngine::free_report(&report);
    } catch (const std::exception&) {
        // Both record-time and finalization-time failures share the latch.
    }
    require(host.last_run_status() == 1
                && std::string(strategy_get_last_error_code(handle)) == "report_sink_failed"
                && std::string(strategy_get_last_error_args(handle)) == "{\"reason\":\"sink_stalled\"}",
            "engine must retain typed sink_stalled at its public boundary");
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    pf_report_sink_summary_v1 summary{sizeof summary, 1};
    require(strategy_report_sink_receipt_v1(handle, &receipt) == 0 && receipt.complete == 0
                && receipt.sha256_hex[0] == '\0'
                && strategy_report_sink_summary_v1(handle, &summary) == 0 && summary.complete == 0,
            "stalled engine must publish neither receipt nor summary success");
    ReportC report{};
    host.fill_report(&report);
    require(report.total_trades == 0 && !report.trades && !report.equity_curve
                && !report.trace && report.equity_curve_len == 0,
            "stalled engine report must not expose partial success state");
    BacktestEngine::free_report(&report);
    require(::fcntl(pipe.write_fd, F_GETFL) == pipe.flags,
            "engine cleanup must preserve borrowed sink ownership");
}

void timed_out(int) {
    constexpr char message[] = "sink did not stop within its derived timeout\n";
    const auto ignored = ::write(STDERR_FILENO, message, sizeof message - 1);
    (void)ignored;
    ::_exit(124);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(::signal(SIGALRM, timed_out) != SIG_ERR, "install sink test timeout");
        ::alarm(timeout_seconds);
        const std::string row = argc > 1 ? argv[1] : "blocked";
        if (row == "blocked") blocked_row(true, false, false, false);
        else if (row == "blocking") blocked_row(false, false, false, false);
        else if (row == "signals") blocked_row(true, true, false, false);
        else if (row == "socket") blocked_row(false, false, true, false);
        else if (row == "real_blocked") blocked_row(true, false, false, true);
        else if (row == "slow") slow_row();
        else if (row == "socket_slow") slow_row(true);
        else if (row == "engine") engine_row();
        else throw std::runtime_error("unknown sink stall row");
        ::alarm(0);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
