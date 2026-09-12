#pragma once

#include <pineforge/bar.hpp>
#include <pineforge/native_calendar.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace pineforge {
inline namespace native_run_spec_v1 { struct NativeRunSpec; }
inline namespace native_driver_v3 {

// Semantic versions hashed into native continuation identity.
inline constexpr const char* kNativeDriverSemanticVersion = "native-driver/v3";
inline constexpr const char* kNativeConsumerSemanticVersion = "native-consumer/v4";
inline constexpr const char* kNativeCalendarSemanticVersion = "native-calendar/v1";

enum class NativePriceProvenance : std::uint8_t {
    Confirmed = 0,
    ObservedPrint = 1,
    ModeledOHLCOpen = 2,
    ModeledOHLCClose = 3,
    CarriedOpen = 4,
    AfterCalculationClose = 5,
    PartialFinalized = 6,
    Calculation = 7,
};

enum class NativePathPhase : std::uint8_t {
    None = 0,
    Open = 1,
    High = 2,
    Low = 3,
    Close = 4,
};

enum class NativeCompletionKind : std::uint8_t {
    Confirmed = 0,
    LazyComplete = 1,
    SessionShortened = 2,
    PartialFinalized = 3,
};

struct NativeCoordinate {
    uint64_t ordinal = 0;
    int interval_index = 0;
    int64_t open_ms = 0;
    int64_t eligible_open_ms = 0;
    int64_t last_traded_close_ms = 0;
    int64_t next_period_open_ms = 0;
    int64_t next_input_open_ms = 0;
    int64_t effective_time_ms = 0;
    int64_t source_price_time_ms = 0;
    NativePriceProvenance provenance = NativePriceProvenance::Confirmed;
    NativePathPhase path_phase = NativePathPhase::None;
    NativeCompletionKind completion = NativeCompletionKind::Confirmed;
};

struct NativeDriverPoint {
    NativeCoordinate coordinate;
    double raw_price = 0.0;
    std::optional<uint64_t> sequence;
    bool matching = false;
    bool excursion = false;
};

// Presentation snapshot copied onto the callback stack. Mutating these
// fields cannot change the consumer's decision floor, matching time, or
// after-calculation coordinate.
struct NativeDecisionContext {
    NativeCoordinate coordinate;
    int64_t decision_floor_ms = 0;
    native_calendar::NativeInterval input_interval{};
    native_calendar::NativeInterval script_interval{};
};

// Pump-produced events obtain ordinals from the consumer allocator.
class INativeDriverSink {
public:
    virtual ~INativeDriverSink() = default;
    virtual uint64_t allocate_native_ordinal() = 0;
    virtual void on_native_matching_point(const NativeDriverPoint& point) = 0;
    virtual void on_native_excursion(const NativeDriverPoint& point) = 0;
    virtual void on_native_calculation(const Bar& script_bar, const NativeCoordinate& coordinate) = 0;
};

bool native_bar_structurally_valid(const Bar& bar) noexcept;

// Confirmed-bar labels admitted by v9: the immutable nominal slot origin
// and the scheduled clipped eligible opening. Both identify one slot key
// `interval.open_ms`. Mid-slot and other off-grid timestamps are refused.
inline bool native_confirmed_bar_label_admitted(
        const native_calendar::NativeInterval& interval, int64_t timestamp) noexcept {
    return timestamp == interval.open_ms || timestamp == interval.eligible_open_ms;
}

// Canonical exclusive completion of a confirmed input slot. Source-price
// time remains a separate fact (bar.timestamp / last print).
inline int64_t native_canonical_input_completion(
        const native_calendar::NativeInterval& interval) noexcept {
    return interval.next_period_open_ms;
}

// ---------------------------------------------------------------------------
// Shared pure input preflight (host + runner)
//
// Exact signature the runner worker should bind:
//   NativeInputPreflightResult
//   preflight_native_inputs(const NativeRunSpec& spec,
//                           const Bar* bars,
//                           int n,
//                           NativeInputPolicy policy);
//
// Calendar-aware, allocation of diagnostic text is the caller's job.
// No host reset, callback, identity mutation, high-water change, or
// decision-floor change. Parses the spec's session/timezone/input_tf
// on each call. Batch may be sparse; StreamWarmup refuses missing
// in-session bars after a closed gap by walking next_input_open_ms
// through interval_containing (Fri 16 -> Mon 11 cannot skip Mon 09:30).
// Warmup complete-script policy is NOT decided here.
// ---------------------------------------------------------------------------
enum class NativeInputPolicy : std::uint8_t {
    Batch = 0,
    StreamWarmup = 1,
};

enum class NativeInputPreflightError : std::uint16_t {
    None = 0,
    NullArray = 1,
    InvalidCount = 2,
    StructuralInvalid = 3,
    Unaligned = 4,
    OffGridLabel = 5,
    NotStrictlyIncreasing = 6,
    OverlappingSlot = 7,
    InSessionGap = 8,
    CalendarFailure = 9,
};

struct NativeInputPreflightResult {
    NativeInputPreflightError error = NativeInputPreflightError::None;
    int index = -1;  // first offending bar, or -1 when not index-specific

    constexpr bool ok() const noexcept {
        return error == NativeInputPreflightError::None;
    }
    constexpr explicit operator bool() const noexcept { return ok(); }
};

NativeInputPreflightResult preflight_native_inputs(
        const NativeRunSpec& spec,
        const Bar* bars,
        int n,
        NativeInputPolicy policy);

}  // inline namespace native_driver_v3
}  // namespace pineforge
