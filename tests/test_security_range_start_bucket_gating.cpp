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
// Since round 8 (family P) the bucket-open cut is also the engine's DEFAULT on
// split-feed runs, keyed on the run's first chart bar and on whether the
// auxiliary 1m feed traded between the bucket's nominal open and that bar
// (TradingView dates a bar by its first traded bar: an NSE week whose Monday
// was a holiday opens on Tuesday and is kept; lab tv famp-sense-*,
// 2026-09-05). Single-feed runs keep their feed-start series (flag-off case
// below, unchanged); the split-feed cases follow it.
//
// Since round 8 (family P) the same bucket-open cut is the engine's DEFAULT
// for every coarser-than-chart and chart-timeframe evaluator, keyed on the
// run's first chart bar (security_first_chart_bar_ms_): TradingView applies
// it on every lane, exchange or OTC, 15m or 1D (lab tv famp-sense-*,
// 2026-09-05). The flag keeps its explicit epoch and the EMA na-warmup.
//
//
// It FAILS without the fix: the timestamp cut keeps the straddling remainder
// as HTF bar 1, so every "first completed bucket" assertion below reports the
// pre-range bucket's open instead.

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>
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
class BucketGateHarness : public pineforge::source::PineStrategyHost {
public:
    std::vector<int64_t> completed[3];
    std::vector<double> completed_close[3];

    explicit BucketGateHarness(const char* input_tf,
                               const char* tf0, const char* tf1, const char* tf2,
                               bool lookahead = false) {
        register_security_eval(0, tf0, input_tf, lookahead, false);
        register_security_eval(1, tf1, input_tf, lookahead, false);
        register_security_eval(2, tf2, input_tf, lookahead, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (!is_complete || sec_id < 0 || sec_id > 2) return;
        completed[sec_id].push_back(bar.timestamp);
        completed_close[sec_id].push_back(bar.close);
    }
    void on_source_bar(const Bar&) override {}
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

// ─── Default cut on the split feed (round 8, family P) ───────────────────────

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#include <pineforge/pineforge.h>

// Three lookahead_off evaluators registered at run time (the split feed
// re-inits them on the auxiliary input tf); records every completed HTF bar's
// label per id.
class SplitGateHarness : public pineforge::source::PineStrategyHost {
public:
    std::vector<int64_t> completed[3];
    std::string tfs[3];
    SplitGateHarness(const char* tf0, const char* tf1, const char* tf2) {
        tfs[0] = tf0; tfs[1] = tf1; tfs[2] = tf2;
    }
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (int i = 0; i < 3; ++i) register_security_eval(i, tfs[i], input_tf_, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (!is_complete || sec_id < 0 || sec_id > 2) return;
        completed[sec_id].push_back(bar.timestamp);
    }
    void on_source_bar(const Bar&) override {}
};

// Session-shaped bars every `step_ms` from `begin` to `end`: `open_at` says
// whether a local instant trades (wday 0 = Sunday, hour/minute local).
template <typename OpenAt>
static std::vector<Bar> session_bars(int64_t begin, int64_t end, int64_t step_ms,
                                     int utc_offset_hours, OpenAt open_at) {
    std::vector<Bar> bars;
    for (int64_t t = begin; t < end; t += step_ms) {
        const int64_t local = t + static_cast<int64_t>(utc_offset_hours) * 3'600'000;
        const int64_t day = local / 86'400'000;
        const int wday = static_cast<int>((day + 4) % 7);
        const int minute_of_day = static_cast<int>((local % 86'400'000) / 60'000);
        if (!open_at(wday, minute_of_day)) continue;
        const double px = 100.0 + static_cast<double>(bars.size() % 50) * 0.25;
        bars.push_back(Bar{px, px, px, px, 1.0, t});
    }
    return bars;
}

// CME_MINI:ES1! (America/Chicago 1700-1600, CDT): Sun 17:00 .. Fri 16:00 with
// the 16:00-17:00 break.
static bool cme_open(int wday, int minute) {
    const int hour = minute / 60;
    if (hour == 16) return false;
    if (wday == 6) return false;
    if (wday == 5 && hour >= 16) return false;
    if (wday == 0 && hour < 17) return false;
    return true;
}
// NSE:NIFTY (Asia/Kolkata 0915-1530): Mon .. Fri 09:15-15:30.
static bool nse_open(int wday, int minute) {
    if (wday == 0 || wday == 6) return false;
    return minute >= 9 * 60 + 15 && minute < 15 * 60 + 30;
}
// OANDA:XAUUSD (America/New_York 1800-1700, EDT): Sun 18:00 .. Fri 17:00 with
// the 17:00-18:00 break.
static bool oanda_open(int wday, int minute) {
    const int hour = minute / 60;
    if (hour == 17) return false;
    if (wday == 6) return false;
    if (wday == 5 && hour >= 17) return false;
    if (wday == 0 && hour < 18) return false;
    return true;
}

static void run_split(SplitGateHarness& h, const std::vector<Bar>& chart,
                      const std::vector<Bar>& aux, const char* chart_tf,
                      const char* tz, const char* session, const char* type) {
    strategy_set_syminfo_timezone(static_cast<pf_strategy_t>(&h), tz);
    strategy_set_syminfo_session(static_cast<pf_strategy_t>(&h), session);
    strategy_set_syminfo_type(static_cast<pf_strategy_t>(&h), type);
    CHECK(strategy_set_aux_security_feed(static_cast<pf_strategy_t>(&h),
                                         reinterpret_cast<const pf_bar_t*>(aux.data()),
                                         static_cast<int>(aux.size()), "1") == 0,
          "auxiliary feed installed");
    h.run(chart.data(), static_cast<int>(chart.size()), chart_tf, chart_tf, false, 4,
          MagnifierDistribution::ENDPOINTS);
    CHECK(h.last_error().empty(), "split-feed run succeeds");
    if (!h.last_error().empty()) std::printf("   engine error: %s\n", h.last_error().c_str());
}

// ES-shaped 15m chart from the deep-backtest range start Tue 2025-04-01
// 00:00Z (19:00 CDT Monday, inside the 04-01 trade date that opened Mon 03-31
// 17:00 CDT) with the 1m feed from Sun 03-30 17:00 CDT. TradingView (lab tv
// famp-sense-es15full / nq15full): "60" keeps the 00:00Z hour, "240" starts
// at 02:00Z (the 22:00Z bucket of 03-31 is absent), "D" starts at the 04-02
// trade date (04-01 22:00Z; the first non-na highest(20)[1] is the 05-01
// 20:45Z bar), "W" at the week opening Sun 04-06 17:00 CDT (first non-na
// 08-29 20:45Z).
static void test_default_cut_cme_15m_range_start() {
    int before = failures;
    const auto chart = session_bars(utc_ms(2025, 4, 1, 0, 0), utc_ms(2025, 4, 18, 21, 0),
                                    900'000, -5, cme_open);
    const auto aux = session_bars(utc_ms(2025, 3, 30, 22, 0), utc_ms(2025, 4, 18, 21, 0),
                                  60'000, -5, cme_open);
    {
        SplitGateHarness h("60", "240", "D");
        run_split(h, chart, aux, "15", "America/Chicago", "1700-1600", "futures");
        CHECK(!h.completed[0].empty(), "60 completed");
        if (!h.completed[0].empty())
            CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 1, 0, 0),
                        "60: the 00:00Z hour opens on the first chart bar and is kept");
        CHECK(!h.completed[1].empty(), "240 completed");
        if (!h.completed[1].empty())
            CHECK_EQ_MS(h.completed[1].front(), utc_ms(2025, 4, 1, 2, 0),
                        "240: the 22:00Z bucket in progress at the range start is absent");
        CHECK(!h.completed[2].empty(), "D completed");
        if (!h.completed[2].empty()) {
            CHECK_EQ_MS(h.completed[2].front(), utc_ms(2025, 4, 1, 22, 0),
                        "D: the 04-01 trade date in progress at the range start is absent");
            if (h.completed[2].size() >= 2)
                CHECK_EQ_MS(h.completed[2][1], utc_ms(2025, 4, 2, 22, 0), "D: the 04-03 trade date follows");
        }
    }
    {
        SplitGateHarness h("W", "W", "W");
        run_split(h, chart, aux, "15", "America/Chicago", "1700-1600", "futures");
        CHECK(!h.completed[0].empty(), "W completed");
        if (!h.completed[0].empty())
            CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 6, 22, 0),
                        "W: the week that opened Sun 03-30 is absent; bar 0 opens Sun 04-06 17:00 CDT");
    }
    std::printf("test_default_cut_cme_15m_range_start: %s\n", (failures > before) ? "FAIL" : "ok");
}

// NSE:NIFTY 15m from Tue 2025-04-01 09:15 IST: Monday 03-31 was a holiday,
// so the week's first traded bar IS the range start and TradingView keeps it
// (famp-sense-nifty15full: "W" first non-na 08-22 09:45Z, the same bar as the
// engine before this cut). The same chart with a 1m feed that traded on
// Monday drops the week (TradingView's NYSE:F 15m: 08-29).
static void test_default_cut_nse_holiday_monday_keeps_the_week() {
    int before = failures;
    const auto chart = session_bars(utc_ms(2025, 4, 1, 3, 45), utc_ms(2025, 4, 25, 10, 0),
                                    900'000, 5, nse_open);
    {
        // The 1m feed also begins on Tuesday: nothing traded on the holiday.
        const auto aux = session_bars(utc_ms(2025, 4, 1, 3, 45), utc_ms(2025, 4, 25, 10, 0),
                                      60'000, 5, nse_open);
        SplitGateHarness h("W", "D", "60");
        run_split(h, chart, aux, "15", "Asia/Kolkata", "0915-1530", "index");
        CHECK(!h.completed[0].empty(), "W completed (holiday Monday)");
        if (!h.completed[0].empty())
            CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 1, 3, 45),
                        "W: the week opening on the holiday's Tuesday is the range start's own and is kept");
        CHECK(!h.completed[1].empty(), "D completed");
        if (!h.completed[1].empty())
            CHECK_EQ_MS(h.completed[1].front(), utc_ms(2025, 4, 1, 3, 45), "D: 04-01 is day 0");
    }
    {
        // Control: the 1m feed traded on Monday 03-31 -> the week was in
        // progress at the range start and is absent.
        auto monday_open = [](int wday, int minute) { return nse_open(wday, minute); };
        const auto aux = session_bars(utc_ms(2025, 3, 31, 3, 45), utc_ms(2025, 4, 25, 10, 0),
                                      60'000, 5, monday_open);
        SplitGateHarness h("W", "D", "60");
        run_split(h, chart, aux, "15", "Asia/Kolkata", "0915-1530", "index");
        CHECK(!h.completed[0].empty(), "W completed (traded Monday)");
        if (!h.completed[0].empty())
            CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 7, 3, 45),
                        "W: a week that traded before the range start is absent; bar 0 is Mon 04-07");
    }
    std::printf("test_default_cut_nse_holiday_monday_keeps_the_week: %s\n", (failures > before) ? "FAIL" : "ok");
}

// OANDA:XAUUSD 1D chart from the 04-01 21:00Z bar (the 04-02 session; the
// range start 04-01 00:00Z drops the 03-31-stamped bar from the chart itself)
// with the 1m feed from Sun 03-30 22:00Z: the week that opened Sun 03-30
// 17:00 EDT traded and is absent, "W" bar 0 opens Sun 04-06 (famp-sense-xau1d:
// first non-na 08-28 21:00Z), while "D" -- the chart's own timeframe -- keeps
// its first bar.
static void test_default_cut_oanda_1d_weekly() {
    int before = failures;
    std::vector<Bar> chart;
    for (int64_t t = utc_ms(2025, 4, 1, 21, 0); t < utc_ms(2025, 5, 3, 0, 0); t += 86'400'000) {
        const int64_t day = (t + 0) / 86'400'000;
        const int wday = static_cast<int>((day + 4) % 7);           // stamp's UTC weekday
        if (wday == 5 || wday == 6) continue;                      // no Fri/Sat-stamped sessions
        const double px = 3000.0 + static_cast<double>(chart.size());
        chart.push_back(Bar{px, px, px, px, 1.0, t});
    }
    const auto aux = session_bars(utc_ms(2025, 3, 30, 22, 0), utc_ms(2025, 5, 2, 21, 0),
                                  60'000, -4, oanda_open);
    SplitGateHarness h("W", "D", "240");
    run_split(h, chart, aux, "1D", "America/New_York", "1800-1700", "cfd");
    CHECK(!h.completed[0].empty(), "W completed on the 1D chart");
    if (!h.completed[0].empty())
        CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 6, 21, 0),
                    "W: the week that opened Sun 03-30 17:00 EDT is absent; bar 0 is the Sun 04-06 stamp");
    CHECK(!h.completed[1].empty(), "D completed on the 1D chart");
    if (!h.completed[1].empty())
        CHECK_EQ_MS(h.completed[1].front(), utc_ms(2025, 4, 1, 21, 0),
                    "D: the chart's own first bar is kept");
    std::printf("test_default_cut_oanda_1d_weekly: %s\n", (failures > before) ? "FAIL" : "ok");
}

// BINANCE:BTCUSDT 1D chart (UTC, 24x7) from the range start Tue 2025-04-01
// under the flag's epoch 04-01 00:00Z, the 1m feed from Sun 03-30: "W" and
// "M" on a daily chart are the chart dailies aggregated (the implicit native
// feed), whose in-progress week is labelled by its first in-range stamp
// (04-01) -- the flag's bucket-open cut kept it as weekly bar 0 (hungpixi-
// macd-enhanced-mtf, lab tv famp-sense-hp-btc1d: TradingView's weekly EMA26
// first reads on the 09-29 week, i.e. bar 0 is the 04-07 week). The week
// that traded before the epoch is absent; April (opening exactly at the
// epoch) and "D" (the chart's own timeframe) keep their first bar. The NSE
// control: a 1D chart whose Monday never traded keeps its Tuesday-opening
// week.
static void test_flag_epoch_1d_chart_weekly_from_chart_dailies() {
    int before = failures;
    auto always = [](int, int) { return true; };
    {
        std::vector<Bar> chart;
        for (int64_t t = utc_ms(2025, 4, 1); t < utc_ms(2025, 5, 10); t += 86'400'000) {
            const double px = 80000.0 + static_cast<double>(chart.size());
            chart.push_back(Bar{px, px, px, px, 1.0, t});
        }
        const auto aux = session_bars(utc_ms(2025, 3, 30), utc_ms(2025, 5, 10), 60'000, 0, always);
        SplitGateHarness h("W", "D", "M");
        h.set_syminfo_metadata("security_range_start_na_warmup",
                               static_cast<double>(utc_ms(2025, 4, 1)));
        run_split(h, chart, aux, "1D", "UTC", "24x7", "crypto");
        CHECK(!h.completed[0].empty(), "flag 1D: W completed");
        if (!h.completed[0].empty())
            CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 7),
                        "flag 1D: the week that opened Mon 03-31 (traded, before the epoch) is absent; W bar 0 is Mon 04-07");
        CHECK(!h.completed[1].empty(), "flag 1D: D completed");
        if (!h.completed[1].empty())
            CHECK_EQ_MS(h.completed[1].front(), utc_ms(2025, 4, 1),
                        "flag 1D: D is the chart's own series, its first bar kept");
        CHECK(!h.completed[2].empty(), "flag 1D: M completed");
        if (!h.completed[2].empty())
            CHECK_EQ_MS(h.completed[2].front(), utc_ms(2025, 4, 1),
                        "flag 1D: April opens exactly at the epoch and is kept");
    }
    {
        // NSE-shaped 1D chart from Tue 04-01 (Monday 03-31 a holiday): the
        // 1m feed never traded on Monday, the week is the range start's own.
        std::vector<Bar> chart;
        for (int64_t t = utc_ms(2025, 4, 1, 3, 45); t < utc_ms(2025, 5, 10); t += 86'400'000) {
            const int wday = static_cast<int>(((t + 5 * 3'600'000) / 86'400'000 + 4) % 7);
            if (wday == 0 || wday == 6) continue;
            const double px = 22000.0 + static_cast<double>(chart.size());
            chart.push_back(Bar{px, px, px, px, 1.0, t});
        }
        const auto aux = session_bars(utc_ms(2025, 4, 1, 3, 45), utc_ms(2025, 5, 10), 60'000, 5, nse_open);
        SplitGateHarness h("W", "D", "M");
        h.set_syminfo_metadata("security_range_start_na_warmup",
                               static_cast<double>(utc_ms(2025, 4, 1)));
        run_split(h, chart, aux, "1D", "Asia/Kolkata", "0915-1530", "index");
        CHECK(!h.completed[0].empty(), "flag 1D NSE: W completed");
        if (!h.completed[0].empty())
            CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 1, 3, 45),
                        "flag 1D NSE: the week opening on the holiday's Tuesday is kept");
    }
    std::printf("test_flag_epoch_1d_chart_weekly_from_chart_dailies: %s\n", (failures > before) ? "FAIL" : "ok");
}
#endif

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
// R19 covered TV pins on XAUUSD: starting at Apr1 00:00 UTC omits the
// in-progress daily bar. time/close are na on Apr1 and ATR14[1] first reads
// Apr23. This holds without an auxiliary feed and for both lookahead modes.
// Keep W/M's existing no-history-evidence behavior outside the daily repair.
static void test_single_feed_otc_daily_partial_bucket() {
    int before = failures;
    const auto full = make_forex_15m_feed();
    const int64_t start = utc_ms(2025, 4, 1);
    std::vector<Bar> bars;
    for (const Bar& bar : full) if (bar.timestamp >= start) bars.push_back(bar);
    for (const std::string& kind : {std::string("forex"), std::string("cfd")}) {
        for (bool lookahead : {false, true}) {
            BucketGateHarness h("15", "D", "W", "M", lookahead);
            h.set_syminfo_timezone(NY);
            h.set_syminfo_session(FX);
            h.set_syminfo_type(kind);
            h.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
            CHECK(h.last_error().empty(), "single-feed OTC partial-day run succeeds");
            CHECK(!h.completed[0].empty(), "single-feed daily series completes");
            if (!h.completed[0].empty()) {
                CHECK_EQ_MS(h.completed[0].front(), utc_ms(2025, 4, 1, 21),
                            "single-feed D starts at the first whole session");
            }
            for (int i=1; i<3; ++i) {
                CHECK(!h.completed[i].empty(), "single-feed W/M completes");
                if (!h.completed[i].empty()) CHECK_EQ_MS(h.completed[i].front(),
                    utc_ms(2025,3,31,21),
                    "single-feed W/M retains its existing feed-start series");
            }
        }
    }
    std::printf("test_single_feed_otc_daily_partial_bucket: %s\n",
                failures > before ? "FAIL" : "ok");
}

// OANDA's daily label is 17:00 ET but trading starts at 18:00. A feed
// beginning at that actual open contains the whole bar and must keep it.
static void test_single_feed_cfd_actual_open_is_not_partial() {
    int before = failures;
    const int64_t begin=utc_ms(2025,4,1,22), end=utc_ms(2025,4,4,21);
    std::vector<Bar> bars;
    for (int64_t t=begin;t<end;t+=900000) {
        const int64_t hour=(t/3600000)%24;
        if (hour==21) continue;  // the 17:00..18:00 EDT daily break
        bars.push_back({100,101,99,100,1,t});
    }
    BucketGateHarness h("15", "D", "W", "M");
    h.set_syminfo_timezone(NY);
    h.set_syminfo_session("1800-1700");
    h.set_syminfo_type("cfd");
    h.run(bars.data(),static_cast<int>(bars.size()),"15","15");
    CHECK(h.last_error().empty(), "single-feed cfd aligned run succeeds");
    CHECK(!h.completed[0].empty(), "single-feed cfd aligned D completes");
    if (!h.completed[0].empty()) CHECK_EQ_MS(h.completed[0].front(),
        utc_ms(2025,4,1,21), "keep whole cfd day whose stamp precedes trading");
    std::printf("test_single_feed_cfd_actual_open_is_not_partial: %s\n",
                failures > before ? "FAIL" : "ok");
}

// Equivalent to request.security("D", ta.atr(14)[1]) plus D close. The
// synthetic bars have true range 8; a deliberately extreme initial partial
// day must never enter either the ATR seed or the projected daily roster.
class OtcDailyAtrHarness : public pineforge::source::PineStrategyHost {
public:
    ta::ATR atr{14};
    std::vector<double> atr_history;
    struct Read { int64_t time; double close; double previous_atr; };
    std::vector<Read> reads;
    double visible_close=na<double>(), previous_atr=na<double>();
    int64_t first_projected_child=0;
    explicit OtcDailyAtrHarness(bool lookahead) {
        register_security_eval(0,"D","15",lookahead,false);
        set_syminfo_metadata("historical_security_lookahead_projection",1);
    }
    void evaluate_security(int,const Bar& bar,bool) override {
        if (security_series_slot_is_new(0)) {
            atr_history.push_back(atr.compute(bar.high,bar.low,bar.close));
        } else if (!atr_history.empty()) {
            atr_history.back()=atr.recompute(bar.high,bar.low,bar.close);
        }
        previous_atr=atr_history.size()>1
            ? atr_history[atr_history.size()-2] : na<double>();
        visible_close=bar.close;
    }
    void on_source_bar(const Bar& bar) override {
        if (first_projected_child==0
            && !security_eval_states_[0].historical_projections.empty()) {
            first_projected_child=
                security_eval_states_[0].historical_projections.front().first_child_ms;
        }
        reads.push_back({bar.timestamp,visible_close,previous_atr});
    }
};

static void test_single_feed_projection_and_previous_atr() {
    int before=failures;
    for (bool cfd : {false,true}) {
        const char* session=cfd ? "1800-1700" : "1700-1700";
        std::vector<Bar> bars={
            {999,1999,0,999,1,utc_ms(2025,4,1)},
            {999,1999,0,999,1,utc_ms(2025,4,1,6)},
            {999,1999,0,999,1,utc_ms(2025,4,1,20,45)}};
        const int trading_days[]={2,3,4,7,8,9,10,11,14,15,16,17,18,21,22,23,24,25,28,29,30};
        int index=0;
        for (int day : trading_days) {
            if (cfd && day==18) continue; // XAU Good Friday; EUR trades
            double base=100+index++;
            bars.push_back({base,base+4,base-4,base,1,
                            utc_ms(2025,4,day-1,cfd?22:21)});
            bars.push_back({base,base+4,base-4,base+1,1,utc_ms(2025,4,day,6)});
            bars.push_back({base,base+4,base-4,base+2,1,utc_ms(2025,4,day,20,45)});
        }
        for (bool lookahead : {false,true}) {
            OtcDailyAtrHarness h(lookahead);
            h.set_syminfo_timezone(NY);h.set_syminfo_session(session);
            h.set_syminfo_type(cfd?"cfd":"forex");
            h.run(bars.data(),static_cast<int>(bars.size()),"15","15");
            CHECK(h.last_error().empty(),"OTC previous ATR run succeeds");
            if (lookahead) CHECK_EQ_MS(h.first_projected_child,
                utc_ms(2025,4,1,cfd?22:21),"projection roster starts at first whole day");
            const int numeric_day=cfd ? (lookahead?23:24) : (lookahead?22:23);
            for (const auto& read : h.reads) {
                if (read.time==utc_ms(2025,4,1,6))
                    CHECK(is_na(read.close),"partial first daily close is absent");
                if (read.time==utc_ms(2025,4,2,6))
                    CHECK(lookahead ? read.close==102 : is_na(read.close),
                          "first daily close leaks on first child only with lookahead");
                if (read.time==utc_ms(2025,4,3,6) && !lookahead)
                    CHECK(read.close==102,"lookahead-off exposes first completed daily close");
                if (read.time==utc_ms(2025,4,numeric_day-1,6))
                    CHECK(is_na(read.previous_atr),"ATR14[1] remains absent one day before seed");
                if (read.time==utc_ms(2025,4,numeric_day,6))
                    CHECK(read.previous_atr==8,"ATR14[1] reads 8 after fourteen whole daily bars");
            }
        }
    }
    std::printf("test_single_feed_projection_and_previous_atr: %s\n",
                failures>before?"FAIL":"ok");
}
#endif

int main() {
    test_bucket_open_utc_grid();
    test_bucket_open_forex_session();
    test_forex_flag_on_first_whole_bucket();
    test_forex_flag_off_unchanged();
    test_utc_grid_aligned_range_start_is_timestamp_cut();
    test_utc_straddling_buckets_are_dropped();
    test_intraday_mid_bucket_range_start_drops_whole_bucket();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    test_default_cut_cme_15m_range_start();
    test_default_cut_nse_holiday_monday_keeps_the_week();
    test_default_cut_oanda_1d_weekly();
    test_flag_epoch_1d_chart_weekly_from_chart_dailies();
    test_single_feed_otc_daily_partial_bucket();
    test_single_feed_cfd_actual_open_is_not_partial();
    test_single_feed_projection_and_previous_atr();
#endif
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_security_range_start_bucket_gating passed.\n");
    return 0;
}
