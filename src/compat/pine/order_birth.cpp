#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/order_birth.hpp>
#include <pineforge/source/pine_pending_intent.hpp>

namespace pineforge::compat::pine {

HistoricalBirthReach select_historical_birth_reach(const OrderBirth& birth,
                                                   bool requested_trailing_exit) {
    if (!birth.from_fill() || first_open_fill_evaluation(birth))
        return HistoricalBirthReach::Standard;
    // Existing later-same-open trailing-exit exception: the origin is still
    // the later fill callback at O. Only its Pine historical reach differs.
    const bool later_open_trailing_exit =
        birth.cursor().domain() == BirthCursorDomain::HistoricalPath
        && birth.cursor().first_point() && requested_trailing_exit;
    return later_open_trailing_exit ? HistoricalBirthReach::Standard
                                   : HistoricalBirthReach::ExtremeWaypoints;
}

bool historical_cascade_reach(const source::PendingOrder& order) {
    return order.pine_birth_reach == HistoricalBirthReach::ExtremeWaypoints;
}

} // namespace pineforge::compat::pine
