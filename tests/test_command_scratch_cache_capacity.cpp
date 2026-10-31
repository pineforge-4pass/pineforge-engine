// Compile this complete unit against the frozen previous header closure first.
// Its first expected diagnostic is the absent CommandScratchPhysicalIoStats.
#include "../src/command_scratch_cache.hpp"
#include "../src/command_scratch_production.hpp"
#include "command_scratch_cache_run_fixture.hpp"

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unistd.h>

namespace command_capacity_test {
using namespace pineforge::detail;
using command_cache_test::require;
using command_cache_test::bytes;
using command_cache_test::buffer;
using command_cache_test::pattern;
constexpr auto page = command_cache_test::page_bytes;

struct Counts {
    std::uint64_t reads = 0, writes = 0, syncs = 0, failures = 0;
    std::uint64_t read_bytes = 0, write_bytes = 0, completed_reads = 0;
    static void charge(void* context, const ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<Counts*>(context);
        if (io.operation == ScratchIoOperation::read_page) {
            ++self.reads;
            self.read_bytes += io.bytes;
            self.completed_reads += io.status == ScratchStatus::ok;
        } else if (io.operation == ScratchIoOperation::write_page) {
            ++self.writes;
            self.write_bytes += io.bytes;
        } else ++self.syncs;
        self.failures += io.status != ScratchStatus::ok;
    }
    ScratchIoObserver observer() noexcept { return {this, charge}; }
};

struct Observers {
    ScratchIoObserver logical, physical;
    Observers(Counts& charged, CommandScratchPhysicalIoStats& inner) noexcept
        : logical(set_command_scratch_io_observer(charged.observer())),
          physical(set_command_scratch_physical_io_observer(inner.observer())) {}
    ~Observers() {
        set_command_scratch_physical_io_observer(physical);
        set_command_scratch_io_observer(logical);
    }
};

struct Environment {
    const char* name;
    std::string previous;
    bool present = false;
    Environment(const char* key, const char* value) : name(key) {
        if (const auto* old = std::getenv(key)) {
            present = true;
            previous = old;
        }
        require((value ? ::setenv(key, value, 1) : ::unsetenv(key)) == 0,
                "select isolated command cache test environment");
    }
    ~Environment() {
        if (present) ::setenv(name, previous.c_str(), 1);
        else ::unsetenv(name);
    }
};

struct Directory {
    std::string path = "./command-cache-XXXXXX";
    Directory() {
        require(::mkdtemp(path.data()) != nullptr, "create command cache disk directory");
    }
    ~Directory() { ::rmdir(path.c_str()); }
};

struct Budget {
    CommandScratchByteCap previous = set_command_scratch_byte_cap(std::uint64_t{0});
    ~Budget() { set_command_scratch_byte_cap(std::move(previous)); }
};

void tiny_vs_default_outputs() {
    auto reference = make_command_scratch_backing(page);
    const auto expected = command_cache_test::run(*reference);
    for (const std::size_t capacity :
            {std::size_t{0}, page, 2 * page, CachedScratchBacking::kCacheBytes,
             2 * CachedScratchBacking::kCacheBytes}) {
        for (const bool resident : {false, true}) {
            Counts logical;
            CommandScratchPhysicalIoStats physical;
            Observers observers(logical, physical);
            auto backing = make_command_scratch_backing(page, resident ? capacity : 0, capacity);
            const auto actual = command_cache_test::run(*backing);
            require(actual == expected,
                    "0/1/2/default/larger caches produce identical complete output bytes");
            require(physical.completed_reads > 0 && physical.writes > 0
                    && physical.failures == 0 && logical.reads >= physical.reads,
                    "complete-output sweep actually reads and writes the inner backing");
            const auto stats = static_cast<const CachedScratchBacking&>(*backing).read_cache_stats();
            require(stats.get_calls > 0 && stats.put_calls > 0,
                    "complete-output sweep exercises the shared cache at every capacity");
        }
    }
}

void cold_reads_and_hit_charges() {
    const auto first = pattern(page, 31), conflict = pattern(page, 32);
    std::vector<std::byte> out(page);
    for (const std::size_t capacity :
            {std::size_t{0}, std::size_t{1}, page - 1, page, 2 * page,
             CachedScratchBacking::kCacheBytes, 2 * CachedScratchBacking::kCacheBytes,
             static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()),
             std::numeric_limits<std::size_t>::max()}) {
        Counts logical;
        CommandScratchPhysicalIoStats physical;
        Observers observers(logical, physical);
        auto backing = make_command_scratch_backing(page, capacity, capacity);
        // An unrepresentable optional allocation is also direct mode.
        const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
        const auto metadata = ReadThroughCache::line_bytes(page) - page;
        const auto representable = capacity <= maximum
            && capacity / page <= (maximum - capacity) / metadata;
        const auto slots = representable ? capacity / page : 0;
        const ScratchPageId collision = slots ? slots : 128;
        require(backing->write_page(0, bytes(first)) == ScratchStatus::ok
                && backing->write_page(collision, bytes(conflict)) == ScratchStatus::ok,
                "cold-read fixture writes two conflicting pages and spills any prefix");
        physical = {};
        logical = {};
        require(backing->read_page(0, buffer(out)) == ScratchStatus::ok && out == first
                && physical.completed_reads == 1 && physical.reads == 1
                && physical.read_bytes == page && logical.reads == 1,
                "cold page is proved by one successful read from the inner backing");
        require(backing->read_page(0, buffer(out)) == ScratchStatus::ok && out == first
                && logical.reads == 2 && physical.reads == (slots ? 1 : 2),
                "a cache hit remains one logical charge with no extra inner read");
        require(backing->read_page(collision, buffer(out)) == ScratchStatus::ok && out == conflict
                && physical.reads == (slots ? 2 : 3) && physical.failures == 0,
                "colliding valid page reads through rather than appearing absent");
        const auto prior_physical = physical.reads;
        require(backing->read_page(backing->page_count(), buffer(out)) == ScratchStatus::not_found
                && physical.reads == prior_physical + 1 && logical.failures == 1,
                "only the inner extent decides absent-page status");
    }
}

void default_payload_and_slots_are_unchanged() {
    for (const std::size_t geometry : {std::size_t{256}, page, std::size_t{1536},
                                     std::size_t{4096}, std::size_t{65536},
                                     std::size_t{65537}}) {
        std::vector<std::byte> storage(4 * geometry);
        auto inner = std::make_unique<FixedMemoryScratchBacking>(
            ScratchBuffer{storage.data(), storage.size()}, geometry);
        const auto expected_slots = geometry >= 512 && geometry <= 65536
            ? 65536 / geometry : 0;
        // Preserve the old payload and line count, and charge the shared
        // cache's complete per-line metadata separately. Observe the real
        // allocation as well as the adapter's scalar diagnostics.
        const auto metadata = expected_slots
            * (ReadThroughCache::line_bytes(geometry) - geometry);
        const auto allocation = expected_slots ? 65536 + metadata : 0;
        const auto before = global_allocation::allocations;
        require(global_allocation::live_bytes == 0, "allocation census starts empty");
        global_allocation::recording = true;
        {
            CachedScratchBacking backing(std::move(inner));
            require(backing.cache_slot_count() == expected_slots
                    && backing.cache_payload_bytes() == (expected_slots ? 65536 : 0)
                    && backing.cache_metadata_bytes() == metadata
                    && backing.cache_storage_bytes() == allocation
                    && global_allocation::allocations == before + (expected_slots ? 1 : 0)
                    && global_allocation::live_bytes == allocation,
                    "default keeps prior payload/slots with one fully charged shared-cache slab");
        }
        global_allocation::recording = false;
        require(global_allocation::live_bytes == 0, "cache destruction releases its actual allocation");
    }
}

void optional_storage_failure_bypass() {
    // Payload and all tags now share one optional allocation, including at
    // capacities above 128 slots. Refuse that real slab at both sizes.
    for (const std::size_t capacity :
            {CachedScratchBacking::kCacheBytes, 2 * CachedScratchBacking::kCacheBytes}) {
        std::array<std::byte, 4 * page> storage{};
        CommandScratchPhysicalIoStats inner_counts;
        auto inner = std::make_unique<FixedMemoryScratchBacking>(
            ScratchBuffer{storage.data(), storage.size()}, page, inner_counts.observer());
        const auto refused = global_allocation::refused;
        global_allocation::recording = true;
        global_allocation::fail_after = 0;
        {
            CachedScratchBacking backing(std::move(inner), {}, 2 * page, {}, capacity);
            global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
            require(global_allocation::refused == refused + 1
                    && global_allocation::live_bytes == 0
                    && backing.cache_slot_count() == 0 && backing.cache_payload_bytes() == 0
                    && backing.cache_metadata_bytes() == 0 && backing.cache_storage_bytes() == 0,
                    "failure of the optional shared slab leaves no payload or metadata");
            std::array<std::byte, page> image{}, out{};
            image[0] = std::byte{0x6d};
            global_allocation::refusing = true;
            const bool okay = backing.write_page(2, {image.data(), image.size()}) == ScratchStatus::ok
                && backing.read_page(2, {out.data(), out.size()}) == ScratchStatus::ok
                && backing.sync(ScratchDurability::buffered) == ScratchStatus::ok;
            global_allocation::refusing = false;
            require(okay && out == image && inner_counts.writes == 1 && inner_counts.reads == 1
                    && inner_counts.syncs == 1 && global_allocation::refused == refused + 1,
                    "bypass reads and writes the inner backing without retrying allocation");
            const auto stats = backing.read_cache_stats();
            require(stats.put_calls == 1 && stats.get_calls == 1
                    && stats.allocation_bypass_count == 1 && stats.current_bytes == 0,
                    "failed optional slab routes reads and writes through the shared cache bypass");
        }
        global_allocation::recording = false;
    }
}

void resident_slab_handoff_does_not_allocate() {
    std::array<std::byte, 6 * page> storage{};
    CommandScratchPhysicalIoStats physical;
    Counts logical;
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{storage.data(), storage.size()}, page, physical.observer());
    CachedScratchBacking backing(std::move(inner), logical.observer(), 2 * page, {}, 2 * page);
    std::array<std::byte, page> first{}, second{}, out{};
    first[17] = std::byte{0x35};
    second[31] = std::byte{0x79};
    const auto allocations = global_allocation::allocations;
    const auto refused = global_allocation::refused;
    global_allocation::refusing = true;
    const bool resident = backing.write_page(0, {first.data(), first.size()}) == ScratchStatus::ok
        && backing.write_page(1, {second.data(), second.size()}) == ScratchStatus::ok
        && backing.read_page(0, {out.data(), out.size()}) == ScratchStatus::ok;
    global_allocation::refusing = false;
    require(resident && out == first && physical.reads == 0 && physical.writes == 0
            && backing.read_cache_stats().put_calls == 0,
            "authoritative resident prefix has not been reused for shared-cache metadata");
    global_allocation::refusing = true;
    const bool spilled = backing.write_page(2, {second.data(), second.size()}) == ScratchStatus::ok
        && backing.read_page(0, {out.data(), out.size()}) == ScratchStatus::ok;
    global_allocation::refusing = false;
    const auto stats = backing.read_cache_stats();
    const auto line = ReadThroughCache::line_bytes(page);
    require(spilled && out == first && storage[17] == first[17]
            && storage[page + 31] == second[31] && storage[2 * page + 31] == second[31]
            && physical.writes == 3 && physical.reads == 1 && physical.completed_reads == 1
            && logical.writes == 3 && logical.reads == 2 && logical.failures == 0,
            "complete prefix lands before shared-cache reuse and charges remain logical");
    require(global_allocation::allocations == allocations && global_allocation::refused == refused
            && stats.put_calls == 1 && stats.get_calls == 1 && stats.misses == 1
            && stats.current_payload_bytes == 2 * page
            && stats.current_metadata_bytes == 2 * (line - page)
            && stats.current_bytes == 2 * line && stats.allocation_bypass_count == 0,
            "shared cache takes over the same slab without any I/O-time allocation");
}
} // namespace command_capacity_test

int main() {
    try {
        using namespace command_capacity_test;
        Directory directory;
        Environment selected("PINEFORGE_COMMAND_SCRATCH_DIR", directory.path.c_str());
        Environment no_default_cap("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", nullptr);
        Budget unlimited;
        tiny_vs_default_outputs();
        std::puts("PASS tiny_vs_default_outputs");
        cold_reads_and_hit_charges();
        std::puts("PASS cold_reads_and_hit_charges");
        default_payload_and_slots_are_unchanged();
        std::puts("PASS default_payload_and_slots_are_unchanged");
        optional_storage_failure_bypass();
        std::puts("PASS optional_storage_failure_bypass");
        resident_slab_handoff_does_not_allocate();
        std::puts("PASS resident_slab_handoff_does_not_allocate");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::refusing = false;
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        std::fprintf(stderr, "FAIL test_command_scratch_cache_capacity: %s\n", error.what());
        return 1;
    }
}
