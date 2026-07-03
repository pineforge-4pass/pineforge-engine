/*
 * test_session_calendar_extra.cpp — densification tests for src/session_time.cpp
 *
 * Targets previously-uncovered code paths:
 *   - overnight session window (start>end wrap) inside/outside/edges
 *     (local_time_in_session_windows wrap arm)
 *   - utc_bucket_open_ms negative-bar floor-toward-(-inf) quantization
 *     (lines 112-119, including the `--q` underflow correction)
 *   - calendar_week_open_local_ms / calendar_month_open_local_ms and the
 *     calendar-period dispatch in compute_tf_open_ms (154-167)
 *   - compute_tf_close_ms DAY/WEEK/MONTH period-END boundaries (180-211)
 *   - parse_day_filter all-digits guard (non-1-7 suffix is NOT a day filter)
 *     and the day-of-week insert/count path (79-100)
 *   - local_time_in_session_windows malformed-window `continue` arms
 *     (no-dash skip; invalid-HHMM skip) (288-302)
 *   - pine_time_tradingday unparseable-session fprintf fallback (474-480)
 *
 * All expected ms values were derived empirically by running the helpers and
 * cross-checked against the calendar (UTC, and America/New_York EDT = UTC-4 in
 * April 2026). They pin the current Pine-correct behavior.
 */

#include <cstdio>
#include <ctime>
#include <string>

#include <pineforge/na.hpp>
#include <pineforge/session_time.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

// Build a Unix-ms for YYYY-MM-DD HH:MM:SS in UTC.
static int64_t utc_ms(int y, int m, int d, int hh, int mm, int ss) {
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = d;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    t.tm_isdst = 0;
    return static_cast<int64_t>(timegm(&t)) * 1000LL;
}

// ---------------------------------------------------------------------------
// Overnight session window "2200-0500" (start minute 1320 > end minute 300).
// Exercises the wrap arm `mod >= sm || mod < em` in
// local_time_in_session_windows.
// ---------------------------------------------------------------------------
static void test_overnight_window_wrap() {
    std::printf("test_overnight_window_wrap\n");
    const std::string sess = "2200-0500";

    // 2026-04-07 (Tue) 23:30 UTC — inside the late-evening half.
    CHECK(pine_session_ismarket(sess, "UTC", utc_ms(2026, 4, 7, 23, 30, 0)) == true);
    // 2026-04-08 03:00 UTC — inside the early-morning half (after midnight).
    CHECK(pine_session_ismarket(sess, "UTC", utc_ms(2026, 4, 8, 3, 0, 0)) == true);
    // 2026-04-07 12:00 UTC — daytime, outside the overnight window.
    CHECK(pine_session_ismarket(sess, "UTC", utc_ms(2026, 4, 7, 12, 0, 0)) == false);

    // Boundaries: start is inclusive, end is exclusive.
    CHECK(pine_session_ismarket(sess, "UTC", utc_ms(2026, 4, 7, 22, 0, 0)) == true);   // 22:00 in
    CHECK(pine_session_ismarket(sess, "UTC", utc_ms(2026, 4, 7, 21, 59, 0)) == false); // 21:59 out
    CHECK(pine_session_ismarket(sess, "UTC", utc_ms(2026, 4, 8, 4, 59, 0)) == true);   // 04:59 in
    CHECK(pine_session_ismarket(sess, "UTC", utc_ms(2026, 4, 8, 5, 0, 0)) == false);   // 05:00 out
}

// ---------------------------------------------------------------------------
// utc_bucket_open_ms quantization, including negative bar_ms.
// For period 3600s (TF "60"): floor toward -infinity.
//   bar = -90,061,000 ms; -90061000 / 3600000 = -25 (trunc), remainder != 0,
//   so q is decremented to -26 -> -93,600,000 ms.
//   bar = -3,600,000 ms is exactly aligned -> returned unchanged.
// For period 300s (TF "5"):
//   bar = -1,000 ms -> floor to -300,000 ms.
// Positive path: a normal intraday bucket.
// ---------------------------------------------------------------------------
static void test_utc_bucket_negative_quantization() {
    std::printf("test_utc_bucket_negative_quantization\n");

    // Unaligned negative -> --q correction branch.
    CHECK(pine_time(-90061000LL, "60", "", "UTC", "60") == -93600000LL);
    // Aligned negative -> remainder zero, no decrement.
    CHECK(pine_time(-3600000LL, "60", "", "UTC", "60") == -3600000LL);
    // Unaligned negative with 5-minute bucket.
    CHECK(pine_time(-1000LL, "5", "", "UTC", "5") == -300000LL);

    // Positive intraday bucket sanity: 2026-04-07 14:30 UTC -> 14:00 UTC bucket.
    int64_t pos = utc_ms(2026, 4, 7, 14, 30, 0);
    CHECK(pine_time(pos, "60", "", "UTC", "60") == utc_ms(2026, 4, 7, 14, 0, 0));
}

// ---------------------------------------------------------------------------
// Calendar WEEK open/close (UTC). Week anchors on Monday 00:00.
//   bar = 2026-04-08 (Wed) 14:30 UTC.
//   week open  = Mon 2026-04-06 00:00:00 UTC.
//   week close = next Mon 2026-04-13 00:00 UTC minus 1 ms
//              = Sun 2026-04-12 23:59:59.999 UTC.
// ---------------------------------------------------------------------------
static void test_calendar_week_utc() {
    std::printf("test_calendar_week_utc\n");
    int64_t bar = utc_ms(2026, 4, 8, 14, 30, 0);

    int64_t open  = pine_time(bar, "W", "", "UTC", "W");
    int64_t close = pine_time_close(bar, "W", "", "UTC", "W");

    CHECK(!is_na(open));
    CHECK(open == utc_ms(2026, 4, 6, 0, 0, 0));            // Monday open
    CHECK(close == utc_ms(2026, 4, 13, 0, 0, 0) - 1);      // Sunday 23:59:59.999
}

// ---------------------------------------------------------------------------
// Calendar MONTH open/close (UTC).
//   bar = 2026-04-08 14:30 UTC.
//   month open  = 2026-04-01 00:00:00 UTC.
//   month close = 2026-05-01 00:00 UTC minus 1 ms = 2026-04-30 23:59:59.999.
// ---------------------------------------------------------------------------
static void test_calendar_month_utc() {
    std::printf("test_calendar_month_utc\n");
    int64_t bar = utc_ms(2026, 4, 8, 14, 30, 0);

    int64_t open  = pine_time(bar, "M", "", "UTC", "M");
    int64_t close = pine_time_close(bar, "M", "", "UTC", "M");

    CHECK(!is_na(open));
    CHECK(open == utc_ms(2026, 4, 1, 0, 0, 0));
    CHECK(close == utc_ms(2026, 5, 1, 0, 0, 0) - 1);
}

// ---------------------------------------------------------------------------
// Calendar DAY open/close (UTC) — compute_tf_close_ms DAY branch.
//   bar = 2026-04-08 14:30 UTC.
//   day open  = 2026-04-08 00:00:00 UTC.
//   day close = 2026-04-09 00:00 UTC minus 1 ms = 2026-04-08 23:59:59.999.
// ---------------------------------------------------------------------------
static void test_calendar_day_utc() {
    std::printf("test_calendar_day_utc\n");
    int64_t bar = utc_ms(2026, 4, 8, 14, 30, 0);

    int64_t open  = pine_time(bar, "D", "", "UTC", "D");
    int64_t close = pine_time_close(bar, "D", "", "UTC", "D");

    CHECK(open == utc_ms(2026, 4, 8, 0, 0, 0));
    CHECK(close == utc_ms(2026, 4, 9, 0, 0, 0) - 1);
}

// ---------------------------------------------------------------------------
// Calendar opens/closes honour the requested IANA timezone (not UTC).
// America/New_York is EDT (UTC-4) on 2026-04-08.
//   bar = 2026-04-08 18:00 UTC = 14:00 ET (Wed).
//   week open  = Mon 2026-04-06 00:00 ET = 2026-04-06 04:00 UTC.
//   month open = 2026-04-01 00:00 ET     = 2026-04-01 04:00 UTC.
//   day  open  = 2026-04-08 00:00 ET     = 2026-04-08 04:00 UTC.
//   week close = next Mon 00:00 ET - 1ms = 2026-04-13 03:59:59.999 UTC.
//   month close= 2026-05-01 00:00 ET -1  = 2026-05-01 03:59:59.999 UTC.
//   day close  = 2026-04-09 00:00 ET -1  = 2026-04-09 03:59:59.999 UTC.
// ---------------------------------------------------------------------------
static void test_calendar_opens_new_york_tz() {
    std::printf("test_calendar_opens_new_york_tz\n");
    const std::string tz = "America/New_York";
    int64_t bar = utc_ms(2026, 4, 8, 18, 0, 0);  // 14:00 ET Wed

    CHECK(pine_time(bar, "W", "", tz, "W") == utc_ms(2026, 4, 6, 4, 0, 0));
    CHECK(pine_time(bar, "M", "", tz, "M") == utc_ms(2026, 4, 1, 4, 0, 0));
    CHECK(pine_time(bar, "D", "", tz, "D") == utc_ms(2026, 4, 8, 4, 0, 0));

    CHECK(pine_time_close(bar, "W", "", tz, "W") == utc_ms(2026, 4, 13, 4, 0, 0) - 1);
    CHECK(pine_time_close(bar, "M", "", tz, "M") == utc_ms(2026, 5, 1, 4, 0, 0) - 1);
    CHECK(pine_time_close(bar, "D", "", tz, "D") == utc_ms(2026, 4, 9, 4, 0, 0) - 1);
}

// ---------------------------------------------------------------------------
// parse_day_filter all-digits guard: a colon suffix that is NOT all 1-7 is
// treated as part of the window body, not a weekday filter. Here the trailing
// ":foo" is ignored when the window is re-parsed (only "0930-1600" matters),
// so a 10:30 ET (14:30 UTC) bar is still inside the NYSE session.
// ---------------------------------------------------------------------------
static void test_day_filter_non_digit_suffix_ignored() {
    std::printf("test_day_filter_non_digit_suffix_ignored\n");
    int64_t bar = utc_ms(2026, 4, 7, 14, 30, 0);  // 10:30 ET (Tue)
    CHECK(pine_session_ismarket("0930-1600:foo", "America/New_York", bar) == true);
}

// ---------------------------------------------------------------------------
// parse_day_filter weekday set (digit insert + count path).
// "1400-1500:23456" = Mon..Fri only (TV days 2-6).
//   2026-04-07 is Tuesday (TV day 3) -> day passes; 14:30 inside window -> in.
//   2026-04-11 is Saturday (TV day 7) -> excluded by filter -> out.
// ---------------------------------------------------------------------------
static void test_weekday_filter_digits() {
    std::printf("test_weekday_filter_digits\n");
    CHECK(pine_session_ismarket("1400-1500:23456", "UTC",
                                utc_ms(2026, 4, 7, 14, 30, 0)) == true);   // Tue
    CHECK(pine_session_ismarket("1400-1500:23456", "UTC",
                                utc_ms(2026, 4, 11, 14, 30, 0)) == false); // Sat
}

// ---------------------------------------------------------------------------
// local_time_in_session_windows malformed-window `continue` arms:
//   - window without a '-' is skipped.
//   - window with valid dash geometry but invalid HHMM digits is skipped.
//   - a valid window after a skipped one still matches.
// ---------------------------------------------------------------------------
static void test_malformed_window_skips() {
    std::printf("test_malformed_window_skips\n");
    int64_t bar = utc_ms(2026, 4, 7, 14, 30, 0);  // 14:30 UTC

    // No dash anywhere -> no usable window -> outside.
    CHECK(pine_session_ismarket("xxxx", "UTC", bar) == false);
    CHECK(pine_session_ismarket("0930", "UTC", bar) == false);

    // First window has bad HHMM digits (h=99 invalid); skipped, no other -> outside.
    CHECK(pine_session_ismarket("99xx-1500", "UTC", bar) == false);

    // First window malformed/invalid, second window valid and matches.
    CHECK(pine_session_ismarket("zz,1400-1500", "UTC", bar) == true);
    CHECK(pine_session_ismarket("9999-9999,1400-1500", "UTC", bar) == true);
}

// ---------------------------------------------------------------------------
// pine_time_tradingday unparseable-session fallback (fprintf to stderr +
// UTC calendar-day midnight). The session is non-empty and not 24/7, but its
// first window has no valid HHMM start, so parse_session_start_minutes() fails.
//   bar = 2024-09-05 16:00 UTC -> falls back to 2024-09-05 00:00 UTC.
// ---------------------------------------------------------------------------
static void test_tradingday_unparseable_fallback() {
    std::printf("test_tradingday_unparseable_fallback\n");
    int64_t bar = utc_ms(2024, 9, 5, 16, 0, 0);
    int64_t expected = utc_ms(2024, 9, 5, 0, 0, 0);

    // Non-numeric "window": is_allday_session is false (start4 != "0000"),
    // parse_session_start_minutes returns -1 -> UTC-midnight fallback.
    CHECK(pine_time_tradingday(bar, "abcd-efgh", "America/New_York") == expected);
    // A bare garbage token with no dash also hits the same fallback.
    CHECK(pine_time_tradingday(bar, "garbage", "UTC") == expected);
}

// ---------------------------------------------------------------------------
// TradingView fixed-offset labels use the intuitive sign convention:
// "GMT+1" means local time is UTC+1. POSIX TZ strings use the reverse sign,
// so the engine must normalize before calling setenv("TZ", ...).
// ---------------------------------------------------------------------------
static void test_tradingview_gmt_offset_signs() {
    std::printf("test_tradingview_gmt_offset_signs\n");

    CHECK(normalize_timezone_for_posix("GMT+1") == "UTC-1");
    CHECK(normalize_timezone_for_posix("UTC-5") == "UTC+5");
    CHECK(normalize_timezone_for_posix("GMT+05:30") == "UTC-5:30");

    const std::string sess = "0800-1000";

    // GMT+1: 07:15 UTC == 08:15 local (inside); 09:15 UTC == 10:15 local (out).
    CHECK(pine_session_ismarket(sess, "GMT+1", utc_ms(2025, 4, 4, 7, 15, 0)) == true);
    CHECK(pine_session_ismarket(sess, "GMT+1", utc_ms(2025, 4, 4, 9, 15, 0)) == false);

    // GMT-3: 11:15 UTC == 08:15 local (inside); 13:15 UTC == 10:15 local (out).
    CHECK(pine_session_ismarket(sess, "GMT-3", utc_ms(2025, 4, 4, 11, 15, 0)) == true);
    CHECK(pine_session_ismarket(sess, "GMT-3", utc_ms(2025, 4, 4, 13, 15, 0)) == false);

    // Calendar buckets use the same normalization.
    CHECK(pine_time(utc_ms(2025, 4, 4, 7, 15, 0), "D", "", "GMT+1", "D") ==
          utc_ms(2025, 4, 3, 23, 0, 0));
}

int main() {
    test_overnight_window_wrap();
    test_utc_bucket_negative_quantization();
    test_calendar_week_utc();
    test_calendar_month_utc();
    test_calendar_day_utc();
    test_calendar_opens_new_york_tz();
    test_day_filter_non_digit_suffix_ignored();
    test_weekday_filter_digits();
    test_malformed_window_skips();
    test_tradingday_unparseable_fallback();
    test_tradingview_gmt_offset_signs();

    std::printf("session_calendar_extra: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
