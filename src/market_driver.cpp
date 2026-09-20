#include <pineforge/market_driver.hpp>
#include <pineforge/native_run_spec.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace pineforge {
inline namespace native_driver_v5 {

namespace {

// The legacy batch route deliberately admitted price-domain values that the
// native market model normally refuses. Keep the ordinary native predicate
// below strict; this exact legacy shape is available only through the hashed
// run-spec tolerance and only for a batch-style preflight.
bool legacy_batch_bar_structurally_valid(const Bar& bar) noexcept {
    if (!std::isfinite(bar.open)) return false;
    if (!std::isfinite(bar.high)) return false;
    if (!std::isfinite(bar.low)) return false;
    if (!std::isfinite(bar.close)) return false;
    if (bar.low > std::min(bar.open, bar.close)) return false;
    if (bar.high < std::max(bar.open, bar.close)) return false;
    return std::isnan(bar.volume) || (std::isfinite(bar.volume) && bar.volume >= 0.0);
}

bool legacy_stream_warmup_bar_structurally_valid(const Bar& bar) noexcept {
    if (!std::isfinite(bar.open) || bar.open < 0.0) return false;
    if (!std::isfinite(bar.high) || bar.high < 0.0) return false;
    if (!std::isfinite(bar.low) || bar.low < 0.0) return false;
    if (!std::isfinite(bar.close) || bar.close < 0.0) return false;
    if (bar.low > std::min(bar.open, bar.close)) return false;
    if (bar.high < std::max(bar.open, bar.close)) return false;
    return std::isfinite(bar.volume) && bar.volume >= 0.0;
}

bool preflight_bar_structurally_valid(const NativeRunSpec& spec, const Bar& bar,
                                      NativeInputPolicy policy) noexcept {
    if (policy == NativeInputPolicy::StreamWarmup
        && native_feed_tolerance_enabled(
            spec.legacy_tolerance, NativeFeedTolerance::WarmupNonNegativeOHLC)) {
        return legacy_stream_warmup_bar_structurally_valid(bar);
    }
    if (policy == NativeInputPolicy::Batch
        && native_feed_tolerance_enabled(
            spec.legacy_tolerance, NativeFeedTolerance::BatchStructuralBars)) {
        return legacy_batch_bar_structurally_valid(bar);
    }
    return native_bar_structurally_valid(bar);
}

bool legacy_tolerant_slot_labels(const NativeRunSpec& spec) noexcept {
    return spec.slot_label_policy == NativeSlotLabelPolicy::FeedTolerant;
}

bool timestamp_delta_overflows(std::int64_t previous, std::int64_t current) noexcept {
    return previous < 0
        && current > std::numeric_limits<std::int64_t>::max() + previous;
}

}  // namespace

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
    auto parsed_session = native_calendar::parse_session(spec.session, spec.timezone);
    if (!parsed_session) {
        out.error = NativeInputPreflightError::CalendarFailure;
        return out;
    }
    // Canonical native hosts retain the base driver's refusal ordering:
    // calendar parsing first, then every bar's structural/interval/label/
    // monotonic/overlap checks in order.  The compatibility policy keeps its
    // intentionally narrower raw-label preflight below.
    const bool canonical = !spec.timeframe_undetected
        && !legacy_tolerant_slot_labels(spec);
    std::optional<native_calendar::Timeframe> parsed_tf;
    if (canonical) {
        parsed_tf = native_calendar::parse_timeframe(spec.input_tf);
        if (!parsed_tf) {
            out.error = NativeInputPreflightError::CalendarFailure;
            return out;
        }
    }
    if (canonical) {
        std::optional<native_calendar::NativeInterval> previous;
        for (int i = 0; i < n; ++i) {
            const Bar& bar = bars[i];
            if (!preflight_bar_structurally_valid(spec, bar, policy)) {
                out.error = NativeInputPreflightError::StructuralInvalid;
                out.index = i;
                return out;
            }
            if (i > 0
                && timestamp_delta_overflows(bars[i - 1].timestamp, bar.timestamp)) {
                out.error = NativeInputPreflightError::TimestampDeltaOverflow;
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
            if (i > 0) {
                const std::int64_t earlier = bars[i - 1].timestamp;
                if (bar.timestamp <= earlier) {
                    out.error = NativeInputPreflightError::NotStrictlyIncreasing;
                    out.index = i;
                    return out;
                }
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
    for (int i = 0; i < n; ++i) {
        const Bar& bar = bars[i];
        if (!preflight_bar_structurally_valid(spec, bar, policy)) {
            out.error = NativeInputPreflightError::StructuralInvalid;
            out.index = i;
            return out;
        }
        if (i > 0) {
            const std::int64_t previous = bars[i - 1].timestamp;
            if (bar.timestamp <= previous) {
                out.error = NativeInputPreflightError::NotStrictlyIncreasing;
                out.index = i;
                return out;
            }
            // An int64 delta overflow is structural on every branch: the
            // legacy source scheduler refused it (ab9714be
            // pine_scheduler.cpp:64-66) and the source route always runs
            // this tolerant branch (A39(12) P1-23 corrected).
            if (timestamp_delta_overflows(previous, bar.timestamp)) {
                out.error = NativeInputPreflightError::TimestampDeltaOverflow;
                out.index = i;
                return out;
            }
        }
    }
    return out;
}

}  // inline namespace native_driver_v5
}  // namespace pineforge
