#pragma once

#include "p2cx_tledger_fixture.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace ledger_cyclic_io {
struct Counts {
    std::uint64_t reads = 0, bytes = 0, maps = 0, unmaps = 0, bad_maps = 0, stats = 0;
    std::uint64_t discards = 0;
};
inline bool measuring = false;
inline Counts counts;
enum class ReadFault { none, prefix, failure };
inline ReadFault read_fault = ReadFault::none;
inline bool fail_next_map = false;

struct Measure {
    Measure() {
        REQUIRE(!measuring);
        counts = {};
        measuring = true;
    }
    ~Measure() { measuring = false; }
    Counts finish() {
        measuring = false;
        return counts;
    }
};
} // namespace ledger_cyclic_io

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    ssize_t received;
    if (ledger_cyclic_io::read_fault == ledger_cyclic_io::ReadFault::failure) {
        ledger_cyclic_io::read_fault = ledger_cyclic_io::ReadFault::none;
        errno = EIO;
        received = -1;
    } else {
        if (ledger_cyclic_io::read_fault == ledger_cyclic_io::ReadFault::prefix) {
            length = std::min(length, 2 * pineforge::TradeLedger::kRecordBytes);
            ledger_cyclic_io::read_fault = ledger_cyclic_io::ReadFault::failure;
        }
        received = static_cast<ssize_t>(
            ::syscall(SYS_pread64, fd, data, length, offset));
    }
    if (ledger_cyclic_io::measuring) {
        ++ledger_cyclic_io::counts.reads;
        if (received > 0)
            ledger_cyclic_io::counts.bytes += static_cast<std::uint64_t>(received);
    }
    return received;
}

extern "C" void* mmap(void* address, std::size_t length, int protection,
                      int flags, int fd, off_t offset) noexcept {
    if (ledger_cyclic_io::measuring) {
        ++ledger_cyclic_io::counts.maps;
        if (protection != PROT_READ || flags != MAP_SHARED)
            ++ledger_cyclic_io::counts.bad_maps;
    }
    if (std::exchange(ledger_cyclic_io::fail_next_map, false)) {
        errno = EIO;
        return MAP_FAILED;
    }
    return reinterpret_cast<void*>(::syscall(SYS_mmap, address, length, protection,
                                             flags, fd, offset));
}

extern "C" int munmap(void* address, std::size_t length) noexcept {
    if (ledger_cyclic_io::measuring) ++ledger_cyclic_io::counts.unmaps;
    return static_cast<int>(::syscall(SYS_munmap, address, length));
}

extern "C" int fstat(int fd, struct stat* status) noexcept {
    if (ledger_cyclic_io::measuring) ++ledger_cyclic_io::counts.stats;
    return static_cast<int>(::syscall(SYS_fstat, fd, status));
}

extern "C" int posix_fadvise(int fd, off_t offset, off_t length, int advice) noexcept {
    if (ledger_cyclic_io::measuring && advice == POSIX_FADV_DONTNEED)
        ++ledger_cyclic_io::counts.discards;
    const int saved_errno = errno;
    const auto result = ::syscall(SYS_fadvise64, fd, offset, length, advice);
    const int error = result < 0 ? errno : 0;
    errno = saved_errno;
    return error;
}
