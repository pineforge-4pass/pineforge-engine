#include "selected_capacity_internal.hpp"

namespace pineforge::detail::selected_capacity {

void observe_bytes(State& state, const Limits& limits, std::uint64_t actual_csv_bytes) {
    reserve(state.bytes, limits.bytes, actual_csv_bytes,
            RunFailureCode::selected_bytes_limit_exceeded,
            "Selected source-and-retained CSV byte capacity exceeded.");
}

}  // namespace pineforge::detail::selected_capacity
