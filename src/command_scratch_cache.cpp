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
        std::size_t pages, ScratchIoObserver observer, Admission admission, bool write_through)
    : ScratchBacking(inner ? inner->kind() : ScratchBackingKind::disk,
                     inner ? inner->page_size() : 0, observer),
      inner_(std::move(inner)), slot_count_(pages), admission_(admission),
      write_through_(write_through) {
    if (!inner_)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "command scratch cache requires a backing");
    if (pages == 0 || pages > max_entries(page_size()))
        throw coded<std::invalid_argument>(RunFailureCode::scratch_capacity_exceeded,
            {{"resource", "layout"}}, "command scratch resident page budget is out of range");
    if (!write_through_ && !admission_)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "command scratch write-behind requires extent admission");
    allocation_bytes_ = resident_bytes(pages, page_size()) - sizeof(*this);
    storage_.reset(new (std::nothrow) std::byte[allocation_bytes_]);
    if (!storage_)
        throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
            std::system_error(ENOMEM, std::generic_category(), "command scratch cache allocate"));
    // byte-array new supplies alignment for objects placed within the array.
    // Slot is trivially destructible; its lifetime begins before first access.
    for (std::size_t i = 0; i < pages; ++i)
        ::new (static_cast<void*>(storage_.get() + i * sizeof(Slot))) Slot{};
    bytes_ = storage_.get() + pages * sizeof(Slot);
}

CachedScratchBacking::~CachedScratchBacking() noexcept {
    // A caller needing an error verdict uses drain() before releasing ownership.
    // Keep destructor nonthrowing, as required by ScratchBacking. The owned
    // production file is unlinked; no independent reader survives its close.
    (void)flush_dirty();
}

std::uint64_t CachedScratchBacking::page_count() const noexcept {
    return inner_->page_count();
}

std::size_t CachedScratchBacking::slot(ScratchPageId id) const noexcept {
    return static_cast<std::size_t>(id % slot_count_);
}

CachedScratchBacking::Slot& CachedScratchBacking::entry(std::size_t index) const noexcept {
    // The slots were individually placement-constructed, not new Slot[]:
    // compute offsets within the byte storage, then obtain the live object.
    return *std::launder(reinterpret_cast<Slot*>(storage_.get() + index * sizeof(Slot)));
}

ScratchStatus CachedScratchBacking::flush_slot(std::size_t index) const noexcept {
    auto& cached = entry(index);
    if (!cached.dirty) return ScratchStatus::ok;
    const auto status = inner_->write_page(cached.id,
        {bytes_ + index * page_size(), page_size()});
    // A failed or torn write retains BOTH the full resident bytes and id.
    // Do not replace the victim or clear dirty: undo/retry may still need it.
    if (status == ScratchStatus::ok) cached.dirty = false;
    return status;
}

ScratchStatus CachedScratchBacking::flush_dirty() const noexcept {
    for (std::size_t i = 0; i < slot_count_; ++i) {
        const auto status = flush_slot(i);
        if (status != ScratchStatus::ok) return status;
    }
    return ScratchStatus::ok;
}

void CachedScratchBacking::remember(std::size_t index, ScratchPageId id,
        const std::byte* bytes, bool dirty) const noexcept {
    std::memmove(bytes_ + index * page_size(), bytes, page_size());
    entry(index) = {id, dirty};
}

ScratchStatus CachedScratchBacking::do_read_page(
        ScratchPageId id, ScratchBuffer destination) const noexcept {
    // Admission keeps the REAL extent immediate, even for resident writes.
    // Do not evict/flush anything for an invalid out-of-extent read.
    if (id >= inner_->page_count()) return result(inner_->read_page(id, destination));
    const auto index = slot(id);
    if (entry(index).id == id) {
        std::memcpy(destination.data, bytes_ + index * page_size(), page_size());
        return result(ScratchStatus::ok);
    }
    auto status = flush_slot(index);
    if (status != ScratchStatus::ok) return result(status);
    status = inner_->read_page(id, destination);
    if (status == ScratchStatus::ok) remember(index, id, destination.data, false);
    return result(status);
}

ScratchStatus CachedScratchBacking::do_write_page(
        ScratchPageId id, ScratchBytes source) noexcept {
    const auto index = slot(id);
    if (write_through_) {
        // Preserve the original fault/short-write semantics in the oracle.
        entry(index) = {};
        const auto status = inner_->write_page(id, source);
        if (status == ScratchStatus::ok) remember(index, id, source.data, false);
        return result(status);
    }
    auto status = admission_(*inner_, id);
    if (status != ScratchStatus::ok) return result(status);
    if (entry(index).id != id) {
        status = flush_slot(index);
        if (status != ScratchStatus::ok) return result(status);
    }
    // This includes erase/reuse: superseded resident payloads are never sent
    // to the file. The latest free-list/journal metadata is still retained.
    remember(index, id, source.data, true);
    return result(ScratchStatus::ok);
}

ScratchStatus CachedScratchBacking::do_sync(ScratchDurability durability) noexcept {
    const auto status = flush_dirty();
    if (status != ScratchStatus::ok) return result(status);
    return result(inner_->sync(durability));
}

} // namespace pineforge::detail
