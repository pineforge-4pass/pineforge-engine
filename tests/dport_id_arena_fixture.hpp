#pragma once

#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#include <dirent.h>
#include <map>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif

namespace dport_id_fixture {
using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchIoError;
constexpr auto kChunk = ScratchArena::kChunkBytes;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline std::uint64_t rounded(std::uint64_t bytes) {
    return (bytes / kChunk + (bytes % kChunk != 0 ? 1 : 0)) * kChunk;
}

struct Directory {
    std::string path = "/tmp/pineforge-dport-ids-XXXXXX";
    Directory() {
        require(::mkdtemp(path.data()) != nullptr, "create test scratch directory");
    }
    ~Directory() { ::rmdir(path.c_str()); }
};

struct Environment {
    const char* key;
    bool present;
    std::string previous;
    Environment(const char* name, const char* value)
        : key(name), present(std::getenv(name) != nullptr),
          previous(present ? std::getenv(name) : "") {
        require((value ? ::setenv(key, value, 1) : ::unsetenv(key)) == 0,
                "set test environment");
    }
    ~Environment() {
        if (present) ::setenv(key, previous.c_str(), 1);
        else ::unsetenv(key);
    }
};

template<class F> void exhausted(F&& operation) {
    bool refused = false;
    try { operation(); }
    catch (const ScratchIoError& error) {
        refused = (error.code().value() == ENOSPC || error.code().value() == EFBIG)
            && pineforge::classify_run_failure(error).code
                == pineforge::RunFailureCode::report_sink_failed;
    }
    require(refused, "arena exhaustion is typed scratch I/O failure");
}

#if defined(__linux__)
struct Backing {
    int fd = -1;
    std::uint64_t bytes;
    explicit Backing(std::uint64_t size) : bytes(size) {
        require(size != 0 && size <= static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max()), "representable computed arena size");
        const int created = ::memfd_create("pineforge-dport-ids",
                                           MFD_ALLOW_SEALING | MFD_CLOEXEC);
        require(created >= 0, "parent creates memfd");
        fd = created;
        if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0
            || ::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
            ::close(std::exchange(fd, -1));
            throw std::runtime_error("parent sizes and seals memfd before jail");
        }
    }
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;
    ~Backing() { if (fd >= 0) ::close(fd); }
};

struct Attachment {
    explicit Attachment(const Backing& backing) {
        ScratchArena::attach(backing.fd, backing.bytes);
    }
    ~Attachment() { ScratchArena::detach(); }
};

// Include the supplied memfd and the arena's duplicate in the baseline.
// An unlinked scratch file is still regular and cannot hide from this check.
class RegularFiles {
    using Identity = std::pair<dev_t, ino_t>;
    std::map<int, Identity> baseline_;

    template<class F> static void scan(F&& visit) {
        DIR* directory = ::opendir("/proc/self/fd");
        require(directory != nullptr, "open descriptor census");
        try {
            while (const auto* entry = ::readdir(directory)) {
                char* end = nullptr;
                const long number = std::strtol(entry->d_name, &end, 10);
                if (end == entry->d_name || *end != '\0' || number < 0
                    || number > std::numeric_limits<int>::max()) continue;
                const auto fd = static_cast<int>(number);
                struct stat status{};
                if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode))
                    visit(fd, status);
            }
        } catch (...) {
            ::closedir(directory);
            throw;
        }
        ::closedir(directory);
    }
public:
    RegularFiles() {
        scan([&](int fd, const struct stat& status) {
            baseline_.emplace(fd, Identity{status.st_dev, status.st_ino});
        });
    }
    void check() const {
        std::size_t count = 0;
        scan([&](int fd, const struct stat& status) {
            ++count;
            const auto found = baseline_.find(fd);
            require(found != baseline_.end()
                    && found->second == Identity{status.st_dev, status.st_ino},
                    "arena operation created a regular-file descriptor");
        });
        require(count == baseline_.size(), "regular-file descriptor count is unchanged");
    }
};

inline void jail() {
    require(::signal(SIGXFSZ, SIG_IGN) != SIG_ERR, "ignore SIGXFSZ in child");
    const struct rlimit file_limit{0, 0};
    require(::setrlimit(RLIMIT_FSIZE, &file_limit) == 0, "apply RLIMIT_FSIZE zero");
    // Include existing sanitizer shadow mappings in the baseline, then
    // permit only a fixed 96 MiB of additional address space in every build.
    std::FILE* statm = std::fopen("/proc/self/statm", "r");
    require(statm != nullptr, "read virtual-memory baseline");
    unsigned long long pages = 0;
    const int parsed = std::fscanf(statm, "%llu", &pages);
    std::fclose(statm);
    const long page = ::sysconf(_SC_PAGESIZE);
    require(parsed == 1 && page > 0, "parse virtual-memory baseline");
    const auto baseline = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
    const auto bound = static_cast<rlim_t>(baseline + 96ULL * 1024ULL * 1024ULL);
    struct rlimit limit{};
    require(::getrlimit(RLIMIT_AS, &limit) == 0, "read RLIMIT_AS");
    limit.rlim_cur = std::min(limit.rlim_cur, bound);
    limit.rlim_max = std::min(limit.rlim_max, bound);
    require(::setrlimit(RLIMIT_AS, &limit) == 0, "apply bounded RLIMIT_AS");
}

template<class F> void child(F&& operation) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork arena witness");
    if (pid == 0) {
        try {
            jail();
            operation();
            ::_exit(0);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "dport id child: %s\n", error.what());
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "jailed arena child completes successfully");
}
#endif
} // namespace dport_id_fixture
