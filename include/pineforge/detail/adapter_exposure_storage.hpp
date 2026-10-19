#pragma once

#include <pineforge/detail/exit_phase_store.hpp>

namespace pineforge::detail {

// Exposure values/handles have already been erased by their existing owner.
// Empty containers still retain buckets/capacity while a cohort's origin
// history pins its resident row. Release that storage without retiring any
// origin, changing a cohort record, or touching the hash's logical inputs.
template<class Container>
inline void release_empty_exposure_capacity(Container& container) {
    if (container.empty()) {
        Container empty;
        container.swap(empty);
    }
}

template<class Container>
inline void clear_exposure_storage(Container& container) {
    Container empty;
    container.swap(empty);
}

} // namespace pineforge::detail
