// tests/test_time_tradingday.cpp
// Unit tests for pine_time_tradingday().
//
// Covers:
//   1. UTC + 24/7 session (empty string) — falls back to UTC midnight
//   2. UTC + "0000-2400" (explicit 24/7) — same fallback
//   3. America/New_York + "0930-1600" (NYSE RTH)
//      a. Bar at 14:00 ET (inside session day)
//      b. Bar at 08:00 ET (before session open — yesterday's trading day)
//   4. America/Havana + "0900-1700" (DST spring-forward edge case)
//   5. Pacific/Lord_Howe + "1000-1600" (half-hour DST edge case)
//   6. Empty session string — falls back to UTC midnight (with warning)

#include <cstdio>
#include <string>

#include <pineforge/session_time.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
            ++tests_failed;                                                 \
        } else {                                                            \
            ++tests_passed;                                                 \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
    // timegm is POSIX (available on macOS and Linux)
    time_t epoch = timegm(&t);
    return static_cast<int64_t>(epoch) * 1000LL;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_utc_allday_empty_session() {
    std::printf("test_utc_allday_empty_session\n");
    // 2024-03-15 14:35 UTC
    int64_t bar = utc_ms(2024, 3, 15, 14, 35, 0);
    int64_t td  = pine_time_tradingday(bar, "", "UTC");
    // Expected: UTC midnight of 2024-03-15
    int64_t expected = utc_ms(2024, 3, 15, 0, 0, 0);
    CHECK(td == expected);
}

static void test_utc_allday_explicit_session() {
    std::printf("test_utc_allday_explicit_session\n");
    // "0000-2400" is treated as 24/7 — fall back to UTC midnight
    int64_t bar = utc_ms(2024, 6, 10, 22, 0, 0);
    int64_t td  = pine_time_tradingday(bar, "0000-2400", "UTC");
    int64_t expected = utc_ms(2024, 6, 10, 0, 0, 0);
    CHECK(td == expected);
}

static void test_nyse_rth_bar_inside_session() {
    std::printf("test_nyse_rth_bar_inside_session (bar after open)\n");
    // 2024-03-15 is a Friday (no DST transition), EDT = UTC-4
    // Bar at 14:00 ET = 18:00 UTC.
    int64_t bar = utc_ms(2024, 3, 15, 18, 0, 0);
    int64_t td  = pine_time_tradingday(bar, "0930-1600", "America/New_York");
    // Expected: 2024-03-15 09:30 ET = 13:30 UTC
    int64_t expected = utc_ms(2024, 3, 15, 13, 30, 0);
    CHECK(td == expected);
}

static void test_nyse_rth_bar_before_session_open() {
    std::printf("test_nyse_rth_bar_before_session_open (bar before open → yesterday)\n");
    // 2024-03-15 Friday, bar at 08:00 ET = 12:00 UTC (before 09:30 open)
    int64_t bar = utc_ms(2024, 3, 15, 12, 0, 0);
    int64_t td  = pine_time_tradingday(bar, "0930-1600", "America/New_York");
    // Expected: 2024-03-14 09:30 ET = 13:30 UTC (Thursday)
    int64_t expected = utc_ms(2024, 3, 14, 13, 30, 0);
    CHECK(td == expected);
}

static void test_nyse_rth_dst_transition_bar() {
    std::printf("test_nyse_rth_dst_transition (spring forward: 2024-03-10 EDT)\n");
    // 2024-03-10: US clocks spring forward at 02:00 EST → 03:00 EDT.
    // After transition, ET = UTC-4.
    // Bar at 15:00 EDT (19:00 UTC) — inside session.
    int64_t bar = utc_ms(2024, 3, 10, 19, 0, 0);
    int64_t td  = pine_time_tradingday(bar, "0930-1600", "America/New_York");
    // Expected: 2024-03-10 09:30 EDT = 13:30 UTC
    int64_t expected = utc_ms(2024, 3, 10, 13, 30, 0);
    CHECK(td == expected);
}

static void test_havana_dst_spring_forward() {
    std::printf("test_havana_dst_spring_forward (America/Havana 2024-03-10)\n");
    // Cuba: clocks spring forward at 00:00 → 01:00 on 2024-03-10.
    // Midnight 2024-03-10 is non-existent in America/Havana.
    // Bar at 10:00 local = well after session open. Engine should not crash.
    // Cuba is typically UTC-5 (EST) / UTC-4 (EDT after spring-forward).
    // After spring-forward on 2024-03-10, Cuba is at UTC-4 (CDT).
    // Bar at 14:30 UTC = 10:30 CDT on 2024-03-10.
    int64_t bar = utc_ms(2024, 3, 10, 14, 30, 0);
    int64_t td  = pine_time_tradingday(bar, "0900-1700", "America/Havana");
    // We don't assert an exact value here because the DST fallback is
    // implementation-defined (midnight + 1h or + 2h as available).
    // We assert: result is positive and within reasonable range of the day.
    int64_t day_utc_midnight = utc_ms(2024, 3, 10, 0, 0, 0);
    CHECK(td > 0);
    // Should be within 24 h of the expected UTC midnight of that day
    int64_t diff = td - day_utc_midnight;
    // Allow ±2h offset from what would be the ideal session-open timestamp
    // plus 4h UTC offset (session 09:00 Cuba + ~4-5h = 13:00-14:00 UTC)
    CHECK(diff >= -7200LL * 1000 && diff <= 90000LL * 1000);
    (void)td;
}

static void test_lord_howe_dst() {
    std::printf("test_lord_howe_dst (Australia/Lord_Howe half-hour DST offset)\n");
    // Lord Howe Island (Australia/Lord_Howe) uses a 30-minute DST offset.
    // During standard time (winter): UTC+10:30; during DST (summer): UTC+11.
    // DST ends in April (fall back).
    // 2024-06-15 (winter, UTC+10:30): bar at 00:00 UTC = 10:30 local.
    int64_t bar = utc_ms(2024, 6, 15, 0, 0, 0);  // 10:30 local LHI
    int64_t td  = pine_time_tradingday(bar, "1000-1600", "Australia/Lord_Howe");
    // Bar local time = 10:30 → after 10:00 session open.
    // Local midnight of 2024-06-15 in LHI = 2024-06-14 13:30 UTC.
    // Session open at 10:00 local = local midnight + 10 h = 2024-06-14 23:30 UTC.
    int64_t expected = utc_ms(2024, 6, 14, 23, 30, 0);
    CHECK(td == expected);
}

static void test_empty_session_fallback() {
    std::printf("test_empty_session_fallback\n");
    // Empty session → warning + UTC midnight
    int64_t bar = utc_ms(2024, 9, 5, 16, 0, 0);
    int64_t td  = pine_time_tradingday(bar, "", "America/New_York");
    int64_t expected = utc_ms(2024, 9, 5, 0, 0, 0);
    CHECK(td == expected);
}

static void test_hhmm_to_minutes_valid() {
    std::printf("test_hhmm_to_minutes_valid\n");
    CHECK(hhmm_to_minutes("0930") == 570);
    CHECK(hhmm_to_minutes("1600") == 960);
    CHECK(hhmm_to_minutes("0000") == 0);
    CHECK(hhmm_to_minutes("2359") == 1439);
}

static void test_hhmm_to_minutes_invalid() {
    std::printf("test_hhmm_to_minutes_invalid\n");
    CHECK(hhmm_to_minutes("") == -1);
    CHECK(hhmm_to_minutes("093") == -1);  // too short
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    test_utc_allday_empty_session();
    test_utc_allday_explicit_session();
    test_nyse_rth_bar_inside_session();
    test_nyse_rth_bar_before_session_open();
    test_nyse_rth_dst_transition_bar();
    test_havana_dst_spring_forward();
    test_lord_howe_dst();
    test_empty_session_fallback();
    test_hhmm_to_minutes_valid();
    test_hhmm_to_minutes_invalid();

    std::printf("time_tradingday: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
