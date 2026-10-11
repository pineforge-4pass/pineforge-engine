#pragma once

// The pure derivation of a calculation schedule: from the driving events of a
// trace, the cadence a binding declared and the kernel's own bar timeline, which
// bar lifecycle every calculation belongs to, where on the event-time axis it
// sits and whether it confirms its bar. Internal to the kernel; the plain data
// it speaks in is include/pineforge/native_calc_point.hpp.
//
// One function, plan_schedule, serves the count query and the apply that
// commits: it reads a State and writes none, and a caller that commits keeps the
// State it returns. Nothing here reads a clock, a calendar or a consumer: the
// timeline arrives as a BarClock, which the consumer builds over the calendar
// and the script timeframe its own sealing path reads, so the end of a bar is
// derived by the kernel and never stated by a caller.

#include <pineforge/native_calc_point.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <tuple>
#include <vector>

namespace pineforge::native_calc {

/// One bar interval of the timebase.
struct Slot {
    /// Exclusive end: the nominal open of the next interval, the value the
    /// consumer's sealing path compares time against.
    std::int64_t end_ms = 0;
    /// Where the lookup of the interval after this one starts, skipping closed
    /// time. Must lie after the instant that was looked up for the walk to move.
    std::int64_t next_open_ms = 0;
    /// Whether a quiet interval here yields a carry-forward bar.
    bool tradable = false;
};

/// The slot that holds an instant, or false when the timebase has none there.
using BarClock = std::function<bool(std::int64_t at_ms, Slot& slot)>;

/// Everything a schedule carries from one schedule to the next. The fields are
/// listed once, in tie(); the digest and every comparison read that list.
struct State {
    /// Trace ordinal of the next driving event (one-based).
    std::uint64_t trace_first = 1;
    /// Whether an event has been planned, and the time of the last one: the
    /// lower bound of a Pre position's window.
    bool has_prev_event = false;
    std::int64_t prev_event_ms = 0;
    /// Whether a bar lifecycle was ever opened, and the last ordinal assigned.
    bool has_last_key = false;
    std::uint64_t last_bar_key = 0;
    /// Whether the lifecycle of last_bar_key is still open, the exclusive end the
    /// kernel derived for it when it opened (zero while none is open) and how
    /// many calculations it has had.
    bool bar_open = false;
    std::int64_t open_end_ms = 0;
    std::uint64_t calc_in_bar = 0;
    /// Where the next quiet interval is looked up: the interval after the last
    /// bar lifecycle's. Meaningful once has_last_key is set.
    std::int64_t cursor_ms = 0;

    auto tie() const noexcept {
        return std::tie(trace_first, has_prev_event, prev_event_ms, has_last_key,
                        last_bar_key, bar_open, open_end_ms, calc_in_bar, cursor_ms);
    }
};

/// The outcome of one derivation. `after` and `points` mean something only when
/// `status` is Ok; a refusal returns the State it was given and no points.
struct Plan {
    Status status = Status::Ok;
    /// Calculations planned; counted even when the points are not collected.
    std::uint64_t count = 0;
    State after;
    std::vector<Point> points;
};

/// Derives the calculations `events` make an instance with `cadence` perform,
/// starting from `before`, over the timeline `clock` answers for. Collects the
/// points only when asked; the count is exact either way.
Plan plan_schedule(const State& before, std::uint32_t cadence, const Event* events,
                   std::size_t count, const BarClock& clock, bool want_points);

/// A word that changes whenever any field of the State does.
std::uint64_t digest(const State& state) noexcept;

/// The continuation identity of a consumer whose schedule group is engaged: the
/// consumer's own identity folded with the group's digest.
std::uint64_t combine(std::uint64_t legacy, std::uint64_t group) noexcept;

}  // namespace pineforge::native_calc
