#include "command_scratch_cache.hpp"

#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pineforge::detail {

CachedScratchBacking::CachedScratchBacking(std::unique_ptr<ScratchBacking> inner,
                                         ScratchIoObserver observer)
    : ScratchBacking(inner ? inner->kind() : ScratchBackingKind::disk,
                     inner ? inner->page_size() : 0, observer),
      inner_(std::move(inner)),
      slot_count_(page_size() >= kMinPageBytes && page_size() <= kCacheBytes
                      ? kCacheBytes / page_size() : 0) {
    if (!inner_)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "command scratch cache requires a backing");
    if (slot_count_ != 0) {
        bytes_.reset(new (std::nothrow) std::byte[kCacheBytes]);
        if (!bytes_)
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(ENOMEM, std::generic_category(), "command scratch cache allocate"));
    }
    page_ids_.fill(scratch_no_page);
}

std::uint64_t CachedScratchBacking::page_count() const noexcept {
    return inner_->page_count();
}

std::size_t CachedScratchBacking::slot(ScratchPageId id) const noexcept {
    return static_cast<std::size_t>(id % slot_count_);
}

void CachedScratchBacking::remember(ScratchPageId id, const std::byte* bytes) const noexcept {
    if (slot_count_ == 0) return;
    const auto index = slot(id);
    std::memcpy(bytes_.get() + index * page_size(), bytes, page_size());
    page_ids_[index] = id;
}

ScratchStatus CachedScratchBacking::do_read_page(
        ScratchPageId id, ScratchBuffer destination) const noexcept {
    // Extent belongs to the inner, even if an entry happens to be cached.
    // Forward misses (including out-of-extent reads) for its exact status.
    if (slot_count_ != 0 && id < inner_->page_count()) {
        const auto index = slot(id);
        if (page_ids_[index] == id) {
            std::memcpy(destination.data, bytes_.get() + index * page_size(), page_size());
            return ScratchStatus::ok;
        }
    }
    const auto status = inner_->read_page(id, destination);
    if (status == ScratchStatus::ok) remember(id, destination.data);
    return status;
}

ScratchStatus CachedScratchBacking::do_write_page(
        ScratchPageId id, ScratchBytes source) noexcept {
    // A failed/short write may already have changed disk bytes or extended
    // the file. Never serve the old bytes and never publish the new ones
    // until the inner reports that the entire page was written.
    if (slot_count_ != 0) page_ids_[slot(id)] = scratch_no_page;
    const auto status = inner_->write_page(id, source);
    if (status == ScratchStatus::ok) remember(id, source.data);
    return status;
}

ScratchStatus CachedScratchBacking::do_sync(ScratchDurability durability) noexcept {
    return inner_->sync(durability);
}

} // namespace pineforge::detail
