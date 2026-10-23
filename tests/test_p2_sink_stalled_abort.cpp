// B2-11: SA_RESTART must not make a full borrowed FIFO uninterruptible.
#include "p2_sink_writer_fixture.hpp"
#include "run_sigterm_abort_fixture.hpp"

#include <atomic>
#include <poll.h>
#include <pthread.h>

namespace {
int observed_fd = -1;
std::atomic<bool> entered_write{false};
}

ssize_t p2_sink_write(int fd, const void* bytes, std::size_t size) {
    if (fd == observed_fd) entered_write.store(true, std::memory_order_release);
    return ::write(fd, bytes, size);
}

#define write p2_sink_write
#include "../src/report_sink_writer.cpp"
#undef write

namespace {
void stalled_fifo(bool nonblocking) {
    p2_sink::Directory directory;
    p2_sink::require(::setenv("TMPDIR", directory.path.c_str(), 1) == 0,
                     "keep native scratch under scratch-s4e");
    int pair[2]{-1, -1};
    p2_sink::require(::pipe(pair) == 0, "create full FIFO");
    p2_sink::Fd reader(pair[0]), output(pair[1]);
    const int initial = ::fcntl(output.value, F_GETFL);
    p2_sink::require(initial >= 0
                         && ::fcntl(output.value, F_SETFL, initial | O_NONBLOCK) == 0,
                     "make FIFO nonblocking for deterministic fill");
    std::array<char, 4096> fill{};
    fill.fill('\n');
    std::size_t filled = 0;
    for (;;) {
        const auto count = ::write(output.value, fill.data(), fill.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        p2_sink::require(count > 0, "fill FIFO");
        filled += static_cast<std::size_t>(count);
    }
    // Fill any sub-PIPE_BUF remainder as well; no reader drains this pipe.
    for (;;) {
        const auto count = ::write(output.value, "\n", 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        p2_sink::require(count == 1, "fill FIFO remainder");
        ++filled;
    }
    p2_sink::require(filled > 0, "FIFO was not filled");
    const int expected_flags = nonblocking ? initial | O_NONBLOCK : initial & ~O_NONBLOCK;
    p2_sink::require(::fcntl(output.value, F_SETFL, expected_flags) == 0,
                     "select caller FIFO mode");
    p2_sink::require(pf_run_abort_install_sigterm_v1() == 0, "install real SIGTERM ingress");
    struct sigaction action{};
    p2_sink::require(::sigaction(SIGTERM, nullptr, &action) == 0
                         && (action.sa_flags & SA_RESTART) != 0,
                     "fixture did not install the production SA_RESTART handler");

    q7_fixture::Host host;
    host.configure("1", "1");
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, output.value, "B2-11"};
    p2_sink::require(host.set_report_sink_fd_v1(&sink) == 0, "install stalled native sink");
    observed_fd = output.value;
    entered_write.store(false, std::memory_order_relaxed);
    const auto target = ::pthread_self();
    std::atomic<int> delivery{-1};
    std::thread signaler([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!entered_write.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // The signal is sent after entering the writer's syscall, while the
        // already-full FIFO still cannot accept even one byte.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        delivery.store(::pthread_kill(target, SIGTERM), std::memory_order_release);
    });
    const pineforge::Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067200000)},
        {100.0, 101.0, 99.0, 100.0, 1.0, INT64_C(1704067260000)},
    };
    bool unrelated_failure = false;
    try {
        // Q7's 70,000-byte trace forces the real run through a 64 KiB sink
        // flush in its first callback, before the stalled reader can drain.
        host.run(bars, 2);
    } catch (const std::exception& error) {
        unrelated_failure = pineforge::classify_run_failure(error).code
            != pineforge::RunFailureCode::run_aborted;
    }
    signaler.join();
    observed_fd = -1;
    const bool stop_requested = pf_run_abort_requested_v1() == 1;
    const int restored = pf_run_abort_restore_sigterm_v1();
    p2_sink::require(!unrelated_failure, "native run threw an unrelated failure");
    p2_sink::require(delivery.load(std::memory_order_acquire) == 0 && stop_requested
                         && restored == 0, "real SIGTERM was not observed/restored");
    p2_sink::require(entered_write.load(std::memory_order_acquire)
                         && host.last_run_status() == 1
                         && std::string(pineforge::run_failure_code_of(host)) == "run_aborted",
                     "stalled write did not end with typed run_aborted");
    p2_sink::require(::fcntl(output.value, F_GETFL) == expected_flags
                         && ::fcntl(output.value, F_GETFD) >= 0,
                     "abort changed the borrowed FIFO's flags or ownership");
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    p2_sink::require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 0
                         && receipt.sha256_hex[0] == '\0', "abort completed the engine receipt");

    const int reader_flags = ::fcntl(reader.value, F_GETFL);
    p2_sink::require(reader_flags >= 0
                         && ::fcntl(reader.value, F_SETFL, reader_flags | O_NONBLOCK) == 0,
                     "inspect full FIFO after abort");
    std::string bytes;
    for (;;) {
        const auto count = ::read(reader.value, fill.data(), fill.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        p2_sink::require(count > 0, "drain FIFO evidence");
        bytes.append(fill.data(), static_cast<std::size_t>(count));
    }
    p2_sink::require(bytes == std::string(filled, '\n'),
                     "abort appended partial report bytes to the full FIFO");
}
}

int main() {
    return p2_sink::test_main("B2-11 stalled SIGTERM", [] {
        p2_sink::bounded_child("B2-11 blocking FIFO", [] { stalled_fifo(false); });
        p2_sink::bounded_child("B2-11 nonblocking FIFO", [] { stalled_fifo(true); });
    });
}
