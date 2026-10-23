// CR-267. Compile this test against b3d483d6c1 before applying the engine fix.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <cstdio>

#if defined(__linux__)
#include <pineforge/detail/scratch_arena.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace {
using pineforge::detail::ReportScratchIoError;
using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchArenaBackend;
constexpr int kFd = ScratchArena::kInheritedFd;
constexpr std::uint64_t kBytes = 2 * ScratchArena::kChunkBytes;
constexpr int kSeals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;

void check(bool value, const char* expression, int line) {
    if (!value)
        throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct OwnedFd {
    int value;
    explicit OwnedFd(int fd = -1) noexcept : value(fd) {}
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    ~OwnedFd() { if (value >= 0) ::close(value); }
};

OwnedFd keep_away_from_inherited_slot(OwnedFd fd) {
    if (fd.value != kFd) return fd;
    OwnedFd moved(::fcntl(fd.value, F_DUPFD_CLOEXEC, kFd + 1));
    CHECK(moved.value >= 0);
    return moved;
}

struct InheritedFd {
    int previous_flags;
    OwnedFd previous;
    explicit InheritedFd(int fd)
        : previous_flags(::fcntl(kFd, F_GETFD)),
          previous(previous_flags >= 0 ? ::fcntl(kFd, F_DUPFD_CLOEXEC, kFd + 1) : -1) {
        CHECK(previous_flags >= 0 || errno == EBADF);
        CHECK(previous_flags < 0 || previous.value >= 0);
        CHECK(fd != kFd);
        CHECK(::dup2(fd, kFd) == kFd);
    }
    ~InheritedFd() {
        if (previous.value >= 0) {
            ::dup2(previous.value, kFd);
            ::fcntl(kFd, F_SETFD, previous_flags);
        } else {
            ::close(kFd);
        }
    }
};

struct Environment {
    const char* key;
    bool present;
    std::string previous;
    Environment(const char* name, const char* value)
        : key(name), present(std::getenv(name) != nullptr),
          previous(present ? std::getenv(name) : "") {
        CHECK((value ? ::setenv(key, value, 1) : ::unsetenv(key)) == 0);
    }
    ~Environment() {
        if (present) ::setenv(key, previous.c_str(), 1);
        else ::unsetenv(key);
    }
};

struct DefaultSigxfsz {
    struct sigaction previous{};
    DefaultSigxfsz() {
        struct sigaction disposition{};
        disposition.sa_handler = SIG_DFL;
        CHECK(::sigemptyset(&disposition.sa_mask) == 0);
        CHECK(::sigaction(SIGXFSZ, &disposition, &previous) == 0);
    }
    ~DefaultSigxfsz() { ::sigaction(SIGXFSZ, &previous, nullptr); }
};

struct TemporaryDirectory {
    std::string path;
    TemporaryDirectory() {
        const char* temporary = std::getenv("TMPDIR");
        path = std::string(temporary && *temporary ? temporary : ".")
            + "/pineforge-cr267-XXXXXX";
        CHECK(::mkdtemp(path.data()) != nullptr);
    }
    ~TemporaryDirectory() { ::rmdir(path.c_str()); }
};

OwnedFd memfd_backing(unsigned flags, int seals) {
    OwnedFd fd(::memfd_create("pineforge-cr267", flags | MFD_CLOEXEC));
    CHECK(fd.value >= 0);
    CHECK(::ftruncate(fd.value, static_cast<off_t>(kBytes)) == 0);
    if (seals != 0) CHECK(::fcntl(fd.value, F_ADD_SEALS, seals) == 0);
    const int expected = (flags & MFD_ALLOW_SEALING) ? seals : F_SEAL_SEAL;
    CHECK(::fcntl(fd.value, F_GET_SEALS) == expected);
    return keep_away_from_inherited_slot(std::move(fd));
}

OwnedFd regular_backing() {
    TemporaryDirectory directory;
    std::string path = directory.path + "/arena-XXXXXX";
    OwnedFd fd(::mkstemp(path.data()));
    CHECK(fd.value >= 0);
    CHECK(::unlink(path.c_str()) == 0);
    CHECK(::ftruncate(fd.value, static_cast<off_t>(kBytes)) == 0);
    errno = 0;
    const int seals = ::fcntl(fd.value, F_GET_SEALS);
    CHECK((seals == -1 && errno == EINVAL) || seals == F_SEAL_SEAL);
    return keep_away_from_inherited_slot(std::move(fd));
}

std::size_t open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    CHECK(directory != nullptr);
    std::size_t count = 0;
    errno = 0;
    while (const auto* entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") != 0
            && std::strcmp(entry->d_name, "..") != 0)
            ++count;
    }
    const int read_error = errno;
    const int close_result = ::closedir(directory);
    CHECK(read_error == 0);
    CHECK(close_result == 0);
    return count;
}

template<class F>
void expect_backing_refusal(F&& operation, ScratchArena* previous = nullptr,
                            const char* message = nullptr) {
    const auto descriptors = open_fd_count();
    DefaultSigxfsz signal;
    bool caught = false;
    try { operation(); }
    catch (const ReportScratchIoError& failure) {
        caught = true;
        CHECK(failure.code().value() == EINVAL);
        const auto typed = pineforge::classify_run_failure(failure);
        CHECK(typed.code == pineforge::RunFailureCode::report_sink_failed);
        CHECK(typed.args && *typed.args == "{\"reason\":\"arena_backing\"}");
        if (message) CHECK(std::strstr(failure.what(), message) != nullptr);
    }
    CHECK(caught);
    CHECK(ScratchArena::attached() == previous);
    CHECK(open_fd_count() == descriptors);
    struct sigaction disposition{};
    CHECK(::sigaction(SIGXFSZ, nullptr, &disposition) == 0);
    CHECK(disposition.sa_handler == SIG_DFL);
}

void expect_attached(ScratchArenaBackend backend) {
    auto* arena = ScratchArena::attached();
    CHECK(arena != nullptr);
    const auto stats = arena->stats();
    CHECK(stats.backend == backend);
    CHECK(stats.bytes_supplied == kBytes);
    CHECK(stats.bytes_used == 0);
    CHECK(stats.windows_mapped_bytes == 0);
    ScratchArena::attach_from_environment();
    CHECK(ScratchArena::attached() == arena);
}

void attachment_case(const OwnedFd& backing, const char* profile, bool accepted,
                     ScratchArenaBackend backend = ScratchArenaBackend::regular_file,
                     std::uint64_t bytes = kBytes) {
    InheritedFd inherited(backing.value);
    Environment fd("PF_SCRATCH_ARENA_FD", "5");
    Environment size("PF_SCRATCH_ARENA_BYTES", std::to_string(bytes).c_str());
    Environment jailed("PF_SCRATCH_ARENA_JAILED", profile);
    const auto descriptors = open_fd_count();
    if (accepted) {
        ScratchArena::attach_from_environment();
        expect_attached(backend);
    } else {
        expect_backing_refusal([] { ScratchArena::attach_from_environment(); });
    }
    ScratchArena::detach();
    CHECK(ScratchArena::attached() == nullptr);
    CHECK(open_fd_count() == descriptors);
}

void reattach_case(const OwnedFd& backing, const OwnedFd* rebound = nullptr) {
    InheritedFd inherited(backing.value);
    Environment fd("PF_SCRATCH_ARENA_FD", "5");
    Environment size("PF_SCRATCH_ARENA_BYTES", std::to_string(kBytes).c_str());
    Environment jailed("PF_SCRATCH_ARENA_JAILED", "1");
    // Direct attach remains unjailed, including its idempotent no-op.
    ScratchArena::attach(kFd, kBytes);
    auto* arena = ScratchArena::attached();
    CHECK(arena != nullptr);
    CHECK(arena->stats().backend == ScratchArenaBackend::regular_file);
    ScratchArena::attach(kFd, kBytes);
    CHECK(ScratchArena::attached() == arena);
    const auto refuse = [&] {
        expect_backing_refusal([] { ScratchArena::attach_from_environment(); }, arena);
        CHECK(arena->stats().backend == ScratchArenaBackend::regular_file);
        CHECK(arena->stats().windows_mapped_bytes == 0);
    };
    if (rebound) {
        // A new, sealed fd 5 must not disguise the live regular-file backend.
        InheritedFd replacement(rebound->value);
        refuse();
    } else {
        refuse();
    }
    ScratchArena::detach();
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const std::string& name, auto&& operation) {
        try {
            CHECK(ScratchArena::attached() == nullptr);
            operation();
            ScratchArena::detach();
            CHECK(ScratchArena::attached() == nullptr);
            std::printf("PASS %s\n", name.c_str());
        } catch (const std::exception& failure) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), failure.what());
            ScratchArena::detach();
        }
    };
    try {
        Environment cap("PF_SCRATCH_WINDOW_BYTES", nullptr);
        auto sealed = memfd_backing(MFD_ALLOW_SEALING, kSeals);
        auto disabled = memfd_backing(0, 0);
        auto unsealed = memfd_backing(MFD_ALLOW_SEALING, 0);
        auto grow = memfd_backing(MFD_ALLOW_SEALING, F_SEAL_GROW);
        auto shrink = memfd_backing(MFD_ALLOW_SEALING, F_SEAL_SHRINK);
        auto mutable_seals = memfd_backing(MFD_ALLOW_SEALING, F_SEAL_GROW | F_SEAL_SHRINK);
        auto regular = regular_backing();

        run("a/jailed/sealed-memfd", [&] {
            attachment_case(sealed, "1", true, ScratchArenaBackend::sealed_memfd);
        });
        run("b/jailed/sealing-disabled-memfd", [&] { attachment_case(disabled, "1", false); });
        run("c/jailed/no-seals", [&] { attachment_case(unsealed, "1", false); });
        run("d/jailed/grow-only", [&] { attachment_case(grow, "1", false); });
        run("d/jailed/shrink-only", [&] { attachment_case(shrink, "1", false); });
        run("d/jailed/grow-shrink-without-seal", [&] {
            attachment_case(mutable_seals, "1", false);
        });
        run("e/jailed/ordinary-file", [&] { attachment_case(regular, "1", false); });
        for (const char* profile : {static_cast<const char*>(nullptr), "0"}) {
            const std::string prefix = std::string("f/") + (profile ? "zero/" : "unset/");
            run(prefix + "sealing-disabled-memfd", [&] {
                attachment_case(disabled, profile, true);
            });
            run(prefix + "ordinary-file", [&] { attachment_case(regular, profile, true); });
            run(prefix + "unsealed-memfd", [&] { attachment_case(unsealed, profile, false); });
            run(prefix + "sealed-memfd", [&] {
                attachment_case(sealed, profile, true, ScratchArenaBackend::sealed_memfd);
            });
        }
        for (const char* profile : {"", "2", "yes", " 1", "01", "true"}) {
            run(std::string("g/malformed-profile='") + profile + "'", [&] {
                attachment_case(sealed, profile, false);
            });
        }
        run("h/jailed/no-fd", [] {
            Environment fd("PF_SCRATCH_ARENA_FD", nullptr);
            Environment size("PF_SCRATCH_ARENA_BYTES", "not-a-size");
            Environment jailed("PF_SCRATCH_ARENA_JAILED", "1");
            ScratchArena::attach_from_environment();
            CHECK(ScratchArena::attached() == nullptr);
        });
        run("h/jailed/wrong-size", [&] {
            InheritedFd inherited(sealed.value);
            Environment fd("PF_SCRATCH_ARENA_FD", "5");
            Environment size("PF_SCRATCH_ARENA_BYTES",
                             std::to_string(kBytes + ScratchArena::kChunkBytes).c_str());
            Environment jailed("PF_SCRATCH_ARENA_JAILED", "1");
            expect_backing_refusal([] { ScratchArena::attach_from_environment(); },
                                   nullptr, "scratch arena backing");
        });
        run("reattach/sealed-with-closed-inherited-slot", [&] {
            InheritedFd inherited(sealed.value);
            Environment fd("PF_SCRATCH_ARENA_FD", "5");
            Environment size("PF_SCRATCH_ARENA_BYTES", std::to_string(kBytes).c_str());
            Environment jailed("PF_SCRATCH_ARENA_JAILED", "1");
            ScratchArena::attach_from_environment();
            CHECK(::close(kFd) == 0);
            expect_attached(ScratchArenaBackend::sealed_memfd);
            ScratchArena::detach();
        });
        run("reattach/direct-disabled-memfd", [&] { reattach_case(disabled); });
        run("reattach/direct-ordinary-file", [&] { reattach_case(regular); });
        run("reattach/regular-with-rebound-sealed-slot", [&] { reattach_case(regular, &sealed); });
        CHECK(ScratchArena::attached() == nullptr);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "FAIL fixture/setup: %s\n", failure.what());
        ScratchArena::detach();
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
#else
int main() {
    std::puts("SKIP test_scratch_arena_jailed_seals: Linux memfd seals are required");
    return 77;
}
#endif
