#pragma once

// File-mode metadata/fault witnesses only. Production snapshots deliberately
// expose no fd. Find the test-owned unlinked file by its complete contents,
// without adding an accessor or a test hook to the frozen facade.
#include "../src/report_scratch.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace scratch_fixture {

inline int descriptor(const pineforge::metrics::ScratchCurveRegion& region) {
    if (!region.scratch || region.descriptor != -1
        || pineforge::detail::ScratchArena::attached()) {
        throw std::runtime_error("metadata witness requires a file-mode facade region");
    }
    DIR* directory = ::opendir("/dev/fd");
    if (!directory) throw std::runtime_error("open test fd directory");
    std::vector<int> candidates;
    while (const auto* entry = ::readdir(directory)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && value >= 0
            && value <= std::numeric_limits<int>::max() && value != ::dirfd(directory)) {
            candidates.push_back(static_cast<int>(value));
        }
    }
    ::closedir(directory);
    const auto first = static_cast<std::uint64_t>(region.byte_offset);
    const auto length = static_cast<std::uint64_t>(region.point_count)
        * sizeof(pf_equity_point_t);
    std::array<unsigned char, 4096> expected{}, observed{};
    for (const int fd : candidates) {
        struct stat status{};
        if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)
            || status.st_nlink != 0 || status.st_size < 0
            || static_cast<std::uint64_t>(status.st_size) < first + length) continue;
        if (length == 0 && static_cast<std::uint64_t>(status.st_size) != first) continue;
        bool equal = true;
        for (std::uint64_t offset = 0; offset < length && equal;) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(expected.size(), length - offset));
            region.scratch->read(first + offset, expected.data(), count);
            ssize_t got;
            do {
                got = ::pread(fd, observed.data(), count, static_cast<off_t>(first + offset));
            } while (got < 0 && errno == EINTR);
            equal = got == static_cast<ssize_t>(count)
                && std::memcmp(expected.data(), observed.data(), count) == 0;
            offset += count;
        }
        if (equal) return fd;
    }
    throw std::runtime_error("no unlinked test descriptor matches the complete scratch region");
}

} // namespace scratch_fixture
