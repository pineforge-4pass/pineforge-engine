#include "command_scratch_cache.hpp"
#include "command_scratch_production.hpp"

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <sys/types.h>
#include <utility>

namespace pineforge::detail {

CachedScratchBacking::CachedScratchBacking(std::unique_ptr<ScratchBacking> inner,
                                         ScratchIoObserver observer,
                                         std::size_t resident_bytes,
                                         std::shared_ptr<CommandScratchByteBudget> budget)
    : ScratchBacking(inner ? inner->kind() : ScratchBackingKind::disk,
                     inner ? inner->page_size() : 0, observer),
      inner_(std::move(inner)),
      slot_count_(page_size() >= kMinPageBytes && page_size() <= kCacheBytes
                      ? kCacheBytes / page_size() : 0),
      resident_pages_(slot_count_ != 0 ? std::min(resident_bytes, kCacheBytes) / page_size() : 0),
      // A null inner gives page_size() 0, so resident_pages_ is 0 and the
      // short-circuit skips the dereference: keep this order in the list.
      resident_(resident_pages_ != 0 && inner_->page_count() == 0),
      budget_(std::move(budget)) {
    if (!inner_)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "command scratch cache requires a backing");
    if (slot_count_ != 0) {
        bytes_.reset(new (std::nothrow) std::byte[kCacheBytes]);
        if (!bytes_)
            throw scratch_out_of_memory_failure();
        page_ids_.reset(new (std::nothrow) ScratchPageId[slot_count_]());
        if (!page_ids_)
            throw scratch_out_of_memory_failure();
        std::fill_n(page_ids_.get(), slot_count_, scratch_no_page);
    }
}

CachedScratchBacking::~CachedScratchBacking() noexcept {
    if (budget_) budget_->release(page_count() * static_cast<std::uint64_t>(page_size()));
}

std::uint64_t CachedScratchBacking::page_count() const noexcept {
    return resident_ ? resident_extent_ : inner_->page_count();
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
    if (resident_) {
        if (id >= resident_extent_) return ScratchStatus::not_found;
        std::memcpy(destination.data, bytes_.get() + static_cast<std::size_t>(id) * page_size(),
                    page_size());
        return ScratchStatus::ok;
    }
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
    // Preserve the file's offset check before byte admission, including when
    // a capped caller supplies an unrepresentable id.
    const auto size = static_cast<std::uint64_t>(page_size());
    if (id >= static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()) / size)
        return ScratchStatus::programming_error;
    const auto before = page_count();
    const auto growth = id >= before ? (id + 1 - before) * size : 0;
    if (budget_ && growth != 0 && !budget_->reserve(growth)) return ScratchStatus::io_error;
    const auto status = write(id, source);
    // A failed spill keeps the resident prefix authoritative. After spill,
    // ftruncate may have extended the file before a failed/short pwrite;
    // retain that real extent's charge, exactly as the all-file backing does.
    if (budget_ && growth != 0) {
        const auto unused = growth - (page_count() - before) * size;
        if (unused != 0) budget_->release(unused);
    }
    return status;
}

ScratchStatus CachedScratchBacking::write(ScratchPageId id, ScratchBytes source) noexcept {
    if (resident_) {
        if (id < resident_pages_) {
            const auto offset = static_cast<std::size_t>(id) * page_size();
            if (id > resident_extent_)
                std::memset(bytes_.get() + static_cast<std::size_t>(resident_extent_) * page_size(),
                            0, static_cast<std::size_t>(id - resident_extent_) * page_size());
            std::memcpy(bytes_.get() + offset, source.data, page_size());
            resident_extent_ = std::max(resident_extent_, id + 1);
            return ScratchStatus::ok;
        }
        // Spill the entire prefix once, including zero-filled holes. Do not
        // reuse any byte as read-cache storage until EVERY page has landed:
        // a failed/short spill leaves the complete prefix available to reads,
        // rollback and a subsequent retry without allocation.
        for (ScratchPageId page = 0; page < resident_extent_; ++page) {
            const auto status = inner_->write_page(page,
                {bytes_.get() + static_cast<std::size_t>(page) * page_size(), page_size()});
            if (status != ScratchStatus::ok) return status;
        }
        resident_ = false; // One-way transition; never returns to resident mode.
    }
    // A failed/short write may already have changed disk bytes or extended
    // the file. Never serve the old bytes and never publish the new ones
    // until the inner reports that the entire page was written.
    if (slot_count_ != 0) page_ids_[slot(id)] = scratch_no_page;
    const auto status = inner_->write_page(id, source);
    if (status == ScratchStatus::ok) remember(id, source.data);
    return status;
}

ScratchStatus CachedScratchBacking::do_sync(ScratchDurability durability) noexcept {
    // The fresh unlinked disk file has no page data while fully resident.
    // Arena/test backings retain their own supported durability statuses.
    if (resident_ && inner_->page_count() == 0 && kind() == ScratchBackingKind::disk
        && !inner_->arena_capacity_bytes())
        return ScratchStatus::ok;
    return inner_->sync(durability);
}

} // namespace pineforge::detail
