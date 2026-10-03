#pragma once

#include <pineforge/native_calendar.hpp>

namespace pineforge::source {

inline bool same_calendar_timeframe(const native_calendar::Timeframe& requested,
                                    const native_calendar::Timeframe& chart) noexcept {
    return requested.is_calendar() && chart.is_calendar()
        && requested.unit() == chart.unit() && requested.count() == chart.count();
}

}  // namespace pineforge::source
