#include <pineforge/market_driver.hpp>
#include <pineforge/native_run_spec.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace pineforge {
inline namespace native_driver_v3 {

bool native_bar_structurally_valid(const Bar& bar) noexcept {
    if (!std::isfinite(bar.open) || bar.open <= 0.0) return false;
    if (!std::isfinite(bar.high) || bar.high <= 0.0) return false;
    if (!std::isfinite(bar.low) || bar.low <= 0.0) return false;
    if (!std::isfinite(bar.close) || bar.close <= 0.0) return false;
    if (bar.low > std::min(bar.open, bar.close)) return false;
    if (bar.high < std::max(bar.open, bar.close)) return false;
    if (!std::isfinite(bar.volume) || bar.volume < 0.0) return false;
    return true;
}

NativeInputPreflightResult preflight_native_inputs(
        const NativeRunSpec& spec,
        const Bar* bars,
        int n,
        NativeInputPolicy policy) {
    NativeInputPreflightResult out;
    if (n < 0) {
        out.error = NativeInputPreflightError::InvalidCount;
        return out;
    }
    if (n > 0 && bars == nullptr) {
        out.error = NativeInputPreflightError::NullArray;
        return out;
    }
    auto parsed_tf = native_calendar::parse_timeframe(spec.input_tf);
    auto parsed_session = native_calendar::parse_session(spec.session, spec.timezone);
    if (!parsed_tf || !parsed_session) {
        out.error = NativeInputPreflightError::CalendarFailure;
        return out;
    }
    std::optional<native_calendar::NativeInterval> previous;
    for (int i = 0; i < n; ++i) {
        const Bar& bar = bars[i];
        if (!native_bar_structurally_valid(bar)) {
            out.error = NativeInputPreflightError::StructuralInvalid;
            out.index = i;
            return out;
        }
        auto interval = native_calendar::interval_containing(
            *parsed_session, *parsed_tf, bar.timestamp);
        if (!interval) {
            out.error = NativeInputPreflightError::Unaligned;
            out.index = i;
            return out;
        }
        if (!native_confirmed_bar_label_admitted(*interval, bar.timestamp)) {
            out.error = NativeInputPreflightError::OffGridLabel;
            out.index = i;
            return out;
        }
        if (i > 0 && bar.timestamp <= bars[i - 1].timestamp) {
            out.error = NativeInputPreflightError::NotStrictlyIncreasing;
            out.index = i;
            return out;
        }
        if (previous) {
            if (interval->open_ms <= previous->open_ms) {
                out.error = NativeInputPreflightError::OverlappingSlot;
                out.index = i;
                return out;
            }
            if (policy == NativeInputPolicy::StreamWarmup) {
                auto expected = native_calendar::interval_containing(
                    *parsed_session, *parsed_tf, previous->next_input_open_ms);
                if (!expected || expected->open_ms != interval->open_ms) {
                    out.error = NativeInputPreflightError::InSessionGap;
                    out.index = i;
                    return out;
                }
            }
        }
        previous = *interval;
    }
    return out;
}

}  // inline namespace native_driver_v3
}  // namespace pineforge
