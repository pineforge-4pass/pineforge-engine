#include <pineforge/session_time.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/timeframe.hpp>
#include "timezone.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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
        // flatten never fired). The calendar DAY/WEEK/MONTH branches below are
        // exact boundaries too: TradingView's time_close("D") - time("D") on
        // BINANCE:BTCUSDT is 86400000, not 86399999 (lab tv w12-tclose-*,
        // tests/fixtures/time_close_function).
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
        return next * 86400 * 1000;
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
        return static_cast<int64_t>(nx) * 1000;
    }
    if (cp == CalendarPeriod::WEEK) {
        local_tm.tm_mday += 7;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000;
    }
    if (cp == CalendarPeriod::MONTH) {
        local_tm.tm_mon += 1;
        local_tm.tm_mday = 1;
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        time_t nx = mktime(&local_tm);
        return static_cast<int64_t>(nx) * 1000;
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

// The session day's first open and last close, in minutes of the local day,
// over EVERY "HHMM-HHMM" window of `windows` (the session without its day
// mask), in whatever order they are written: what session.ispremarket and
// session.ispostmarket are measured from. False when the session has no
// pre- or post-market: no window parses, one spans 24 hours (start == end, as
// TradingView spells it on OANDA forex, or "0000-2400"), or one wraps
// midnight, so the session day opens on the previous date. TradingView flags
// no bar of such a session pre- or post-market (CME_MINI:ES1!, CBOT:ZC1!,
// OANDA:XAUUSD / EURUSD, BINANCE:ETHUSDT.P; lane K-SESSION-WINDOWS F6).
static bool extended_hours_bounds(const std::string& windows, int& first_open,
                                  int& last_close) {
    bool any = false;
    std::size_t pos = 0;
    while (pos <= windows.size()) {
        const std::size_t comma = windows.find(',', pos);
        std::string win = windows.substr(pos, comma == std::string::npos
                                                  ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? windows.size() + 1 : comma + 1;
        trim_inplace(win);
        const std::size_t dash = win.find('-');
        if (dash == std::string::npos || dash < 4 || win.size() < dash + 5)
            continue;
        const int sm = hhmm_to_minutes(win.substr(dash - 4, 4));
        const std::string end = win.substr(dash + 1, 4);
        const int em = end == "2400" ? 24 * 60 : hhmm_to_minutes(end);
        if (sm < 0 || em < 0)
            continue;
        if (sm == em || (sm == 0 && em == 24 * 60) || em < sm)
            return false;
        if (!any || sm < first_open) first_open = sm;
        if (!any || em > last_close) last_close = em;
        any = true;
    }
    return any;
}

// A window clock of a time() / time_close() / session.* session argument,
// the four characters of `s` at `at`, as TradingView reads it: HH * 60 + MM
// minutes after a day's midnight, unchecked -- "2400" is the day's end, "2430"
// 00:30 on the next day, "0060" 01:00. -1 when they are not four digits. The
// window test then reads a time of day in the window when it, or it on the
// next day, falls in [start, end), an end at or before the start being on
// the next day (lab tv tapes w11-sess2400-{btc15,btc1d,xau15} and
// w11-edge-*-btc15, tests/fixtures/session_clock).
int session_clock_minutes(const std::string& s, std::size_t at) {
    if (at + 4 > s.size())
        return -1;
    int digit[4];
    for (int k = 0; k < 4; ++k) {
        const char c = s[at + static_cast<std::size_t>(k)];
        if (c < '0' || c > '9')
            return -1;
        digit[k] = c - '0';
    }
    return (digit[0] * 10 + digit[1]) * 60 + digit[2] * 10 + digit[3];
}

// The windows of `windows` whose two clocks read (session_clock_minutes).
int session_window_count(const std::string& windows) {
    int count = 0;
    std::size_t pos = 0;
    while (pos <= windows.size()) {
        const std::size_t comma = windows.find(',', pos);
        std::string win = windows.substr(pos, comma == std::string::npos
                                                  ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? windows.size() + 1 : comma + 1;
        trim_inplace(win);
        const std::size_t dash = win.find('-');
        if (dash != std::string::npos && dash >= 4 && session_clock_minutes(win, 0) >= 0
            && session_clock_minutes(win, dash + 1) >= 0)
            ++count;
    }
    return count;
}

// The windows of `session` and the days it trades, as every predicate reads
// them: the day list it spells, else Monday to Friday for a session of several
// windows (lab tv w11-sessmask{,2}-btc15, tests/fixtures/session_clock); an
// empty list is every day.
void session_day_list(const std::string& session, std::string& windows,
                      std::unordered_set<int>& days) {
    parse_day_filter(session, windows, &days);
    if (days.empty() && session_window_count(windows) > 1)
        days = {2, 3, 4, 5, 6};
}

// Whether the local time of `local_tm` lies in a window of `windows` whose
// session day is in `days` (1 = Sunday .. 7 = Saturday). A window's session
// day is the day it starts, but the day it ends for a window that ends at or
// before its start, past midnight: "1700-1700:23456" opens Monday's session
// on Sunday at 17:00, "1800-0200:23456" admits Sunday 18:00 and not Friday
// 18:00, "2330-2430:23456" keeps Friday's 00:00-00:30 tail on Saturday and
// "0000-0000:23456" is the calendar's weekdays (lab tv tapes
// w11-sessmask{,2,3}-btc15, tests/fixtures/session_clock).
bool local_time_in_session_days(const std::string& windows, const struct tm& local_tm,
                                const std::unordered_set<int>& days) {
    const int mod = local_tm.tm_hour * 60 + local_tm.tm_min;
    std::size_t pos = 0;
    while (pos <= windows.size()) {
        const std::size_t comma = windows.find(',', pos);
        std::string win = windows.substr(pos, comma == std::string::npos
                                                  ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? windows.size() + 1 : comma + 1;
        trim_inplace(win);
        const std::size_t dash = win.find('-');
        if (dash == std::string::npos || dash < 4)
            continue;
        const int sm = session_clock_minutes(win, 0);
        int em = session_clock_minutes(win, dash + 1);
        if (sm < 0 || em < 0)
            continue;
        const int ends_next_day = em <= sm && em > 0 ? 1 : 0;
        if (em <= sm)
            em += 24 * 60;
        // `back` days ago the window opened that holds this time of day.
        int back = 0;
        for (int t = mod; t < em; t += 24 * 60, ++back) {
            if (t < sm)
                continue;
            const int day = ((local_tm.tm_wday - back + ends_next_day) % 7 + 7) % 7 + 1;
            if (days.count(day) != 0)
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// A session argument's own D / W / M bars (lane W12-ENG-TIME). time(tf,
// session, tz) and time_close(tf, session, tz) with a D, W or M tf read the
// bar TradingView builds on the session argument itself, in the session's
// timezone (the explicit tz, else syminfo.timezone), not a calendar floor
// (lab tv tapes w12-tfd-* and w12-tfd2-*, tests/fixtures/session_period):
//  - Every session day -- a window's day is the day it starts, the day it
//    ends for one past midnight, and a list (given, or Monday to Friday for
//    several windows) leaves days out, as local_time_in_session_days reads
//    them -- is one D bar, from its first window's open to its last window's
//    close. time("D", "1700-2400", "America/New_York") opens at 17:00 New
//    York, time("D", "1800-1700") at 18:00 the day before, and a bar in the
//    break of "0930-1130,1300-1500" reads the day that opened at 09:30. A bar
//    no day bar holds reads na.
//  - A W or M bar opens at the first session day of its Monday-start week or
//    calendar month, by the session day's date, and holds every instant until
//    the next one opens: it is never na.
//  - time_close("D") is the day bar's close. time_close("W"/"M") is the next
//    period's open on an intraday chart, and the close of the period's last
//    session day on a daily-or-higher chart.
//  - On a daily-or-higher chart a window spelled past midnight without
//    wrapping ("2330-2430", "2300-2500") stops at the week's end when it opens
//    on a Saturday: time("D", "2330-2430") on BINANCE:BTCUSDT 1D reads na on
//    Sunday's 00:00 bar, where the 60-minute chart reads Saturday's 23:30
//    open (w12-tfd3-btc1d / -btc60). A wrapping window ("2330-0030") is its
//    end day's and is never cut.
// The bar is the session's on every chart: on a D chart, time(timeframe.period,
// session, tz) is the day bar that holds the chart bar's time.
//
// An intraday tf is built on the session too: its bars open at each window's
// open and every tf after it, the last cut at the window's close, so
// time(timeframe.period, "0930-1600") on BINANCE:BTCUSDT 60 reads 09:30 for
// the 10:00 bar, time("240", "0930-1600") 09:30 then 13:30, and a second
// window opens its own grid ("0930-1130,1245-1500": 12:45, 13:45); a bar in
// no window reads na, as passes_session_filter reads it (w12-tfd4-*,
// w12-tfd3-btc60). A window that opens on the chart's grid reads each chart
// bar's own time, as before.
// ---------------------------------------------------------------------------

struct ArgWindow {
    int start = 0;       // minutes after midnight of the day it opens on
    int length = 0;      // minutes
    int day_offset = 0;  // 0: opens on its session day, -1: on the day before
};

struct ArgSession {
    std::vector<ArgWindow> windows;
    std::unordered_set<int> days;  // 1 = Sunday .. 7 = Saturday; empty: every day
};

// The windows and the day list of a session argument, read as
// passes_session_filter reads them. False when no window reads.
bool read_arg_session(const std::string& session, ArgSession& out) {
    out = ArgSession{};
    std::string windows;
    session_day_list(session, windows, out.days);
    trim_inplace(windows);
    if (windows.empty() || windows == "24x7")
        windows = "0000-0000";
    std::size_t pos = 0;
    while (pos <= windows.size()) {
        const std::size_t comma = windows.find(',', pos);
        std::string win = windows.substr(pos, comma == std::string::npos
                                                  ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? windows.size() + 1 : comma + 1;
        trim_inplace(win);
        const std::size_t dash = win.find('-');
        if (dash == std::string::npos || dash < 4)
            continue;
        const int sm = session_clock_minutes(win, 0);
        const int em = session_clock_minutes(win, dash + 1);
        if (sm < 0 || em < 0)
            continue;
        const int length = em <= sm ? em + 24 * 60 - sm : em - sm;
        if (length <= 0)
            continue;
        out.windows.push_back({sm, length, em <= sm && em > 0 ? -1 : 0});
    }
    return !out.windows.empty();
}

int64_t floor_div(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// The epoch instant of a wall-clock minute in `tz` (days since 1970-01-01 *
// 1440 + minutes), read through the offsets decompose_ms_local holds (no
// zone switch per call): the offsets in force a day before and a day after
// give the candidates, and a candidate whose own wall clock reads `minute` is
// the instant -- the first of two in a repeated hour. A minute a switch skips
// reads the first instant after it (native_calendar::resolve_civil, as the
// kernel calendar resolves one).
int64_t wall_minute_ms(int64_t minute, const std::string& tz) {
    const int64_t naive = minute * 60000;
    if (utc_zone(tz))
        return naive;
    bool found = false;
    int64_t best = 0;
    for (const int64_t probe : {naive - kMsPerDay, naive + kMsPerDay}) {
        struct tm at {};
        decompose_ms_local(probe, tz, at);
        const int64_t candidate = naive - static_cast<int64_t>(at.tm_gmtoff) * 1000;
        struct tm back {};
        decompose_ms_local(candidate, tz, back);
        const int64_t wall =
            days_from_civil(back.tm_year + 1900, back.tm_mon + 1, back.tm_mday) * 24 * 60
            + back.tm_hour * 60 + back.tm_min;
        if (wall == minute && back.tm_sec == 0 && (!found || candidate < best)) {
            best = candidate;
            found = true;
        }
    }
    if (found)
        return best;
    const int64_t day = floor_div(minute, 24 * 60);
    const int of_day = static_cast<int>(minute - day * 24 * 60);
    const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(day);
    if (const auto r = native_calendar::resolve_civil(tz, static_cast<int>(date.year), date.month,
                                                      date.day, of_day / 60, of_day % 60)) {
        return r->epoch_ms;
    }
    return naive;
}

struct ArgDayBar {
    bool traded = false;
    int64_t open_ms = 0;
    int64_t close_ms = 0;
};

// The offset switch whose wall time, read on the offset before it, falls on a
// local date (detail::SessionClockSwitches::transition_wall_clock): the first
// instant on the new offset, both offsets in seconds east of UTC, and the
// switch's pre-transition wall time -- 02:00 in New York both ways, 02:00 in
// London in autumn and 01:00 in spring.
struct DaySwitch {
    bool has = false;
    int64_t at_ms = 0;
    long pre_s = 0;
    long post_s = 0;
    int pre_wall_minute = 0;
};

// One window of a session argument on one session day, as TradingView's
// session clock reads it across a switch (session_argument_intraday_bar).
// `clock`: the window opens before 24:00 and lasts less than a day, so the
// clock reads it; any other spelling keeps its plain wall-clock instance
// [open_ms, end_ms). `deviates`: it starts or ends at the date's switch's
// pre-transition wall time and reads the post-transition offset throughout.
struct WallOccurrence {
    bool traded = false;
    bool clock = false;
    bool overnight = false;
    bool deviates = false;
    int start_minute = 0;
    int end_minute = 0;
    int64_t length_ms = 0;
    int64_t start_wall = 0;  // wall clock, ms since 1970-01-01 read as UTC
    int64_t end_wall = 0;
    long post_s = 0;
    int64_t open_ms = 0;     // the first instant in the window
    int64_t end_ms = 0;      // its end instant
};

// One session argument in one zone, read once per thread: its windows and day
// list, and each session day's D bar (per chart kind) and window instances,
// resolved once. A script's several session sites, in several zones, keep one
// entry each, so reading one never evicts another's days. The session clock's
// switch and occurrences of a day are kept apart from the plain instances, so
// the two readings never mix.
struct ArgCache {
    struct Day {
        int64_t day = 0;
        bool used = false;
        bool bar_done[2] = {false, false};
        ArgDayBar bar[2];
        bool windows_done = false;
        std::vector<std::pair<int64_t, int64_t>> windows;
        bool switch_done = false;
        DaySwitch day_switch;
        bool occurrences_done = false;
        std::vector<WallOccurrence> occurrences;
    };
    static constexpr int kDays = 64;
    std::string session;
    std::string tz;
    bool ok = false;
    // utc_zone(tz), once asked (arg_cache_utc).
    bool utc_known = false;
    bool utc = false;
    ArgSession parsed;
    Day days[kDays];
    uint64_t used_at = 0;
};

ArgCache& arg_cache(const std::string& session, const std::string& tz) {
    constexpr std::size_t kEntries = 32;
    thread_local std::vector<std::unique_ptr<ArgCache>> entries;
    thread_local uint64_t clock = 0;
    for (auto& entry : entries) {
        if (entry->session == session && entry->tz == tz) {
            entry->used_at = ++clock;
            return *entry;
        }
    }
    if (entries.size() < kEntries) {
        entries.push_back(std::make_unique<ArgCache>());
    } else {
        auto oldest = std::min_element(entries.begin(), entries.end(),
                                       [](const std::unique_ptr<ArgCache>& a,
                                          const std::unique_ptr<ArgCache>& b) {
                                           return a->used_at < b->used_at;
                                       });
        std::swap(*oldest, entries.back());
        entries.back() = std::make_unique<ArgCache>();
    }
    ArgCache& entry = *entries.back();
    entry.session = session;
    entry.tz = tz;
    entry.ok = read_arg_session(session, entry.parsed);
    entry.used_at = ++clock;
    return entry;
}

ArgCache::Day& arg_cache_day(ArgCache& cache, int64_t day) {
    ArgCache::Day& slot =
        cache.days[static_cast<std::size_t>(((day % ArgCache::kDays) + ArgCache::kDays)
                                            % ArgCache::kDays)];
    if (!slot.used || slot.day != day) {
        slot = ArgCache::Day{};
        slot.used = true;
        slot.day = day;
    }
    return slot;
}

// Session day `day` (days since 1970-01-01, the date in the zone) of the
// cached session: its D bar, not traded on a day the list leaves out. On a
// daily-or-higher chart a Saturday window that does not wrap ends at the
// week's end.
ArgDayBar arg_day_bar(ArgCache& cache, int64_t day, bool daily_chart) {
    ArgCache::Day& slot = arg_cache_day(cache, day);
    const int kind = daily_chart ? 1 : 0;
    if (slot.bar_done[kind])
        return slot.bar[kind];
    const ArgSession& s = cache.parsed;
    ArgDayBar bar;
    const int weekday = static_cast<int>(((day + 4) % 7 + 7) % 7) + 1;  // 1970-01-01: Thursday
    if (s.days.empty() || s.days.count(weekday) != 0) {
        int64_t first = 0;
        int64_t last = 0;
        for (std::size_t k = 0; k < s.windows.size(); ++k) {
            const ArgWindow& w = s.windows[k];
            const int64_t start = (day + w.day_offset) * 24 * 60 + w.start;
            int64_t end = start + w.length;
            if (daily_chart && weekday == 7 && w.day_offset == 0)
                end = std::min(end, (day + 1) * 24 * 60);
            if (k == 0 || start < first) first = start;
            if (k == 0 || end > last) last = end;
        }
        bar.open_ms = wall_minute_ms(first, cache.tz);
        bar.close_ms = wall_minute_ms(last, cache.tz);
        bar.traded = bar.close_ms > bar.open_ms;
    }
    slot.bar_done[kind] = true;
    slot.bar[kind] = bar;
    return bar;
}

// The key of the D / W / M period a session day's date falls in: the day, its
// Monday-start week (1969-12-29 is day -3), or year * 12 + month - 1.
int64_t arg_period_key(int64_t day, CalendarPeriod period) {
    if (period == CalendarPeriod::WEEK)
        return floor_div(day + 3, 7);
    if (period == CalendarPeriod::MONTH) {
        const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(day);
        return static_cast<int64_t>(date.year) * 12 + (date.month - 1);
    }
    return day;
}

// The first date of period `key`.
int64_t arg_period_first_day(int64_t key, CalendarPeriod period) {
    if (period == CalendarPeriod::WEEK)
        return key * 7 - 3;
    if (period == CalendarPeriod::MONTH) {
        const int64_t year = floor_div(key, 12);
        return days_from_civil(year, key - year * 12 + 1, 1);
    }
    return key;
}

// The window instances of session day `day` of the cached session in epoch
// ms, [open, close) each, none on a day the list leaves out.
const std::vector<std::pair<int64_t, int64_t>>& arg_day_windows(ArgCache& cache, int64_t day) {
    ArgCache::Day& slot = arg_cache_day(cache, day);
    if (slot.windows_done)
        return slot.windows;
    slot.windows_done = true;
    const ArgSession& s = cache.parsed;
    const int weekday = static_cast<int>(((day + 4) % 7 + 7) % 7) + 1;  // 1970-01-01: Thursday
    if (!s.days.empty() && s.days.count(weekday) == 0)
        return slot.windows;
    for (const ArgWindow& w : s.windows) {
        const int64_t start = (day + w.day_offset) * 24 * 60 + w.start;
        const int64_t open_ms = wall_minute_ms(start, cache.tz);
        const int64_t close_ms = wall_minute_ms(start + w.length, cache.tz);
        if (close_ms > open_ms)
            slot.windows.emplace_back(open_ms, close_ms);
    }
    return slot.windows;
}

// ---------------------------------------------------------------------------
// TradingView's session clock across an offset switch (TradingView tapes of
// synthetic BINANCE:ETHUSDT.P 15 scripts: ten controls exported twice,
// byte-identical, and one earlier overnight tape;
// tests/fixtures/session_transition_clock). In the zone, a window's
// occurrence is a day window on its date D, or an overnight window ending on
// D.
//  1. An occurrence deviates when D has a switch and the window starts or
//     ends at the switch's pre-transition wall time (New York 02:00 both ways,
//     London 02:00 in autumn and 01:00 in spring).
//  2. A deviating occurrence reads the post-transition offset throughout: it
//     ends at D@end - post offset, owns the bars after the previous date's
//     occurrence's end up to and including its own end, and such a bar is in
//     the session when its time of day on the post-transition offset is in
//     the window -- so New York's 1800-0200 opens at 18:00 EST (23:00 UTC) on
//     2025-11-01, not 18:00 EDT, and reads 06:15-06:45 UTC that morning.
//  3. Any other bar: a day window holds [I(D, start), I(D, end)), a repeated
//     wall time on its earlier instant and a skipped one on the
//     post-transition offset unmoved (wall - post offset); an overnight window
//     holds the wall clock [(D-1)@start, D@end) before I(D, end), a repeated
//     end on its later instant -- both passes of the repeated hour, cut at a
//     skipped end unmoved.
// The bar's value is its tf bar counted from the occurrence's first instant,
// as before; a deviating occurrence opens at D'@start - post offset (D' the
// start's date), and a bar it owns before that reads the window instance on
// the post-transition offset that holds it.
// What the tapes pin, and so all the rule covers: a script's own session
// argument to time() / time_close() (a 24x7 UTC symbol, New York and London
// switches). Other zones follow the same form untaped. The chart's own bar
// close, which the Pine host reads through the SYMBOL's session
// (symbol_session_time_close), keeps the plain wall-clock windows until a
// tape pins a symbol session with an endpoint at its zone's switch hour.
// ---------------------------------------------------------------------------

namespace detail_clock {

constexpr int64_t kMsPerMinute = 60000;
constexpr int64_t kMsPerHour = 3600000;

long offset_at(int64_t ms, const std::string& tz) {
    struct tm at {};
    decompose_ms_local(ms, tz, at);
    return static_cast<long>(at.tm_gmtoff);
}

// The switch of local date `day` in `tz` (one at most a day: switches of a
// zone are months apart), by the offsets read before and after every
// instant of the date and the exact second bisected between them.
DaySwitch find_day_switch(int64_t day, const std::string& tz) {
    DaySwitch out;
    const int64_t midnight = day * kMsPerDay;
    const int64_t lo = midnight - 16 * kMsPerHour;              // offsets reach +14 h
    const int64_t hi = midnight + kMsPerDay + 14 * kMsPerHour;  // and -12 h
    const long before = offset_at(lo, tz);
    if (offset_at(hi, tz) == before)
        return out;
    int64_t a = lo / 1000;
    int64_t b = hi / 1000;
    while (b - a > 1) {
        const int64_t mid = a + (b - a) / 2;
        if (offset_at(mid * 1000, tz) == before)
            a = mid;
        else
            b = mid;
    }
    const int64_t wall = b * 1000 + static_cast<int64_t>(before) * 1000;
    if (floor_div(wall, kMsPerDay) != day)
        return out;
    out.has = true;
    out.at_ms = b * 1000;
    out.pre_s = before;
    out.post_s = offset_at(out.at_ms, tz);
    out.pre_wall_minute = static_cast<int>((wall - midnight) / kMsPerMinute);
    return out;
}

}  // namespace detail_clock

const DaySwitch& arg_day_switch(ArgCache& cache, int64_t day) {
    ArgCache::Day& slot = arg_cache_day(cache, day);
    if (!slot.switch_done) {
        slot.day_switch = detail_clock::find_day_switch(day, cache.tz);
        slot.switch_done = true;
    }
    return slot.day_switch;
}

// The instant of wall-clock minute `minute` (days since 1970-01-01 * 1440 +
// minutes) on the session clock: the one instant whose wall clock reads it,
// the earlier of a repeated one (the later when `later`), and a skipped one
// on the post-transition offset, unmoved; `skipped` reports the last.
int64_t clock_wall_instant(ArgCache& cache, int64_t minute, bool later, bool* skipped) {
    const int64_t naive = minute * detail_clock::kMsPerMinute;
    const int64_t date = floor_div(minute, 24 * 60);
    const DaySwitch& sw = arg_day_switch(cache, date);
    long offsets[2] = {0, 0};
    int count = 0;
    if (sw.has) {
        offsets[count++] = sw.pre_s;
        offsets[count++] = sw.post_s;
    } else {
        const int64_t noon = date * kMsPerDay + 12 * detail_clock::kMsPerHour;
        const long guess = detail_clock::offset_at(noon, cache.tz);
        offsets[count++] =
            detail_clock::offset_at(noon - static_cast<int64_t>(guess) * 1000, cache.tz);
    }
    bool found = false;
    int64_t instant = 0;
    for (int k = 0; k < count; ++k) {
        const int64_t candidate = naive - static_cast<int64_t>(offsets[k]) * 1000;
        if (detail_clock::offset_at(candidate, cache.tz) != offsets[k])
            continue;
        instant = candidate;
        found = true;
        if (!later)
            break;
    }
    if (skipped != nullptr)
        *skipped = !found;
    if (found)
        return instant;
    return naive - static_cast<int64_t>(sw.has ? sw.post_s : offsets[0]) * 1000;
}

// Every window's occurrence on session day `day` of the cached session, in
// the order of its windows.
const std::vector<WallOccurrence>& arg_day_occurrences(ArgCache& cache, int64_t day) {
    ArgCache::Day& slot = arg_cache_day(cache, day);
    if (slot.occurrences_done)
        return slot.occurrences;
    const ArgSession& s = cache.parsed;
    const int weekday = static_cast<int>(((day + 4) % 7 + 7) % 7) + 1;  // 1970-01-01: Thursday
    const bool traded = s.days.empty() || s.days.count(weekday) != 0;
    std::vector<WallOccurrence> out(s.windows.size());
    for (std::size_t k = 0; k < s.windows.size(); ++k) {
        const ArgWindow& w = s.windows[k];
        WallOccurrence& o = out[k];
        o.traded = traded;
        const int64_t start = (day + w.day_offset) * 24 * 60 + w.start;
        const int64_t end = start + w.length;
        o.start_wall = start * detail_clock::kMsPerMinute;
        o.end_wall = end * detail_clock::kMsPerMinute;
        o.length_ms = static_cast<int64_t>(w.length) * detail_clock::kMsPerMinute;
        o.clock = w.start >= 0 && w.start < 24 * 60 && w.length > 0 && w.length < 24 * 60;
        if (!o.clock) {
            o.open_ms = wall_minute_ms(start, cache.tz);
            o.end_ms = wall_minute_ms(end, cache.tz);
            continue;
        }
        // The window's date D is the session day: the day it opens, or the
        // day it ends for one past midnight (read_arg_session's day_offset).
        o.overnight = w.start + w.length > 24 * 60;
        o.start_minute = w.start;
        o.end_minute = o.overnight ? w.start + w.length - 24 * 60 : w.start + w.length;
        const DaySwitch& sw = arg_day_switch(cache, day);
        o.deviates = sw.has && (sw.pre_wall_minute == o.end_minute
                                || sw.pre_wall_minute == o.start_minute);
        if (o.deviates) {
            o.post_s = sw.post_s;
            o.open_ms = o.start_wall - static_cast<int64_t>(sw.post_s) * 1000;
            o.end_ms = o.end_wall - static_cast<int64_t>(sw.post_s) * 1000;
            continue;
        }
        o.end_ms = clock_wall_instant(cache, end, o.overnight, nullptr);
        bool skipped = false;
        o.open_ms = clock_wall_instant(cache, start, false, &skipped);
        // An overnight window opens where the wall clock first reads its
        // start: the switch itself for a start the switch skips.
        if (o.overnight && skipped)
            o.open_ms = arg_day_switch(cache, floor_div(start, 24 * 60)).at_ms;
    }
    ArgCache::Day& kept = arg_cache_day(cache, day);
    kept.occurrences = std::move(out);
    kept.occurrences_done = true;
    return kept.occurrences;
}

// The occurrences of session day `day` when its ring slot already holds them
// (arg_day_occurrences then returns them and changes nothing), else null.
const std::vector<WallOccurrence>* resolved_occurrences(const ArgCache& cache, int64_t day) {
    const ArgCache::Day& slot =
        cache.days[static_cast<std::size_t>(((day % ArgCache::kDays) + ArgCache::kDays)
                                            % ArgCache::kDays)];
    return slot.used && slot.day == day && slot.occurrences_done ? &slot.occurrences : nullptr;
}

// utc_zone(cache.tz), asked once per entry: the answer depends on the zone's
// spelling alone, as every memo slot holding a spelling records
// normalize_timezone_for_posix's reading of it. Asked where the reading
// first needs it, so a spelling that throws still throws there.
bool arg_cache_utc(ArgCache& cache) {
    if (!cache.utc_known) {
        cache.utc = utc_zone(cache.tz);
        cache.utc_known = true;
    }
    return cache.utc;
}

// session_argument_intraday_bar on the session clock (the rule above): the
// latest occurrence that holds `bar_ms` -- by session day, then by open, as
// the plain reading picks -- cut into tf bars from its first instant.
bool clock_intraday_bar(int64_t bar_ms, ArgCache& cache, int64_t tf_ms, int64_t& open_ms,
                        int64_t& close_ms) {
    struct tm local {};
    decompose_ms_local(bar_ms, cache.tz, local);
    const int64_t today = days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    const int64_t wall = bar_ms + static_cast<int64_t>(local.tm_gmtoff) * 1000;
    // The scans below read days today - 5 .. today + 2. Each day's occurrences
    // are asked of the cache at its first read, in the order the scans reach
    // it; a later read is the same vector, since resolving a day touches only
    // the days next to it and no two days within ten share a ring slot.
    const std::vector<WallOccurrence>* asked[8] = {};
    const auto occurrences = [&](int64_t d) -> const std::vector<WallOccurrence>& {
        const std::vector<WallOccurrence>*& entry = asked[d - (today - 5)];
        if (entry == nullptr) {
            entry = resolved_occurrences(cache, d);
            if (entry == nullptr)
                entry = &arg_day_occurrences(cache, d);
        }
        return *entry;
    };
    bool found = false;
    int64_t best_day = 0;
    int64_t best_start = 0;
    int64_t best_end = 0;
    for (std::size_t k = 0; k < cache.parsed.windows.size(); ++k) {
        bool held = false;
        int64_t day = 0;
        int64_t start = 0;
        int64_t end = 0;
        if (!occurrences(today)[k].clock) {
            // A window the clock does not read: its plain instances, as the
            // plain reading scans them (one spelled past a day reaches back).
            for (int64_t d = today + 1; d >= today - 5 && !held; --d) {
                const WallOccurrence& o = occurrences(d)[k];
                if (o.traded && o.end_ms > o.open_ms && o.open_ms <= bar_ms && bar_ms < o.end_ms) {
                    held = true;
                    day = d;
                    start = o.open_ms;
                    end = o.end_ms;
                }
            }
        } else {
            // Rule 2: a deviating occurrence owns (previous end, own end].
            bool owned = false;
            for (int64_t d = today - 1; d <= today + 2 && !owned; ++d) {
                const WallOccurrence& o = occurrences(d)[k];
                if (!o.traded || !o.deviates || bar_ms > o.end_ms)
                    continue;
                if (occurrences(d - 1)[k].end_ms >= bar_ms)
                    continue;
                owned = true;
                const int64_t post_wall = bar_ms + static_cast<int64_t>(o.post_s) * 1000;
                const int64_t post_day = floor_div(post_wall, kMsPerDay);
                const int64_t of_day = post_wall - post_day * kMsPerDay;
                const int64_t from = o.start_minute * detail_clock::kMsPerMinute;
                const int64_t to = o.end_minute * detail_clock::kMsPerMinute;
                const bool in = o.overnight ? (of_day >= from || of_day < to)
                                            : (of_day >= from && of_day < to);
                if (in) {
                    const int64_t opened = o.overnight && of_day < from ? post_day - 1 : post_day;
                    held = true;
                    day = d;
                    start = opened * kMsPerDay + from - static_cast<int64_t>(o.post_s) * 1000;
                    end = start + o.length_ms;
                }
            }
            // Rule 3: the other occurrences.
            for (int64_t d = today + 2; d >= today - 2 && !owned && !held; --d) {
                const WallOccurrence& o = occurrences(d)[k];
                if (!o.traded || o.deviates)
                    continue;
                const bool in = o.overnight
                    ? o.start_wall <= wall && wall < o.end_wall && bar_ms < o.end_ms
                    : o.open_ms <= bar_ms && bar_ms < o.end_ms;
                if (in) {
                    held = true;
                    day = d;
                    start = o.open_ms;
                    end = o.end_ms;
                }
            }
        }
        if (held && (!found || day > best_day || (day == best_day && start > best_start))) {
            found = true;
            best_day = day;
            best_start = start;
            best_end = end;
        }
    }
    if (!found)
        return false;
    open_ms = best_start + (bar_ms - best_start) / tf_ms * tf_ms;
    close_ms = std::min(open_ms + tf_ms, best_end);
    return true;
}

// The session argument's intraday bar of `tf_ms` that holds `bar_ms`: the
// latest window instance that holds it, cut into tf bars from its open, the
// last one closing at the window's close. False (na) when no window holds it.
// Outside UTC, TradingView's session clock reads a script's session argument
// across a switch (clock_intraday_bar) unless a test turned
// transition_wall_clock off; `transition_clock` false keeps the plain reading
// (the symbol's own session: symbol_session_time_close).
bool session_argument_intraday_bar(int64_t bar_ms, const std::string& session,
                                   const std::string& tz, int64_t tf_ms, int64_t& open_ms,
                                   int64_t& close_ms, bool transition_clock) {
    ArgCache& cache = arg_cache(session, tz);
    if (tf_ms <= 0 || !cache.ok)
        return false;
    if (transition_clock && detail::session_clock_switches().transition_wall_clock
        && !arg_cache_utc(cache))
        return clock_intraday_bar(bar_ms, cache, tf_ms, open_ms, close_ms);
    struct tm local {};
    decompose_ms_local(bar_ms, tz, local);
    const int64_t today = days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    // A window past midnight opens the day before its day; one spelled longer
    // than a day ("0000-9959") reaches a few days on.
    for (int64_t d = today + 1; d >= today - 5; --d) {
        bool found = false;
        int64_t start = 0;
        int64_t end = 0;
        for (const auto& window : arg_day_windows(cache, d)) {
            if (window.first <= bar_ms && bar_ms < window.second
                && (!found || window.first > start)) {
                found = true;
                start = window.first;
                end = window.second;
            }
        }
        if (!found)
            continue;
        open_ms = start + (bar_ms - start) / tf_ms * tf_ms;
        close_ms = std::min(open_ms + tf_ms, end);
        return true;
    }
    return false;
}

// The session argument's bar of `period` that holds `bar_ms`: its open and its
// close (time_close's, which depends on `daily_chart` for W and M). False
// (na) when no bar holds it.
bool session_argument_bar(int64_t bar_ms, const std::string& session, const std::string& tz,
                          CalendarPeriod period, bool daily_chart, int64_t& open_ms,
                          int64_t& close_ms) {
    ArgCache& cache = arg_cache(session, tz);
    if (!cache.ok)
        return false;
    struct tm local {};
    decompose_ms_local(bar_ms, tz, local);
    const int64_t today = days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    // The latest session day opened by the bar; a window past midnight opens
    // the day before its day, and a list skips at most six days.
    int64_t day = 0;
    ArgDayBar held;
    bool found = false;
    for (int64_t d = today + 1; d >= today - 8 && !found; --d) {
        const ArgDayBar bar = arg_day_bar(cache, d, daily_chart);
        if (bar.traded && bar.open_ms <= bar_ms) {
            day = d;
            held = bar;
            found = true;
        }
    }
    if (!found)
        return false;
    if (period == CalendarPeriod::DAY) {
        if (bar_ms >= held.close_ms)
            return false;
        open_ms = held.open_ms;
        close_ms = held.close_ms;
        return true;
    }
    const int64_t key = arg_period_key(day, period);
    const int64_t next_first = arg_period_first_day(key + 1, period);
    open_ms = held.open_ms;
    for (int64_t d = arg_period_first_day(key, period); d < day; ++d) {
        const ArgDayBar bar = arg_day_bar(cache, d, daily_chart);
        if (bar.traded) {
            open_ms = bar.open_ms;
            break;
        }
    }
    close_ms = held.close_ms;
    if (daily_chart) {
        for (int64_t d = next_first - 1; d > day; --d) {
            const ArgDayBar bar = arg_day_bar(cache, d, daily_chart);
            if (bar.traded) {
                close_ms = bar.close_ms;
                break;
            }
        }
        return true;
    }
    for (int64_t d = next_first; d < next_first + 62; ++d) {
        const ArgDayBar bar = arg_day_bar(cache, d, daily_chart);
        if (bar.traded) {
            close_ms = bar.open_ms;
            break;
        }
    }
    return true;
}

}  // anonymous namespace

namespace detail {

// TradingView's session clock across a switch (session_time.hpp).
// Constant-initialized, process-wide; only tests change it.
namespace {
SessionClockSwitches g_session_clock_switches;
}  // namespace

SessionClockSwitches& session_clock_switches() noexcept {
    return g_session_clock_switches;
}

}  // namespace detail

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
        int sm = session_clock_minutes(win, 0);
        int em = session_clock_minutes(win, dash + 1);
        if (sm < 0 || em < 0)
            continue;
        // An end at or before the start is on the next day: a wrapping
        // window ("1800-1700"), and a window whose start equals its end
        // ("1700-1700" on OANDA forex, "0000-0000"), which is TradingView's
        // 24-hour session -- every bar belongs to it (finding 455).
        if (em <= sm)
            em += 24 * 60;
        for (int t = mod; t < em; t += 24 * 60) {
            if (t >= sm)
                return true;
        }
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
    session_day_list(session, windows, day_filter);

    struct tm local_tm {};
    decompose_ms_local(bar_ms, tz, local_tm);  // gmtime_r for UTC (no TZ flip)

    if (day_filter.empty())
        return local_time_in_session_windows(windows, local_tm);
    // A 24-hour body ("24x7:23456") takes its list on calendar days, as
    // "0000-0000" does.
    std::string body = windows;
    trim_inplace(body);
    if (body.empty() || body == "24x7")
        body = "0000-0000";
    return local_time_in_session_days(body, local_tm, day_filter);
}

bool session_trades_at(const std::string& session, const std::string& tz, int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return true;
    std::string windows;
    std::unordered_set<int> days;
    session_day_list(session, windows, days);
    trim_inplace(windows);
    if (windows.empty() || windows == "24x7")
        windows = "0000-0000";
    if (days.empty())
        days = {2, 3, 4, 5, 6};
    struct tm local_tm {};
    decompose_ms_local(bar_ms, tz, local_tm);
    return local_time_in_session_days(windows, local_tm, days);
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

    // The days it trades are the in-market predicate's (session_day_list):
    // a Saturday of a session of several windows and no list is no day of
    // it. The session day of a bar before its open is the bar's own date:
    // an overnight session has no pre-market (extended_hours_bounds).
    std::string windows;
    std::unordered_set<int> day_filter;
    session_day_list(session, windows, day_filter);

    // From 04:00 to the session day's first open, over every window: a
    // break between two windows is not before the open.
    int first_open = 0;
    int last_close = 0;
    if (!extended_hours_bounds(windows, first_open, last_close))
        return false;

    int pre_open_min = 4 * 60;

    struct tm local_tm {};
    decompose_ms_local(bar_ms, tz, local_tm);  // gmtime_r for UTC (no TZ flip)

    int day_of_week_sun1 = local_tm.tm_wday + 1;
    if (!day_filter.empty() && day_filter.count(day_of_week_sun1) == 0)
        return false;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;
    return (mod >= pre_open_min && mod < first_open);
}

bool session_in_postmarket(const std::string& session,
                               const std::string& tz,
                               int64_t bar_ms) {
    if (session.empty() || session == "24x7")
        return false;

    // The days it trades are the in-market predicate's (session_day_list),
    // and a bar after the close is on its session day's own date.
    std::string windows;
    std::unordered_set<int> day_filter;
    session_day_list(session, windows, day_filter);

    // From the session day's last close, over every window, to 20:00: a
    // break between two windows is not after the close.
    int first_open = 0;
    int last_close = 0;
    if (!extended_hours_bounds(windows, first_open, last_close))
        return false;

    int post_close_min = 20 * 60;

    struct tm local_tm {};
    decompose_ms_local(bar_ms, tz, local_tm);  // gmtime_r for UTC (no TZ flip)

    int day_of_week_sun1 = local_tm.tm_wday + 1;
    if (!day_filter.empty() && day_filter.count(day_of_week_sun1) == 0)
        return false;

    int mod = local_tm.tm_hour * 60 + local_tm.tm_min;
    return (mod >= last_close && mod < post_close_min);
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
//   session_tz — the zone the SESSION window ("0930-1600") is read in.
//     Pine: "To interpret the time zone of the specified session, time() and
//     time_close() use the time zone of the exchange by default, unless a
//     timezone argument is specified" (i.e. the default is syminfo.timezone).
//     So an explicit tz argument wins; otherwise the caller-supplied
//     syminfo_tz (codegen passes ``syminfo_.timezone``); otherwise UTC (the
//     historical engine default, still what an unknown/empty syminfo yields).
//
//   tf_tz — the zone the five-argument forms' D/W/M calendar floor is
//     computed in when there is no session argument: explicit tz argument,
//     else UTC. A valid session argument's D/W/M period is the session's own
//     bar, in session_tz (session_argument_bar); intraday timeframes sit
//     on the symbol's own HTF grid (the seven-argument forms) or the epoch
//     grid (these) and never depend on tz.
//
// When a 2-arg call passes a timezone-looking string in the session slot (and
// no explicit tz), TV treats it as an invalid session and ignores it: drop the
// session (no filter) but leave tz at the chart/UTC default — do NOT adopt the
// string as the timezone. The daily boundary then rolls at the chart/exchange
// (UTC) timezone, matching TradingView.
//
// Each of the three is one of the call's own strings or a constant, named by
// reference, so a call (once per bar per site) copies none.
struct SessionTzTriple {
    const std::string& session;
    const std::string& session_tz;
    const std::string& tf_tz;
};

static SessionTzTriple resolve_session_tz(const std::string& session,
                                          const std::string& tz_in,
                                          const std::string& syminfo_tz) {
    static const std::string kNoSession;
    static const std::string kUtc = "UTC";
    const bool explicit_tz = !tz_in.empty();
    return {!explicit_tz && session_arg_is_timezone(session) ? kNoSession : session,
            explicit_tz ? tz_in : !syminfo_tz.empty() ? syminfo_tz : kUtc,
            explicit_tz ? tz_in : kUtc};
}

// The timeframe a time()/time_close() call reads: its own, else the chart's,
// else "1".
static const std::string& call_timeframe(const std::string& tf_in, const std::string& chart_tf) {
    static const std::string kOneMinute = "1";
    return !tf_in.empty() ? tf_in : !chart_tf.empty() ? chart_tf : kOneMinute;
}

// time() / time_close() of a valid session argument: the session's own bar of
// `tf` (session_argument_bar for D / W / M, session_argument_intraday_bar for
// an intraday tf), na when no bar holds `bar_ms`. False when `tf` is neither.
static bool session_argument_time(int64_t bar_ms, const std::string& tf,
                                  const std::string& session, const std::string& session_tz,
                                  const std::string& chart_tf, bool close, int64_t& out,
                                  bool transition_clock = true) {
    int64_t open_ms = 0;
    int64_t close_ms = 0;
    bool held = false;
    const CalendarPeriod cp = calendar_period_for(tf);
    if (cp != CalendarPeriod::NONE) {
        held = session_argument_bar(bar_ms, session, session_tz, cp,
                                    tf_is_daily_or_higher(chart_tf), open_ms, close_ms);
    } else {
        const int sec = tf_to_seconds(tf);
        if (sec <= 0 || sec >= kSecPerDay)
            return false;
        held = session_argument_intraday_bar(bar_ms, session, session_tz,
                                             static_cast<int64_t>(sec) * 1000, open_ms, close_ms,
                                             transition_clock);
    }
    out = !held ? na<int64_t>() : close ? close_ms : open_ms;
    return true;
}

int64_t timeframe_time(int64_t bar_ms,
                  const std::string& tf_in,
                  const std::string& session,
                  const std::string& tz_in,
                  const std::string& chart_tf,
                  const std::string& syminfo_tz) {
    const std::string& tf = call_timeframe(tf_in, chart_tf);

    const SessionTzTriple resolved = resolve_session_tz(session, tz_in, syminfo_tz);
    const std::string& sess = resolved.session;
    const std::string& session_tz = resolved.session_tz;
    const std::string& tf_tz = resolved.tf_tz;

    int64_t value = 0;
    if (!sess.empty() && session_argument_time(bar_ms, tf, sess, session_tz, chart_tf, false, value))
        return value;
    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    return compute_tf_open_ms(bar_ms, tf, tf_tz, "UTC", "");
}

static int64_t session_time_close_reading(int64_t bar_ms,
                                         const std::string& tf_in,
                                         const std::string& session,
                                         const std::string& tz_in,
                                         const std::string& chart_tf,
                                         const std::string& syminfo_tz,
                                         bool transition_clock) {
    const std::string& tf = call_timeframe(tf_in, chart_tf);

    const SessionTzTriple resolved = resolve_session_tz(session, tz_in, syminfo_tz);
    const std::string& sess = resolved.session;
    const std::string& session_tz = resolved.session_tz;
    const std::string& tf_tz = resolved.tf_tz;

    int64_t value = 0;
    if (!sess.empty() && session_argument_time(bar_ms, tf, sess, session_tz, chart_tf, true, value,
                                               transition_clock))
        return value;
    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    int64_t t_open = compute_tf_open_ms(bar_ms, tf, tf_tz, "UTC", "");
    return compute_tf_close_ms(t_open, tf, tf_tz);
}

int64_t timeframe_time_close(int64_t bar_ms,
                        const std::string& tf_in,
                        const std::string& session,
                        const std::string& tz_in,
                        const std::string& chart_tf,
                        const std::string& syminfo_tz) {
    return session_time_close_reading(bar_ms, tf_in, session, tz_in, chart_tf, syminfo_tz, true);
}

namespace detail {

int64_t symbol_session_time_close(int64_t bar_ms, const std::string& tf,
                                  const std::string& session, const std::string& tz,
                                  const std::string& chart_tf) {
    return session_time_close_reading(bar_ms, tf, session, tz, chart_tf, std::string(), false);
}

}  // namespace detail

// Symbol-clock forms. Without a session argument the D/W/M bar open is the
// SYMBOL's bar (session-day keyed — 17:00 ET on OANDA forex, 09:30 ET RTH on
// equities, UTC midnight on a 24x7 UTC symbol), never a UTC calendar-day
// floor. A VALID session argument defines the bar itself: the session's own
// D/W/M bar in its timezone (session_argument_bar) -- `time("D",
// "0000-2359", "America/New_York")` on a UTC crypto symbol rolls at New York
// midnight (lukeborgerding-orb-avwap-retest anchors a manual VWAP on
// ta.change() of exactly that and only matches TV's tape 100% with the NY
// roll, 18% with the symbol's UTC roll), and `time("D", "1700-2400")` on
// OANDA:XAUUSD opens at 17:00 New York (w12-tfd-xau15). A timezone-looking
// string in the session slot is an invalid session (dropped by
// resolve_session_tz) and falls back to the symbol clock. An intraday tf
// without a session argument is the symbol's day-stamp-anchored HTF grid
// bucket: TradingView's time("60") on NYSE:F 15 is 09:30 / 10:30 / .. /
// 15:30 ET, time("240") on NSE:NIFTY 09:15 / 13:15 IST and on OANDA:XAUUSD
// 17:00 ET + 4h*k, 00:00 UTC + 4h*k only on 24x7 UTC (pin-time-hours,
// 2025-04-01..07-01) — the very grid request.security aggregates on
// (session_intraday_bucket_open_ms), one grid in one place; with one, the
// session's own grid (session_argument_intraday_bar).
static bool symbol_clock_applies(const std::string& resolved_session,
                                 CalendarPeriod cp) {
    return cp != CalendarPeriod::NONE && resolved_session.empty();
}

// The close of the symbol's D / W / M period that a daily-or-higher chart bar
// stamped at `bar_ms` holds: TradingView closes it at the period's last
// traded close, to the millisecond, whatever time of day the stamp reads --
// OANDA:XAUUSD's 17:00 ET daily stamps, inside the 1800-1700 session's break,
// close at the next 17:00 ET and its weeks on Friday 17:00 ET; NASDAQ:AAPL's
// days at 16:00 ET and its weeks on Friday 16:00 ET; BINANCE:BTCUSDT's at the
// next 00:00 UTC (lab tv w11-tclose3-*, tests/fixtures/daily_break_close, and
// w12-tclose-*1d, tests/fixtures/time_close_function). The chart's
// time_close (PineStrategyHost::chart_time_close) and time_close("D"/"W"/"M")
// on such a chart read it alike. A stamp outside its session -- a daily bar a
// feed stamps at midnight, before the open or after the close -- is the bar
// of the session it opens, as the 17:00 ET break stamp is
// (session_covered_instant_ms), so it closes with that session, never before
// its own time, and a stamp exactly at a close is the bar of the session that
// closes there; a week or month dated on a weekend its session does not trade
// (a Sunday-dated equity week) sits after its period's last traded close and
// is the next period's. Exchange holidays and early closes are not modelled
// (session_period_last_traded_close_ms): TradingView closes AAPL's 2025-07-03
// at 13:00 ET.
// The stamp of the first session day at or after the symbol-clock D stamp
// `stamp` that trades: its trading date is a day of the session's list, or,
// for a session with trading hours and no list, a weekday (the weekday rule
// of session_period_last_traded_close_ms). TradingView opens a month at its
// first traded session -- OANDA:XAUUSD's February 2025 at Sunday 02-02
// 17:00 ET, NASDAQ:AAPL's March at Monday 03-03 09:30 ET, CAPITALCOM:BTCUSD's
// (1700-1700 every day) at Friday 01-31 17:00 ET -- and on an intraday chart
// closes it where the next month's opens (w12-tclose-*, tests/fixtures/
// time_close_function and dst_day_close), where the nominal calendar reads
// the session of the 1st even on a day that does not trade.
static int64_t first_traded_day_stamp_ms(int64_t stamp, const std::string& tz,
                                         const std::string& session) {
    if (session.empty() || session == "24x7")
        return stamp;
    // Every bar of a month asks for the same few stamps (its open, and the
    // next month's for time_close): the last answers are kept.
    struct Memo {
        bool used = false;
        int64_t stamp = 0;
        int64_t answer = 0;
        std::string tz;
        std::string session;
    };
    constexpr int kMemos = 4;
    thread_local Memo memos[kMemos];
    thread_local int next_memo = 0;
    for (const Memo& memo : memos) {
        if (memo.used && memo.stamp == stamp && memo.tz == tz && memo.session == session)
            return memo.answer;
    }
    const int64_t asked = stamp;
    std::string windows;
    std::unordered_set<int> days;
    session_day_list(session, windows, days);
    for (int guard = 0; guard < 7; ++guard) {
        const int64_t close = session_period_close_ms(stamp, tz, session, CalendarPeriod::DAY);
        struct tm local {};
        decompose_ms_local(close - 1, tz, local);
        const bool trades = days.empty() ? local.tm_wday != 0 && local.tm_wday != 6
                                         : days.count(local.tm_wday + 1) != 0;
        if (trades)
            break;
        stamp = session_period_open_ms(session_covered_instant_ms(close, tz, session), tz, session,
                                       CalendarPeriod::DAY);
    }
    Memo& memo = memos[next_memo];
    next_memo = (next_memo + 1) % kMemos;
    memo.used = true;
    memo.stamp = asked;
    memo.answer = stamp;
    memo.tz = tz;
    memo.session = session;
    return stamp;
}

// The symbol's D bar when its session day lasts 24 hours on the wall clock --
// 24x7 or empty, or one window whose end is its start (1700-1700,
// 0000-0000) -- in a zone with daylight saving: the day from one wall-clock
// open to the next, 23 or 25 hours across a switch, where the runtime's
// session day is its open plus 24 hours (lab tv w12-tclose-cap1d and
// w12-tzscan-*, tests/fixtures/dst_day_close: CAPITALCOM:BTCUSD, 1700-1700 New
// York every day, closes its Saturday 2025-03-08 17:00 EST bar at Sunday
// 17:00 EDT; ACTIVTRADES:BTCUSD, Europe/Amsterdam, its Sunday 2025-03-30 00:00
// CET bar at Monday 00:00 CEST). False, and the runtime's day stands, for any
// other session, in UTC, on a day the session's list leaves out, and where
// the chart's native daily partition holds the bar: the feed's stamps and
// merged days are the symbol's D bars there (session_period_open_ms). A day
// of 24 hours reads here too: the runtime's midnight floor puts New York's
// Saturday 23:00 EST before a spring switch in Sunday's day.
static const std::string kAllDaySession = "24x7";

static bool wall_clock_day_bar(int64_t bar_ms, const std::string& tz,
                               const std::string& session, int64_t& open_ms,
                               int64_t& close_ms) {
    if (utc_zone(tz))
        return false;
    if (const NativeDayPartition* p = active_native_day_partition();
        p != nullptr && p->tz == tz && p->session == session
        && native_day_partition_index(*p, bar_ms) >= 0) {
        return false;
    }
    const std::string& spelled = session.empty() ? kAllDaySession : session;
    const ArgCache& cache = arg_cache(spelled, tz);
    if (!cache.ok || cache.parsed.windows.size() != 1
        || cache.parsed.windows[0].length != 24 * 60) {
        return false;
    }
    return session_argument_bar(bar_ms, spelled, tz, CalendarPeriod::DAY, false, open_ms,
                                close_ms);
}

static int64_t chart_period_close_ms(int64_t bar_ms, const std::string& tz,
                                     const std::string& session, CalendarPeriod period) {
    int64_t day_open = 0;
    int64_t day_close = 0;
    if (period == CalendarPeriod::DAY && wall_clock_day_bar(bar_ms, tz, session, day_open, day_close))
        return day_close;
    // A stamp exactly at its session day's close that no session day opens
    // at -- a feed stamping each bar at its close, 16:00 ET on an equity,
    // 16:00 CT on CME's 1700-1600 -- is the bar of the session that closes
    // there, as the kernel reads a raw label (the calendar interval that
    // holds it); OANDA's 17:00 ET, where the next session day's stamp is,
    // opens that session.
    int64_t at = bar_ms;
    if (session_period_close_ms(bar_ms - 1, tz, session, CalendarPeriod::DAY) == bar_ms
        && session_period_open_ms(bar_ms, tz, session, CalendarPeriod::DAY) != bar_ms) {
        at = bar_ms - 1;
    }
    const int64_t covered = session_covered_instant_ms(at, tz, session);
    // A list that trades a Saturday or Sunday is read by the session's own
    // calendar (session_argument_bar): the period's last session day of the
    // list closes it, where the runtime's weekday rule stops on Friday.
    if (period != CalendarPeriod::DAY) {
        std::string windows;
        std::unordered_set<int> days;
        session_day_list(session, windows, days);
        int64_t week_open = 0;
        int64_t week_close = 0;
        if ((days.count(1) != 0 || days.count(7) != 0)
            && session_argument_bar(covered, session, tz, period, true, week_open, week_close)) {
            return week_close;
        }
    }
    // A period whose last traded close is the stamp itself was over when the
    // stamp opened the next session; a close stamp keeps its own period.
    int64_t close = session_period_last_traded_close_ms(covered, tz, session, period);
    if (at != bar_ms ? close < bar_ms : close <= bar_ms) {
        close = session_period_last_traded_close_ms(
            session_period_close_ms(covered, tz, session, period), tz, session, period);
    }
    return close;
}

int64_t timeframe_time(int64_t bar_ms,
                  const std::string& tf_in,
                  const std::string& session,
                  const std::string& tz_in,
                  const std::string& chart_tf,
                  const std::string& sym_tz,
                  const std::string& sym_session) {
    const std::string& tf = call_timeframe(tf_in, chart_tf);

    // Composition with the syminfo session-tz default: the session window
    // is read in the explicit tz, else syminfo.timezone (sym_tz), else UTC;
    // a VALID session under a D/W/M tf is the session's own bar in that
    // zone, while no valid session takes the symbol's own D/W/M bar.
    const SessionTzTriple resolved = resolve_session_tz(session, tz_in, sym_tz);
    const std::string& sess = resolved.session;
    const std::string& session_tz = resolved.session_tz;
    const std::string& tf_tz = resolved.tf_tz;

    int64_t value = 0;
    if (!sess.empty() && session_argument_time(bar_ms, tf, sess, session_tz, chart_tf, false, value))
        return value;
    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    const CalendarPeriod cp = calendar_period_for(tf);
    if (symbol_clock_applies(sess, cp)) {
        int64_t day_open = 0;
        int64_t day_close = 0;
        if (cp == CalendarPeriod::DAY
            && wall_clock_day_bar(bar_ms, sym_tz, sym_session, day_open, day_close)) {
            return day_open;
        }
        const int64_t open = session_period_open_ms(bar_ms, sym_tz, sym_session, cp);
        if (cp != CalendarPeriod::MONTH)
            return open;
        // A bar on a day the list's reading steps over proves the day trades:
        // the month opened by then.
        const int64_t traded = first_traded_day_stamp_ms(open, sym_tz, sym_session);
        return traded <= bar_ms ? traded : open;
    }
    return compute_tf_open_ms(bar_ms, tf, tf_tz, sym_tz, sym_session);
}

int64_t timeframe_time_close(int64_t bar_ms,
                        const std::string& tf_in,
                        const std::string& session,
                        const std::string& tz_in,
                        const std::string& chart_tf,
                        const std::string& sym_tz,
                        const std::string& sym_session) {
    const std::string& tf = call_timeframe(tf_in, chart_tf);

    const SessionTzTriple resolved = resolve_session_tz(session, tz_in, sym_tz);
    const std::string& sess = resolved.session;
    const std::string& session_tz = resolved.session_tz;
    const std::string& tf_tz = resolved.tf_tz;

    int64_t value = 0;
    if (!sess.empty() && session_argument_time(bar_ms, tf, sess, session_tz, chart_tf, true, value))
        return value;
    if (!sess.empty() && !passes_session_filter(sess, session_tz, bar_ms))
        return na<int64_t>();

    const CalendarPeriod cp = calendar_period_for(tf);
    if (symbol_clock_applies(sess, cp)) {
        // The exact boundary, as TradingView reads it (w12-tclose-*,
        // tests/fixtures/time_close_function): on an intraday chart the
        // session day's close, and a week or month closes where the next one
        // opens (OANDA:XAUUSD's week at the next Sunday 17:00 ET); on a
        // daily-or-higher chart the period's last traded close, the chart
        // bar's own reading (XAUUSD's week on Friday 17:00 ET).
        if (tf_is_daily_or_higher(chart_tf))
            return chart_period_close_ms(bar_ms, sym_tz, sym_session, cp);
        int64_t day_open = 0;
        int64_t day_close = 0;
        if (cp == CalendarPeriod::DAY
            && wall_clock_day_bar(bar_ms, sym_tz, sym_session, day_open, day_close)) {
            return day_close;
        }
        const int64_t close = session_period_close_ms(bar_ms, sym_tz, sym_session, cp);
        return cp == CalendarPeriod::MONTH ? first_traded_day_stamp_ms(close, sym_tz, sym_session)
                                           : close;
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
