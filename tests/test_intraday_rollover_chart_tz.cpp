// test_intraday_rollover_chart_tz.cpp — pin down the chart-timezone
// rollover semantics of ``BacktestEngine::_decompose_bar_time_chart_tz()``.
//
// This is the helper consumed by the three intraday-day rollover gates
// (``max_intraday_filled_orders`` in engine_fills.cpp, the loss-day
// counter in engine_orders.cpp, and ``max_intraday_loss`` in
// engine_risk.cpp). Pre-fix the gates rolled at UTC 00:00; post-fix
// they roll at chart 00:00 — which is what TradingView's broker
// emulator does, keyed off the chart's display TZ.
//
// Surfaced by the validation probe
// ``corpus/validation/97-tp-sl-gap-reversal-oca`` (UTC+8 chart): 234
// TV entries were missing in the engine because the cap reset at chart
// 08:00 instead of chart 00:00, so the engine locked itself out of
// chart-afternoon entries every day.
//
// What this fixture pins:
//
//   1. Empty / "UTC" / "Etc/UTC" chart TZ keeps the legacy UTC fast
//      path — same numbers as ``_decompose_bar_time()``.
//   2. With chart_tz="Asia/Taipei" (UTC+8), the decomposition returns
//      the chart-local wall clock (hour, day, month all shifted +8h).
//   3. The chart-day rollover happens at 16:00 UTC on the prior day
//      (== 00:00 the next chart day at UTC+8), NOT at 00:00 UTC.
//   4. The bare-name ``_bar_*()`` accessors are UNCHANGED — they
//      continue to return UTC even after ``set_chart_timezone`` is set,
//      preserving the contract pinned by ``test_chart_timezone.cpp``.
//
// The test does not drive a full ``run()`` against synthetic OHLCV;
// the rollover logic is single-helper-deep so a direct unit test on
// the helper is the high-signal/low-flake path here.

#include <cstdio>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

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

namespace {

// Same TimeProbeEngine pattern as test_chart_timezone.cpp — direct
// access to ``current_bar_`` plus an ``on_bar`` no-op so the abstract
// base can be instantiated. We additionally re-export the new
// chart-tz helper.
class TimeProbeEngine : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}
    void set_bar_timestamp(int64_t ts_ms) {
        current_bar_.timestamp = ts_ms;
    }
    using BacktestEngine::_bar_hour;
    using BacktestEngine::_bar_dayofmonth;
    using BacktestEngine::_bar_month;
    using BacktestEngine::_decompose_bar_time;
    using BacktestEngine::_decompose_bar_time_chart_tz;
};

// Unix ms timestamps used as fixtures. All keyed off 2025-03-31 to
// dodge any DST transition (Asia/Taipei has none, but anchoring the
// fixture on a stable date keeps the assertions readable):
//
//   kUtc_0330        = 2025-03-31 03:30:00 UTC (= 11:30 Taipei,  same UTC day)
//   kUtc_1530        = 2025-03-31 15:30:00 UTC (= 23:30 Taipei,  same UTC day, same Taipei day)
//   kUtc_1600        = 2025-03-31 16:00:00 UTC (= 00:00 Taipei NEXT day — rollover boundary)
//   kUtc_1700_prior  = 2025-03-30 17:00:00 UTC (= 01:00 Taipei the 31st — UTC day=30, Taipei day=31)
constexpr int64_t kUtc_0330       = 1743391800000LL;
constexpr int64_t kUtc_1530       = 1743435000000LL;
constexpr int64_t kUtc_1600       = 1743436800000LL;
constexpr int64_t kUtc_1700_prior = 1743354000000LL;

void test_default_no_chart_tz_matches_utc_helper() {
    // When the chart TZ is unset, the chart-tz helper is a no-op
    // pass-through to the cheap UTC ``_decompose_bar_time()``.
    std::printf("test_default_no_chart_tz_matches_utc_helper\n");
    TimeProbeEngine eng;
    eng.set_bar_timestamp(kUtc_0330);
    auto utc = eng._decompose_bar_time();
    auto ctz = eng._decompose_bar_time_chart_tz();
    CHECK(utc.year == ctz.year);
    CHECK(utc.month == ctz.month);
    CHECK(utc.dayofmonth == ctz.dayofmonth);
    CHECK(utc.hour == ctz.hour);
    CHECK(utc.minute == ctz.minute);
}

void test_explicit_utc_chart_tz_matches_utc_helper() {
    std::printf("test_explicit_utc_chart_tz_matches_utc_helper\n");
    for (const char* tz : {"UTC", "Etc/UTC"}) {
        TimeProbeEngine eng;
        eng.set_chart_timezone(tz);
        eng.set_bar_timestamp(kUtc_0330);
        auto utc = eng._decompose_bar_time();
        auto ctz = eng._decompose_bar_time_chart_tz();
        CHECK(utc.dayofmonth == ctz.dayofmonth);
        CHECK(utc.hour == ctz.hour);
    }
}

void test_chart_tz_shifts_hour_and_day_for_taipei() {
    // 2025-03-31 03:30 UTC == 2025-03-31 11:30 Taipei. Same calendar
    // day, hour shifted +8.
    std::printf("test_chart_tz_shifts_hour_and_day_for_taipei\n");
    TimeProbeEngine eng;
    eng.set_chart_timezone("Asia/Taipei");
    eng.set_bar_timestamp(kUtc_0330);
    auto bt = eng._decompose_bar_time_chart_tz();
    CHECK(bt.year == 2025);
    CHECK(bt.month == 3);
    CHECK(bt.dayofmonth == 31);
    CHECK(bt.hour == 11);
    CHECK(bt.minute == 30);
}

void test_chart_tz_rollover_at_16_utc_for_taipei() {
    // The core regression. Pre-fix the engine's intraday day-key
    // (``dayofmonth*100 + month``) flipped at 00:00 UTC; post-fix it
    // must flip at chart 00:00 — i.e. 16:00 UTC for a UTC+8 chart.
    //
    // 15:30 UTC (= 23:30 Taipei, day 31): still on chart day 31.
    // 16:00 UTC (= 00:00 Taipei, day 1):   rolled to chart day 1 (April).
    std::printf("test_chart_tz_rollover_at_16_utc_for_taipei\n");
    TimeProbeEngine eng;
    eng.set_chart_timezone("Asia/Taipei");

    eng.set_bar_timestamp(kUtc_1530);
    auto pre = eng._decompose_bar_time_chart_tz();
    int pre_key = pre.dayofmonth * 100 + pre.month;
    CHECK(pre.dayofmonth == 31);
    CHECK(pre.month == 3);
    CHECK(pre.hour == 23);
    CHECK(pre_key == 3103);  // dayofmonth=31, month=3

    eng.set_bar_timestamp(kUtc_1600);
    auto post = eng._decompose_bar_time_chart_tz();
    int post_key = post.dayofmonth * 100 + post.month;
    CHECK(post.dayofmonth == 1);
    CHECK(post.month == 4);
    CHECK(post.hour == 0);
    CHECK(post_key == 104);  // dayofmonth=1, month=4
    CHECK(pre_key != post_key);  // gate would reset intraday counters here
}

void test_utc_helper_does_NOT_roll_at_16_utc() {
    // Negative control: the UTC helper SHOULD roll at 00:00 UTC,
    // proving the chart-tz behaviour above is genuinely the new path
    // and not just the old UTC behaviour relabelled.
    //
    // 2025-03-31 15:30 UTC and 2025-03-31 16:00 UTC are the SAME UTC
    // day — UTC-keyed gates would NOT reset between them.
    std::printf("test_utc_helper_does_NOT_roll_at_16_utc\n");
    TimeProbeEngine eng;
    eng.set_bar_timestamp(kUtc_1530);
    auto pre = eng._decompose_bar_time();
    eng.set_bar_timestamp(kUtc_1600);
    auto post = eng._decompose_bar_time();
    CHECK(pre.dayofmonth == post.dayofmonth);
    CHECK(pre.month == post.month);
    CHECK(pre.dayofmonth * 100 + pre.month
        == post.dayofmonth * 100 + post.month);
}

void test_chart_tz_late_evening_utc_is_next_chart_day() {
    // 2025-03-30 17:00 UTC == 2025-03-31 01:00 Taipei. UTC says
    // day=30, chart says day=31. This is the symmetric corner-case to
    // the rollover test.
    std::printf("test_chart_tz_late_evening_utc_is_next_chart_day\n");
    TimeProbeEngine eng;
    eng.set_bar_timestamp(kUtc_1700_prior);

    auto utc = eng._decompose_bar_time();
    CHECK(utc.dayofmonth == 30);
    CHECK(utc.month == 3);
    CHECK(utc.hour == 17);

    eng.set_chart_timezone("Asia/Taipei");
    auto ctz = eng._decompose_bar_time_chart_tz();
    CHECK(ctz.dayofmonth == 31);
    CHECK(ctz.month == 3);
    CHECK(ctz.hour == 1);
}

void test_bare_var_form_unaffected_by_chart_tz() {
    // Regression guard. ``_bar_hour()`` / ``_bar_dayofmonth()`` /
    // ``_bar_month()`` route through ``_decompose_bar_time()`` (UTC),
    // NOT the new chart-tz helper. They MUST continue to return the
    // exchange-TZ wall clock so the dozens of ``hour == N`` stop-cross
    // probes in corpus/validation/ don't silently shift by the chart
    // offset. This pairs with test_chart_timezone.cpp's
    // ``test_variable_form_stays_utc_after_chart_tz_set``.
    std::printf("test_bare_var_form_unaffected_by_chart_tz\n");
    TimeProbeEngine eng;
    eng.set_chart_timezone("Asia/Taipei");
    eng.set_bar_timestamp(kUtc_0330);
    CHECK(eng._bar_hour() == 3);            // UTC hour, NOT 11
    CHECK(eng._bar_dayofmonth() == 31);     // same UTC date, coincidence
    CHECK(eng._bar_month() == 3);

    // The 16:00 UTC fixture exercises the bit where chart-day and
    // UTC-day disagree — the bare accessor must follow UTC.
    eng.set_bar_timestamp(kUtc_1600);
    CHECK(eng._bar_hour() == 16);           // UTC hour, NOT 0
    CHECK(eng._bar_dayofmonth() == 31);     // UTC day, NOT 1
    CHECK(eng._bar_month() == 3);           // UTC month, NOT 4
}

}  // namespace

int main() {
    test_default_no_chart_tz_matches_utc_helper();
    test_explicit_utc_chart_tz_matches_utc_helper();
    test_chart_tz_shifts_hour_and_day_for_taipei();
    test_chart_tz_rollover_at_16_utc_for_taipei();
    test_utc_helper_does_NOT_roll_at_16_utc();
    test_chart_tz_late_evening_utc_is_next_chart_day();
    test_bare_var_form_unaffected_by_chart_tz();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
