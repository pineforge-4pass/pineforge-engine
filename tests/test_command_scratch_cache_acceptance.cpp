// Requires the common acceptance fixtures in the integrated test closure.
// Compile against the previous production header closure first.
#include "cache_sweep_fixture.hpp"
#include "../src/command_scratch_cache.hpp"
#include <pineforge/detail/scratch_arena.hpp>

#include <cerrno>

namespace command_cache_acceptance {
using namespace pineforge::detail;
using cache_sweep::require;

constexpr std::size_t page = 512;
constexpr std::size_t pages = 1025; // More than the largest 512-line policy.

class Backing final : public ScratchBacking {
public:
    explicit Backing(CommandScratchPhysicalIoStats& physical)
        : ScratchBacking(ScratchBackingKind::test_memory, page, physical.observer()),
          bytes(page * pages) {
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>((i * 37 + (i / page) * 11) & 255);
    }
    std::uint64_t page_count() const noexcept override { return pages; }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return fault ? &failure : nullptr;
    }
    bool fault = false;
    std::vector<std::byte> bytes;
    ReportScratchIoError failure{EIO, "command cache acceptance read", {{"reason", "scratch_io"}}};
protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer out) const noexcept override {
        if (fault) return ScratchStatus::io_error;
        if (id >= page_count()) return ScratchStatus::not_found;
        std::memcpy(out.data, bytes.data() + id * page, out.size);
        return ScratchStatus::ok;
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes in) noexcept override {
        if (id >= page_count()) return ScratchStatus::programming_error;
        std::memcpy(bytes.data() + id * page, in.data, in.size);
        return ScratchStatus::ok;
    }
    ScratchStatus do_sync(ScratchDurability) noexcept override { return ScratchStatus::ok; }
};

cache_sweep::Result run(cache_sweep::Setting setting) {
    CommandScratchPhysicalIoStats physical;
    auto backing = std::make_unique<Backing>(physical);
    auto* inner = backing.get();
    std::unique_ptr<CachedScratchBacking> cache;
    if (setting.defaults) cache = std::make_unique<CachedScratchBacking>(std::move(backing));
    else cache = std::make_unique<CachedScratchBacking>(std::move(backing), ScratchIoObserver{}, 0,
        std::shared_ptr<CommandScratchByteBudget>{}, setting.units);
    cache_sweep::Result result;
    auto& s = result.cache;
    s.configured_payload = setting.defaults ? CachedScratchBacking::kCacheBytes : setting.units;
    s.configured_metadata = (s.configured_payload / page)
        * (ReadThroughCache::line_bytes(page) - page);
    s.fixed_control = sizeof(CachedScratchBacking); // Includes the shared control object.
    s.workspace = page; // Caller copy-out buffer; no second cache payload allocation.
    const auto read = [&](std::size_t id) {
        std::array<std::byte, page> out{};
        require(cache->read_page(id, {out.data(), out.size()}) == ScratchStatus::ok,
                "shared command cache refused a valid page");
        require(std::memcmp(out.data(), inner->bytes.data() + id * page, page) == 0,
                "shared command cache changed backing bytes");
        result.output.records.append(reinterpret_cast<const char*>(out.data()), out.size());
    };
    const auto cold = physical.completed_reads;
    read(0);
    s.cold_reads = physical.completed_reads - cold;
    read(0);
    for (std::size_t id = 1; id < pages; ++id) read(id);
    const auto reread = physical.completed_reads;
    read(0);
    s.evicted_reread_reads = physical.completed_reads - reread;
    std::array<std::byte, page> update{};
    update[31] = std::byte{0xe1};
    require(cache->write_page(0, {update.data(), update.size()}) == ScratchStatus::ok,
            "shared command cache write-through failed");
    require(std::memcmp(inner->bytes.data(), update.data(), update.size()) == 0,
            "command acceptance write preserves every submitted update byte in the backing");
    read(0);
    const auto stats = cache->read_cache_stats();
    s.payload = cache->cache_payload_bytes();
    s.metadata = cache->cache_metadata_bytes();
    s.peak_resident = cache->cache_storage_bytes();
    s.hits = stats.hits;
    s.misses = stats.misses;
    s.evictions = stats.evictions;
    s.inner_reads = physical.reads;
    s.inner_bytes = physical.read_bytes;
    require(physical.writes == 1 && physical.failures == 0,
            "command acceptance update reached the inner writer exactly once");
    // Touch each later page so page zero is cold at every selected capacity.
    for (std::size_t id = 1; id < pages; ++id) {
        std::array<std::byte, page> out{};
        require(cache->read_page(id, {out.data(), out.size()}) == ScratchStatus::ok,
                "command acceptance fault preparation failed");
    }
    inner->fault = true;
    const auto attempts = physical.reads;
    std::array<std::byte, page> out{};
    require(cache->read_page(0, {out.data(), out.size()}) == ScratchStatus::io_error
            && physical.reads == attempts + 1 && physical.failures == 1
            && cache->carried_failure() == &inner->failure,
            "cold command read must surface the original inner I/O failure");
    s.fault_reads = physical.reads - attempts;
    return result;
}
} // namespace command_cache_acceptance

int main() {
    try {
        cache_sweep::sweep("command_pages", 512, 65536, 262144, command_cache_acceptance::run);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "shared command page sweep FAIL: %s\n", error.what());
        return 1;
    }
}
