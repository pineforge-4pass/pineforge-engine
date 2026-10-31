#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr std::size_t kCachePolicyModuleValues = 2053;

struct CachePolicyModuleObservation {
    std::size_t pages = 0;
    // Owner totals include fixed control and the backing wrapper.
    std::size_t resident_payload_bytes = 0;
    std::size_t resident_metadata_bytes = 0;
    // Only these optional shared lines are subject to the host byte allowance.
    std::size_t read_cache_payload_bytes = 0;
    std::size_t read_cache_metadata_bytes = 0;
    std::uint64_t backing_reads = 0;
    std::uint64_t backing_read_bytes = 0;
};
