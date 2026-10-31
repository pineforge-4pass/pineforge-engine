#pragma once

// Include in one translation unit per Linux executable. Count actual backing
// reads, independently of the decoded cache, and forward to the kernel.
#include <pineforge/detail/scratch_file.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

namespace trade_ledger_cache_io {
struct Counts {
    std::uint64_t calls = 0, bytes = 0, failures = 0;
};
inline Counts counts;
inline bool measuring = false;
inline int next_error = 0;

struct Measure {
    Measure() {
        if (measuring) throw std::logic_error("nested ledger backing measurement");
        counts = {};
        measuring = true;
    }
    ~Measure() { measuring = false; next_error = 0; }
    Counts finish() {
        measuring = false;
        return counts;
    }
};
} // namespace trade_ledger_cache_io

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    using namespace trade_ledger_cache_io;
    ssize_t result;
    if (measuring && next_error != 0) {
        errno = next_error;
        next_error = 0;
        result = -1;
    } else {
        result = static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
    }
    if (measuring) {
        ++counts.calls;
        if (result > 0) counts.bytes += static_cast<std::uint64_t>(result);
        if (result < 0) ++counts.failures;
    }
    return result;
}
