#include <pineforge/timeframe.hpp>
#include <pineforge/session_time.hpp>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pineforge {

// ─── Auto-detect timeframe from bar timestamps ────────────────────────────────

std::string detect_timeframe(const Bar* bars, int n, int max_samples) {
    if (n < 2) return "1";

    int count = std::min(n - 1, max_samples);
    std::vector<int64_t> deltas;
    deltas.reserve(count);

    for (int i = 0; i < count; ++i) {
        int64_t d = bars[i + 1].timestamp - bars[i].timestamp;
        if (d > 0) deltas.push_back(d);
    }

    if (deltas.empty()) return "1";

    // Compute median delta
    std::sort(deltas.begin(), deltas.end());
    int64_t median_ms = deltas[deltas.size() / 2];

    // Convert to seconds
    int64_t secs = median_ms / 1000;

    // Map to nearest standard TF (TradingView format)
    // Standard TFs in seconds: 1m=60, 3m=180, 5m=300, 15m=900, 30m=1800,
    //   1h=3600, 2h=7200, 4h=14400, 1D=86400, 1W=604800
    struct TFEntry { int64_t seconds; const char* label; };
    static const TFEntry standard_tfs[] = {
        {60,     "1"},
        {180,    "3"},
        {300,    "5"},
        {900,    "15"},
        {1800,   "30"},
        {3600,   "60"},
        {7200,   "120"},
        {14400,  "240"},
        {86400,  "D"},
        {604800, "W"},
    };

    // Find the closest standard TF
    int64_t best_diff = std::abs(secs - standard_tfs[0].seconds);
    const char* best_label = standard_tfs[0].label;

    for (const auto& tf : standard_tfs) {
        int64_t diff = std::abs(secs - tf.seconds);
        if (diff < best_diff) {
            best_diff = diff;
            best_label = tf.label;
        }
    }

    return std::string(best_label);
}

// ─── TF string parsing ────────────────────────────────────────────────────────

int tf_to_seconds(const std::string& tf) {
    if (tf.empty()) return 0;

    // Check for trailing letter
    char last = tf.back();

    if (last == 'M') {
        return -1;  // calendar-based (monthly)
    }

    if (last == 'D') {
        // "D" or "1D" or "2D" etc.
        if (tf.size() == 1) return 86400;
        int n = std::stoi(tf.substr(0, tf.size() - 1));
        return n * 86400;
    }

    if (last == 'W') {
        // "W" or "1W" etc.
        if (tf.size() == 1) return 604800;
        int n = std::stoi(tf.substr(0, tf.size() - 1));
        return n * 604800;
    }

    if (last == 'S') {
        // Pine sub-minute literals: "15S", "30S", etc. Bare "S" has no
        // canonical meaning in Pine; reject by returning 0 so callers
        // treat it as a parse failure.
        if (tf.size() == 1) return 0;
        int n = std::stoi(tf.substr(0, tf.size() - 1));
        return n;
    }

    // All-numeric: minutes
    int minutes = std::stoi(tf);
    return minutes * 60;
}

int tf_ratio(const std::string& input_tf, const std::string& target_tf) {
    int input_s = tf_to_seconds(input_tf);
    int target_s = tf_to_seconds(target_tf);

    // If target is calendar-based (monthly), return -1
    if (target_s < 0) return -1;

    // If input is calendar-based, that's unusual but treat as error
    if (input_s <= 0) return -2;

    if (target_s < input_s) return -2;  // target < input: error
    if (target_s == input_s) return 1;

    return target_s / input_s;
}

// ─── Calendar boundary detection ───────────────────────────────────────────────

CalendarPeriod calendar_period_for(const std::string& tf) {
    if (tf.empty()) return CalendarPeriod::NONE;
    char last = tf.back();
    if (last == 'M') return CalendarPeriod::MONTH;
    if (last == 'W') return CalendarPeriod::WEEK;
    if (last == 'D') return CalendarPeriod::DAY;
    // Numeric (minutes): not calendar-based in the sense of boundary detection,
    // but for aggregation purposes we don't use CALENDAR mode for minute TFs.
    return CalendarPeriod::NONE;
}

static void decompose_utc(int64_t ms, struct tm& out) {
    time_t secs = static_cast<time_t>(ms / 1000);
    gmtime_r(&secs, &out);
}

// ─── Symbol-clock anchoring ────────────────────────────────────────────────────
//
// HTF bucket keys for session symbols run on the exchange clock, not the raw
// epoch grid: TradingView anchors daily boundaries at symbol-local midnight
// and intraday buckets offset by the session open (09:30 on equities), which
// is why a 4h request.security on a stock does not group at 12:00/16:00 UTC.
// With tz=UTC and no session every helper below reduces to plain epoch
// arithmetic — bit-identical to the previous behavior the corpus pins.

static int64_t utc_floor_day_ms(int64_t ms) {
    return (ms / kMsPerDay) * kMsPerDay;
}

/// Offset (ms) that tz adds to wall-clock time at `ms`. Cached per local day:
/// the value only changes at DST edges, and calendar_day_open_local_ms keeps
/// its own UTC integer fast path, so the hot loop never enters ScopedTimezone.
static int64_t tz_offset_ms(int64_t ms, const std::string& tz) {
    if (tz.empty() || tz == "UTC" || tz == "Etc/UTC") return 0;
    // Memoize per UTC DAY, checked BEFORE the slow ScopedTimezone/mktime
    // path: one miss per symbol-day instead of per call/hour. DST
    // transitions land on wall-clock hours while session markets are closed
    // (02:00 local Sunday), so the first bar seen on each UTC date carries
    // the offset every traded bar of that date uses.
    //
    // A few direct-mapped slots instead of one: the session-day anchors
    // below resolve the offset of the period OPEN's UTC day as well as the
    // query bar's (a Tuesday 03:00 ET bar belongs to the session that opened
    // Monday 17:00 ET, a different UTC date), and a single slot would thrash
    // between the two on every call. Pure cache — values are identical.
    // Slots are keyed by (UTC day, tz): a process that touches two zones
    // (unit tests; multi-symbol hosts) must not read one zone's offset for
    // the other.
    struct Slot { int64_t key = -1, val = 0; std::string tz; };
    constexpr int kSlots = 8;
    thread_local Slot cache[kSlots];
    const int64_t dayk = utc_floor_day_ms(ms);
    Slot& slot = cache[static_cast<size_t>(((dayk / kMsPerDay) % kSlots + kSlots) % kSlots)];
    if (dayk == slot.key && slot.tz == tz) return slot.val;
    // Sample at MID-UTC-day: pairing the LOCAL midnight with the query
    // instant's UTC floor is wrong whenever the local date straddles UTC
    // midnight (FX evening bars are still the previous local date), which
    // seeded whole UTC days with a negative offset and split every HTF
    // candle in two. Midday always belongs to the local date whose true
    // zone offset this cache stores.
    int64_t day_open = calendar_day_open_local_ms(dayk + kMsPerDay / 2, tz);
    slot.val = day_open - dayk;
    slot.key = dayk;
    slot.tz = tz;
    return slot.val;
}

/// Minutes from symbol-local midnight to the first session window's open
/// ("0930-1600..." -> 570). Empty/"24x7" sessions anchor nothing.
static int session_open_offset_minutes(const std::string& session) {
    if (session.empty() || session == "24x7") return 0;
    int digits = 0, value = 0;
    for (char c : session) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (++digits >= 4) break;
        } else if (digits > 0) {
            break;
        }
    }
    return (value / 100) * 60 + (value % 100);
}

/// Timestamp moved onto the exchange clock (tz offset only). This is the
/// clock for CALENDAR periods: TV splits D/W/M at symbol-local midnight.
/// Subtracting the session open here too would SHIFT each session wholesale
/// instead of anchoring it — daily grouping became tz-invariant, which is
/// exactly the bug this split fixes.
static int64_t calendar_clock_ms(int64_t ts, const std::string& tz) {
    return ts - tz_offset_ms(ts, tz);
}

static int session_length_minutes(const std::string& session);

/// Minutes from symbol-local midnight to the symbol's DAY STAMP: the instant
/// its daily bar starts, which is where TradingView anchors the intraday HTF
/// grid ("45", "240", ...) and where the session-day ordinal below rolls.
/// For every declared session that is the session open -- 09:30 ET on NYSE
/// RTH, 09:15 IST on NSE, 17:00 CT on CME 1700-1600, 17:00 ET on OANDA forex
/// 1700-1700, midnight on 24x7 -- with ONE exception the engine has to know
/// by its session string: OANDA's 1800-1700 metals / CFD session (XAUUSD,
/// XAGUSD, ...). It trades from 18:00 ET, but OANDA aligns every
/// instrument's day at the 17:00 ET forex roll -- the stamp its native daily
/// bars carry (engine_aux_security.cpp routes them by the session they
/// COVER for that reason) -- and TradingView inherits that alignment.
/// Pinned with `lab tv` on OANDA:XAUUSD 15, 2025-04-01..04-15
/// (pin-xau-grid-240 / -45: entries on ta.change(time("<tf>"))): "240"
/// buckets open 21:00Z / 01:00Z / 05:00Z / 09:00Z / 13:00Z / 17:00Z -- 17:00
/// EDT, the 17:00-21:00 bucket holding only the 18:00-20:45 bars -- and "45"
/// buckets 21:00Z / 21:45Z / 22:30Z / 23:15Z; the 18:00 ET session-open
/// anchor put them at 22:00Z / 02:00Z / ... and 22:00Z / 22:45Z / 23:30Z.
/// Every 3commas-* XAUUSD DCA probe (request.security "240" RSI gate) shows
/// the same 17:00-ET grid on TV against the engine's 18:00-ET one, the DCA
/// legs agreeing to 1e-6 once the grid does. The stamp is also where the
/// symbol's D bar OPENS -- time("D") on OANDA:XAUUSD 15 reads 17:00 ET on
/// 147/147 entries of pin-time-hours (2025-04-01..07-01), never the 18:00
/// ET session open -- so session_period_open_ms and the D/W/M bucket label
/// sit on it (session_day_stamp_real_ms). The session OPEN keeps the
/// trading window: session_day_open_real_ms / session_covered_instant_ms
/// (where a native 17:00-stamped bar's content lies) and the close
/// arithmetic (open + length = the next 17:00 stamp). No 1800-1700 bar
/// trades in the 17:00-18:00 hour the two clocks attribute differently.
static int session_day_stamp_offset_minutes(const std::string& session) {
    const int open = session_open_offset_minutes(session);
    if (open == 18 * 60
        && (open + session_length_minutes(session)) % 1440 == 17 * 60) {
        return 17 * 60;
    }
    return open;
}

/// Intraday grid clock: exchange-tz seconds since (local-midnight + the
/// symbol's day stamp), so integer division by the bucket width reproduces
/// TV's day-anchored grouping (09:30/13:30 on equities, 17:00 ET on OANDA).
/// The session-day ordinal (session_day_index) runs on the same clock: the
/// day rolls at the stamp, and the nominal session open is added back per
/// day by session_day_open_nominal_ms.
static int64_t intraday_clock_ms(int64_t ts, const std::string& tz,
                                 const std::string& session) {
    return calendar_clock_ms(ts, tz)
               - static_cast<int64_t>(session_day_stamp_offset_minutes(session)) * 60000;
}

int64_t session_intraday_bucket_open_ms(int64_t ms, int64_t bucket_sec,
                                        const std::string& tz,
                                        const std::string& session) {
    if (bucket_sec <= 0) return ms;
    // Floor (not truncate) so the open never lands after `ms` on a negative
    // clock; for every real feed the two agree.
    const int64_t bucket_ms = bucket_sec * 1000;
    const int64_t clock = intraday_clock_ms(ms, tz, session);
    int64_t open_clock = (clock / bucket_ms) * bucket_ms;
    if (clock < 0 && clock % bucket_ms != 0) open_clock -= bucket_ms;
    return ms - (clock - open_clock);
}

/// Minutes from symbol-local midnight to the first session window's CLOSE
/// ("0930-1600..." -> 960). Returns -1 when the session is none/24x7/wrapped
/// (end<=start), in which case eager session-close completion is unavailable
/// and callers fall back to the next-bar-crossing rule.
static int session_close_offset_minutes(const std::string& session) {
    if (session.empty() || session == "24x7") return -1;
    int values[2] = {-1, -1};
    int idx = 0, digits = 0, value = 0;
    for (char c : session) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (++digits == 4) {
                if (idx < 2) values[idx] = value;
                ++idx;
                digits = 0;
                value = 0;
                if (idx > 2) break;
            }
        } else if (digits > 0) {
            digits = 0; value = 0;
        }
        if (idx >= 2) break;
    }
    if (values[0] < 0 || values[1] < 0) return -1;
    int start = (values[0] / 100) * 60 + (values[0] % 100);
    int end = (values[1] / 100) * 60 + (values[1] % 100);
    if (end <= start) return -1;  // wrapped/overnight window: no eager rule
    return end;
}

static const std::string& anchor_utc() {
    static const std::string s = "UTC";
    return s;
}
static const std::string& empty_session() {
    static const std::string s;
    return s;
}

/// Epoch day (days since 1970-01-01) of a proleptic-Gregorian civil date
/// (Howard Hinnant's days_from_civil; m is 1-based).
static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

/// Monday that starts the ISO week of a wall-clock decomposition
/// (continuous across year boundaries; see the historical note below).
static long monday_epoch_day_of(const struct tm& t) {
    long day = days_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    int dow = (t.tm_wday + 6) % 7;   // Mon=0..Sun=6
    return day - dow;
}

// ─── Session-day anchors (shared by security keys, VWAP, tf_change, time()) ──

static int64_t floor_div_day(int64_t ms) {
    int64_t d = ms / kMsPerDay;
    if (ms < 0 && ms % kMsPerDay != 0) --d;
    return d;
}

static bool is_utc_tz(const std::string& tz) {
    return tz.empty() || tz == "UTC" || tz == "Etc/UTC";
}

/// Length of the (first) session window in minutes: 0930-1600 -> 390,
/// 1700-1700 -> 1440 (wraps midnight), 24x7/empty -> 1440.
static int session_length_minutes(const std::string& session) {
    if (session.empty() || session == "24x7") return 1440;
    int values[2] = {-1, -1};
    int idx = 0, digits = 0, value = 0;
    for (char c : session) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (++digits == 4) {
                if (idx < 2) values[idx] = value;
                ++idx;
                digits = 0;
                value = 0;
            }
        } else if (digits > 0) {
            digits = 0; value = 0;
        }
        if (idx >= 2) break;
    }
    if (values[0] < 0 || values[1] < 0) return 1440;
    int start = (values[0] / 100) * 60 + (values[0] % 100);
    int end = (values[1] / 100) * 60 + (values[1] % 100);
    int len = end - start;
    if (len <= 0) len += 1440;   // wraps midnight (1700-1700, 1800-1700)
    return len;
}

/// Days from a session-day's nominal OPEN date to its TradingView trading
/// date: the local date the session closes on (last instant before the
/// close). 1700-1700 -> +1 (the Sunday-17:00 open is Monday's bar);
/// 0930-1600, 0000-0000 and 24x7 -> 0.
static int session_trading_date_shift_days(const std::string& session) {
    const int start = session_open_offset_minutes(session);
    const int len = session_length_minutes(session);
    return (start + len - 1) / 1440;
}

/// Wall-clock instant (UTC-encoded local time) at which session-day `d`
/// opens: d * day + session open. Pure integer math on the exchange clock.
static int64_t session_day_open_nominal_ms(int64_t d, const std::string& session) {
    return d * kMsPerDay
         + static_cast<int64_t>(session_open_offset_minutes(session)) * 60000;
}

/// Real epoch of a nominal wall-clock instant (UTC-encoded local time):
/// mapped back through the zone offset in force at THAT instant (resolved
/// twice so an east-of-UTC zone whose local instant lands on the previous
/// UTC date reads its own offset, not the nominal date's).
static int64_t nominal_to_real_ms(int64_t nominal, const std::string& tz) {
    if (is_utc_tz(tz)) return nominal;
    int64_t real = nominal + tz_offset_ms(nominal, tz);
    real = nominal + tz_offset_ms(real, tz);
    return real;
}

/// Real epoch of session-day `d`'s OPEN: the first traded instant.
static int64_t session_day_open_real_ms(int64_t d, const std::string& tz,
                                        const std::string& session) {
    return nominal_to_real_ms(session_day_open_nominal_ms(d, session), tz);
}

/// Real epoch of session-day `d`'s DAY STAMP: where the symbol's D bar
/// opens (session_day_stamp_offset_minutes) -- the session open everywhere
/// but 1800-1700, whose bar is stamped 17:00 ET an hour before it trades.
static int64_t session_day_stamp_real_ms(int64_t d, const std::string& tz,
                                         const std::string& session) {
    return nominal_to_real_ms(
        d * kMsPerDay
            + static_cast<int64_t>(session_day_stamp_offset_minutes(session)) * 60000,
        tz);
}

// ─── Native daily partition (chart-level; see timeframe.hpp) ──────────────────

namespace {

thread_local const NativeDayPartition* g_active_day_partition = nullptr;

/// The active partition when it keys the very clock a caller asks about:
/// tz and session equal to the partition's (the chart symbol's), so another
/// symbol's request.security clock, and every run without an installed
/// partition, read the nominal rules.
const NativeDayPartition* matching_day_partition(const std::string& tz,
                                                 const std::string& session) {
    const NativeDayPartition* p = g_active_day_partition;
    if (p == nullptr || p->stamps.empty() || p->tz != tz || p->session != session) {
        return nullptr;
    }
    return p;
}

/// The nominal session-day ordinal: floor of the intraday clock's day.
int64_t session_day_index_nominal(int64_t ms, const std::string& tz,
                                  const std::string& session) {
    return floor_div_day(intraday_clock_ms(ms, tz, session));
}

}  // namespace

int native_day_partition_index(const NativeDayPartition& p, int64_t ms) {
    const auto it = std::upper_bound(p.stamps.begin(), p.stamps.end(), ms);
    if (it == p.stamps.begin()) return -1;
    if (it == p.stamps.end() && ms >= p.last_bound) return -1;
    return static_cast<int>(it - p.stamps.begin()) - 1;
}

const NativeDayPartition* set_active_native_day_partition(const NativeDayPartition* p) {
    const NativeDayPartition* prev = g_active_day_partition;
    g_active_day_partition = (p != nullptr && !p->stamps.empty()) ? p : nullptr;
    return prev;
}

const NativeDayPartition* active_native_day_partition() {
    return g_active_day_partition;
}

int64_t session_day_index(int64_t ms, const std::string& tz,
                          const std::string& session) {
    // Under the chart's native daily partition the ordinal is the period's
    // TRADE day -- the session-day of its last chart bar -- so the merged
    // Memorial-Day bar (Sun 17:00 CT through Tue 16:00) is one day for
    // ta.vwap's anchor and the D/W/M keys, and a period's ordinal equals the
    // nominal one wherever the two calendars agree.
    if (const NativeDayPartition* p = matching_day_partition(tz, session)) {
        const int k = native_day_partition_index(*p, ms);
        if (k >= 0) return p->trade_day[static_cast<std::size_t>(k)];
    }
    return session_day_index_nominal(ms, tz, session);
}

/// Epoch day (days since 1970-01-01) of the first session-day of the
/// D/W/M period containing session-day `d`.
static int64_t session_period_first_day(int64_t d, const std::string& session,
                                        CalendarPeriod period) {
    if (period == CalendarPeriod::DAY || period == CalendarPeriod::NONE) return d;
    const int shift = session_trading_date_shift_days(session);
    const int64_t td = d + shift;                       // trading date (epoch day)
    if (period == CalendarPeriod::WEEK) {
        // 1970-01-01 (day 0) was a Thursday: tm_wday 4. Monday-start weeks
        // over trading dates: forex Sun-17:00 opens are Monday's trading
        // date and start the week; equities / 24x7 start Monday 09:30 / 00:00.
        const int wday = static_cast<int>(((td + 4) % 7 + 7) % 7);   // 0=Sun
        const int days_from_mon = (wday + 6) % 7;
        return td - days_from_mon - shift;
    }
    // MONTH: first-of-month of the trading date.
    time_t secs = static_cast<time_t>(td * kSecPerDay);
    struct tm g {};
    gmtime_r(&secs, &g);
    return td - (g.tm_mday - 1) - shift;
}

int64_t session_period_open_ms(int64_t ms, const std::string& tz,
                               const std::string& session,
                               CalendarPeriod period) {
    if (period == CalendarPeriod::NONE) return ms;
    // Under the chart's native daily partition the D bar opens at its native
    // stamp (time("D") on the Mon 05-26 17:00 CT reopen reads Sun 05-25
    // 17:00, the merged bar's stamp) and a W / M bar on its group's first
    // stamp.
    if (const NativeDayPartition* p = matching_day_partition(tz, session)) {
        const int k = native_day_partition_index(*p, ms);
        if (k >= 0) {
            const auto idx = static_cast<std::size_t>(k);
            if (period == CalendarPeriod::DAY) return p->stamps[idx];
            if (period == CalendarPeriod::WEEK) return p->week_open[idx];
            return p->month_open[idx];
        }
    }
    // The period's bar opens where its first session-day's D bar does: the
    // day stamp (the session open on every session but 1800-1700).
    const int64_t d = session_day_index(ms, tz, session);
    return session_day_stamp_real_ms(session_period_first_day(d, session, period),
                                     tz, session);
}

/// Epoch day of the first session-day of the W/M period FOLLOWING the one
/// that contains session-day `d`.
static int64_t session_period_next_first_day(int64_t d, const std::string& session,
                                             CalendarPeriod period) {
    const int64_t first = session_period_first_day(d, session, period);
    if (period == CalendarPeriod::WEEK) return first + 7;
    // First session-day of the NEXT month: step a trading date into the
    // following month and re-anchor.
    const int shift = session_trading_date_shift_days(session);
    const int64_t td = first + shift;
    time_t secs = static_cast<time_t>(td * kSecPerDay);
    struct tm g {};
    gmtime_r(&secs, &g);
    int y = g.tm_year + 1900, m = g.tm_mon + 2;   // next month, 1-based
    if (m > 12) { m = 1; ++y; }
    return days_from_civil(y, m, 1) - shift;
}

/// Exclusive close (real epoch ms) of session-day `d`: its open plus the
/// session length (16:00 ET on equities, the next 17:00 ET on forex).
static int64_t session_day_close_real_ms(int64_t d, const std::string& tz,
                                         const std::string& session) {
    // Session close on the same wall clock as the open (a DST step inside
    // a session never happens while a market is open).
    return session_day_open_real_ms(d, tz, session)
         + static_cast<int64_t>(session_length_minutes(session)) * 60000;
}

bool build_native_day_partition(NativeDayPartition& out,
                                const std::string& tz,
                                const std::string& session,
                                const std::vector<int64_t>& stamps,
                                const Bar* input_bars, int n_input) {
    out = NativeDayPartition{};
    if (stamps.empty()) return false;
    for (std::size_t k = 1; k < stamps.size(); ++k) {
        if (stamps[k] <= stamps[k - 1]) return false;
    }
    if (input_bars == nullptr || n_input < 0) n_input = 0;
    NativeDayPartition p;
    p.tz = tz;
    p.session = session;
    p.stamps = stamps;
    // The last native bar reaches at least its own nominal session-day; a
    // later input bar has no stamp to prove more and keeps the nominal
    // calendar (TimeframeAggregator::set_native_periods' last bound).
    p.last_bound = session_day_close_real_ms(
        session_day_index_nominal(stamps.back(), tz, session), tz, session);
    const std::size_t n = stamps.size();
    p.trade_day.reserve(n);
    p.week_open.reserve(n);
    p.month_open.reserve(n);
    // Trade instants in one merge pass: the last input bar before the next
    // stamp (before the last bound for the last stamp), the stamp itself
    // when no input bar lies in the period.
    int j = 0;
    int64_t prev_week = 0, prev_month = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const int64_t stamp = stamps[k];
        const int64_t next = (k + 1 < n) ? stamps[k + 1] : p.last_bound;
        int64_t last_in_period = stamp;
        while (j < n_input && input_bars[j].timestamp < next) {
            if (input_bars[j].timestamp >= stamp) {
                last_in_period = input_bars[j].timestamp;
            }
            ++j;
        }
        const int64_t td = session_day_index_nominal(last_in_period, tz, session);
        p.trade_day.push_back(td);
        // A native bar's W / M group is the nominal period of its trade day;
        // consecutive stamps of one group share the group's first stamp. The
        // FIRST stamp cannot prove it opens its group (the feed may start
        // mid-week / mid-month): that group opens on the nominal first
        // session-day's stamp when that lies at or before the stamp, on the
        // stamp itself otherwise (a feed starting on a holiday session that
        // belongs to the next week).
        const int64_t week = session_period_first_day(td, session, CalendarPeriod::WEEK);
        const int64_t month = session_period_first_day(td, session, CalendarPeriod::MONTH);
        if (k == 0) {
            p.week_open.push_back(std::min(
                stamp, session_day_stamp_real_ms(week, tz, session)));
            p.month_open.push_back(std::min(
                stamp, session_day_stamp_real_ms(month, tz, session)));
        } else {
            p.week_open.push_back(week == prev_week ? p.week_open.back() : stamp);
            p.month_open.push_back(month == prev_month ? p.month_open.back() : stamp);
        }
        prev_week = week;
        prev_month = month;
    }
    out = std::move(p);
    return true;
}

int64_t session_covered_instant_ms(int64_t ms, const std::string& tz,
                                   const std::string& session) {
    const int64_t d = session_day_index(ms, tz, session);
    // Before the session-day's open -- the 17:00-18:00 ET hour between the
    // 1800-1700 day stamp and its session open, where OANDA stamps the daily
    // bar: the stamp covers the session about to open.
    const int64_t open = session_day_open_real_ms(d, tz, session);
    if (ms < open) return open;
    // Inside the session-day (before its exclusive close): covered as-is.
    // ""/24x7 sessions close exactly at the next open, so the gap is empty
    // and every timestamp takes this branch.
    if (ms < session_day_close_real_ms(d, tz, session)) return ms;
    // In the gap after the close: the stamp covers the next session.
    return session_day_open_real_ms(d + 1, tz, session);
}

int64_t session_period_close_ms(int64_t ms, const std::string& tz,
                                const std::string& session,
                                CalendarPeriod period) {
    if (period == CalendarPeriod::NONE) return ms;
    // Under the chart's native daily partition a W / M bar closes where the
    // next group's first stamp opens, when the partition holds it; the D bar
    // (and a group the partition does not close) closes on its trade day's
    // nominal session-day close through the partitioned ordinal below.
    if (const NativeDayPartition* p = matching_day_partition(tz, session)) {
        const int k = native_day_partition_index(*p, ms);
        if (k >= 0 && period != CalendarPeriod::DAY) {
            const std::vector<int64_t>& group =
                period == CalendarPeriod::WEEK ? p->week_open : p->month_open;
            const std::size_t n = p->stamps.size();
            for (std::size_t j = static_cast<std::size_t>(k) + 1; j < n; ++j) {
                if (group[j] != group[static_cast<std::size_t>(k)]) return p->stamps[j];
            }
        }
    }
    const int64_t d = session_day_index(ms, tz, session);
    if (period == CalendarPeriod::DAY) return session_day_close_real_ms(d, tz, session);
    // W/M close on the next period's first D bar open -- its day stamp.
    return session_day_stamp_real_ms(session_period_next_first_day(d, session, period),
                                     tz, session);
}

/// True when the run declares a real exchange session (equities RTH, forex
/// 1700-1700). ""/"24x7" feeds anchor nothing and keep every integer fast
/// path — the corpus regime must stay bit-identical.
static bool has_trading_session(const std::string& session) {
    return !session.empty() && session != "24x7";
}

/// Exclusive close (real epoch ms) of the LAST TRADED session-day of the
/// D/W/M period containing `ms`. TradingView finalizes a D/W/M bar on the
/// last chart bar that belongs to it, and on exchange-calendar symbols the
/// period's last calendar day is frequently not a trading day: an equity
/// week ends Friday 16:00 (Saturday holds no session), a month closing on a
/// weekend ends on its last Friday, and the forex week's Friday-17:00-ET
/// open (Saturday trading date) never trades, so the week ends Friday
/// 17:00 ET. Weekend TRADING dates (Sat/Sun) are skipped; exchange
/// holidays are not modelled (the period then completes lazily on the next
/// period's first bar, exactly as before).
int64_t session_period_last_traded_close_ms(int64_t ms, const std::string& tz,
                                            const std::string& session,
                                            CalendarPeriod period) {
    if (period == CalendarPeriod::NONE) return ms;
    // ""/"24x7" markets trade every calendar day: nothing to skip.
    if (!has_trading_session(session)) return session_period_close_ms(ms, tz, session, period);
    // Under the chart's native daily partition a W / M group the partition
    // closes ends on its last native day's trade-day close.
    if (const NativeDayPartition* p = matching_day_partition(tz, session)) {
        const int k = native_day_partition_index(*p, ms);
        if (k >= 0 && period != CalendarPeriod::DAY) {
            const std::vector<int64_t>& group =
                period == CalendarPeriod::WEEK ? p->week_open : p->month_open;
            const std::size_t n = p->stamps.size();
            std::size_t last = static_cast<std::size_t>(k);
            while (last + 1 < n && group[last + 1] == group[static_cast<std::size_t>(k)]) ++last;
            if (last + 1 < n) {
                return session_day_close_real_ms(p->trade_day[last], tz, session);
            }
        }
    }
    const int64_t d = session_day_index(ms, tz, session);
    if (period == CalendarPeriod::DAY) return session_day_close_real_ms(d, tz, session);
    const int shift = session_trading_date_shift_days(session);
    int64_t last = session_period_next_first_day(d, session, period) - 1;
    for (int guard = 0; guard < 7; ++guard) {
        const int64_t td = last + shift;                          // trading date
        const int wday = static_cast<int>(((td + 4) % 7 + 7) % 7);   // 0=Sun..6=Sat
        if (wday != 0 && wday != 6) break;
        --last;
    }
    return session_day_close_real_ms(last, tz, session);
}

/// Period key for D/W/M attribution by SESSION-DAY: every bar belongs to the
/// session that contains it, and that session belongs to the calendar period
/// of its TRADING date (see session_trading_date_shift_days). Forex weeks
/// therefore start at the weekend-open session (TV's Sun-17:00-ET week) and
/// forex months at the session closing on the 1st, while equity/24x7 feeds —
/// whose sessions open and close on one local date — resolve exactly as
/// before (weeks stay Monday-partitioned over traded days; months unchanged).
static long session_period_key(int64_t ms, const std::string& tz,
                               const std::string& session,
                               CalendarPeriod period) {
    const int64_t day_idx = session_day_index(ms, tz, session);
    return static_cast<long>(session_period_first_day(day_idx, session, period));
}

bool crosses_boundary(int64_t prev_ms, int64_t curr_ms, CalendarPeriod period) {
    return crosses_boundary(prev_ms, curr_ms, period, "UTC", "");
}

bool crosses_boundary(int64_t prev_ms, int64_t curr_ms, CalendarPeriod period,
                      const std::string& tz, const std::string& session) {
    // Calendar periods split on the SESSION-DAY clock: TradingView's forex
    // daily candle runs 17:00-ET -> 17:00-ET, so the day key must advance at
    // the session open, not at symbol-local midnight (equities coincide,
    // which is why an AAPL-only probe cannot discriminate this).
    struct tm prev_tm, curr_tm;
    const int64_t pf_prev_clock = intraday_clock_ms(prev_ms, tz, session);
    const int64_t pf_curr_clock = intraday_clock_ms(curr_ms, tz, session);
    if (getenv("PF_SEC_TRACE") && curr_ms - prev_ms <= 3600000
        && (curr_ms - prev_ms != 0)) {
        std::fprintf(stderr,
            "[xsb] prev=%lld clock=%lld | curr=%lld clock=%lld\n",
            (long long)prev_ms, (long long)pf_prev_clock,
            (long long)curr_ms, (long long)pf_curr_clock);
    }
    decompose_utc(pf_prev_clock, prev_tm);
    decompose_utc(pf_curr_clock, curr_tm);

    switch (period) {
        case CalendarPeriod::DAY:
            // Under the chart's native daily partition two instants share a
            // D period exactly when their partitioned ordinals agree (the
            // Mon 05-26 17:00 CT reopen stays in Sunday's stamp); an instant
            // the partition does not hold keeps its nominal ordinal, which a
            // held period's trade day never repeats.
            if (const NativeDayPartition* p = matching_day_partition(tz, session)) {
                if (native_day_partition_index(*p, prev_ms) >= 0
                    || native_day_partition_index(*p, curr_ms) >= 0) {
                    return session_day_index(prev_ms, tz, session)
                           != session_day_index(curr_ms, tz, session);
                }
            }
            return prev_tm.tm_yday != curr_tm.tm_yday ||
                   prev_tm.tm_year != curr_tm.tm_year;
        case CalendarPeriod::WEEK:
            // Sunday-start week keys require a declared session (the weekend
            // open defines the FX week). Session-less feeds — 24x7 crypto,
            // bare daily grids — keep the Monday-start epoch math below,
            // which their TV weekly candles use.
            if (!session.empty() && session != "24x7") {
                return session_period_key(prev_ms, tz, session, period)
                       != session_period_key(curr_ms, tz, session, period);
            }
            return monday_epoch_day_of(prev_tm) != monday_epoch_day_of(curr_tm);
        case CalendarPeriod::MONTH:
            // Session-day attribution reduces to the shifted-instant date when
            // no session offsets the clock, so this is safe unconditionally.
            return session_period_key(prev_ms, tz, session, period)
                   != session_period_key(curr_ms, tz, session, period);
        case CalendarPeriod::NONE:
            return false;
    }
    return false;
}

// ─── tf_change ────────────────────────────────────────────────────────────────

bool tf_change(int64_t prev_ms, int64_t curr_ms, const std::string& tf) {
    return tf_change(prev_ms, curr_ms, tf, "UTC", "");
}

bool tf_change(int64_t prev_ms, int64_t curr_ms, const std::string& tf,
               const std::string& tz, const std::string& session) {
    if (prev_ms == 0 || curr_ms == 0) return false;
    CalendarPeriod period = calendar_period_for(tf);
    if (period != CalendarPeriod::NONE) {
        return crosses_boundary(prev_ms, curr_ms, period, tz, session);
    }
    int secs = tf_to_seconds(tf);
    if (secs <= 0) return false;
    int64_t bucket_ms = static_cast<int64_t>(secs) * 1000;
    return (intraday_clock_ms(prev_ms, tz, session) / bucket_ms) !=
           (intraday_clock_ms(curr_ms, tz, session) / bucket_ms);
}

// ─── TimeframeAggregator ───────────────────────────────────────────────────────

TimeframeAggregator::TimeframeAggregator()
    : mode_(Mode::PASSTHROUGH), ratio_(1) {}

TimeframeAggregator::TimeframeAggregator(int ratio)
    : mode_(Mode::RATIO), ratio_(ratio < 1 ? 1 : ratio) {}

TimeframeAggregator::TimeframeAggregator(const std::string& target_tf,
                                         const std::string& input_tf) {
    CalendarPeriod target_period = calendar_period_for(target_tf);

    if (target_period != CalendarPeriod::NONE) {
        // Calendar-based aggregation (D, W, M targets, or ratio targets
        // that also have calendar semantics like daily from intraday).
        mode_ = Mode::CALENDAR;
        cal_period_ = target_period;
    } else {
        // Both are ratio-based TFs. Compute ratio.
        int r = tf_ratio(input_tf, target_tf);
        if (r > 0) {
            mode_ = Mode::RATIO;
            ratio_ = r;
            target_seconds_ = tf_to_seconds(target_tf);
        } else {
            // Fallback to passthrough on error
            mode_ = Mode::PASSTHROUGH;
            ratio_ = 1;
        }
    }
    int in_s = tf_to_seconds(input_tf);
    if (in_s > 0) input_seconds_ = in_s;
}

TimeframeAggregator::TimeframeAggregator(const std::string& target_tf,
                                         const std::string& input_tf,
                                         const std::string& tz,
                                         const std::string& session)
    : TimeframeAggregator(target_tf, input_tf) {
    anchor_tz_ = tz.empty() ? "UTC" : tz;
    anchor_session_ = session;
}

void TimeframeAggregator::reset_current(const Bar& bar) {
    current_bar_ = bar;
    current_bar_.timestamp = bar_label_ms(bar.timestamp);
    sub_bar_count_ = 1;
    current_emitted_complete_ = false;
}

void TimeframeAggregator::merge_into_current(const Bar& bar) {
    // open stays from the first bar, timestamp stays from first bar
    if (bar.high > current_bar_.high) current_bar_.high = bar.high;
    if (bar.low < current_bar_.low)   current_bar_.low = bar.low;
    current_bar_.close = bar.close;
    current_bar_.volume += bar.volume;
    ++sub_bar_count_;
}

// ─── feed() per-mode helpers (file-local) ─────────────────────────────────────
//
// Three mode branches (PASSTHROUGH / RATIO / CALENDAR) are independent state
// machines big enough to deserve their own functions. They cannot be private
// members on TimeframeAggregator without modifying timeframe.hpp, so they
// live as free helpers and receive a ``FeedState`` reference-pack that
// ``feed()`` populates from its own (private) members. ``feed_reset_current``
// / ``feed_merge_into_current`` mirror the private member-method versions
// above; the duplication is local and trivial (3-5 lines each).

namespace {

struct FeedState {
    Bar& current_bar;
    int& sub_bar_count;
    bool& current_emitted_complete;
    Bar& last_completed_bar;
    bool& has_completed;
    const std::string* anchor_tz = nullptr;
    const std::string* anchor_session = nullptr;
    // Owner, for bar_label_ms(): the bucket a sub-bar opens is stamped with
    // its grid / session-day open, never with the sub-bar's own timestamp.
    const TimeframeAggregator* agg = nullptr;
};

void feed_reset_current(FeedState s, const Bar& bar) {
    s.current_bar = bar;
    // Bucket label (finding 473): TradingView dates an aggregated bar by the
    // bucket it occupies on the symbol-clock grid, not by the first sub-bar
    // that happened to trade in it. A 1m tape whose forex session opens at
    // 17:04 ET (OANDA prints nothing for the first minutes) still yields a
    // 17:00 chart bar on TV; keeping 17:04 here moved every entry / exit
    // booked on that bar four minutes late and broke trade identity.
    // Gap-free feeds are untouched: their first sub-bar IS the bucket open.
    if (s.agg) s.current_bar.timestamp = s.agg->bar_label_ms(bar.timestamp);
    s.sub_bar_count = 1;
    s.current_emitted_complete = false;
}

void feed_merge_into_current(FeedState s, const Bar& bar) {
    // open stays from the first bar, timestamp stays from first bar
    if (bar.high > s.current_bar.high) s.current_bar.high = bar.high;
    if (bar.low  < s.current_bar.low)  s.current_bar.low  = bar.low;
    s.current_bar.close = bar.close;
    s.current_bar.volume += bar.volume;
    ++s.sub_bar_count;
}

AggregatedBar feed_passthrough_mode(const Bar& input_bar, FeedState s) {
    AggregatedBar result;
    result.bar = input_bar;
    result.is_complete = true;
    result.sub_bar_count = 1;
    s.last_completed_bar = input_bar;
    s.has_completed = true;
    s.current_bar = input_bar;
    s.sub_bar_count = 1;
    return result;
}

AggregatedBar feed_ratio_mode(const Bar& input_bar, FeedState s,
                               int ratio, int64_t target_seconds,
                               int64_t input_seconds) {
    AggregatedBar result;
    if (target_seconds > 0 && s.sub_bar_count > 0) {
        // Time-bucket aware ratio mode:
        // - emit completed bars at the LAST sub-bar of the bucket (count hit),
        // - still emit at boundary for sparse/irregular data (partial bucket).
        int64_t bucket_ms = static_cast<int64_t>(target_seconds) * 1000;
        const std::string& atz = s.anchor_tz ? *s.anchor_tz : anchor_utc();
        const std::string& asess = s.anchor_session ? *s.anchor_session : empty_session();
        int64_t curr_bucket = intraday_clock_ms(s.current_bar.timestamp, atz, asess) / bucket_ms;
        const int64_t in_clock = intraday_clock_ms(input_bar.timestamp, atz, asess);
        int64_t next_bucket = in_clock / bucket_ms;
        bool boundary = next_bucket != curr_bucket;

        if (boundary) {
            if (s.current_emitted_complete) {
                feed_reset_current(s, input_bar);
                // The incoming bar can be both the first child of a fresh HTF
                // bucket and the final chart bar of a declared session.  The
                // normal session-close check below is reached only after a
                // same-bucket merge, so this singleton shape used to remain
                // partial until the next session's first bar crossed the
                // boundary.  TradingView finalizes the clipped HTF bucket on
                // this caller.  Keep the eager path limited to a genuinely
                // coarser fixed intraday target on a real session; equal-TF,
                // 24x7 and calendar aggregation retain their existing paths.
                bool singleton_session_final = false;
                if (ratio > 1 && input_seconds > 0
                    && target_seconds < kSecPerDay
                    && has_trading_session(asess)) {
                    const int64_t next_ms =
                        input_bar.timestamp + input_seconds * 1000;
                    singleton_session_final =
                        next_ms >= session_period_last_traded_close_ms(
                            input_bar.timestamp, atz, asess,
                            CalendarPeriod::DAY);
                }
                if (singleton_session_final) {
                    s.last_completed_bar = s.current_bar;
                    s.has_completed = true;
                    s.current_emitted_complete = true;
                }
                result.bar = s.current_bar;
                result.is_complete = singleton_session_final;
                result.sub_bar_count = s.sub_bar_count;
                return result;
            }
            s.last_completed_bar = s.current_bar;
            s.has_completed = true;
            result.bar = s.last_completed_bar;
            result.is_complete = true;
            result.sub_bar_count = s.sub_bar_count;
            feed_reset_current(s, input_bar);
            return result;
        }

        feed_merge_into_current(s, input_bar);
        // A bucket finalizes exactly once. The count rule is the gap-free
        // fast path; the two rules below catch buckets the count never
        // reaches. Guarding all of them on !current_emitted_complete keeps a
        // bucket that was finalized early (real end / session close) from
        // being emitted a second time should its count still be reached by
        // later same-bucket bars (feeds with after-close bars, a DST
        // fall-back hour on a 24x7 non-UTC clock); for a count-finalized
        // bucket the count is never matched again, so the gap-free path is
        // bit-identical.
        bool complete = false;
        if (!s.current_emitted_complete) {
            complete = (s.sub_bar_count == ratio);
            // Real-end completion (finding 467): TradingView finalizes an
            // intraday HTF bucket on the chart bar whose close reaches the
            // bucket's end, whether or not every sub-bar traded. A thin
            // bucket — the 60m 17:00-18:00 ET bucket on Dec 25 with
            // 22:00-22:03Z missing holds 56 of 60 minutes — never hits its
            // count, and waiting for the NEXT bucket's first bar (the
            // boundary test above) exposes it one chart bar late: 23:00:00Z
            // lies inside the 23:00 chart bar, not the 22:45 one whose close
            // IS the bucket end. Complete on the input bar whose end
            // (timestamp + input duration, on the same anchor clock the
            // bucket key uses) reaches the bucket end. For a full bucket the
            // last sub-bar's end is the bucket end, so this fires on exactly
            // the bar the count rule fires on — 24x7/UTC gap-free feeds stay
            // bit-identical. A bucket whose LAST sub-bar is missing still
            // completes on the boundary bar; the engine feeds every
            // request.security aggregator before the chart aggregator on
            // each input bar, so the chart bar whose close reaches the
            // bucket end still sees it (see run_aggregation_bar_loop).
            if (!complete && input_seconds > 0) {
                const int64_t end_clock = (next_bucket + 1) * bucket_ms;
                complete = in_clock + input_seconds * 1000 >= end_clock;
            }
            // Session-close completion: an intraday bucket that straddles
            // the session close (RTH '240' 13:30-17:30 holds 10 of 16
            // sub-bars, '60' 15:30-16:30 two of four) never reaches its count
            // NOR its real end inside the session. TradingView clips the
            // bucket at the session close and finalizes it on the session's
            // LAST chart bar (15:45 ET), not on the next session's first bar
            // where the boundary test above would catch it a session late.
            // Declared sessions only: ""/"24x7" feeds keep the count/
            // real-end/boundary rules bit-for-bit, and multi-day ratio
            // targets ("2D") are calendar-sized, not session-sized.
            if (!complete && input_seconds > 0 && target_seconds < kSecPerDay
                && has_trading_session(asess)) {
                const int64_t next_ms = input_bar.timestamp + input_seconds * 1000;
                if (next_ms >= session_period_last_traded_close_ms(
                        input_bar.timestamp, atz, asess, CalendarPeriod::DAY)) {
                    complete = true;
                }
            }
        }
        if (complete) {
            s.last_completed_bar = s.current_bar;
            s.has_completed = true;
            s.current_emitted_complete = true;
        }
        result.bar = s.current_bar;
        result.is_complete = complete;
        result.sub_bar_count = s.sub_bar_count;
        return result;
    }
    // Fallback: original count-based logic.
    if (s.sub_bar_count == 0) {
        // First bar ever
        feed_reset_current(s, input_bar);
    } else if (s.sub_bar_count >= ratio) {
        // Previous cycle completed; start new one
        feed_reset_current(s, input_bar);
    } else {
        feed_merge_into_current(s, input_bar);
    }

    bool complete = (s.sub_bar_count >= ratio);
    if (complete) {
        s.last_completed_bar = s.current_bar;
        s.has_completed = true;
        s.current_emitted_complete = true;
    }

    result.bar = s.current_bar;
    result.is_complete = complete;
    result.sub_bar_count = s.sub_bar_count;
    return result;
}

AggregatedBar feed_calendar_mode(const Bar& input_bar, FeedState s,
                                  CalendarPeriod cal_period,
                                  int64_t input_seconds,
                                  int64_t next_input_ms,
                                  int64_t calling_close_ms) {
    AggregatedBar result;
    const bool pf_tr = getenv("PF_SEC_TRACE") != nullptr;
    if (pf_tr) {
        std::fprintf(stderr,
            "[calmode] ts=%lld first=%lld subs=%d emitted=%d tz='%s' sess='%s'\n",
            (long long)input_bar.timestamp, (long long)s.current_bar.timestamp,
            s.sub_bar_count, s.current_emitted_complete ? 1 : 0,
            s.anchor_tz ? s.anchor_tz->c_str() : "<null>",
            s.anchor_session ? s.anchor_session->c_str() : "<null>");
    }
    if (s.sub_bar_count == 0) {
        // Very first bar
        feed_reset_current(s, input_bar);
        result.bar = s.current_bar;
        result.is_complete = false;
        result.sub_bar_count = s.sub_bar_count;
        return result;
    }

    // Does the new bar fall in a different calendar period than
    // the current group's first bar? (The owner's period key: the native
    // period partition when a native feed installed one, the nominal
    // session calendar otherwise -- see TimeframeAggregator::period_changes.)
    const std::string& atz = s.anchor_tz ? *s.anchor_tz : anchor_utc();
    const std::string& asess = s.anchor_session ? *s.anchor_session : empty_session();
    const bool native_periods = s.agg && s.agg->has_native_periods();
    if (native_periods
            ? s.agg->period_changes(s.current_bar.timestamp, input_bar.timestamp)
            : crosses_boundary(s.current_bar.timestamp, input_bar.timestamp,
                               cal_period, atz, asess)) {
        if (s.current_emitted_complete) {
            feed_reset_current(s, input_bar);
            result.bar = s.current_bar;
            result.is_complete = false;
            result.sub_bar_count = s.sub_bar_count;
            return result;
        }
        // First *actual* bar of the next calendar period: finalize the previous
        // period if it was not already closed by end-of-period projection below.
        int completed_subs = s.sub_bar_count;
        s.last_completed_bar = s.current_bar;
        s.has_completed = true;

        // Start new aggregation with this bar
        feed_reset_current(s, input_bar);

        result.bar = s.last_completed_bar;
        result.is_complete = true;
        result.sub_bar_count = completed_subs;
        if (pf_tr) {
            std::fprintf(stderr,
                "[calmode] BOUNDARY-COMPLETE trigger_ts=%lld bucket_first=%lld close=%.6f subs=%d\n",
                (long long)input_bar.timestamp,
                (long long)s.last_completed_bar.timestamp,
                s.last_completed_bar.close, completed_subs);
        }
        return result;
    }

    // Same calendar period: merge. TradingView-style: the aggregated period is
    // complete once we have processed a bar such that the *next* bar of input_tf
    // duration would cross the calendar boundary (e.g. last 1m of the session
    // completes the daily bar). This matches request.security HTF behavior used
    // in validation; "complete only on the next period's first bar" shifts all HTF
    // series and breaks TV parity.
    //
    // Session symbols never reach that crossing inside the session (an RTH day
    // ends at 16:00 local, far from midnight), so on such feeds the rule above
    // always defers completion to the NEXT session's first bar — exposing the
    // finished daily bucket a full session after TV does. When the run declares
    // a same-day session window, complete eagerly on the bar whose close
    // reaches the session end: that is TV's own bucket-final bar.
    feed_merge_into_current(s, input_bar);
    bool complete = false;
    if (native_periods && next_input_ms > input_bar.timestamp) {
        // TradingView's own period partition is installed and the next input
        // bar is known: the bucket completes on the last input bar before
        // the next native stamp -- and only there. The nominal rules below
        // are not consulted: they would close a CME holiday session at its
        // 12:00 CT pause or split it from the 17:00 reopen, where TradingView
        // keeps one daily bar (lab tv esd pin, 2026-09-05).
        complete = s.agg->period_changes(input_bar.timestamp, next_input_ms);
    } else if (input_seconds > 0) {
        const std::string& atz = s.anchor_tz ? *s.anchor_tz : anchor_utc();
        const std::string& asess = s.anchor_session ? *s.anchor_session : empty_session();
        int64_t next_ms = input_bar.timestamp + input_seconds * 1000;
        complete = crosses_boundary(input_bar.timestamp, next_ms, cal_period,
                                    atz, asess);
        if (!complete && cal_period != CalendarPeriod::NONE) {
            // Eager session-close completion applies only to the period's
            // FINAL session: today's session must be the last one in this
            // day/week/month (same-wall-clock-tomorrow starts a new period).
            const int end_min = session_close_offset_minutes(asess);
            if (end_min > 0
                && crosses_boundary(input_bar.timestamp,
                                    input_bar.timestamp + kMsPerDay,
                                    cal_period, atz, asess)) {
                const int64_t local = calendar_clock_ms(input_bar.timestamp, atz);
                const int64_t pos_ms = local - (local / kMsPerDay) * kMsPerDay
                                     + input_seconds * 1000;
                if (pos_ms >= static_cast<int64_t>(end_min) * 60000) {
                    complete = true;
                }
            }
        }
        if (!complete
            && (cal_period == CalendarPeriod::WEEK
                || cal_period == CalendarPeriod::MONTH)
            && has_trading_session(asess)) {
            // W/M on an exchange-calendar session: the rule above asks
            // whether the same wall clock TOMORROW starts a new period, but
            // Friday + 24h is Saturday — still this week (and a weekend
            // month-end is still this month) — so the week completed on
            // Monday 09:30 instead of Friday 15:45, where TradingView
            // finalizes it (its last chart bar). Complete when the bar's
            // end reaches the close of the period's last TRADED session-day
            // (weekend trading dates skipped).
            //
            // Wrapped sessions (forex 1700-1700) need the same rule: the
            // next-bar crossing above sees Friday 17:00 ET as the
            // Friday-open session-day (Saturday's trading date, same
            // week/month), so W/M completed on Sunday 17:00 and a
            // crossover filled 17:15 where TradingView signals on Friday
            // 16:45 and fills Sunday 17:00. The last traded session-day of
            // the forex week is the Thursday-open one, closing Friday
            // 17:00 ET.
            complete = next_ms >= session_period_last_traded_close_ms(
                input_bar.timestamp, atz, asess, cal_period);
        }
    }
    const bool early_close_completes = !s.agg || s.agg->early_close_completes();
    if (!complete && !native_periods
        && next_input_ms > input_bar.timestamp
        && has_trading_session(asess)
        && (early_close_completes || calling_close_ms > 0)) {
        // The nominal rules above end a period at its scheduled close and
        // model neither early closes nor exchange holidays: on NYSE:F the
        // 13:00 ET half-day of Fri 2025-11-28 left the week (and the day)
        // open after the 12:45 bar and a holiday Friday leaves the week open
        // after Thursday 15:45, so both completed lazily on Monday 09:30 --
        // one chart bar after TradingView, which finalizes a D/W/M bar on
        // the period's LAST chart bar (lab tv wm-w-f15-nov, 2026-09-05: W
        // close 13.28 on the 12:45 bar). A historical run holds its whole
        // feed, so the caller tells us where the next input bar lies: when
        // it opens a new period, this bar was the period's last. Declared
        // sessions only -- a data hole before a 24x7 midnight is a hole,
        // not a close, and 24x7 / UTC feeds stay bit-identical -- and the
        // full-session case is unchanged: the nominal rules already fire on
        // that same bar. Exchange sessions only, where TradingView's own
        // session template carries the early close: on an OTC quote stream
        // (set_early_close_completes(false) -- OANDA:XAUUSD, cfd, 1800-1700
        // ET) the Fri 2025-07-04 data end at 12:45 ET is not a close for
        // TradingView, which surfaces that D, and the week holding it, on
        // the next session's first 18:00 bar: the nominal rules above plus
        // the boundary completion, exactly as before this rule (lab tv
        // oanda pin, ledger log-20260905t034240z-30be11fe, 2026-09-05) --
        // on the 15m chart. What TradingView compares is the CALLING chart
        // bar's time_close against the period's nominal close, and the
        // daily bar of that same early-close day keeps its nominal 17:00
        // ET time_close: on the OANDA:XAUUSD 1D chart "W" advances on the
        // 07-04 daily bar (cW 3336.61, tW Sun 06-29 17:00) as on every
        // regular Friday, "D" is the chart bar itself on 07-04 and on Mon
        // 2026-02-16, and "M" completes on the month's last daily bar (lab
        // tv oanda1d-{jul,feb,novm,decm}, 2026-09-05). So when the caller
        // names that close (the split-feed path: a finer auxiliary slice
        // under a native D/W/M chart bar), an OTC bucket the next input
        // bar leaves completes on this input bar iff the calling bar's
        // close reaches the period's nominal close
        // (session_period_last_traded_close_ms); without it (0: the input
        // bar is the chart bar, whose own end the nominal rules above
        // already tested) the bucket waits, bit-identical to before.
        if (crosses_boundary(input_bar.timestamp, next_input_ms,
                             cal_period, atz, asess)) {
            complete = early_close_completes
                || calling_close_ms >= session_period_last_traded_close_ms(
                       input_bar.timestamp, atz, asess, cal_period);
        }
    }
    if (!complete && native_periods && cal_period == CalendarPeriod::DAY
        && input_seconds > 0 && next_input_ms <= input_bar.timestamp) {
        // The feed's last bar under a native period partition (no next bar
        // to compare): a bar whose close reaches its session-day's close is
        // the day's last chart bar on a wrapped session too -- ES1! "D"
        // completes on a chart ending at Fri 15:45 CT (16:00 close), as
        // TradingView's tape does on that bar (esd-aug, 08-15 15:45). The
        // nominal rules above cannot see it: a 1700-1600 session-day rolls
        // at 17:00, not at its 16:00 close. Without a native feed the feed's
        // last bar keeps the nominal rules bit-identical.
        const std::string& atz = s.anchor_tz ? *s.anchor_tz : anchor_utc();
        const std::string& asess = s.anchor_session ? *s.anchor_session : empty_session();
        complete = input_bar.timestamp + input_seconds * 1000
            >= session_period_last_traded_close_ms(input_bar.timestamp, atz,
                                                   asess, CalendarPeriod::DAY);
    }
    if (complete) {
        s.last_completed_bar = s.current_bar;
        s.has_completed = true;
        s.current_emitted_complete = true;
        result.bar = s.last_completed_bar;
        result.is_complete = true;
        result.sub_bar_count = s.sub_bar_count;
        if (pf_tr) {
            std::fprintf(stderr,
                "[calmode] PROJECTION/EAGER-COMPLETE trigger_ts=%lld bucket_first=%lld close=%.6f subs=%d\n",
                (long long)input_bar.timestamp,
                (long long)s.last_completed_bar.timestamp,
                s.last_completed_bar.close, s.sub_bar_count);
        }
        return result;
    }
    result.bar = s.current_bar;
    result.is_complete = false;
    result.sub_bar_count = s.sub_bar_count;
    return result;
}

}  // namespace

AggregatedBar TimeframeAggregator::feed(const Bar& input_bar) {
    return feed(input_bar, 0, 0);
}

AggregatedBar TimeframeAggregator::feed(const Bar& input_bar,
                                        int64_t next_input_ms) {
    return feed(input_bar, next_input_ms, 0);
}

AggregatedBar TimeframeAggregator::feed(const Bar& input_bar,
                                        int64_t next_input_ms,
                                        int64_t calling_close_ms) {
    FeedState s{current_bar_, sub_bar_count_, current_emitted_complete_,
                last_completed_bar_, has_completed_,
                &anchor_tz_, &anchor_session_, this};
    switch (mode_) {
        case Mode::PASSTHROUGH:
            return feed_passthrough_mode(input_bar, s);
        case Mode::RATIO:
            return feed_ratio_mode(input_bar, s, ratio_, target_seconds_,
                                   input_seconds_);
        case Mode::CALENDAR:
            return feed_calendar_mode(input_bar, s, cal_period_, input_seconds_,
                                      next_input_ms, calling_close_ms);
    }

    // Unreachable
    AggregatedBar result;
    result.bar = input_bar;
    result.is_complete = true;
    result.sub_bar_count = 1;
    return result;
}

Bar TimeframeAggregator::current() const {
    return current_bar_;
}

Bar TimeframeAggregator::last_completed() const {
    return last_completed_bar_;
}

bool TimeframeAggregator::has_pending_partial() const {
    // The time-bucket ratio path is the one whose completions are all
    // guarded by current_emitted_complete_ (feed_ratio_mode), so a bucket
    // finalized here is never emitted a second time; the count-only
    // fallback would re-complete it by count.
    return mode_ == Mode::RATIO && target_seconds_ > 0
        && sub_bar_count_ > 0 && !current_emitted_complete_;
}

AggregatedBar TimeframeAggregator::complete_pending_partial() {
    AggregatedBar result;
    result.bar = current_bar_;
    result.sub_bar_count = sub_bar_count_;
    result.is_complete = false;
    if (!has_pending_partial()) return result;
    last_completed_bar_ = current_bar_;
    has_completed_ = true;
    current_emitted_complete_ = true;
    result.is_complete = true;
    return result;
}

bool TimeframeAggregator::is_active() const {
    return mode_ != Mode::PASSTHROUGH;
}

CalendarPeriod TimeframeAggregator::calendar_period() const {
    return mode_ == Mode::CALENDAR ? cal_period_ : CalendarPeriod::NONE;
}

void TimeframeAggregator::set_native_periods(std::vector<int64_t> stamps,
                                             std::vector<int64_t> trade_instants,
                                             CalendarPeriod feed_period) {
    native_stamps_.clear();
    native_group_open_.clear();
    native_last_bound_ = 0;
    if (mode_ != Mode::CALENDAR || stamps.empty()
        || stamps.size() != trade_instants.size()) {
        return;
    }
    for (std::size_t k = 1; k < stamps.size(); ++k) {
        if (stamps[k] <= stamps[k - 1]) return;
    }
    native_group_open_.reserve(stamps.size());
    // A native bar's W / M group is the nominal period of its TRADE DATE --
    // the session-day it completes on (its last input bar), not the
    // session-day of its stamp: the ES1! bar stamped Thu 07-03 17:00 CT that
    // TradingView runs through the Independence-Day session to Mon 07-07
    // 15:45 is Monday's bar and Monday's week. Consecutive stamps of one
    // nominal period share the group's first stamp as their open; DAY
    // groups are the stamps themselves.
    int64_t prev_period = 0;
    for (std::size_t k = 0; k < stamps.size(); ++k) {
        int64_t open = stamps[k];
        if (cal_period_ != CalendarPeriod::DAY) {
            const int64_t period = session_period_open_ms(
                trade_instants[k], anchor_tz_, anchor_session_, cal_period_);
            if (k > 0 && period == prev_period) open = native_group_open_.back();
            prev_period = period;
        }
        native_group_open_.push_back(open);
    }
    // The last native bar reaches at least its nominal period; a later
    // input bar has no stamp to prove more and keeps the nominal calendar.
    native_last_bound_ = session_period_close_ms(
        stamps.back(), anchor_tz_, anchor_session_,
        feed_period == CalendarPeriod::NONE ? cal_period_ : feed_period);
    native_stamps_ = std::move(stamps);
}

int TimeframeAggregator::native_index(int64_t ms) const {
    const auto it = std::upper_bound(native_stamps_.begin(),
                                     native_stamps_.end(), ms);
    if (it == native_stamps_.begin()) return -1;
    if (it == native_stamps_.end() && ms >= native_last_bound_) return -1;
    return static_cast<int>(it - native_stamps_.begin()) - 1;
}

bool TimeframeAggregator::period_changes(int64_t prev_ms, int64_t curr_ms) const {
    if (mode_ != Mode::CALENDAR) return false;
    if (native_stamps_.empty()) {
        return crosses_boundary(prev_ms, curr_ms, cal_period_, anchor_tz_,
                                anchor_session_);
    }
    return bucket_open_ms(prev_ms) != bucket_open_ms(curr_ms);
}

int64_t TimeframeAggregator::bucket_open_ms(int64_t ms) const {
    switch (mode_) {
        case Mode::CALENDAR: {
            // The native period's W / M group open (its stamp for DAY) when
            // a native feed installed the partition; nominal before the
            // first stamp and without one.
            const int k = native_index(ms);
            if (k >= 0) return native_group_open_[static_cast<std::size_t>(k)];
            return session_period_open_ms(ms, anchor_tz_, anchor_session_,
                                          cal_period_);
        }
        case Mode::RATIO:
            // Same grid feed_ratio_mode keys on (and time("<intraday tf>")
            // reads): exchange-tz ms since local-midnight + day stamp,
            // floored to the bucket width. Count-only ratio: no grid.
            return session_intraday_bucket_open_ms(ms, target_seconds_,
                                                   anchor_tz_, anchor_session_);
        case Mode::PASSTHROUGH:
            return ms;
    }
    return ms;
}

int64_t TimeframeAggregator::bar_label_ms(int64_t ms) const {
    switch (mode_) {
        case Mode::RATIO:
            // The session-anchored intraday grid open (bucket_open_ms), which
            // is `ms` itself whenever the grid-opening sub-bar traded.
            return bucket_open_ms(ms);
        case Mode::CALENDAR: {
            // The D/W/M bar is dated by its first TRADED session-day (a
            // holiday-Monday equity week is Tuesday's bar, exactly as the
            // first-present sub-bar already implied), but within that
            // session-day by the DAY STAMP: the forex daily / weekly bar
            // whose tape starts at 17:04 ET is still the 17:00 ET bar, and
            // the 1800-1700 day that trades from 18:00 ET is its 17:00 ET
            // stamped bar (session_day_stamp_offset_minutes). Under a
            // native partition the day stamp IS the native stamp of the
            // period holding `ms` (the Sun 17:00 CT stamp of a merged
            // Labor-Day bar, through Tuesday).
            const int k = native_index(ms);
            if (k >= 0) return native_stamps_[static_cast<std::size_t>(k)];
            return session_period_open_ms(ms, anchor_tz_, anchor_session_,
                                          CalendarPeriod::DAY);
        }
        case Mode::PASSTHROUGH:
            return ms;
    }
    return ms;
}

} // namespace pineforge
