#pragma once

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_tables.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pineforge::detail {

// The bounded profile admits physical scratch bytes. Population fields remain
// in the ABI as zero-valued compatibility hints; they are not admission
// maxima. Actual growth is charged to the attached scratch spool.
inline ScratchManifest default_scratch_manifest(std::uint64_t bars) {
    if (bars > std::numeric_limits<std::uint64_t>::max() / 2)
        throw ScratchCapacityError(ScratchResource::layout,
                                   "default scratch replacement reserve overflow");
    return {
        0, 0, // K_q/B_q retained in the descriptor; the q store is removed.
        0, 25165824,
        0, 4227072,
        0, 4194304,
        27, 6912,
        0,
    };
}

// A stream without a declared lifetime bar count has the same byte policy.
inline ScratchManifest default_scratch_manifest() {
    return default_scratch_manifest(0);
}

} // namespace pineforge::detail
