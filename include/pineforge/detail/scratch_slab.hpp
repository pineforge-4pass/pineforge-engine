#pragma once

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_tables.hpp>
#include <pineforge/detail/run_usage.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <sys/types.h>

namespace pineforge::detail {

inline constexpr std::size_t kScratchRegionCount = 9;
inline constexpr std::uint64_t kScratchPageSize = 4096;

struct Region {
    std::uint8_t* base;
    std::uint64_t capacity;
    std::uint64_t logical_extent;
    ScratchResource resource;
};

struct RegionLayout {
    ScratchResource resource;
    std::uint64_t offset;
    std::uint64_t capacity; // Exact admitted bytes; page padding is not capacity.
    std::uint64_t span;
};

struct Layout {
    std::array<RegionLayout, kScratchRegionCount> regions{};
    std::uint64_t slab_size = kScratchPageSize;
};

namespace scratch_layout_detail {
inline std::uint64_t add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        throw ScratchCapacityError(ScratchResource::layout);
    return left + right;
}
inline std::uint64_t multiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        throw ScratchCapacityError(ScratchResource::layout);
    return left * right;
}
inline std::uint64_t slots(std::uint64_t keys) {
    if (keys == 0) return 0;
    const auto minimum = std::max(std::uint64_t{64}, multiply(2, keys));
    std::uint64_t result = 64;
    while (result < minimum) result = multiply(result, 2);
    return result;
}
inline std::uint64_t page(std::uint64_t bytes) {
    const auto pages = add(bytes / kScratchPageSize,
                          bytes % kScratchPageSize == 0 ? 0 : 1);
    return multiply(pages, kScratchPageSize);
}
template<class HostSize>
inline bool fits(std::uint64_t bytes) noexcept {
    if constexpr (std::numeric_limits<HostSize>::digits
                  < std::numeric_limits<std::uint64_t>::digits)
        return bytes <= static_cast<std::uint64_t>(std::numeric_limits<HostSize>::max());
    else
        return true;
}
} // namespace scratch_layout_detail

// No allocation or mutation. Every intermediate, page ceiling and prefix sum
// is checked before the caller can allocate or publish any run ownership.
inline Layout compute_layout(const ScratchManifest& manifest) {
    using namespace scratch_layout_detail;
    if (manifest.V > 27) throw ScratchCapacityError(ScratchResource::layout, 27, manifest.V);
    if (manifest.K_q != 0 || manifest.B_q != 0) throw ScratchCapacityError(ScratchResource::layout);
    const std::array<std::uint64_t, kScratchRegionCount> capacities{{
        add(multiply(40, manifest.K_q), manifest.B_q),
        multiply(16, slots(manifest.K_q)),
        add(multiply(40, manifest.K_e), manifest.B_e),
        multiply(16, slots(manifest.K_e)),
        add(multiply(40, manifest.K_c), manifest.B_c),
        multiply(16, slots(manifest.K_c)),
        manifest.U == 0 ? 0 : add(add(1, multiply(96, manifest.U)), manifest.B_U),
        manifest.V == 0 ? 0 : add(add(1, multiply(60, manifest.V)), manifest.B_V),
        multiply(16, manifest.R),
    }};
    Layout layout;
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < capacities.size(); ++i) {
        const auto span = page(capacities[i]);
        layout.regions[i] = {static_cast<ScratchResource>(i + 1), offset,
                             capacities[i], span};
        offset = add(offset, span);
    }
    layout.slab_size = std::max(kScratchPageSize, offset);
    if (!fits<std::size_t>(layout.slab_size) || !fits<off_t>(layout.slab_size))
        throw ScratchCapacityError(ScratchResource::layout,
            std::min<std::uint64_t>(std::numeric_limits<std::size_t>::max(),
                                    std::numeric_limits<off_t>::max()),
            layout.slab_size);
    return layout;
}

// One 4096-aligned backing allocation, never grown, moved or replaced by an
// operation on this object. Region metadata is outside that allocation.
// Binders retain Region identities, not byte views: base is directory metadata
// only. Callers must not dereference/cache base or retain a pointer into the
// slab; all payload access copies through read_at/write_at. This also permits
// a future mapped-window byte store without exposing window lifetimes.
class ScratchSlab {
public:
    explicit ScratchSlab(const ScratchManifest& manifest)
        : manifest_(manifest), layout_(compute_layout(manifest)),
          allocation_(static_cast<std::uint8_t*>(::operator new(
              static_cast<std::size_t>(layout_.slab_size),
              std::align_val_t{static_cast<std::size_t>(kScratchPageSize)}))) {
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            const auto& item = layout_.regions[i];
            regions_[i] = {allocation_ + static_cast<std::size_t>(item.offset),
                           item.capacity, 0, item.resource};
        }
        zero();
    }
    ~ScratchSlab() {
        for (std::size_t i = 0; i < regions_.size(); ++i) deactivate(i);
        ::operator delete(allocation_,
            std::align_val_t{static_cast<std::size_t>(kScratchPageSize)});
    }
    ScratchSlab(const ScratchSlab&) = delete;
    ScratchSlab& operator=(const ScratchSlab&) = delete;
    ScratchSlab(ScratchSlab&&) = delete;
    ScratchSlab& operator=(ScratchSlab&&) = delete;

    const ScratchManifest& manifest() const noexcept { return manifest_; }
    const Layout& layout() const noexcept { return layout_; }
    const std::array<Region, kScratchRegionCount>& regions() const noexcept { return regions_; }
    const Region& region(ScratchResource resource) const { return regions_[index(resource)]; }

    // Activity belongs to the shared logical region, not each borrowing
    // ScratchFile. Construction alone does not count unused slab resources.
    void activate(ScratchResource resource) {
        const auto i = index(resource);
        if (!usage_active_[i]) {
            usage_active_[i] = true;
            run_usage_scratch_open(regions_[i].logical_extent);
        }
    }

    // Read-only reservation: neither bytes nor logical_extent change, even
    // on success. Backend row/key tickets must precede their own first mutation.
    bool can_extend(ScratchResource resource, std::uint64_t new_extent) const noexcept {
        const auto word = static_cast<std::uint32_t>(resource);
        return word >= 1 && word <= kScratchRegionCount
            && new_extent <= regions_[word - 1].capacity;
    }
    bool can_extend(const Region& item, std::uint64_t new_extent) const {
        validate(item);
        return can_extend(item.resource, new_extent);
    }
    void require_extend(ScratchResource resource, std::uint64_t new_extent) const {
        if (!can_extend(resource, new_extent)) {
            const auto word = static_cast<std::uint32_t>(resource);
            if (word < 1 || word > kScratchRegionCount) throw ScratchCapacityError(resource);
            throw ScratchCapacityError(resource, regions_[word - 1].capacity, new_extent);
        }
    }
    void require_extend(const Region& item, std::uint64_t new_extent) const {
        validate(item);
        require_extend(item.resource, new_extent);
    }

    // Publish a zero-initialized readable prefix (e.g. the entire fixed hash
    // index at bind). Extents are monotonic until reset; this never resizes.
    void extend(ScratchResource resource, std::uint64_t new_extent) {
        require_extend(resource, new_extent);
        auto& item = regions_[index(resource)];
        activate(resource);
        run_usage_scratch_resize(item.logical_extent, std::max(item.logical_extent, new_extent));
        item.logical_extent = std::max(item.logical_extent, new_extent);
    }

    // Reads require [offset,offset+len) inside BOTH capacity and logical_extent.
    // Writes require capacity and publish max(old extent,offset+len) only after
    // the copy; zeroed holes are permitted. A zero-length operation checks its
    // offset but neither dereferences its buffer nor extends the prefix.
    void read_at(ScratchResource resource, std::uint64_t offset,
                 void* destination, std::size_t length) const {
        const auto& item = region(resource);
        if (offset > item.capacity || length > item.capacity - offset
            || offset > item.logical_extent || length > item.logical_extent - offset)
            throw AuxiliaryScratchIoError(EIO, "scratch read outside logical extent");
        if (length == 0) return;
        if (!destination) throw AuxiliaryScratchIoError(EINVAL, "scratch read null destination");
        std::memcpy(destination, item.base + static_cast<std::size_t>(offset), length);
    }
    void read_at(const Region& item, std::uint64_t offset,
                 void* destination, std::size_t length) const {
        validate(item);
        read_at(item.resource, offset, destination, length);
    }
    void write_at(ScratchResource resource, std::uint64_t offset,
                  const void* source, std::size_t length) {
        auto& item = regions_[index(resource)];
        if (offset > item.capacity || length > item.capacity - offset) {
            if (length > std::numeric_limits<std::uint64_t>::max() - offset)
                throw ScratchCapacityError(resource);
            throw ScratchCapacityError(resource, item.capacity,
                                       offset + static_cast<std::uint64_t>(length));
        }
        if (length == 0) return;
        if (!source) throw AuxiliaryScratchIoError(EINVAL, "scratch write null source");
        std::memcpy(item.base + static_cast<std::size_t>(offset), source, length);
        activate(resource);
        run_usage_scratch_resize(item.logical_extent,
            std::max(item.logical_extent, offset + static_cast<std::uint64_t>(length)));
        item.logical_extent = std::max(item.logical_extent,
                                      offset + static_cast<std::uint64_t>(length));
    }
    void write_at(const Region& item, std::uint64_t offset,
                  const void* source, std::size_t length) {
        validate(item);
        write_at(item.resource, offset, source, length);
    }

    // zero preserves readable extents; reset reinitializes bytes AND extents.
    // Per-region reset supports clearing/refolding one store in the same run.
    void zero() noexcept {
        std::memset(allocation_, 0, static_cast<std::size_t>(layout_.slab_size));
    }
    void zero(ScratchResource resource) {
        auto& item = regions_[index(resource)];
        if (item.capacity != 0)
            std::memset(item.base, 0, static_cast<std::size_t>(item.capacity));
    }
    void reset() noexcept {
        zero();
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            deactivate(i);
            regions_[i].logical_extent = 0;
        }
    }
    void reset(ScratchResource resource) {
        zero(resource);
        const auto i = index(resource);
        deactivate(i);
        regions_[i].logical_extent = 0;
    }

private:
    void deactivate(std::size_t i) noexcept {
        if (usage_active_[i]) {
            run_usage_scratch_close(regions_[i].logical_extent);
            usage_active_[i] = false;
        }
    }
    static std::size_t index(ScratchResource resource) {
        const auto word = static_cast<std::uint32_t>(resource);
        if (word < 1 || word > kScratchRegionCount)
            throw ScratchCapacityError(ScratchResource::layout);
        return static_cast<std::size_t>(word - 1);
    }
    void validate(const Region& item) const {
        if (&item != &regions_[index(item.resource)])
            throw ScratchCapacityError(ScratchResource::layout,
                                       "scratch region belongs to another slab");
    }
    const ScratchManifest manifest_;
    const Layout layout_;
    std::uint8_t* const allocation_;
    std::array<Region, kScratchRegionCount> regions_{};
    // The slab is shared heap state; none of these flags is embedded in a
    // ScratchFile, IdPairScratchFile, or a fixed fold segment.
    std::array<bool, kScratchRegionCount> usage_active_{};
};

} // namespace pineforge::detail
