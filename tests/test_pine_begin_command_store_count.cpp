// A-10: count successful production backing creations, including a backing
// destroyed before the run starts. Descriptor snapshots miss that extra file.
// The sole renamed definition still performs the original factory's work.
#include "../src/command_scratch_production.hpp"
#define make_command_scratch_backing uncounted_command_scratch_backing
#include "../src/command_scratch_production.cpp"
#undef make_command_scratch_backing

#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdio>

namespace {
std::size_t backing_creations = 0;
}

namespace pineforge::detail {
std::unique_ptr<ScratchBacking> make_command_scratch_backing(std::size_t page_size) {
    auto backing = uncounted_command_scratch_backing(page_size);
    ++backing_creations;
    return backing;
}
} // namespace pineforge::detail

namespace {
using namespace pineforge;
namespace sd = pineforge::source::detail;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
} } while (false)

struct Host final : source::PineStrategyHost {
    int callbacks = 0;
    void on_source_bar(const Bar&) override { ++callbacks; }
};

const std::array<Bar, 2> bars{{
    {100.0, 101.0, 99.0, 100.0, 1.0, 1704067200000LL},
    {100.0, 101.0, 99.0, 100.0, 1.0, 1704067260000LL},
}};

void one_exit_service(std::size_t before) {
    // The admitted run owns exactly three distinct services: Exit, Trade,
    // Dependency. Trade and Dependency are mandatory consumer provisions,
    // so only one of the creations can be the Exit service.
    constexpr std::size_t mandatory_consumer_services = 2;
    CHECK(backing_creations >= before + mandatory_consumer_services);
    CHECK(backing_creations - before - mandatory_consumer_services == 1);
    CHECK(backing_creations - before == 3);
}

void batch_and_reuse() {
    Host host;
    for (int run = 0; run < 2; ++run) {
        const auto before = backing_creations;
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_run_status() == 0);
        CHECK(host.last_error().empty());
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.callbacks == (run + 1) * static_cast<int>(bars.size()));
        one_exit_service(before);
    }
}

void stream_begin() {
    Host host;
    const auto before = backing_creations;
    CHECK(host.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1"));
    CHECK(host.last_run_status() == 0);
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Running);
    one_exit_service(before);
    CHECK(host.stream_end());
    CHECK(host.last_run_status() == 0);
}

void standalone_projection(bool bound_to_source_host) {
    Host host;
    source::PineExecutionAdapter adapter;
    if (bound_to_source_host) adapter.bind(host);
    source::StagedConfiguration staged;
    staged.syminfo.mintick = 0.25;
    staged.syminfo.pointvalue = 1.0;
    const auto before = backing_creations;
    const auto spec = adapter.project(source::PineStrategyConfig{}, staged, NativeBeginArgs{});
    CHECK(backing_creations - before == 1);
    const auto& scratch = sd::exit_scratch(adapter);
    CHECK(scratch.run == spec.identity);
    CHECK(scratch.store.status() == detail::ScratchStatus::ok);
    CHECK(scratch.store.record_count() == 0);
}
} // namespace

int main() {
    batch_and_reuse();
    stream_begin();
    standalone_projection(false);
    // A second adapter bound to a Pine host is still a standalone projector;
    // only that host's own adapter is guaranteed a replacement service.
    standalone_projection(true);
    std::printf("%s test_pine_begin_command_store_count (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
