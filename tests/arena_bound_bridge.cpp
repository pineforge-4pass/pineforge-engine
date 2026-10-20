// Test-only exports, linked into the SAME module as generated.cpp and the
// static engine. A separately loaded helper would measure another singleton.
// No runtime PF_API export and no production header delta.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
std::string bridge_error;
std::uint64_t legacy_writes = 0;
bool measuring = false;

// The named report sink is permitted. All legacy private scratch is unlinked.
// Only references in this test module's linked engine are wrapped; Python's
// own file writes and the wrapper's preallocation are unaffected.
bool legacy_scratch(int fd) {
    if (!measuring) return false;
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
