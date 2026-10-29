#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/run_budget.h>
#include "../src/report_sink_writer.hpp"
#include "../src/resource_clock.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits.h>
#include <limits>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
// One payload spans two 64 KiB writer windows. The progressing fixture
// derives its row count from the measured kernel capacity below.
constexpr std::size_t window_bytes = 64 * 1024;
constexpr std::size_t payload_bytes = 2 * window_bytes;
// Choose S=300*the documented 1 ms sampling floor, hence P=S/300=1 ms.
// Real stalls also use S: 300 samples leave a wide margin above one cadence.
constexpr std::int64_t sample_floor_ms = 1, sample_ceiling_ms = 100;
constexpr std::int64_t stall_ms = 300 * sample_floor_ms;
constexpr std::int64_t sample_ms = stall_ms / 300;
// The default witness crosses twice the former 30 s policy on virtual time.
// Retain that duration as the real harness allowance for the one-byte reader
// under instrumentation: ceil(2*former_stall_ms/1000)+1, never a sink limit.
constexpr std::int64_t former_stall_ms = 30 * 1000;
constexpr unsigned timeout_seconds = (2 * former_stall_ms + 999) / 1000 + 1;
std::int64_t wall = 0, configured_stall_ms = stall_ms;
int reader_fd = -1, wakes = 0;
int clock_reads = 0;
bool progressing = false, interrupted = false, drain_all = false, socket_reader = false;
bool close_on_wake = false, drain_once = false;
bool hold_default = false, observed_default_wait = false;
report_sink::LineWriter* waiting_writer = nullptr;
std::string consumed;

void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}

int clock_read(clockid_t kind, timespec* out) {
    ++clock_reads;
    const auto ms = kind == CLOCK_MONOTONIC ? wall : 0;
    out->tv_sec = ms / 1000;
    out->tv_nsec = (ms % 1000) * 1000000;
    return 0;
}

int poll_wait(pollfd* fds, nfds_t count, int timeout) {
    ++wakes;
    const int ready = ::poll(fds, count, 0);
    if (ready != 0) return ready;
    if (hold_default) {
        // No reads at the first blocked wait; only virtual time advances.
        wall += 2 * former_stall_ms;
        hold_default = false;
        errno = EINTR;
        return -1;
    }
    if (configured_stall_ms == 0) {
        require(timeout == -1, "off sink must retain an indefinite blocking poll");
        if (!observed_default_wait) {
            require(wall == 2 * former_stall_ms && consumed.empty()
                        && (!waiting_writer || !waiting_writer->diagnostics().complete),
                    "off sink must still be waiting before the reader drains");
            require(clock_reads == 0, "off sink must not sample resource clocks");
            observed_default_wait = true;
        }
    } else {
        const auto cadence = std::clamp(configured_stall_ms / 300,
                                       sample_floor_ms, sample_ceiling_ms);
        require(timeout > 0 && timeout <= cadence,
                "poll cadence must respect the configured interval and sampling clamps");
        if (wakes == 1)
            require(timeout == std::min(cadence, configured_stall_ms),
                    "first poll must use the derived sampling cadence");
    }
    if (timeout > 0) wall += timeout;
    if (close_on_wake) {
        require(::close(reader_fd) == 0, "close socket peer during wait");
        reader_fd = -1;
        close_on_wake = false;
        return ::poll(fds, count, 0);
    }
    if (drain_once) {
        char byte;
        require(::read(reader_fd, &byte, 1) == 1, "release one socket buffer");
        consumed += byte;
        drain_once = false;
        pollfd writable{fds[0].fd, POLLOUT, 0};
        require(::poll(&writable, 1, 0) == 0,
                "one freed socket buffer must remain below POLLOUT");
    }
    if (progressing) {
        if (drain_all || socket_reader) {
            // AF_UNIX accounts buffer allocations, so a partial buffer read
            // need not make a send possible. Release up to one PIPE_BUF per
            // slow wake; the writer must retry send even without POLLOUT.
            std::array<char, window_bytes> bytes{};
            const auto count_read = ::read(reader_fd, bytes.data(),
                                           drain_all ? bytes.size() : PIPE_BUF);
            require(count_read > 0, "fast reader consumes pending bytes");
            consumed.append(bytes.data(), static_cast<std::size_t>(count_read));
        } else {
            // ONE byte, below PIPE_BUF and POLLOUT's free-space threshold.
            // Pipe queue depth must reset the stall timer on every wake.
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
        clock_reads = 0; configured_stall_ms = stall_ms;
        progressing = slow; interrupted = signals; consumed.clear();
        drain_all = fast;
        socket_reader = close_on_wake = drain_once = false;
        hold_default = observed_default_wait = false; waiting_writer = nullptr;
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
    explicit Pipe(bool nonblocking = true, bool socket = false, bool byte_buffers = false) {
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
            const auto written = ::write(write_fd, bytes.data(), byte_buffers ? 1 : bytes.size());
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

// Keep new rows compilable against the old public writer declaration, so an
// absent configuration method fails at runtime rather than at setup.
template<class Writer>
auto configure_stall_impl(Writer& writer, std::int64_t interval, int)
    -> decltype(writer.set_sink_stall_ms(std::uint64_t{}), void()) {
    writer.set_sink_stall_ms(static_cast<std::uint64_t>(interval));
    configured_stall_ms = interval;
}
template<class Writer>
void configure_stall_impl(Writer&, std::int64_t, ...) {
    require(false, "writer stall configuration is absent");
}
void configure_stall(report_sink::LineWriter& writer, std::int64_t interval) {
    configure_stall_impl(writer, interval, 0);
}

void emit(report_sink::LineWriter& writer, std::size_t rows = 1) {
    writer.write_header(records);
    const pf_trace_entry_t trace{0, 0, 0, 1.0};
    const std::string payload(payload_bytes, 'x');
    for (std::size_t i = 0; i < rows; ++i) writer.write_x(trace, payload);
    writer.write_trailer();
}

void blocked_row(bool nonblocking, bool signals, bool socket, bool real_time,
                 std::int64_t interval = stall_ms) {
    Scope scope(false, signals);
    if (real_time) {
        resource_clock::read_clock = nullptr;
        resource_clock::wait_poll = nullptr;
    }
    Pipe pipe(nonblocking, socket);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "stall-test"};
    {
        report_sink::LineWriter writer(sink);
        configure_stall(writer, interval);
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
        require(wall >= interval && wall <= 2 * interval,
                "stall expiry must be bounded even with repeated EINTR");
    if (!real_time)
        require(wall == interval, "stall expiry must honor the exact configured interval");
}

void socket_closed_row(bool during_wait) {
    Scope scope(false, false);
    Pipe pipe(false, true);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "stall-test"};
    if (during_wait) {
        close_on_wake = true;
    } else {
        require(::close(pipe.read_fd) == 0, "close socket peer before write");
        reader_fd = -1;
    }
    // The poll hook owns the peer close in the other case.
    pipe.read_fd = -1;
    report_sink::LineWriter writer(sink);
    configure_stall(writer, stall_ms);
    bool stopped = false;
    try { emit(writer); }
    catch (const report_sink::WriterError& error) {
        stopped = true;
        const auto failure = classify_run_failure(error);
        require(error.error_number() == EPIPE || error.error_number() == ECONNRESET,
                "closed socket must retain peer-closed errno");
        require(failure.code == RunFailureCode::report_sink_failed && failure.args
                    && *failure.args == "{\"reason\":\"sink_peer_closed\"}",
                "closed socket must retain sink_peer_closed, never sink_stalled");
    }
    require(stopped && !writer.diagnostics().complete,
            "closed socket must fail without success diagnostics");
    require(!close_on_wake && wall < stall_ms,
            "closed socket must stop before the stall deadline");
    require(::fcntl(pipe.write_fd, F_GETFL) == pipe.flags,
            "closed socket cleanup must preserve borrowed descriptor flags");
}

void socket_short_send_row() {
    Scope scope(false, false);
    // Linux AF_UNIX charges each send's buffer allocation. Fill with one-byte
    // sends so reading one byte releases one allocation, less than POLLOUT's
    // free-space threshold. A nonblocking send can nevertheless accept bytes.
    Pipe pipe(false, true, true);
    drain_once = true;
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "stall-test"};
    {
        report_sink::LineWriter writer(sink);
        configure_stall(writer, stall_ms);
        bool stopped = false;
        try { emit(writer); }
        catch (const report_sink::WriterError& error) {
            const auto failure = classify_run_failure(error);
            stopped = error.error_number() == ETIMEDOUT
                && failure.code == RunFailureCode::report_sink_failed && failure.args
                && *failure.args == "{\"reason\":\"sink_stalled\"}";
        }
        require(stopped && !writer.diagnostics().complete,
                "socket must stall when no more bytes can be accepted");
    }
    require(wall == stall_ms + sample_ms,
            "accepted socket bytes must reset the deadline at the first wake");
    const auto wire = pipe.finish();
    require(wire.size() > pipe.capacity
                && wire.find("{\"schema\":\"pf-report-sink\"") != std::string::npos,
            "socket must accept report bytes below POLLOUT before the peer stalls");
    require(wire.find("\"trailer\"") == std::string::npos,
            "partial socket progress must not publish a success trailer");
}

std::string progressing_wire(bool fast, bool socket = false) {
    Scope scope(true, false, fast);
    Pipe pipe(true, socket);
    socket_reader = socket;
    pipe.empty();
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "stall-test"};
    {
        report_sink::LineWriter writer(sink);
        configure_stall(writer, stall_ms);
        // A socket slow wake drains at most PIPE_BUF bytes. Force more than
        // 2*S/P wakes by sending > C + (2*S/P)*PIPE_BUF + one payload:
        // rows=floor((C + (2*S/P)*PIPE_BUF)/payload_bytes)+2. Pipe wakes drain
        // one byte and only need the original C + one payload geometry.
        const auto progress_bytes = socket ? (2 * stall_ms / sample_ms) * PIPE_BUF : 0;
        emit(writer, (pipe.capacity + progress_bytes) / payload_bytes + 2);
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

std::string default_wire(bool fast, int registration = 0, bool socket = false) {
    Scope scope(true, false, true);
    configured_stall_ms = 0;
    hold_default = true;
    Pipe pipe(true, socket);
    pipe.empty();
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "default-test"};
    {
        report_sink::LineWriter writer(sink);
        waiting_writer = &writer;
        if (registration == 1) configure_stall(writer, 0);
        if (registration == 2) {
            configure_stall(writer, stall_ms);
            configure_stall(writer, 0);
        }
        configured_stall_ms = 0;
        // The fast reference uses the same default, without a simulated hold.
        if (fast) {
            hold_default = false;
            observed_default_wait = true;
        }
        // As in the progress fixture, N=floor(C/payload_bytes)+2 forces
        // backpressure even when a platform has unusually large buffers.
        try { emit(writer, pipe.capacity / payload_bytes + 2); }
        catch (const report_sink::WriterError& error) {
            const auto failure = classify_run_failure(error);
            require(!(failure.code == RunFailureCode::report_sink_failed && failure.args
                        && *failure.args == "{\"reason\":\"sink_stalled\"}"),
                    "off sink must still wait after twice the former deadline");
            throw;
        }
        require(writer.diagnostics().complete, "off sink must complete after its reader resumes");
        require(observed_default_wait && !hold_default && clock_reads == 0,
                "default sink must wait without a timer before completing");
    }
    require(::fcntl(pipe.write_fd, F_GETFL) == pipe.flags,
            "default sink must preserve borrowed flags");
    return pipe.finish();
}

void default_row(bool socket = false) {
    const auto actual = default_wire(false, 0, socket);
    const auto expected = default_wire(true, 0, socket);
    require(actual == expected && actual.find("\"trailer\"") != std::string::npos,
            "default stalled-reader wire must equal the complete fast-reader wire");
    require(default_wire(false, 1, socket) == expected && default_wire(false, 2, socket) == expected,
            "zero and removed sink intervals must preserve the complete default wire");
}

template<class T, class = void> struct BudgetStorage : T { std::uint64_t sink_stall_ms; };
template<class T> struct BudgetStorage<T, std::void_t<decltype(T::sink_stall_ms)>> : T {};
using Budget = BudgetStorage<pf_run_budget_v1>;

struct Host final : NativeStrategyHost {
    int callbacks = 0;
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
        ++callbacks;
        require(strategy_set_run_budget_v1(static_cast<BacktestEngine*>(this), nullptr) == -3,
                "active run must preserve its sink interval");
        trace(std::string(payload_bytes, 'x'), 1);
    }
};

std::string engine_default_wire(bool fast, int registration) {
    Scope scope(true, false, true);
    configured_stall_ms = 0;
    hold_default = !fast; observed_default_wait = fast;
    Pipe pipe(true);
    pipe.empty();
    Host host;
    auto handle = static_cast<BacktestEngine*>(&host);
    Budget budget{};
    budget.struct_size = sizeof budget; budget.version = 1;
    if (registration) {
        if (registration == 2) budget.sink_stall_ms = stall_ms;
        require(strategy_set_run_budget_v1(handle, &budget) == 0, "install all-zero host budget");
        if (registration == 2)
            require(strategy_set_run_budget_v1(handle, nullptr) == 0, "remove all host limits");
    }
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, "default-engine-test"};
    require(strategy_set_report_sink_fd_v1(handle, &sink) == 0, "install default engine sink");
    // N=floor(C/payload_bytes)+2 makes trace output exceed measured capacity
    // by at least one payload, forcing a blocked wait on every platform.
    const auto count = pipe.capacity / payload_bytes + 2;
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < count; ++i)
        bars.push_back({100, 101, 99, 100, 1,
                        INT64_C(1704067200000) + static_cast<std::int64_t>(i) * 60000});
    ReportC report{};
    try {
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                 MagnifierDistribution::ENDPOINTS);
        host.fill_report(&report);
    } catch (const std::exception& error) {
        BacktestEngine::free_report(&report);
        const auto failure = classify_run_failure(error);
        require(!(failure.code == RunFailureCode::report_sink_failed && failure.args
                    && *failure.args == "{\"reason\":\"sink_stalled\"}"),
                "default host sink must wait beyond the former deadline");
        throw;
    }
    BacktestEngine::free_report(&report);
    require(host.last_run_status() == 0,
            "default host sink must wait beyond the former deadline");
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(strategy_report_sink_receipt_v1(handle, &receipt) == 0 && receipt.complete
                && observed_default_wait && !hold_default && clock_reads == 0,
            "absent, zero and removed host limits must complete without resource clock reads");
    require(::fcntl(pipe.write_fd, F_GETFL) == pipe.flags,
            "default engine sink must preserve borrowed flags");
    return pipe.finish();
}

void engine_default_row() {
    const auto actual = engine_default_wire(false, 0);
    const auto expected = engine_default_wire(true, 0);
    require(actual == expected && actual.find("\"trailer\"") != std::string::npos,
            "default engine wire must equal the complete fast-reader wire");
    require(engine_default_wire(false, 1) == expected && engine_default_wire(false, 2) == expected,
            "zero and removed host budgets must preserve the complete default wire");
}

void engine_row(bool socket = false, const char* rejection = nullptr,
                std::int64_t interval = stall_ms, bool large_header = false) {
    Scope scope(false, false);
    Pipe pipe(true, socket);
    Host host;
    auto handle = static_cast<BacktestEngine*>(&host);
    Budget budget{};
    budget.struct_size = sizeof budget; budget.version = 1;
    budget.sink_stall_ms = interval;
    require(strategy_set_run_budget_v1(handle, &budget) == 0, "install host-selected sink interval");
    configured_stall_ms = interval;
    if (rejection) {
        Budget bad{};
        bad.struct_size = sizeof bad; bad.version = 1;
        const std::string reason = rejection;
        if (reason == "version") ++bad.version;
        else if (reason == "size") --bad.struct_size;
        else if (reason == "old_size") bad.struct_size = 2 * sizeof(std::uint32_t)
                                                          + 2 * sizeof(std::uint64_t);
        else if (reason == "large_size") ++bad.struct_size;
        else if (reason == "wall_range") bad.wall_ms = std::numeric_limits<std::uint64_t>::max();
        else if (reason == "cpu_range") bad.cpu_ms = std::numeric_limits<std::uint64_t>::max();
        else if (reason == "sink_range") bad.sink_stall_ms = std::numeric_limits<std::uint64_t>::max();
        else throw std::runtime_error("unknown descriptor rejection");
        require(strategy_set_run_budget_v1(handle, &bad) == -1,
                "invalid descriptor must be refused before replacing the sink interval");
    }
    // Configuration is copied; the caller's object cannot remove the limit.
    budget.sink_stall_ms = 0;
    // A run id of one writer window, plus the fixed schema fields, forces
    // a header flush before any callback while remaining below the line cap.
    const auto run_id = large_header ? std::string(window_bytes, 'r') : std::string("stall-test");
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.write_fd, run_id.c_str()};
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
    require(wall == interval,
            "host sink interval must be copied and retained after descriptor refusal");
    if (large_header)
        require(host.callbacks == 0, "sink interval must apply before the header flush");
}

void sink_setter_row() {
    for (const auto* reason : {"version", "size", "old_size", "large_size",
                               "wall_range", "cpu_range", "sink_range"})
        engine_row(false, reason);
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
        else if (row == "socket_nonblocking") blocked_row(true, false, true, false);
        else if (row == "socket_signals") blocked_row(false, true, true, false);
        else if (row == "socket_closed") socket_closed_row(false);
        else if (row == "socket_close_wait") socket_closed_row(true);
        else if (row == "socket_short_send") socket_short_send_row();
        else if (row == "real_blocked") blocked_row(true, false, false, true);
        else if (row == "real_socket") blocked_row(false, false, true, true);
        else if (row == "slow") slow_row();
        else if (row == "socket_slow") slow_row(true);
        else if (row == "engine") engine_row();
        else if (row == "engine_socket") engine_row(true);
        else if (row == "default") default_row();
        else if (row == "default_socket") default_row(true);
        else if (row == "engine_default") engine_default_row();
        else if (row == "begin_sink") engine_row(false, nullptr, stall_ms, true);
        // S=2*(300*1 ms)+1 ms: P=2 ms, with a final 1 ms wait. This pins
        // arbitrary intervals and the remaining-time cap, not a round policy.
        else if (row == "configured") engine_row(false, nullptr, 2 * stall_ms + sample_floor_ms);
        else if (row == "setter") sink_setter_row();
        // At S=1 ms, floor(S/300)=0, so the 1 ms clamp is essential.
        else if (row == "cadence_floor")
            blocked_row(true, false, false, false, sample_floor_ms);
        // S=2*300*100+1 ms gives a raw 200 ms cadence, capped to 100 ms;
        // the final 1 ms also witnesses the remaining-time cap.
        else if (row == "cadence_ceiling")
            blocked_row(true, false, false, false, 2 * 300 * sample_ceiling_ms + sample_floor_ms);
        else throw std::runtime_error("unknown sink stall row");
        ::alarm(0);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
