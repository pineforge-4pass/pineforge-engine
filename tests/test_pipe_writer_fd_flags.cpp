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
};

void run_case(bool duplicate_writer) {
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
}
} // namespace

int main() {
    const char* active_case = "plain writer";
    try {
        run_case(false);
        active_case = "F_DUPFD_CLOEXEC writer";
        run_case(true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pipe writer fd control (%s): %s\n", active_case, error.what());
        return 1;
    }
}
