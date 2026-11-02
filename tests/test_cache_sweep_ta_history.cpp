#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"
#include "cache_sweep_fixture.hpp"
#include <pineforge/detail/read_through_cache.hpp>
#include <pineforge/series.hpp>

#include <cstdio>
#include <cstring>

namespace ta_history_sweep {
using acceptance_process::require;
using Values = pineforge::detail::WindowValues<double>;
using Cache = pineforge::detail::ReadThroughCache;
constexpr std::size_t page_bytes = Values::kChunk * sizeof(double);
constexpr std::size_t count = 65 * Values::kChunk + 17;

std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

cache_sweep::Result run(cache_sweep::Setting setting) {
    const auto capacity = setting.defaults ? pineforge::WindowCacheCapacity{}
        : pineforge::WindowCacheCapacity{setting.units};
    Values values(count, -3.0, capacity);
    std::vector<double> expected(count, -3.0);
    for (std::size_t page = 0; page * Values::kChunk < count; ++page) {
        const auto first = page * Values::kChunk;
        const auto last = std::min(count, first + Values::kChunk) - 1;
        expected[first + 3] = static_cast<double>(page * 37 + 5) / 8;
        expected[last] = page % 2 ? -0.0 : pineforge::na<double>();
        values.write(first + 3, expected[first + 3]);
        values.write(last, expected[last]);
    }
    cache_sweep::Result result;
    // Test output, outside the measured first-read allocation interval.
    result.output.records.reserve((count + 16) * (sizeof(std::uint64_t) + sizeof(double)));
    auto record = [&](std::size_t index, double actual) {
        require(bits(actual) == bits(expected[index]), "TA sweep differs from its owning oracle");
        cache_sweep::scalar(result.output.records, static_cast<std::uint64_t>(index));
        cache_sweep::scalar(result.output.records, actual);
    };
    auto read = [&](std::size_t index) { record(index, values.read(index)); };

    const auto policy = values.read_cache_policy();
    const auto before = values.cache_stats();
    const auto heap_before = global_allocation::live_bytes;
    const auto allocations_before = global_allocation::recorded_allocations;
    const double first = values.read(3);
    const auto cache_allocation = global_allocation::live_bytes - heap_before;
    const auto allocation_calls = global_allocation::recorded_allocations - allocations_before;
    const auto cold = values.cache_stats().backing_read_attempts - before.backing_read_attempts;
    require(cache_allocation == policy.capacity_bytes,
            "TA optional allocation disagrees with the inclusive shared-cache budget");
    require(allocation_calls == (policy.capacity_bytes ? 1U : 0U),
            "TA first read allocated hidden cache storage");
    record(3, first);
    read(3);
    const auto after_hot = values.cache_stats().backing_read_attempts;
    require(after_hot - before.backing_read_attempts == (setting.defaults ? 0U
                : setting.units == 0 ? 2U : 1U),
            "TA warm read has unexpected inner backing traffic");

    // Page 64 collides with page 0 for 1, 2 and 64 direct-mapped lines.
    // This trace is identical at every setting, including the owning default.
    read(64 * Values::kChunk + 3);
    const auto before_reread = values.cache_stats().backing_read_attempts;
    read(3);
    const auto reread = values.cache_stats().backing_read_attempts - before_reread;
    require(cold == (setting.defaults ? 0U : 1U)
                && reread == (setting.defaults ? 0U : 1U),
            "TA cold and evicted rereads did not reach the inner backing");

    read(count - 1); // Exact partial tail, including the na payload bits.
    const auto before_write = values.cache_stats();
    values.write(count - 1, 151.25);
    expected.back() = 151.25;
    const auto after_write = values.cache_stats();
    require(after_write.backing_read_attempts == before_write.backing_read_attempts
                && after_write.backing_write_attempts - before_write.backing_write_attempts
                    == (setting.defaults ? 0U : 1U),
            "TA scalar write added a read/modify/write or changed write-through ordering");
    read(count - 1);
    {
        auto checkpoint = values;
        values.write(3, 313.5);
        require(bits(checkpoint.read(3)) == bits(expected[3]),
                "TA sweep changed a checkpoint's immutable backing");
        record(3, checkpoint.read(3));
        expected[3] = 313.5;
        read(3);
    }
    // Every final logical value, in order; no checksum or selected-row proxy.
    for (std::size_t index = 0; index < count; ++index) read(index);
    constexpr auto records = count + 8;
    require(result.output.records.size() == records * (sizeof(std::uint64_t) + sizeof(double)),
            "TA acceptance transcript omitted a logical record");

    const auto shared = values.read_cache_stats();
    const auto inner = values.cache_stats();
    auto& telemetry = result.cache;
    const auto slots = policy.capacity_bytes / Cache::line_bytes(page_bytes);
    telemetry.configured_payload = slots * page_bytes;
    telemetry.configured_metadata = slots * (Cache::line_bytes(page_bytes) - page_bytes);
    telemetry.payload = shared.current_payload_bytes;
    telemetry.metadata = shared.current_metadata_bytes;
    telemetry.peak_resident = shared.peak_bytes;
    telemetry.workspace = telemetry.fixed_workspace = inner.workspace_bytes;
    telemetry.fixed_control = sizeof(Values);
    telemetry.hits = shared.hits;
    telemetry.misses = shared.misses;
    telemetry.evictions = shared.evictions;
    // These counters live at ScratchFile calls, below ReadThroughCache; they
    // include clone reads and do not depend on cache-reported misses.
    telemetry.inner_reads = inner.backing_read_attempts;
    telemetry.inner_bytes = inner.backing_read_bytes;
    telemetry.cold_reads = cold;
    telemetry.evicted_reread_reads = reread;
    require(telemetry.payload <= telemetry.configured_payload
                && telemetry.metadata <= telemetry.configured_metadata
                && telemetry.peak_resident <= policy.capacity_bytes,
            "TA shared cache exceeded its inclusive policy");
    if (setting.defaults) {
        require(!inner.scratch_backed && inner.backing_read_attempts == 0
                    && shared.get_calls == 0 && inner.resident_payload_bytes == count * sizeof(double),
                "TA default must retain its authoritative owning representation");
    } else {
        require(inner.scratch_backed && shared.get_calls > 0
                    && cache_sweep::cold_read_proven(telemetry),
                "TA finite-policy sweep never exercised shared-cache backing reads");
        if (setting.units == 0) {
            require(shared.current_bytes == 0 && shared.hits == 0,
                    "TA bypass secretly retained optional cache bytes");
        } else {
            require(shared.hits > 0 && shared.evictions > 0 && reread == 1,
                    "TA cached sweep did not hit, evict and physically reread");
        }
    }
    std::printf("CACHE_SWEEP kind=ta_history setting=%s pages=%zu "
                "configured_payload=%llu configured_metadata=%llu payload=%llu metadata=%llu "
                "peak_resident=%llu fixed_control=%llu workspace=%llu "
                "hits=%llu misses=%llu evictions=%llu inner_reads=%llu inner_bytes=%llu "
                "cold_reads=%llu evicted_reread_reads=%llu first_read_heap=%zu "
                "owner_payload=%zu owner_metadata=%zu heap_live=%zu heap_peak=%zu\n",
        setting.name, capacity.pages,
        static_cast<unsigned long long>(telemetry.configured_payload),
        static_cast<unsigned long long>(telemetry.configured_metadata),
        static_cast<unsigned long long>(telemetry.payload),
        static_cast<unsigned long long>(telemetry.metadata),
        static_cast<unsigned long long>(telemetry.peak_resident),
        static_cast<unsigned long long>(telemetry.fixed_control),
        static_cast<unsigned long long>(telemetry.workspace),
        static_cast<unsigned long long>(telemetry.hits),
        static_cast<unsigned long long>(telemetry.misses),
        static_cast<unsigned long long>(telemetry.evictions),
        static_cast<unsigned long long>(telemetry.inner_reads),
        static_cast<unsigned long long>(telemetry.inner_bytes),
        static_cast<unsigned long long>(cold), static_cast<unsigned long long>(reread),
        cache_allocation, inner.resident_payload_bytes, inner.resident_metadata_bytes,
        global_allocation::live_bytes, global_allocation::peak_bytes);
    return result;
}
} // namespace ta_history_sweep

int main() {
    try {
        global_allocation::recording = true;
        {
            cache_sweep::Output baseline;
            for (const auto setting : std::array<cache_sweep::Setting, 5>{{
                     {0, false, "bypass"}, {1, false, "one_line"}, {2, false, "two_lines"},
                     {0, true, "default_owning"}, {64, false, "large"}}}) {
                auto result = ta_history_sweep::run(setting);
                cache_sweep::replay(result.output);
                if (setting.units == 0 && !setting.defaults) baseline = std::move(result.output);
                else cache_sweep::compare(baseline, result.output);
            }
        }
        global_allocation::recording = false;
        acceptance_process::require(global_allocation::recorded_allocations > 0
                    && global_allocation::recorded_releases > 0,
                "TA acceptance allocation census was not active");
        std::puts("TA cache sweep: complete records, hashes, trades, fills and sink bytes agree");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "TA cache sweep FAIL: %s\n", error.what());
        return 1;
    }
}
