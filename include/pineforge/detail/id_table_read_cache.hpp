#pragma once

#include <pineforge/detail/cache_capacity.hpp>
#include <pineforge/detail/read_through_cache.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace pineforge::detail {

// Copy-in/copy-out logical pages for the private disk id tables only.
//
// WRITE-THROUGH: this class never writes or delays a write. Owners invalidate
// before the real write, including a write that might fail after a prefix.
// Clear, move and backend/identity changes invalidate ALL lines. Growth
// invalidates the old tail page onward in that file; in-place writes
// invalidate every overlapping line, even when the write fails.
// EOF: out-of-extent reads go straight to the original reader, preserving its
// short-read/EIO/errno text and copied prefix. A fill stops at logical_end;
// no zero padding and no bytes beyond that end are read or returned.
// BYPASS: plain_disk must be false for EVERY slab/arena backend, including
// unjailed regular-file arenas and inherited/sealed memfds. No allocation,
// tag probe or copied page occurs on that path. No payload pointer escapes.
//
// Policy fixes line bytes and line count, independent of the id population.
// The legacy byte constructor keeps its 4 KiB geometry and 64 KiB ceiling.
// Default capacity is zero, with no allocation. Only the adapter opts its
// four tables in: 64/64/32/32 KiB (pair data+index together), 192 KiB of page
// bytes per adapter. ReadThroughCache owns every tag and payload line in one
// optional allocation: lines * ReadThroughCache::line_bytes(line_bytes).
// A 4 KiB stack transfer buffer is separate; wider policies lazily allocate
// one optional line-sized transfer buffer. Either allocation may bypass.
// One adapter owns each cache; no sharing across threads or per-id directory.
// This is disposable physical state, not logical/durable broker state.
class IdTableReadCache {
public:
    static constexpr std::size_t kPageBytes = 4096;
    static constexpr std::size_t kCacheBytes = 64 * 1024;
    static constexpr std::size_t kSlots = kCacheBytes / kPageBytes;
    static constexpr std::size_t kPairCacheBytes = kCacheBytes / 2;
    static constexpr std::size_t kAdapterCacheBytes = 3 * kCacheBytes;

    struct Policy {
        std::size_t line_bytes = kPageBytes;
        std::size_t lines = 0; // Zero bypasses; neither field limits backing data.
    };

    static Policy policy_for_capacity(CacheCapacity capacity) noexcept {
        const auto representable = static_cast<std::size_t>(
            std::numeric_limits<std::ptrdiff_t>::max())
            / ReadThroughCache::line_bytes(kPageBytes);
        return Policy{kPageBytes, std::min(capacity.lines(kPageBytes), representable)};
    }

    explicit IdTableReadCache(std::size_t bytes = 0) { set_capacity(bytes); }
    explicit IdTableReadCache(Policy policy) { set_capacity(policy); }
    explicit IdTableReadCache(CacheCapacity capacity)
        : IdTableReadCache(policy_for_capacity(capacity)) {}
    void set_cache_capacity(CacheCapacity capacity) {
        set_capacity(policy_for_capacity(capacity));
    }
    IdTableReadCache(const IdTableReadCache&) = delete;
    IdTableReadCache& operator=(const IdTableReadCache&) = delete;
    IdTableReadCache(IdTableReadCache&& other) noexcept { *this = std::move(other); }
    IdTableReadCache& operator=(IdTableReadCache&& other) noexcept {
        if (this != &other) {
            release();
            line_bytes_ = other.line_bytes_;
            slots_ = other.slots_;
            capacity_bytes_ = other.capacity_bytes_;
            // The shared cache is nonmovable and its callbacks borrow this
            // owner. Both instances start fresh after an ownership change.
            other.release();
        }
        return *this;
    }
    void set_capacity(std::size_t bytes) {
        if (bytes > kCacheBytes || bytes % kPageBytes != 0)
            throw std::invalid_argument("id read cache capacity must be page-aligned and at most 64 KiB");
        set_capacity(Policy{kPageBytes, bytes / kPageBytes});
    }
    void set_capacity(Policy policy) {
        constexpr auto maximum = static_cast<std::size_t>(
            std::numeric_limits<std::ptrdiff_t>::max());
        if (policy.line_bytes == 0)
            throw std::invalid_argument("id read cache line size must be positive");
        const auto inclusive_line = ReadThroughCache::line_bytes(policy.line_bytes);
        if (policy.line_bytes > maximum || !inclusive_line
            || policy.lines > maximum / inclusive_line)
            throw std::invalid_argument("id read cache geometry is not representable");
        release();
        line_bytes_ = policy.line_bytes;
        slots_ = policy.lines;
        capacity_bytes_ = slots_ * inclusive_line;
    }
    void invalidate() const noexcept {
        if (cache_) (void)cache_->invalidate_all();
    }
    void invalidate_from(unsigned file, std::uint64_t offset) const noexcept {
        if (!cache_) return;
        const auto page = offset / line_bytes_;
        const auto shift = file_shift(file);
        if (page > kMaximum - shift || page + shift > kMaximum / line_bytes_) {
            invalidate();
            return;
        }
        const auto key = page_key(file, page);
        (void)cache_->invalidate_from(key.backing_identity, key.generation,
                                      key.page_number * line_bytes_);
    }
    void release() noexcept {
        cache_.reset();
        prepared_.reset();
        wide_workspace_.reset();
    }

    void before_write(unsigned file, std::uint64_t logical_end,
                      std::uint64_t offset, std::size_t length) const noexcept {
        if (!cache_ || length == 0) return;
        if (offset > logical_end || length > logical_end - offset) {
            // Include any overwritten prefix when a write straddles the end.
            // Pure appends retain all complete pages before the old tail.
            invalidate_from(file, std::min(offset, logical_end));
            return;
        }
        const auto first = offset / line_bytes_;
        const auto last = (offset + length - 1) / line_bytes_;
        // The public cache port has key and tail invalidation. Bound the key
        // walk by configured slots; a wider write conservatively drops the
        // same file's following pages too, without visiting backing records.
        if (last - first >= slots_) {
            invalidate_from(file, offset);
            return;
        }
        for (auto page = first;; ++page) {
            (void)cache_->invalidate(page_key(file, page));
            if (page == last) break;
        }
    }

    template<class File>
    void read(const File& source, bool plain_disk, std::uint64_t logical_end,
              unsigned file, std::uint64_t offset, void* destination, std::size_t length) const {
        if (!plain_disk || slots_ == 0 || length == 0
            || offset >= logical_end || length > logical_end - offset) {
            source.read(offset, destination, length);
            return;
        }
        // A translated key must remain ordered for tail invalidation.
        if ((offset + length - 1) / line_bytes_ > kMaximum - file_shift(file)
            || !prepare()) {
            source.read(offset, destination, length);
            return;
        }
        context_.source = &source;
        context_.read = [](const void* owner, std::uint64_t at, void* bytes, std::size_t size) {
            static_cast<const File*>(owner)->read(at, bytes, size);
        };
        struct ActiveRead {
            ReadContext& context;
            ~ActiveRead() {
                context.source = nullptr;
                context.read = nullptr;
                context.failure = nullptr;
            }
        } active{context_};
        if (line_bytes_ > kPageBytes) {
            read_cached(logical_end, file, offset, destination, length, wide_workspace_.get());
        } else {
            std::array<std::byte, kPageBytes> workspace;
            read_cached(logical_end, file, offset, destination, length, workspace.data());
        }
    }

private:
    static constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint64_t kIdentityMix = 0x9e3779b97f4a7c15ULL;
    static constexpr std::uint64_t kGenerationMix = 0xbf58476d1ce4e5b9ULL;
    static std::uint64_t file_shift(unsigned file) noexcept {
        return static_cast<std::uint64_t>(file) * 7;
    }
    static ReadThroughCacheKey page_key(unsigned file, std::uint64_t page) noexcept {
        // The shared direct map XORs identity*I and generation*G. Exchanging
        // I/G in these two salts cancels those products, leaving the legacy
        // (page + file*7) placement while the full keys still distinguish files.
        return {file * kGenerationMix, file * kIdentityMix, page + file_shift(file)};
    }
    struct StorageDeleter {
        void operator()(void* bytes) const noexcept { ::operator delete(bytes); }
    };
    using Storage = std::unique_ptr<void, StorageDeleter>;
    struct ReadContext {
        const void* source = nullptr;
        void (*read)(const void*, std::uint64_t, void*, std::size_t) = nullptr;
        std::uint64_t offset = 0;
        std::exception_ptr failure;
    };
    static ReadThroughCacheIoResult backing_read(void* owner, ReadThroughCacheKey,
                                                ScratchBuffer destination) noexcept {
        auto& context = *static_cast<ReadContext*>(owner);
        try {
            context.read(context.source, context.offset, destination.data, destination.size);
            return {ScratchStatus::ok, destination.size, false, nullptr};
        } catch (...) {
            context.failure = std::current_exception();
            // The throwing reader exposes no completed-byte count. Preserve
            // its exact exception rather than inventing a partial-I/O count.
            return {ScratchStatus::io_error, 0, false, nullptr};
        }
    }
    bool prepare() const noexcept {
        if (cache_) return true;
        // Acquire the shared block before get(), so an allocation failure can
        // use the ORIGINAL read span (not a speculative page fill). A later
        // logical read retries; the shared instance itself never sees ENOMEM.
        prepared_.reset(::operator new(capacity_bytes_, std::nothrow));
        if (!prepared_) return false;
        if (line_bytes_ > kPageBytes) {
            wide_workspace_.reset(new (std::nothrow) std::byte[line_bytes_]);
            if (!wide_workspace_) {
                prepared_.reset();
                return false;
            }
        }
        const ReadThroughCachePolicy policy{
            capacity_bytes_, line_bytes_, ReadThroughCacheReplacement::direct_mapped,
            ReadThroughCacheWritePolicy::write_through};
        const ReadThroughCacheBacking backing{&context_, &backing_read, nullptr};
        const ReadThroughCacheAllocator allocator{
            &prepared_,
            [](void* storage, std::size_t) noexcept -> void* {
                return static_cast<Storage*>(storage)->release();
            },
            [](void*, void* bytes, std::size_t) noexcept { ::operator delete(bytes); }
        };
        cache_.emplace(policy, backing, allocator);
        return true;
    }
    void read_cached(std::uint64_t logical_end, unsigned file, std::uint64_t offset,
                     void* destination, std::size_t length, std::byte* workspace) const {
        auto* output = static_cast<unsigned char*>(destination);
        while (length != 0) {
            const auto page = offset / line_bytes_;
            const auto base = page * line_bytes_;
            const auto within = static_cast<std::size_t>(offset - base);
            const auto valid = static_cast<std::size_t>(
                std::min<std::uint64_t>(line_bytes_, logical_end - base));
            context_.offset = base;
            const auto status = cache_->get(page_key(file, page), {workspace, valid});
            if (status != ScratchStatus::ok) {
                if (context_.failure) std::rethrow_exception(context_.failure);
                throw std::logic_error("id read cache callback contract failed");
            }
            const auto count = std::min(length, valid - within);
            std::memcpy(output, workspace + within, count);
            output += count;
            offset += count;
            length -= count;
        }
    }
    std::size_t line_bytes_ = kPageBytes;
    std::size_t slots_ = 0;
    std::size_t capacity_bytes_ = 0;
    mutable Storage prepared_;
    mutable std::unique_ptr<std::byte[]> wide_workspace_;
    mutable ReadContext context_;
    mutable std::optional<ReadThroughCache> cache_;
};
} // namespace pineforge::detail
