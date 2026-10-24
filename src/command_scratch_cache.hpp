#pragma once

#include <pineforge/detail/scratch_backing.hpp>

#include <cstddef>
#include <limits>
#include <memory>

namespace pineforge::detail {

// Exclusive write-behind decorator. ALL inner access, including reopen and
// journal/undo reads, must go through this object. A bypassing reader must
// first call drain() and check its status. Install the logical observer HERE,
// including hits; the owned inner has no logical observer.
//
// One integer page budget, fixed at construction, independent of extent/bars.
// The one allocation holds Slot[pages] followed by pages * page_size bytes.
// sizeof(Slot) includes the dirty flag AND padding; resident_bytes includes
// the object itself. Allocator bookkeeping and the inner/Core are separate.
// No I/O operation allocates or throws. Buffered commits may remain resident;
// explicit sync/drain and orderly destruction write back, stronger sync also
// invokes the inner durability barrier. Buffered has no abrupt-death guarantee.
class CachedScratchBacking final : public ScratchBacking, public ScratchBackingDrain {
    struct Slot {
        ScratchPageId id = scratch_no_page;
        bool dirty = false;
    };

public:
    static constexpr std::size_t kDefaultEntries = 1024;
    static constexpr std::size_t kMaxEntries = 16384;
    static constexpr std::size_t kMaxResidentBytes = 8 * 1024 * 1024;

    static constexpr std::size_t slot_metadata_bytes() noexcept { return sizeof(Slot); }
    // Saturates for unrepresentable input, including values not admitted by
    // the constructor. Valid budgets are <=max_entries(page_bytes).
    static constexpr std::size_t resident_bytes(
            std::size_t pages, std::size_t page_bytes) noexcept {
        constexpr auto limit = std::numeric_limits<std::size_t>::max();
        return page_bytes > limit - sizeof(Slot)
                || pages > (limit - sizeof(CachedScratchBacking)) / (page_bytes + sizeof(Slot))
            ? limit : sizeof(CachedScratchBacking) + pages * (page_bytes + sizeof(Slot));
    }
    static constexpr std::size_t max_entries(std::size_t page_bytes) noexcept {
        if (page_bytes == 0 || page_bytes > kMaxResidentBytes - sizeof(Slot)) return 0;
        const auto pages = (kMaxResidentBytes - sizeof(CachedScratchBacking))
            / (page_bytes + sizeof(Slot));
        return pages < kMaxEntries ? pages : kMaxEntries;
    }
    static constexpr std::size_t default_entries(std::size_t page_bytes) noexcept {
        const auto maximum = max_entries(page_bytes);
        return maximum < kDefaultEntries ? maximum : kDefaultEntries;
    }

    // Admit extent WITHOUT writing page contents. Production callbacks keep
    // byte-cap/arena admission, representable offsets and zero holes immediate.
    // On ok the inner advertises id < page_count(). Required for write-behind.
    using Admission = ScratchStatus (*)(ScratchBacking&, ScratchPageId) noexcept;

    // Zero/oversized budgets: coded invalid_argument, scratch_capacity_exceeded
    // resource=layout. Allocation failure: scratch_io_failed/ENOMEM. The
    // explicit write-through mode is the physical-I/O oracle, with the same
    // logical calls, allocation/record bytes and fixed memory accounting.
    CachedScratchBacking(std::unique_ptr<ScratchBacking> inner, std::size_t pages,
                         ScratchIoObserver observer = {}, Admission admission = nullptr,
                         bool write_through = false);
    ~CachedScratchBacking() noexcept override;

    std::uint64_t page_count() const noexcept override;
    std::size_t resident_entries() const noexcept { return slot_count_; }
    std::size_t allocated_bytes() const noexcept { return allocation_bytes_; }
    // Checked close/drain boundary; also required before direct inner reads.
    // Failure leaves unwritten slots dirty and retriable. Does not close the
    // backing, so carried_failure remains inspectable. Physical only: unlike
    // explicit sync(), this does not add a logical observer charge.
    ScratchStatus drain() noexcept override { return result(flush_dirty()); }
    std::optional<std::uint64_t> arena_capacity_bytes() const noexcept override {
        return inner_->arena_capacity_bytes();
    }
    // Borrow the inner failure only for the last failed logical operation.
    // A successful hit clears the cache's diagnostic view without inner I/O;
    // Core snapshots the original failure before cleanup, as before.
    const ReportScratchIoError* carried_failure() const noexcept override {
        return last_io_failed_ ? inner_->carried_failure() : nullptr;
    }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override;
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override;
    ScratchStatus do_sync(ScratchDurability durability) noexcept override;

private:
    std::size_t slot(ScratchPageId id) const noexcept;
    Slot& entry(std::size_t index) const noexcept;
    ScratchStatus result(ScratchStatus status) const noexcept {
        last_io_failed_ = status == ScratchStatus::io_error;
        return status;
    }
    ScratchStatus flush_slot(std::size_t index) const noexcept;
    ScratchStatus flush_dirty() const noexcept;
    void remember(std::size_t index, ScratchPageId id,
                  const std::byte* bytes, bool dirty) const noexcept;

    std::unique_ptr<ScratchBacking> inner_;
    const std::size_t slot_count_;
    const Admission admission_;
    const bool write_through_;
    std::size_t allocation_bytes_ = 0;
    std::unique_ptr<std::byte[]> storage_;
    std::byte* bytes_ = nullptr;
    mutable bool last_io_failed_ = false;
};

} // namespace pineforge::detail
