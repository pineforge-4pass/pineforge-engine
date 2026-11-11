#include "selected_capacity_internal.hpp"

namespace pineforge::detail::selected_capacity {

void observe_source_rows(State& state, const Limits& limits, const RecordEvent& event) {
    if (event.role != Role::Source) return;

    reserve(state.source_rows, limits.source_rows, 1,
            RunFailureCode::selected_source_rows_limit_exceeded,
            "Selected source-row capacity exceeded.");
}

}  // namespace pineforge::detail::selected_capacity
