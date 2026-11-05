#include "selected_capacity_internal.hpp"

namespace pineforge::detail::selected_capacity {

void observe_primary_rows(State& state, const Limits& limits, const RecordEvent& event) {
    if (event.role != Role::Primary) return;

    reserve(state.primary_rows, limits.primary_rows, 1,
            RunFailureCode::selected_primary_rows_limit_exceeded,
            "Selected primary-row capacity exceeded.");
}

}  // namespace pineforge::detail::selected_capacity
