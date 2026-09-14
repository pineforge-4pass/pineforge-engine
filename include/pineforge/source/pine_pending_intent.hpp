#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/frozen_market_instruction.hpp>
#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/compat/pine/order_birth.hpp>
#include <pineforge/compat/pine/order_priority.hpp>

namespace pineforge::source {

// @source-state begin
// Transitional ownership shell: the completed header transfer replaces this
// with the verbatim relocated declaration once the generic header has no
// PendingOrder storage.
struct PendingOrder : ::pineforge::PendingOrder {};

inline bool placement_has_prior_close(const PendingOrder& order) {
    return ::pineforge::placement_has_prior_close(order);
}
inline bool placement_at_entry_capacity(const PendingOrder& order) {
    return ::pineforge::placement_at_entry_capacity(order);
}
inline bool placement_has_opposite_market_predecessor(
        const MarketAdmissionJournal& journal, const PendingOrder& current) {
    return ::pineforge::placement_has_opposite_market_predecessor(journal, current);
}

// @source-state end

} // namespace pineforge::source
