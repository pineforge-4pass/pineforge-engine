#include <cstdio>
#include <string>

#include <pineforge/na.hpp>
#include <pineforge/session_time.hpp>

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

static void test_time_hourly_bucket_utc() {
    std::printf("test_time_hourly_bucket_utc\n");
    int64_t bar = 1775572200000LL;  // 2026-04-07 14:30 UTC
    int64_t t = pine_time(bar, "60", "", "UTC", "60");
    CHECK(!is_na(t));
    // Hour bucket in UTC: 14:00 UTC
    CHECK(t == 1775570400000LL);
}

static void test_time_session_ny_inside() {
    std::printf("test_time_session_ny_inside\n");
    int64_t bar = 1775572200000LL;  // 14:30 UTC = 10:30 America/New_York (EDT)
    int64_t t = pine_time(bar, "60", "0800-1600", "America/New_York", "60");
    CHECK(!is_na(t));
}

static void test_time_session_ny_outside() {
    std::printf("test_time_session_ny_outside\n");
    int64_t bar = 1775601000000LL;  // 22:30 UTC = 18:30 NY — outside 0800-1600
    int64_t t = pine_time(bar, "60", "0800-1600", "America/New_York", "60");
    CHECK(is_na(t));
}

static void test_time_weekday_filter_mon_fri_only() {
    std::printf("test_time_weekday_filter_mon_fri_only\n");
    // Saturday 2026-04-12 — session hours in NY but :23456 excludes Sat/Sun
    int64_t bar_sat = 1776009600000LL;  // 2026-04-12 16:00 UTC
    int64_t t = pine_time(bar_sat, "60", "0800-1600:23456", "America/New_York", "60");
    CHECK(is_na(t));
}

static void test_time_close_hourly() {
    std::printf("test_time_close_hourly\n");
    int64_t bar = 1775572200000LL;
    int64_t tc = pine_time_close(bar, "60", "", "UTC", "60");
    CHECK(!is_na(tc));
    int64_t to = pine_time(bar, "60", "", "UTC", "60");
    CHECK(tc == to + 3600000 - 1);
}

// --- 2-arg time(tf, tz): a timezone passed in the session slot ---

static void test_time_tz_in_session_slot_equiv() {
    std::printf("test_time_tz_in_session_slot_equiv\n");
    // time("D", "America/New_York") binds the tz into the session slot. That is
    // an invalid session, which TV ignores entirely — it does NOT adopt the
    // string as the timezone. The daily boundary rolls at the chart/exchange
    // (UTC) timezone, exactly as a plain time("D") does. It must not be na, and
    // must NOT match the 3-arg explicit-tz form (which still rolls at NY).
    int64_t bar = 1775572200000LL;  // 2026-04-07 14:30 UTC = 10:30 NY (EDT)
    int64_t two_arg = pine_time(bar, "D", "America/New_York", "", "15");
    int64_t plain   = pine_time(bar, "D", "", "", "15");                // time("D") → UTC/chart
    int64_t three_arg = pine_time(bar, "D", "", "America/New_York", "15");  // explicit tz → NY
    CHECK(!is_na(two_arg));          // was na before PR#66; still not na
    CHECK(two_arg == plain);         // invalid session tz-string ignored → rolls at UTC/chart
    CHECK(two_arg != three_arg);     // explicit-tz 3-arg form still uses the given tz
}

static void test_time_tz_in_session_daily_change() {
    std::printf("test_time_tz_in_session_daily_change\n");
    // The tz-string session is ignored, so the daily boundary rolls at UTC.
    // Bars in the same UTC-day share the daily time; the next UTC-day differs —
    // this is what makes ta.change(time("D", tz)) fire once per day.
    int64_t bar_a = 1775572200000LL;               // 2026-04-07 14:30 UTC
    int64_t bar_b = bar_a + 3600000LL;             // +1h → 15:30 UTC, same UTC-day
    int64_t bar_next = bar_a + 24LL * 3600000LL;   // +24h, next UTC-day
    int64_t ta = pine_time(bar_a, "D", "America/New_York", "", "15");
    int64_t tb = pine_time(bar_b, "D", "America/New_York", "", "15");
    int64_t tn = pine_time(bar_next, "D", "America/New_York", "", "15");
    CHECK(ta == tb);   // same day: no change
    CHECK(ta != tn);   // new day: change fires
}

static void test_time_real_session_2arg_still_filters() {
    std::printf("test_time_real_session_2arg_still_filters\n");
    // A genuine 2-arg session (no tz) must still filter — the reinterpretation
    // only triggers for tz-looking strings, never "0800-1600".
    int64_t bar_in  = 1775572200000LL;  // 14:30 UTC — inside 0800-1600 UTC
    int64_t bar_out = 1775601000000LL;  // 22:30 UTC — outside
    CHECK(!is_na(pine_time(bar_in,  "60", "0800-1600", "", "60")));
    CHECK( is_na(pine_time(bar_out, "60", "0800-1600", "", "60")));
}

static void test_time_gmt_in_session_slot() {
    std::printf("test_time_gmt_in_session_slot\n");
    // GMT/UTC specifiers in the session slot are also recognized as timezones.
    int64_t bar = 1775572200000LL;
    CHECK(!is_na(pine_time(bar, "D", "GMT+0", "", "15")));
    CHECK(!is_na(pine_time(bar, "D", "UTC", "", "15")));
}

int main() {
    test_time_hourly_bucket_utc();
    test_time_session_ny_inside();
    test_time_session_ny_outside();
    test_time_weekday_filter_mon_fri_only();
    test_time_close_hourly();
    test_time_tz_in_session_slot_equiv();
    test_time_tz_in_session_daily_change();
    test_time_real_session_2arg_still_filters();
    test_time_gmt_in_session_slot();

    std::printf("session_time: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
