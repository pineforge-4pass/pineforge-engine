// Bounded transport only. No parser, strategy, publication or growing queue.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string_view>
#include <time.h>
#include <unistd.h>

namespace {
constexpr std::size_t slab_bytes = 65536;
std::uint64_t now() {
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return std::uint64_t(ts.tv_sec) * 1000000000 + std::uint64_t(ts.tv_nsec);
}
bool nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool write_all(int fd, const char* data, std::size_t size) {
    while (size) {
        const auto n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n; size -= static_cast<std::size_t>(n);
    }
    return true;
}
// Encode-only oracle comparison: replay the real producer's file through the
// identical relay/consumers. Never used for a backtest or reported as an engine.
int replay(int argc, char** argv) {
    if (argc != 8 || std::string_view(argv[2]) != "--input"
        || std::string_view(argv[4]) != "--summary" || std::string_view(argv[6]) != "--sink-fd") return 2;
    int sink = -1;
    const auto text = std::string_view(argv[7]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), sink);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || sink < 3) return 2;
    std::array<char, slab_bytes> buffer{};
    for (unsigned pass = 0; pass < 2; ++pass) {
        const int source = ::open(argv[pass ? 5 : 3], O_RDONLY | O_CLOEXEC);
        if (source < 0) return 5;
        std::uint64_t bytes = 0;
        for (;;) {
            const auto n = ::read(source, buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) continue;
            if (n == 0) break;
            if (n < 0) { (void)::close(source); return 5; }
            bytes += static_cast<std::uint64_t>(n);
            if (pass && bytes > 2u * 1024 * 1024) { (void)::close(source); return 8; }
            if (!write_all(pass ? STDOUT_FILENO : sink, buffer.data(), static_cast<std::size_t>(n))) {
                (void)::close(source); return 5;
            }
        }
        if (::close(source) < 0) return 5;
        if (!pass && ::close(sink) < 0) return 5; // deliver EOF before SUMMARY
    }
    return 0;
}
int number(std::string_view text) {
    int fd = -1;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), fd);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? fd : -1;
}
struct Failure {
    const char* child = "";
    const char* kind = "";
    int status = 0;
    std::uint64_t time = 0;
};
} // namespace

int main(int argc, char** argv) {
    (void)::signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && std::string_view(argv[1]) == "--replay") return replay(argc, argv);
    int input = -1, a = -1, c = -1, status_fd = -1, control = -1, progress = -1;
    unsigned seen = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (++i == argc) return 2;
        const int fd = number(argv[i]);
        const int slot = flag == "--in-fd" ? 0 : flag == "--a-fd" ? 1 : flag == "--c-fd" ? 2
            : flag == "--status-fd" ? 3 : flag == "--control-fd" ? 4 : flag == "--progress-fd" ? 5 : -1;
        if (fd < 0 || slot < 0 || (seen & (1u << slot))) return 2;
        seen |= 1u << slot;
        if (slot == 0) input = fd;
        else if (slot == 1) a = fd;
        else if (slot == 2) c = fd;
        else if (slot == 3) status_fd = fd;
        else if (slot == 4) control = fd;
        else progress = fd;
    }
    if ((seen & 31) != 31) return 2;
    const std::array<int, 6> fds{{input, a, c, status_fd, control, progress}};
    for (std::size_t i = 0; i < fds.size(); ++i)
        for (std::size_t j = i + 1; j < fds.size(); ++j)
            if (fds[i] >= 0 && fds[i] == fds[j]) return 2;

    Failure failure;
    int result = 0;
    auto fail = [&](const char* child, const char* kind, int status, std::uint64_t time = 0) {
        if (failure.time) return;
        failure = {child, kind, status, time ? time : now()};
        result = 5;
    };
    std::array<int, 3> capacities{{65536, 65536, 65536}}; // Darwin: documented conservative bound
#if defined(__linux__)
    for (unsigned i = 0; i < capacities.size(); ++i) {
        capacities[i] = ::fcntl(fds[i], F_GETPIPE_SZ);
        // Shrink an oversized default; accept any smaller granted capacity.
        if (capacities[i] > static_cast<int>(slab_bytes)) {
            (void)::fcntl(fds[i], F_SETPIPE_SZ, static_cast<int>(slab_bytes));
            capacities[i] = ::fcntl(fds[i], F_GETPIPE_SZ);
        }
        if (capacities[i] <= 0 || capacities[i] > static_cast<int>(slab_bytes))
            fail("relay", "pipe_capacity", 5);
    }
#endif
    if (!nonblocking(input) || !nonblocking(a) || !nonblocking(c) || !nonblocking(control))
        fail("relay", "fcntl", 5);
    std::array<char, slab_bytes> slab{};
    std::size_t length = 0, a_at = 0, c_at = 0;
    std::uint64_t read_bytes = 0, a_bytes = 0, c_bytes = 0, slabs = 0;
    // Optional fixed-size observation file for black-box backpressure tests.
    // Native u64 words: sequence, input bytes, delivered A/C, length, A/C offsets.
    auto observe = [&]() {
        if (progress < 0) return;
        const std::array<std::uint64_t, 7> words{{slabs, read_bytes, a_bytes, c_bytes, length, a_at, c_at}};
        ssize_t n;
        do { n = ::pwrite(progress, words.data(), sizeof(words), 0); } while (n < 0 && errno == EINTR);
        if (n != static_cast<ssize_t>(sizeof(words))) fail("relay", "progress_io", 5);
    };
    std::array<char, 96> message{};
    std::size_t message_size = 0;
    while (!result) {
        const bool drained = a_at == length && c_at == length;
        // Both outputs stay in poll even between slabs: POLLERR detects a dead
        // reader while the engine is idle, without waiting for the next write.
        pollfd ready[] = {
            {drained ? input : -1, POLLIN, 0},
            {a, static_cast<short>(a_at < length ? POLLOUT : 0), 0},
            {c, static_cast<short>(c_at < length ? POLLOUT : 0), 0},
            {control, POLLIN, 0},
        };
        int n;
        do { n = ::poll(ready, 4, -1); } while (n < 0 && errno == EINTR);
        if (n < 0) { fail("relay", "poll", 5); break; }
        // Owner sends at most one <=96-byte record: A|C, wait4 status, timestamp.
        if (ready[3].revents & POLLIN) {
            const auto got = ::read(control, message.data() + message_size, message.size() - 1 - message_size);
            if (got > 0) {
                message_size += static_cast<std::size_t>(got);
                message[message_size] = '\0';
                if (std::memchr(message.data(), '\n', message_size)) {
                    char who = 0; int code = 0; unsigned long long when = 0;
                    if (std::sscanf(message.data(), "%c %d %llu", &who, &code, &when) == 3
                        && (who == 'A' || who == 'C') && code != 0 && when)
                        fail(who == 'A' ? "stage_a" : "stage_c", "status", code, when);
                    else fail("relay", "control_invalid", 5);
                } else if (message_size == message.size() - 1) fail("relay", "control_overflow", 5);
            } else if (got < 0 && errno != EINTR && errno != EAGAIN) fail("relay", "control_io", 5);
        }
        if (result) break;
        for (unsigned i = 1; i <= 2; ++i)
            if (ready[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                fail(i == 1 ? "stage_a" : "stage_c", "EPIPE", EPIPE);
        if (ready[3].revents & (POLLHUP | POLLERR | POLLNVAL)) fail("owner", "cancelled", 5);
        if (result) break;
        if (drained && (ready[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            const auto got = ::read(input, slab.data(), slab.size());
            if (got == 0) break;
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                fail("runner", "read", errno); break;
            }
            length = static_cast<std::size_t>(got);
            a_at = c_at = 0; read_bytes += length; ++slabs;
        }
        auto deliver = [&](int fd, std::size_t& at, std::uint64_t& bytes, const char* child) {
            if (at == length) return;
            const auto sent = ::write(fd, slab.data() + at, length - at);
            if (sent < 0 && (errno == EINTR || errno == EAGAIN)) return;
            if (sent <= 0) { fail(child, errno == EPIPE ? "EPIPE" : "write", errno); return; }
            at += static_cast<std::size_t>(sent); bytes += static_cast<std::uint64_t>(sent);
        };
        deliver(a, a_at, a_bytes, "stage_a");
        if (!result) deliver(c, c_at, c_bytes, "stage_c");
        observe();
    }
    // Closing E.read wakes a blocked engine even when one consumer is stopped.
    (void)::close(input); (void)::close(a); (void)::close(c);
    observe();
    char document[1024];
    const int size = std::snprintf(document, sizeof(document),
        "{\"complete\":%s,\"input_bytes\":%llu,\"a_bytes\":%llu,\"c_bytes\":%llu,"
        "\"slab_bytes\":%zu,\"slabs\":%llu,\"pipe_bytes\":[%d,%d,%d],"
        "\"first_failure\":{\"child\":\"%s\",\"kind\":\"%s\",\"status\":%d,\"monotonic_ns\":%llu}}\n",
        result ? "false" : "true", static_cast<unsigned long long>(read_bytes),
        static_cast<unsigned long long>(a_bytes), static_cast<unsigned long long>(c_bytes),
        slab_bytes, static_cast<unsigned long long>(slabs), capacities[0], capacities[1], capacities[2],
        failure.child, failure.kind, failure.status, static_cast<unsigned long long>(failure.time));
    if (size < 0 || size >= static_cast<int>(sizeof(document))
        || !write_all(status_fd, document, static_cast<std::size_t>(size))) return 5;
    return result;
}
