#pragma once

#include <pineforge/run_failure.hpp>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <system_error>
#include <time.h>

namespace pineforge::resource_clock {

// Internal, per-thread syscall seam. Production leaves both pointers null.
// Tests advance resource time only after the real I/O/callback action.
inline thread_local int (*read_clock)(clockid_t, timespec*) = nullptr;
inline thread_local int (*wait_poll)(pollfd*, nfds_t, int) = nullptr;

inline std::int64_t milliseconds(clockid_t kind) {
    timespec value{};
    const int status = read_clock ? read_clock(kind, &value) : ::clock_gettime(kind, &value);
    if (status != 0)
        throw coded<std::system_error>(RunFailureCode::engine_invariant, {},
            std::system_error(errno, std::generic_category(), "resource clock"));
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (value.tv_sec >= maximum / 1000) return maximum;
    return static_cast<std::int64_t>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
}

inline std::int64_t wall_ms() { return milliseconds(CLOCK_MONOTONIC); }
inline std::int64_t cpu_ms() { return milliseconds(CLOCK_PROCESS_CPUTIME_ID); }

inline int poll(pollfd* descriptors, nfds_t count, int timeout) {
    return wait_poll ? wait_poll(descriptors, count, timeout)
                     : ::poll(descriptors, count, timeout);
}

} // namespace pineforge::resource_clock
