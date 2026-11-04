#pragma once

#include "pine_host_reads.hpp"

#include <array>
#include <cstddef>

namespace pineforge::source::detail {

// Pure reader: no unordered-map operator[], only size() and iteration over
// values into std::array counters. No allocation, no mutation, nothing hashed.
// The three slots are ExitLimit, ExitStop and ExitTrail, in that order.
struct LookupIndexCounts {
    bool present = false;
    std::size_t cohort_sides = 0;
    std::size_t unbound = 0;
    std::array<std::size_t, 3> legs_by_origin_keys{};
    std::array<std::size_t, 3> legs_by_origin_ids{};
    std::size_t immediate_closes_keys = 0;
    std::size_t immediate_closes_ids = 0;
};

LookupIndexCounts probe_lookup_index(const NativeHostCache* cache) noexcept;

} // namespace pineforge::source::detail
