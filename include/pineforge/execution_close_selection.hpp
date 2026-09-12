#pragma once

#include <cstdint>
#include <vector>

namespace pineforge::execution {
inline namespace close_selection_v1 {

// Transient membership for one inspect/project/settle call.
// Run-local opening-request provenances in the engine's current cycle.
// Not a CloseScope alternative, not a Request field, not hashed, not stored.
struct SelectedOpeningSet {
    std::int64_t cycle = 0;
    std::vector<std::uint64_t> incarnations;
};

} // inline namespace close_selection_v1
} // namespace pineforge::execution
