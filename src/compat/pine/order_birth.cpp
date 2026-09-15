#include <pineforge/compat/pine/order_birth.hpp>

namespace pineforge::compat::pine {

HistoricalBirthReach select_historical_birth_reach(const OrderBirth& birth,
                                                    bool requested_trailing_exit) noexcept {
    if (!birth.from_fill() || first_open_fill_evaluation(birth))
        return HistoricalBirthReach::Standard;
    const bool later_open_trailing_exit = birth.cursor().domain()
            == BirthCursorDomain::HistoricalPath
        && birth.cursor().first_point() && requested_trailing_exit;
    return later_open_trailing_exit ? HistoricalBirthReach::Standard
                                    : HistoricalBirthReach::ExtremeWaypoints;
}

} // namespace pineforge::compat::pine
