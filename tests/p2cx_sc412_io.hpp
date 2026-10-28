#pragma once
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

// Linux executable-local interposition, as in test_p2cx_tledger_io.cpp.
// Every request reaches the real syscall. No production observations/hooks.
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace sc412 {
inline void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
enum Family : std::size_t { ids, pairs, other, families };
struct Traffic {
    std::uint64_t reads = 0, writes = 0;
    std::uint64_t read_requests = 0, read_bytes = 0, write_bytes = 0;
    std::uint64_t past_end = 0;
};
inline std::array<Traffic, families> traffic{};
inline bool measuring = false;
struct File {
    int fd = -1;
    dev_t device{};
    ino_t inode{};
    Family family = other;
};
inline std::array<File, 256> files{};
inline std::size_t file_count = 0;

inline File identify(int fd) {
    char link[64]{}, name[512]{};
    std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const auto size = ::readlink(link, name, sizeof name - 1);
    Family family = other;
    if (size > 0) {
        if (std::strstr(name, "pineforge-id-pairs-")) family = pairs;
        else if (std::strstr(name, "pineforge-ids-")) family = ids;
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0) return {fd, {}, {}, family};
    File result{fd, status.st_dev, status.st_ino, family};
    if (family != other) {
        bool known = false;
        for (std::size_t i = 0; i < file_count; ++i)
            known = known || (files[i].device == result.device && files[i].inode == result.inode);
        if (!known) {
            require(file_count < files.size(), "fixture descriptor registry has room");
            files[file_count++] = result;
        }
    }
    return result;
}
inline void reset_files() {
    file_count = 0;
    traffic = {};
}
struct Measure {
    Measure() { require(!measuring, "no nested measurement"); traffic = {}; measuring = true; }
    ~Measure() { measuring = false; }
    auto finish() { measuring = false; return traffic; }
};
inline std::uint64_t hash(std::uint64_t seed, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < length; ++i) { seed ^= bytes[i]; seed *= 1099511628211ULL; }
    return seed;
}
constexpr std::uint64_t basis = 1469598103934665603ULL;

// This descriptor has its own open file description, not a cache or a dup
// of a table object. Raw syscalls keep oracle reads out of measured traffic.
inline int independent_fd(int fd) {
    char path[64]{};
    std::snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    const int result = ::open(path, O_RDONLY | O_CLOEXEC);
    require(result >= 0, "open separate file-image descriptor");
    return result;
}
inline std::vector<unsigned char> image(int fd) {
    const int reader = independent_fd(fd);
    struct stat status{};
    require(::fstat(reader, &status) == 0, "image extent");
    std::vector<unsigned char> result(static_cast<std::size_t>(status.st_size));
    std::size_t at = 0;
    while (at < result.size()) {
        const auto got = static_cast<ssize_t>(::syscall(
            SYS_pread64, reader, result.data() + at, result.size() - at, static_cast<off_t>(at)));
        if (got < 0 && errno == EINTR) continue;
        require(got > 0, "read actual file image");
        at += static_cast<std::size_t>(got);
    }
    require(::close(reader) == 0, "close separate descriptor");
    return result;
}
inline auto images() {
    std::vector<std::vector<unsigned char>> result;
    for (std::size_t i = 0; i < file_count; ++i) {
        struct stat status{};
        // Rehash replacements and cleared files no longer exist. Compare
        // every still-live image, in real first-observed creation order.
        if (::fstat(files[i].fd, &status) == 0
            && status.st_dev == files[i].device && status.st_ino == files[i].inode)
            result.push_back(image(files[i].fd));
    }
    return result;
}

// The adapter uses the same capacity constructor. A frozen-base build has
// no such constructor and executes its original uncached operations.
template<class Table>
Table table_with_cache(std::size_t bytes) {
    if constexpr (std::is_constructible_v<Table, std::size_t>) return Table(bytes);
    else return Table();
}
} // namespace sc412

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    const int saved = errno;
    const auto file = sc412::identify(fd);
    struct stat status{};
    const bool sized = ::fstat(fd, &status) == 0;
    errno = saved;
    const auto result = static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
    const int error = errno;
    if (sc412::measuring) {
        auto& count = sc412::traffic[file.family];
        ++count.reads;
        count.read_requests += length;
        if (result > 0) count.read_bytes += static_cast<std::uint64_t>(result);
        if (sized && (offset > status.st_size
            || (offset >= 0 && length > static_cast<std::uint64_t>(status.st_size - offset))))
            ++count.past_end;
    }
    errno = error;
    return result;
}
extern "C" ssize_t pwrite(int fd, const void* data, std::size_t length, off_t offset) {
    const int saved = errno;
    const auto file = sc412::identify(fd);
    errno = saved;
    const auto result = static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, length, offset));
    const int error = errno;
    if (sc412::measuring) {
        auto& count = sc412::traffic[file.family];
        ++count.writes;
        if (result > 0) count.write_bytes += static_cast<std::uint64_t>(result);
    }
    errno = error;
    return result;
}
// Cover either large-file spelling without relying on compiler flags.
#if !defined(pread) && (!defined(_FILE_OFFSET_BITS) || _FILE_OFFSET_BITS != 64)
extern "C" ssize_t pread64(int fd, void* data, std::size_t length, off64_t offset) {
    return pread(fd, data, length, static_cast<off_t>(offset));
}
#endif
#if !defined(pwrite) && (!defined(_FILE_OFFSET_BITS) || _FILE_OFFSET_BITS != 64)
extern "C" ssize_t pwrite64(int fd, const void* data, std::size_t length, off64_t offset) {
    return pwrite(fd, data, length, static_cast<off_t>(offset));
}
#endif
