#pragma once

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_tables.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pineforge::detail {

// Physical resident rows <= 2x the rows kept at the last spill pass + 64
// (+ pinned rows); this is not a bound in terms of currently live rows.
// Admission still limits non-spillable cohort rows in the bounded scratch
// profile. This does not bound the number or heap size of pending orders.
inline constexpr std::size_t kMaxResidentCohorts = 65536;

// The floor is the compatibility default. A sized run may raise admission
// through the separately named cap below.
inline constexpr std::size_t kMaxResidentCohortsCap = 1048576;

inline constexpr std::size_t resident_cohort_limit(
    std::size_t requested = kMaxResidentCohorts) noexcept {
    return requested < kMaxResidentCohorts
        ? kMaxResidentCohorts
        : requested > kMaxResidentCohortsCap ? kMaxResidentCohortsCap : requested;
}

struct ResidentCohortSizing {
    std::size_t limit = kMaxResidentCohorts;
    std::uint64_t need = 0;
    bool declared_over_cap = false;
};

constexpr std::uint64_t saturating_add(std::uint64_t left,
                                       std::uint64_t right) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

constexpr std::uint64_t saturating_multiply(std::uint64_t left,
                                            std::uint64_t right) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return std::numeric_limits<std::uint64_t>::max();
    return left * right;
}

// `declared` records that the caller supplied a deliberate demand estimate.
// The default-derived estimate is still useful for choosing the cap, but it
// must not reject a long run merely because bars * the default fresh rate is
// above that cap.
inline constexpr ResidentCohortSizing resident_cohort_sizing(
    std::uint64_t bars, std::uint64_t fresh_per_bar = 1,
    std::uint64_t literals = 0, std::uint64_t fixed_fresh = 0,
    bool declared = false) noexcept {
    const auto per_bar = saturating_multiply(bars, fresh_per_bar);
    const auto need = saturating_add(saturating_add(per_bar, literals), fixed_fresh);
    const auto cap = static_cast<std::uint64_t>(kMaxResidentCohortsCap);
    const auto request = need > cap ? kMaxResidentCohortsCap
                                    : static_cast<std::size_t>(need);
    return {resident_cohort_limit(request), need, declared && need > cap};
}

inline void require_resident_cohort_sizing(const ResidentCohortSizing& sizing) {
    if (!sizing.declared_over_cap) return;
    throw ScratchCapacityError(ScratchResource::cohort,
        "resident cohort limit " + scratch_limit_number(sizing.limit)
        + " rows is below declared need " + scratch_limit_number(sizing.need));
}

// P0 is a POLICY admission tier, not a measured bound or a proof that a
// strategy fits. Counts include historical keys and internally generated
// successors. The finite-batch replacement reserve deliberately scales with
// input bars; actual exhaustion must fail, never grow or fall back to disk.
inline ScratchManifest default_scratch_manifest(std::uint64_t bars) {
    if (bars > std::numeric_limits<std::uint64_t>::max() / 2)
        throw ScratchCapacityError(ScratchResource::layout,
                                   "default scratch replacement reserve overflow");
    return {
        0, 0, // K_q/B_q retained in the descriptor; the q store is removed.
        262144, 25165824,
        32768, 4227072,
        65536, 4194304,
        27, 6912,
        2 * bars,
    };
}

// A stream without a declared lifetime bar count has its own fixed policy
// tier. A zero-bar finite batch remains distinct and admits zero roots.
inline ScratchManifest default_scratch_manifest() {
    auto manifest = default_scratch_manifest(0);
    manifest.R = 65536;
    return manifest;
}

} // namespace pineforge::detail
