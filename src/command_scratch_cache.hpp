#pragma once

#include <pineforge/detail/scratch_backing.hpp>

#include <array>
#include <cstddef>
#include <memory>

namespace pineforge::detail {

// Exclusive read-through decorator for any ScratchBacking. All access to the
// owned inner backing must go through this object; construct the inner with
// NO observer and install the logical I/O observer here, including cache hits.
// Construction reserves up to 64 KiB of page bytes per store, none when
// bypassed: at most three stores per run use 192 KiB per engine, plus fixed
// slot-key/object metadata. Nothing depends on extent or bars. No I/O operation
// allocates or throws.
//
// Page size is immutable per backing. Direct-mapped keys therefore identify
// pages in this one geometry (512 B through 1 MiB, including non-powers of
// two); pages above 64 KiB bypass the cache, as do sizes below 512 B.
class CachedScratchBacking final : public ScratchBacking {
public:
    static constexpr std::size_t kCacheBytes = 64 * 1024;

    // The inner must be nonnull. A page-buffer allocation failure is a typed
    // scratch_io_failed std::system_error with ENOMEM, as for factory setup.
    explicit CachedScratchBacking(std::unique_ptr<ScratchBacking> inner,
                                  ScratchIoObserver observer = {});
    ~CachedScratchBacking() noexcept override = default;

    std::uint64_t page_count() const noexcept override;
    // The decorator adds no state of its own to these two: the arena share the
    // stores size their directory by, and the typed failure the owner rethrows,
    // both belong to the inner backing.
    std::optional<std::uint64_t> arena_capacity_bytes() const noexcept override {
        return inner_->arena_capacity_bytes();
    }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return inner_->carried_failure();
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

    std::unique_ptr<ScratchBacking> inner_;
    const std::size_t slot_count_;
    std::unique_ptr<std::byte[]> bytes_;
    mutable std::array<ScratchPageId, kMaxSlots> page_ids_{};
};

} // namespace pineforge::detail
