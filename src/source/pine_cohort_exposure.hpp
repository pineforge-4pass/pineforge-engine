#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace pineforge::source {
class OriginRoster;

namespace detail {

constexpr std::size_t kCohortFoldStack = 128;

struct CohortExposureFold {
    double total;
    std::uint64_t steps;
    std::uint8_t path; // 0: empty, 1: roster walk, 2: sort, 3: overflow fallback.
};

// Allocation-free, acceptance-ordered fold. Diagnostic work is returned to
// the adapter wrapper so observations keep their adapter and run identity.
CohortExposureFold fold_cohort_exposure(
    const OriginRoster& origins,
    const std::unordered_map<std::uint64_t, double>& live_units_by_origin) noexcept;

#ifdef PINEFORGE_COHORT_EXPOSURE_FIXTURES
// Abort on any bit mismatch against the frozen position selector.
void verify_cohort_exposure(
    const OriginRoster& origins,
    const std::unordered_map<std::uint64_t, double>& live_units_by_origin,
    double actual) noexcept;
#endif

} // namespace detail
} // namespace pineforge::source
