// time("<tf>") / time_close("<tf>") on the symbol clock: TradingView keys an
// intraday tf on the symbol's DAY-STAMP-anchored HTF grid -- the same grid
// request.security aggregates on (session_intraday_bucket_open_ms) -- and
// time("D") on the D bar's stamp, which on OANDA's 1800-1700 metals session
// is the 17:00 ET forex roll an hour before the session trades.
//
// Pinned with `lab tv` (pin-time-hours, 15m charts, 2025-04-01..07-01, all
// EDT): strategy.entry(qty = hour(time(tf), "America/New_York") * 100 +
// minute(time(tf), "America/New_York") + 10000 / 20000 / 30000 for "D" /
// "240" / "60"), so every entry's Size column is the HHMM (New York) of the
// tf bar open:
//   OANDA:XAUUSD (America/New_York 1800-1700): "D" = 17:00 on 147/147
//     entries (the engine used to report the 18:00 session open); "240" in
//     {01:00, 05:00, 09:00, 13:00, 17:00, 21:00}; "60" every hour on the
//     hour except 17:00 (nothing trades in the 17:00-18:00 break);
//   NYSE:F (America/New_York 0930-1600): "D" = 09:30 on 41/41; "60" in
//     {09:30, 10:30, .., 15:30} -- session-anchored, NOT the epoch hour the
//     engine used to report; "240" in {09:30, 13:30};
//   NSE:NIFTY (Asia/Kolkata 0915-1530): "D" = 23:45 NY (= 09:15 IST) on
//     39/39; "240" in {23:45, 03:45} NY (= 09:15 / 13:15 IST);
//   BINANCE:BTCUSDT (UTC 24x7): "D" = 20:00 NY (= 00:00 UTC) on 19/19 --
//     the epoch grid, where the day stamp is 00:00 UTC;
//   CME_MINI:ES1! and NQ1! (America/Chicago 1700-1600; pin-time-hours-cme,
//     capital 1e14 so every entry is affordable): "D" = 18:00 NY (= 17:00
//     CT) on 147/147 each; "240" in {18:00, 22:00, 02:00, 06:00, 10:00,
//     14:00} NY (= 17:00 / 21:00 / 01:00 / 05:00 / 09:00 / 13:00 CT); "60"
//     every hour on the hour except 17:00 NY (the 16:00-17:00 CT break) --
//     the 17:00-CT session-open grid request.security already used.
//
// Rules pinned here:
//   A. OANDA:XAUUSD: time("D") is the 17:00 ET stamp for every bar of the
//      session, time_close("D") the next stamp; "240" / "60" are the
//      stamp-anchored grid, bit-identical to TimeframeAggregator's bucket
//      and tf_change; the same in EST;
//   B. NYSE:F: "60" / "240" anchored at 09:30 ET; time("D") 09:30 ET;
//   C. NSE:NIFTY: "240" anchored at 09:15 IST; time("D") 09:15 IST;
//   D. 24x7 UTC: every form is the epoch grid, bit-identical to the
//      five-argument (tz-less) forms;
//   E. CME 1700-1600 CT: stamp == open; time("D") is 17:00 CT, "240" / "60"
//      the 17:00-CT grid request.security already used (unchanged);
//      D / W anchors unchanged.
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include <pineforge/na.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/timeframe.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

#define CHECK_EQ_MS(actual, expected)                                          \
    do {                                                                        \
        const int64_t _a = (actual), _e = (expected);                          \
        if (_a != _e) {                                                         \
            std::printf("  FAIL  %s:%d  %s == %s  (got %lld, want %lld)\n",     \
                        __FILE__, __LINE__, #actual, #expected,                 \
                        (long long)_a, (long long)_e);                          \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

namespace {

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (static_cast<int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}

const std::string NY  = "America/New_York";
const std::string CHI = "America/Chicago";
const std::string IST = "Asia/Kolkata";
const std::string UTC = "UTC";
const std::string XAU = "1800-1700";          // OANDA metals / CFDs
const std::string RTH = "0930-1600";          // NYSE / NASDAQ
const std::string NSE = "0915-1530";          // NSE India
const std::string CME = "1700-1600";          // CME Globex (Chicago)
const std::string ALL = "24x7";
const std::string NONE;
const std::string CHART = "15";
const int64_t k15m = 15 * 60 * 1000;
const int64_t k1h  = 60 * 60 * 1000;
const int64_t k4h  = 4 * k1h;
const int64_t k1d  = 24 * k1h;

// The pin strategy's Size column: HHMM of `t` in New York.
int64_t hhmm_ny(int64_t t) { return pine_hour(t, NY) * 100 + pine_minute(t, NY); }

bool in_set(int64_t v, std::initializer_list<int64_t> s) {
    for (int64_t x : s) if (x == v) return true;
    return false;
}

int weekday_utc(int64_t ms) {           // 0 = Sunday .. 6 = Saturday
    const int64_t days = ms / k1d;
    return static_cast<int>(((days + 4) % 7 + 7) % 7);
}

// Symbol-clock time() / time_close() as codegen calls them (no session
// argument, no tz argument, chart tf "15").
int64_t T(int64_t t, const char* tf, const std::string& tz, const std::string& sess) {
    return pine_time(t, tf, "", "", CHART, tz, sess);
}
int64_t TC(int64_t t, const char* tf, const std::string& tz, const std::string& sess) {
    return pine_time_close(t, tf, "", "", CHART, tz, sess);
}
// The tz-less five-argument form: the epoch grid.
int64_t T0(int64_t t, const char* tf) { return pine_time(t, tf, "", "", CHART); }

// One grid: time(tf) is the aggregator's bucket open, and ta.change(time(tf))
// fires exactly where tf_change does. Returns the number of disagreements.
int grid_disagreements(const std::vector<int64_t>& bars, const char* tf,
                       const std::string& tz, const std::string& sess) {
    TimeframeAggregator agg(tf, CHART, tz, sess);
    int bad = 0;
    for (size_t i = 0; i < bars.size(); ++i) {
        const int64_t open = T(bars[i], tf, tz, sess);
        if (open != agg.bucket_open_ms(bars[i])) ++bad;
        if (open > bars[i]) ++bad;
        if (i > 0) {
            const bool changed = open != T(bars[i - 1], tf, tz, sess);
            if (changed != tf_change(bars[i - 1], bars[i], tf, tz, sess)) ++bad;
        }
    }
    return bad;
}

// ─── A. OANDA:XAUUSD, 1800-1700 America/New_York ─────────────────────────────

// One 1800-1700 session of 15m bars from `open_z` (18:00 ET): 92 bars up to
// 16:45 ET; nothing trades in the 17:00-18:00 ET break.
std::vector<int64_t> xau_session(int64_t open_z) {
    std::vector<int64_t> v;
    for (int i = 0; i < 92; ++i) v.push_back(open_z + i * k15m);
    return v;
}

void test_xau_time_d_is_the_1700_stamp() {
    std::printf("A. OANDA:XAUUSD time(\"D\") == 17:00 ET stamp; \"240\" / \"60\" on the stamp grid\n");
    const int64_t mon_open = utc_ms(2025, 3, 31, 22, 0);     // Mon 18:00 EDT
    const int64_t mon_stamp = utc_ms(2025, 3, 31, 21, 0);    // Mon 17:00 EDT
    // The session's first bar: D opens an hour before it, at the stamp.
    CHECK_EQ_MS(T(mon_open, "D", NY, XAU), mon_stamp);
    CHECK_EQ_MS(hhmm_ny(T(mon_open, "D", NY, XAU)), 1700);
    // time_close is the boundary itself (lane W12-ENG-TIME: the exact boundary, lab tv w12-tclose-*, tests/fixtures/time_close_function).
    CHECK_EQ_MS(TC(mon_open, "D", NY, XAU), utc_ms(2025, 4, 1, 21, 0));
    CHECK_EQ_MS(T(mon_open, "1D", NY, XAU), mon_stamp);
    // "240": 21:00Z-anchored (17:00 EDT); the 17:00-21:00 ET bucket holds
    // only the 18:00-20:45 bars.
    CHECK_EQ_MS(T(mon_open, "240", NY, XAU), mon_stamp);
    CHECK_EQ_MS(TC(mon_open, "240", NY, XAU), utc_ms(2025, 4, 1, 1, 0));
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 0, 45), "240", NY, XAU), mon_stamp);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 1, 0), "240", NY, XAU), utc_ms(2025, 4, 1, 1, 0));
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 20, 45), "240", NY, XAU), utc_ms(2025, 4, 1, 17, 0));
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 4, 1, 20, 45), "240", NY, XAU)), 1300);
    // The epoch grid the engine used to report (00:00Z-anchored) is not it.
    CHECK_EQ_MS(T0(utc_ms(2025, 4, 1, 0, 45), "240"), utc_ms(2025, 4, 1, 0, 0));
    CHECK(T(utc_ms(2025, 4, 1, 0, 45), "240", NY, XAU) != T0(utc_ms(2025, 4, 1, 0, 45), "240"));
    // "60": on the hour (the stamp is on the hour, so it coincides with the
    // epoch hour), never 17:00.
    CHECK_EQ_MS(T(mon_open, "60", NY, XAU), mon_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 20, 45), "60", NY, XAU), utc_ms(2025, 4, 1, 20, 0));
    CHECK_EQ_MS(TC(utc_ms(2025, 4, 1, 20, 45), "60", NY, XAU), utc_ms(2025, 4, 1, 21, 0));
    // Last bar of the session: still Monday's D bar; it closes at the stamp.
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 20, 45), "D", NY, XAU), mon_stamp);
    CHECK_EQ_MS(TC(utc_ms(2025, 4, 1, 20, 45), "D", NY, XAU), utc_ms(2025, 4, 1, 21, 0));
    // Next session's first bar: the next stamp.
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 22, 0), "D", NY, XAU), utc_ms(2025, 4, 1, 21, 0));
    // Every bar of every session in the tape's range reads the tape's
    // Size values (147/147 D entries at 17:00; H4 in the six-hour set; H1
    // on the hour and never 17:00), and the D value never moves within a
    // session.
    int sessions = 0, bad_d = 0, bad_h4 = 0, bad_h1 = 0, bad_grid = 0;
    for (int64_t day = utc_ms(2025, 3, 30); day < utc_ms(2025, 6, 30); day += k1d) {
        const int wd = weekday_utc(day);
        if (wd == 5 || wd == 6) continue;          // no Fri / Sat evening open
        const int64_t open_z = day + 22 * k1h;      // 18:00 EDT
        const std::vector<int64_t> bars = xau_session(open_z);
        for (int64_t t : bars) {
            const int64_t d = T(t, "D", NY, XAU);
            if (d != open_z - k1h || hhmm_ny(d) != 1700) ++bad_d;
            if (TC(t, "D", NY, XAU) != open_z - k1h + k1d) ++bad_d;
            const int64_t h4 = T(t, "240", NY, XAU);
            if (!in_set(hhmm_ny(h4), {100, 500, 900, 1300, 1700, 2100})) ++bad_h4;
            if ((h4 - (open_z - k1h)) % k4h != 0 || h4 > t || t >= h4 + k4h) ++bad_h4;
            if (TC(t, "240", NY, XAU) != h4 + k4h) ++bad_h4;
            const int64_t h1 = T(t, "60", NY, XAU);
            if (pine_minute(h1, NY) != 0 || hhmm_ny(h1) == 1700 || h1 > t || t >= h1 + k1h) ++bad_h1;
        }
        bad_grid += grid_disagreements(bars, "240", NY, XAU);
        bad_grid += grid_disagreements(bars, "60", NY, XAU);
        bad_grid += grid_disagreements(bars, "45", NY, XAU);
        ++sessions;
    }
    CHECK(sessions == 66);   // Sun Mar 30 .. Sun Jun 29 evening opens
    CHECK(bad_d == 0);
    CHECK(bad_h4 == 0);
    CHECK(bad_h1 == 0);
    CHECK(bad_grid == 0);
    // Winter (EST): the stamp is 17:00 EST = 22:00Z; the "240" grid follows
    // it (22:00Z / 02:00Z / 06:00Z / 10:00Z ..), the same New York hours.
    const int64_t tue_0700_est = utc_ms(2025, 1, 14, 12, 0);
    CHECK_EQ_MS(T(tue_0700_est, "D", NY, XAU), utc_ms(2025, 1, 13, 22, 0));
    CHECK_EQ_MS(hhmm_ny(T(tue_0700_est, "D", NY, XAU)), 1700);
    CHECK_EQ_MS(T(tue_0700_est, "240", NY, XAU), utc_ms(2025, 1, 14, 10, 0));
    CHECK_EQ_MS(hhmm_ny(T(tue_0700_est, "240", NY, XAU)), 500);
    CHECK_EQ_MS(T(utc_ms(2025, 1, 13, 23, 0), "240", NY, XAU), utc_ms(2025, 1, 13, 22, 0));
    // A session argument only filters; the grid is the symbol's.
    CHECK_EQ_MS(pine_time(utc_ms(2025, 4, 1, 0, 45), "240", XAU, "", CHART, NY, XAU), mon_stamp);
    CHECK(is_na(pine_time(utc_ms(2025, 4, 1, 21, 30), "240", XAU, "", CHART, NY, XAU)));
}

// ─── B. NYSE:F, 0930-1600 America/New_York ───────────────────────────────────

std::vector<int64_t> rth_session(int64_t open_z) {
    std::vector<int64_t> v;
    for (int i = 0; i < 26; ++i) v.push_back(open_z + i * k15m);   // 09:30 .. 15:45
    return v;
}

void test_nyse_intraday_grid_anchored_at_0930() {
    std::printf("B. NYSE:F time(\"60\") == 09:30 + k h, \"240\" in {09:30, 13:30}, \"D\" 09:30 ET\n");
    const int64_t tue_open = utc_ms(2025, 4, 1, 13, 30);     // Tue 09:30 EDT
    CHECK_EQ_MS(T(tue_open, "D", NY, RTH), tue_open);
    CHECK_EQ_MS(hhmm_ny(T(tue_open, "D", NY, RTH)), 930);
    CHECK_EQ_MS(TC(tue_open, "D", NY, RTH), utc_ms(2025, 4, 1, 20, 0));
    // "60" anchored at 09:30: the 09:45 bar belongs to the 09:30 bar, not to
    // the epoch 09:00 hour the engine used to report.
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 13, 45), "60", NY, RTH), tue_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 14, 0), "60", NY, RTH), tue_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 14, 30), "60", NY, RTH), utc_ms(2025, 4, 1, 14, 30));
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 4, 1, 14, 30), "60", NY, RTH)), 1030);
    CHECK_EQ_MS(TC(utc_ms(2025, 4, 1, 13, 45), "60", NY, RTH), utc_ms(2025, 4, 1, 14, 30));
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 19, 45), "60", NY, RTH), utc_ms(2025, 4, 1, 19, 30));
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 4, 1, 19, 45), "60", NY, RTH)), 1530);
    CHECK_EQ_MS(T0(utc_ms(2025, 4, 1, 13, 45), "60"), utc_ms(2025, 4, 1, 13, 0));
    CHECK(T(utc_ms(2025, 4, 1, 13, 45), "60", NY, RTH) != T0(utc_ms(2025, 4, 1, 13, 45), "60"));
    // "240": 09:30 and 13:30.
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 17, 15), "240", NY, RTH), tue_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 17, 30), "240", NY, RTH), utc_ms(2025, 4, 1, 17, 30));
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 4, 1, 19, 45), "240", NY, RTH)), 1330);
    CHECK_EQ_MS(TC(utc_ms(2025, 4, 1, 17, 30), "240", NY, RTH), utc_ms(2025, 4, 1, 21, 30));
    // With a session argument (only a filter) the grid is the same.
    CHECK_EQ_MS(pine_time(utc_ms(2025, 4, 1, 14, 0), "60", RTH, "", CHART, NY, RTH), tue_open);
    CHECK_EQ_MS(pine_time(utc_ms(2025, 4, 1, 14, 0), "60", RTH, NY, CHART, NY, RTH), tue_open);
    CHECK(is_na(pine_time(utc_ms(2025, 4, 1, 12, 0), "60", RTH, "", CHART, NY, RTH)));
    // Every weekday session in the tape's range.
    int sessions = 0, bad_d = 0, bad_h4 = 0, bad_h1 = 0, bad_grid = 0;
    for (int64_t day = utc_ms(2025, 4, 1); day < utc_ms(2025, 7, 1); day += k1d) {
        const int wd = weekday_utc(day);
        if (wd == 0 || wd == 6) continue;
        const int64_t open_z = day + 13 * k1h + 30 * 60000;      // 09:30 EDT
        const std::vector<int64_t> bars = rth_session(open_z);
        for (int64_t t : bars) {
            if (T(t, "D", NY, RTH) != open_z || hhmm_ny(T(t, "D", NY, RTH)) != 930) ++bad_d;
            const int64_t h4 = T(t, "240", NY, RTH);
            if (!in_set(hhmm_ny(h4), {930, 1330}) || h4 > t || t >= h4 + k4h) ++bad_h4;
            const int64_t h1 = T(t, "60", NY, RTH);
            if (!in_set(hhmm_ny(h1), {930, 1030, 1130, 1230, 1330, 1430, 1530})
                || h1 > t || t >= h1 + k1h) ++bad_h1;
        }
        bad_grid += grid_disagreements(bars, "240", NY, RTH);
        bad_grid += grid_disagreements(bars, "60", NY, RTH);
        ++sessions;
    }
    CHECK(sessions == 65);
    CHECK(bad_d == 0);
    CHECK(bad_h4 == 0);
    CHECK(bad_h1 == 0);
    CHECK(bad_grid == 0);
}

// ─── C. NSE:NIFTY, 0915-1530 Asia/Kolkata ────────────────────────────────────

void test_nse_intraday_grid_anchored_at_0915_ist() {
    std::printf("C. NSE:NIFTY time(\"240\") in {09:15, 13:15} IST (23:45 / 03:45 NY); \"D\" 09:15 IST\n");
    const int64_t tue_open = utc_ms(2025, 4, 1, 3, 45);      // Tue 09:15 IST
    CHECK_EQ_MS(T(tue_open, "D", IST, NSE), tue_open);
    CHECK_EQ_MS(hhmm_ny(T(tue_open, "D", IST, NSE)), 2345);
    CHECK_EQ_MS(TC(tue_open, "D", IST, NSE), utc_ms(2025, 4, 1, 10, 0));
    // "240": 03:45Z (09:15 IST) and 07:45Z (13:15 IST).
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 4, 0), "240", IST, NSE), tue_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 7, 30), "240", IST, NSE), tue_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 7, 45), "240", IST, NSE), utc_ms(2025, 4, 1, 7, 45));
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 4, 1, 9, 45), "240", IST, NSE)), 345);
    CHECK_EQ_MS(TC(utc_ms(2025, 4, 1, 7, 45), "240", IST, NSE), utc_ms(2025, 4, 1, 11, 45));
    // The epoch grid (04:00Z / 08:00Z) is not it.
    CHECK_EQ_MS(T0(utc_ms(2025, 4, 1, 4, 0), "240"), utc_ms(2025, 4, 1, 4, 0));
    CHECK(T(utc_ms(2025, 4, 1, 4, 0), "240", IST, NSE) != T0(utc_ms(2025, 4, 1, 4, 0), "240"));
    // "60" (no H1 entries on this tape: the pin strategy ran out of equity)
    // follows the same grid: 09:15 / 10:15 / .. IST.
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 4, 30), "60", IST, NSE), tue_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 4, 45), "60", IST, NSE), utc_ms(2025, 4, 1, 4, 45));
    int sessions = 0, bad_d = 0, bad_h4 = 0, bad_grid = 0;
    for (int64_t day = utc_ms(2025, 4, 1); day < utc_ms(2025, 7, 1); day += k1d) {
        const int wd = weekday_utc(day);
        if (wd == 0 || wd == 6) continue;
        const int64_t open_z = day + 3 * k1h + 45 * 60000;       // 09:15 IST
        std::vector<int64_t> bars;
        for (int i = 0; i < 25; ++i) bars.push_back(open_z + i * k15m);   // 09:15 .. 15:15
        for (int64_t t : bars) {
            if (T(t, "D", IST, NSE) != open_z || hhmm_ny(T(t, "D", IST, NSE)) != 2345) ++bad_d;
            const int64_t h4 = T(t, "240", IST, NSE);
            if (!in_set(hhmm_ny(h4), {2345, 345}) || h4 > t || t >= h4 + k4h) ++bad_h4;
        }
        bad_grid += grid_disagreements(bars, "240", IST, NSE);
        bad_grid += grid_disagreements(bars, "60", IST, NSE);
        ++sessions;
    }
    CHECK(sessions == 65);
    CHECK(bad_d == 0);
    CHECK(bad_h4 == 0);
    CHECK(bad_grid == 0);
}

// ─── D. BINANCE:BTCUSDT, 24x7 UTC ────────────────────────────────────────────

void test_24x7_utc_is_the_epoch_grid() {
    std::printf("D. 24x7 UTC: time(\"D\") == 00:00 UTC (20:00 NY); every form is the epoch grid\n");
    const int64_t t = utc_ms(2025, 4, 1, 8, 15);
    CHECK_EQ_MS(T(t, "D", UTC, ALL), utc_ms(2025, 4, 1, 0, 0));
    CHECK_EQ_MS(hhmm_ny(T(t, "D", UTC, ALL)), 2000);
    CHECK_EQ_MS(T(t, "240", UTC, ALL), utc_ms(2025, 4, 1, 8, 0));
    CHECK_EQ_MS(T(t, "60", UTC, ALL), utc_ms(2025, 4, 1, 8, 0));
    int bad = 0, n = 0;
    for (int64_t ms = utc_ms(2025, 4, 1); ms < utc_ms(2025, 7, 1); ms += 7 * k1h + 37 * 60000) {
        for (const char* tf : {"D", "240", "60", "45", "15"}) {
            for (const std::string* sess : {&ALL, &NONE}) {
                if (T(ms, tf, UTC, *sess) != T0(ms, tf)) ++bad;
                if (TC(ms, tf, UTC, *sess) != pine_time_close(ms, tf, "", "", CHART)) ++bad;
            }
        }
        ++n;
    }
    CHECK(n > 200);
    CHECK(bad == 0);
}

// ─── E. CME 1700-1600 America/Chicago: the grid does not move ────────────────

void test_cme_grid_unchanged() {
    std::printf("E. CME 1700-1600 CT: time(\"D\") == 17:00 CT; \"240\" / \"60\" on the 17:00-CT grid request.security keeps\n");
    const int64_t mon_open = utc_ms(2025, 3, 31, 22, 0);     // Mon 17:00 CDT
    CHECK_EQ_MS(session_period_open_ms(utc_ms(2025, 4, 1, 7, 0), CHI, CME, CalendarPeriod::DAY), mon_open);
    CHECK_EQ_MS(session_period_close_ms(utc_ms(2025, 4, 1, 7, 0), CHI, CME, CalendarPeriod::DAY),
                utc_ms(2025, 4, 1, 21, 0));
    CHECK_EQ_MS(session_period_open_ms(utc_ms(2025, 4, 1, 7, 0), CHI, CME, CalendarPeriod::WEEK),
                utc_ms(2025, 3, 30, 22, 0));
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 7, 0), "D", CHI, CME), mon_open);
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 4, 1, 7, 0), "D", CHI, CME)), 1800);
    CHECK_EQ_MS(TC(utc_ms(2025, 4, 1, 7, 0), "D", CHI, CME), utc_ms(2025, 4, 1, 21, 0));
    // request.security's "240" grid, exactly as test_oanda_day_stamp_grid
    // rule E pins it: 22:00Z / 02:00Z / 06:00Z / .. / 18:00Z.
    TimeframeAggregator agg("240", CHART, CHI, CME);
    CHECK_EQ_MS(agg.bucket_open_ms(mon_open), mon_open);
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 1, 45)), mon_open);
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 2, 0)), utc_ms(2025, 4, 1, 2, 0));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 20, 45)), utc_ms(2025, 4, 1, 18, 0));
    // time("240") reads that grid.
    CHECK_EQ_MS(T(mon_open, "240", CHI, CME), mon_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 1, 45), "240", CHI, CME), mon_open);
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 2, 0), "240", CHI, CME), utc_ms(2025, 4, 1, 2, 0));
    CHECK_EQ_MS(T(utc_ms(2025, 4, 1, 20, 45), "240", CHI, CME), utc_ms(2025, 4, 1, 18, 0));
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 4, 1, 20, 45), "240", CHI, CME)), 1400);
    CHECK_EQ_MS(T(utc_ms(2025, 3, 31, 22, 30), "60", CHI, CME), mon_open);
    CHECK_EQ_MS(hhmm_ny(T(utc_ms(2025, 3, 31, 22, 30), "60", CHI, CME)), 1800);
    // The epoch grid (00:00Z-anchored) is not it.
    CHECK_EQ_MS(T0(utc_ms(2025, 4, 1, 1, 45), "240"), utc_ms(2025, 4, 1, 0, 0));
    CHECK(T(utc_ms(2025, 4, 1, 1, 45), "240", CHI, CME) != T0(utc_ms(2025, 4, 1, 1, 45), "240"));
    // Every bar of every session in the tapes' range reads the tapes' Size
    // values (ES1! and NQ1!: 147/147 D entries at 18:00 NY; H4 in the
    // six-hour set; H1 on the hour and never 17:00 NY), and D closes at
    // 16:00 CT.
    int sessions = 0, bad_d = 0, bad_h4 = 0, bad_h1 = 0, bad_grid = 0;
    for (int64_t day = utc_ms(2025, 3, 30); day < utc_ms(2025, 6, 30); day += k1d) {
        const int wd = weekday_utc(day);
        if (wd == 5 || wd == 6) continue;          // no Fri / Sat evening open
        const int64_t open_z = day + 22 * k1h;      // 17:00 CDT
        std::vector<int64_t> bars;
        for (int i = 0; i < 92; ++i) bars.push_back(open_z + i * k15m);   // 17:00 .. 15:45 CDT
        for (int64_t t : bars) {
            const int64_t d = T(t, "D", CHI, CME);
            if (d != open_z || hhmm_ny(d) != 1800) ++bad_d;
            if (TC(t, "D", CHI, CME) != open_z + 23 * k1h) ++bad_d;
            const int64_t h4 = T(t, "240", CHI, CME);
            if (!in_set(hhmm_ny(h4), {200, 600, 1000, 1400, 1800, 2200})) ++bad_h4;
            if ((h4 - open_z) % k4h != 0 || h4 > t || t >= h4 + k4h) ++bad_h4;
            if (TC(t, "240", CHI, CME) != h4 + k4h) ++bad_h4;
            const int64_t h1 = T(t, "60", CHI, CME);
            if (pine_minute(h1, NY) != 0 || hhmm_ny(h1) == 1700 || h1 > t || t >= h1 + k1h) ++bad_h1;
        }
        bad_grid += grid_disagreements(bars, "240", CHI, CME);
        bad_grid += grid_disagreements(bars, "60", CHI, CME);
        ++sessions;
    }
    CHECK(sessions == 66);   // Sun Mar 30 .. Sun Jun 29 evening opens
    CHECK(bad_d == 0);
    CHECK(bad_h4 == 0);
    CHECK(bad_h1 == 0);
    CHECK(bad_grid == 0);
}

}  // namespace

int main() {
    std::printf("=== time(<tf>) on the day-stamp grid ===\n\n");
    test_xau_time_d_is_the_1700_stamp();
    test_nyse_intraday_grid_anchored_at_0930();
    test_nse_intraday_grid_anchored_at_0915_ist();
    test_24x7_utc_is_the_epoch_grid();
    test_cme_grid_unchanged();
    std::printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
