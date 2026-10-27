// RED-FIRST: frozen headers lack WriterFailure::stalled. Runtime red: a
// blocked reader never returns; the parent deadline makes that a failure.
#include "report_sink_failure_fixture.hpp"
#include "../src/report_sink_writer.hpp"

#include <chrono>
#include <thread>
#include <sys/wait.h>

namespace {
using namespace cr150_w1;
namespace rs = pineforge::report_sink;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void transfer(bool socket, bool nonblocking, bool drain, bool unlimited = false) {
    Environment budget("PF_REPORT_SINK_STALL_MS", unlimited ? "0" : "50");
    int pair[2];
    require((socket ? ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) : ::pipe(pair)) == 0,
            "create a real stream");
    Fd reader(pair[0]), writer(pair[1]);
    if (socket) {
        const int size = 4096;
        require(::setsockopt(writer.value, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) == 0,
                "bound socket buffering");
    }
    if (nonblocking)
        require(::fcntl(writer.value, F_SETFL, O_NONBLOCK) == 0, "set nonblocking writer");
    const int flags = ::fcntl(writer.value, F_GETFL);
    std::string received, reader_error;
    std::thread consumer;
    if (drain) {
        consumer = std::thread([&] {
            if (unlimited) std::this_thread::sleep_for(150ms);
            char bytes[4096];
            for (;;) {
                const auto count = ::read(reader.value, bytes, sizeof bytes);
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) { reader_error = "reader syscall failed"; break; }
                if (!count) break;
                received.append(bytes, static_cast<std::size_t>(count));
                std::this_thread::sleep_for(2ms);
            }
        });
    }
    bool stalled = false;
    bool writer_failed = false;
    pineforge::RunFailureValue failure;
    const auto start = Clock::now();
    try {
        pf_report_sink_fd_v1 sink{sizeof sink, 1, writer.value, "stall-control"};
        rs::LineWriter output(sink);
        output.write_header({false, true, false, 0, 0});
        pf_trace_entry_t entry{};
        entry.timestamp = 60000;
        entry.bar_index = 1;
        entry.value = 17.0;
        // Exceeds both the actual stream capacity and the writer's 64 KiB
        // buffer. No reader at all in the stalled row.
        output.write_x(entry, std::string(512 * 1024, 'x'));
        output.write_trailer();
    } catch (const rs::WriterError& error) {
        writer_failed = true;
        stalled = error.reason() == rs::WriterFailure::stalled;
        failure = error.run_failure();
    } catch (...) {
        writer.reset();
        if (consumer.joinable()) consumer.join();
        throw;
    }
    const auto elapsed = Clock::now() - start;
    const bool flags_preserved = ::fcntl(writer.value, F_GETFL) == flags;
    writer.reset();
    if (consumer.joinable()) consumer.join();
    require(flags_preserved, "restore the caller's descriptor flags on success and failure");
    require(reader_error.empty(), reader_error);
    require(stalled == !drain, "stall iff the reader makes no progress");
    require(writer_failed == !drain, "a draining reader must not fail the writer");
    if (!drain) {
        expect_failure(failure, "{\"reason\":\"sink_stalled\"}");
        require(elapsed >= 50ms && elapsed < 5s, "stall returns within a bounded wall interval");
    } else {
        require(elapsed > 50ms, "control transfer exceeds the total stall budget");
        require(received.find(std::string(512 * 1024, 'x')) != std::string::npos,
                "slow reader received the complete large record");
        require(received.find("\"trailer\":") != std::string::npos,
                "steady progress permits a complete trailer");
    }
}

// An implementation regressing to blocking write/poll(-1) must fail the row
// rather than hang the suite. This is a test watchdog, not engine enforcement.
void bounded(bool socket, bool nonblocking, bool drain, bool unlimited = false) {
    const auto child = ::fork();
    require(child >= 0, "fork bounded stream row");
    if (child == 0) {
        try { transfer(socket, nonblocking, drain, unlimited); ::_exit(0); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "stall child: %s\n", error.what());
            ::_exit(1);
        }
    }
    const auto deadline = Clock::now() + 10s;
    int status = 0;
    for (;;) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno == EINTR) continue;
        require(result == 0, "wait for stream row");
        if (Clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            require(false, "stream row exceeded watchdog: writer did not return");
        }
        std::this_thread::sleep_for(5ms);
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "stream child failed");
}
} // namespace

int main() {
    try {
        for (bool socket : {false, true})
            for (bool nonblocking : {false, true}) {
                bounded(socket, nonblocking, false);
                bounded(socket, nonblocking, true);
            }
        bounded(true, false, true, true); // explicit zero permits a >50 ms stall
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_sink_stall_budget: %s\n", error.what());
        return 1;
    }
}
