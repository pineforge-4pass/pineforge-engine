#pragma once

#include <pineforge/detail/scratch_errors.hpp>

#include <cstdint>

namespace pineforge {
namespace detail {

enum class ScratchTableBackend : std::uint32_t {
    disk_scratch = 0,
    bounded_memory = 1,
};

// Admission maxima, not observed populations. K/B include erased keys and
// generated identifiers; all B fields count bytes, including embedded NULs.
struct ScratchManifest {
    // Reserved zero: q is removed; retain these fields in their ABI order.
    std::uint64_t K_q = 0;
    std::uint64_t B_q = 0;
    std::uint64_t K_e = 0;
    std::uint64_t B_e = 0;
    std::uint64_t K_c = 0;
    std::uint64_t B_c = 0;
    std::uint64_t U = 0;
    std::uint64_t B_U = 0;
    std::uint64_t V = 0;
    std::uint64_t B_V = 0;
    std::uint64_t R = 0;
};

struct ScratchTableConfig {
    ScratchTableBackend backend = ScratchTableBackend::disk_scratch;
    ScratchManifest manifest{};
};

// v2 admission is independent of physical storage. An inactive bit means no
// logical bound; an active bit with maximum zero is a finite zero. These bits
// mirror PF_SCRATCH_BOUND_* without including the public strategy C header.
enum class ScratchBound : std::uint64_t {
    K_q = UINT64_C(1) << 0, B_q = UINT64_C(1) << 1,
    K_e = UINT64_C(1) << 2, B_e = UINT64_C(1) << 3,
    K_c = UINT64_C(1) << 4, B_c = UINT64_C(1) << 5,
    U = UINT64_C(1) << 6, B_U = UINT64_C(1) << 7,
    V = UINT64_C(1) << 8, B_V = UINT64_C(1) << 9,
    R = UINT64_C(1) << 10,
};

struct ScratchBounds {
    std::uint64_t active_fields = 0;
    ScratchManifest manifest{};

    bool active(ScratchBound field) const noexcept {
        return (active_fields & static_cast<std::uint64_t>(field)) != 0;
    }
    std::uint64_t maximum(ScratchBound field) const noexcept {
        switch (field) {
        case ScratchBound::K_q: return manifest.K_q;
        case ScratchBound::B_q: return manifest.B_q;
        case ScratchBound::K_e: return manifest.K_e;
        case ScratchBound::B_e: return manifest.B_e;
        case ScratchBound::K_c: return manifest.K_c;
        case ScratchBound::B_c: return manifest.B_c;
        case ScratchBound::U: return manifest.U;
        case ScratchBound::B_U: return manifest.B_U;
        case ScratchBound::V: return manifest.V;
        case ScratchBound::B_V: return manifest.B_V;
        case ScratchBound::R: return manifest.R;
        }
        return 0;
    }
    void require_growth(ScratchBound field, std::uint64_t current, std::uint64_t extra,
                        ScratchResource resource, bool bytes = false) const {
        if (!active(field)) return;
        const auto limit = maximum(field);
        if (current > limit || extra > limit - current)
            throw ScratchCapacityError(resource, bytes
                ? scratch_byte_limit_message(resource, limit)
                : scratch_count_limit_message(resource, limit));
    }
};

// v2 root admission exposes exact uint64 values to native callers. The
// existing closed C error schema carries resource; its diagnostic also names
// limit/need. Adding structured C arguments requires the registry owner.
class ScratchRootCapacityError final : public std::runtime_error, public RunFailureInfo {
public:
    ScratchRootCapacityError(std::uint64_t maximum, std::uint64_t requested)
        : std::runtime_error("successor roots: limit " + scratch_limit_number(maximum)
                             + " rows; need " + scratch_limit_number(requested)),
          RunFailureInfo(RunFailureCode::scratch_capacity_exceeded,
                         {{"resource", "successor_roots"}}),
          limit_(maximum), need_(requested) {}
    ScratchResource resource() const noexcept { return ScratchResource::successor_roots; }
    std::uint64_t limit() const noexcept { return limit_; }
    std::uint64_t need() const noexcept { return need_; }
private:
    std::uint64_t limit_;
    std::uint64_t need_;
};

class ScratchSlab;

} // namespace detail
} // namespace pineforge
