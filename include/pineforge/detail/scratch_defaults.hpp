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

// Compile-time hard cap and fixed fallback share the policy above. Future
// parameter sizing supplies requested here; no formula or setting yet.
inline constexpr std::size_t resident_cohort_limit(
    std::size_t requested = kMaxResidentCohorts) noexcept {
    return requested < kMaxResidentCohorts ? requested : kMaxResidentCohorts;
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
