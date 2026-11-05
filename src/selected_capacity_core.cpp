// Selected-capacity common primitive. Field observations are implemented by
// their later owners after the native reader has a validated actual record.

#include "selected_capacity_internal.hpp"

#include <pineforge/run_failure.hpp>

#include <limits>
#include <string>

namespace pineforge::detail::selected_capacity {

void reserve(std::uint64_t& used, const Bound& bound, std::uint64_t requested,
             RunFailureCode code, const char* english) {
    if (bound.present
        && (used > bound.value || requested > bound.value - used)) {
        RunFailureArgs args{
            {"limit", std::to_string(bound.value)},
            {"used", std::to_string(used)},
            {"requested", std::to_string(requested)},
        };
        throw coded<std::runtime_error>(code, args, english ? english : "");
    }

    if (requested > std::numeric_limits<std::uint64_t>::max() - used) {
        throw coded<std::runtime_error>(
            RunFailureCode::engine_invariant, {}, "Pine native run counter exhausted");
    }

    used += requested;
}

}  // namespace pineforge::detail::selected_capacity
