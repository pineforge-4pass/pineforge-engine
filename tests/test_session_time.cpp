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

int main() {
    test_time_hourly_bucket_utc();
    test_time_session_ny_inside();
    test_time_session_ny_outside();
    test_time_weekday_filter_mon_fri_only();
    test_time_close_hourly();

    std::printf("session_time: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
