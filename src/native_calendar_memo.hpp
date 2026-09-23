#pragma once

// Internal: a caller-owned memo of one SessionCalendar's resolved session
// days (R5 lane PERF-K1). Not a public native API: it lives in src/, and the
// public native_calendar.hpp is unchanged.
//
// Resolving the session day an instant falls in costs a civil conversion of
// the instant and, for up to three candidate open dates, a fresh session day:
// every declared window instance resolved through the zone (mktime under the
// process timezone lock), merged, and copied. A caller that asks about many
// instants of the same few days -- a run's bars, their intervals, their
// session-day facts -- pays that per question. The overloads below answer
// exactly what their memo-free namesakes in native_calendar.hpp answer,
// value for value, and resolve each session day once:
//
//   * session_day_from_open is a pure function of the calendar VALUE
//     (parse_session is its only constructor) and the open date, so its
//     results are kept in a direct-mapped table keyed by the date's day
//     number and handed out by address, never copied;
//   * the containing-day lookup keeps the memo-free algorithm itself: the
//     instant's local civil date, then the open dates on it, the day before
//     and the day after, first holding day wins. The local date is integer
//     arithmetic for a UTC calendar and one localtime per new whole second
//     otherwise, kept for the last few seconds asked. It is NOT short-cut by
//     "the last day resolved holds this instant": around a clock change of a
//     day or more (America/Sitka 1867, Pacific/Apia 1892, Pacific/Kwajalein
//     1969) the cycles overlap and that shortcut answers a different day than
//     the memo-free lookup does;
//   * whether the calendar's zone is UTC is decided once per memo.
//
// A memo serves one calendar value. The owner calls reset() whenever the
// calendar it passes is rebuilt or reassigned; a memo handed a calendar at
// another address forgets everything first. It holds no pointer into the
// calendar between calls, a bounded number of days (kDaySlots) and a few
// civil dates, and is not thread-safe: one owner, one thread, like the
// calendar value it serves.

#include <pineforge/native_calendar.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace pineforge::native_calendar {
inline namespace native_calendar_v2 {

class SessionDayMemo {
public:
    // Defined in native_calendar.cpp, the only translation unit that reads it.
    struct State;

    SessionDayMemo() noexcept;
    ~SessionDayMemo();
    SessionDayMemo(SessionDayMemo&&) noexcept;
    SessionDayMemo& operator=(SessionDayMemo&&) noexcept;
    SessionDayMemo(const SessionDayMemo&) = delete;
    SessionDayMemo& operator=(const SessionDayMemo&) = delete;

    // Forget every resolved day. Call it whenever the calendar this memo
    // serves is rebuilt.
    void reset() noexcept;

    // The memo's state for `calendar`, created on first use; a calendar at
    // another address than the last one's resets it first.
    State& state(const SessionCalendar& calendar);

private:
    std::unique_ptr<State> state_;
};

// Value for value what interval_containing(calendar, script_tf, input_tf, ms)
// answers.
std::optional<NativeInterval> interval_containing(const SessionCalendar& calendar,
                                                  const Timeframe& script_tf,
                                                  const Timeframe& input_tf,
                                                  int64_t ms,
                                                  SessionDayMemo& memo);

// Value for value what interval_containing(calendar, tf, ms) answers.
std::optional<NativeInterval> interval_containing(const SessionCalendar& calendar,
                                                  const Timeframe& tf,
                                                  int64_t ms,
                                                  SessionDayMemo& memo);

// Value for value what session_day_at(calendar, ms) answers.
std::optional<NativeSessionDay> session_day_at(const SessionCalendar& calendar,
                                               int64_t ms,
                                               SessionDayMemo& memo);

// Value for value what in_session(calendar, ms) answers.
bool in_session(const SessionCalendar& calendar, int64_t ms, SessionDayMemo& memo);

}  // inline namespace native_calendar_v2
}  // namespace pineforge::native_calendar
