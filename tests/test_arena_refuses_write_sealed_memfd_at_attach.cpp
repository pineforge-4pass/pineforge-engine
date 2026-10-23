// D-08: exact seal-mask validation belongs to attach, before any writable map.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/detail/scratch_arena.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace pineforge::detail;
constexpr int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct OwnedFd {
    int value;
    ~OwnedFd() { if (value >= 0) ::close(value); }
};

struct AttachmentCleanup {
    struct sigaction previous{};
    AttachmentCleanup() {
        require(ScratchArena::attached() == nullptr, "test starts without an arena");
        require(::sigaction(SIGXFSZ, nullptr, &previous) == 0, "save signal disposition");
    }
    ~AttachmentCleanup() {
        ScratchArena::detach(); // this test never opens an arena region
        ::sigaction(SIGXFSZ, &previous, nullptr);
    }
};

void check_mask(int seals) {
    constexpr auto bytes = ScratchArena::kChunkBytes;
    OwnedFd backing{::memfd_create("pineforge-d08", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    require(backing.value >= 0, "fixture creates a sealable memfd");
    require(::ftruncate(backing.value, static_cast<off_t>(bytes)) == 0, "fixture sizes the memfd");
    if (seals != 0)
        require(::fcntl(backing.value, F_ADD_SEALS, seals) == 0, "fixture applies the requested seals");
    require(::fcntl(backing.value, F_GET_SEALS) == seals, "fixture has the exact requested seal mask");

    if (seals == required_seals || seals == F_SEAL_SEAL) {
        ScratchArena::attach(backing.value, bytes);
        require(ScratchArena::attached() != nullptr, "documented mask is accepted");
        const auto expected = seals == required_seals
            ? ScratchArenaBackend::sealed_memfd : ScratchArenaBackend::regular_file;
        require(ScratchArena::attached()->stats().backend == expected,
                "documented mask selects its documented backend");
        ScratchArena::detach();
        return;
    }

    bool refused = false;
    try {
        ScratchArena::attach(backing.value, bytes);
    } catch (const ReportScratchIoError& failure) {
        const auto& typed = failure.run_failure();
        require(failure.code().value() == EINVAL, "bad seal mask is invalid backing");
        require(typed.code == pineforge::RunFailureCode::report_sink_failed,
                "bad seal mask carries the report scratch failure code");
        require(typed.args && *typed.args == "{\"reason\":\"arena_backing\"}",
                "bad seal mask is refused with arena_backing at attach");
        refused = true;
    }
    require(refused, "attach must refuse every undocumented seal mask");
    require(ScratchArena::attached() == nullptr, "refused attach publishes no arena");
    require(::fcntl(backing.value, F_GET_SEALS) == seals, "refusal does not change caller seals");
    require(::fcntl(backing.value, F_GETFD) >= 0, "refusal does not close the caller fd");
}
} // namespace

int main() {
    try {
        AttachmentCleanup cleanup;
        require(::unsetenv("PF_SCRATCH_WINDOW_BYTES") == 0, "use the documented default window cap");
        // A positive control prevents unrelated setup failures from being
        // mistaken for the expected backing refusal.
        check_mask(required_seals);
        check_mask(required_seals | F_SEAL_WRITE); // first red on base
        constexpr int bits[]{F_SEAL_GROW, F_SEAL_SHRINK, F_SEAL_SEAL, F_SEAL_WRITE,
#if defined(F_SEAL_FUTURE_WRITE)
                             F_SEAL_FUTURE_WRITE,
#endif
        };
        constexpr auto count = sizeof(bits) / sizeof(bits[0]);
        for (unsigned subset = 0; subset < (1U << count); ++subset) {
            int seals = 0;
            for (unsigned bit = 0; bit < count; ++bit)
                if ((subset & (1U << bit)) != 0) seals |= bits[bit];
            check_mask(seals);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "D-08: %s\n", error.what());
        return 1;
    }
}
