/*
 * test_session_predicates.cpp — unit tests for Pine v6 session.is* predicates.
 *
 * Tests cover:
 *  - NYSE RTH bar inside / outside session (session.ismarket)
 *  - Premarket bar (session.ispremarket)
 *  - Postmarket bar (session.ispostmarket)
 *  - First/last session bar transitions across session boundaries
 *  - 24x7 session (crypto) — ismarket always true, pre/post always false
 */

#include <cstdio>
#include <string>

#include <pineforge/session_time.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

// -----------------------------------------------------------------------
// Timestamp helpers
// NYSE RTH: 0930-1600 America/New_York
// 2026-04-07 (Tuesday) chosen — well past DST spring-forward.
//
// UTC offsets for America/New_York on 2026-04-07 (EDT = UTC-4).
//
//  09:30 ET  = 13:30 UTC  => 1775573400000 ms
//  10:30 ET  = 14:30 UTC  => 1775577000000 ms  (inside RTH)
//  16:00 ET  = 20:00 UTC  => 1775592000000 ms  (session close — OUTSIDE by <end convention)
//  17:00 ET  = 21:00 UTC  => 1775595600000 ms  (post-market)
//  06:00 ET  = 10:00 UTC  => 1775559600000 ms  (pre-market)
//  03:30 ET  = 07:30 UTC  => 1775548200000 ms  (before pre-market opens 04:00)
// -----------------------------------------------------------------------
static const std::string NYSE_SESSION = "0930-1600";
static const std::string NYSE_TZ      = "America/New_York";

// 2026-04-07 10:30 ET (inside RTH)
static const int64_t T_INSIDE_RTH     = 1775577000000LL;
// 2026-04-07 20:00 UTC = 16:00 ET (session end, exclusive — outside)
static const int64_t T_RTH_CLOSE_UTC  = 1775592000000LL;
// 2026-04-07 17:00 ET (post-market)
static const int64_t T_POSTMARKET     = 1775595600000LL;
// 2026-04-07 06:00 ET (pre-market, between 04:00 and 09:30)
static const int64_t T_PREMARKET      = 1775559600000LL;
// 2026-04-07 03:30 ET (before pre-market opens at 04:00)
static const int64_t T_BEFORE_PRE     = 1775548200000LL;


static void test_ismarket_inside_rth() {
    std::printf("test_ismarket_inside_rth\n");
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH) == true);
}

static void test_ismarket_outside_rth_close() {
    std::printf("test_ismarket_outside_rth_close\n");
    // 16:00 ET is the session END — exclusive convention means 16:00 itself
    // is OUTSIDE the session window [0930, 1600).
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_RTH_CLOSE_UTC) == false);
}

static void test_ismarket_postmarket_is_false() {
    std::printf("test_ismarket_postmarket_is_false\n");
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET) == false);
}

static void test_ismarket_premarket_is_false() {
    std::printf("test_ismarket_premarket_is_false\n");
    CHECK(pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET) == false);
}

static void test_ispremarket_true() {
    std::printf("test_ispremarket_true\n");
    // 06:00 ET is between 04:00 and 09:30 => premarket
    CHECK(pine_session_ispremarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET) == true);
}

static void test_ispremarket_before_0400_false() {
    std::printf("test_ispremarket_before_0400_false\n");
    // 03:30 ET is before 04:00 => NOT premarket
    CHECK(pine_session_ispremarket(NYSE_SESSION, NYSE_TZ, T_BEFORE_PRE) == false);
}

static void test_ispremarket_inside_rth_false() {
    std::printf("test_ispremarket_inside_rth_false\n");
    // Inside RTH => NOT premarket
    CHECK(pine_session_ispremarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH) == false);
}

static void test_ispostmarket_true() {
    std::printf("test_ispostmarket_true\n");
    // 17:00 ET is between 16:00 and 20:00 => postmarket
    CHECK(pine_session_ispostmarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET) == true);
}

static void test_ispostmarket_inside_rth_false() {
    std::printf("test_ispostmarket_inside_rth_false\n");
    // Inside RTH => NOT postmarket
    CHECK(pine_session_ispostmarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH) == false);
}

static void test_ispostmarket_premarket_false() {
    std::printf("test_ispostmarket_premarket_false\n");
    // Premarket time => NOT postmarket
    CHECK(pine_session_ispostmarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET) == false);
}

static void test_24x7_ismarket_always_true() {
    std::printf("test_24x7_ismarket_always_true\n");
    CHECK(pine_session_ismarket("24x7", "UTC", T_PREMARKET) == true);
    CHECK(pine_session_ismarket("24x7", "UTC", T_INSIDE_RTH) == true);
    CHECK(pine_session_ismarket("",     "UTC", T_POSTMARKET) == true);
}

static void test_24x7_prepost_always_false() {
    std::printf("test_24x7_prepost_always_false\n");
    CHECK(pine_session_ispremarket ("24x7", "UTC", T_PREMARKET) == false);
    CHECK(pine_session_ispostmarket("24x7", "UTC", T_POSTMARKET) == false);
    CHECK(pine_session_ispremarket ("",     "UTC", T_PREMARKET) == false);
    CHECK(pine_session_ispostmarket("",     "UTC", T_POSTMARKET) == false);
}

// Test session-boundary transitions using hhmm helpers directly
static void test_hhmm_to_minutes_basic() {
    std::printf("test_hhmm_to_minutes_basic\n");
    CHECK(hhmm_to_minutes("0930") == 9 * 60 + 30);
    CHECK(hhmm_to_minutes("1600") == 16 * 60);
    CHECK(hhmm_to_minutes("0000") == 0);
    CHECK(hhmm_to_minutes("2359") == 23 * 60 + 59);
    CHECK(hhmm_to_minutes("xx")   == -1);
    CHECK(hhmm_to_minutes("2400") == -1);
}

// Test session with weekday filter
static void test_ismarket_weekend_filter() {
    std::printf("test_ismarket_weekend_filter\n");
    // 2026-04-11 (Saturday) 14:30 UTC = 10:30 ET — session hours but filtered out by :23456
    static const int64_t bar_sat = 1775921400000LL;
    CHECK(pine_session_ismarket("0930-1600:23456", NYSE_TZ, bar_sat) == false);
    // Without day filter — should be inside
    CHECK(pine_session_ismarket("0930-1600", NYSE_TZ, bar_sat) == true);
}

// Test first/last bar transitions via passes_session_filter directly
static void test_firstlastbar_transitions() {
    std::printf("test_firstlastbar_transitions\n");
    // Simulate a sequence of bars:
    //   bar0: premarket  (outside session)
    //   bar1: RTH open   (inside session  — isfirstbar=true)
    //   bar2: inside RTH (inside session  — isfirstbar=false)
    //   bar3: post-mkt   (outside session — during prev bar: islastbar=true)
    bool prev_in = false;
    bool in_session;

    // bar0: premarket — not in session
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_PREMARKET);
    CHECK(in_session == false);
    CHECK((in_session && !prev_in) == false);   // not first bar
    prev_in = in_session;

    // bar1: inside RTH — first bar
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH);
    CHECK(in_session == true);
    CHECK((in_session && !prev_in) == true);    // FIRST bar of session
    prev_in = in_session;

    // bar2: still inside RTH — not first bar
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_INSIDE_RTH + 300000LL);  // +5m
    CHECK(in_session == true);
    CHECK((in_session && !prev_in) == false);   // NOT first bar
    prev_in = in_session;

    // bar2 islastbar check: next bar (post-market) is outside session
    bool next_in = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET);
    CHECK(next_in == false);
    CHECK((prev_in && !next_in) == true);       // LAST bar of session (bar2 fires islastbar)

    // bar3: post-market — not in session
    in_session = pine_session_ismarket(NYSE_SESSION, NYSE_TZ, T_POSTMARKET);
    CHECK(in_session == false);
    prev_in = in_session;
}

// A session window whose start equals its end ("1700-1700" — TradingView's
// spelling of OANDA forex's 24-hour session; "0000-0000") spans the WHOLE
// day. The half-open [start, end) arithmetic used to make it EMPTY, so every
// bar of a forex symbol read session.ismarket == false and time(session) /
// time_close == na (finding 455: a strategy gating its 'Session Close' exit
// on minute(time_close) never fired on EURUSD).
static void test_start_equals_end_is_full_day() {
    const std::string tz = "America/New_York";
    // 2026-04-07 (Tue, EDT = UTC-4), exact epoch ms:
    const int64_t kTs_0930_ET = 1775568600000LL;   // 13:30 UTC
    const int64_t kTs_1030_ET = 1775572200000LL;   // 14:30 UTC
    const int64_t kTs_1515_ET = 1775589300000LL;   // 19:15 UTC
    const int64_t kTs_1630_ET = 1775593800000LL;   // 20:30 UTC
    const int64_t kTs_0500_ET = 1775552400000LL;   // 09:00 UTC (pre-market hours)
    const int64_t kTs_1730_ET = 1775597400000LL;   // 21:30 UTC (post-market hours)
    const int64_t kTs_SAT_1030_ET = 1775917800000LL;   // 2026-04-11 Sat
    // Sweep a full local day at 1-minute grain: every minute is in session.
    int in_1700 = 0, in_0000 = 0, in_days = 0;
    for (int m = 0; m < 1440; ++m) {
        int64_t ts = kTs_0930_ET + static_cast<int64_t>(m) * 60000LL;
        if (pine_session_ismarket("1700-1700", tz, ts)) ++in_1700;
        if (pine_session_ismarket("0000-0000", tz, ts)) ++in_0000;
        if (pine_session_ismarket("1700-1700:1234567", tz, ts)) ++in_days;
    }
    CHECK(in_1700 == 1440);
    CHECK(in_0000 == 1440);
    CHECK(in_days == 1440);
    // time(session) / time_close(session) resolve for every bar instead of na.
    CHECK(pine_time(kTs_1030_ET, "15", "1700-1700", tz, "15") == kTs_1030_ET);
    CHECK(pine_time_close(kTs_1030_ET, "15", "1700-1700", tz, "15")
          == kTs_1030_ET + 15 * 60000LL);
    // 15:15 ET bar on a 15m chart: time_close is 15:30 ET (the exemplar's
    // minute(time_close) >= 30 session-close gate).
    CHECK(pine_time_close(kTs_1515_ET, "15", "1700-1700", tz, "15")
          == kTs_1515_ET + 15 * 60000LL);
    CHECK(pine_time(kTs_1730_ET, "15", "1700-1700", tz, "15") == kTs_1730_ET);
    // A 24-hour session has no pre-/post-market.
    CHECK(!pine_session_ispremarket("1700-1700", tz, kTs_0500_ET));
    CHECK(!pine_session_ispostmarket("1700-1700", tz, kTs_1730_ET));
    CHECK(pine_session_ispremarket("0930-1600", tz, kTs_0500_ET));   // control
    CHECK(pine_session_ispostmarket("0930-1600", tz, kTs_1730_ET));  // control
    // Day-of-week filter still applies: Saturday is out even for 1700-1700.
    CHECK(!pine_session_ismarket("1700-1700:23456", tz, kTs_SAT_1030_ET));
    CHECK(pine_session_ismarket("1700-1700:23456", tz, kTs_1030_ET));
    // Ordinary and wrapped windows are untouched.
    CHECK(pine_session_ismarket("0930-1600", tz, kTs_1030_ET));
    CHECK(!pine_session_ismarket("0930-1600", tz, kTs_1630_ET));
    CHECK(pine_session_ismarket("1700-1600", tz, kTs_1030_ET));
    CHECK(!pine_session_ismarket("1700-1600", tz, kTs_1630_ET));
    CHECK(pine_session_ismarket("1700-1600", tz, kTs_1730_ET));
}

int main() {
    test_ismarket_inside_rth();
    test_ismarket_outside_rth_close();
    test_ismarket_postmarket_is_false();
    test_ismarket_premarket_is_false();
    test_ispremarket_true();
    test_ispremarket_before_0400_false();
    test_ispremarket_inside_rth_false();
    test_ispostmarket_true();
    test_ispostmarket_inside_rth_false();
    test_ispostmarket_premarket_false();
    test_24x7_ismarket_always_true();
    test_24x7_prepost_always_false();
    test_hhmm_to_minutes_basic();
    test_ismarket_weekend_filter();
    test_firstlastbar_transitions();
    test_start_equals_end_is_full_day();

    std::printf("\nsession_predicates: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
