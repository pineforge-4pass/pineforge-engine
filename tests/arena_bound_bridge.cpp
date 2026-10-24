// Test-only exports, linked into the SAME module as generated.cpp and the
// static engine. A separately loaded helper would measure another singleton.
// No runtime PF_API export and no production header delta.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/run_failure.hpp>
#include "command_scratch_arena_share_fixture.hpp"

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
pineforge::detail::ScratchArenaStats command_before, command_latest, command_failed_before, command_failed;
std::uint64_t command_failures = 0;
std::uint64_t command_needed = 0;
std::uint64_t command_failed_extent = 0;
std::uint64_t command_peak_extent = 0;
std::uint64_t command_capacity = 0;
pineforge::detail::ScratchIoObserver previous_command_observer{};
bool command_observing = false;

void observe_command(void*, const pineforge::detail::ScratchIoCharge& io) noexcept {
    using namespace pineforge::detail;
    const auto current = ScratchArena::attached()->stats();
    if (io.operation == ScratchIoOperation::write_page) {
        const auto extent = (io.page + 1) * io.bytes; // Observer supplies each store's page size.
        if (io.status == ScratchStatus::ok)
            command_peak_extent = std::max(command_peak_extent, extent);
        if (io.status == ScratchStatus::io_error && command_failures++ == 0) {
            // Read AFTER the refusal, before teardown releases any regions.
            command_failed_before = command_latest;
            command_failed = current;
            command_failed_extent = extent;
            command_needed = command_arena_test::required_arena(extent);
        }
    }
    command_latest = current;
}

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
int arena_bound_test_watch_commands(std::uint64_t report_first_bytes) noexcept {
    try {
        using namespace pineforge::detail;
        bridge_error.clear();
        auto* arena = ScratchArena::attached();
        command_arena_test::require(arena && !command_observing && arena->stats().bytes_used == 0,
                                    "command observation precedes the run");
        command_before = arena->stats();
        command_arena_test::require(
            command_before.bytes_total - command_before.bytes_used >= report_first_bytes,
            "report users have their independently counted first-write allowance");
        // Inspect a real factory-selected region, without claiming a page.
        // Its floored bound must be the same for every store, regardless of page size.
        {
            auto backing = make_command_scratch_backing(command_arena_test::kPageBytes);
            command_arena_test::require(backing->arena_capacity_bytes().has_value(),
                                        "command factory really selected the attached arena");
            command_capacity = *backing->arena_capacity_bytes();
            command_arena_test::require(
                command_capacity == command_arena_test::share_capacity(command_before.bytes_supplied)
                && command_capacity >= ScratchArena::kChunkBytes
                && arena->stats().bytes_used == command_before.bytes_used,
                "open fixes the stats-derived share with a one-chunk floor without claiming chunks");
        }
        command_failures = command_needed = command_failed_extent = command_peak_extent = 0;
        command_failed = command_failed_before = {};
        command_latest = command_before;
        previous_command_observer = set_command_scratch_io_observer({nullptr, observe_command});
        command_observing = true;
        return 0;
    } catch (const std::exception& error) {
        bridge_error = error.what();
        return -1;
    }
}
void arena_bound_test_stop_watching_commands() noexcept {
    if (command_observing)
        pineforge::detail::set_command_scratch_io_observer(previous_command_observer);
    command_observing = false;
}
std::uint64_t arena_bound_test_command_failures() noexcept { return command_failures; }
std::uint64_t arena_bound_test_command_needed() noexcept { return command_needed; }
std::uint64_t arena_bound_test_command_failed_extent() noexcept { return command_failed_extent; }
std::uint64_t arena_bound_test_command_peak_extent() noexcept { return command_peak_extent; }
std::uint64_t arena_bound_test_command_capacity() noexcept { return command_capacity; }
std::uint64_t arena_bound_test_command_share_numerator() noexcept {
    return pineforge::detail::kCommandScratchArenaShare.num;
}
std::uint64_t arena_bound_test_command_share_denominator() noexcept {
    return pineforge::detail::kCommandScratchArenaShare.den;
}
std::uint64_t arena_bound_test_command_before_used() noexcept { return command_before.bytes_used; }
std::uint64_t arena_bound_test_command_before_total() noexcept { return command_before.bytes_total; }
std::uint64_t arena_bound_test_command_before_supplied() noexcept { return command_before.bytes_supplied; }
std::uint64_t arena_bound_test_command_failed_before_used() noexcept { return command_failed_before.bytes_used; }
std::uint64_t arena_bound_test_command_failed_before_high_water() noexcept { return command_failed_before.high_water; }
std::uint64_t arena_bound_test_command_failed_high_water() noexcept { return command_failed.high_water; }
std::uint64_t arena_bound_test_command_failed_supplied() noexcept { return command_failed.bytes_supplied; }
std::uint64_t arena_bound_test_command_failed_used() noexcept { return command_failed.bytes_used; }
std::uint64_t arena_bound_test_command_failed_free() noexcept { return command_failed.chunks_free; }
int arena_bound_test_detach() noexcept {
    try {
        arena_bound_test_stop_watching_commands();
        pineforge::detail::ScratchArena::detach();
        measuring = false;
        return 0;
    } catch (const std::exception& error) {
        bridge_error = error.what();
        return -1;
    }
}
} // extern "C"
