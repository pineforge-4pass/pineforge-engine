// CR-150 L19l part B. This unit intentionally includes the new storage
// header: record the missing-header fail-before on the lab's frozen closure.
#include <pineforge/detail/kernel_roster_store.hpp>
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using Store = pineforge::detail::KernelRosterStore;
namespace no = pineforge::native_order;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ScratchDirectory {
    std::string path = "/tmp/pineforge-kernel-rosters-test-XXXXXX";
    ScratchDirectory() {
        require(::mkdtemp(path.data()) != nullptr, "test scratch directory created");
    }
    ~ScratchDirectory() { ::rmdir(path.c_str()); }
};

int roster_descriptor(off_t expected_bytes) {
    DIR* descriptors = ::opendir("/dev/fd");
    require(descriptors != nullptr, "descriptor directory opened");
    int result = -1;
    while (const auto* entry = ::readdir(descriptors)) {
        char* end = nullptr;
        const long number = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || number < 0
            || number > std::numeric_limits<int>::max()) continue;
        const int fd = static_cast<int>(number);
        struct stat status{};
        std::uint64_t first = 0;
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode)
            && status.st_nlink == 0 && status.st_size == expected_bytes
            && ::pread(fd, &first, sizeof first, 0) == static_cast<ssize_t>(sizeof first)
            && first == 1) {
            result = fd;
            break;
        }
    }
    ::closedir(descriptors);
    require(result >= 0, "anonymous kernel roster file found");
    return result;
}

void sorted_store_matches_retained_handles() {
    ScratchDirectory scratch;
    Store store;
    store.set_directory(scratch.path);
    std::vector<std::uint64_t> reference;
    require(store.size() == reference.size(), "new store has no handles");
    require(!store.contains(0) && !store.contains(1), "new store rejects unallocated handles");
    for (std::uint64_t handle = 1; handle <= 16384; ++handle) {
        store.append(handle);
        reference.push_back(handle);
        require(store.size() == reference.size(), "count equals the in-memory table");
        for (const std::uint64_t key : {std::uint64_t{0}, std::uint64_t{1},
                                       handle / 2, handle, handle + 1}) {
            const bool expected = std::binary_search(reference.begin(), reference.end(), key);
            require(store.contains(key) == expected, "tail and disk lookup equal the in-memory table");
        }
    }
    for (std::uint64_t i = 0; i < reference.size(); ++i) {
        const auto key = reference[static_cast<std::size_t>((i * 257) % reference.size())];
        require(store.contains(key), "every old handle can recur in a permuted lookup");
    }
    const auto count = store.size();
    bool failed = false;
    try {
        store.append(1);
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed && store.size() == count, "duplicate append cannot renumber or alias a cohort");
}

void layout_cursor_and_cleanup() {
    ScratchDirectory scratch;
    int fd = -1;
    {
        Store store;
        store.set_directory(scratch.path);
        for (std::uint64_t i = 1; i <= 65; ++i) store.append(i);
        fd = roster_descriptor(64 * 8);
        require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "scratch file closes on exec");
        struct stat status{};
        require(::fstat(fd, &status) == 0 && (status.st_mode & 0777) == 0600,
                "scratch file is private");
        std::array<std::uint64_t, 64> rows{};
        require(::pread(fd, rows.data(), sizeof rows, 0) == static_cast<ssize_t>(sizeof rows),
                "sorted handle batch is readable");
        for (std::size_t i = 0; i < rows.size(); ++i) {
            require(rows[i] == i + 1, "file contains exactly one eight-byte row per handle");
        }
        require(::lseek(fd, 7, SEEK_SET) == 7, "test moves the file cursor");
        require(store.contains(1) && store.contains(64) && store.contains(65),
                "both spilled handles and tail handles are found");
        require(::lseek(fd, 0, SEEK_CUR) == 7, "pread lookup does not move the file cursor");
        bool refused = false;
        try {
            store.set_directory(scratch.path + "/different");
        } catch (const std::logic_error&) {
            refused = true;
        }
        require(refused, "directory cannot change after a spill");
    }
    errno = 0;
    require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "destruction closes the anonymous file");
}

void failed_create_keeps_handles_and_numbering() {
    ScratchDirectory scratch;
    no::WorkingRequestCore core{{"roster-create", 1}};
    core.set_successor_roots_directory(scratch.path + "/missing");
    for (int i = 0; i < 64; ++i) (void)core.cohort_open();
    bool failed = false;
    try {
        (void)core.cohort_open();
    } catch (const std::system_error& error) {
        failed = error.code().value() == ENOENT;
    }
    require(failed && core.cohorts().size() == 64, "create failure leaves logical rosters unchanged");
    core.set_successor_roots_directory(scratch.path);
    require(core.cohort_open() == no::CohortHandle{65}, "create retry consumes the same handle");
    const int fd = roster_descriptor(64 * 8);
    core.reset({"roster-create", 2});
    errno = 0;
    require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "kernel reset closes its roster file");
}

struct FileSizeLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler handler = SIG_DFL;
    FileSizeLimit() {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file size limit");
        handler = std::signal(SIGXFSZ, SIG_IGN);
        require(handler != SIG_ERR, "ignore file size signal");
        struct rlimit limit = previous;
        limit.rlim_cur = 12; // One whole handle and half the second handle.
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::signal(SIGXFSZ, handler);
            throw std::runtime_error("set file size limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, handler);
    }
};

void partial_write_does_not_publish_handles() {
    ScratchDirectory scratch;
    Store store;
    store.set_directory(scratch.path);
    for (std::uint64_t i = 1; i <= 64; ++i) store.append(i);
    bool failed = false;
    {
        FileSizeLimit limit;
        try {
            store.append(65);
        } catch (const std::system_error& error) {
            failed = error.code().value() == EFBIG;
        }
    }
    require(failed && store.size() == 64, "partial batch does not publish the next handle");
    const int fd = roster_descriptor(12);
    for (std::uint64_t i = 1; i <= 64; ++i) require(store.contains(i), "authoritative tail survives short write");
    require(!store.contains(65), "uncommitted handle cannot be found");
    store.append(65);
    require(roster_descriptor(64 * 8) == fd && store.size() == 65,
            "retry overwrites the partial batch and preserves numbering");
    require(store.contains(64) && store.contains(65), "retry preserves disk and tail handles");
}

void truncated_or_corrupt_identity_is_an_io_failure() {
    ScratchDirectory scratch;
    {
        Store store;
        store.set_directory(scratch.path);
        for (std::uint64_t i = 1; i <= 65; ++i) store.append(i);
        const int fd = roster_descriptor(64 * 8);
        require(::ftruncate(fd, 63 * 8 + 4) == 0, "test truncates the last handle");
        bool failed = false;
        try {
            (void)store.contains(64);
        } catch (const std::system_error& error) {
            failed = error.code().value() == EIO;
        }
        require(failed, "short pread cannot silently invent an absent cohort");
    }
    {
        Store store;
        store.set_directory(scratch.path);
        for (std::uint64_t i = 1; i <= 65; ++i) store.append(i);
        const int fd = roster_descriptor(64 * 8);
        const std::uint64_t wrong = 0;
        require(::pwrite(fd, &wrong, sizeof wrong, 32 * 8) == static_cast<ssize_t>(sizeof wrong),
                "test corrupts a handle in the file");
        bool failed = false;
        try {
            (void)store.contains(33);
        } catch (const std::system_error& error) {
            failed = error.code().value() == EIO;
        }
        require(failed, "a malformed row is a typed I/O error");
    }
}

} // namespace

int main() {
    try {
        sorted_store_matches_retained_handles();
        layout_cursor_and_cleanup();
        failed_create_keeps_handles_and_numbering();
        partial_write_does_not_publish_handles();
        truncated_or_corrupt_identity_is_an_io_failure();
        std::puts("kernel roster store: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "kernel roster store: %s\n", error.what());
        return 1;
    }
}
