#pragma once
#include "../../order_birth.hpp"

namespace pineforge { inline namespace engine_script_run_v13 { struct PendingOrder; } }
namespace pineforge::compat::pine {

// Historical Pine permissions remain policy, not physical birth facts.
enum class HistoricalBirthReach : int32_t { Standard, ExtremeWaypoints };
inline bool first_open_fill_evaluation(const OrderBirth& birth) {
    return birth.from_fill() && birth.evaluation_ordinal() == 1
        && birth.cursor().first_point();
}
HistoricalBirthReach select_historical_birth_reach(const OrderBirth& birth,
                                                   bool requested_trailing_exit);
bool historical_cascade_reach(const PendingOrder& order);

} // namespace pineforge::compat::pine

namespace pineforge {
using PineHistoricalBirthReach = compat::pine::HistoricalBirthReach;
}
