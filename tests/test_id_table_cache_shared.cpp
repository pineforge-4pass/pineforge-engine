#include <pineforge/detail/read_through_cache.hpp>
#include <pineforge/detail/id_table_read_cache.hpp>

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "cache_sweep_fixture.hpp"

#include <memory>
#include <stdexcept>

namespace id_table_cache_shared_test {
using namespace pineforge::detail;
using acceptance_process::require;
using Id = IdTableReadCache;

// Named namespace: the friend-access instantiation has external linkage.
template<class Tag, auto Member>
struct Access { friend auto member(Tag) { return Member; } };
struct Core { friend auto member(Core); };
template struct Access<Core, &Id::cache_>;

struct BackingError : std::runtime_error {
    const void* owner;
    explicit BackingError(const void* file)
        : std::runtime_error("injected id backing I/O error"), owner(file) {}
};
struct File {
    std::vector<unsigned char> bytes;
    mutable std::uint64_t reads = 0, read_bytes = 0;
    bool fault = false;
    File(std::size_t length, unsigned salt) : bytes(length) {
        for (std::size_t i = 0; i < length; ++i)
            bytes[i] = static_cast<unsigned char>((i * 29 + (i / 4096) * 13 + salt) & 255);
    }
    void read(std::uint64_t offset, void* destination, std::size_t count) const {
        ++reads; // The inner transfer, below both adapters and cache statistics.
        if (fault) throw BackingError(this);
        require(offset <= bytes.size() && count <= bytes.size() - offset,
                "shared id cache read beyond authoritative extent");
        std::memcpy(destination, bytes.data() + offset, count);
        read_bytes += count;
    }
};

void allocation_census() {
    for (const auto policy : {Id::Policy{512, 0}, Id::Policy{512, 1},
                              Id::Policy{512, 3}, Id::Policy{512, 32},
                              Id::Policy{8192, 2}}) {
        File file(3 * policy.line_bytes + 19, 41);
        const auto before_bytes = global_allocation::live_bytes;
        {
            Id cache(policy);
            unsigned char observed = 0;
            const auto before_allocations = global_allocation::allocations;
            global_allocation::recording = true;
            cache.read(file, true, file.bytes.size(), 0, 13, &observed, 1);
            global_allocation::recording = false;
            const auto allocation_count = global_allocation::allocations - before_allocations;
            const auto expected = policy.lines * ReadThroughCache::line_bytes(policy.line_bytes);
            const auto transfer = policy.lines && policy.line_bytes > Id::kPageBytes
                ? policy.line_bytes : 0;
            const auto& core = cache.*member(Core{});
            require(observed == file.bytes[13] && file.reads == 1,
                    "allocation census must consume an actual cold backing byte");
            require(global_allocation::live_bytes - before_bytes == expected + transfer
                && allocation_count == (policy.lines ? 1U : 0U) + (transfer ? 1U : 0U),
                "allocator census includes all shared metadata and separate wide workspace");
            if (policy.lines) {
                require(core && core->status() == ScratchStatus::ok
                    && core->slot_count() == policy.lines
                    && core->policy().page_size == policy.line_bytes
                    && core->policy().capacity_bytes == expected
                    && core->policy().replacement == ReadThroughCacheReplacement::direct_mapped
                    && core->policy().write_policy == ReadThroughCacheWritePolicy::write_through,
                    "id policy configures the actual shared cache instance");
                const auto stats = core->stats();
                require(stats.current_payload_bytes == policy.lines * policy.line_bytes
                    && stats.current_bytes == expected && stats.peak_bytes == expected
                    && stats.current_metadata_bytes == expected - stats.current_payload_bytes
                    && stats.current_pins == 0,
                    "shared cache reports allocated payload, complete metadata and no escaping pins");
                cache.read(file, true, file.bytes.size(), 0, 13, &observed, 1);
                require(file.reads == 1 && core->stats().hits == 1,
                        "shared cache engagement is independently proven by no inner transfer");
            } else {
                require(!core, "disabled id cache constructs no shared allocation");
            }
            global_allocation::recording = true; // Include owner destruction in the census.
        }
        global_allocation::recording = false;
        require(global_allocation::live_bytes == before_bytes,
                "shared cache and wide workspace release every recorded byte");
    }
}

// The same 257-page paired-file workload as the reusable id-page acceptance
// adapter, using the shared cache's census instead of the retired private tags.
cache_sweep::Result id_pages(cache_sweep::Setting setting) {
    constexpr std::size_t page = Id::kPageBytes, pages = 257;
    File data(page * pages, 0), index(page * pages, 73);
    std::unique_ptr<Id> cache;
    if (setting.defaults) cache = std::make_unique<Id>();
    else cache = std::make_unique<Id>(Id::Policy{page, setting.units / page});
    const auto slots = setting.defaults ? 0 : setting.units / page;
    const auto inclusive = slots * ReadThroughCache::line_bytes(page);
    cache_sweep::Result result;
    auto& s = result.cache;
    s.configured_payload = slots * page;
    s.configured_metadata = inclusive - s.configured_payload;
    s.fixed_control = sizeof(Id);
    s.fixed_workspace = Id::kPageBytes;
    s.workspace = slots ? page : 0;
    result.output.records.reserve((2 * pages + 2) * page);
    const auto read = [&](unsigned file, std::size_t id) {
        const auto& source = file ? index : data;
        std::array<unsigned char, page> out{};
        const auto before = source.reads;
        cache->read(source, true, source.bytes.size(), file, id * page, out.data(), out.size());
        require(std::memcmp(out.data(), source.bytes.data() + id * page, page) == 0,
                "paired id page lost identity or complete backing bytes");
        if (source.reads == before) ++s.hits;
        else ++s.misses;
        result.output.records.append(reinterpret_cast<const char*>(out.data()), out.size());
    };
    read(0, 0);
    s.cold_reads = data.reads;
    read(0, 0);
    for (std::size_t id = 1; id < pages; ++id) {
        read(0, id);
        read(1, id);
    }
    const auto before_reread = data.reads;
    read(0, 0);
    s.evicted_reread_reads = data.reads - before_reread;
    cache->before_write(0, data.bytes.size(), 19, 1);
    data.bytes[19] ^= 0x5b;
    read(0, 0);
    const auto& core = (*cache).*member(Core{});
    if (slots) {
        require(core.has_value(), "enabled acceptance row never instantiated the shared cache");
        const auto stats = core->stats();
        s.payload = stats.current_payload_bytes;
        s.metadata = stats.current_metadata_bytes;
        s.peak_resident = stats.peak_bytes;
        s.evictions = stats.evictions;
    } else {
        require(!core, "zero acceptance policy unexpectedly instantiated a shared cache");
    }
    s.inner_reads = data.reads + index.reads;
    s.inner_bytes = data.read_bytes + index.read_bytes;
    cache->invalidate();
    data.fault = true;
    const auto before_fault = data.reads;
    bool surfaced = false;
    try { read(0, 0); }
    catch (const BackingError& error) { surfaced = error.owner == &data; }
    require(surfaced && data.reads == before_fault + 1,
            "id cold backing fault preserves the exact original exception");
    s.fault_reads = data.reads - before_fault;
    return result;
}
} // namespace id_table_cache_shared_test

int main() {
    try {
        id_table_cache_shared_test::allocation_census();
        cache_sweep::sweep("id_pages_shared", 4096, 0, 262144,
                           id_table_cache_shared_test::id_pages);
        std::puts("id shared cache allocation census and complete acceptance replay: OK");
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "id shared cache FAIL: %s\n", error.what());
        return 1;
    }
}
