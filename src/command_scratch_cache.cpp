#include "command_scratch_cache.hpp"
#include "command_scratch_production.hpp"

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <sys/types.h>
#include <utility>

namespace pineforge::detail {

std::size_t CachedScratchBacking::cache_allocation_bytes(
        std::size_t geometry, std::size_t capacity) noexcept {
    const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (geometry < kMinPageBytes || capacity > maximum) return 0;
    const auto slots = capacity / geometry;
    const auto line = ReadThroughCache::line_bytes(geometry);
    if (!slots || !line) return 0;
    const auto metadata = line - geometry;
    if (metadata > (maximum - capacity) / slots) return 0;
    // Preserve the old payload capacity, including its trailing sub-page
    // slack, and add every shared tag/pin/replacement byte explicitly.
    return capacity + slots * metadata;
}

const std::size_t CachedScratchBacking::kCommandScratchMemoryBytes =
    5 * (cache_allocation_bytes(kMinPageBytes) + 2 * 1024);

ReadThroughCachePolicy CachedScratchBacking::cache_policy(
        std::size_t geometry, std::size_t capacity) noexcept {
    const auto storage = cache_allocation_bytes(geometry, capacity);
    const auto bytes = storage ? (capacity / geometry) * ReadThroughCache::line_bytes(geometry) : 0;
    return {bytes, geometry, ReadThroughCacheReplacement::direct_mapped,
            ReadThroughCacheWritePolicy::write_through};
}

CachedScratchBacking::CachedScratchBacking(std::unique_ptr<ScratchBacking> inner,
                                         ScratchIoObserver observer,
                                         std::size_t resident_bytes,
                                         std::shared_ptr<CommandScratchByteBudget> budget,
                                         std::size_t cache_bytes)
    : ScratchBacking(inner ? inner->kind() : ScratchBackingKind::disk,
                     inner ? inner->page_size() : 0, observer),
      inner_(std::move(inner)),
      cache_bytes_(cache_bytes),
      storage_bytes_(cache_allocation_bytes(page_size(), cache_bytes)),
      resident_pages_(storage_bytes_ != 0
                          ? std::min(resident_bytes / page_size(), cache_bytes / page_size()) : 0),
      resident_(false),
      budget_(std::move(budget)),
      cache_(cache_policy(page_size(), cache_bytes),
             {inner_.get(), read_inner, write_inner},
             {this, borrow_storage, release_storage}) {
    if (!inner_)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "command scratch cache requires a backing");
    if (storage_bytes_ != 0) {
        bytes_.reset(new (std::nothrow) std::byte[storage_bytes_]);
        if (!bytes_) cache_diagnostics_.allocation_bypassed();
    }
    // No prefix has been admitted if the optional allocation failed. Core's
    // allocator will return null once, permanently selecting direct I/O.
    // read_cache_stats() exposes that failure even before the first access.
    resident_ = bytes_ && resident_pages_ != 0 && inner_->page_count() == 0;
}

CachedScratchBacking::~CachedScratchBacking() noexcept {
    if (budget_) budget_->release(page_count() * static_cast<std::uint64_t>(page_size()));
}

std::uint64_t CachedScratchBacking::page_count() const noexcept {
    return resident_ ? resident_extent_ : inner_->page_count();
}

void* CachedScratchBacking::borrow_storage(void* context, std::size_t size) noexcept {
    auto& self = *static_cast<CachedScratchBacking*>(context);
    // Never construct shared tags over authoritative resident bytes.
    return !self.resident_ && size <= self.storage_bytes_ ? self.bytes_.get() : nullptr;
}

void CachedScratchBacking::release_storage(void*, void*, std::size_t) noexcept {
    // The owning slab outlives the shared cache and releases the allocation.
}

ReadThroughCacheIoResult CachedScratchBacking::read_inner(
        void* context, ReadThroughCacheKey key, ScratchBuffer destination) noexcept {
    auto& inner = *static_cast<ScratchBacking*>(context);
    const auto status = inner.read_page(key.page_number, destination);
    return {status, status == ScratchStatus::ok ? destination.size : 0, false,
            status == ScratchStatus::ok ? nullptr : inner.carried_failure()};
}

ReadThroughCacheIoResult CachedScratchBacking::write_inner(
        void* context, ReadThroughCacheKey key, ScratchBytes source) noexcept {
    auto& inner = *static_cast<ScratchBacking*>(context);
    const auto status = inner.write_page(key.page_number, source);
    // A failed write may have changed a prefix or extended the backing.
    // ScratchBacking exposes no unchanged-on-failure guarantee.
    return {status, status == ScratchStatus::ok ? source.size : 0, false,
            status == ScratchStatus::ok ? nullptr : inner.carried_failure()};
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
    if (id >= inner_->page_count()) return inner_->read_page(id, destination);
    // This cache owns one immutable backing/generation. Zero identity fields
    // retain the original direct mapping exactly: page_number % slot_count.
    return cache_.get({0, 0, id}, destination);
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
    // Keep this local predicate before write()'s resident/spill choice. Only
    // the inner disk backing reserves shared scratch bytes; charging here
    // would count resident pages as disk and charge a spilled prefix twice.
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
    return cache_.put({0, 0, id}, source);
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
