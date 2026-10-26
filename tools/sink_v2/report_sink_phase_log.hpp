#pragma once

// Trusted-tool diagnostics only. No wire/receipt state or record-level hooks.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

namespace pineforge { namespace report_sink {

class PhaseLog {
    int fd_ = -1; // borrowed; never probed/read/closed
    int status_ = 7; // an unwound/unfinished invocation cannot look successful
public:
    explicit PhaseLog(int fd = -1) noexcept : fd_(fd) { mark("process_start"); }
    PhaseLog(const PhaseLog&) = delete;
    PhaseLog& operator=(const PhaseLog&) = delete;
    ~PhaseLog() { mark("exit", true, status_); }
    void status(int value) noexcept { status_ = value; }
    void mark(const char* event, bool accounting = false, int status = -1) const noexcept {
        if (fd_ < 0) return; // no clock/resource/fd syscall when disabled
        const int saved = errno;
        struct timespec now{};
        struct rusage usage{};
        char line[512];
        int size = -1;
        if (::clock_gettime(CLOCK_MONOTONIC, &now) == 0
            && (!accounting || ::getrusage(RUSAGE_SELF, &usage) == 0)) {
            const auto ns = static_cast<unsigned long long>(now.tv_sec) * 1000000000ull + now.tv_nsec;
            if (accounting) {
                const auto user = static_cast<unsigned long long>(usage.ru_utime.tv_sec) * 1000000000ull
                    + static_cast<unsigned long long>(usage.ru_utime.tv_usec) * 1000ull;
                const auto system = static_cast<unsigned long long>(usage.ru_stime.tv_sec) * 1000000000ull
                    + static_cast<unsigned long long>(usage.ru_stime.tv_usec) * 1000ull;
#if defined(__APPLE__)
                const auto rss = static_cast<unsigned long long>(usage.ru_maxrss) / 1024;
#else
                const auto rss = static_cast<unsigned long long>(usage.ru_maxrss);
#endif
                size = std::snprintf(line, sizeof line,
                    "{\"event\":\"%s\",\"monotonic_ns\":%llu,\"cpu_user_ns\":%llu,"
                    "\"cpu_sys_ns\":%llu,\"max_rss_kb\":%llu,\"status\":%d}\n",
                    event, ns, user, system, rss, status);
            } else {
                size = std::snprintf(line, sizeof line, "{\"event\":\"%s\",\"monotonic_ns\":%llu}\n", event, ns);
            }
        }
        bool failed = size <= 0 || static_cast<std::size_t>(size) >= sizeof line;
        for (int at = 0; !failed && at < size;) {
            const auto n = ::write(fd_, line + at, static_cast<std::size_t>(size - at));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) failed = true;
            else at += static_cast<int>(n);
        }
        // Diagnostics must not replace a primary refusal or change publication.
        // A truncated log is explicit and cannot certify missing phases.
        if (failed) std::fputs("pf_sink_transcode_v2: phase log write/clock/rusage failed\n", stderr);
        errno = saved;
    }
};

}} // namespace pineforge::report_sink
