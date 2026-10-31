#pragma once

#include "cache_sweep_fixture.hpp"
#include <pineforge/detail/read_through_cache.hpp>
#if !defined(PINEFORGE_CACHE_SWEEP_ID_ONLY)
#include "../src/command_scratch_cache.hpp"
#include <pineforge/detail/scratch_arena.hpp>
#endif
#if !defined(PINEFORGE_CACHE_SWEEP_COMMAND_ONLY)
#include <pineforge/detail/id_table_read_cache.hpp>
#endif

#include <cerrno>
#include <memory>

// Compile each selector only with its store's production header closure.
// The id adapter has no public statistics port; observe its actual shared
// cache in a named namespace, without probing the retired private tag layout.
namespace cache_sweep_access {
#if !defined(PINEFORGE_CACHE_SWEEP_COMMAND_ONLY)
using Id = pineforge::detail::IdTableReadCache;
template<class Tag, auto Member>
struct Reader { friend auto member(Tag) { return Member; } };
struct IdCore { friend auto member(IdCore); };
template struct Reader<IdCore, &Id::cache_>;
#endif
} // namespace cache_sweep_access

namespace cache_sweep {
using namespace pineforge::detail;

#if !defined(PINEFORGE_CACHE_SWEEP_ID_ONLY)
class PageBacking final : public ScratchBacking {
public:
    PageBacking(std::size_t size, std::size_t pages)
        : ScratchBacking(ScratchBackingKind::test_memory, size), bytes(size * pages) {
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>((i * 37 + (i / size) * 11) & 255);
    }
    std::uint64_t page_count() const noexcept override { return bytes.size() / page_size(); }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return fault ? &failure : nullptr;
    }
    mutable std::uint64_t reads = 0, read_bytes = 0;
    bool fault = false;
    std::vector<std::byte> bytes;
    ReportScratchIoError failure{EIO, "command sweep backing read", {{"reason", "scratch_io"}}};
protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer out) const noexcept override {
        ++reads; // INNER primitive, below CachedScratchBacking and its observer.
        if (fault) return ScratchStatus::io_error;
        if (id >= page_count()) return ScratchStatus::not_found;
        std::memcpy(out.data, bytes.data() + id * page_size(), out.size);
        read_bytes += out.size;
        return ScratchStatus::ok;
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes in) noexcept override {
        if (id >= page_count()) return ScratchStatus::io_error;
        std::memcpy(bytes.data() + id * page_size(), in.data, in.size);
        return ScratchStatus::ok;
    }
    ScratchStatus do_sync(ScratchDurability) noexcept override { return ScratchStatus::ok; }
};

inline Result command_pages(Setting setting) {
    using Command = CachedScratchBacking;
    constexpr std::size_t page = 512, pages = 1025;
    auto backing = std::make_unique<PageBacking>(page, pages);
    auto* inner = backing.get();
    std::unique_ptr<Command> cache;
    if (setting.defaults) cache = std::make_unique<Command>(std::move(backing));
    else cache = std::make_unique<Command>(std::move(backing), ScratchIoObserver{}, 0,
        std::shared_ptr<CommandScratchByteBudget>{}, setting.units);
    Result result;
    auto& s = result.cache;
    const auto configured = setting.defaults ? Command::kCacheBytes : setting.units;
    s.configured_payload = configured;
    s.configured_metadata = (configured / page) * (ReadThroughCache::line_bytes(page) - page);
    s.fixed_control = sizeof(Command); // Includes the shared cache control.
    const auto read = [&](std::size_t id) {
        std::array<std::byte, page> out{};
        require(cache->read_page(id, {out.data(), out.size()}) == ScratchStatus::ok,
                "command page cold read refused");
        require(std::memcmp(out.data(), inner->bytes.data() + id * page, page) == 0,
                "command page read changed backing bytes");
        result.output.records.append(reinterpret_cast<const char*>(out.data()), out.size());
    };
    const auto cold = inner->reads;
    read(0);
    s.cold_reads = inner->reads - cold;
    read(0); // Actual warm hit; no observer counter is used as evidence.
    for (std::size_t id = 1; id < pages; ++id) read(id);
    const auto reread = inner->reads;
    read(0);
    s.evicted_reread_reads = inner->reads - reread;
    // A write-through update and reread uses the same input at every size.
    std::array<std::byte, page> update{};
    update[31] = std::byte{0xe1};
    require(cache->write_page(0, {update.data(), update.size()}) == ScratchStatus::ok,
            "command write-through failed");
    require(std::memcmp(inner->bytes.data(), update.data(), update.size()) == 0,
            "command write-through did not update every authoritative byte");
    read(0);
    const auto stats = cache->read_cache_stats();
    s.payload = cache->cache_payload_bytes();
    s.metadata = cache->cache_metadata_bytes();
    s.peak_resident = cache->cache_storage_bytes();
    s.hits = stats.hits;
    s.misses = stats.misses;
    s.evictions = stats.evictions;
    require(stats.current_payload_bytes == s.payload && stats.current_metadata_bytes == s.metadata
        && stats.peak_bytes == s.peak_resident,
            "command slab accounting disagrees with its actual shared cache");
    s.workspace = page;
    // Force an uncached valid page; a real backing error must surface.
    for (std::size_t id = 1; id < pages; ++id) {
        std::array<std::byte, page> out{};
        require(cache->read_page(id, {out.data(), out.size()}) == ScratchStatus::ok,
                "command fault preparation failed");
    }
    inner->fault = true;
    const auto before_fault = inner->reads;
    std::array<std::byte, page> out{};
    require(cache->read_page(0, {out.data(), out.size()}) == ScratchStatus::io_error
        && inner->reads == before_fault + 1 && cache->carried_failure() == &inner->failure,
            "command cold backing fault did not preserve its original typed failure");
    s.fault_reads = inner->reads - before_fault;
    s.inner_reads = inner->reads;
    s.inner_bytes = inner->read_bytes;
    return result;
}
#endif

#if !defined(PINEFORGE_CACHE_SWEEP_COMMAND_ONLY)
struct IdBackingError : std::runtime_error {
    const void* owner;
    explicit IdBackingError(const void* source)
        : std::runtime_error("injected id backing I/O error"), owner(source) {}
};
struct IdFile {
    std::vector<unsigned char> bytes;
    mutable std::uint64_t reads = 0, read_bytes = 0;
    mutable bool fault = false;
    IdFile(std::size_t length, unsigned salt) : bytes(length) {
        for (std::size_t i = 0; i < length; ++i)
            bytes[i] = static_cast<unsigned char>((i * 29 + (i / 4096) * 13 + salt) & 255);
    }
    void read(std::uint64_t offset, void* destination, std::size_t count) const {
        ++reads; // INNER primitive below the id decorator.
        if (fault) throw IdBackingError(this);
        require(offset <= bytes.size() && count <= bytes.size() - offset,
                "id cache read beyond authoritative extent");
        std::memcpy(destination, bytes.data() + offset, count);
        read_bytes += count;
    }
};

inline Result id_pages(Setting setting) {
    using namespace cache_sweep_access;
    constexpr std::size_t page = Id::kPageBytes, pages = 257;
    IdFile data(page * pages, 0), index(page * pages, 73);
    std::unique_ptr<Id> cache;
    if (setting.defaults) cache = std::make_unique<Id>();
    else cache = std::make_unique<Id>(Id::Policy{page, setting.units / page});
    Result result;
    auto& s = result.cache;
    const auto slots = setting.defaults ? 0 : setting.units / page;
    s.configured_payload = slots * page;
    s.configured_metadata = slots * (ReadThroughCache::line_bytes(page) - page);
    s.fixed_control = sizeof(Id);
    s.fixed_workspace = page; // Internal copy-out buffer at this geometry.
    s.workspace = 2 * page; // Internal transfer and the caller's owning page.
    const auto read = [&](unsigned file, std::size_t id) {
        const auto& source = file ? index : data;
        std::array<unsigned char, page> out{};
        const auto before = source.reads;
        cache->read(source, true, source.bytes.size(), file, id * page, out.data(), out.size());
        require(std::memcmp(out.data(), source.bytes.data() + id * page, page) == 0,
                "paired id page lost identity or bytes");
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
    const auto before = data.reads;
    read(0, 0);
    s.evicted_reread_reads = data.reads - before;
    cache->before_write(0, data.bytes.size(), 19, 1);
    data.bytes[19] ^= 0x5b;
    read(0, 0);
    const auto& core = (*cache).*member(IdCore{});
    if (slots) {
        require(core.has_value(), "enabled id sweep never constructed its shared cache");
        const auto stats = core->stats();
        require(core->slot_count() == slots && core->policy().page_size == page
            && core->policy().capacity_bytes == s.configured_payload + s.configured_metadata
            && stats.hits == s.hits && stats.misses == s.misses,
                "id shared cache policy or statistics disagree with inner page transfers");
        s.payload = stats.current_payload_bytes;
        s.metadata = stats.current_metadata_bytes;
        s.peak_resident = stats.peak_bytes;
        s.evictions = stats.evictions;
    } else {
        require(!core, "zero id policy retained a shared cache instance");
    }
    cache->invalidate();
    data.fault = true;
    const auto attempts = data.reads;
    bool surfaced = false;
    try { read(0, 0); }
    catch (const IdBackingError& error) { surfaced = error.owner == &data; }
    require(surfaced && data.reads == attempts + 1, "id cold backing fault did not surface");
    s.fault_reads = data.reads - attempts;
    s.inner_reads = data.reads + index.reads;
    s.inner_bytes = data.read_bytes + index.read_bytes;
    return result;
}
#endif
} // namespace cache_sweep
