#pragma once

#include <pineforge/bar.hpp>
#include <pineforge/native_calendar.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace pineforge {
inline namespace native_run_spec_v3 { struct NativeRunSpec; }
inline namespace native_driver_v5 {

// Semantic versions hashed into native continuation identity.
inline constexpr const char* kNativeDriverSemanticVersion = "native-driver/v5";
inline constexpr const char* kNativeConsumerSemanticVersion = "native-consumer/v9";
inline constexpr const char* kNativeCalendarSemanticVersion = "native-calendar/v1";

/// Where a driver point's price came from. Confirmed is a confirmed input bar's
/// own label; ObservedPrint a realtime tick; the two ModeledOHLC values and
/// AfterCalculationClose are the modeled waypoints of a confirmed bar's path;
/// CarriedOpen is a quiet tradable interval's carried last price; PartialFinalized
/// a partially finalized observed slot; Calculation the script calculation point
/// itself; CurrentExecution a synchronous execute_current. A host reads it off
/// NativeDecisionContext::coordinate and never has to infer it.
enum class NativePriceProvenance : std::uint8_t {
    Confirmed = 0,
    ObservedPrint = 1,
    ModeledOHLCOpen = 2,
    ModeledOHLCClose = 3,
    CarriedOpen = 4,
    AfterCalculationClose = 5,
    PartialFinalized = 6,
    Calculation = 7,
    CurrentExecution = 8,
};

/// Which leg of a modeled OHLC walk a point sits on. None is a discrete point (a
/// calculation, an observed print); Open, High, Low and Close are the waypoints,
/// in the order NativeRunSpec::path_order resolves. A margin check point's
/// cursor.point.path_phase is the waypoint the check was taken at.
enum class NativePathPhase : std::uint8_t {
    None = 0,
    Open = 1,
    High = 2,
    Low = 3,
    Close = 4,
};

/// How a script interval or a higher-timeframe bucket was closed. Confirmed means
/// its own last contributing bar closed it; LazyComplete that the NEXT interval's
/// first input did — a hole over the last slot, or a script interval a session
/// close clips (a subscription bucket it clips is Confirmed on its own last bar);
/// PartialFinalized that a stream end finalized a forming observed slot; no path
/// produces SessionShortened. Delivered on NativeTimeframeBarContext::completion
/// for a bucket and on the coordinate for a calculation.
enum class NativeCompletionKind : std::uint8_t {
    Confirmed = 0,
    LazyComplete = 1,
    SessionShortened = 2,
    PartialFinalized = 3,
};

/// Everything the kernel knows about WHERE a point is, as one owning value: its
/// event ordinal, the interval index, the nominal and scheduled-eligible opens,
/// the last traded close, the next period and next input opens, the effective time
/// the account converts and hashes at, the source price time, and the three
/// classifications above. A host reads it through NativeDecisionContext; mutating
/// a copy cannot move the consumer's floor, matching time or after-calculation
/// coordinate.
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

/// One point the driver produced: its coordinate, the raw price before slippage or
/// any grid, the provider's sequence for an observed print (nullopt otherwise),
/// and whether the point is a matching point, an excursion sample, or both.
/// Recorded in the event history as the NativeEventKind::Driver rows
/// native_events() returns.
struct NativeDriverPoint {
    NativeCoordinate coordinate;
    double raw_price = 0.0;
    std::optional<uint64_t> sequence;
    bool matching = false;
    bool excursion = false;
};

// Generic facts about the retained intrabar driver.  They let a host project
// run diagnostics without consulting a source scheduler or borrowing driver
// state. Counts are cumulative for the run except the two current-script-bar
// shape fields.
struct NativeDriverStatistics {
    bool intrabar_path_enabled = false;
    int sub_bars_per_script_bar = 1;
    int samples_per_sub_bar = 0;
    uint64_t sub_bars_processed = 0;
    uint64_t sample_ticks_processed = 0;
};

// Presentation snapshot copied onto the callback stack. Mutating these
// fields cannot change the consumer's decision floor, matching time, or
// after-calculation coordinate.
struct NativeDecisionContext {
    NativeCoordinate coordinate;
    int64_t decision_floor_ms = 0;
    native_calendar::NativeInterval input_interval{};
    native_calendar::NativeInterval script_interval{};
    // A non-magnified run is the one-element intrabar path.  The sub-bar
    // timestamp is deliberately separate from the script label: execution
    // ledgers use the former while script-time policy uses the latter.
    int sub_index = 0;
    int sub_count = 1;
    bool is_terminal_sub_bar = true;
    // Session-day facts of the script bar under delivery, read off the run's
    // own calendar (NativeRunSpec::session / ::timezone) and session day
    // (native_calendar::session_day_ordinal: the cycle that rolls at the first
    // window's start, keyed to its trading date — an overnight session is one
    // day across local midnight). Every callback of the bar carries the same
    // four: its open, its sub-bars and ticks, its calculation, and every fill
    // and recalculation on it.
    //   in_session         the script bar's label is in session.
    //   opens_session_day  in session, and the bar before it is not, or is on
    //                      another session day.
    //   closes_session_day in session, and the bar after it is not, or is on
    //                      another session day.
    // "The bar before / after" is the one the run holds — the batch input or
    // stream warmup being consumed — and otherwise the calendar's slot one
    // script width away. At the run's own edges nothing is held: its first bar
    // opens its session day, and a batch's final bar closes it, because a
    // batch is complete input; a stream reads on, because it continues.
    //   closes_session_day_open_ended
    //                      closes_session_day for a run whose input goes on
    //                      past it: a batch's final bar, which nothing held
    //                      follows, is judged by the calendar one script width
    //                      on, exactly as a stream's bar is, instead of by the
    //                      run's end. Every other bar reads closes_session_day.
    //                      It is what a host that recomputes a batch whose
    //                      last input is still forming reads for that bar.
    // A bar at or above the session-day grain (D, W, M) holds whole session
    // days: all four are true. Derived per point from the calendar, the label
    // and the run's input, they are presentation, not state, and fold into no
    // digest. They sit in what was this struct's padding, so no member moved.
    bool in_session = false;
    bool opens_session_day = false;
    bool closes_session_day = false;
    bool closes_session_day_open_ended = false;
    int64_t sub_bar_open_ms = 0;
    int64_t script_bar_open_ms = 0;
    NativeDriverStatistics driver_statistics{};
};

// Pump-produced events obtain ordinals from the consumer allocator.
class INativeDriverSink {
public:
    virtual ~INativeDriverSink() = default;
    /// The consumer's ordinal allocator, called by the pump for every event it
    /// produces. Implemented by the kernel's own consumer; a host never implements
    /// this interface.
    virtual uint64_t allocate_native_ordinal() = 0;
    /// A point at which live requests may match. The consumer runs the matching pass
    /// here; excursion-only points do not reach it.
    virtual void on_native_matching_point(const NativeDriverPoint& point) = 0;
    /// A point that updates open lots' favorable and adverse excursions without
    /// offering a match.
    virtual void on_native_excursion(const NativeDriverPoint& point) = 0;
    /// The script bar's calculation point, with the complete script bar and its
    /// coordinate. The consumer turns it into the host's on_native_bar /
    /// on_native_recalculate call.
    virtual void on_native_calculation(const Bar& script_bar, const NativeCoordinate& coordinate) = 0;
};

/// Whether a bar can be admitted at all: finite positive OHLC with
/// high >= max(open, close) and low <= min(open, close), and a finite nonnegative
/// volume. A run's NativeFeedTolerance may relax the positivity and the volume
/// half; this is the strict predicate those bits are measured against. It is also
/// what refuses a bar of an auxiliary feed (InvalidAuxiliaryFeedBar).
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

/// Why a bar array was refused before the run touched anything. NullArray and
/// InvalidCount are the argument shape; StructuralInvalid is
/// native_bar_structurally_valid; Unaligned and OffGridLabel are the slot label;
/// NotStrictlyIncreasing and OverlappingSlot the ordering; InSessionGap a missing
/// in-session slot, which only a stream warmup refuses; CalendarFailure a calendar
/// the spec's own timezone and session could not resolve. A preflight refusal
/// leaves the host Ready or Running and does NOT raise the decision floor.
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
    TimestampDeltaOverflow = 10,
};

/// What preflight_native_inputs answers: the error and the first offending bar's
/// index, or -1 when the refusal is not about one bar. ok() and the explicit
/// operator bool are the success reads.
struct NativeInputPreflightResult {
    NativeInputPreflightError error = NativeInputPreflightError::None;
    int index = -1;  // first offending bar, or -1 when not index-specific

    constexpr bool ok() const noexcept {
        return error == NativeInputPreflightError::None;
    }
    constexpr explicit operator bool() const noexcept { return ok(); }
};

/// Judge a whole bar array against a run spec before any of it is consumed, under
/// NativeInputPolicy::Batch (sparse input is admitted; missing in-session slots
/// are legal) or ::StreamWarmup (every provided slot must be a complete confirmed
/// interval and an in-session gap is refused). Every public begin runs it first,
/// which is why an invalid array leaves the lifecycle where it was instead of
/// failing the host.
NativeInputPreflightResult preflight_native_inputs(
        const NativeRunSpec& spec,
        const Bar* bars,
        int n,
        NativeInputPolicy policy);

}  // inline namespace native_driver_v5
}  // namespace pineforge
