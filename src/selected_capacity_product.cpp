#include "selected_capacity_driver.hpp"

namespace pineforge::detail::selected_capacity {

ObserverTable make_product_observer_table() noexcept {
    return ObserverTable{
        &observe_source_rows,
        &observe_primary_rows,
        &observe_script_bars,
        &observe_auxiliary_rows,
        &observe_bytes,
    };
}

DriverResult materialize_selected_capacity(
    const StreamInput* inputs, std::size_t count,
    const SelectedPlanRequest& request, const Limits& limits,
    MaterializerSink sink, void* sink_context) {
    const ObserverTable observers = make_product_observer_table();
    return run_driver(inputs, count, request, limits, observers, sink, sink_context);
}

}  // namespace pineforge::detail::selected_capacity
