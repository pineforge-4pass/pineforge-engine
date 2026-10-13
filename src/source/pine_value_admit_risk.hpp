#pragma once

#include "pine_value_admission.hpp"

#include <pineforge/source/pine_adapter.hpp>

#include <cmath>

namespace pineforge::source {

// Risk statements apply live, including discarded earlier pre-roll
// evaluations. A double NaN is the existing disabled/absent limit (D1),
// so only infinities are invalid here. Percent flags carry no numeric value;
// the adapter keeps its existing sticky-true behavior.
inline void admit_pine_risk_max_drawdown(double value, bool /*percent*/) {
    if (std::isinf(value))
        throw_strategy_value_invalid(value_field::risk_max_drawdown,
                                     StrategyValueReason::nonfinite);
}

inline void admit_pine_risk_max_intraday_loss(double value, bool /*percent*/) {
    if (std::isinf(value))
        throw_strategy_value_invalid(value_field::risk_max_intraday_loss,
                                     StrategyValueReason::nonfinite);
}

inline void admit_pine_risk_max_position_size(double value) {
    if (std::isinf(value))
        throw_strategy_value_invalid(value_field::risk_max_position_size,
                                     StrategyValueReason::nonfinite);
}

// direction, max_cons_loss_days and max_intraday_filled_orders take int,
// which cannot represent NaN/infinity. They need no admission and retain
// all existing domain, cap-assignment and count-switch behavior.

// Assertion of the admission invariant, immediately before a retained
// snapshot's direct restore. Ordinary capture copies already-admitted values;
// this release-active check also refuses a hand-built invalid retained snapshot
// through the existing typed failure path. Check all doubles before any write.
inline void assert_admitted_pine_replay_risk_config(const PineReplayRiskConfig& snapshot) {
    admit_pine_risk_max_drawdown(snapshot.max_drawdown, snapshot.max_drawdown_percent);
    admit_pine_risk_max_intraday_loss(snapshot.max_intraday_loss,
                                     snapshot.max_intraday_loss_percent);
    admit_pine_risk_max_position_size(snapshot.max_position_size);
}

} // namespace pineforge::source
