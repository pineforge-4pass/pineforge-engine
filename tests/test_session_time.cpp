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
    // Intraday time_close is the EXACT bar-close boundary (open + duration),
    // == the next bar's open — NOT the last ms of the bar. So minute()/hour()
    // of a boundary-aligned bar match TV (see session_time.cpp compute_tf_close_ms).
    CHECK(tc == to + 3600000);
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

// --- syminfo_tz: default zone for a tz-less session (Pine: exchange tz) ---

static void test_time_session_default_tz_is_syminfo() {
    std::printf("test_time_session_default_tz_is_syminfo\n");
    // time(tf, "0930-1600") with no tz argument: Pine reads the session in
    // syminfo.timezone. 12:00Z is 08:00 EDT (outside NY RTH) but inside
    // 0930-1600 read as UTC; 19:30Z is 15:30 EDT (inside) but outside in UTC.
    int64_t bar_1200z = 1775563200000LL;  // 2026-04-07 12:00 UTC = 08:00 EDT
    int64_t bar_1930z = 1775590200000LL;  // 2026-04-07 19:30 UTC = 15:30 EDT
    // Historical default (empty syminfo_tz) == UTC, byte-identical.
    CHECK(!is_na(pine_time(bar_1200z, "15", "0930-1600", "", "15")));
    CHECK( is_na(pine_time(bar_1930z, "15", "0930-1600", "", "15")));
    CHECK(!is_na(pine_time(bar_1200z, "15", "0930-1600", "", "15", "")));
    CHECK(!is_na(pine_time(bar_1200z, "15", "0930-1600", "", "15", "UTC")));
    // syminfo.timezone = America/New_York flips both bars.
    CHECK( is_na(pine_time(bar_1200z, "15", "0930-1600", "", "15", "America/New_York")));
    CHECK(!is_na(pine_time(bar_1930z, "15", "0930-1600", "", "15", "America/New_York")));
    // time_close follows the same rule.
    CHECK( is_na(pine_time_close(bar_1200z, "15", "0930-1600", "", "15", "America/New_York")));
    CHECK(!is_na(pine_time_close(bar_1930z, "15", "0930-1600", "", "15", "America/New_York")));
    // The intraday open value itself is unaffected (UTC bucket).
    CHECK(pine_time(bar_1930z, "15", "0930-1600", "", "15", "America/New_York") == bar_1930z);
}

static void test_time_session_explicit_tz_beats_syminfo() {
    std::printf("test_time_session_explicit_tz_beats_syminfo\n");
    int64_t bar_1200z = 1775563200000LL;  // 08:00 EDT / 12:00 UTC / 21:00 Tokyo
    // Explicit "UTC" wins over syminfo NY: 12:00 is inside 0930-1600.
    CHECK(!is_na(pine_time(bar_1200z, "15", "0930-1600", "UTC", "15", "America/New_York")));
    // Explicit NY wins over syminfo UTC: 08:00 EDT is outside.
    CHECK( is_na(pine_time(bar_1200z, "15", "0930-1600", "America/New_York", "15", "UTC")));
}

static void test_time_session_syminfo_tz_dst_aware() {
    std::printf("test_time_session_syminfo_tz_dst_aware\n");
    // Session "0930-1600" in America/New_York across the 2025-11-02 fall-back:
    // the UTC start of the window steps from 13:30Z (EDT) to 14:30Z (EST).
    int64_t fri_1345z = 1761918300000LL;  // 2025-10-31 13:45 UTC = 09:45 EDT → inside
    int64_t mon_1345z = 1762177500000LL;  // 2025-11-03 13:45 UTC = 08:45 EST → outside
    int64_t mon_1445z = 1762181100000LL;  // 2025-11-03 14:45 UTC = 09:45 EST → inside
    CHECK(!is_na(pine_time(fri_1345z, "15", "0930-1600", "", "15", "America/New_York")));
    CHECK( is_na(pine_time(mon_1345z, "15", "0930-1600", "", "15", "America/New_York")));
    CHECK(!is_na(pine_time(mon_1445z, "15", "0930-1600", "", "15", "America/New_York")));
    // Under the old UTC default all three were "inside" — DST-invariant in UTC.
    CHECK(!is_na(pine_time(fri_1345z, "15", "0930-1600", "", "15")));
    CHECK(!is_na(pine_time(mon_1345z, "15", "0930-1600", "", "15")));
}

static void test_time_syminfo_tz_does_not_move_calendar_open() {
    std::printf("test_time_syminfo_tz_does_not_move_calendar_open\n");
    // syminfo_tz is a SESSION default only. The D open of a tz-less call keeps
    // rolling exactly where it did before (UTC) — the calendar path is owned
    // by a separate fix. An explicit tz still moves it (pre-existing).
    int64_t bar = 1775572200000LL;  // 2026-04-07 14:30 UTC = 10:30 EDT
    int64_t plain    = pine_time(bar, "D", "", "", "15");
    int64_t with_sym = pine_time(bar, "D", "", "", "15", "America/New_York");
    int64_t sess_sym = pine_time(bar, "D", "0000-2359", "", "15", "America/New_York");
    int64_t explicit_ny = pine_time(bar, "D", "0000-2359", "America/New_York", "15");
    CHECK(plain == 1775520000000LL);      // 2026-04-07 00:00 UTC
    CHECK(with_sym == plain);
    CHECK(sess_sym == plain);
    CHECK(explicit_ny != plain);
    CHECK(pine_time_close(bar, "D", "", "", "15", "America/New_York")
          == pine_time_close(bar, "D", "", "", "15"));
    // tz-looking string in the session slot is still dropped (invalid session),
    // not adopted, regardless of syminfo_tz.
    CHECK(pine_time(bar, "D", "America/New_York", "", "15", "America/New_York") == plain);
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
    test_time_session_default_tz_is_syminfo();
    test_time_session_explicit_tz_beats_syminfo();
    test_time_session_syminfo_tz_dst_aware();
    test_time_syminfo_tz_does_not_move_calendar_open();

    std::printf("session_time: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
