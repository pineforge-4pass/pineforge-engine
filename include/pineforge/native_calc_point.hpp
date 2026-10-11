#pragma once

#include <cstdint>

// The calculation points of a driving-event schedule: what the kernel derives,
// for an instance that computes on a declared cadence, from the events it is
// given and its own bar state (src/native_calc_plan.hpp holds the derivation).
// Plain data only, so the boundary that exposes a plan and the code that checks
// one read the same vocabulary.

namespace pineforge::native_calc {

/// The driving events of a schedule. The numbers are the event-kind registry of
/// the named-stream boundary; zero, and every value above AdvanceTime, name no
/// event: the derivation refuses them rather than plan nothing for them.
enum class EventKind : std::uint32_t {
    WarmupBar = 1,
    Bar = 2,
    Tick = 3,
    AdvanceTime = 4,
};

/// Whether `kind` is in the registry: the one range check a raw number passes
/// before the derivation treats it as an EventKind.
constexpr bool is_known(EventKind kind) noexcept {
    const auto raw = static_cast<std::uint32_t>(kind);
    return raw >= static_cast<std::uint32_t>(EventKind::WarmupBar)
        && raw <= static_cast<std::uint32_t>(EventKind::AdvanceTime);
}

/// One driving event: its kind and its time. `time_ms` is a tick's time, a
/// bar's open time, or the instant a time advance moves the clock to.
struct Event {
    EventKind kind = EventKind::Tick;
    std::int64_t time_ms = 0;
};

/// Which driving events make an instance calculate, as a bit set. A cadence
/// always contains kCadenceBarClose; the other two bits are optional.
inline constexpr std::uint32_t kCadenceAdvanceTime = 1u << 0;
inline constexpr std::uint32_t kCadenceBarClose = 1u << 1;
inline constexpr std::uint32_t kCadenceTick = 1u << 2;
inline constexpr std::uint32_t kCadenceMask =
    kCadenceAdvanceTime | kCadenceBarClose | kCadenceTick;

/// Whether a calculation ran before the payload of its driving event was
/// applied (a bar closed only because time reached its end) or after it.
enum class Phase : std::uint32_t {
    Pre = 0,
    Post = 1,
};

/// Provisional for an intrabar calculation, Confirmed for the calculation that
/// closes a bar.
enum class Finality : std::uint32_t {
    Provisional = 0,
    Confirmed = 1,
};

/// One planned calculation.
struct Point {
    /// Zero-based index of the driving event among the events of the schedule.
    std::uint64_t event_index = 0;
    /// Trace ordinal of the driving event, one-based and independent of how the
    /// trace is cut into schedules.
    std::uint64_t event = 0;
    /// Ordinal of the bar lifecycle the calculation belongs to, assigned by the
    /// kernel: zero for the first lifecycle opened, one more for each next.
    std::uint64_t bar_key = 0;
    /// Ordinal of the calculation within its bar lifecycle.
    std::uint64_t calc_ordinal = 0;
    /// Ordinal of the calculation within its driving event.
    std::uint64_t calc = 0;
    /// Event-time position: the bar's exclusive end (never later than the event
    /// time) for a Pre calculation, the event time itself for a Post one.
    std::int64_t at_ms = 0;
    Phase phase = Phase::Pre;
    Finality finality = Finality::Provisional;
};

/// How a derivation ended. Ok with a count of zero is a schedule that plans no
/// calculation; every other status is a refusal and plans nothing.
enum class Status : std::uint32_t {
    Ok = 0,
    /// The consumer's schedule group was not enabled for the current run.
    NotEngaged,
    /// A null event array with a nonzero count.
    BadArguments,
    /// A cadence without the bar-close bit, or with a bit outside the set.
    BadCadence,
    /// An event kind outside the registry.
    UnknownEventKind,
    /// A bar lifecycle had to open while the last ordinal assigned was already
    /// the largest representable one: the boundary reports capacity exhausted.
    BarKeyExhausted,
    /// The trace ordinal of the next event is the largest representable one.
    TraceExhausted,
    /// The timebase has no bar interval at the time of an event that opens one.
    NoBarSlot,
    /// The interval the timebase holds for an event ends no later than the
    /// event: its end cannot be the exclusive end of a bar that event opens.
    BarEndNotAfterEvent,
    /// A position the kernel planned itself lies outside the window of its
    /// driving event: an internal invariant failure, not an admission error.
    PositionOutsideWindow,
};

}  // namespace pineforge::native_calc
