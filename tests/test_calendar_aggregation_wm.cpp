/*
 * test_calendar_aggregation_wm.cpp — Weekly/Monthly request.security calendar
 * aggregation, end-to-end through the real TimeframeAggregator.
 *
 * Production-readiness probe (WS1/#4). Engine-only.
 *
 * Skeptic's objection: "every HTF probe in the corpus is 60/240/D — 'D works so
 * W/M follow' is a non-sequitur. Month arithmetic (28/29/30/31-day, leap Feb,
 * Dec->Jan year rollover) and ISO weeks are structurally different from daily."
 *
 * Oracle: feed UTC-midnight daily bars to TimeframeAggregator("M","D") and
 * assert each COMPLETED month's sub_bar_count equals that month's true day
 * count — the month-length-sensitive proof. Plus a weekly 7-day check.
 */

#include <cstdint>
#include <cstdio>
#include <vector>

#include <pineforge/timeframe.hpp>
#include <pineforge/bar.hpp>

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

namespace {

// Howard Hinnant's days_from_civil: days since 1970-01-01 (UTC), proleptic
// Gregorian. No libc, no tz — pure integer arithmetic, fully deterministic.
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t utc_midnight_ms(int y, unsigned m, unsigned d) {
    return days_from_civil(y, m, d) * 86400LL * 1000LL;
}

Bar daily_bar(int y, unsigned m, unsigned d) {
    Bar b;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.0;
    b.volume = 1000.0;
    b.timestamp = utc_midnight_ms(y, m, d);
    return b;
}

// Days in month (Gregorian).
unsigned dim(int y, unsigned m) {
    static const unsigned d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

// Feed every UTC day in [start, end) (half-open) through the aggregator and
// collect the sub_bar_count of every completed period.
std::vector<int> completed_subcounts(TimeframeAggregator& agg,
                                     int y0, unsigned m0, unsigned d0,
                                     int y1, unsigned m1, unsigned d1) {
    std::vector<int> out;
    int64_t cur = days_from_civil(y0, m0, d0);
    int64_t last = days_from_civil(y1, m1, d1);
    for (; cur < last; ++cur) {
        // reconstruct civil date for the bar timestamp
        Bar b;
        b.open = 100; b.high = 101; b.low = 99; b.close = 100; b.volume = 1000;
        b.timestamp = cur * 86400LL * 1000LL;
        AggregatedBar r = agg.feed(b);
        if (r.is_complete) out.push_back(r.sub_bar_count);
    }
    return out;
}

}  // namespace

// Monthly: each completed month's sub_bar_count == its true day count, across
// a leap February and a Dec->Jan year rollover.
static void test_monthly_subcounts_match_day_count() {
    std::printf("test_monthly_subcounts_match_day_count\n");
    TimeframeAggregator agg("M", "D");
    // 2023-12-01 .. 2024-04-01 : completes Dec 2023, Jan, Feb(leap), Mar 2024.
    std::vector<int> got = completed_subcounts(agg, 2023, 12, 1, 2024, 4, 1);
    // We expect at least the four interior months to have completed.
    int expect[] = { (int)dim(2023,12), (int)dim(2024,1),
                     (int)dim(2024,2),  (int)dim(2024,3) };  // 31,31,29,31
    CHECK(got.size() >= 4);
    for (size_t i = 0; i < got.size() && i < 4; ++i) {
        CHECK(got[i] == expect[i]);
    }
    // Leap February must be 29, not 28 — the month-length-sensitive proof.
    bool saw_29 = false;
    for (int c : got) if (c == 29) saw_29 = true;
    CHECK(saw_29);
}

// Weekly: full interior weeks aggregate exactly 7 daily bars.
static void test_weekly_full_weeks_are_seven() {
    std::printf("test_weekly_full_weeks_are_seven\n");
    TimeframeAggregator agg("W", "D");
    std::vector<int> got = completed_subcounts(agg, 2024, 1, 1, 2024, 3, 1);
    CHECK(got.size() >= 6);
    int sevens = 0;
    for (int c : got) {
        CHECK(c >= 1 && c <= 7);          // never over-counts a week
        if (c == 7) ++sevens;
    }
    CHECK(sevens >= 6);                    // most interior weeks are full
}

int main() {
    test_monthly_subcounts_match_day_count();
    test_weekly_full_weeks_are_seven();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
