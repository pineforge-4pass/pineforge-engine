#pragma once

#include <pineforge/native_calendar.hpp>

#include <optional>
#include <string_view>
#include <utility>

namespace pineforge::internal {

using NativeFeedPeriod = std::pair<native_calendar::TimeframeUnit, int>;
inline constexpr NativeFeedPeriod daily_feed_period{native_calendar::TimeframeUnit::Day, 1};

inline NativeFeedPeriod native_feed_period(const native_calendar::Timeframe& tf) noexcept {
    // Keep calendar units distinct. Express whole minutes of seconds in
    // minutes so fixed aliases agree without multiplying a parsed count.
    using Unit = native_calendar::TimeframeUnit;
    if (tf.unit() == Unit::Second && tf.count() % 60 == 0)
        return {Unit::Minute, tf.count() / 60};
    return {tf.unit(), tf.count()};
}

inline std::optional<NativeFeedPeriod> native_feed_period(std::string_view spelling) {
    if (const auto tf = native_calendar::parse_timeframe(spelling))
        return native_feed_period(*tf);
    return std::nullopt;
}

} // namespace pineforge::internal
