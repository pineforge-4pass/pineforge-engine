#pragma once

#include <pineforge/detail/scratch_backing.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <array>
#include <cstddef>
#include <memory>

namespace pineforge::detail {

struct CommandScratchByteBudget;

// Exclusive resident/read-through decorator for any ScratchBacking. All access to the
// owned inner backing must go through this object; construct the inner with
// NO observer and install the logical I/O observer here, including cache hits.
// One fixed allocation serves first as a dense resident prefix, then as the
// read cache after spill. At most three stores per run share the fixed bound K
// below, including their decorator objects/slot keys. Nothing depends on extent
// or bars. I/O operations do not throw; a refused byte admission may allocate
// the bounded typed diagnostic carried to the owner.
//
// Page size is immutable per backing. Direct-mapped keys therefore identify
// pages in this one geometry (512 B through 1 MiB, including non-powers of
// two); pages above 64 KiB bypass the cache, as do sizes below 512 B.
class CachedScratchBacking final : public ScratchBacking {
public:
    static constexpr std::size_t kCacheBytes = 64 * 1024;
    // K = 198 KiB/run: three 64-KiB buffers plus 2 KiB of object/slot keys each.
    static constexpr std::size_t kCommandScratchMemoryBytes = 3 * (kCacheBytes + 2 * 1024);

    // The inner must be nonnull. A page-buffer allocation failure is a typed
    // scratch_io_failed std::system_error with ENOMEM, as for factory setup.
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
    // A byte-cap refusal belongs to this decorator; other failures belong to
    // the inner backing. Inspect only after a failed logical operation.
    const ReportScratchIoError* carried_failure() const noexcept override {
        return failure_ ? &*failure_ : inner_->carried_failure();
    }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override;
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override;
    ScratchStatus do_sync(ScratchDurability durability) noexcept override;

private:
    static constexpr std::size_t kMinPageBytes = 512;
    static constexpr std::size_t kMaxSlots = kCacheBytes / kMinPageBytes;

    std::size_t slot(ScratchPageId id) const noexcept;
    void remember(ScratchPageId id, const std::byte* bytes) const noexcept;
    ScratchStatus write(ScratchPageId id, ScratchBytes source) noexcept;

    std::unique_ptr<ScratchBacking> inner_;
    const std::size_t slot_count_;
    const std::size_t resident_pages_;
    bool resident_;
    std::uint64_t resident_extent_ = 0;
    std::shared_ptr<CommandScratchByteBudget> budget_;
    mutable std::optional<ReportScratchIoError> failure_;
    std::unique_ptr<std::byte[]> bytes_;
    mutable std::array<ScratchPageId, kMaxSlots> page_ids_{};
};

static_assert(sizeof(CachedScratchBacking) <= 2 * 1024,
              "three backing objects and page buffers must fit the fixed K");

} // namespace pineforge::detail
