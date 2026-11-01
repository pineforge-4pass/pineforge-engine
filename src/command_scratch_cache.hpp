#pragma once

#include <pineforge/detail/read_through_cache.hpp>
#include "run_cache_diagnostics.hpp"

#include <cstddef>
#include <memory>

namespace pineforge::detail {

struct CommandScratchByteBudget;

// Exclusive resident/read-through decorator for any ScratchBacking. All access to the
// owned inner backing must go through this object. Install the logical I/O
// observer here, including cache hits; an inner observer measures only backing
// operations and must not charge the logical budget a second time.
// One optional construction-time slab serves first as a dense resident prefix,
// then as shared-cache storage after spill. The default bound includes up to
// five owners: trade, dependency, exit, lazy book and lazy sequence index.
// Nothing depends on extent or bars. No I/O operation allocates or throws.
//
// Page size is immutable per backing. Direct-mapped keys therefore identify
// pages in this one geometry, including non-powers of two. Pages below 512 B
// or above the selected buffer capacity bypass the cache.
class CachedScratchBacking final : public ScratchBacking {
public:
    static constexpr std::size_t kCacheBytes = 64 * 1024;
    // Five default payloads plus all shared per-line metadata and <=2 KiB of
    // fixed control per owner. Derived from line_bytes(), not a private tag
    // layout. The 512-byte geometry has the largest default line count.
    static const std::size_t kCommandScratchMemoryBytes;
    static std::size_t cache_allocation_bytes(
        std::size_t page_size, std::size_t cache_bytes = kCacheBytes) noexcept;

    // The inner must be nonnull. Optional slab allocation failure
    // disables both caching and the resident prefix; reads/writes then go
    // directly to the inner, with the same logical observer and byte budget.
    // read_cache_stats().allocation_bypass_count reports that failure once,
    // immediately after construction. This instance never retries the slab.
    // cache_bytes is payload capacity, separate from this object's fixed
    // control and floor(cache_bytes/page_size) complete shared-cache tags.
    // Zero/sub-page/unrepresentable capacity bypasses without refusing data.
    // resident_bytes is a construction-only prefix cap bounded by cache_bytes;
    // zero retains write-through mode. Only a fresh inner can start resident.
    // A budget here must not also be installed on the inner.
    explicit CachedScratchBacking(std::unique_ptr<ScratchBacking> inner,
                                  ScratchIoObserver observer = {},
                                  std::size_t resident_bytes = 0,
                                  std::shared_ptr<CommandScratchByteBudget> budget = {},
                                  std::size_t cache_bytes = kCacheBytes);
    ~CachedScratchBacking() noexcept override;

    std::uint64_t page_count() const noexcept override;
    std::size_t cache_slot_count() const noexcept { return bytes_ ? cache_.slot_count() : 0; }
    std::size_t cache_payload_bytes() const noexcept { return bytes_ ? cache_bytes_ : 0; }
    std::size_t cache_metadata_bytes() const noexcept {
        return bytes_ ? storage_bytes_ - cache_bytes_ : 0;
    }
    std::size_t cache_storage_bytes() const noexcept { return bytes_ ? storage_bytes_ : 0; }
    // Core statistics start when the slab becomes read-cache storage. Prefix
    // residency and reserved payload slack are counted by the accessors above.
    // ScratchBacking reports no partial byte count: core completed-byte totals
    // include only operations that returned ok, not failed syscall prefixes.
    ReadThroughCacheStats read_cache_stats() const noexcept {
        auto stats = cache_.stats();
        // The core borrows lazily on first I/O. Disclose the constructor's
        // failed slab now, without double-counting that later null borrow.
        // A disabled/unsupported geometry requested no allocation.
        if (storage_bytes_ != 0 && !bytes_) stats.allocation_bypass_count = 1;
        return stats;
    }
    // The arena share used to size the store directory and the typed failure
    // rethrown by the owner both belong to the inner backing.
    std::optional<std::uint64_t> arena_capacity_bytes() const noexcept override {
        return inner_->arena_capacity_bytes();
    }
    // Forwards the inner backing's last failure. A cache hit does not reset
    // it, so this is meaningful only after a failed logical operation.
    const ReportScratchIoError* carried_failure() const noexcept override {
        return inner_->carried_failure();
    }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override;
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override;
    ScratchStatus do_sync(ScratchDurability durability) noexcept override;

private:
    static constexpr std::size_t kMinPageBytes = 512;

    static ReadThroughCachePolicy cache_policy(
        std::size_t page_size, std::size_t cache_bytes) noexcept;
    static ReadThroughCacheIoResult read_inner(
        void*, ReadThroughCacheKey, ScratchBuffer) noexcept;
    static ReadThroughCacheIoResult write_inner(
        void*, ReadThroughCacheKey, ScratchBytes) noexcept;
    static void* borrow_storage(void*, std::size_t) noexcept;
    static void release_storage(void*, void*, std::size_t) noexcept;
    ScratchStatus write(ScratchPageId id, ScratchBytes source) noexcept;

    std::unique_ptr<ScratchBacking> inner_;
    const std::size_t cache_bytes_;
    const std::size_t storage_bytes_;
    const std::size_t resident_pages_;
    bool resident_;
    std::uint64_t resident_extent_ = 0;
    std::shared_ptr<CommandScratchByteBudget> budget_;
    // Destruction order matters: core Slot lifetimes end before the slab is
    // freed. The custom allocator only borrows this already allocated storage.
    std::unique_ptr<std::byte[]> bytes_;
    mutable ReadThroughCache cache_;
    RunCacheSource cache_diagnostics_{this, [](const void* owner) noexcept {
        const auto& self = *static_cast<const CachedScratchBacking*>(owner);
        return static_cast<std::uint64_t>(sizeof(CachedScratchBacking)
                                         + self.cache_storage_bytes());
    }};
};

// Only fixed control lives inline: std::size_t capacities and 64-bit page ids
// impose no object-size cap on the separately allocated cache storage.
static_assert(sizeof(CachedScratchBacking) <= 2 * 1024,
              "fixed cache control must fit its per-owner allowance");

// Internal factory policy seam: resident prefix and optional cache payload are
// independently selected for this instance. Defaults in the production header
// retain the existing 64-KiB policy. Attached arenas always use direct admission.
std::unique_ptr<ScratchBacking> make_command_scratch_backing(
    std::size_t page_size, std::size_t resident_bytes, std::size_t cache_bytes);

// Diagnostic observer at the inner file/arena boundary. Each factory call
// snapshots it independently of the logical charged-I/O observer. It sees
// attempted inner operations (including errors), never decorator hits. A
// successful in-extent read proves backing readback; syscall counters can
// additionally distinguish requests from OS transfers. Context must outlive
// all backings constructed under the registration. Returns the prior value.
ScratchIoObserver set_command_scratch_physical_io_observer(ScratchIoObserver observer) noexcept;

// Optional, caller-owned diagnostics. Production updates these counters from
// the inner operation's completion callback, independently of cache statistics.
// Byte totals are requested bytes; completed_reads counts only successful page
// reads. The object must outlive backings that snapshot its observer.
struct CommandScratchPhysicalIoStats {
    std::uint64_t reads = 0, writes = 0, syncs = 0, failures = 0;
    std::uint64_t read_bytes = 0, write_bytes = 0, completed_reads = 0;
    ScratchIoObserver observer() noexcept { return {this, charge}; }
private:
    static void charge(void*, const ScratchIoCharge&) noexcept;
};

} // namespace pineforge::detail
