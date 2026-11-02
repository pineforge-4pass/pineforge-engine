#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/detail/read_through_cache.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include "../src/command_scratch_cache.hpp"
#include "../src/source/pine_sequence_cache_observer.hpp"
#include "cache_sweep_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace pd = pineforge::detail;
using pineforge::source::PlacementSnapshot;
using pineforge::source::PlacementTable;
constexpr auto absent = std::numeric_limits<std::uint64_t>::max();
constexpr auto past_last = std::size_t{2053};
constexpr auto sequence_page_bytes = pd::scratch_min_page_size;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct InnerReads {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    pd::ScratchIoObserver previous;
    InnerReads() : previous(pd::set_command_scratch_physical_io_observer({this, charge})) {}
    ~InnerReads() { pd::set_command_scratch_physical_io_observer(previous); }
    static void charge(void* context, const pd::ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<InnerReads*>(context);
        if (io.operation == pd::ScratchIoOperation::read_page && io.status == pd::ScratchStatus::ok) {
            ++self.calls;
            self.bytes += io.bytes;
        }
        if (self.previous.charge) self.previous.charge(self.previous.context, io);
    }
};

struct CacheWatch {
    const pd::CachedScratchBacking* backing = nullptr;
    pineforge::source::detail::SequenceCacheObserver previous;
    CacheWatch()
        : previous(pineforge::source::detail::set_sequence_cache_observer({this, created})) {}
    ~CacheWatch() { pineforge::source::detail::set_sequence_cache_observer(previous); }
    static void created(void* context, const pd::ScratchBacking& backing) noexcept {
        auto& self = *static_cast<CacheWatch*>(context);
        self.backing = dynamic_cast<const pd::CachedScratchBacking*>(&backing);
        if (self.previous.created) self.previous.created(self.previous.context, backing);
    }
};

std::string id(std::size_t i) {
    // Equal long prefixes exercise complete multi-chunk comparison. The
    // aggregate retained keys alone exceed every tested cache capacity.
    return std::string(137, 'q') + "-" + std::to_string(i);
}

struct Reference {
    std::map<std::uint64_t, PlacementSnapshot> retained;
    std::vector<PlacementSnapshot> erased;
    std::uint64_t query(const std::string& source, const std::string& from) const {
        auto any = absent;
        auto pair = absent;
        const auto fold = [&](const PlacementSnapshot& row) {
            if (row.source_id != source) return;
            any = std::min(any, row.command_sequence);
            if (row.from_entry == from) pair = std::min(pair, row.command_sequence);
        };
        for (const auto& row : erased) fold(row);
        for (const auto& row : retained) fold(row.second);
        return pair == absent ? any : pair;
    }
};

void probe_backing_cache(const pd::CachedScratchBacking& backing, InnerReads& inner) {
    const auto page_bytes = backing.page_size();
    require(page_bytes == sequence_page_bytes, "sequence backing preserves the store page geometry");
    const auto slots = backing.cache_slot_count();
    const auto extent = backing.page_count();
    require(extent > slots, "sequence backing probe exceeds every resident cache line");
    std::vector<std::byte> early(page_bytes), observed(page_bytes);
    const auto read = [&](pd::ScratchPageId page, std::vector<std::byte>& destination) {
        require(backing.read_page(page, {destination.data(), destination.size()})
                    == pd::ScratchStatus::ok,
                "sequence backing probe reads a complete retained page");
    };

    // The store's two-page LRU can satisfy repeated sequence reads before
    // they reach this cache. Read its actual backing without changing any
    // stored byte, so a one-line backing hit is independently observable.
    read(0, early);
    const auto before_warm = inner.calls;
    const auto before_stats = backing.read_cache_stats();
    read(0, observed);
    require(observed == early, "warm sequence backing read preserves every page byte");
    const auto warm_stats = backing.read_cache_stats();
    if (slots != 0) {
        require(inner.calls == before_warm && warm_stats.hits > before_stats.hits,
                "warm sequence backing read hits without inner I/O");
    } else {
        require(inner.calls == before_warm + 1 && warm_stats.hits == before_stats.hits,
                "bypass sequence backing read reaches the inner on every request");
    }

    // The full extent and order are identical at every capacity. Every
    // direct-mapped line is replaced before returning to the early page.
    for (pd::ScratchPageId page = 1; page < extent; ++page) read(page, observed);
    const auto before_early = inner.calls;
    read(0, observed);
    require(observed == early && inner.calls == before_early + 1,
            "evicted sequence backing page is physically reread byte for byte");
    if (slots != 0)
        require(backing.read_cache_stats().evictions > warm_stats.evictions,
                "sequence backing probe evicts a real resident line");
}

std::vector<std::uint64_t> run(std::size_t capacity, cache_sweep::Telemetry& telemetry) {
    InnerReads inner; // must outlive the lazy backing
    CacheWatch watch; // borrowed cache owner is inspected before clear below
    PlacementTable table(0, capacity);
    Reference reference;
    std::vector<std::uint64_t> output;
    const auto put = [&](std::uint64_t incarnation, std::size_t identity,
                         std::string from, std::uint64_t sequence) {
        PlacementSnapshot row;
        row.source_id = id(identity);
        row.from_entry = std::move(from);
        row.command_sequence = sequence;
        table.try_emplace(incarnation, row);
        reference.retained.emplace(incarnation, std::move(row));
    };
    // One insertion does not open the derived backing. The next mutation or
    // first query can do so under the base tree's existing lazy contract.
    put(1, 0, "exact", 77);
    require(inner.calls == 0 && table.sequence_work()[2] == 0 && watch.backing == nullptr,
            "first retained insertion leaves the derived backing unopened");
    put(2, 0, "other", 3);
    put(3, 0, "max", absent);
    // 2049 distinct long ids contribute at least 2049*137 bytes before
    // framing or duplicate index domains, above the 256-KiB large cache.
    for (std::size_t i = 4; i != past_last; ++i) put(i, i, "entry", i * 13 + 5);
    const auto query = [&](std::size_t identity, const std::string& from) {
        const auto expected = reference.query(id(identity), from);
        const auto actual = table.command_sequence_for(id(identity), from);
        require(actual == expected, "sequence query matches independent retained-plus-erased scan");
        output.push_back(actual);
    };
    const auto before_cold = inner.calls;
    query(0, "exact");
    query(0, "exact");
    query(0, "max"); // explicit MAX exact pair falls back to any=3
    query(9999, "missing");
    const auto before = inner.calls;
    // Visit far more complete keys than fit in the largest cache, then
    // return to early keys. This is not an all-resident comparison.
    for (std::size_t i = 4; i != past_last; ++i) query(i, "entry");
    telemetry.cold_reads = inner.calls - before_cold;
    const auto before_early = inner.calls;
    query(0, "exact");
    require(inner.calls > before_early && before_early > before && inner.bytes != 0,
            "evicted early retained identity reread reaches the inner backing");
    telemetry.evicted_reread_reads = inner.calls - before_early;

    PlacementSnapshot replacement;
    replacement.source_id = id(0);
    replacement.from_entry = "exact";
    replacement.command_sequence = 1;
    reference.retained[1] = replacement;
    table.replace(1, std::move(replacement));
    query(0, "exact");
    for (const auto incarnation : {1ULL, 2ULL, 44ULL, 711ULL}) {
        reference.erased.push_back(reference.retained.at(incarnation));
        reference.retained.erase(incarnation);
        require(table.erase(incarnation), "retirement transfers a real row to erased minima");
    }
    table.compact();
    for (std::size_t i = 4; i != past_last; ++i) query(i, "entry");
    query(0, "exact");
    query(0, "max");
    output.push_back(table.erased_digest());
    output.push_back(table.erased_rows());
    // Compare all retained row identities in their public iteration order.
    for (const auto& row : table) {
        output.push_back(row.first);
        output.push_back(row.second.command_sequence);
    }
    require(watch.backing != nullptr, "sequence factory exposes its real cache for diagnostics");
    const auto query_stats = watch.backing->read_cache_stats();
    const auto query_reads = inner.calls;
    probe_backing_cache(*watch.backing, inner);
    const auto probe_reads = inner.calls - query_reads;
    const auto stats = watch.backing->read_cache_stats();
    const auto page_bytes = watch.backing->page_size();
    const auto line_bytes = pd::ReadThroughCache::line_bytes(page_bytes);
    const auto slots = watch.backing->cache_slot_count();
    telemetry.configured_payload = capacity;
    // The factory's third argument is payload bytes. Complete shared tags
    // are additional; resident_bytes alone would leave the default cache on.
    telemetry.configured_metadata = (capacity / page_bytes) * (line_bytes - page_bytes);
    telemetry.payload = watch.backing->cache_payload_bytes();
    telemetry.metadata = watch.backing->cache_metadata_bytes();
    telemetry.peak_resident = watch.backing->cache_storage_bytes();
    require(slots == capacity / page_bytes
                && telemetry.payload == capacity
                && telemetry.metadata == telemetry.configured_metadata
                && telemetry.peak_resident == capacity + telemetry.configured_metadata
                && stats.allocation_bypass_count == 0,
            "sequence payload allowance admits every complete line and its metadata");
    require(stats.current_payload_bytes == telemetry.payload
                && stats.current_metadata_bytes == telemetry.metadata
                && stats.current_bytes == telemetry.peak_resident,
            "spilled sequence cache reports every allocated payload and metadata byte");
    telemetry.fixed_control = sizeof(pd::CachedScratchBacking);
    telemetry.workspace = telemetry.fixed_workspace = table.sequence_work()[4];
    telemetry.hits = stats.hits;
    telemetry.misses = stats.misses;
    telemetry.evictions = stats.evictions;
    // The diagnostic pointer is borrowed only until clear. Capture before
    // destroying this index; the next construction replaces it below.
    watch.backing = nullptr;
    table.clear();
    reference.retained.clear();
    reference.erased.clear();
    put(1, 0, "reset", 111);
    query(0, "reset");
    require(watch.backing != nullptr
                && watch.backing->cache_payload_bytes() <= telemetry.configured_payload
                && watch.backing->cache_metadata_bytes() <= telemetry.configured_metadata
                && watch.backing->cache_storage_bytes()
                    <= telemetry.configured_payload + telemetry.configured_metadata,
            "rebuilt sequence backing preserves the configured payload and metadata budget");
    telemetry.inner_reads = inner.calls;
    telemetry.inner_bytes = inner.bytes;
    std::printf("retained_sequence payload=%zu page_bytes=%zu line_bytes=%zu slots=%zu "
                "allocated_payload=%llu allocated_metadata=%llu hits=%llu misses=%llu "
                "evictions=%llu query_hits=%llu query_evictions=%llu query_backing_reads=%llu "
                "probe_hits=%llu probe_evictions=%llu probe_backing_reads=%llu "
                "backing_reads=%llu backing_bytes=%llu answers=%zu\n",
                capacity, page_bytes, line_bytes, slots,
                static_cast<unsigned long long>(telemetry.payload),
                static_cast<unsigned long long>(telemetry.metadata),
                static_cast<unsigned long long>(telemetry.hits),
                static_cast<unsigned long long>(telemetry.misses),
                static_cast<unsigned long long>(telemetry.evictions),
                static_cast<unsigned long long>(query_stats.hits),
                static_cast<unsigned long long>(query_stats.evictions),
                static_cast<unsigned long long>(query_reads),
                static_cast<unsigned long long>(stats.hits - query_stats.hits),
                static_cast<unsigned long long>(stats.evictions - query_stats.evictions),
                static_cast<unsigned long long>(probe_reads),
                static_cast<unsigned long long>(inner.calls),
                static_cast<unsigned long long>(inner.bytes), output.size());
    return output;
}
} // namespace

int main() {
    try {
        cache_sweep::sweep("retained_sequence", sequence_page_bytes,
            pd::CachedScratchBacking::kCacheBytes, 4 * pd::CachedScratchBacking::kCacheBytes,
            [](cache_sweep::Setting setting) {
                cache_sweep::Result result;
                const auto output = run(setting.defaults
                    ? pd::CachedScratchBacking::kCacheBytes : setting.units, result.cache);
                for (const auto word : output) cache_sweep::scalar(result.output.records, word);
                return result;
            });
        std::puts("retained-sequence capacity sweep passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "retained-sequence capacity sweep: %s\n", error.what());
        return 1;
    }
}
