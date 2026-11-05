#include "selected_capacity_internal.hpp"

#include <pineforge/run_failure.hpp>

#include <stdexcept>

namespace pineforge::detail::selected_capacity {

void observe_script_bars(State& state, const Limits& limits, const RecordEvent& event) {
    if (event.role != Role::Primary) return;
    if (!event.has_script_bucket)
        throw coded<std::logic_error>(
            RunFailureCode::engine_invariant, {},
            "script-bar observer got a Primary row without a script bucket");
    if (state.has_last_primary_bucket
        && state.last_primary_bucket == event.script_bucket_key) return;

    // Each changed Primary script-bucket key opens one bucket, including
    // pre-roll and a final partial bucket.
    reserve(state.script_bars, limits.script_bars, 1,
            RunFailureCode::selected_script_bars_limit_exceeded,
            "Selected script-bar capacity exceeded.");
    state.last_primary_bucket = event.script_bucket_key;
    state.has_last_primary_bucket = true;
}

}  // namespace pineforge::detail::selected_capacity
