#include "selected_capacity_internal.hpp"

namespace pineforge::detail::selected_capacity {

void observe_auxiliary_rows(State& state, const Limits& limits, const RecordEvent& event) {
    if (event.role != Role::Auxiliary) return;

    reserve(state.auxiliary_rows, limits.auxiliary_rows, 1,
            RunFailureCode::selected_auxiliary_rows_limit_exceeded,
            "Selected auxiliary-row capacity exceeded.");
}

}  // namespace pineforge::detail::selected_capacity
