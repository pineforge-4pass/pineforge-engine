#pragma once
#include <pineforge/engine.hpp>
#include <pineforge/source/market_admission.hpp>

// Storage counterfactuals only. Production never rewrites an original command.
// The retired source PendingOrder declaration is deliberately not included:
// native-route twins supply their read-only L4dPendingOrder projection before
// including this fixture.  This header is not a production compatibility seam.
namespace placement_fixture {
template<class Change>
void change(pineforge::source::PendingOrder& order, Change edit) {
    using namespace pineforge;
    auto observed = std::make_shared<admission::CommandObservation>();
    if (order.market_admission.observation()) {
        *observed = *order.market_admission.observation();
    } else {
        observed->command = order.incarnation ? order.incarnation : 1;
        observed->bar = order.created_bar;
        observed->id = order.id;
        observed->buy = order.is_long;
    }
    edit(*observed);
    admission::Draft replacement;
    replacement.bind(observed);
    if (order.market_admission.review())
        replacement.reviewed(*order.market_admission.review());
    if (order.market_admission.sizing_revision())
        replacement.sizing_revised(*order.market_admission.sizing_revision());
    order.market_admission = std::move(replacement);
}
inline void prior_close_quantity(pineforge::source::PendingOrder& order, double quantity) {
    change(order, [quantity](auto& observation) { observation.prior_close_quantity = quantity; });
}
inline void at_capacity(pineforge::source::PendingOrder& order) {
    change(order, [](auto& observation) {
        observation.placement_side = static_cast<int>(observation.buy
            ? pineforge::PositionSide::LONG : pineforge::PositionSide::SHORT);
        observation.held_entries = 1;
        observation.configuration.pyramiding = 1;
    });
}
}
