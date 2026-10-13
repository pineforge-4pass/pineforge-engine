#pragma once

#include <pineforge/source/pine_adapter.hpp>

#include "pine_value_admission.hpp"

#include <cmath>

namespace pineforge::source {

// D3-a is pending. MAP: strategy(na/computed expression) is not emitted as a
// numeric member; omissions are finite. No script provenance for a raw host
// NaN is established, so preserve it. The supervisor can flip this one line.
inline constexpr bool kDeclarationNanIsInvalid = false;

inline bool declaration_value_is_invalid(double value) noexcept {
    return std::isinf(value) || (kDeclarationNanIsInvalid && std::isnan(value));
}

// Call only for the source declaration, before storage and any caller override
// merge. Finite values, including non-positive margins, keep their definitions.
// Existing financial/domain refusals and caller values have their own doors.
inline void admit_strategy_declaration(const PineStrategyConfig& config) {
    if (declaration_value_is_invalid(config.default_qty_value)) {
        throw_strategy_value_invalid(value_field::strategy_default_qty_value,
                                     StrategyValueReason::nonfinite);
    }
    if (declaration_value_is_invalid(config.margin_long)) {
        throw_strategy_value_invalid(value_field::strategy_margin_long,
                                     StrategyValueReason::nonfinite);
    }
    if (declaration_value_is_invalid(config.margin_short)) {
        throw_strategy_value_invalid(value_field::strategy_margin_short,
                                     StrategyValueReason::nonfinite);
    }
}

}  // namespace pineforge::source
