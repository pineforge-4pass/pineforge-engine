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

    std::printf("\nsession_predicates: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
