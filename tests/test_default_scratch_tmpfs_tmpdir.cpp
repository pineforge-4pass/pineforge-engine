// RED FIRST: at 61ab57dac1 the real closed-trade ledger opens its bounded
// scratch on TMPDIR's tmpfs and the run fails report_sink_failed/scratch_io.
// The native run supplies no sink or scratch-directory argument.
#include "default_scratch_tmpfs_fixture.hpp"

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    require_filesystems();
    CreationWatch watch;
    const auto before = descriptor_count();
    {
        Host host;
        run(host);
        require_success(host);
        Report report(host);
        require(report.value.total_trades == 1 && report.value.trades_len == 1
                    && report.value.trades != nullptr
                    && report.value.trades[0].open_at_end == 0
                    && report.value.metrics.all.num_trades == 1,
                "plain native run lost its completed trade or owning report");
        require_success(host);
        watch.collect();
        watch.require_removed();
    }
    exercise_command_scratch(pineforge::detail::ScratchBackingKind::disk);
    watch.collect();
    require(watch.created_in("/var/tmp"), "default run never used disk-backed /var/tmp");
    require(!watch.created_in("/dev/shm") && !watch.created_in("/tmp"),
            "TMPDIR on tmpfs must resolve directly to /var/tmp");
    watch.require_removed();
    require(descriptor_count() == before, "default run leaked an unlinked scratch descriptor");
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_default_scratch_tmpfs_tmpdir", row);
}
