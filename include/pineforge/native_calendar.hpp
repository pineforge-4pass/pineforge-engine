#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pineforge::native_calendar {
inline namespace native_calendar_v2 {

// Isolated calendar / interval values for a later native host. This component
// does not match orders, emit strategy callbacks, aggregate OHLCV, or run a
// NativeRunSpec. Legacy helpers in session_time.cpp / timeframe.cpp stay
// unchanged and are not native authority.

// ---------------------------------------------------------------------------
// Timeframe
// ---------------------------------------------------------------------------
//
// Grammar: optional count (>= 1, no leading zeros) then a unit suffix.
//   nS  seconds     "15S"
//   n   minutes     "1", "5", "60", "240"
//   D / nD          "D", "1D", "2D"
//   W / nW          "W", "1W", "2W"
//   M / nM          "M", "1M", "3M"
// Literal is identity: "1D" is not rewritten to "D".
//
// Rejected before any conversion: empty, whitespace, leading zeros ("01",
// "01D"), bare "S", unknown suffix including "1H"/"4H" (legacy stoi("1H")==1
// minute is not copied), overflowed digit strings, count whose unit duration
// is not representable as int64 milliseconds.

enum class TimeframeUnit { Second, Minute, Day, Week, Month };

// Parsed timeframe. Default, moved-from, and any object that is not the
// result of a successful parse is invalid. Fields are immutable; there is
// no public constructor that can inject a zero count or a mismatched
// literal. Copy preserves validity; move leaves the source invalid.
class Timeframe {
public:
    Timeframe() = default;
    Timeframe(const Timeframe&) = default;
    Timeframe& operator=(const Timeframe&) = default;
    Timeframe(Timeframe&& other) noexcept { *this = std::move(other); }
    Timeframe& operator=(Timeframe&& other) noexcept {
        if (this == &other) return *this;
        unit_ = other.unit_;
        count_ = other.count_;
        literal_ = std::move(other.literal_);
        valid_ = other.valid_;
        other.valid_ = false;
        other.count_ = 0;
        other.literal_.clear();
        return *this;
    }

    bool valid() const noexcept { return valid_ && count_ >= 1 && unit_ok(unit_); }
    TimeframeUnit unit() const noexcept { return unit_; }
    int count() const noexcept { return count_; }
    const std::string& literal() const noexcept { return literal_; }
    bool is_fixed() const noexcept {
        return unit_ == TimeframeUnit::Second || unit_ == TimeframeUnit::Minute;
    }
    bool is_calendar() const noexcept { return valid() && !is_fixed(); }

private:
    friend std::optional<Timeframe> parse_timeframe(std::string_view);
    friend bool operator==(const Timeframe&, const Timeframe&) noexcept;
    static bool unit_ok(TimeframeUnit u) noexcept {
        switch (u) {
        case TimeframeUnit::Second:
        case TimeframeUnit::Minute:
        case TimeframeUnit::Day:
        case TimeframeUnit::Week:
        case TimeframeUnit::Month:
            return true;
        }
        return false;
    }
    Timeframe(TimeframeUnit unit, int count, std::string literal)
        : unit_(unit), count_(count), literal_(std::move(literal)), valid_(true) {}
    TimeframeUnit unit_ = TimeframeUnit::Minute;
    int count_ = 0;
    std::string literal_;
    bool valid_ = false;
};

inline bool operator==(const Timeframe& a, const Timeframe& b) noexcept {
    return a.valid() == b.valid() && a.unit_ == b.unit_ && a.count_ == b.count_
           && a.literal_ == b.literal_;
}

std::optional<Timeframe> parse_timeframe(std::string_view text);

// Pairing is a classification only. Equal-TF passthrough versus grouping is
// the host's choice; this component does not drop 2D/2W/M/3M and does not
// collapse count the way calendar_period_for(last letter) does.
enum class TimeframePairing {
    Passthrough,            // byte-identical literals
    SameUnitMultiple,       // same unit, script.count % input.count == 0, script >= input
    FixedDivisible,         // both fixed, script ms % input ms == 0 (e.g. 15S -> 1)
    FixedToCalendar,        // second/minute -> D/W/M
    CalendarToCalendar,     // D/nD -> W/M, W/nW -> M
    ScriptFiner,            // script strictly finer than input
    IndivisibleFixed,       // fixed remainder != 0 (5 -> 7) or same-unit non-multiple
    StreamMonthlyInputRefused,
    Invalid,                // one or both timeframes are not valid parsed values
};

struct TimeframeCompatibility {
    TimeframePairing pairing = TimeframePairing::Invalid;
    int64_t group_factor = 0;  // script/input count or ms ratio; 0 if not a ratio pairing
};

TimeframeCompatibility compatibility(const Timeframe& input, const Timeframe& script);

// Stream input M/nM is the existing refusal (engine_stream requires a fixed
// positive duration). Batch monthly remains accepted via compatibility().
TimeframeCompatibility stream_compatibility(const Timeframe& input, const Timeframe& script);

// ---------------------------------------------------------------------------
// Session / calendar definition
// ---------------------------------------------------------------------------
//
// Currently valid session syntax, parsed once into an immutable value:
//   empty or "24x7"                         all-day every day, origin 00:00
//   HHMM-HHMM                               one window; 2400 is a legal end
//   HHMM-HHMM,HHMM-HHMM                     comma-separated union
//   start == end                            24h wrap from that clock (1700-1700)
//   end < start                             overnight wrap (1800-1700, 2200-0200)
//   :1234567                                day mask, 1=Sun .. 7=Sat, after the windows
//
// Overnight / day-mask interpretation (generic; no stock/forex recognizer):
//   A wrapped or 24h-from-nonzero window is one session-day. The day mask
//   applies to that session-day's trading date — the local civil date of
//   last_traded_close_ms - 1ms — not to each timestamp's weekday. Sunday
//   18:00 of an 1800-1700 session with mask :23456 (Mon-Fri) is therefore
//   Monday's session, not a Sunday reject.
//
// Normalized schedule: the first declared start O fixes civil cycles
// [O(date), O(date+1)). Recurring instances of every window (including wrap
// and equal-start full-day) are built on that date and its neighbors,
// intersected with the cycle, clipped at both cycle ends, then unioned.
// A start earlier than O can contribute coverage on BOTH sides of the cycle
// boundary (overlapping 1300-1600,0900-1500); merely shifting every earlier
// start one day is not sufficient. That split is schedule normalization,
// not OHLC splitting. Origin stays the first declared start; permuting later
// windows cannot change the union, while changing the first window can
// change origin and trading-date assignment.
//
// Trading date is the civil date of the final nominal UNMASKED recurring
// coverage endpoint minus one instant, computed in civil minutes before
// weekday masks and before UTC DST-gap collapse. A rejected mask or empty
// resolved window never moves that date onto the next live cycle; there is
// no cycle-end-next-day fallback. Inverse lookup matches this date only, so
// an empty Sunday cannot alias Monday. Empty resolved/allowed coverage has
// no membership or synthesis; D/W/M bounds still contain the query
// (open <= query < next_period_open). A DST gap that maps both ends of one
// instance to the same epoch omits that instance only.
//
// Native origin is that first-window start. Pine's 17:00 day stamp on an
// 1800-1700 session is a compatibility label, not this component's default.
//
// Fold tie-break (civil -> epoch): a repeated local civil time maps to the
// earlier UTC occurrence (first time through). Adjacent intervals that share
// a civil boundary reuse that same epoch, so conversion cannot invent a gap
// or overlap. A missing local time (DST gap) maps to the first representable
// instant at or after the declared civil. Conversion does not copy tm_isdst
// from a query instant and does not use a UTC-day offset cache as authority.

class SessionCalendar;
std::optional<SessionCalendar> parse_session(std::string_view session,
                                             std::string_view timezone);

// Single calendar-owned timezone acceptance boundary. Empty is accepted and
// means UTC for this component (native full-spec nonempty scheduling TZ is
// separate). Also used by public resolve_civil. Not for hot interval loops.
//
// Accepted:
//   IANA/TZif names and aliases installed under libc's effective zoneinfo
//     root (not a search of trees libc will not read):
//     charset [A-Za-z0-9/_+-], no absolute path, no `.` / `..` components;
//     realpath stays in that root and the file magic is TZif. Leading `:`
//     is a tzfile reference only.
//     macOS: /var/db/timezone/zoneinfo (TZDIR ignored; tzset(3) does not
//     use it). glibc: nonempty TZDIR is exclusive (relative allowed;
//     missing/non-directory fails closed). Unset/empty TZDIR uses
//     /usr/share/zoneinfo.
//   UTC, GMT, Etc/UTC, Etc/GMT
//   UTC/GMT conventional offsets (TV sign):
//     (UTC|GMT)[+-](H|HH|HMM|HHMM|H:MM|HH:MM) hours 0–23, minutes 0–59;
//     digit length is bounded before conversion so overflow cannot escape.
//   POSIX TZ, including DST rule forms:
//     std offset [dst[offset][,start[/time],end[/time]]]
//     A slash before any comma is a tzfile path; slash after a comma is
//     POSIX rule time (M3.2.0/2), not an IANA name.
// Rejected before libc setenv/mktime: unknown names, UTC+24:00 / UTC+01:99,
// overflowed digit strings, whitespace/control/NUL, absolute paths, `..`
// traversal, and non-TZif zoneinfo files (zone.tab, iso3166.tab, +VERSION).
bool timezone_accepted(std::string_view timezone);

// Observational identity for the native runner (setup/identity time only).
// Shares the acceptance/normalization/effective-root resolver. Does not hash.
// Semantics version 1. nullopt = backing facts could not be established;
// the runner must refuse. Does not relax timezone_accepted.
enum class TimezoneSourceKind : std::uint32_t {
    Utc = 1,
    FixedOffset = 2,
    PosixExplicit = 3,
    PosixDefaultDst = 4,
    Tzfile = 5,
};

struct TimezoneIdentityDescriptor {
    static constexpr std::uint32_t kSemanticsVersion = 1;
    std::uint32_t semantics_version = 0;
    TimezoneSourceKind kind = TimezoneSourceKind::Utc;
    std::string input;
    std::string effective_definition;
    std::string zoneinfo_root;
    std::vector<std::string> resource_paths;
    bool valid() const noexcept;
};

std::optional<TimezoneIdentityDescriptor>
timezone_identity_descriptor(std::string_view timezone);

class SessionWindow {
public:
    SessionWindow() = default;
    bool valid() const noexcept {
        if (start_minutes_ < 0 || start_minutes_ >= 1440) return false;
        if (end_minutes_ < 0 || end_minutes_ > 1440) return false;
        const bool expect_full = (end_minutes_ != 1440 && start_minutes_ == end_minutes_);
        const bool expect_wrap = (!expect_full && end_minutes_ < start_minutes_);
        return full_day_ == expect_full && wraps_ == expect_wrap;
    }
    int start_minutes() const noexcept { return start_minutes_; }
    int end_minutes() const noexcept { return end_minutes_; }
    bool wraps() const noexcept { return wraps_; }
    bool full_day() const noexcept { return full_day_; }

private:
    friend std::optional<SessionCalendar> parse_session(std::string_view, std::string_view);
    SessionWindow(int start, int end, bool wraps, bool full_day)
        : start_minutes_(start), end_minutes_(end), wraps_(wraps), full_day_(full_day) {}
    int start_minutes_ = 0;  // [0, 1440)
    int end_minutes_ = 0;    // (0, 1440]; 1440 = 24:00
    bool wraps_ = false;
    bool full_day_ = false;
};

// Parsed session. Default and moved-from objects are invalid. Windows, mask,
// origin and flags are immutable after a successful parse. 2400 is stored as
// end_minutes 1440; cyclic coverage is computed internally and is not a
// public field that can be set to 25:00.
class SessionCalendar {
public:
    SessionCalendar() = default;
    SessionCalendar(const SessionCalendar&) = default;
    SessionCalendar& operator=(const SessionCalendar&) = default;
    SessionCalendar(SessionCalendar&& other) noexcept { *this = std::move(other); }
    SessionCalendar& operator=(SessionCalendar&& other) noexcept {
        if (this == &other) return *this;
        timezone_ = std::move(other.timezone_);
        literal_ = std::move(other.literal_);
        windows_ = std::move(other.windows_);
        day_mask_ = other.day_mask_;
        origin_minutes_ = other.origin_minutes_;
        all_day_ = other.all_day_;
        valid_ = other.valid_;
        other.valid_ = false;
        other.day_mask_ = 0;
        other.origin_minutes_ = 0;
        other.all_day_ = false;
        other.timezone_.clear();
        other.literal_.clear();
        other.windows_.clear();
        return *this;
    }

    bool valid() const noexcept;
    const std::string& timezone() const noexcept { return timezone_; }
    const std::string& literal() const noexcept { return literal_; }
    const std::vector<SessionWindow>& windows() const noexcept { return windows_; }
    std::uint8_t day_mask() const noexcept { return day_mask_; }
    int origin_minutes() const noexcept { return origin_minutes_; }
    bool all_day() const noexcept { return all_day_; }

private:
    friend std::optional<SessionCalendar> parse_session(std::string_view, std::string_view);
    SessionCalendar(std::string timezone,
                    std::string literal,
                    std::vector<SessionWindow> windows,
                    std::uint8_t day_mask,
                    int origin_minutes,
                    bool all_day)
        : timezone_(std::move(timezone)),
          literal_(std::move(literal)),
          windows_(std::move(windows)),
          day_mask_(day_mask),
          origin_minutes_(origin_minutes),
          all_day_(all_day),
          valid_(true) {}
    std::string timezone_;
    std::string literal_;
    std::vector<SessionWindow> windows_;
    std::uint8_t day_mask_ = 0;
    int origin_minutes_ = 0;
    bool all_day_ = false;
    bool valid_ = false;
};

std::optional<SessionCalendar> parse_session(std::string_view session,
                                             std::string_view timezone);

bool in_session(const SessionCalendar& calendar, int64_t ms);

// ---------------------------------------------------------------------------
// Civil conversion
// ---------------------------------------------------------------------------

enum class CivilKind { Unique, Gap, Fold };

struct CivilResolution {
    int64_t epoch_ms = 0;
    CivilKind kind = CivilKind::Unique;
};

// Resolve (timezone, civil) to a Unix-ms epoch. Invalid calendar dates and
// out-of-range fields return nullopt. Fold -> earlier UTC; gap -> first
// representable instant >= declared. Independent of prior queries.
std::optional<CivilResolution> resolve_civil(std::string_view timezone,
                                             int year,
                                             int month,
                                             int day,
                                             int hour,
                                             int minute,
                                             int second = 0);

// ---------------------------------------------------------------------------
// Period keys (stable, independent of the first supplied sample)
// ---------------------------------------------------------------------------
//
// Session-day ordinal: Howard Hinnant days_from_civil of the session-day's
// trading date. Day 0 is 1970-01-01. Dates before that are negative; nD uses
// floor division toward -inf: key = floor(ordinal / n) * n.
//
// Session-week origin: Monday 1969-12-29, the Monday of the week containing
// 1970-01-01. Week index 0 is that week. nW key = floor(week_index / n) * n.
// An overnight session whose trading date is Monday opens at the previous
// civil day's first-window start (Sunday 18:00 for 1800-1700) — derived from
// the windows, not from a forex special case.
//
// Session-month ordinal: year * 12 + (month - 1) of the trading date.
// 3M groups Jan-Mar, Apr-Jun, Jul-Sep, Oct-Dec (January remainder 0).
// Dates before year 0 continue with floor division toward -inf.
//
// Incomplete leading input does not move these anchors.

std::optional<int64_t> session_day_ordinal(const SessionCalendar& calendar, int64_t ms);
std::optional<int64_t> session_week_ordinal(const SessionCalendar& calendar, int64_t ms);
std::optional<int64_t> session_month_ordinal(const SessionCalendar& calendar, int64_t ms);
std::optional<int64_t> period_key(const SessionCalendar& calendar, const Timeframe& tf, int64_t ms);

// ---------------------------------------------------------------------------
// Interval
// ---------------------------------------------------------------------------
//
// open_ms                  nominal origin of this interval (session origin or
//                          fixed-grid anchor from the declared first-window
//                          origin). A split-session reopen can fall after this
//                          anchor (hourly grid 12:30 vs 13:00 lunch reopen).
// eligible_open_ms         first in-session instant of this interval; equal to
//                          open_ms when the nominal anchor is itself eligible.
// last_traded_close_ms     exclusive end of trading in this interval
// next_period_open_ms      nominal open of the next interval of this (unit,
//                          count), including a closed grid slot
// next_input_open_ms       next actual eligible input opening at or after
//                          last_traded_close_ms; skips declared closed time
//                          (13:00, not a closed 12:30). Not a resolution-
//                          stepped cursor; it jumps to the next normalized
//                          span. Off-session observed ticks are a later host
//                          concern and are not forced through this field.
//
// RTH Friday 16:00 last-traded close is not Monday 09:30. Those are separate
// fields. Fixed buckets add elapsed UTC seconds/minutes from the session-day
// origin and clip to the union of windows. Calendar periods use civil
// session-day / week / month boundaries and every session window.
//
// A timestamp in a declared closed gap still belongs to the period whose
// [open, next period open) covers it; last_traded_close may then precede it.

struct NativeInterval {
    int64_t open_ms = 0;
    int64_t eligible_open_ms = 0;
    int64_t last_traded_close_ms = 0;
    int64_t next_period_open_ms = 0;
    int64_t next_input_open_ms = 0;
};

// Uses tf as both the period unit and the input unit.
std::optional<NativeInterval> interval_containing(const SessionCalendar& calendar,
                                                  const Timeframe& tf,
                                                  int64_t ms);

std::optional<NativeInterval> interval_containing(const SessionCalendar& calendar,
                                                  const Timeframe& script_tf,
                                                  const Timeframe& input_tf,
                                                  int64_t ms);

}  // inline namespace native_calendar_v2
}  // namespace pineforge::native_calendar
