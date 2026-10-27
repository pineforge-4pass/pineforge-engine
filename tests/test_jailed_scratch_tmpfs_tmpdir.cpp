// Negative control: sealed-arena admission and a real fd-sink native run,
// with strict private-TMPDIR inotify and own-process mkstemp attribution.
#define PINEFORGE_TEST_SCRATCH_ATTRIBUTION 1
#include "default_scratch_tmpfs_fixture.hpp"

#if defined(__linux__)
#include <sys/mman.h>

namespace {
using namespace s4e_tmp;
constexpr std::uint64_t arena_bytes = 32 * 1024 * 1024;

class InheritedArena {
public:
    InheritedArena() {
        saved_.value = ::fcntl(5, F_DUPFD_CLOEXEC, 16);
        require(saved_.value >= 0 || errno == EBADF, "cannot preserve the inherited arena slot");
        Fd source(::memfd_create("s4e-jailed-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        require(source.value >= 0 && ::ftruncate(source.value, arena_bytes) == 0,
                "cannot create the jailed test arena");
        require(::fcntl(source.value, F_ADD_SEALS,
                        F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                "cannot seal the jailed test arena");
        require(::dup2(source.value, 5) == 5, "cannot install the inherited arena at fd 5");
        if (source.value == 5) source.value = -1;
    }
    ~InheritedArena() {
        ::close(5);
        if (saved_.value >= 0) ::dup2(saved_.value, 5);
    }
private:
    Fd saved_;
};

void row() {
    require_filesystems(false);
    PlainEnvironment environment;
    // Reserve fd 5 before the inotify and pipe descriptors are opened.
    InheritedArena backing;
    CreationWatch watch;
    {
        Environment jailed_missing("PF_SCRATCH_ARENA_JAILED", "1");
        Host missing;
        require(strategy_require_report_scratch_arena_v1(missing.handle()) != 0,
                "jailed admission without an arena unexpectedly succeeded");
        require(std::string(pineforge::run_failure_code_of(missing)) == "report_sink_failed"
                    && std::string(pineforge::run_failure_args_of(missing))
                        == "{\"reason\":\"arena_backing\"}",
                "missing jailed arena did not retain its admission failure");
    }
    Environment arena_fd("PF_SCRATCH_ARENA_FD", "5");
    Environment bytes("PF_SCRATCH_ARENA_BYTES", "33554432");
    Environment jailed("PF_SCRATCH_ARENA_JAILED", "1");
    Environment window("PF_SCRATCH_WINDOW_BYTES", "1048576");
    std::array<int, 2> pair{};
    require(::pipe(pair.data()) == 0, "cannot create the jailed report pipe");
    Fd reader(pair[0]), writer(pair[1]);
    const auto before = descriptor_count();
    {
        Host host;
        require(strategy_require_report_scratch_arena_v1(host.handle()) == 0,
                "sealed jailed arena admission failed");
        exercise_command_scratch(pineforge::detail::ScratchBackingKind::memfd);
        const pf_report_sink_fd_v1 sink{sizeof sink, 1, writer.value, "s4e-jailed"};
        require(host.set_report_sink_fd_v1(&sink) == 0, "jailed fd sink setter failed");
        run(host);
        require_success(host);
        Report report(host);
        pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
        require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1
                    && receipt.count_t == 1 && receipt.count_e == 8,
                "jailed run did not complete the real trade and equity sink");
        require(report.value.trades == nullptr && report.value.trades_len == 0,
                "jailed fd sink restored owning trade arrays");
        require_success(host);
    }
    const auto* arena = pineforge::detail::ScratchArena::attached();
    require(arena && arena->stats().backend == pineforge::detail::ScratchArenaBackend::sealed_memfd
                && arena->stats().high_water > 0,
            "jailed run did not use the sealed arena");
    pineforge::detail::ScratchArena::detach();
    require(descriptor_count() == before, "jailed run leaked a scratch descriptor");
    writer.close();
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(reader.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "cannot read the jailed report stream");
        if (count == 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    require(output.find("\n[\"T\",") != std::string::npos
                && output.find("\n{\"trailer\":\"pf-report-sink\"") != std::string::npos,
            "jailed report stream lacks its trade or completed trailer");
    watch.collect();
    require(watch.empty(), "jailed run created a scratch file under TMPDIR, /tmp or /var/tmp");
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_jailed_scratch_tmpfs_tmpdir", row);
}
#endif // __linux__
