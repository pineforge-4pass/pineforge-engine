#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <pineforge/timeframe.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

// ─── helpers ───────────────────────────────────────────────────────────────────

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                           \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static Bar make_bar(double o, double h, double l, double c, double v,
                    int64_t ts_ms) {
    return Bar{o, h, l, c, v, ts_ms};
}

// ─── TF string parsing ────────────────────────────────────────────────────────

static void test_tf_to_seconds() {
    std::printf("test_tf_to_seconds\n");
    CHECK(tf_to_seconds("1")   == 60);
    CHECK(tf_to_seconds("5")   == 300);
    CHECK(tf_to_seconds("15")  == 900);
    CHECK(tf_to_seconds("30")  == 1800);
    CHECK(tf_to_seconds("60")  == 3600);
    CHECK(tf_to_seconds("120") == 7200);
    CHECK(tf_to_seconds("240") == 14400);
    CHECK(tf_to_seconds("D")   == 86400);
    CHECK(tf_to_seconds("1D")  == 86400);
    CHECK(tf_to_seconds("W")   == 604800);
    CHECK(tf_to_seconds("1W")  == 604800);
    CHECK(tf_to_seconds("M")   == -1);    // calendar-based
    CHECK(tf_to_seconds("1M")  == -1);
}

static void test_tf_ratio() {
    std::printf("test_tf_ratio\n");
    // 5-min bars aggregated to 15-min => ratio 3
    CHECK(tf_ratio("5", "15") == 3);
    // 1-min to 60-min => ratio 60
    CHECK(tf_ratio("1", "60") == 60);
    // 1-min to daily => 1440
    CHECK(tf_ratio("1", "D") == 1440);
    // 5-min to daily => 288
    CHECK(tf_ratio("5", "D") == 288);
    // daily to weekly => 7 (approximate but that's the convention)
    // Actually 604800/86400 = 7
    CHECK(tf_ratio("D", "W") == 7);
    // target < input => error
    CHECK(tf_ratio("60", "5") == -2);
    // same timeframe => 1
    CHECK(tf_ratio("5", "5") == 1);
    // monthly is calendar-based
    CHECK(tf_ratio("1", "M") == -1);
    CHECK(tf_ratio("D", "M") == -1);
}

// ─── Ratio-based aggregation ──────────────────────────────────────────────────

static void test_ratio_aggregation() {
    std::printf("test_ratio_aggregation (3 bars -> 1)\n");

    TimeframeAggregator agg(3);
    CHECK(agg.is_active());

    // Bar 1 of 3
    auto r1 = agg.feed(make_bar(100, 105, 98, 102, 10, 1000));
    CHECK(!r1.is_complete);
    CHECK(r1.sub_bar_count == 1);
    CHECK(r1.bar.open == 100.0);
    CHECK(r1.bar.high == 105.0);
    CHECK(r1.bar.low == 98.0);
    CHECK(r1.bar.close == 102.0);

    // Bar 2 of 3
    auto r2 = agg.feed(make_bar(102, 110, 97, 108, 20, 2000));
    CHECK(!r2.is_complete);
    CHECK(r2.sub_bar_count == 2);
    CHECK(r2.bar.open == 100.0);   // open stays from first bar
    CHECK(r2.bar.high == 110.0);   // max of highs
    CHECK(r2.bar.low == 97.0);     // min of lows
    CHECK(r2.bar.close == 108.0);  // close updates

    // Bar 3 of 3 — completes
    auto r3 = agg.feed(make_bar(108, 112, 100, 106, 30, 3000));
    CHECK(r3.is_complete);
    CHECK(r3.sub_bar_count == 3);
    CHECK(r3.bar.open == 100.0);
    CHECK(r3.bar.high == 112.0);
    CHECK(r3.bar.low == 97.0);
    CHECK(r3.bar.close == 106.0);
    CHECK(r3.bar.volume == 60.0);  // 10+20+30
    CHECK(r3.bar.timestamp == 1000); // timestamp of the first bar

    // Verify last_completed
    Bar lc = agg.last_completed();
    CHECK(lc.open == 100.0);
    CHECK(lc.close == 106.0);

    // Next bar starts a new cycle
    auto r4 = agg.feed(make_bar(106, 107, 104, 105, 5, 4000));
    CHECK(!r4.is_complete);
    CHECK(r4.sub_bar_count == 1);
    CHECK(r4.bar.open == 106.0);
}

// ─── Passthrough (default constructor) ────────────────────────────────────────

static void test_passthrough() {
    std::printf("test_passthrough (no aggregation)\n");

    TimeframeAggregator agg;  // default: passthrough
    CHECK(!agg.is_active());

    auto r = agg.feed(make_bar(100, 105, 98, 102, 10, 1000));
    CHECK(r.is_complete);
    CHECK(r.sub_bar_count == 1);
    CHECK(r.bar.open == 100.0);
}

// ─── Calendar-based aggregation (daily from 1-min bars) ──────────────────────

static void test_calendar_daily_aggregation() {
    std::printf("test_calendar_daily_aggregation\n");

    // Aggregate 1-min bars into daily bars.
    // We'll use "D" target with "1" input.
    TimeframeAggregator agg("D", "1");
    CHECK(agg.is_active());

    // Create bars on 2024-01-15 (Monday).
    // 2024-01-15 00:00 UTC = 1705276800 seconds = 1705276800000 ms
    int64_t day1_start_ms = 1705276800LL * 1000;

    // Feed a bar at 00:00
    auto r1 = agg.feed(make_bar(100, 105, 98, 102, 10, day1_start_ms));
    CHECK(!r1.is_complete);
    CHECK(r1.sub_bar_count == 1);

    // Feed a bar at 00:01
    auto r2 = agg.feed(make_bar(102, 106, 101, 104, 15,
                                 day1_start_ms + 60 * 1000));
    CHECK(!r2.is_complete);
    CHECK(r2.sub_bar_count == 2);
    CHECK(r2.bar.open == 100.0);
    CHECK(r2.bar.high == 106.0);

    // Feed a bar at 23:59 same day — with 1m input, the daily aggregate is *complete*
    // once the last minute of the session is merged (TV-style; see TimeframeAggregator).
    auto r3 = agg.feed(make_bar(104, 107, 99, 103, 20,
                                 day1_start_ms + 1439 * 60 * 1000));
    CHECK(r3.is_complete);
    CHECK(r3.bar.high == 107.0);
    CHECK(r3.bar.low == 98.0);
    CHECK(r3.bar.close == 103.0);
    CHECK(r3.sub_bar_count == 3);

    Bar lc_after_r3 = agg.last_completed();
    CHECK(lc_after_r3.open == 100.0);
    CHECK(lc_after_r3.volume == 45.0);  // 10+15+20

    // First bar of the next calendar day: start a new in-progress aggregate (the
    // previous day was already finalized on the 23:59 feed).
    int64_t day2_start_ms = day1_start_ms + 86400LL * 1000;
    auto r4 = agg.feed(make_bar(103, 108, 102, 105, 25, day2_start_ms));
    CHECK(!r4.is_complete);
    Bar lc = agg.last_completed();
    CHECK(lc.open == 100.0);
    CHECK(lc.high == 107.0);
    CHECK(lc.low == 98.0);
    CHECK(lc.close == 103.0);
    CHECK(lc.volume == 45.0);

    Bar cur = agg.current();
    CHECK(cur.open == 103.0);
    CHECK(cur.close == 105.0);
}

// ─── Calendar-based weekly aggregation ───────────────────────────────────────

static void test_calendar_weekly_aggregation() {
    std::printf("test_calendar_weekly_aggregation\n");

    TimeframeAggregator agg("W", "D");
    CHECK(agg.is_active());

    // 2024-01-15 is Monday. Feed daily bars Mon-Fri.
    int64_t mon_ms = 1705276800LL * 1000;  // 2024-01-15 00:00 UTC

    agg.feed(make_bar(100, 105, 98, 102, 100, mon_ms));
    agg.feed(make_bar(102, 106, 100, 104, 110, mon_ms + 86400LL * 1000));
    agg.feed(make_bar(104, 108, 101, 106, 120, mon_ms + 2 * 86400LL * 1000));
    agg.feed(make_bar(106, 110, 103, 107, 130, mon_ms + 3 * 86400LL * 1000));
    auto r_fri = agg.feed(make_bar(107, 112, 105, 109, 140,
                                    mon_ms + 4 * 86400LL * 1000));
    CHECK(!r_fri.is_complete); // Friday is still same week

    // Next Monday starts a new week => completes previous
    int64_t next_mon_ms = mon_ms + 7 * 86400LL * 1000;
    auto r_next = agg.feed(make_bar(109, 111, 107, 110, 150, next_mon_ms));
    CHECK(r_next.is_complete);

    Bar lc = agg.last_completed();
    CHECK(lc.open == 100.0);
    CHECK(lc.high == 112.0);
    CHECK(lc.low == 98.0);
    CHECK(lc.close == 109.0);
    CHECK(lc.volume == 600.0); // 100+110+120+130+140
}

// ─── script_tf finer than input_tf must throw ─────────────────────────────────

class NoopStrategy : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}
};

static void test_run_rejects_script_tf_finer_than_input_tf() {
    std::printf("test_run_rejects_script_tf_finer_than_input_tf\n");
    NoopStrategy strat;
    std::vector<Bar> bars = {
        make_bar(10, 10, 10, 10, 100, 0),
        make_bar(11, 11, 11, 11, 100, 3600000),
        make_bar(12, 12, 12, 12, 100, 7200000),
    };
    // Engine must NOT throw across its public API — exceptions get
    // captured into last_error() so the C ABI can never unwind a C++
    // exception across the extern "C" boundary.
    bool threw = false;
    try {
        strat.run(bars.data(), static_cast<int>(bars.size()),
                  "60", "5", false, 0, MagnifierDistribution::ENDPOINTS);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(!strat.last_error().empty());
    CHECK(strat.last_error().find("script timeframe must be coarser")
          != std::string::npos);

    // Subsequent successful run must clear the captured error.
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "60", "60", false, 0, MagnifierDistribution::ENDPOINTS);
    CHECK(strat.last_error().empty());
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== TimeframeAggregator Tests ===\n\n");

    test_tf_to_seconds();
    test_tf_ratio();
    test_ratio_aggregation();
    test_passthrough();
    test_calendar_daily_aggregation();
    test_calendar_weekly_aggregation();
    test_run_rejects_script_tf_finer_than_input_tf();

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
