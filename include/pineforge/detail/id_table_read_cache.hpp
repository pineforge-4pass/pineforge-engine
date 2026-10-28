#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
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
// Fixed budget: at most 16 x 4 KiB = 64 KiB of lazy page bytes per table,
// plus 16 fixed tags. Default capacity is zero, with no allocation. Only the
// adapter opts its four tables in: 64/64/32/32 KiB (pair data+index together).
// Total page bytes are 192 KiB per adapter, not per engine, regardless of
// bars, ids or trades. One adapter owns each cache; no sharing across threads.
// No growing directory.
// This is disposable physical state, not logical/durable broker state.
class IdTableReadCache {
public:
    static constexpr std::size_t kPageBytes = 4096;
    static constexpr std::size_t kCacheBytes = 64 * 1024;
    static constexpr std::size_t kSlots = kCacheBytes / kPageBytes;
    static constexpr std::size_t kPairCacheBytes = kCacheBytes / 2;
    static constexpr std::size_t kAdapterCacheBytes = 3 * kCacheBytes;

    explicit IdTableReadCache(std::size_t bytes = 0) { set_capacity(bytes); }
    IdTableReadCache(const IdTableReadCache&) = delete;
    IdTableReadCache& operator=(const IdTableReadCache&) = delete;
    IdTableReadCache(IdTableReadCache&& other) noexcept { *this = std::move(other); }
    IdTableReadCache& operator=(IdTableReadCache&& other) noexcept {
        if (this != &other) {
            release();
            bytes_ = std::move(other.bytes_);
            slots_ = other.slots_;
            other.invalidate();
            // Never transfer a valid line across an ownership change.
        }
        return *this;
    }
    void set_capacity(std::size_t bytes) {
        if (bytes > kCacheBytes || bytes % kPageBytes != 0)
            throw std::invalid_argument("id read cache capacity must be page-aligned and at most 64 KiB");
        release();
        slots_ = bytes / kPageBytes;
    }
    void invalidate() const noexcept {
        for (auto& tag : tags_) tag.page = kInvalid;
    }
    void invalidate_from(unsigned file, std::uint64_t offset) const noexcept {
        const auto first = offset / kPageBytes;
        for (auto& tag : tags_)
            if (tag.file == file && tag.page >= first) tag.page = kInvalid;
    }
    void release() noexcept { invalidate(); bytes_.reset(); }

    void before_write(unsigned file, std::uint64_t logical_end,
                      std::uint64_t offset, std::size_t length) const noexcept {
        if (slots_ == 0 || length == 0) return;
        if (offset > logical_end || length > logical_end - offset) {
            // Include any overwritten prefix when a write straddles the end.
            // Pure appends retain all complete pages before the old tail.
            invalidate_from(file, std::min(offset, logical_end));
            return;
        }
        const auto first = offset / kPageBytes;
        const auto last = (offset + length - 1) / kPageBytes;
        for (auto& tag : tags_)
            if (tag.file == file && tag.page >= first && tag.page <= last) tag.page = kInvalid;
    }

    template<class File>
    void read(const File& source, bool plain_disk, std::uint64_t logical_end,
              unsigned file, std::uint64_t offset, void* destination, std::size_t length) const {
        if (!plain_disk || slots_ == 0 || length == 0
            || offset >= logical_end || length > logical_end - offset) {
            source.read(offset, destination, length);
            return;
        }
        if (!bytes_) {
            bytes_.reset(new (std::nothrow) unsigned char[slots_ * kPageBytes]);
            if (!bytes_) {
                source.read(offset, destination, length);
                return;
            }
        }
        auto* output = static_cast<unsigned char*>(destination);
        while (length != 0) {
            const auto page = offset / kPageBytes;
            const auto base = page * kPageBytes;
            const auto within = static_cast<std::size_t>(offset - base);
            const auto valid = static_cast<std::size_t>(
                std::min<std::uint64_t>(kPageBytes, logical_end - base));
            // Deterministic direct mapping; data/index have separate tags
            // but share this one fixed allocation.
            const auto slot = static_cast<std::size_t>((page + file * 7ULL) % slots_);
            auto& tag = tags_[slot];
            auto* line = bytes_.get() + slot * kPageBytes;
            if (tag.page != page || tag.file != file) {
                tag.page = kInvalid; // A failed/partial fill publishes nothing.
                source.read(base, line, valid);
                tag.file = file;
                tag.page = page;
            }
            const auto count = std::min(length, valid - within);
            std::memcpy(output, line + within, count);
            output += count;
            offset += count;
            length -= count;
        }
    }

private:
    static constexpr auto kInvalid = std::numeric_limits<std::uint64_t>::max();
    struct Tag {
        std::uint64_t page = kInvalid;
        unsigned file = 0;
    };
    mutable std::array<Tag, kSlots> tags_{};
    mutable std::unique_ptr<unsigned char[]> bytes_;
    std::size_t slots_ = 0;
};
} // namespace pineforge::detail
