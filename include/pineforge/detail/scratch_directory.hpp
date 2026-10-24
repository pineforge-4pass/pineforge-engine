#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#elif defined(__APPLE__)
#include <sys/param.h>
#include <sys/mount.h>
#endif

namespace pineforge::detail {

inline const char* scratch_memory_filesystem_name(std::uint64_t type) noexcept {
    // Normalize f_type on platforms where the filesystem word is signed.
    const auto magic = static_cast<std::uint32_t>(type);
    if (magic == UINT32_C(0x01021994)) return "tmpfs";
    if (magic == UINT32_C(0x858458f6)) return "ramfs";
    return nullptr;
}

inline bool scratch_filesystem_is_memory(std::uint64_t type) noexcept {
    return scratch_memory_filesystem_name(type) != nullptr;
}

// Keep the historical operation alone unless a selection note needs the
// selected directory too. Shared by both file-create implementations.
inline std::string scratch_create_message(const char* operation,
                                          const std::string& directory,
                                          const std::string& selection_note) {
    if (selection_note.empty()) return operation;
    return std::string(operation) + "; directory '" + directory + "'; " + selection_note;
}

// Translate only create selection/refusal notes, never an inner operation or
// its system_error suffix. The outer typed error supplies its own suffix.
inline std::string scratch_failure_detail(const char* what, int errno_value) {
    std::string message(what);
    const auto suffix = ": " + std::error_code(errno_value, std::generic_category()).message();
    if (message.size() >= suffix.size()
        && message.compare(message.size() - suffix.size(), suffix.size(), suffix) == 0)
        message.resize(message.size() - suffix.size());
    constexpr char create_prefix[] = "scratch create; ";
    constexpr char refusal_prefix[] = "scratch requires disk backing; ";
    if (message.compare(0, sizeof(create_prefix) - 1, create_prefix) == 0)
        return message.substr(sizeof(create_prefix) - 1);
    // The refusal itself is part of the note; retain its established wording.
    if (message.compare(0, sizeof(refusal_prefix) - 1, refusal_prefix) == 0)
        return message;
    return {};
}

inline std::string scratch_with_detail(const char* prefix, const char* what, int errno_value) {
    const auto detail = scratch_failure_detail(what, errno_value);
    return detail.empty() ? std::string(prefix) : std::string(prefix) + "; " + detail;
}

// Return an errno, not a boolean: a failed query is not proof of disk backing.
inline int scratch_directory_filesystem_type(const char* path, std::uint64_t& type) noexcept {
#if defined(__linux__) || defined(__APPLE__)
    // A filesystem type is evidence only for a searchable directory. In
    // particular, statfs("/dev/null") can report tmpfs rather than ENOTDIR.
    // Do not require read permission: a write/search-only directory is usable.
    struct stat directory{};
    int status;
    do { status = ::stat(path, &directory); } while (status != 0 && errno == EINTR);
    if (status != 0) return errno;
    if (!S_ISDIR(directory.st_mode)) return ENOTDIR;
#if defined(AT_EACCESS)
    // Match the effective credentials used by mkstemp.
    do { status = ::faccessat(AT_FDCWD, path, X_OK, AT_EACCESS); }
    while (status != 0 && errno == EINTR);
#else
    // Without AT_EACCESS, retain the available real-id search-permission check.
    do { status = ::access(path, X_OK); } while (status != 0 && errno == EINTR);
#endif
    if (status != 0) return errno;
    struct statfs filesystem{};
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

// Empty return => refusal/error are populated. proven_memory distinguishes a
// successful memory-filesystem query from a query that itself returns ENODEV.
// A successful fallback retains its selection note in refusal for a later
// create failure; accepted first/explicit candidates have no rejection note.
// No directory is created here.
// A nonempty explicit path is never replaced or normalized. With no explicit
// path, retain the legacy TMPDIR-or-/tmp choice; only proven tmpfs/ramfs permits
// trying /var/tmp. Default query errors stop with their errno; explicit query
// errors leave creation to the caller. /tmp is used only if TMPDIR is unset/empty.
// Probe has signature int(const char*, uint64_t&) and returns a syscall errno.
// The injected form lets tests cover a tmpfs /tmp without changing mounts.
template<class Probe>
inline std::string resolve_scratch_directory(const std::string& configured,
                                             std::string& refusal, int& error,
                                             bool& proven_memory, Probe probe) {
    refusal.clear();
    error = 0;
    proven_memory = false;
    int query_error = 0;
    const auto reject = [&](const std::string& path, const char* source) {
        std::uint64_t type = 0;
        query_error = probe(path.c_str(), type);
        proven_memory = query_error == 0 && scratch_filesystem_is_memory(type);
        if (query_error == 0 && !proven_memory) return false;
        if (!refusal.empty()) refusal += "; ";
        refusal += source;
        refusal += " '";
        refusal += path;
        refusal += "': ";
        if (query_error != 0) {
            error = query_error;
            refusal += "directory check failed (errno ";
            refusal += std::to_string(query_error);
            refusal += ": ";
            refusal += std::error_code(query_error, std::generic_category()).message();
            refusal += ')';
        } else {
            error = ENODEV;
            refusal += scratch_memory_filesystem_name(type);
            refusal += " requires disk backing, not tmpfs or ramfs";
        }
        return true;
    };
    if (!configured.empty()) {
        if (!reject(configured, "explicit directory")) return configured;
        // Preserve the caller's original create errno for an invalid explicit
        // path. A query failure is no reason to refuse or redirect that path.
        if (query_error != 0) {
            refusal.clear();
            error = 0;
            return configured;
        }
        refusal.insert(0, "scratch requires disk backing; ");
        return {};
    }
    const char* temporary = std::getenv("TMPDIR");
    const bool has_temporary = temporary && *temporary;
    const std::string first = has_temporary ? temporary : "/tmp";
    if (!reject(first, has_temporary ? "TMPDIR" : "legacy /tmp (TMPDIR unset or empty)"))
        return first;
    // Only a successful tmpfs/ramfs probe permits fallback; even ENODEV from
    // the probe itself is a hard query failure, with proven_memory == false.
    if (query_error != 0) {
        refusal.insert(0, "scratch create; ");
        return {};
    }
    if (!reject("/var/tmp", "fallback")) {
        refusal += "; selected fallback '/var/tmp'; set TMPDIR to a disk-backed directory";
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
                                             std::string& refusal, int& error,
                                             bool& proven_memory) {
    return resolve_scratch_directory(configured, refusal, error, proven_memory,
                                     scratch_directory_filesystem_type);
}

} // namespace pineforge::detail
