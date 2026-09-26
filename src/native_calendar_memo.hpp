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
//     "the last day resolved holds this instant": where the cycles do not
//     tile (America/Sitka and America/Juneau in October 1867 under a
//     "2330-2300" session, measured on macOS) that shortcut answers another
//     session day than the memo-free lookup does;
//   * an interval is a pure function of the calendar value, the instant and
//     each timeframe's unit and count, so the last few answered are kept;
//   * whether the calendar is valid, and whether its zone is UTC, is decided
//     once per memo.
//
// All of it rests on the resolution being a function of its inputs, which
// the memo-free calendar assumes too. It is, except where glibc's mktime
// resolves a civil time inside a fold that keeps tm_isdst: glibc starts from
// the previous call's offset, so there the memo-free answer itself depends on
// what was asked before (America/Sitka 1867-10-18, Pacific/Kwajalein
// 1969-09-30), and a memo keeps the first.
//
// A memo serves one calendar value. The owner calls reset() whenever the
// calendar it passes is rebuilt or reassigned; a memo handed a calendar at
// another address forgets everything first (the address is identity only,
// never dereferenced). It holds a bounded number of days (kDaySlots),
// intervals and civil dates, and is not thread-safe: one owner, one thread,
// like the calendar value it serves.

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
    State& state(const SessionCalendar& calendar) {
        if (state_ && calendar_ == &calendar) return *state_;
        return bind(calendar);
    }

    // How many intervals the memo has resolved since it last bound a calendar
    // or was reset, as opposed to answered from the last few it holds: a cost
    // reading, derived like everything else here and folded nowhere.
    std::uint64_t interval_resolutions() const noexcept;

private:
    State& bind(const SessionCalendar& calendar);

    // Identity only, never dereferenced: the calendar the state belongs to.
    const SessionCalendar* calendar_ = nullptr;
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

// Whether `calendar`'s zone is UTC, as the memo decided once for it and the
// memo-free calendar decides at every resolution. A UTC calendar is integer
// arithmetic throughout (no libc zone, no mktime): its session days are the
// fixed daily cycles [date + origin, next date + origin), which tile time, so
// every instant it can represent has one session day, and every lookup finds it.
bool utc_calendar(const SessionCalendar& calendar, SessionDayMemo& memo);

}  // inline namespace native_calendar_v2
}  // namespace pineforge::native_calendar
