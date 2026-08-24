// test_security_range_start_bucket_gating — pins the KI-55 range-start cut on
// HTF-BUCKET opens (finding 452, rank 2).
//
// TradingView's deep-backtest request.security series are built from the HTF
// bars whose OPEN lies inside the loaded chart range. A bucket that opened
// before the range start is absent — it is not a partial first bar. The engine
// flag ``security_range_start_na_warmup`` therefore drops, per evaluator, every
// input bar whose D/W/M (or intraday-grid) bucket opened before the range
// start, so the first HTF bar every series sees is a whole bucket that opened
// at/after the range start. On OANDA:EURUSD (America/New_York, 1700-1700) with
// the lab's pad epoch 2025-03-31 00:00 UTC that means:
//   D  first bar = the session opening Mon 2025-03-31 17:00 EDT (trading date
//      Apr 1), not the remainder of the Sunday session;
//   W  first bar = the week opening Sun 2025-04-06 17:00 EDT (the week that
//      opened Sun Mar 30 17:00 EDT straddles the range start and is dropped);
//   M  first bar = April (opens Mon Mar 31 17:00 EDT); the March remainder is
//      dropped.
// heneralmomo25-selda-97ma's weekly EMA26 re-simulation reproduces TV 29/29
// only under exactly that weekly series (SMA-seeded from the Apr-6 week).
//
// With a range start on the bucket grid — 24x7 UTC midnight for intraday TFs
// and D, Monday for W, the 1st for M — the cut is the plain timestamp cut, so
// the existing corpus pins (test_security_range_start_na_warmup) hold.
//
// It FAILS without the fix: the timestamp cut keeps the straddling remainder
// as HTF bar 1, so every "first completed bucket" assertion below reports the
// pre-range bucket's open instead.

#include <pineforge/engine.hpp>
#include <pineforge/na.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace pineforge;

static int failures = 0;

#define CHECK(cond, tag) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", (tag), __LINE__); \
        ++failures; \
    } \
} while (0)

#define CHECK_EQ_MS(actual, expected, tag) do { \
    const int64_t _a = (actual), _e = (expected); \
    if (_a != _e) { \
        std::printf("FAIL: %s (line %d): got %lld want %lld\n", (tag), __LINE__, \
                    (long long)_a, (long long)_e); \
        ++failures; \
    } \
} while (0)

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
static int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (static_cast<int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}

static const std::string NY = "America/New_York";
static const std::string FX = "1700-1700";

// ─── TimeframeAggregator::bucket_open_ms ─────────────────────────────────────

static void test_bucket_open_utc_grid() {
    TimeframeAggregator pass;
    CHECK_EQ_MS(pass.bucket_open_ms(123456789), 123456789,
                "passthrough returns the input timestamp");

    TimeframeAggregator hour("60", "15");
    CHECK_EQ_MS(hour.bucket_open_ms(4'500'000), 3'600'000,
                "60m grid: 01:15 belongs to the 01:00 bucket");
    CHECK_EQ_MS(hour.bucket_open_ms(3'600'000), 3'600'000,
                "60m grid: 01:00 opens its own bucket");
    CHECK_EQ_MS(hour.bucket_open_ms(7'199'999), 3'600'000,
                "60m grid: 01:59:59.999 still the 01:00 bucket");

    TimeframeAggregator day("D", "15");
    CHECK_EQ_MS(day.bucket_open_ms(utc_ms(2025, 3, 31, 0, 15)), utc_ms(2025, 3, 31),
                "UTC day opens at midnight");
    TimeframeAggregator week("W", "15");
    // Wed 2025-04-02 -> Monday 2025-03-31 (24x7 Monday-start week).
    CHECK_EQ_MS(week.bucket_open_ms(utc_ms(2025, 4, 2, 5, 0)), utc_ms(2025, 3, 31),
                "UTC week opens Monday 00:00");
    CHECK_EQ_MS(week.bucket_open_ms(utc_ms(2025, 3, 31)), utc_ms(2025, 3, 31),
                "UTC Monday 00:00 opens its week");
    TimeframeAggregator month("M", "15");
    CHECK_EQ_MS(month.bucket_open_ms(utc_ms(2025, 3, 31, 23, 45)), utc_ms(2025, 3, 1),
                "UTC month opens on the 1st");
    CHECK_EQ_MS(month.bucket_open_ms(utc_ms(2025, 4, 1)), utc_ms(2025, 4, 1),
                "UTC 1st 00:00 opens its month");
    std::printf("test_bucket_open_utc_grid: %s\n", failures ? "FAIL" : "ok");
}

static void test_bucket_open_forex_session() {
    int before = failures;
    // Sun 2025-03-30 20:15 EDT (= 2025-03-31 00:15 UTC): every period that
    // contains it opened at Sun 17:00 EDT = 2025-03-30 21:00 UTC.
    const int64_t sun_2015 = utc_ms(2025, 3, 31, 0, 15);
    const int64_t sun_open = utc_ms(2025, 3, 30, 21, 0);

    TimeframeAggregator h4("240", "15", NY, FX);
    CHECK_EQ_MS(h4.bucket_open_ms(sun_2015), sun_open,
                "240 grid anchored at the 17:00 session open");
    CHECK_EQ_MS(h4.bucket_open_ms(utc_ms(2025, 3, 31, 1, 0)), utc_ms(2025, 3, 31, 1, 0),
                "240 grid: 21:00 EDT opens the next bucket");
    TimeframeAggregator h1("60", "15", NY, FX);
    CHECK_EQ_MS(h1.bucket_open_ms(sun_2015), utc_ms(2025, 3, 31, 0, 0),
                "60 grid: 20:15 EDT belongs to the 20:00 EDT bucket");

    TimeframeAggregator day("D", "15", NY, FX);
    CHECK_EQ_MS(day.bucket_open_ms(sun_2015), sun_open, "forex day opens Sun 17:00 EDT");
    CHECK_EQ_MS(day.bucket_open_ms(utc_ms(2025, 3, 31, 20, 45)), sun_open,
                "Mon 16:45 EDT is still the Sunday session");
    CHECK_EQ_MS(day.bucket_open_ms(utc_ms(2025, 3, 31, 21, 0)), utc_ms(2025, 3, 31, 21, 0),
                "Mon 17:00 EDT opens the next session");

    TimeframeAggregator week("W", "15", NY, FX);
    CHECK_EQ_MS(week.bucket_open_ms(sun_2015), sun_open, "forex week opens Sun 17:00 EDT");
    CHECK_EQ_MS(week.bucket_open_ms(utc_ms(2025, 4, 4, 20, 45)), sun_open,
                "Fri 16:45 EDT closes the week that opened Sun Mar 30");
    CHECK_EQ_MS(week.bucket_open_ms(utc_ms(2025, 4, 6, 21, 0)), utc_ms(2025, 4, 6, 21, 0),
                "Sun Apr 6 17:00 EDT opens the next week");

    TimeframeAggregator month("M", "15", NY, FX);
    // March = sessions whose trading date is in March: opens on the
    // session-day of trading date Mar 1, i.e. Fri Feb 28 17:00 EST (UTC-5).
    CHECK_EQ_MS(month.bucket_open_ms(sun_2015), utc_ms(2025, 2, 28, 22, 0),
                "forex March opened Fri Feb 28 17:00 EST");
    CHECK_EQ_MS(month.bucket_open_ms(utc_ms(2025, 3, 31, 20, 45)), utc_ms(2025, 2, 28, 22, 0),
                "Mon Mar 31 16:45 EDT is still March");
    CHECK_EQ_MS(month.bucket_open_ms(utc_ms(2025, 3, 31, 21, 0)), utc_ms(2025, 3, 31, 21, 0),
                "Mon Mar 31 17:00 EDT (trading date Apr 1) opens April");
    std::printf("test_bucket_open_forex_session: %s\n",
                (failures > before) ? "FAIL" : "ok");
}

// ─── End-to-end: first completed HTF bucket under the flag ───────────────────

// Three lookahead_off evaluators on one input feed; records the OPEN
// timestamp (= aggregated bar timestamp) of every completed HTF bar per id.
class BucketGateHarness : public BacktestEngine {
public:
    std::vector<int64_t> completed[3];
    std::vector<double> completed_close[3];

    explicit BucketGateHarness(const char* input_tf,
                               const char* tf0, const char* tf1, const char* tf2) {
        register_security_eval(0, tf0, input_tf, false, false);
        register_security_eval(1, tf1, input_tf, false, false);
        register_security_eval(2, tf2, input_tf, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (!is_complete || sec_id < 0 || sec_id > 2) return;
        completed[sec_id].push_back(bar.timestamp);
        completed_close[sec_id].push_back(bar.close);
    }
    void on_bar(const Bar&) override {}
};

// 15m OANDA:EURUSD-shaped feed: Sun 17:00 EDT .. Fri 17:00 EDT, every week
// from Sun 2025-03-30 through Fri 2025-05-09 (EDT throughout: no DST edge).
static std::vector<Bar> make_forex_15m_feed() {
    std::vector<Bar> bars;
    const int64_t begin = utc_ms(2025, 3, 30, 21, 0);
    const int64_t end = utc_ms(2025, 5, 9, 21, 0);
    for (int64_t t = begin; t < end; t += 900'000) {
        const int64_t local = t - 4 * 3'600'000;                  // EDT
        const int64_t day = local / 86'400'000;                   // epoch day
        const int wday = static_cast<int>((day + 4) % 7);         // 0 = Sun
        const int hour = static_cast<int>((local % 86'400'000) / 3'600'000);
        const bool closed = (wday == 5 && hour >= 17) || wday == 6
                         || (wday == 0 && hour < 17);
        if (closed) continue;
        const double px = 1.0 + static_cast<double>(bars.size()) * 1e-5;
        bars.push_back(Bar{px, px, px, px, 1.0, t});
    }
    return bars;
}

static void test_forex_flag_on_first_whole_bucket() {
    int before = failures;
    BucketGateHarness h("15", "D", "W", "M");
    h.set_syminfo_timezone(NY);
    h.set_syminfo_session(FX);
    // Lab pad epoch for a TV range starting 2025-04-01: 2025-03-31 00:00 UTC
    // = Sun 2025-03-30 20:00 EDT, inside the Sunday session / week / March.
    h.set_syminfo_metadata("security_range_start_na_warmup",
                           static_cast<double>(utc_ms(2025, 3, 31)));
    auto bars = make_forex_15m_feed();
    h.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(h.last_error().empty(), "forex flag-on run succeeds");

    CHECK(!h.completed[0].empty(), "D completed at least once");
    if (!h.completed[0].empty()) {
        CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 3, 31, 21, 0),
                    "D: first bar is the Mon 17:00 EDT session (Sunday remainder dropped)");
    }
    CHECK(!h.completed[1].empty(), "W completed at least once");
    if (!h.completed[1].empty()) {
        CHECK_EQ_MS(h.completed[1].front(), utc_ms(2025, 4, 6, 21, 0),
                    "W: first bar is the week opening Sun Apr 6 17:00 EDT");
        // Full 5-session weeks follow at 7-day spacing.
        if (h.completed[1].size() >= 2) {
            CHECK_EQ_MS(h.completed[1][1], utc_ms(2025, 4, 13, 21, 0),
                        "W: second bar opens Sun Apr 13 17:00 EDT");
        }
    }
    CHECK(!h.completed[2].empty(), "M completed at least once (April)");
    if (!h.completed[2].empty()) {
        CHECK_EQ_MS(h.completed[2].front(), utc_ms(2025, 3, 31, 21, 0),
                    "M: first bar is April (opens Mon Mar 31 17:00 EDT); March remainder dropped");
        CHECK(h.completed[2].size() == 1, "M: only April completes inside the feed");
    }
    std::printf("test_forex_flag_on_first_whole_bucket: %s\n",
                (failures > before) ? "FAIL" : "ok");
}

static void test_forex_flag_off_unchanged() {
    int before = failures;
    BucketGateHarness h("15", "D", "W", "M");
    h.set_syminfo_timezone(NY);
    h.set_syminfo_session(FX);
    auto bars = make_forex_15m_feed();
    h.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(h.last_error().empty(), "forex flag-off run succeeds");
    // Feed start = Sun Mar 30 17:00 EDT: every series begins there.
    for (int i = 0; i < 3; ++i) {
        CHECK(!h.completed[i].empty(), "flag-off: series completed");
        if (!h.completed[i].empty()) {
            CHECK_EQ_MS(h.completed[i].front(), utc_ms(2025, 3, 30, 21, 0),
                        "flag-off: first bucket opens at the feed start");
        }
    }
    CHECK(h.completed[2].size() == 2, "flag-off: March (partial) and April complete");
    std::printf("test_forex_flag_off_unchanged: %s\n",
                (failures > before) ? "FAIL" : "ok");
}

// 24x7 UTC hourly feed Mon 2025-03-24 00:00 .. Sun 2025-05-04 23:00 (long
// enough for April to complete on May 1).
static std::vector<Bar> make_utc_hourly_feed() {
    std::vector<Bar> bars;
    for (int64_t t = utc_ms(2025, 3, 24); t < utc_ms(2025, 5, 5); t += 3'600'000) {
        const double px = 100.0 + static_cast<double>(bars.size());
        bars.push_back(Bar{px, px, px, px, 1.0, t});
    }
    return bars;
}

static void test_utc_grid_aligned_range_start_is_timestamp_cut() {
    int before = failures;
    BucketGateHarness h("60", "240", "D", "W");
    // Mon 2025-03-31 00:00 UTC sits on every grid (4h, D, Monday).
    h.set_syminfo_metadata("security_range_start_na_warmup",
                           static_cast<double>(utc_ms(2025, 3, 31)));
    auto bars = make_utc_hourly_feed();
    h.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(h.last_error().empty(), "utc aligned run succeeds");
    for (int i = 0; i < 3; ++i) {
        CHECK(!h.completed[i].empty(), "utc aligned: series completed");
        if (!h.completed[i].empty()) {
            CHECK_EQ_MS(h.completed[i].front(), utc_ms(2025, 3, 31),
                        "utc aligned: first bucket opens exactly at the range start");
        }
    }
    std::printf("test_utc_grid_aligned_range_start_is_timestamp_cut: %s\n",
                (failures > before) ? "FAIL" : "ok");
}

static void test_utc_straddling_buckets_are_dropped() {
    int before = failures;
    BucketGateHarness h("60", "240", "W", "M");
    // Wed 2025-03-26 02:00 UTC: inside the 00:00-04:00 4h bucket, inside the
    // week that opened Mon Mar 24, inside March.
    h.set_syminfo_metadata("security_range_start_na_warmup",
                           static_cast<double>(utc_ms(2025, 3, 26, 2, 0)));
    auto bars = make_utc_hourly_feed();
    h.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(h.last_error().empty(), "utc straddle run succeeds");
    CHECK(!h.completed[0].empty(), "240 completed");
    if (!h.completed[0].empty()) {
        CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 3, 26, 4, 0),
                    "240: the straddling 00:00 bucket is dropped, first bar opens 04:00");
    }
    CHECK(!h.completed[1].empty(), "W completed");
    if (!h.completed[1].empty()) {
        CHECK_EQ_MS(h.completed[1].front(), utc_ms(2025, 3, 31),
                    "W: the straddling Mar-24 week is dropped, first bar is Mon Mar 31");
    }
    CHECK(!h.completed[2].empty(), "M completed");
    if (!h.completed[2].empty()) {
        CHECK_EQ_MS(h.completed[2].front(), utc_ms(2025, 4, 1),
                    "M: the March remainder is dropped, first bar is April");
    }
    std::printf("test_utc_straddling_buckets_are_dropped: %s\n",
                (failures > before) ? "FAIL" : "ok");
}

// Mirrors test_security_range_start_na_warmup's hour feed with the range
// start moved INSIDE hour 1: hour 1 is now dropped whole (its open precedes
// the range start), so the first completed HTF close is hour 2's.
static void test_intraday_mid_bucket_range_start_drops_whole_bucket() {
    int before = failures;
    BucketGateHarness h("15", "60", "60", "60");
    const double hour_close[8] = {999.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0};
    std::vector<Bar> bars;
    for (int hr = 0; hr < 8; ++hr) {
        for (int q = 0; q < 4; ++q) {
            const double c = hour_close[hr];
            bars.push_back(Bar{c, c, c, c, 1.0,
                               static_cast<int64_t>(hr) * 3'600'000
                               + static_cast<int64_t>(q) * 900'000});
        }
    }
    h.set_syminfo_metadata("security_range_start_na_warmup", 3'600'000.0 + 900'000.0);
    h.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(h.last_error().empty(), "mid-bucket run succeeds");
    CHECK(h.completed_close[0].size() == 6,
          "mid-bucket: hours 2..7 complete (hour 1 dropped whole, not kept partial)");
    if (!h.completed_close[0].empty()) {
        CHECK(h.completed_close[0].front() == 20.0,
              "mid-bucket: first completed HTF close is hour 2's");
    }
    std::printf("test_intraday_mid_bucket_range_start_drops_whole_bucket: %s\n",
                (failures > before) ? "FAIL" : "ok");
}

int main() {
    test_bucket_open_utc_grid();
    test_bucket_open_forex_session();
    test_forex_flag_on_first_whole_bucket();
    test_forex_flag_off_unchanged();
    test_utc_grid_aligned_range_start_is_timestamp_cut();
    test_utc_straddling_buckets_are_dropped();
    test_intraday_mid_bucket_range_start_drops_whole_bucket();
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_security_range_start_bucket_gating passed.\n");
    return 0;
}
