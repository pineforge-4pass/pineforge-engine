// Hand-derived policy/layout tests. Frozen base: scratch_defaults.hpp is
// missing (expected first compiler diagnostic, to be recorded on the lab).
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/native_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace pineforge;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void policy_fields(const detail::ScratchManifest& m, std::uint64_t roots) {
    require(m.K_q == 0 && m.B_q == 0, "removed q reserves no data or index");
    require(m.K_e == 262144 && m.B_e == 25165824, "P0 erased key/byte policy");
    require(m.K_c == 32768 && m.B_c == 4227072, "P0 consumed key/byte policy");
    require(m.U == 65536 && m.B_U == 4194304, "P0 cohort row/byte policy");
    require(detail::kMaxResidentCohorts == 4096, "active cohort policy remains the old U");
    require(m.V == 27 && m.B_V == 6912, "P0 rank row/byte policy");
    require(m.R == roots, "replacement policy uses this run's bar count");
}

void layout_pages(const detail::ScratchManifest& m, std::uint64_t root_pages,
                  std::uint64_t total_pages, std::uint64_t slab_bytes) {
    // Independent arithmetic, not harvested output:
    // De=40*262144+25165824=35651584 -> 8704 pages; Ie=16*524288 -> 2048.
    // Dc=40*32768+4227072=5537792 -> 1352; Ic=16*65536 -> 256.
    // Coh=1+96*65536+4194304=10485761 -> 2561 (2560 pages + 1 byte).
    // Rank=1+60*27+6912=8533 -> 3. q=0. Fixed sum=14924 pages.
    const std::array<std::uint64_t, 9> pages{{0, 0, 8704, 2048, 1352, 256, 2561, 3, root_pages}};
    const auto layout = detail::compute_layout(m); // Real layout, no multi-GB allocation.
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        require(layout.regions[i].offset == offset, "page-rounded prefix offset");
        require(layout.regions[i].span == pages[i] * 4096, "hand-derived region page count");
        offset += pages[i] * 4096;
    }
    require(layout.slab_size == total_pages * 4096, "hand-derived total pages");
    require(layout.slab_size == slab_bytes, "independent decimal byte total");
    require(layout.regions[0].capacity == 0 && layout.regions[1].capacity == 0,
            "q fields retain zero exact capacity");
    require(layout.regions[6].capacity == 10485761, "cohort page padding is not capacity");
    require(layout.regions[7].capacity == 8533, "rank page padding is not capacity");
    require(layout.regions[8].capacity == 16 * m.R, "exact root capacity");
}

void finite_batch_layouts() {
    struct Case { std::uint64_t bars, roots, root_pages, pages, bytes; };
    // Roots=32*N bytes. Ceiling pages:
    // 32000 -> 8; 3200000=781*4096+1024 -> 782;
    // 32000000=7812*4096+2048 -> 7813; 3200000000=781250*4096.
    const std::array<Case, 4> cases{{
        {1000, 2000, 8, 14932, 61161472},
        {100000, 200000, 782, 15706, 64331776},
        {1000000, 2000000, 7813, 22737, 93130752},
        {100000000, 200000000, 781250, 796174, 3261128704ULL},
    }};
    for (const auto& c : cases) {
        const auto m = detail::default_scratch_manifest(c.bars);
        policy_fields(m, c.roots);
        layout_pages(m, c.root_pages, c.pages, c.bytes);
    }
}

void overflow_and_stream_tier() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    // Test the multiply boundary separately from layout's stricter byte limits.
    policy_fields(detail::default_scratch_manifest(maximum / 2), maximum - 1);
    for (const auto bars : {maximum / 2 + 1, maximum}) {
        bool caught = false;
        try {
            (void)detail::default_scratch_manifest(bars);
        } catch (const detail::ScratchCapacityError& error) {
            caught = true;
            const auto value = classify_run_failure(error);
            require(value.code == RunFailureCode::scratch_capacity_exceeded, "typed R overflow");
            require(run_failure_code_class(value.code) == RunFailureClass::resource,
                    "overflow is a resource refusal");
            require(!run_failure_code_retryable(value.code), "overflow is not retryable");
            require(value.args && *value.args == std::string("{\"resource\":\"layout\"}"),
                    "overflow carries only the closed layout resource");
        }
        require(caught, "2*bars overflow must refuse before wrapping");
    }
    const auto stream = detail::default_scratch_manifest();
    policy_fields(stream, 65536);
    // 16*65536=1048576=256 pages; 14924+256=15180, 62177280 bytes.
    layout_pages(stream, 256, 15180, 62177280);
    const auto empty_batch = detail::default_scratch_manifest(0);
    policy_fields(empty_batch, 0);
    layout_pages(empty_batch, 0, 14924, 61128704);
}

void default_limit_messages() {
    const auto m = detail::default_scratch_manifest(0);
    require(detail::scratch_count_limit_message(detail::ScratchResource::cohort, m.U) ==
        "more than 65,536 distinct order ids in one run; reuse ids, e.g. build the id from a modulo of bar_index",
        "default cohort count text");
    require(detail::scratch_byte_limit_message(detail::ScratchResource::cohort, m.B_U) ==
        "order ids use more than 4,194,304 bytes in one run; use shorter ids or reuse ids",
        "default cohort byte text");
    require(detail::scratch_count_limit_message(detail::ScratchResource::erased_index, m.K_e) ==
        "more than 262,144 retired order-id keys in one run (each retired order uses 2); reuse ids, e.g. a modulo of bar_index",
        "default retired key text");
    require(detail::scratch_byte_limit_message(detail::ScratchResource::erased_data, m.B_e) ==
        "retired order ids use more than 25,165,824 bytes in one run; use shorter ids or reuse ids",
        "default retired byte text");
    require(detail::scratch_count_limit_message(detail::ScratchResource::consumed_index, m.K_c) ==
        "more than 32,768 keys for partially filled exits in one run; reuse ids, e.g. a modulo of bar_index",
        "default partial exit key text");
    require(detail::scratch_byte_limit_message(detail::ScratchResource::consumed_data, m.B_c) ==
        "partially filled exits use more than 4,227,072 bytes in one run; use shorter ids or reuse ids",
        "default partial exit byte text");
}

// Exercise the actual prepare boundary. Stop there deliberately, before the
// other lanes' binders: this is a default-selection test, not a RAM-run proof.
class DefaultProbe final : public NativeStrategyHost {
public:
    int prepare_calls = 0;
    std::shared_ptr<detail::ScratchSlab> observed;
    void prepare_native_begin(const NativeBeginArgs&) override {
        ++prepare_calls;
        observed = scratch_tables_for_run();
        throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                        "scratch defaults provider-boundary witness");
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void engine_all_zero_selects_per_run_default() {
    const std::array<Bar, 3> bars{{
        {100, 100, 100, 100, 1, 0}, {100, 100, 100, 100, 1, 60000},
        {100, 100, 100, 100, 1, 120000},
    }};
    pf_scratch_tables_v1 config{};
    config.struct_size = static_cast<std::uint32_t>(sizeof(config));
    config.version = 1;
    config.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
    DefaultProbe batch;
    require(batch.set_scratch_tables_v1(&config) == 0, "select all-zero bounded policy");
    batch.run(bars.data(), 3);
    require(batch.prepare_calls == 1 && batch.observed, "default slab before provider");
    policy_fields(batch.observed->manifest(), 6);
    const auto preceding_owner = batch.observed;
    batch.run(bars.data(), 1);
    require(batch.prepare_calls == 2 && batch.observed != preceding_owner,
            "fresh per-run owner on repeated begin");
    policy_fields(batch.observed->manifest(), 2);
    const auto& installed = batch.scratch_table_config().manifest;
    require(installed.K_q == 0 && installed.B_q == 0 && installed.K_e == 0
            && installed.B_e == 0 && installed.K_c == 0 && installed.B_c == 0
            && installed.U == 0 && installed.B_U == 0 && installed.V == 0
            && installed.B_V == 0 && installed.R == 0,
            "begin snapshots the default without replacing installed all-zero policy");

    DefaultProbe stream;
    require(stream.set_scratch_tables_v1(&config) == 0, "select default for stream");
    require(!stream.stream_begin(bars.data(), 3, "1", "1"), "stop at stream provider");
    require(stream.prepare_calls == 1 && stream.observed, "stream slab before provider");
    policy_fields(stream.observed->manifest(), 65536); // warmup_n is not a lifetime count.

    // Any nonzero field means an explicit manifest: do not fill its zeros.
    DefaultProbe explicit_caps;
    config.R = 1;
    require(explicit_caps.set_scratch_tables_v1(&config) == 0, "select explicit tiny policy");
    explicit_caps.run(bars.data(), 3);
    require(explicit_caps.prepare_calls == 1 && explicit_caps.observed, "explicit slab observed");
    const auto& m = explicit_caps.observed->manifest();
    require(m.K_q == 0 && m.B_q == 0 && m.K_e == 0 && m.B_e == 0
            && m.K_c == 0 && m.B_c == 0 && m.U == 0 && m.B_U == 0
            && m.V == 0 && m.B_V == 0 && m.R == 1,
            "partly zero explicit manifest remains explicit");

    DefaultProbe disk;
    disk.run(bars.data(), 3);
    require(disk.prepare_calls == 1 && !disk.observed, "default disk begin owns no slab");
}
} // namespace

int main() {
    try {
        finite_batch_layouts();
        overflow_and_stream_tier();
        default_limit_messages();
        engine_all_zero_selects_per_run_default();
        std::puts("test_scratch_defaults: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_defaults: %s\n", error.what());
        return 1;
    }
}
