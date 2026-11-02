#pragma once

#include <pineforge/detail/read_through_cache.hpp>

#include <cstddef>
#include <limits>

namespace pineforge {

// An inclusive allowance for optional cache lines. Geometry, fixed control
// state and mandatory transfer/transaction workspace belong to the store.
struct CacheCapacity {
    std::size_t capacity_bytes = 0;

    // Compatibility for constructors expressed in payload bytes. Reserve the
    // shared line charge once, preserving every complete legacy payload page.
    static CacheCapacity from_payload(std::size_t page_size,
                                      std::size_t payload_bytes) noexcept {
        const auto charged = detail::ReadThroughCache::line_bytes(page_size);
        const auto count = page_size ? payload_bytes / page_size : 0;
        return charged && count <= std::numeric_limits<std::size_t>::max() / charged
            ? CacheCapacity{count * charged} : CacheCapacity{};
    }

    detail::ReadThroughCachePolicy for_page(std::size_t page_size) const noexcept {
        return {capacity_bytes, page_size};
    }

    std::size_t lines(std::size_t page_size) const noexcept {
        const auto charged = detail::ReadThroughCache::line_bytes(page_size);
        return charged == 0 ? 0 : capacity_bytes / charged;
    }
    // Bridge for adapters whose constructor still accepts payload bytes. The
    // common line charge has already been reserved, so payload never exceeds
    // the inclusive host allowance. Common-cache adapters use for_page().
    std::size_t payload_bytes(std::size_t page_size) const noexcept {
        return lines(page_size) * page_size;
    }
};

} // namespace pineforge
