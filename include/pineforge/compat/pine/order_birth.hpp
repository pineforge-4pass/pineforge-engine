#pragma once

#include <pineforge/order_birth.hpp>

#include <cstdint>

namespace pineforge::compat::pine {

// Historical Pine reach is source policy layered over generic immutable birth
// facts.  It is neither an additional native trigger state nor a second book.
enum class HistoricalBirthReach : std::int32_t { Standard, ExtremeWaypoints };

inline bool first_open_fill_evaluation(const OrderBirth& birth) noexcept {
    return birth.from_fill() && birth.evaluation_ordinal() == 1
        && birth.cursor().first_point();
}

HistoricalBirthReach select_historical_birth_reach(const OrderBirth& birth,
                                                    bool requested_trailing_exit) noexcept;
inline bool historical_cascade_reach(HistoricalBirthReach reach) noexcept {
    return reach == HistoricalBirthReach::ExtremeWaypoints;
}

} // namespace pineforge::compat::pine

namespace pineforge { using PineHistoricalBirthReach = compat::pine::HistoricalBirthReach; }
