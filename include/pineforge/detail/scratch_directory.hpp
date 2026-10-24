#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>
#if defined(__linux__)
#include <sys/vfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif

namespace pineforge::detail {

inline bool scratch_filesystem_is_memory(std::uint64_t type) noexcept {
    // Normalize f_type on platforms where the filesystem word is signed.
    const auto magic = static_cast<std::uint32_t>(type);
    return magic == UINT32_C(0x01021994) || magic == UINT32_C(0x858458f6);
}

// Return an errno, not a boolean: a failed query is not proof of disk backing.
inline int scratch_directory_filesystem_type(const char* path, std::uint64_t& type) noexcept {
#if defined(__linux__) || defined(__APPLE__)
    struct statfs filesystem{};
    int status;
    do { status = ::statfs(path, &filesystem); } while (status != 0 && errno == EINTR);
    if (status != 0) return errno;
    type = static_cast<std::uint32_t>(filesystem.f_type);
    return 0;
#else
    (void)path;
    (void)type;
    return ENOTSUP;
#endif
}

// Empty return => refusal/error are populated. No directory is created here.
// A nonempty explicit path is never replaced or normalized. With no explicit
// path, retain the legacy TMPDIR-or-/tmp choice; only proven tmpfs/ramfs permits
// trying /var/tmp. Query errors retain their errno and stop resolution. /tmp is
// the legacy candidate only when TMPDIR is unset/empty.
// Probe has signature int(const char*, uint64_t&) and returns a syscall errno.
// The injected form lets tests cover a tmpfs /tmp without changing mounts.
template<class Probe>
inline std::string resolve_scratch_directory(const std::string& configured,
                                             std::string& refusal, int& error,
                                             Probe probe) {
    refusal.clear();
    error = 0;
    int query_error = 0;
    const auto reject = [&](const std::string& path, const char* source) {
        std::uint64_t type = 0;
        query_error = probe(path.c_str(), type);
        if (query_error == 0 && !scratch_filesystem_is_memory(type)) return false;
        if (!refusal.empty()) refusal += "; ";
        refusal += source;
        refusal += " '";
        refusal += path;
        refusal += "': ";
        if (query_error != 0) {
            error = query_error;
            refusal += "statfs failed (errno ";
            refusal += std::to_string(query_error);
            refusal += ": ";
            refusal += std::error_code(query_error, std::generic_category()).message();
            refusal += ')';
        } else {
            error = ENODEV;
            refusal += static_cast<std::uint32_t>(type) == UINT32_C(0x01021994)
                ? "tmpfs" : "ramfs";
            refusal += " requires disk backing, not tmpfs or ramfs";
        }
        return true;
    };
    if (!configured.empty()) {
        if (!reject(configured, "explicit directory")) return configured;
        refusal.insert(0, error == ENODEV ? "scratch requires disk backing; " : "scratch create; ");
        return {};
    }
    const char* temporary = std::getenv("TMPDIR");
    const bool has_temporary = temporary && *temporary;
    const std::string first = has_temporary ? temporary : "/tmp";
    if (!reject(first, has_temporary ? "TMPDIR" : "legacy /tmp (TMPDIR unset or empty)"))
        return first;
    // Only a successful tmpfs/ramfs probe (error == ENODEV) permits fallback;
    // even ENODEV from the probe itself is a hard query failure.
    if (query_error != 0) {
        refusal.insert(0, "scratch create; ");
        return {};
    }
    if (!reject("/var/tmp", "fallback")) {
        refusal.clear();
        error = 0;
        return "/var/tmp";
    }
    if (query_error != 0) {
        refusal.insert(0, "scratch create; ");
        return {};
    }
    // Both default candidates are proven memory-backed. Keep the established
    // errno/text and add every rejected candidate and a remedy.
    error = ENODEV;
    refusal.insert(0, "scratch requires disk backing; ");
    refusal += "; set TMPDIR to a disk-backed directory";
    return {};
}

inline std::string resolve_scratch_directory(const std::string& configured,
                                             std::string& refusal, int& error) {
    return resolve_scratch_directory(configured, refusal, error,
                                     scratch_directory_filesystem_type);
}

} // namespace pineforge::detail
