#pragma once

#include <pineforge/detail/scratch_backing.hpp>

#include <array>
#include <cstddef>
#include <memory>

namespace pineforge::detail {

struct CommandScratchByteBudget;

// Exclusive resident/read-through decorator for any ScratchBacking. All access to the
// owned inner backing must go through this object; construct the inner with
// NO observer and install the logical I/O observer here, including cache hits.
// One fixed buffer serves first as a dense resident prefix, then as the
// read cache after spill. Its slot table uses the wrapped backing's slot count.
// K below charges the worst case (512-byte pages) for three stores, including
// decorator objects/slot keys. Nothing depends on extent or bars; no I/O operation allocates or throws.
//
// Page size is immutable per backing. Direct-mapped keys therefore identify
// pages in this one geometry (512 B through 1 MiB, including non-powers of
// two); pages above kCacheBytes bypass the cache, as do sizes below 512 B.
class CachedScratchBacking final : public ScratchBacking {
public:
    static constexpr std::size_t kCacheBytes = 64 * 1024;
    static constexpr std::size_t kMinPageBytes = 512, kFixedObjectBytes = 1024, kMaxSlots = kCacheBytes / kMinPageBytes;
    static constexpr std::size_t kCommandScratchMemoryBytes =
        3 * (kCacheBytes + kMaxSlots * sizeof(ScratchPageId) + kFixedObjectBytes); // K charges three buffers, 512-byte-page tables and fixed objects.
    // The inner must be nonnull. A page-buffer or slot-table allocation failure is a typed
    // out_of_memory failure carried by the shared, prebuilt scratch exception.
    // resident_bytes is a construction-only cap, clamped to kCacheBytes;
    // zero retains the all-file/write-through mode. Only a fresh inner can
    // start resident. A budget here must not also be installed on the inner.
    explicit CachedScratchBacking(std::unique_ptr<ScratchBacking> inner,
                                  ScratchIoObserver observer = {},
                                  std::size_t resident_bytes = 0,
                                  std::shared_ptr<CommandScratchByteBudget> budget = {});
    ~CachedScratchBacking() noexcept override;

    std::uint64_t page_count() const noexcept override;
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
    // Cache geometry constants are public so the fixed bound names each component.
    // The remaining members carry the backing and cache state.

    std::size_t slot(ScratchPageId id) const noexcept;
    void remember(ScratchPageId id, const std::byte* bytes) const noexcept;
    ScratchStatus write(ScratchPageId id, ScratchBytes source) noexcept;

    std::unique_ptr<ScratchBacking> inner_;
    const std::size_t slot_count_;
    const std::size_t resident_pages_;
    bool resident_;
    std::uint64_t resident_extent_ = 0;
    std::shared_ptr<CommandScratchByteBudget> budget_;
    std::unique_ptr<std::byte[]> bytes_;
    mutable std::unique_ptr<ScratchPageId[]> page_ids_;
};

static_assert(sizeof(CachedScratchBacking) <= CachedScratchBacking::kFixedObjectBytes,
              "the fixed cache object must fit its fixed K component");

} // namespace pineforge::detail
