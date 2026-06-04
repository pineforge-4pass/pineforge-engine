/*
 * test_timeframe_aggregator_extra.cpp — coverage densification for
 * src/timeframe.cpp.
 *
 * Targets the lines the existing test_timeframe.cpp / test_calendar_aggregation_wm.cpp
 * never exercise:
 *
 *   - tf_change(): the prev_ms==0 / curr_ms==0 first-bar arm, the calendar-period
 *     branch (D/W/M via crosses_boundary), the intraday seconds-bucket branch,
 *     and the secs<=0 guard.
 *   - crosses_boundary(CalendarPeriod::NONE) -> always false.
 *   - TimeframeAggregator string ctor falling back to PASSTHROUGH when the
 *     requested target TF is finer than the input TF (tf_ratio < 0), plus the
 *     passthrough feed() path: every input bar emits one complete bar unchanged.
 *
 * Expected values are derived from UTC wall-clock arithmetic (verified against
 * Python's datetime) and from the documented OHLCV aggregation rules
 * (high=max, low=min, close=last, volume=sum) — not tautologies.
 */

#include <cstdint>
#include <cstdio>
#include <string>

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

static Bar make_bar(double o, double h, double l, double c, double v,
                    int64_t ts_ms) {
    return Bar{o, h, l, c, v, ts_ms};
}

// ─── Verified UTC-millisecond timestamps (see Python cross-check) ────────────
//   2024-01-15 is a Monday; weeks are Monday-start.
static constexpr int64_t MON_2024_01_15_00 = 1705276800000LL; // 2024-01-15 00:00
static constexpr int64_t MON_2024_01_15_12 = 1705320000000LL; // 2024-01-15 12:00
static constexpr int64_t TUE_2024_01_16_00 = 1705363200000LL; // 2024-01-16 00:00
static constexpr int64_t FRI_2024_01_19_00 = 1705622400000LL; // 2024-01-19 00:00 (same wk)
static constexpr int64_t MON_2024_01_22_00 = 1705881600000LL; // 2024-01-22 00:00 (next wk)
static constexpr int64_t SUN_2024_01_28_00 = 1706400000000LL; // 2024-01-28 00:00 (same mo)
static constexpr int64_t THU_2024_02_01_00 = 1706745600000LL; // 2024-02-01 00:00 (next mo)

// ─── tf_change ───────────────────────────────────────────────────────────────

static void test_tf_change_first_bar_arm() {
    std::printf("test_tf_change_first_bar_arm\n");
    // prev_ms == 0 -> first-bar semantics: never a change (line 183).
    CHECK(tf_change(0, MON_2024_01_15_00, "D") == false);
    CHECK(tf_change(0, TUE_2024_01_16_00, "60") == false);
    // curr_ms == 0 also short-circuits to false.
    CHECK(tf_change(MON_2024_01_15_00, 0, "D") == false);
}

static void test_tf_change_calendar_daily() {
    std::printf("test_tf_change_calendar_daily\n");
    // Same calendar day (00:00 vs 12:00) -> no boundary cross.
    CHECK(tf_change(MON_2024_01_15_00, MON_2024_01_15_12, "D") == false);
    // Crossing into the next UTC day -> true.
    CHECK(tf_change(MON_2024_01_15_00, TUE_2024_01_16_00, "D") == true);
    // "1D" parses to the same DAY calendar period.
    CHECK(tf_change(MON_2024_01_15_00, TUE_2024_01_16_00, "1D") == true);
}

static void test_tf_change_calendar_weekly() {
    std::printf("test_tf_change_calendar_weekly\n");
    // Mon -> Fri of the same Monday-start week -> no cross.
    CHECK(tf_change(MON_2024_01_15_00, FRI_2024_01_19_00, "W") == false);
    // Mon -> next Mon -> crosses the week boundary.
    CHECK(tf_change(MON_2024_01_15_00, MON_2024_01_22_00, "W") == true);
}

static void test_tf_change_calendar_monthly() {
    std::printf("test_tf_change_calendar_monthly\n");
    // Two days inside January -> same month.
    CHECK(tf_change(MON_2024_01_15_00, SUN_2024_01_28_00, "M") == false);
    // January -> February -> crosses the month boundary.
    CHECK(tf_change(MON_2024_01_15_00, THU_2024_02_01_00, "M") == true);
}

static void test_tf_change_intraday_bucket() {
    std::printf("test_tf_change_intraday_bucket\n");
    // tf "60" => 3600 s => 3,600,000 ms hour buckets.
    // 00:00 and 00:30 share the 473688th hour bucket -> no change.
    int64_t at_30min = MON_2024_01_15_00 + 30LL * 60 * 1000;
    CHECK(tf_change(MON_2024_01_15_00, at_30min, "60") == false);
    // 00:00 vs 01:00 -> next hour bucket (473689) -> change.
    int64_t at_60min = MON_2024_01_15_00 + 60LL * 60 * 1000;
    CHECK(tf_change(MON_2024_01_15_00, at_60min, "60") == true);
    // tf "5" => 300 s => 300,000 ms buckets. 00:00 vs 00:04 same bucket.
    int64_t at_4min = MON_2024_01_15_00 + 4LL * 60 * 1000;
    CHECK(tf_change(MON_2024_01_15_00, at_4min, "5") == false);
    // 00:00 vs 00:05 -> next 5-min bucket -> change.
    int64_t at_5min = MON_2024_01_15_00 + 5LL * 60 * 1000;
    CHECK(tf_change(MON_2024_01_15_00, at_5min, "5") == true);
}

static void test_tf_change_secs_guard() {
    std::printf("test_tf_change_secs_guard\n");
    // Bare "S" parses to 0 seconds (no canonical meaning) -> tf_to_seconds==0,
    // so tf_change hits the `secs <= 0` guard and returns false even across an
    // otherwise large timestamp jump.
    CHECK(tf_to_seconds("S") == 0);
    CHECK(tf_change(MON_2024_01_15_00, THU_2024_02_01_00, "S") == false);
    // Empty string -> 0 seconds -> same guard.
    CHECK(tf_change(MON_2024_01_15_00, THU_2024_02_01_00, "") == false);
}

// ─── crosses_boundary NONE arm ────────────────────────────────────────────────

static void test_crosses_boundary_none() {
    std::printf("test_crosses_boundary_none\n");
    // CalendarPeriod::NONE never reports a boundary cross, regardless of how far
    // apart the two timestamps are (lines 174-175).
    CHECK(crosses_boundary(MON_2024_01_15_00, THU_2024_02_01_00,
                           CalendarPeriod::NONE) == false);
    CHECK(crosses_boundary(MON_2024_01_15_00, MON_2024_01_15_00,
                           CalendarPeriod::NONE) == false);
    // calendar_period_for of a numeric (minute) TF is NONE.
    CHECK(calendar_period_for("60") == CalendarPeriod::NONE);
    CHECK(calendar_period_for("") == CalendarPeriod::NONE);
}

// ─── PASSTHROUGH fallback via string ctor (target finer than input) ──────────

static void test_passthrough_fallback_target_finer_than_input() {
    std::printf("test_passthrough_fallback_target_finer_than_input\n");
    // target "5" is FINER than input "60" => tf_ratio("60","5") == -2 => the
    // string ctor falls back to PASSTHROUGH (lines 220-222), NOT active.
    CHECK(tf_ratio("60", "5") == -2);
    TimeframeAggregator agg("5", "60");
    CHECK(!agg.is_active());

    // In passthrough, EVERY input bar emits one complete bar, unchanged.
    Bar b1 = make_bar(100, 105, 98, 102, 10, 1000);
    auto r1 = agg.feed(b1);
    CHECK(r1.is_complete);
    CHECK(r1.sub_bar_count == 1);
    CHECK(r1.bar.open == 100.0);
    CHECK(r1.bar.high == 105.0);
    CHECK(r1.bar.low == 98.0);
    CHECK(r1.bar.close == 102.0);
    CHECK(r1.bar.volume == 10.0);
    CHECK(r1.bar.timestamp == 1000);
    // last_completed mirrors the just-fed bar (no max/min/sum accumulation).
    CHECK(agg.last_completed().close == 102.0);
    CHECK(agg.current().close == 102.0);

    // A second, lower bar must NOT carry over the previous high/low/volume —
    // passthrough is stateless per bar.
    Bar b2 = make_bar(90, 92, 80, 85, 7, 2000);
    auto r2 = agg.feed(b2);
    CHECK(r2.is_complete);
    CHECK(r2.sub_bar_count == 1);
    CHECK(r2.bar.open == 90.0);
    CHECK(r2.bar.high == 92.0);   // not max(105,92)
    CHECK(r2.bar.low == 80.0);    // not min(98,80)
    CHECK(r2.bar.close == 85.0);
    CHECK(r2.bar.volume == 7.0);  // not 10+7
    CHECK(r2.bar.timestamp == 2000);
    CHECK(agg.last_completed().volume == 7.0);
    CHECK(agg.current().open == 90.0);
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== TimeframeAggregator Extra Coverage Tests ===\n\n");

    test_tf_change_first_bar_arm();
    test_tf_change_calendar_daily();
    test_tf_change_calendar_weekly();
    test_tf_change_calendar_monthly();
    test_tf_change_intraday_bucket();
    test_tf_change_secs_guard();
    test_crosses_boundary_none();
    test_passthrough_fallback_target_finer_than_input();

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
