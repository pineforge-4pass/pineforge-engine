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
// never dereferenced). It holds a bounded number of days (kDayCapacity),
// intervals and civil dates, and is not thread-safe: one owner, one thread,
// like the calendar value it serves.
//
// What it owns does not depend on what it was asked (CR-150 lane L19n). The
// day slots are direct-mapped by the open date's day number: a day asked after
// kDayCapacity others replaces the one in its slot and is resolved again if it
// is asked again. That moves no answer. A lookup reads a day by address and
// copies what it keeps before it reads another, so none waits on a day being
// held, and a resolution is a function of the calendar value and the open date
// (up to the glibc fold above, where the memo keeps the first answer and a day
// evicted takes the one it resolves again). Every slot's span buffer is
// reserved once, when the memo binds its calendar, to the most spans a session
// day of that calendar has (session_day_span_bound), and is only assigned into
// after: the heap is the same after the first lookup as after the millionth,
// whatever days they asked (retained_days, retained_span_capacity).

#include <pineforge/native_calendar.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace pineforge::native_calendar {
inline namespace native_calendar_v2 {

class SessionDayMemo {
public:
    // Defined in native_calendar.cpp, the only translation unit that reads it.
    struct State;

    // The session days the memo keeps at once.
    static constexpr std::size_t kDayCapacity = 64;

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
    // How many times, since then, it read libc's local time for its calendar's
    // zone. The same kind of cost reading.
    std::uint64_t libc_local_reads() const noexcept;
    // How many session days it has resolved since then, as opposed to answered
    // from the days it keeps: a day asked again after kDayCapacity others is
    // resolved again. The same kind of cost reading.
    std::uint64_t day_resolutions() const noexcept;
    // Its footprint, for the tests that pin it: how many of its day slots hold
    // an answer now (a day, or that the calendar has none), and the capacity,
    // in spans, of their buffers together. The first never passes
    // kDayCapacity; the second is reserved when the calendar is bound and does
    // not move after it. Both are 0 for a memo that has bound no calendar.
    std::size_t retained_days() const noexcept;
    std::size_t retained_span_capacity() const noexcept;

private:
    State& bind(const SessionCalendar& calendar);

    // Identity only, never dereferenced: the calendar the state belongs to.
    const SessionCalendar* calendar_ = nullptr;
    std::unique_ptr<State> state_;
};

// An upper bound on the spans of one session day of `calendar`, a function of
// the calendar value alone: a day merges at most one span per declared window
// for each of the three civil dates its cycle reads (the date before the open
// date, the open date and the date after), or is the one span of an all-day
// calendar, and merging and resolving never add one. 0 for a calendar that is
// not valid, which resolves no day. The memo reserves it in every day slot, and
// the consumer in every certified cycle it keeps.
std::size_t session_day_span_bound(const SessionCalendar& calendar) noexcept;

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

// The session day holding `ms` as this memo's lookups resolve it -- its cycle
// [origin_ms, next_origin_ms), the ordinal its instants key to and its
// in-session spans -- when that cycle is CERTIFIED: every lookup of every
// instant of the cycle (session_day_at, interval_containing for fixed
// timeframes, in_session) is this day, no other day holds any of its
// instants, and the calendar resolves every civil time within 22 days of it
// the same way whatever it was asked before. A UTC or fixed-offset zone
// certifies every cycle; a TZif zone under glibc or macOS, every cycle whose
// reach its table covers with transitions that are small, far apart and never
// a backward move that keeps the daylight-saving flag, each read back from
// libc (native_calendar.cpp, "Certified session-day cycles"). nullopt
// otherwise -- the zone alone decided that, and nothing was resolved -- and
// when no session day holds `ms`.
struct CycleCertificate {
    int64_t origin_ms = 0;
    int64_t next_origin_ms = 0;
    int64_t ordinal = 0;
    std::vector<std::pair<int64_t, int64_t>> spans;
};
std::optional<CycleCertificate> cycle_certificate(const SessionCalendar& calendar,
                                                  int64_t ms,
                                                  SessionDayMemo& memo);

// The bucket interval_containing divides a session day into for the fixed
// timeframe `tf`, in ms; 0 for a timeframe that is not valid or not fixed.
int64_t fixed_bucket_ms(const Timeframe& tf);

}  // inline namespace native_calendar_v2
}  // namespace pineforge::native_calendar
