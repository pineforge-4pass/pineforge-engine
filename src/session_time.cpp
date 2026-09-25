#include <pineforge/session_time.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/timeframe.hpp>
#include "timezone.hpp"
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_set>
#include <vector>

namespace pineforge {

// =========================================================================
// Public helpers (exposed via session_time.hpp)
// =========================================================================

int hhmm_to_minutes(const std::string& hhmm) {
    if (hhmm.size() < 4)
        return -1;
    int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    int m = (hhmm[2] - '0') * 10 + (hhmm[3] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    return h * 60 + m;
}

namespace {

// Calendar fields by integer arithmetic (Howard Hinnant's civil_from_days on
// the floor day) for every second within +/-2^40 of the epoch, about 34,800
// years each way; libc answers the rest.
constexpr int64_t kCivilSpan = int64_t{1} << 40;

bool in_civil_span(int64_t secs) {
    return secs > -kCivilSpan && secs < kCivilSpan;
}

// gmtime_r's nine ISO fields of `secs` (in_civil_span only): the date is
// native_calendar::native_civil_date of the floor day.
void civil_fields(int64_t secs, struct tm& out) {
    const int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
    const int64_t second_of_day = secs - days * 86400;
    const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(days);
    out.tm_sec = static_cast<int>(second_of_day % 60);
    out.tm_min = static_cast<int>(second_of_day / 60 % 60);
    out.tm_hour = static_cast<int>(second_of_day / 3600);
    out.tm_mday = date.day;
    out.tm_mon = date.month - 1;
    out.tm_year = static_cast<int>(date.year - 1900);
    out.tm_wday = static_cast<int>(((days + 4) % 7 + 7) % 7);  // 1970-01-01: Thursday
    out.tm_yday = static_cast<int>(days - native_calendar::native_civil_days(date.year, 1, 1));
    out.tm_isdst = 0;
}

bool same_civil_fields(const struct tm& a, const struct tm& b) {
    return a.tm_sec == b.tm_sec && a.tm_min == b.tm_min && a.tm_hour == b.tm_hour
        && a.tm_mday == b.tm_mday && a.tm_mon == b.tm_mon && a.tm_year == b.tm_year
        && a.tm_wday == b.tm_wday && a.tm_yday == b.tm_yday;
}

// Days from 1970-01-01 to (year, month 1-12, day): civil_fields' inverse.
int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    return native_calendar::native_civil_days(year, static_cast<int>(month), static_cast<int>(day));
}

// The zone name gmtime_r reports ("UTC" on macOS, "GMT" on glibc): libc's own
// static text, read once. An idempotent atomic cache, not a guarded static:
// every thread would store the same pointer, and the guard is what a
// sanitizer build pays for per call.
const char* utc_zone_name() {
    static std::atomic<const char*> cached{nullptr};
    const char* name = cached.load(std::memory_order_acquire);
    if (name == nullptr) {
        struct tm probe {};
        const time_t zero = 0;
        gmtime_r(&zero, &probe);
        name = probe.tm_zone;
        cached.store(name, std::memory_order_release);
    }
    return name;
}

// gmtime_r, every field.
void utc_fields(time_t secs, struct tm& out) {
    if (!in_civil_span(static_cast<int64_t>(secs))) {
        gmtime_r(&secs, &out);
        return;
    }
    civil_fields(static_cast<int64_t>(secs), out);
    out.tm_gmtoff = 0;
    out.tm_zone = const_cast<char*>(utc_zone_name());
}

// A zone's offset intervals, memoised per thread: [lo, hi] is a span of
// seconds over which localtime_r gave one (tm_gmtoff, tm_isdst, tm_zone), and
// the fields of any second in it are the civil fields of that second plus
// tm_gmtoff. Read with no libc call, no process lock and no TZ switch while a
// stamp stays inside; every edge is a localtime_r answer under the zone's
// ScopedTimezone, taken while answering a stamp outside every interval.
//
// A stamp outside costs what it always did, one localtime_r, and opens a
// one-second interval. A stamp that lands within kBridgeSteps probe gaps of an
// interval in the same state joins it -- a run reading bar after bar, a daily
// bar included -- once localtime_r confirms that state every kProbeGap across
// the gap, and the interval then reaches up to kReachSteps gaps further in the
// direction the reads moved, stopping at the exact transition second (bisected)
// when a probe reads another state. So localtime_r was asked about a second at
// most kProbeGap from any second an interval holds, and an interval can only
// miss a pair of transitions closer together than that which restore the same
// offset, flag and abbreviation: the assumption tzcode's own zdump makes when it
// samples localtime at twelve-hour intervals to find transitions (zdump(8),
// LIMITATIONS). The shortest gap between two transitions of one zone in the tz
// database is days (6.9 over 1970-2100 in 2026c). A zone whose libc fields are
// not civil-plus-offset (leap-second "right/" zones) is never memoised:
// localtime_r answers each of its calls. A thread holds kZoneSlots intervals,
// the least recently read one giving way.
constexpr int64_t kProbeGap = 12 * 3600;
constexpr int kBridgeSteps = 4;   // a gap of up to 48 h is bridged
constexpr int kReachSteps = 60;   // an interval grows by up to 30 days per miss
constexpr int kZoneSlots = 8;

struct ZoneInterval {
    char zone[48];           // the caller's spelling (zone_size bytes)
    std::size_t zone_size;   // 0: slot unused
    bool utc;                // normalize_timezone_for_posix(zone) is UTC
    int64_t lo, hi;          // lo > hi: no interval
    long gmtoff;
    int isdst;
    char abbr[16];
    std::uint64_t last_read;
};

thread_local ZoneInterval tl_zones[kZoneSlots] = {};
thread_local std::uint64_t tl_zone_clock = 0;

bool plain_utc_spelling(const std::string& tz) {
    return tz.empty() || tz == "UTC" || tz == "Etc/UTC" || tz == "GMT" || tz == "Etc/GMT";
}

bool holds_zone(const ZoneInterval& slot, const std::string& tz) {
    return slot.zone_size != 0 && slot.zone_size == tz.size()
        && std::memcmp(slot.zone, tz.data(), tz.size()) == 0;
}

bool normalizes_to_utc(const std::string& tz) {
    const std::string t = normalize_timezone_for_posix(tz);
    return t.empty() || t == "UTC" || t == "Etc/UTC";
}

// Whether `tz` reads as UTC (its POSIX normalization is "UTC"), asked of the
// memo for a spelling it holds.
bool utc_zone(const std::string& tz) {
    if (plain_utc_spelling(tz)) return true;
    for (const ZoneInterval& slot : tl_zones) {
        if (holds_zone(slot, tz)) return slot.utc;
    }
    return normalizes_to_utc(tz);
}

ZoneInterval& claim_zone_slot(const std::string& tz, bool utc) {
    ZoneInterval* slot = &tl_zones[0];
    for (ZoneInterval& candidate : tl_zones) {
        if (candidate.zone_size == 0) {
            slot = &candidate;
            break;
        }
        if (candidate.last_read < slot->last_read) slot = &candidate;
    }
    std::memcpy(slot->zone, tz.data(), tz.size());
    slot->zone_size = tz.size();
    slot->utc = utc;
    slot->lo = 1;
    slot->hi = 0;
    slot->last_read = ++tl_zone_clock;
    return *slot;
}

bool same_state(const struct tm& t, long gmtoff, int isdst, const char* abbr) {
    return t.tm_zone != nullptr && t.tm_gmtoff == gmtoff && t.tm_isdst == isdst
        && std::strcmp(t.tm_zone, abbr) == 0;
}

// localtime_r at `secs` reads the slot's state, and the civil fields of `secs`
// plus its offset are libc's. Under the zone's ScopedTimezone only.
bool state_holds(const ZoneInterval& slot, int64_t secs) {
    const time_t t = static_cast<time_t>(secs);
    struct tm probe {};
    if (localtime_r(&t, &probe) == nullptr
        || !same_state(probe, slot.gmtoff, slot.isdst, slot.abbr)) {
        return false;
    }
    struct tm civil {};
    civil_fields(secs + slot.gmtoff, civil);
    return same_civil_fields(civil, probe);
}

// Every kProbeGap strictly between two seconds already in the slot's state
// (`from` < `to`, at most kBridgeSteps gaps apart) reads that state too.
bool bridges(const ZoneInterval& slot, int64_t from, int64_t to) {
    for (int64_t probe = from + kProbeGap; probe < to; probe += kProbeGap) {
        if (!state_holds(slot, probe)) return false;
    }
    return true;
}

// The farthest second from `from` (in the slot's state) in `direction` (+1 or
// -1), at most kReachSteps gaps on, that is still in it: the last probe that
// read the state, or the transition edge bisected after it.
int64_t reach(const ZoneInterval& slot, int64_t from, int64_t direction) {
    int64_t inside = from;
    for (int step = 0; step < kReachSteps; ++step) {
        int64_t outside = inside + direction * kProbeGap;
        if (state_holds(slot, outside)) {
            inside = outside;
            continue;
        }
        while (inside - outside > 1 || outside - inside > 1) {
            const int64_t mid = inside + (outside - inside) / 2;
            if (state_holds(slot, mid))
                inside = mid;
            else
                outside = mid;
        }
        break;
    }
    return inside;
}

}  // namespace

// Decompose a Unix-ms timestamp into local calendar fields for `tz` -- every
// field gmtime_r (UTC) or localtime_r under the zone's ScopedTimezone gives --
// WITHOUT per-call libc on the hot path (hour()/minute()/... each bar). A zone
// that normalizes to UTC is civil arithmetic (utc_fields). Another zone reads
// the thread's offset intervals while the stamp is inside one, and asks
// localtime_r under the CACHED tz_util::ScopedTimezone only for a stamp outside
// them, so a run pays neither the process lock nor a tzset per call: each
// changed-TZ tzset() on macOS is a notifyd Mach-IPC round trip (~173us; ~888k/run
// over the full feed -> minutes of apparent "hang", KI-35). DST stays exact:
// every interval edge is a localtime_r answer. External linkage only so
// tests/test_local_time_fields.cpp can compare it with libc field by field; not
// part of the installed API.
void decompose_ms_local(int64_t bar_ms, const std::string& tz, struct tm& out) {
    const time_t secs = static_cast<time_t>(bar_ms / 1000);
    if (plain_utc_spelling(tz)) {
        utc_fields(secs, out);
        return;
    }
    const int64_t at = static_cast<int64_t>(secs);
    bool held = false;
    bool utc = false;
    for (ZoneInterval& slot : tl_zones) {
        if (!holds_zone(slot, tz)) continue;
        held = true;
        utc = slot.utc;
        if (utc) {
            slot.last_read = ++tl_zone_clock;
            break;
        }
        if (slot.lo <= at && at <= slot.hi) {
            slot.last_read = ++tl_zone_clock;
            civil_fields(at + slot.gmtoff, out);
            out.tm_isdst = slot.isdst;
            out.tm_gmtoff = slot.gmtoff;
            out.tm_zone = slot.abbr;
            return;
        }
    }
    if (!held) {
        // Detect UTC on the NORMALIZED string, but hand the RAW tz to
        // ScopedTimezone — it normalizes internally (timezone.cpp), so passing
        // an already-normalized string double-normalizes and flips the sign of
        // fixed offsets ("UTC+2" -> "UTC-2" -> "UTC+2" => wrong hour).
        utc = normalizes_to_utc(tz);
        if (utc && tz.size() <= sizeof(ZoneInterval::zone)) claim_zone_slot(tz, true);
    }
    if (utc) {
        utc_fields(secs, out);
        return;
    }
    tz_util::ScopedTimezone guard(tz);
    if (localtime_r(&secs, &out) == nullptr || out.tm_zone == nullptr) return;
    const int64_t margin = (kBridgeSteps + kReachSteps + 1) * kProbeGap + 2 * 86400;
    if (tz.size() > sizeof(ZoneInterval::zone) || !in_civil_span(at - margin)
        || !in_civil_span(at + margin)
        || std::strlen(out.tm_zone) >= sizeof(ZoneInterval::abbr)) {
        return;
    }
    struct tm civil {};
    civil_fields(at + out.tm_gmtoff, civil);
    if (!same_civil_fields(civil, out)) return;
    const int64_t bridge = kBridgeSteps * kProbeGap;
    for (ZoneInterval& slot : tl_zones) {
        if (!holds_zone(slot, tz) || slot.lo > slot.hi
            || !same_state(out, slot.gmtoff, slot.isdst, slot.abbr)) {
            continue;
        }
        if (at > slot.hi && at - slot.hi <= bridge && bridges(slot, slot.hi, at)) {
            slot.hi = reach(slot, at, +1);
            slot.last_read = ++tl_zone_clock;
            return;
        }
        if (at < slot.lo && slot.lo - at <= bridge && bridges(slot, at, slot.lo)) {
            slot.lo = reach(slot, at, -1);
            slot.last_read = ++tl_zone_clock;
            return;
        }
    }
    ZoneInterval& slot = claim_zone_slot(tz, false);
    slot.gmtoff = out.tm_gmtoff;
    slot.isdst = out.tm_isdst;
    std::strcpy(slot.abbr, out.tm_zone);
    slot.lo = at;
    slot.hi = at;
}

// Pine time/date extraction — value-identical to the codegen's former inline
// setenv+tzset lambda (pineforge-codegen tables.py TIME_FIELD_EXPRS) but
// churn-free. hour()/minute()/... route through these instead of open-coding it.
int local_hour(int64_t bar_ms, const std::string& tz)       { struct tm t; decompose_ms_local(bar_ms, tz, t); return t.tm_hour; }
int local_minute(int64_t bar_ms, const std::string& tz)     { struct tm t; decompose_ms_local(bar_ms, tz, t); return t.tm_min; }
int local_second(int64_t bar_ms, const std::string& tz)     { struct tm t; decompose_ms_local(bar_ms, tz, t); return t.tm_sec; }
int local_dayofmonth(int64_t bar_ms, const std::string& tz) { struct tm t; decompose_ms_local(bar_ms, tz, t); return t.tm_mday; }
int local_dayofweek(int64_t bar_ms, const std::string& tz)  { struct tm t; decompose_ms_local(bar_ms, tz, t); return t.tm_wday + 1; }
int local_month(int64_t bar_ms, const std::string& tz)      { struct tm t; decompose_ms_local(bar_ms, tz, t); return t.tm_mon + 1; }
int local_year(int64_t bar_ms, const std::string& tz)       { struct tm t; decompose_ms_local(bar_ms, tz, t); return t.tm_year + 1900; }
int local_weekofyear(int64_t bar_ms, const std::string& tz) { struct tm t; decompose_ms_local(bar_ms, tz, t); return (t.tm_yday + 7 - ((t.tm_wday + 6) % 7)) / 7; }

static int64_t calendar_day_open_local_ms_tz(int64_t bar_ms, const std::string& tz);

int64_t calendar_day_open_local_ms(int64_t bar_ms, const std::string& tz) {
    // UTC needs no tzset: local midnight is exact integer floor. Avoiding
    // ScopedTimezone(UTC) here keeps the process TZ from flipping to UTC every
    // bar (which would re-slow the strategy's hour()/minute() zone) — see KI-35.
    if (utc_zone(tz)) {
        time_t secs = static_cast<time_t>(bar_ms / 1000);
        return static_cast<int64_t>((secs / 86400) * 86400) * 1000;
    }
    return calendar_day_open_local_ms_tz(bar_ms, tz);
}

static int64_t calendar_day_open_local_ms_tz(int64_t bar_ms, const std::string& tz) {
    tz_util::ScopedTimezone guard(tz);
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);
    local_tm.tm_hour = 0;
    local_tm.tm_min  = 0;
    local_tm.tm_sec  = 0;
    // DST edge-case: on spring-forward days in certain timezones (e.g.,
    // America/Havana, Pacific/Lord_Howe), midnight is non-existent and
    // mktime() returns -1. Retry with +1 h and +2 h until we obtain a
    // valid epoch. This gives the first representable second of the day.
    time_t day0 = mktime(&local_tm);
    if (day0 == static_cast<time_t>(-1)) {
        std::fprintf(stderr,
            "[pineforge] WARNING: mktime() returned -1 for midnight in tz='%s' "
            "(DST gap?). Falling back to midnight+1h.\n",
            tz.c_str());
        local_tm.tm_hour = 1;
        day0 = mktime(&local_tm);
        if (day0 == static_cast<time_t>(-1)) {
            local_tm.tm_hour = 2;
            day0 = mktime(&local_tm);
        }
        if (day0 != static_cast<time_t>(-1)) {
            // Snap back to the hour boundary we actually used (already set above).
        }
    }
    return static_cast<int64_t>(day0) * 1000;
}

// =========================================================================
// File-private helpers (anonymous namespace — file-local only)
// =========================================================================


namespace {

static void trim_inplace(std::string& s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
}

// Suffix :1234567 — digits 1–7 are Sun–Sat (TradingView session weekdays)
static void parse_day_filter(const std::string& session_in,
                             std::string& windows_out,
                             std::unordered_set<int>* days_out) {
    windows_out = session_in;
    if (days_out)
        days_out->clear();
    if (session_in.empty())
        return;

    std::size_t colon = session_in.rfind(':');
    if (colon == std::string::npos || colon + 1 >= session_in.size())
        return;

    bool all_digits = true;
    for (std::size_t i = colon + 1; i < session_in.size(); ++i) {
        char c = session_in[i];
        if (c < '1' || c > '7') {
            all_digits = false;
            break;
        }
    }
    if (!all_digits)
        return;

    windows_out = session_in.substr(0, colon);
    trim_inplace(windows_out);
    if (days_out) {
        for (std::size_t i = colon + 1; i < session_in.size(); ++i)
            days_out->insert(session_in[i] - '0');
    }
}

static int64_t calendar_week_open_local_ms_tz(int64_t bar_ms, const std::string& tz);

static int64_t calendar_week_open_local_ms(int64_t bar_ms, const std::string& tz) {
    // UTC needs no tzset: the Monday-00:00 week floor is exact integer
    // arithmetic on the epoch day number (1970-01-01 = day 0 was a THURSDAY,
    // tm_wday 4; the slow path opens weeks on Monday via (tm_wday + 6) % 7).
    // Avoiding ScopedTimezone(UTC) here keeps the process TZ from flipping to
    // UTC every bar (which would re-slow the strategy's hour()/minute() zone)
    // — see KI-35: time("W") under UTC alternating with hour(time, tz) defeats
    // the single-slot g_active_tz cache and pays a tzset->notifyd round trip
    // per bar.
    if (utc_zone(tz)) {
        time_t secs = static_cast<time_t>(bar_ms / 1000);
        int64_t days = static_cast<int64_t>(secs) / 86400;
        if (secs % 86400 != 0 && secs < 0)
            --days;  // floor toward -inf (pre-1970 bars)
        int wday = static_cast<int>(((days + 4) % 7 + 7) % 7);  // 0=Sun, == tm_wday
        int days_from_mon = (wday + 6) % 7;
        return (days - days_from_mon) * 86400000LL;
    }
    return calendar_week_open_local_ms_tz(bar_ms, tz);
}

static int64_t calendar_week_open_local_ms_tz(int64_t bar_ms, const std::string& tz) {
    tz_util::ScopedTimezone guard(tz);
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);
    int wday = local_tm.tm_wday;  // 0=Sun
    int days_from_mon = (wday + 6) % 7;
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    local_tm.tm_mday -= days_from_mon;
    time_t wk0 = mktime(&local_tm);
    return static_cast<int64_t>(wk0) * 1000;
}

static int64_t calendar_month_open_local_ms_tz(int64_t bar_ms, const std::string& tz);

static int64_t calendar_month_open_local_ms(int64_t bar_ms, const std::string& tz) {
    // UTC needs no tzset: gmtime_r's day-of-month (utc_fields), and every UTC
    // day is exactly 86400 s, so the month open is the day floor minus
    // (tm_mday - 1) days. Avoiding ScopedTimezone(UTC) here keeps the process
    // TZ from flipping to UTC every bar (which would re-slow the strategy's
    // hour()/minute() zone) — see KI-35.
    if (utc_zone(tz)) {
        time_t secs = static_cast<time_t>(bar_ms / 1000);
        int64_t days = static_cast<int64_t>(secs) / 86400;
        if (secs % 86400 != 0 && secs < 0)
            --days;  // floor toward -inf (pre-1970 bars)
        struct tm g {};
        utc_fields(secs, g);
        return (days - (g.tm_mday - 1)) * 86400000LL;
    }
    return calendar_month_open_local_ms_tz(bar_ms, tz);
}

static int64_t calendar_month_open_local_ms_tz(int64_t bar_ms, const std::string& tz) {
    tz_util::ScopedTimezone guard(tz);
    time_t secs = static_cast<time_t>(bar_ms / 1000);
    struct tm local_tm {};
    localtime_r(&secs, &local_tm);
    local_tm.tm_mday = 1;
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    time_t m0 = mktime(&local_tm);
    return static_cast<int64_t>(m0) * 1000;
}

// D/W/M open on the `tz` calendar; an intraday tf on the SYMBOL's
// day-stamp-anchored HTF grid (session_intraday_bucket_open_ms — the grid
// request.security aggregates on), which with sym_tz "UTC" and no
// sym_session is the epoch grid the five-argument forms keep.
static int64_t compute_tf_open_ms(int64_t bar_ms,
                                  const std::string& tf,
                                  const std::string& tz,
                                  const std::string& sym_tz,
                                  const std::string& sym_session) {
    if (tf.empty())
        return bar_ms;

    CalendarPeriod cp = calendar_period_for(tf);
    if (cp == CalendarPeriod::DAY)
        return calendar_day_open_local_ms(bar_ms, tz);
    if (cp == CalendarPeriod::WEEK)
        return calendar_week_open_local_ms(bar_ms, tz);
    if (cp == CalendarPeriod::MONTH)
        return calendar_month_open_local_ms(bar_ms, tz);

    int sec = tf_to_seconds(tf);
    if (sec > 0 && sec < kSecPerDay)
        return session_intraday_bucket_open_ms(bar_ms, sec, sym_tz, sym_session);
    return bar_ms;
}

static int64_t compute_tf_close_ms(int64_t open_ms,
                                   const std::string& tf,
                                   const std::string& tz) {
    CalendarPeriod cp = calendar_period_for(tf);
    int sec = tf_to_seconds(tf);

    if (sec > 0 && sec < kSecPerDay) {
        // Intraday time_close is the EXACT bar-close boundary (== next bar's
        // open = open_ms + duration), NOT the last millisecond of the bar. TV's
        // time_close for a 15:15 15m bar is 15:30:00.000, so minute(time_close)
        // == 30 (verified against vasudevshenoy-manoj-betrayed-me, whose
        // `minute(time_close) >= 30` intraday session-close flatten fires 222
        // times in TV; the prior `- 1` gave 15:29:59.999 -> minute 29 -> the
        // flatten never fired). The calendar DAY/WEEK/MONTH branches below keep
        // their `- 1` (period-END = last ms of the period, per their TV-boundary
        // tests) because those are distinct semantics from an intraday close.
        return open_ms + static_cast<int64_t>(sec) * 1000;
    }

    time_t osec = static_cast<time_t>(open_ms / 1000);
    if (cp != CalendarPeriod::NONE && utc_zone(tz) && in_civil_span(static_cast<int64_t>(osec))) {
        // What mktime answers below under TZ=UTC, where it is civil arithmetic:
        // the next period's 00:00 (the next day, seven days on, the first of the
        // next month), without switching the process TZ to UTC (KI-35).
        const int64_t days = static_cast<int64_t>(osec) / 86400
            - (static_cast<int64_t>(osec) % 86400 < 0 ? 1 : 0);
        int64_t next = days + (cp == CalendarPeriod::DAY ? 1 : 7);
        if (cp == CalendarPeriod::MONTH) {
            struct tm fields {};
            civil_fields(static_cast<int64_t>(osec), fields);
            next = fields.tm_mon == 11
                ? days_from_civil(fields.tm_year + 1900 + 1, 1, 1)
                : days_from_civil(fields.tm_year + 1900, fields.tm_mon + 2, 1);
        }
        return next * 86400 * 1000 - 1;
    }

    tz_util::ScopedTimezone guard(tz);
    struct tm local_tm {};
    localtime_r(&osec, &local_tm);

    if (cp == CalendarPeriod::DAY) {
        local_tm.tm_mday += 1;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000 - 1;
    }
    if (cp == CalendarPeriod::WEEK) {
        local_tm.tm_mday += 7;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000 - 1;
    }
    if (cp == CalendarPeriod::MONTH) {
        local_tm.tm_mon += 1;
        local_tm.tm_mday = 1;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000 - 1;
    }
    return open_ms;
}

// Parse the first session-start time from a session string such as
// "0930-1600" or "0930-1600,1700-2000". Returns -1 on failure.
static int parse_session_start_minutes(const std::string& session) {
    if (session.empty() || session == "24x7")
        return -1;
    // Strip day-of-week suffix (:23456 style)
    std::string windows;
    parse_day_filter(session, windows, nullptr);
    trim_inplace(windows);
    if (windows.empty())
        return -1;
    // First window is everything before the first comma
    std::size_t comma = windows.find(',');
    std::string first = (comma == std::string::npos) ? windows
                                                     : windows.substr(0, comma);
    trim_inplace(first);
    // First 4 chars are HHMM start
    if (first.size() < 4)
        return -1;
    int start_m = hhmm_to_minutes(first.substr(0, 4));
    return start_m;
}

// Detect 24/7 session: empty string, "24x7", or start "0000" with end
// "2400" or "0000".
static bool is_allday_session(const std::string& session) {
    if (session.empty() || session == "24x7")
        return true;
    // Check if session string normalises to "0000-2400"
    std::string windows;
    parse_day_filter(session, windows, nullptr);
    trim_inplace(windows);
    if (windows.empty())
        return true;
    // Check first window
    std::size_t dash = windows.find('-');
    if (dash == std::string::npos || dash < 4)
        return false;
    std::string start4 = windows.substr(0, 4);
    std::string end4   = windows.substr(dash + 1, 4);
    return (start4 == "0000" && (end4 == "2400" || end4 == "0000"));
}

// True when the first "HHMM-HHMM" window of `windows` has start == end —
// TradingView's spelling of a 24-hour session ("1700-1700" on OANDA forex,
// "0000-0000"). Such a window spans the whole day, not zero minutes.
static bool first_window_is_full_day(const std::string& windows) {
    std::size_t dash = windows.find('-');
    if (dash == std::string::npos || dash < 4 || windows.size() < dash + 5)
        return false;
    int sm = hhmm_to_minutes(windows.substr(dash - 4, 4));
    int em = hhmm_to_minutes(windows.substr(dash + 1, 4));
    return sm >= 0 && em >= 0 && sm == em;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public session predicate helpers (exposed via session_time.hpp)
// (hhmm_to_minutes already defined at top of file from G1's promotion)
// ---------------------------------------------------------------------------

bool local_time_in_session_windows(const std::string& windows_body,
                                   const struct tm& local_tm) {
    if (windows_body.empty() || windows_body == "24x7")
        return true;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;

    std::string s = windows_body;
    // trim_inplace is file-local; inline it here via a lambda approach would
    // require capturing s — simpler to just duplicate the trim logic inline.
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();

    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t comma = s.find(',', pos);
        std::string seg = (comma == std::string::npos) ? s.substr(pos)
                                                       : s.substr(pos, comma - pos);
        while (!seg.empty() && std::isspace((unsigned char)seg.front())) seg.erase(seg.begin());
        while (!seg.empty() && std::isspace((unsigned char)seg.back())) seg.pop_back();
        if (!seg.empty())
            parts.push_back(seg);
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    if (parts.empty())
        parts.push_back(s);

    for (const std::string& win : parts) {
        std::size_t dash = win.find('-');
        if (dash == std::string::npos || dash < 4 || win.size() < dash + 4)
            continue;
        std::string left = win.substr(0, 4);
        std::string right = win.substr(dash + 1, 4);
        int sm = hhmm_to_minutes(left);
        int em = hhmm_to_minutes(right);
        if (sm < 0 || em < 0)
            continue;
        // A window whose start equals its end ("1700-1700" on OANDA forex,
        // "0000-0000") is TradingView's 24-hour session: the market is
        // open the whole day and every bar belongs to it. The half-open
        // arithmetic below would otherwise make it EMPTY, so time(session)
        // / time_close / session.ismarket returned na / false on every
        // bar of a forex symbol (finding 455).
        if (sm == em)
            return true;
        bool in_win = (sm <= em) ? (mod >= sm && mod < em)
                                 : (mod >= sm || mod < em);
        if (in_win)
            return true;
    }
    return false;
}

bool passes_session_filter(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return true;

    std::string windows;
    std::unordered_set<int> day_filter;
    parse_day_filter(session, windows, &day_filter);

    struct tm local_tm {};
    decompose_ms_local(bar_ms, tz, local_tm);  // gmtime_r for UTC (no TZ flip)

    int day_of_week_sun1 = local_tm.tm_wday + 1;  // 1=Sunday
    if (!day_filter.empty() && day_filter.count(day_of_week_sun1) == 0)
        return false;

    return local_time_in_session_windows(windows, local_tm);
}

// ---------------------------------------------------------------------------
// Session predicate public free functions
// ---------------------------------------------------------------------------

bool session_in_market(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms) {
    return passes_session_filter(session, tz, bar_ms);
}

bool session_in_premarket(const std::string& session,
                              const std::string& tz,
                              int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return false;

    std::string windows;
    std::unordered_set<int> day_filter;
    parse_day_filter(session, windows, &day_filter);

    std::string rth_open_str;
    {
        std::size_t dash = windows.find('-');
        if (dash >= 4)
            rth_open_str = windows.substr(0, 4);
    }
    int rth_open_min = hhmm_to_minutes(rth_open_str);
    if (rth_open_min < 0)
        return false;
    // start == end is a 24-hour session (see local_time_in_session_windows):
    // the market never closes, so there is no pre-market.
    if (first_window_is_full_day(windows))
        return false;

    int pre_open_min = 4 * 60;

    struct tm local_tm {};
    decompose_ms_local(bar_ms, tz, local_tm);  // gmtime_r for UTC (no TZ flip)

    int day_of_week_sun1 = local_tm.tm_wday + 1;
    if (!day_filter.empty() && day_filter.count(day_of_week_sun1) == 0)
        return false;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;
    return (mod >= pre_open_min && mod < rth_open_min);
}

bool session_in_postmarket(const std::string& session,
                               const std::string& tz,
                               int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return false;

    std::string windows;
    std::unordered_set<int> day_filter;
    parse_day_filter(session, windows, &day_filter);

    std::string rth_close_str;
    {
        std::size_t dash = windows.find('-');
        if (dash != std::string::npos && dash + 4 < windows.size())
            rth_close_str = windows.substr(dash + 1, 4);
    }
    int rth_close_min = hhmm_to_minutes(rth_close_str);
    if (rth_close_min < 0)
        return false;
    // start == end is a 24-hour session: no post-market either.
    if (first_window_is_full_day(windows))
        return false;

    int post_close_min = 20 * 60;

    struct tm local_tm {};
    decompose_ms_local(bar_ms, tz, local_tm);  // gmtime_r for UTC (no TZ flip)

    int day_of_week_sun1 = local_tm.tm_wday + 1;
    if (!day_filter.empty() && day_filter.count(day_of_week_sun1) == 0)
        return false;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;
    return (mod >= rth_close_min && mod < post_close_min);
}

// Chart-timeframe forms (see session_time.hpp): a D/W/M chart bar is the
// symbol's regular-session bar whatever time of day its stamp reads.
bool session_in_market(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms,
                           const std::string& chart_tf) {
    if (tf_is_daily_or_higher(chart_tf))
        return true;
    return session_in_market(session, tz, bar_ms);
}

bool session_in_premarket(const std::string& session,
                              const std::string& tz,
                              int64_t bar_ms,
                              const std::string& chart_tf) {
    if (tf_is_daily_or_higher(chart_tf))
        return false;
    return session_in_premarket(session, tz, bar_ms);
}

bool session_in_postmarket(const std::string& session,
                               const std::string& tz,
                               int64_t bar_ms,
                               const std::string& chart_tf) {
    if (tf_is_daily_or_higher(chart_tf))
        return false;
    return session_in_postmarket(session, tz, bar_ms);
}

// ---------------------------------------------------------------------------
// timeframe_time / timeframe_time_close (existing public API)
// ---------------------------------------------------------------------------
// =========================================================================
// Public functions
// =========================================================================
// A 2-argument time()/time_close() call binds its second string to the
// `session` parameter, but scripts commonly pass a TIMEZONE there — e.g.
// `time("D", "America/New_York")`. TradingView does NOT reinterpret that
// string as the timezone: a value that is not a valid session spec (IANA
// "Area/Location" or a GMT/UTC specifier) is an invalid session that TV
// ignores entirely, rolling the daily boundary at the chart/exchange
// (UTC in the engine's model) timezone — NOT at the string's tz. We detect
// such a string here only so we can drop it (no session filter, tz left at
// the chart/UTC default). A real session window ("0930-1600") never contains
// '/' nor starts with GMT/UTC, and a 3-arg call supplies tz_in, so neither is
// affected.
static bool session_arg_is_timezone(const std::string& s) {
    if (s.empty())
        return false;
    if (s.find('/') != std::string::npos)                 // IANA e.g. America/New_York
        return true;
    if (s.rfind("GMT", 0) == 0 || s.rfind("UTC", 0) == 0)  // GMT / UTC / ±offset
        return true;
    return false;
}

// Resolve the session / timezone triple for a time()/time_close() call.
//
// Two distinct timezones come out of this:
//
//   session_tz_out — the zone the SESSION window ("0930-1600") is read in.
//     Pine: "To interpret the time zone of the specified session, time() and
//     time_close() use the time zone of the exchange by default, unless a
//     timezone argument is specified" (i.e. the default is syminfo.timezone).
//     So an explicit tz argument wins; otherwise the caller-supplied
//     syminfo_tz (codegen passes ``syminfo_.timezone``); otherwise UTC (the
//     historical engine default, still what an unknown/empty syminfo yields).
//
//   tf_tz_out — the zone the TIMEFRAME open/close is computed in. This is
//     deliberately left exactly as before: explicit tz argument, else UTC.
//     The D/W/M calendar path of compute_tf_open_ms is owned elsewhere and
//     must not silently start rolling at syminfo.timezone just because a
//     session was supplied; intraday timeframes sit on the symbol's own
//     HTF grid (the seven-argument forms) or the epoch grid (these) and
//     never depended on tz.
//
// When a 2-arg call passes a timezone-looking string in the session slot (and
// no explicit tz), TV treats it as an invalid session and ignores it: drop the
// session (no filter) but leave tz at the chart/UTC default — do NOT adopt the
// string as the timezone. The daily boundary then rolls at the chart/exchange
// (UTC) timezone, matching TradingView.
static void resolve_session_tz(const std::string& session,
                               const std::string& tz_in,
                               const std::string& syminfo_tz,
                               std::string& sess_out,
                               std::string& session_tz_out,
                               std::string& tf_tz_out) {
    sess_out = session;
    tf_tz_out = tz_in;
    if (tf_tz_out.empty() && session_arg_is_timezone(sess_out)) {
        sess_out.clear();
    }
    if (tf_tz_out.empty())
        tf_tz_out = "UTC";
    session_tz_out = tz_in;
    if (session_tz_out.empty())
        session_tz_out = syminfo_tz;
    if (session_tz_out.empty())
        session_tz_out = "UTC";
}

int64_t timeframe_time(int64_t bar_ms,
                  const std::string& tf_in,
                  const std::string& session,
                  const std::string& tz_in,
                  const std::string& chart_tf,
                  const std::string& syminfo_tz) {
    std::string tf = tf_in.empty() ? chart_tf : tf_in;
    if (tf.empty())
        tf = "1";

    std::string sess, session_tz, tf_tz;
    resolve_session_tz(session, tz_in, syminfo_tz, sess, session_tz, tf_tz);

    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    return compute_tf_open_ms(bar_ms, tf, tf_tz, "UTC", "");
}

int64_t timeframe_time_close(int64_t bar_ms,
                        const std::string& tf_in,
                        const std::string& session,
                        const std::string& tz_in,
                        const std::string& chart_tf,
                        const std::string& syminfo_tz) {
    std::string tf = tf_in.empty() ? chart_tf : tf_in;
    if (tf.empty())
        tf = "1";

    std::string sess, session_tz, tf_tz;
    resolve_session_tz(session, tz_in, syminfo_tz, sess, session_tz, tf_tz);

    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    int64_t t_open = compute_tf_open_ms(bar_ms, tf, tf_tz, "UTC", "");
    return compute_tf_close_ms(t_open, tf, tf_tz);
}

// Symbol-clock forms. Without a session argument the D/W/M bar open is the
// SYMBOL's bar (session-day keyed — 17:00 ET on OANDA forex, 09:30 ET RTH on
// equities, UTC midnight on a 24x7 UTC symbol), never a UTC calendar-day
// floor. A VALID session argument defines the day itself: TradingView keys
// the period on the session's timezone (`time("D", "0000-2359",
// "America/New_York")` on a UTC crypto symbol rolls at New York midnight —
// measured: lukeborgerding-orb-avwap-retest anchors a manual VWAP on
// ta.change() of exactly that and only matches TV's tape 100% with the NY
// roll, 18% with the symbol's UTC roll), so that path keeps the tz-only
// calendar floor of the five-argument forms. A timezone-looking string in the
// session slot is an invalid session (dropped by resolve_session_tz) and
// falls back to the symbol clock. An intraday tf is the symbol's
// day-stamp-anchored HTF grid bucket whatever the session argument (it
// only filters): TradingView's time("60") on NYSE:F 15 is 09:30 / 10:30 /
// .. / 15:30 ET, time("240") on NSE:NIFTY 09:15 / 13:15 IST and on
// OANDA:XAUUSD 17:00 ET + 4h*k, 00:00 UTC + 4h*k only on 24x7 UTC
// (pin-time-hours, 2025-04-01..07-01) — the very grid request.security
// aggregates on (session_intraday_bucket_open_ms), one grid in one place.
static bool symbol_clock_applies(const std::string& resolved_session,
                                 CalendarPeriod cp) {
    return cp != CalendarPeriod::NONE && resolved_session.empty();
}

int64_t timeframe_time(int64_t bar_ms,
                  const std::string& tf_in,
                  const std::string& session,
                  const std::string& tz_in,
                  const std::string& chart_tf,
                  const std::string& sym_tz,
                  const std::string& sym_session) {
    std::string tf = tf_in.empty() ? chart_tf : tf_in;
    if (tf.empty())
        tf = "1";

    // Composition with the syminfo session-tz default: the session window
    // is read in the explicit tz, else syminfo.timezone (sym_tz), else UTC;
    // a VALID session keeps the tf-open in the explicit-tz-else-UTC calendar
    // (tf_tz), while no valid session takes the symbol's own D/W/M bar.
    std::string sess, session_tz, tf_tz;
    resolve_session_tz(session, tz_in, sym_tz, sess, session_tz, tf_tz);

    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    const CalendarPeriod cp = calendar_period_for(tf);
    if (symbol_clock_applies(sess, cp))
        return session_period_open_ms(bar_ms, sym_tz, sym_session, cp);
    return compute_tf_open_ms(bar_ms, tf, tf_tz, sym_tz, sym_session);
}

int64_t timeframe_time_close(int64_t bar_ms,
                        const std::string& tf_in,
                        const std::string& session,
                        const std::string& tz_in,
                        const std::string& chart_tf,
                        const std::string& sym_tz,
                        const std::string& sym_session) {
    std::string tf = tf_in.empty() ? chart_tf : tf_in;
    if (tf.empty())
        tf = "1";

    std::string sess, session_tz, tf_tz;
    resolve_session_tz(session, tz_in, sym_tz, sess, session_tz, tf_tz);

    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    const CalendarPeriod cp = calendar_period_for(tf);
    if (symbol_clock_applies(sess, cp)) {
        // Calendar periods report the period END (last ms), matching the
        // tz-only forms above; intraday closes stay the exact boundary.
        return session_period_close_ms(bar_ms, sym_tz, sym_session, cp) - 1;
    }
    int64_t t_open = compute_tf_open_ms(bar_ms, tf, tf_tz, sym_tz, sym_session);
    return compute_tf_close_ms(t_open, tf, tf_tz);
}

int64_t session_trading_day_open_ms(int64_t bar_ms,
                             const std::string& session,
                             const std::string& tz) {
    // 24/7 or empty session: fall back to UTC calendar-day midnight.
    if (is_allday_session(session)) {
        if (session.empty()) {
            std::fprintf(stderr,
                "[pineforge] WARNING: session_trading_day_open_ms called with empty session "
                "string — falling back to UTC calendar-day midnight.\n");
        }
        // UTC midnight: truncate to day boundary
        time_t secs = static_cast<time_t>(bar_ms / 1000);
        return static_cast<int64_t>((secs / kSecPerDay) * kSecPerDay) * 1000;
    }

    int session_start_min = parse_session_start_minutes(session);
    if (session_start_min < 0) {
        // Unparseable session: fall back to UTC midnight
        std::fprintf(stderr,
            "[pineforge] WARNING: session_trading_day_open_ms: cannot parse session '%s' "
            "— falling back to UTC calendar-day midnight.\n",
            session.c_str());
        time_t secs = static_cast<time_t>(bar_ms / 1000);
        return static_cast<int64_t>((secs / kSecPerDay) * kSecPerDay) * 1000;
    }

    // Obtain bar's local time in the given timezone.
    std::string eff_tz = tz.empty() ? "UTC" : tz;
    int bar_local_min;
    {
        struct tm local_tm {};
        decompose_ms_local(bar_ms, eff_tz, local_tm);  // gmtime_r for UTC (no TZ flip)
        bar_local_min = local_tm.tm_hour * 60 + local_tm.tm_min;
    }

    // Determine whether the bar belongs to today's or yesterday's trading day.
    // If bar's local time >= session_start: today's session.
    // Else: yesterday's session.
    int64_t day_open_ms = calendar_day_open_local_ms(bar_ms, eff_tz);

    if (bar_local_min < session_start_min) {
        // Bar is before today's session open → belongs to yesterday's trading day.
        // Go back one day: subtract 24 h and recompute the day open.
        int64_t yesterday_ms = bar_ms - kMsPerDay;
        day_open_ms = calendar_day_open_local_ms(yesterday_ms, eff_tz);
    }

    // Add session start offset.
    int64_t trading_day_open_ms = day_open_ms
        + static_cast<int64_t>(session_start_min) * 60LL * 1000LL;

    return trading_day_open_ms;
}

} // namespace pineforge
