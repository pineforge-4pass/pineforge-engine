// Isolate pipe flag observations from every engine operation.
#include "fd_flag_diagnostics.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Pipe {
    int descriptors[2]{-1, -1};
    Pipe() {
        require(::pipe(descriptors) == 0, "pipe failed");
        // Exactly the reader setup used by both nonfinite-equity fd tests.
        const int flags = ::fcntl(descriptors[0], F_GETFL);
        require(flags >= 0 && ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0,
                "nonblocking reader setup failed");
    }
    ~Pipe() {
        for (const int fd : descriptors) if (fd >= 0) ::close(fd);
    }
    std::string drain() const {
        std::string bytes;
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return bytes;
            require(count >= 0, "pipe read failed");
            if (count == 0) return bytes;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    void duplicate_writer() {
        // Match L22A's inherited descriptor allocation and original close.
        const int duplicate = ::fcntl(descriptors[1], F_DUPFD_CLOEXEC, 17);
        require(duplicate >= 17, "cannot allocate a non-default inherited fd");
        const int original = descriptors[1];
        descriptors[1] = duplicate;
        require(::close(original) == 0, "original writer close failed");
    }
    void close_reader() {
        const int reader = descriptors[0];
        descriptors[0] = -1;
        require(::close(reader) == 0, "pipe reader close failed");
    }
    void close_writer() {
        const int writer = descriptors[1];
        descriptors[1] = -1;
        require(::close(writer) == 0, "pipe writer close failed");
    }
};

void reject_real_descriptor_mutations(Pipe& pipe) {
    // Plant real syscalls on the same writer that just acquired FWASWRITTEN
    // on Darwin. These controls also run on Linux in the existing CTest row.
    fd_flag_diagnostics::Trace reflag_trace(pipe.descriptors[1], "before planted F_SETFL");
    require(reflag_trace.unchanged(), reflag_trace.message("cannot read mutation fixture flags"));
    const int flags = reflag_trace.baseline().flags;
    require(::fcntl(pipe.descriptors[1], F_SETFL, flags ^ O_NONBLOCK) == 0,
            "cannot plant an O_NONBLOCK change");
    const auto reflagged = reflag_trace.record("after planted F_SETFL");
    require(reflagged.flags >= 0 && ((reflagged.flags ^ flags) & O_NONBLOCK) != 0,
            reflag_trace.message("O_NONBLOCK plant did not change the real descriptor"));
    require(!fd_flag_diagnostics::same_flags(reflag_trace.baseline(), reflagged)
            && !reflag_trace.unchanged(),
            reflag_trace.message("borrowed-fd guard accepted a real O_NONBLOCK change"));
    require(::fcntl(pipe.descriptors[1], F_SETFL, flags) == 0, "cannot restore status flags");
    const auto restored = reflag_trace.record("after restoring F_SETFL");
    require(fd_flag_diagnostics::same_flags(reflag_trace.baseline(), restored)
            && !reflag_trace.unchanged(),
            reflag_trace.message("restoring flags hid a change at an earlier boundary"));

    fd_flag_diagnostics::Trace descriptor_trace(pipe.descriptors[1], "before planted F_SETFD");
    const int descriptor_flags = descriptor_trace.baseline().descriptor_flags;
    require(descriptor_trace.unchanged(), descriptor_trace.message("cannot read descriptor flags"));
    require(::fcntl(pipe.descriptors[1], F_SETFD, descriptor_flags ^ FD_CLOEXEC) == 0,
            "cannot plant an FD_CLOEXEC change");
    const auto reflagged_descriptor = descriptor_trace.record("after planted F_SETFD");
    require(reflagged_descriptor.descriptor_flags >= 0
            && ((reflagged_descriptor.descriptor_flags ^ descriptor_flags) & FD_CLOEXEC) != 0,
            descriptor_trace.message("FD_CLOEXEC plant did not change the real descriptor"));
    require(!fd_flag_diagnostics::same_flags(descriptor_trace.baseline(), reflagged_descriptor)
            && !descriptor_trace.unchanged(),
            descriptor_trace.message("borrowed-fd guard accepted a real FD_CLOEXEC change"));
    require(::fcntl(pipe.descriptors[1], F_SETFD, descriptor_flags) == 0,
            "cannot restore descriptor flags");

    fd_flag_diagnostics::Trace close_trace(pipe.descriptors[1], "before planted close");
    require(close_trace.unchanged(), close_trace.message("cannot read close fixture flags"));
    pipe.close_writer(); // Clear the fixture owner too, avoiding a double close.
    const auto closed = close_trace.record("after planted close");
    require(closed.flags == -1 && closed.flags_errno == EBADF
            && closed.descriptor_flags == -1 && closed.descriptor_flags_errno == EBADF,
            close_trace.message("close plant did not invalidate both real flag queries"));
    require(!fd_flag_diagnostics::same_flags(close_trace.baseline(), closed)
            && !fd_flag_diagnostics::same_flags(closed, closed) && !close_trace.unchanged(),
            close_trace.message("borrowed-fd guard accepted a real close"));
}

enum class Plant { none, nonblock, close };

void run_case(bool duplicate_writer, Plant plant = Plant::none) {
    Pipe pipe;
    if (duplicate_writer) pipe.duplicate_writer();
    fd_flag_diagnostics::Trace fd_trace(pipe.descriptors[1], "before nonempty write");
    const int flags = fd_trace.baseline().flags;
    const int descriptor_flags = fd_trace.baseline().descriptor_flags;
    require(flags >= 0 && descriptor_flags >= 0, fd_trace.message("cannot read pipe writer flags"));

    constexpr char payload[] = "borrowed-pipe-control";
    ssize_t written;
    do {
        written = ::write(pipe.descriptors[1], payload, sizeof payload - 1);
    } while (written < 0 && errno == EINTR);
    require(written == static_cast<ssize_t>(sizeof payload - 1), "nonempty pipe write failed");
    const auto after_write = fd_trace.record("after nonempty write");
    const auto bytes = pipe.drain();
    const auto after_drain = fd_trace.record("after draining reader");
    require(bytes == payload, "pipe control drain changed payload");
    pipe.close_reader();
    const auto after_close = fd_trace.record("after closing reader");

#if defined(__APPLE__)
    const int raw_difference = flags ^ after_write.flags;
    require(raw_difference == 0 || raw_difference == fd_flag_diagnostics::darwin_was_written,
            fd_trace.message("pipe write changed more than Darwin's written bit"));
#else
    require(after_write.flags == flags && after_drain.flags == flags && after_close.flags == flags,
            fd_trace.message("pipe write changed raw status flags"));
#endif
    require(fd_flag_diagnostics::same_flags(fd_trace.baseline(), after_write)
            && fd_flag_diagnostics::same_flags(fd_trace.baseline(), after_drain)
            && fd_flag_diagnostics::same_flags(fd_trace.baseline(), after_close),
            fd_trace.message("comparable pipe writer flags changed"));
    require(fd_flag_diagnostics::comparable_status_flags(after_write.flags)
                == fd_flag_diagnostics::comparable_status_flags(flags)
            && after_write.descriptor_flags == descriptor_flags
            && fd_flag_diagnostics::comparable_status_flags(after_drain.flags)
                == fd_flag_diagnostics::comparable_status_flags(flags)
            && after_drain.descriptor_flags == descriptor_flags
            && fd_flag_diagnostics::comparable_status_flags(after_close.flags)
                == fd_flag_diagnostics::comparable_status_flags(flags)
            && after_close.descriptor_flags == descriptor_flags,
            fd_trace.message("pipe writer flags changed"));

    // Explicit red-first modes exercise the very same invariant. If the
    // guard misses the plant, return normally so the external check fails.
    if (plant == Plant::nonblock) {
        require(::fcntl(pipe.descriptors[1], F_SETFL, after_close.flags | O_NONBLOCK) == 0,
                "cannot plant an O_NONBLOCK change");
        fd_trace.record("after planted F_SETFL");
    } else if (plant == Plant::close) {
        pipe.close_writer();
        fd_trace.record("after planted close");
    }
    require(fd_trace.unchanged(), fd_trace.message("borrowed fd invariant"));
    if (plant != Plant::none) return;

    // Mutate observations of the real pipe without involving an engine.
    for (const int bit : {O_NONBLOCK, O_APPEND, O_ACCMODE}) {
        auto changed = fd_trace.baseline();
        changed.flags ^= bit;
        require(fd_flag_diagnostics::comparable_status_flags(changed.flags)
                    != fd_flag_diagnostics::comparable_status_flags(flags)
                && !fd_flag_diagnostics::same_flags(fd_trace.baseline(), changed),
                fd_trace.message("comparison hid a caller-controlled status flag change"));
    }
    auto changed_descriptor = fd_trace.baseline();
    changed_descriptor.descriptor_flags ^= FD_CLOEXEC;
    require(!fd_flag_diagnostics::same_flags(fd_trace.baseline(), changed_descriptor),
            fd_trace.message("comparison hid an FD_CLOEXEC change"));
    auto failed_query = fd_trace.baseline();
    failed_query.flags = -1;
    require(fd_flag_diagnostics::comparable_status_flags(failed_query.flags) == failed_query.flags
            && !fd_flag_diagnostics::same_flags(fd_trace.baseline(), failed_query)
            && !fd_flag_diagnostics::same_flags(failed_query, fd_trace.baseline())
            && !fd_flag_diagnostics::same_flags(failed_query, failed_query),
            fd_trace.message("failed F_GETFL query compared equal"));
    failed_query = fd_trace.baseline();
    failed_query.descriptor_flags = -1;
    require(!fd_flag_diagnostics::same_flags(fd_trace.baseline(), failed_query)
            && !fd_flag_diagnostics::same_flags(failed_query, failed_query),
            fd_trace.message("failed F_GETFD query compared equal"));

    reject_real_descriptor_mutations(pipe);
}
} // namespace

int main(int argc, char* argv[]) {
    const char* active_case = "plain writer";
    try {
        Plant plant = Plant::none;
        if (argc == 2 && std::string(argv[1]) == "--plant-nonblock") plant = Plant::nonblock;
        else if (argc == 2 && std::string(argv[1]) == "--plant-close") plant = Plant::close;
        else require(argc == 1, "expected --plant-nonblock, --plant-close, or no arguments");
        run_case(false, plant);
        if (plant != Plant::none) return 0;
        active_case = "F_DUPFD_CLOEXEC writer";
        run_case(true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pipe writer fd control (%s): %s\n", active_case, error.what());
        return 1;
    }
}
