#pragma once

#include <limits>

namespace pineforge::detail {

// Preserve the number of observations up to the largest count we can store.
inline void advance_observation_count(int& count) noexcept {
    if (count < std::numeric_limits<int>::max()) ++count;
}

} // namespace pineforge::detail
