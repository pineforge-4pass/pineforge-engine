// Test-only exports, linked into the SAME module as generated.cpp and the
// static engine. A separately loaded helper would measure another singleton.
// No runtime PF_API export and no production header delta.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
std::string bridge_error;
std::uint64_t legacy_writes = 0;
std::uint64_t legacy_creations = 0;
bool measuring = false;

bool attached_measurement() {
    return measuring && pineforge::detail::ScratchArena::attached();
}

bool refuse_creation() {
    if (!attached_measurement()) return false;
    ++legacy_creations;
    errno = EACCES;
    return true;
}

bool creates_file(int directory, const char* path, int flags) {
    if (!attached_measurement()) return false;
    if ((flags & O_TMPFILE) == O_TMPFILE) return true;
    if (!(flags & O_CREAT)) return false;
    // Python already created the named report sink. Reopening that regular,
    // linked file cannot create an empty scratch file; a new name, O_EXCL,
    // symlink, or anonymous regular file is refused before open is called.
    if (flags & O_EXCL) return true;
    struct stat status{};
    return ::fstatat(directory, path, &status, AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISREG(status.st_mode) || status.st_nlink == 0;
}

// The pre-existing named report sink is permitted. Legacy scratch is unlinked.
// Only references in this test module's linked engine are wrapped; Python's
// own file writes and the wrapper's preallocation are unaffected.
bool legacy_scratch(int fd) {
    if (!attached_measurement()) return false;
    struct stat status {};
    if (::fstat(fd, &status) != 0) return false;
    if (!S_ISREG(status.st_mode) || status.st_nlink != 0) return false;
    ++legacy_writes;
    errno = EFBIG;
    return true;
}
} // namespace

extern "C" {
ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
ssize_t __real_write(int, const void*, std::size_t);
int __real_ftruncate(int, off_t);
int __real_mkstemp(char*);
int __real_mkostemp(char*, int);
int __real_open(const char*, int, ...);
int __real_openat(int, const char*, int, ...);

int __wrap_mkstemp(char* path) {
    return refuse_creation() ? -1 : __real_mkstemp(path);
}
int __wrap_mkostemp(char* path, int flags) {
    return refuse_creation() ? -1 : __real_mkostemp(path, flags);
}
int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    if (creates_file(AT_FDCWD, path, flags) && refuse_creation()) return -1;
    return __real_open(path, flags, mode);
}
int __wrap_openat(int directory, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    if (creates_file(directory, path, flags) && refuse_creation()) return -1;
    return __real_openat(directory, path, flags, mode);
}

ssize_t __wrap_pwrite(int fd, const void* data, std::size_t length, off_t offset) {
    return legacy_scratch(fd) ? -1 : __real_pwrite(fd, data, length, offset);
}
ssize_t __wrap_write(int fd, const void* data, std::size_t length) {
    return legacy_scratch(fd) ? -1 : __real_write(fd, data, length);
}
int __wrap_ftruncate(int fd, off_t length) {
    return legacy_scratch(fd) ? -1 : __real_ftruncate(fd, length);
}

int arena_bound_test_attach(int fd, std::uint64_t bytes) noexcept {
    try {
        bridge_error.clear();
        legacy_writes = 0;
        legacy_creations = 0;
        pineforge::detail::ScratchArena::attach(fd, bytes);
        measuring = true;
        return 0;
    } catch (const std::exception& error) {
        bridge_error = error.what();
        return -1;
    }
}
std::uint64_t arena_bound_test_high_water() {
    const auto* arena = pineforge::detail::ScratchArena::attached();
    return arena ? arena->stats().high_water : 0;
}
std::uint64_t arena_bound_test_bytes_used() {
    const auto* arena = pineforge::detail::ScratchArena::attached();
    return arena ? arena->stats().bytes_used : 0;
}
std::uint64_t arena_bound_test_legacy_writes() noexcept { return legacy_writes; }
std::uint64_t arena_bound_test_legacy_creations() noexcept { return legacy_creations; }
int arena_bound_test_creation_probe() noexcept {
    // Exercise the actual linked references. A missing --wrap cannot pass
    // merely because a directory happened to deny writes: the counter must
    // record all three attempts before these deliberate probes are removed.
    if (!attached_measurement()) return -1;
    const auto before = legacy_creations;
    char temporary[] = "pineforge-arena-creation-probe-XXXXXX";
    errno = 0;
    int fd = ::mkstemp(temporary);
    const bool temp_refused = fd == -1 && errno == EACCES;
    if (fd >= 0) { ::close(fd); ::unlink(temporary); }
    const char* named = "pineforge-arena-creation-probe-exclusive";
    errno = 0;
    fd = ::open(named, O_RDWR | O_CREAT | O_EXCL, 0600);
    const bool open_refused = fd == -1 && errno == EACCES;
    if (fd >= 0) { ::close(fd); ::unlink(named); }
    errno = 0;
    fd = ::openat(AT_FDCWD, named, O_RDWR | O_CREAT | O_EXCL, 0600);
    const bool openat_refused = fd == -1 && errno == EACCES;
    if (fd >= 0) { ::close(fd); ::unlink(named); }
    if (!temp_refused || !open_refused || !openat_refused || legacy_creations != before + 3)
        return -1;
    legacy_creations = before;
    return 0;
}
const char* arena_bound_test_error() noexcept { return bridge_error.c_str(); }
int arena_bound_test_detach() noexcept {
    try {
        pineforge::detail::ScratchArena::detach();
        measuring = false;
        return 0;
    } catch (const std::exception& error) {
        bridge_error = error.what();
        return -1;
    }
}
} // extern "C"
