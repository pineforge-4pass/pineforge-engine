// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// Native higher-timeframe request.security feeds (strategy_set_native_security_feed).
//
// TradingView's "D" request on an intraday chart of CME futures and US/Indian
// equities returns the exchange's own daily bar (settlement / official close),
// which no aggregation of the intraday feed reproduces. These pin that a
// completed daily bucket takes the native bar's OHLCV while the aggregator
// keeps deciding when it completes, that other timeframes, the chart and the
// broker are untouched, that a missing native bar keeps the aggregate, and
// that the split (1m auxiliary) feed path substitutes the same way.

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "native_fixed_period_fixture.hpp"

using namespace pineforge;

#ifndef PINEFORGE_HAS_NATIVE_SECURITY_FEED_V1
#error "native security feed test requires the V1 feature probe"
#endif

namespace {

constexpr int64_t kMinute = 60000;
constexpr int64_t kQuarter = 15 * kMinute;
constexpr int64_t kDay = 86400000;

class DailyProbe final : public pineforge::source::PineStrategyHost {
public:
    std::vector<double> chart_closes;
    std::vector<double> daily_closes;        // sec 0: "D", completed buckets
    std::vector<double> daily_opens;
    std::vector<double> daily_volumes;
    std::vector<double> hourly_closes;       // sec 1: "60", completed buckets
    std::vector<double> daily_at_chart_close;
    double latest_daily = na<double>();

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "D", input_tf_, false, false);
        register_security_eval(1, "60", input_tf_, false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        if (!is_complete) return;
        if (sec_id == 0) {
            latest_daily = bar.close;
            daily_closes.push_back(bar.close);
            daily_opens.push_back(bar.open);
            daily_volumes.push_back(bar.volume);
        } else if (sec_id == 1) {
            hourly_closes.push_back(bar.close);
        }
    }

    void on_source_bar(const Bar& bar) override {
        chart_closes.push_back(bar.close);
        daily_at_chart_close.push_back(latest_daily);
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_close_all();
    }
};

bool near(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

// One synthetic session (0930-1600 America/New_York) of 15m bars: 26 bars
// from 09:30 to 15:45, closes 100*(day+1) + k so the last 15m close of day d
// is 100*(d+1) + 25.
std::vector<Bar> ny_session(int day, int64_t open_ms) {
    std::vector<Bar> bars;
    for (int k = 0; k < 26; ++k) {
        const double base = 100.0 * (day + 1) + k;
        bars.push_back({base - 0.5, base + 1.0, base - 1.0, base, 10.0,
                        open_ms + k * kQuarter});
    }
    return bars;
}

// Invented winter weekday: eleven epoch days plus 14:30 UTC (09:30 local).
constexpr int64_t kNyDay1 = 11 * kDay + (14 * 60 + 30) * kMinute;
constexpr int64_t kNyDay2 = kNyDay1 + kDay;

void test_completed_daily_bucket_carries_the_native_bar() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    // Invented native daily bars: the official close differs from the last
    // 15m close (125 / 225), and so do open/volume.
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
        {199.0, 230.0, 190.0, 222.5, 6000.0, kNyDay2},
    };

    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 2) == 0);
    assert(probe.native_security_feed_enabled());

    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());

    // The daily bucket completes on the session's last 15m bar (the
    // aggregator's timing) and carries the native OHLCV, not the aggregate.
    assert((probe.daily_closes == std::vector<double>{111.5, 222.5}));
    assert((probe.daily_opens == std::vector<double>{99.0, 199.0}));
    assert((probe.daily_volumes == std::vector<double>{5000.0, 6000.0}));
    assert(probe.native_security_substitutions() == 2);
    assert(probe.native_security_misses() == 0);
    // Exposed at the day's last chart bar, na before the first completion.
    assert(probe.daily_at_chart_close.size() == 52);
    assert(std::isnan(probe.daily_at_chart_close[24]));
    assert(near(probe.daily_at_chart_close[25], 111.5));
    assert(near(probe.daily_at_chart_close[26], 111.5));
    assert(near(probe.daily_at_chart_close[51], 222.5));
    // The intraday "60" request is aggregated exactly as before: 09:30-10:29
    // closes on the 10:15 bar (k = 3).
    assert(!probe.hourly_closes.empty());
    assert(near(probe.hourly_closes[0], 103.0));
    // Chart and broker never see the native feed.
    assert(probe.chart_closes.size() == 52);
    assert(near(probe.chart_closes[25], 125.0));
    assert(probe.trade_count() == 1);
    assert(near(probe.get_trade(0).entry_price, chart[1].open));
    assert(near(probe.get_trade(0).exit_price, chart[2].open));
}


void test_a_bucket_without_a_native_bar_keeps_its_aggregate() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
    };

    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "1D",
        reinterpret_cast<const pf_bar_t*>(daily), 1) == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.daily_closes == std::vector<double>{111.5, 225.0}));
    assert(probe.native_security_substitutions() == 1);
    assert(probe.native_security_misses() == 1);
}


void test_without_a_native_feed_the_run_is_the_aggregate() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
        {199.0, 230.0, 190.0, 222.5, 6000.0, kNyDay2},
    };
    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 2) == 0);
    // n == 0 clears exactly that timeframe's feed.
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D", nullptr, 0) == 0);
    assert(!probe.native_security_feed_enabled());
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.daily_closes == std::vector<double>{125.0, 225.0}));
    assert(probe.native_security_substitutions() == 0);
    assert(probe.native_security_misses() == 0);
}


void test_feed_validation_fails_closed() {
    const Bar unordered[] = {
        {1.0, 1.0, 1.0, 1.0, 1.0, kNyDay2},
        {1.0, 1.0, 1.0, 1.0, 1.0, kNyDay1},
    };
    DailyProbe probe;
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(unordered), 2) == -1);
    assert(!probe.last_error().empty());
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "",
        reinterpret_cast<const pf_bar_t*>(unordered), 1) == -1);
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "bogus",
        reinterpret_cast<const pf_bar_t*>(unordered), 1) == -1);
    assert(strategy_set_native_security_feed(
        nullptr, "D", reinterpret_cast<const pf_bar_t*>(unordered), 1) == -1);
    assert(!probe.native_security_feed_enabled());

    // Historical runs only: a stream refuses to start over a native feed.
    const Bar daily[] = {{1.0, 1.0, 1.0, 1.0, 1.0, kNyDay1}};
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 1) == 0);
    std::vector<Bar> warmup = ny_session(0, kNyDay1);
    assert(!probe.stream_begin(warmup.data(), static_cast<int>(warmup.size()),
                               "15", "15"));
    assert(probe.last_error().find("native request.security feed")
           != std::string::npos);
}


// The campaign's finer-tf retry: native 15m chart + 1m auxiliary feed. The
// "D" evaluator is then fed from the auxiliary slice, and its completed
// bucket must take the native daily bar exactly as on the plain path.
void test_split_aux_feed_path_substitutes_the_same_native_bar() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    std::vector<Bar> aux;
    for (const Bar& bar : chart) {
        for (int m = 0; m < 15; ++m) {
            const double v = bar.close - 1.0 + m / 15.0;
            aux.push_back({v, v, v, v, 1.0, bar.timestamp + m * kMinute});
        }
    }
    const Bar daily[] = {
        {99.0, 130.0, 90.0, 111.5, 5000.0, kNyDay1},
        {199.0, 230.0, 190.0, 222.5, 6000.0, kNyDay2},
    };

    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux.data()),
        static_cast<int>(aux.size()), "1") == 0);
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 2) == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.daily_closes == std::vector<double>{111.5, 222.5}));
    assert(probe.native_security_substitutions() == 2);
    assert(probe.native_security_misses() == 0);
    assert(near(probe.chart_closes[25], 125.0));
}


// The CME shape: a 1700-1600 America/Chicago session whose daily bar is
// stamped at the 17:00 open and closes at 16:00 the next calendar day, with
// TradingView's close the 15:00 settlement rather than the 15:45 bar's close.
void test_overnight_cme_session_labels_by_session_day() {
    // 14:30 UTC + 8h30 = 23:00 UTC, the winter 17:00 Chicago open.
    constexpr int64_t open1 = kNyDay1 + (8 * 60 + 30) * kMinute;
    constexpr int64_t open2 = open1 + kDay;
    constexpr int64_t open3 = open2 + kDay;
    std::vector<Bar> chart;
    for (int day = 0; day < 2; ++day) {
        for (int k = 0; k < 92; ++k) {  // 17:00 .. 15:45 next day
            const double base = 5000.0 + 100.0 * day + k;
            chart.push_back({base - 0.25, base + 0.5, base - 0.5, base, 10.0,
                             (day == 0 ? open1 : open2) + k * kQuarter});
        }
    }
    // The third session's first bar: an overnight session's daily bucket is
    // finalized by the next session's first chart bar.
    chart.push_back({5200.0, 5200.5, 5199.5, 5200.0, 10.0, open3});
    const Bar daily[] = {
        {4999.0, 5100.0, 4990.0, 5077.25, 1.0, open1},
        {5099.0, 5200.0, 5090.0, 5177.25, 1.0, open2},
        {5199.0, 5300.0, 5190.0, 5277.25, 1.0, open3},
    };
    DailyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/Chicago");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "1700-1600");
    assert(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily), 3) == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    // Both completed session-days carry the settlement print of the native
    // bar stamped at their 17:00 open, matched by session-day label.
    assert((probe.daily_closes == std::vector<double>{5077.25, 5177.25}));
    assert(probe.native_security_substitutions() == 2);
    assert(probe.native_security_misses() == 0);
    // Completion timing stays the aggregator's: the session-day completes on
    // its last chart bar (15:45 CT, index 91 -- the bar closing at the 16:00
    // session close, which the run knows because the next input bar opens the
    // next session; TimeframeAggregator::feed(bar, next_input_ms)), and the
    // value is held on the next session's first bar (index 92).
    assert(probe.daily_at_chart_close.size() == 185);
    assert(std::isnan(probe.daily_at_chart_close[90]));
    assert(near(probe.daily_at_chart_close[91], 5077.25));
    assert(near(probe.daily_at_chart_close[92], 5077.25));
    assert(near(probe.daily_at_chart_close[183], 5177.25));
    assert(near(probe.daily_at_chart_close[184], 5177.25));
}

}  // namespace

// ── the tuple form: [o, h, l, c, v] = request.security(tickerid, tf, [open,
// high, low, close, volume]) reads the same native row ────────────────────
//
// Synthetic tuple fixture pinned against TradingView behaviour (recorded
// tapes are kept outside this repository). An input-bound "D"/"1D" request
// assigns every tuple member from the completed daily bucket. Native rows
// must replace that entire bucket without rounding fractional prices; with
// no native feed, all five members must remain the intraday aggregate.
namespace {

// Two full invented daytime sessions, aligned to the same winter 09:30
// session open as the scalar tests. Keeping all 26 quarter-hour slots makes
// completion occur at indices 25 and 51, with the first row held in between.
std::vector<Bar> synthetic_tuple_sessions() {
    std::vector<Bar> chart;
    for (int day = 0; day < 2; ++day) {
        for (int k = 0; k < 26; ++k) {
            const double c = 640.0 + 32.0 * day + 0.5 * (k % 7) + 0.125;
            chart.push_back({c - 0.25, c + 0.75, c - 0.75, c,
                             20.0 + 2.0 * k,
                             kNyDay1 + day * kDay + k * kQuarter});
        }
    }
    return chart;
}

// Invented native rows. Day 1 shares aggregate O/H/L but replaces close and
// volume; day 2 differs in every member. Its high has sub-cent precision.
const Bar kTupleDaily1{639.875, 643.875, 639.375, 642.12, 4096.0, kNyDay1};
const Bar kTupleDaily2{672.125, 676.0625, 670.875, 674.25, 8192.0, kNyDay2};

// The generated strategy's shape for `[o, h, l, c, v] = request.security(
// syminfo.tickerid, tf_input, [open, high, low, close, volume])`: one
// evaluator assigning the five members from the bucket it is handed, the
// timeframe string an input value ("D" is the default; "1D" is its alias).
class TupleProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit TupleProbe(std::string tf) : tf_(std::move(tf)) {}
    struct Row { double o, h, l, c, v; };
    double o_ = na<double>(), h_ = na<double>(), l_ = na<double>(),
           c_ = na<double>(), v_ = na<double>();
    std::vector<Row> at_chart_close;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, tf_, input_tf_, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool) override {
        if (sec_id != 0) return;
        o_ = bar.open; h_ = bar.high; l_ = bar.low; c_ = bar.close; v_ = bar.volume;
    }
    void on_source_bar(const Bar&) override {
        at_chart_close.push_back({o_, h_, l_, c_, v_});
    }

private:
    std::string tf_;
};

bool row_is(const TupleProbe::Row& r, const Bar& b) {
    return r.o == b.open && r.h == b.high && r.l == b.low && r.c == b.close
        && r.v == b.volume;
}

}  // namespace

void test_tuple_request_reads_the_native_daily_row() {
    for (const char* tf : {"D", "1D"}) {
        std::vector<Bar> chart = synthetic_tuple_sessions();
        const Bar daily[] = {kTupleDaily1, kTupleDaily2};
        TupleProbe probe(tf);
        strategy_set_syminfo_timezone(
            static_cast<pf_strategy_t>(&probe), "America/New_York");
        strategy_set_syminfo_session(
            static_cast<pf_strategy_t>(&probe), "0930-1600");
        assert(strategy_set_native_security_feed(
            static_cast<pf_strategy_t>(&probe), "D",
            reinterpret_cast<const pf_bar_t*>(daily), 2) == 0);
        probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
                  false, 4, MagnifierDistribution::ENDPOINTS);
        assert(probe.last_error().empty());
        assert(probe.at_chart_close.size() == 52);
        // na until the first daily completion (session bar 25).
        assert(std::isnan(probe.at_chart_close[24].c));
        // Every tuple member must equal the supplied native row, verbatim.
        // Day 1 close 642.12 differs from aggregate 642.125 by half a cent;
        // day 2 high 676.0625 must retain its sub-cent fraction. The native
        // day 2 O/H/L/C/V all differ from its intraday aggregate.
        assert(row_is(probe.at_chart_close[25], kTupleDaily1));
        assert(row_is(probe.at_chart_close[26], kTupleDaily1));   // held
        assert(row_is(probe.at_chart_close[50], kTupleDaily1));
        assert(row_is(probe.at_chart_close[51], kTupleDaily2));
        // Direct native constants also catch tuple-field swaps or rounding.
        assert(probe.at_chart_close[51].h == 676.0625);
        assert(probe.at_chart_close[51].c == 674.25);
        assert(probe.native_security_substitutions() == 2);
        assert(probe.native_security_misses() == 0);
    }
}

void test_tuple_request_without_the_feed_is_the_aggregate() {
    // With no native feed, read the formula-generated intraday OHLCV.
    // For day d: first open = 640 + 32*d + 0.125 - 0.25;
    // high = 640 + 32*d + 6*0.5 + 0.125 + 0.75;
    // low = 640 + 32*d + 0.125 - 0.75;
    // final close = 640 + 32*d + (25 % 7)*0.5 + 0.125.
    std::vector<Bar> chart = synthetic_tuple_sessions();
    TupleProbe probe("D");
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600");
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert(probe.at_chart_close.size() == 52);
    const TupleProbe::Row& d1 = probe.at_chart_close[25];
    assert(d1.o == 639.875 && d1.h == 643.875 && d1.l == 639.375 && d1.c == 642.125);
    const TupleProbe::Row& d2 = probe.at_chart_close[51];
    assert(d2.o == 671.875 && d2.h == 675.875 && d2.l == 671.375 && d2.c == 674.125);
    // Sum(k=0..25, 20 + 2*k) = 26*20 + 2*(25*26/2) = 1170.
    assert(d2.v == 1170.0);
    assert(probe.native_security_substitutions() == 0);
}

namespace {
int fixed_failures = 0;
#define FIXED_CHECK(condition) do { if (!(condition)) { \
    ++fixed_failures; std::fprintf(stderr, "fixed-period line %d: %s\n", \
                                 __LINE__, #condition); } } while (false)

class FixedPeriodProbe final : public pineforge::source::PineStrategyHost {
public:
    struct Read {
        Bar bar{};
        std::int64_t requested_close = 0;
        bool present = false;
    };
    std::string requested = "75";
    bool ahead = false;
    bool gaps = false;
    Read latest;
    std::vector<Read> reads;
    std::vector<Bar> charts;
    std::vector<std::int64_t> chart_ends;
    std::vector<source::PineRequestDelivery> deliveries;
    std::vector<Bar> evaluated_bars;
    int evaluated = 0;
    int clears = 0;
    std::vector<int> clears_at_chart;
    void configure_security_evaluators() override {
        latest = {}; reads.clear(); charts.clear(); chart_ends.clear();
        deliveries.clear(); evaluated_bars.clear();
        evaluated = 0; clears = 0; clears_at_chart.clear();
        security_eval_states_.clear();
        register_security_eval(0, requested, input_tf_, ahead, gaps);
    }
    void evaluate_security(int, const Bar& bar, bool) override {
        ++evaluated;
        evaluated_bars.push_back(bar);
        latest = {bar, session_intraday_bucket_end_ms(bar.timestamp,
            internal::tf_to_clock_seconds(requested), syminfo_.timezone, syminfo_.session), true};
        deliveries.push_back(current_request_delivery(0));
    }
    void clear_security(int) override { latest = {}; ++clears; }
    void on_source_bar(const Bar& bar) override {
        reads.push_back(latest); charts.push_back(bar);
        chart_ends.push_back(time_close());
        clears_at_chart.push_back(clears);
        // The original reporter's quantity expression and decision cadence.
        if (bar_index_ > 6 && bar_index_ % 7 == 3 && latest.present)
            strategy_entry("L", true, na<double>(), na<double>(),
                           1 + static_cast<int>(latest.bar.close) % 5);
        if (bar_index_ > 6 && bar_index_ % 7 == 5) strategy_close_all();
    }
};

void install_fixed_native(FixedPeriodProbe& host, const std::vector<Bar>& rows) {
    FIXED_CHECK(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&host), host.requested.c_str(),
        reinterpret_cast<const pf_bar_t*>(rows.data()),
        static_cast<int>(rows.size())) == 0);
}

void check_fixed_source_tape(int chart_minutes, int input_minutes,
                             int auxiliary_minutes, bool ahead, bool gaps) {
    using namespace tv_fixed_period_75_fact;
    auto bars = native_fixed_period_fixture::input(input_minutes);
    const auto rows = native_fixed_period_fixture::native();
    FixedPeriodProbe host;
    host.ahead = ahead; host.gaps = gaps;
    install_fixed_native(host, rows);
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (auxiliary_minutes != 0) {
        const auto aux = native_fixed_period_fixture::input(auxiliary_minutes);
        FIXED_CHECK(strategy_set_aux_security_feed(
            static_cast<pf_strategy_t>(&host),
            reinterpret_cast<const pf_bar_t*>(aux.data()),
            static_cast<int>(aux.size()), std::to_string(auxiliary_minutes).c_str()) == 0);
    }
#else
#error "fixed-period source replay requires the actual auxiliary-feed path"
#endif
    host.run(bars.data(), static_cast<int>(bars.size()),
             std::to_string(input_minutes), std::to_string(chart_minutes),
             false, 4, MagnifierDistribution::ENDPOINTS);
    FIXED_CHECK(host.last_error().empty());
    const int count = chart_minutes == 30 ? kChart30Bars : kChart60Bars;
    FIXED_CHECK(host.reads.size() == static_cast<std::size_t>(count));
    // Every value, NA and timestamp below cites decoded.json:
    // Retained TradingView capture: decoded.json
    // SHA256 9dd2b5b17e428532d1280c0151330d715949610304a74e849bbefd22f86ba12c
    // A.bars[i].changed/control; B.bars[i].changed/control.
    const int* expected = chart_minutes == 30
        ? (gaps ? kChart30GapsOn : kChart30GapsOff)
        : (ahead ? kChart60LookaheadOn : kChart60LookaheadOff);
    for (int i = 0; i < count && i < static_cast<int>(host.reads.size()); ++i) {
        const auto& got = host.reads[static_cast<std::size_t>(i)];
        FIXED_CHECK(host.charts[i].timestamp == kOriginMs + i * chart_minutes * kMinuteMs);
        FIXED_CHECK(host.chart_ends[i] == kOriginMs + (i + 1) * chart_minutes * kMinuteMs);
        FIXED_CHECK(got.present == (expected[i] >= 0));
        if (!got.present || expected[i] < 0) continue;
        const auto& want = kBars[expected[i]];
        FIXED_CHECK(got.bar.close == want.value);
        FIXED_CHECK(got.bar.timestamp == kOriginMs + want.open_minute * kMinuteMs);
        FIXED_CHECK(got.requested_close == kOriginMs + want.close_minute * kMinuteMs);
        // Native payload, independent synthetic O/H/L/V sentinels.
        const auto& payload = rows[static_cast<std::size_t>(expected[i])];
        FIXED_CHECK(got.bar.open == payload.open && got.bar.high == payload.high);
        FIXED_CHECK(got.bar.low == payload.low && got.bar.volume == payload.volume);
        if (!ahead) FIXED_CHECK(got.requested_close <= host.chart_ends[i]);
    }
    FIXED_CHECK(host.evaluated > 0);
    FIXED_CHECK(host.native_security_substitutions() > 0);
    if (ahead) {
        // Native-context invariant: a short requested bar with no chart-open
        // contributor still advances expression history before the next
        // selected bar. The body must retain B's publication mapping above.
        // The payloads are supplied input; no uncaptured TV TA is asserted.
        FIXED_CHECK(host.evaluated_bars.size() == rows.size() - 1);
        for (std::size_t i = 0; i < host.evaluated_bars.size() && i + 1 < rows.size(); ++i) {
            FIXED_CHECK(host.evaluated_bars[i].timestamp == rows[i].timestamp);
            FIXED_CHECK(host.evaluated_bars[i].close == rows[i].close);
        }
    }
    if (gaps) {
        for (std::size_t i = 0; i < host.clears_at_chart.size(); ++i)
            FIXED_CHECK(host.clears_at_chart[i] == static_cast<int>(i + 1));
    }
    for (const auto& delivery : host.deliveries) {
        FIXED_CHECK(delivery.caller_valid);
        FIXED_CHECK(delivery.actual_complete && !delivery.replay);
        FIXED_CHECK(delivery.requested_slot >= 0);
    }
    if (chart_minutes == 60 && !ahead) {
        int trade = 0;
        for (int i = 7; i + 3 < count; ++i) {
            if (i % 7 != 3) continue;
            FIXED_CHECK(trade < host.trade_count());
            if (trade >= host.trade_count()) break;
            // Reporter quantity, derived solely from B's retained value.
            FIXED_CHECK(host.get_trade(trade).qty ==
                        1 + static_cast<int>(kBars[expected[i]].value) % 5);
            ++trade;
        }
        FIXED_CHECK(trade > 0);
    }
}

void test_native_fixed_period_retained_source_replays() {
    // The same observable chart under chart input, fine primary, and auxiliary
    // routes. 1m/5m divide 75: selecting only evaluator remainders misses them.
    for (int chart : {30, 60}) {
        for (int fine : {chart, 10, 5, 1}) {
            for (bool mode : {false, true}) {
                check_fixed_source_tape(chart, fine, 0,
                                        chart == 60 && mode, chart == 30 && mode);
            }
        }
        for (int aux : {10, 5, 1}) {
            for (bool mode : {false, true}) {
                check_fixed_source_tape(chart, chart, aux,
                                        chart == 60 && mode, chart == 30 && mode);
            }
        }
    }
}

void test_native_fixed_period_sparse_chart_attribution() {
    using namespace tv_fixed_period_75_fact;
    // Native-control invariant only: no uncaptured TV values/NA are invented.
    // Remove 00:50 so chart [00:30,01:00) seals on the 01:00 input. Its
    // 01:10 close must not publish the 65m requested close to that old chart.
    auto dense = native_fixed_period_fixture::input(10, 240);
    auto sparse = dense;
    sparse.erase(sparse.begin() + 5);
    std::vector<Bar> native;
    for (int m = 0; m < 240; m += 65) {
        const double v = 900.5 + m;
        native.push_back({v - 1, v + 2, v - 2, v, 40.0, kOriginMs + m * kMinuteMs});
    }
    for (bool gaps : {false, true}) {
        FixedPeriodProbe full, holed;
        full.requested = holed.requested = "65";
        full.gaps = holed.gaps = gaps;
        install_fixed_native(full, native); install_fixed_native(holed, native);
        full.run(dense.data(), static_cast<int>(dense.size()), "10", "30", false,
                 4, MagnifierDistribution::ENDPOINTS);
        holed.run(sparse.data(), static_cast<int>(sparse.size()), "10", "30", false,
                  4, MagnifierDistribution::ENDPOINTS);
        FIXED_CHECK(full.last_error().empty() && holed.last_error().empty());
        FIXED_CHECK(full.reads.size() == holed.reads.size());
        for (std::size_t i = 0; i < holed.reads.size(); ++i) {
            FIXED_CHECK(!holed.reads[i].present ||
                        holed.reads[i].requested_close <= holed.chart_ends[i]);
            if (i >= full.reads.size()) continue;
            FIXED_CHECK(full.charts[i].timestamp == holed.charts[i].timestamp);
            FIXED_CHECK(full.chart_ends[i] == holed.chart_ends[i]);
            FIXED_CHECK(full.reads[i].present == holed.reads[i].present);
            if (full.reads[i].present && holed.reads[i].present)
                FIXED_CHECK(full.reads[i].bar.close == holed.reads[i].bar.close);
        }
    }
}

void test_native_fixed_period_tick_admission_and_reset() {
    auto bars = native_fixed_period_fixture::input(60, 360);
    const auto rows = native_fixed_period_fixture::native();
    FixedPeriodProbe refused;
    install_fixed_native(refused, rows);
    // Existing admission contract. This setup is NOT a behavior RED.
    FIXED_CHECK(!refused.stream_begin(bars.data(), static_cast<int>(bars.size()), "60", "60"));
    FIXED_CHECK(refused.last_error().find("native request.security feed") != std::string::npos);
    FixedPeriodProbe live;
    FIXED_CHECK(live.stream_begin(bars.data(), static_cast<int>(bars.size()), "60", "60"));
    const TradeTick tick{bars.back().timestamp + 60 * kMinute + 1, 1, 222.5, 1.0};
    FIXED_CHECK(live.stream_push_tick(tick)); // reaches the real on_native_tick
    FIXED_CHECK(live.last_error().empty());
    FIXED_CHECK(live.stream_push_tick({tick.timestamp + 60 * kMinute, 2, 223.5, 2.0}));
    FIXED_CHECK(live.last_error().empty());
    FIXED_CHECK(live.stream_end(false));

    FixedPeriodProbe rerun, fresh;
    install_fixed_native(rerun, rows); install_fixed_native(fresh, rows);
    rerun.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false,
              4, MagnifierDistribution::ENDPOINTS);
    const auto first_hash = rerun.broker_state_hash();
    const auto first_reads = rerun.reads;
    rerun.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false,
              4, MagnifierDistribution::ENDPOINTS);
    fresh.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false,
              4, MagnifierDistribution::ENDPOINTS);
    FIXED_CHECK(rerun.last_error().empty() && fresh.last_error().empty());
    FIXED_CHECK(rerun.broker_state_hash() == first_hash);
    FIXED_CHECK(rerun.broker_state_hash() == fresh.broker_state_hash());
    FIXED_CHECK(rerun.reads.size() == first_reads.size());
    for (std::size_t i = 0; i < rerun.reads.size() && i < first_reads.size(); ++i) {
        FIXED_CHECK(rerun.reads[i].present == first_reads[i].present);
        if (rerun.reads[i].present && first_reads[i].present)
            FIXED_CHECK(rerun.reads[i].bar.close == first_reads[i].bar.close);
    }
}

void test_native_fixed_period_unbacked_and_divisible_controls() {
    const auto bars = native_fixed_period_fixture::input(60, 360);
    for (bool ahead : {false, true}) {
        FixedPeriodProbe unbacked;
        unbacked.ahead = ahead;
        unbacked.run(bars.data(), static_cast<int>(bars.size()), "60", "60",
                     false, 4, MagnifierDistribution::ENDPOINTS);
        FIXED_CHECK(unbacked.last_error().empty());
        FIXED_CHECK(unbacked.reads.size() == bars.size());
        for (std::size_t i = 0; i < unbacked.reads.size() && i < bars.size(); ++i) {
            // Established unbacked fractional-ratio passthrough, not TV data.
            FIXED_CHECK(unbacked.reads[i].present);
            FIXED_CHECK(unbacked.reads[i].bar.close == bars[i].close);
        }
        FIXED_CHECK(unbacked.native_security_substitutions() == 0);
    }
    FixedPeriodProbe divisible;
    divisible.requested = "120";
    std::vector<Bar> native;
    for (int i = 0; i < 3; ++i) {
        const double v = 2000.125 + i;
        native.push_back({v - 1, v + 1, v - 2, v, 33.0,
                          bars[static_cast<std::size_t>(i * 2)].timestamp});
    }
    install_fixed_native(divisible, native);
    divisible.run(bars.data(), static_cast<int>(bars.size()), "60", "60",
                  false, 4, MagnifierDistribution::ENDPOINTS);
    FIXED_CHECK(divisible.last_error().empty());
    FIXED_CHECK(divisible.reads.size() == bars.size());
    for (std::size_t i = 0; i < divisible.reads.size() && i < bars.size(); ++i) {
        FIXED_CHECK(divisible.reads[i].present == (i >= 1));
        if (i >= 1 && divisible.reads[i].present)
            FIXED_CHECK(divisible.reads[i].bar.close == native[(i - 1) / 2].close);
    }
}

void test_native_fixed_period_grid_and_session_controls() {
    namespace fact = tv_fixed_period_75_fact;
    auto bars = native_fixed_period_fixture::input(60, 360);
    auto shifted = native_fixed_period_fixture::native();
    for (auto& bar : shifted) bar.timestamp += fact::kMinuteMs;
    FixedPeriodProbe wrong_grid;
    install_fixed_native(wrong_grid, shifted);
    wrong_grid.run(bars.data(), static_cast<int>(bars.size()), "60", "60",
                   false, 4, MagnifierDistribution::ENDPOINTS);
    FIXED_CHECK(!wrong_grid.last_error().empty());
    FIXED_CHECK(wrong_grid.last_error().find("native fixed-period bar is off the requested grid")
                != std::string::npos);

    // Native invariant, not a TV price/NA assertion. A raw label outside the
    // session supplies no positive accepted close; it must not abort the run.
    FixedPeriodProbe session;
    session.set_syminfo_timezone("UTC");
    session.set_syminfo_session("0930-1600");
    std::vector<Bar> native;
    for (int minute = 570; minute < 960; minute += 75) {
        const double value = 700.5 + minute;
        native.push_back({value - 1, value + 1, value - 2, value, 50.0,
                          fact::kOriginMs + minute * fact::kMinuteMs});
    }
    install_fixed_native(session, native);
    std::vector<Bar> sparse;
    for (int minute : {510, 570, 630, 810, 870, 930}) {
        const double value = 100.5 + minute;
        sparse.push_back({value, value + 1, value - 1, value, 1,
                          fact::kOriginMs + minute * fact::kMinuteMs});
    }
    session.run(sparse.data(), static_cast<int>(sparse.size()), "60", "60",
                false, 4, MagnifierDistribution::ENDPOINTS);
    FIXED_CHECK(session.last_error().empty());
    FIXED_CHECK(session.reads.size() == sparse.size());
    for (std::size_t i = 0; i < session.reads.size(); ++i) {
        if (session.reads[i].present)
            FIXED_CHECK(session.reads[i].requested_close <= session.chart_ends[i]);
    }
}

constexpr char kBreakSession[] = "0930-1200,1300-1615";  // Fixed periods count from the 09:30 open.

std::int64_t break_session_at(int hour, int minute) {
    return tv_fixed_period_75_fact::kOriginMs
        + (hour * 60 + minute) * tv_fixed_period_75_fact::kMinuteMs;
}

// The 14:30 input closes at 15:30, completing the 180-minute row labelled 12:30
// and sealing the 120-minute chart bar labelled 13:30. Pick that chart bar by its open.
void check_row_across_a_break(const std::vector<Bar>& bars) {
    const auto at = break_session_at;
    const std::vector<Bar> rows = {
        {1000.5, 1004.5, 996.5, 1002.5, 11.0, at(9, 30)},
        {2000.5, 2004.5, 1996.5, 2002.5, 12.0, at(12, 30)},
        {3000.5, 3004.5, 2996.5, 3002.5, 13.0, at(15, 30)},
    };
    FixedPeriodProbe host;
    host.requested = "180";
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session(kBreakSession);
    install_fixed_native(host, rows);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "120",
             false, 4, MagnifierDistribution::ENDPOINTS);
    FIXED_CHECK(host.last_error().empty());
    FIXED_CHECK(std::count_if(host.evaluated_bars.begin(), host.evaluated_bars.end(),
                              [at](const Bar& bar) { return bar.timestamp == at(12, 30); }) == 1);
    std::size_t chart = 0;
    while (chart < host.charts.size() && host.charts[chart].timestamp != at(13, 30)) ++chart;
    FIXED_CHECK(chart < host.reads.size());
    if (chart >= host.reads.size()) return;
    const auto& read = host.reads[chart];
    FIXED_CHECK(read.present);
    if (!read.present) return;
    FIXED_CHECK(read.bar.timestamp == at(12, 30));
    FIXED_CHECK(read.bar.close == rows[1].close);
    FIXED_CHECK(read.requested_close == at(15, 30));
}

// A provider label inside a break must not stop the request site's clock. The control
// has the same inputs without the label in the break and reads the same row.
void test_native_fixed_period_provider_label_inside_a_break() {
    const auto at = break_session_at;
    const std::vector<Bar> control = {
        {110, 111, 109, 110, 1, at(13, 30)},
        {120, 121, 119, 120, 1, at(14, 30)},
        {130, 131, 129, 130, 1, at(15, 30)},
    };
    std::vector<Bar> inside_break = control;
    inside_break.insert(inside_break.begin(), Bar{100, 101, 99, 100, 1, at(12, 31)});
    check_row_across_a_break(control);
    check_row_across_a_break(inside_break);
}

// The nominal label of a slot a break clips (12:30 for [12:30, 13:30), traded from 13:00)
// must not stop it either.
void test_native_fixed_period_nominal_label_of_a_clipped_slot() {
    const auto at = break_session_at;
    check_row_across_a_break({
        {100, 101, 99, 100, 1, at(12, 30)},
        {110, 111, 109, 110, 1, at(13, 30)},
        {120, 121, 119, 120, 1, at(14, 30)},
        {130, 131, 129, 130, 1, at(15, 30)},
    });
}

// Return the requested bars in evaluation order. By default the 60-minute inputs
// are also the chart bars on kBreakSession. Callers can choose another session or
// chart timeframe; `run` identifies the tape in failure diagnostics.
std::vector<Bar> published_across_a_break(const char* requested, const std::vector<Bar>& rows,
                                          const std::vector<Bar>& bars, const char* run,
                                          const char* session = kBreakSession,
                                          const char* chart = "60") {
    FixedPeriodProbe host;
    host.requested = requested;
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session(session);
    install_fixed_native(host, rows);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", chart,
             false, 4, MagnifierDistribution::ENDPOINTS);
    if (!host.last_error().empty()) {
        ++fixed_failures;
        std::fprintf(stderr, "fixed-period run '%s': %s\n", run, host.last_error().c_str());
    }
    return host.evaluated_bars;
}

std::vector<std::int64_t> stamps_of(const std::vector<Bar>& published) {
    std::vector<std::int64_t> stamps;
    for (const Bar& bar : published) stamps.push_back(bar.timestamp);
    return stamps;
}

// FIXED_CHECK names only the line: say which run of a test failed.
void name_failed_run(int failures_before, const char* run, std::size_t published) {
    if (fixed_failures != failures_before)
        std::fprintf(stderr, "fixed-period run '%s' failed (%zu rows evaluated)\n", run, published);
}

Bar break_session_row(int minute) {  // a native row that opens `minute` minutes into the day
    const double value = 7000.5 + minute;
    return Bar{value - 1.0, value + 1.0, value - 2.0, value, 50.0,
               break_session_at(minute / 60, minute % 60)};
}

Bar break_session_bar(double value, int hour, int minute) {
    return Bar{value, value + 1.0, value - 1.0, value + 0.5, 1.0, break_session_at(hour, minute)};
}

// Two inputs inside the break, both before their slot's first traded instant (13:00). The input
// after 12:31 arrives before that instant, so 12:31 neither starts the clock nor is observed. The
// input after 12:45 is 13:30, so 12:45 is seated at 13:00, starts the clock there and is observed
// into [12:00, 13:15): the site publishes that row first, then the rows the control publishes.
void test_native_fixed_period_two_inputs_inside_a_break() {
    const auto at = break_session_at;
    std::vector<Bar> rows;
    for (const int minute : {570, 645, 720, 795, 870, 945}) rows.push_back(break_session_row(minute));
    const std::vector<Bar> control = {break_session_bar(130.5, 13, 30), break_session_bar(140.5, 14, 30),
                                      break_session_bar(150.5, 15, 30)};
    std::vector<Bar> tape = {break_session_bar(120.5, 12, 31), break_session_bar(125.5, 12, 45)};
    tape.insert(tape.end(), control.begin(), control.end());

    int before = fixed_failures;
    const auto from_control = stamps_of(published_across_a_break("75", rows, control, "control"));
    const std::vector<std::int64_t> want_control = {at(13, 15), at(14, 30), at(15, 45)};
    FIXED_CHECK(from_control == want_control);
    name_failed_run(before, "control", from_control.size());

    before = fixed_failures;
    const char* run = "two inputs inside the break";
    const auto from_tape = stamps_of(published_across_a_break("75", rows, tape, run));
    std::vector<std::int64_t> want_tape = {at(12, 0)};
    want_tape.insert(want_tape.end(), want_control.begin(), want_control.end());
    FIXED_CHECK(from_tape == want_tape);
    name_failed_run(before, run, from_tape.size());
}

// An input inside the break that ends before its slot's first traded instant (13:00) has no
// traded time, so its bar must not enter the bucket that opens at 13:00. That bucket's native
// row is absent, so the site publishes the aggregate of the bars from 13:00 on, which opens with
// the 13:00 bar, as it does on the control without the 12:31 bar.
void test_native_fixed_period_break_input_is_not_folded_into_the_next_bucket() {
    const auto at = break_session_at;
    std::vector<Bar> rows;
    for (const int minute : {570, 675, 885}) rows.push_back(break_session_row(minute));
    const std::vector<Bar> control = {
        break_session_bar(100.5, 9, 30), break_session_bar(200.5, 10, 30),
        break_session_bar(300.5, 11, 30), break_session_bar(613.5, 13, 0),
        break_session_bar(700.5, 13, 30), break_session_bar(800.5, 14, 30)};
    std::vector<Bar> tape = control;
    tape.insert(tape.begin() + 3, break_session_bar(512.5, 12, 31));
    const std::vector<std::int64_t> want = {at(9, 30), at(11, 15), at(13, 0)};
    const auto check = [&](const char* run, const std::vector<Bar>& bars) {
        const int before = fixed_failures;
        const auto published = published_across_a_break("105", rows, bars, run);
        FIXED_CHECK(stamps_of(published) == want);
        if (published.size() == want.size()) {
            // The aggregate of the bars 13:00, 13:30 and 14:30, and nothing of the 12:31 bar.
            FIXED_CHECK(published.back().open == 613.5);
            FIXED_CHECK(published.back().high == 801.5);
            FIXED_CHECK(published.back().low == 612.5);
            FIXED_CHECK(published.back().close == 801.0);
            FIXED_CHECK(published.back().volume == 3.0);
        }
        name_failed_run(before, run, published.size());
    };
    check("control", control);
    check("bar inside the break", tape);
}

// A first input labelled inside a break AFTER traded time of its slot (10:05 in the slot
// [09:30, 10:30) of 0930-1000,1010-1600, which trades to 10:00 and again from 10:10) is seated on
// the next traded instant, so the clock goes on. Seated on its label, the clock never moved again
// and the site published nothing. The tape is the control with that bar in front, and both runs
// must publish `want`. Minutes are minutes into the day.
void check_first_label_in_a_break(const char* session, const char* requested, const char* chart,
                                  const std::vector<int>& row_minutes, int label_minute,
                                  const std::vector<int>& control_minutes,
                                  const std::vector<std::int64_t>& want,
                                  const char* control_run, const char* tape_run) {
    std::vector<Bar> rows;
    for (const int minute : row_minutes) rows.push_back(break_session_row(minute));
    std::vector<Bar> control;
    for (const int minute : control_minutes)
        control.push_back(break_session_bar(100.5 + minute, minute / 60, minute % 60));
    std::vector<Bar> tape = control;
    tape.insert(tape.begin(), break_session_bar(50.5, label_minute / 60, label_minute % 60));
    for (const bool with_label : {false, true}) {
        const int before = fixed_failures;
        const char* run = with_label ? tape_run : control_run;
        const auto published = stamps_of(published_across_a_break(
            requested, rows, with_label ? tape : control, run, session, chart));
        FIXED_CHECK(published == want);
        name_failed_run(before, run, published.size());
    }
}

// On an aggregated chart: input 60, chart 120, a 180-minute request (rows 09:30, 12:30, 15:30).
void test_native_fixed_period_first_label_in_a_break_on_an_aggregated_chart() {
    const auto at = break_session_at;
    check_first_label_in_a_break(
        "0930-1000,1010-1600", "180", "120", {570, 750, 930}, 10 * 60 + 5,
        {630, 690, 750, 810, 870, 930}, {at(9, 30), at(12, 30), at(15, 30)},
        "aggregated chart, control", "aggregated chart, first label in a break after traded time");
}

// On a chart of the input's own timeframe: chart = input = 60, a 75-minute request.
void test_native_fixed_period_first_label_in_a_break_on_the_input_timeframe() {
    const auto at = break_session_at;
    check_first_label_in_a_break(
        "0930-1000,1010-1600", "75", "60", {570, 645, 720, 795, 870, 945}, 10 * 60 + 5,
        {630, 690, 750, 810, 870, 930},
        {at(9, 30), at(10, 45), at(12, 0), at(13, 15), at(14, 30), at(15, 45)},
        "input timeframe, control", "input timeframe, first label in a break after traded time");
}

// Two breaks in one day: the label 11:05 lies in the second one, after traded time of its slot
// [10:30, 11:30), and its seat is the start of the span after it (11:10), not of an earlier span.
void test_native_fixed_period_first_label_in_the_second_break_of_a_day() {
    const auto at = break_session_at;
    check_first_label_in_a_break(
        "0930-1000,1010-1100,1110-1600", "180", "120", {570, 750, 930}, 11 * 60 + 5,
        {690, 750, 810, 870, 930}, {at(9, 30), at(12, 30), at(15, 30)},
        "two breaks, control", "two breaks, first label in the second break");
}

// ── an exchange session-day that closes early ────────────────────────────────
//
// Native engine controls over supplied bars (native_fixed_period_fixture::
// exchange): no TradingView price, na or time is asserted here. One site of
// one requested timeframe; the host records what each chart bar reads and
// places no order.
class ExchangeDayProbe final : public pineforge::source::PineStrategyHost {
public:
    struct Read {
        Bar bar{};
        bool present = false;
    };
    std::string requested = "75";
    bool gaps = false;
    Read latest;
    std::vector<Read> reads;
    std::vector<Bar> charts;
    std::vector<Bar> evaluated_bars;
    void configure_security_evaluators() override {
        latest = {}; reads.clear(); charts.clear(); evaluated_bars.clear();
        security_eval_states_.clear();
        register_security_eval(0, requested, input_tf_, false, gaps);
    }
    void evaluate_security(int, const Bar& bar, bool) override {
        evaluated_bars.push_back(bar);
        latest = {bar, true};
    }
    void clear_security(int) override { latest = {}; }
    void on_source_bar(const Bar& bar) override {
        reads.push_back(latest);
        charts.push_back(bar);
    }
};

struct ExchangeDayForm {
    int chart;         // chart timeframe, minutes
    int input;         // primary input timeframe, minutes
    int aux;           // auxiliary request.security feed, minutes; 0: none
    int requested;     // requested timeframe, minutes
    int early_close;   // minute of day the middle session-day stops at
    const char* type;  // syminfo.type
    bool exchange;     // whether that class's session template knows early closes
    bool gaps;
};

#define EXCHANGE_DAY_CHECK(condition) do { if (!(condition)) { \
    ++fixed_failures; std::fprintf(stderr, \
        "exchange-day line %d: %s [chart %d input %d aux %d request %d close %d %s%s row %d]\n", \
        __LINE__, #condition, form.chart, form.input, form.aux, form.requested, \
        form.early_close, form.type, form.gaps ? " gaps" : "", at); } } while (false)

// Replays `bars` (and `aux`, when the form names an auxiliary feed) against
// the supplied native rows and holds every chart bar, and the evaluation
// order, to the fixture's arithmetic.
void check_exchange_day_tape(const ExchangeDayForm& form, const std::vector<Bar>& bars,
                             const std::vector<Bar>& aux) {
    namespace day = native_fixed_period_fixture::exchange;
    const auto rows = day::native(form.requested, form.early_close);
    int at = -1;
    ExchangeDayProbe host;
    host.requested = std::to_string(form.requested);
    host.gaps = form.gaps;
    host.set_syminfo_timezone(day::kTimezone);
    host.set_syminfo_session(day::kSession);
    host.set_syminfo_type(form.type);
    EXCHANGE_DAY_CHECK(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&host), host.requested.c_str(),
        reinterpret_cast<const pf_bar_t*>(rows.data()),
        static_cast<int>(rows.size())) == 0);
    if (form.aux != 0) {
        EXCHANGE_DAY_CHECK(strategy_set_aux_security_feed(
            static_cast<pf_strategy_t>(&host),
            reinterpret_cast<const pf_bar_t*>(aux.data()),
            static_cast<int>(aux.size()), std::to_string(form.aux).c_str()) == 0);
    }
    host.run(bars.data(), static_cast<int>(bars.size()), std::to_string(form.input),
             std::to_string(form.chart), false, 4, MagnifierDistribution::ENDPOINTS);
    EXCHANGE_DAY_CHECK(host.last_error().empty());

    // The series is built from the auxiliary feed when there is one.
    const auto& source = form.aux != 0 ? aux : bars;
    const int source_step = form.aux != 0 ? form.aux : form.input;
    const auto closes =
        day::row_closes(rows, form.requested, source, source_step, form.exchange);
    const auto charts = day::charts(bars, form.input, form.chart);
    EXCHANGE_DAY_CHECK(host.reads.size() == charts.size());
    std::int64_t previous = std::numeric_limits<std::int64_t>::min();
    for (std::size_t i = 0; i < charts.size() && i < host.reads.size(); ++i) {
        at = static_cast<int>(i);
        const int want = day::expected_row(closes, charts[i].close_ms, previous, form.gaps);
        previous = charts[i].close_ms;
        const auto& got = host.reads[i];
        EXCHANGE_DAY_CHECK(host.charts[i].timestamp == charts[i].open_ms);
        // Neither a row whose period is still open at this chart bar's close
        // nor a held row its own close has not reached.
        EXCHANGE_DAY_CHECK(got.present == (want >= 0));
        if (!got.present || want < 0) continue;
        // That very row: its label and every field of the supplied payload.
        const Bar& row = rows[static_cast<std::size_t>(want)];
        EXCHANGE_DAY_CHECK(got.bar.timestamp == row.timestamp);
        EXCHANGE_DAY_CHECK(got.bar.open == row.open && got.bar.high == row.high);
        EXCHANGE_DAY_CHECK(got.bar.low == row.low && got.bar.close == row.close);
        EXCHANGE_DAY_CHECK(got.bar.volume == row.volume);
    }
    // Every supplied row is evaluated exactly once and in order: a period the
    // early close ended is not delivered again by the next session's first
    // input, and no later row overtakes it.
    at = -1;
    EXCHANGE_DAY_CHECK(host.evaluated_bars.size() == rows.size());
    for (std::size_t i = 0; i < rows.size() && i < host.evaluated_bars.size(); ++i) {
        at = static_cast<int>(i);
        EXCHANGE_DAY_CHECK(host.evaluated_bars[i].timestamp == rows[i].timestamp);
        EXCHANGE_DAY_CHECK(host.evaluated_bars[i].close == rows[i].close);
    }
}

void check_exchange_day_tape(const ExchangeDayForm& form) {
    namespace day = native_fixed_period_fixture::exchange;
    check_exchange_day_tape(
        form, day::bars(form.input, form.early_close),
        form.aux != 0 ? day::bars(form.aux, form.early_close) : std::vector<Bar>{});
}
#undef EXCHANGE_DAY_CHECK

// The session-day's last accepted input ends the requested period it leaves
// open: a 75-minute period that opened at 12:00 is complete at a 13:00 (and a
// 12:30) close, on the chart bar that close ends, when the next accepted
// input opens a later session-day. The same law the divisible ratio path
// keeps (its merged and its singleton form), here for a request that is not
// a multiple of its chart. Chart input, a finer primary input and the
// auxiliary feed all reach it; the days around the early one keep the
// nominal close.
void test_native_fixed_period_exchange_early_close() {
    for (int early_close : {13 * 60, 12 * 60 + 30}) {
        for (bool gaps : {false, true}) {
            for (int chart : {30, 60}) {
                for (int fine : {5, 1}) {
                    check_exchange_day_tape(
                        {chart, fine, 0, 75, early_close, "stock", true, gaps});
                    check_exchange_day_tape(
                        {chart, chart, fine, 75, early_close, "stock", true, gaps});
                }
            }
            // The chart's own bars as input. A 30-minute bar never crosses a
            // 75-minute boundary of this session inside the early day.
            check_exchange_day_tape({30, 30, 0, 75, early_close, "stock", true, gaps});
        }
    }
    for (bool gaps : {false, true}) {
        // A 60-minute chart bar of its own input: the 12:30 bar's accepted
        // interval runs to 13:30, past the period's nominal end, so the
        // period completes on it without the early-close law.
        check_exchange_day_tape({60, 60, 0, 75, 13 * 60, "stock", true, gaps});
        // The law is the instrument class's, not one spelling of it.
        check_exchange_day_tape({30, 30, 0, 75, 13 * 60, "futures", true, gaps});
    }
}

// The gate: an OTC quote stream has no holiday template, so its period keeps
// its nominal end and is revealed only by the next accepted input.
void test_native_fixed_period_early_close_gate() {
    for (const char* type : {"crypto", "forex", "cfd"}) {
        for (int early_close : {13 * 60, 12 * 60 + 30}) {
            for (bool gaps : {false, true}) {
                check_exchange_day_tape({30, 30, 0, 75, early_close, type, false, gaps});
                check_exchange_day_tape({30, 5, 0, 75, early_close, type, false, gaps});
                check_exchange_day_tape({30, 30, 5, 75, early_close, type, false, gaps});
            }
        }
    }
}

// A hole inside one session-day is not a close: the input before it leaves
// its period open, the first input after it reveals that period and the
// unobserved one behind it in order, and nothing is read before its own end.
void test_native_fixed_period_session_hole_is_not_a_close() {
    namespace day = native_fixed_period_fixture::exchange;
    // Three whole session-days, so no close is early; 13:00 to 14:30 of the
    // first one is missing from every supplied feed.
    const int whole = day::kCloseMinute;
    const auto holed = [whole](int step) {
        return day::without(day::bars(step, whole), 0, 13 * 60, 14 * 60 + 30);
    };
    for (bool gaps : {false, true}) {
        for (int fine : {30, 5}) {
            check_exchange_day_tape({30, fine, 0, 75, whole, "stock", true, gaps},
                                    holed(fine), {});
        }
        check_exchange_day_tape({30, 30, 5, 75, whole, "stock", true, gaps},
                                holed(30), holed(5));
    }
}

// A request the chart divides keeps the ratio path and its own early-close
// forms: the 12:30 bar of a 13:00 close is the whole 60-minute period it
// opens (the singleton form), and a finer input ends that period on its last
// bar (the merged form).
void test_native_divisible_period_early_close_is_kept() {
    for (int early_close : {13 * 60, 12 * 60 + 30}) {
        check_exchange_day_tape({30, 30, 0, 60, early_close, "stock", true, false});
        check_exchange_day_tape({30, 5, 0, 60, early_close, "stock", true, false});
    }
}

// ── inputs after a chart bar that is calculated ahead of its completion ─────
//
// Native engine controls over supplied bars: no TradingView price, na or time
// is asserted here, and no expectation is read back from the engine.
//
// A chart bar its own inputs do not complete (its session-day closes early,
// or the supplied bars over its last slots are missing) is calculated when
// the first input of a later chart bar is accepted, before that input reaches
// any request.security site. The source scheduler hears of the completion
// from that same input. Every input from there on must still reach every site
// exactly once and in order, and the chart bar calculated ahead must read
// nothing a later input supplied.
//
// One host, three sites, so that a fully supplied native tape cannot stand in
// for the inputs:
//   0  the requested fixed period, with native rows. The second period of
//      every session-day has no row: it is the aggregate of the inputs
//      observed into it, or it is never evaluated at all;
//   1  "D" with no feed of its own: built from the inputs alone;
//   2  a finer lookahead_on request: evaluated on every input and published
//      once per chart bar, before that chart bar's body.
class LaterInputProbe final : public pineforge::source::PineStrategyHost {
public:
    struct Read {
        Bar bar{};
        bool present = false;
    };
    struct FineEvaluation {
        double close = 0.0;
        bool publication = false;
        bool replay = false;
    };
    std::string requested = "75";
    std::string fine = "15";
    bool gaps = false;
    Read latest_period;
    Read latest_day;
    std::vector<Read> period_reads;              // site 0, at each chart bar
    std::vector<Read> day_reads;                 // site 1, at each chart bar
    std::vector<std::size_t> fine_before_chart;  // site 2 evaluations before each chart bar
    std::vector<Bar> charts;
    std::vector<Bar> period_evaluated;           // every evaluation, in order
    std::vector<Bar> day_evaluated;
    std::vector<FineEvaluation> fine_evaluated;
    void configure_security_evaluators() override {
        latest_period = {}; latest_day = {};
        period_reads.clear(); day_reads.clear(); fine_before_chart.clear(); charts.clear();
        period_evaluated.clear(); day_evaluated.clear(); fine_evaluated.clear();
        security_eval_states_.clear();
        register_security_eval(0, requested, input_tf_, false, gaps);
        register_security_eval(1, "D", input_tf_, false, false);
        register_security_eval(2, fine, input_tf_, true, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool publication) override {
        if (sec_id == 0) {
            period_evaluated.push_back(bar);
            latest_period = {bar, true};
        } else if (sec_id == 1) {
            day_evaluated.push_back(bar);
            latest_day = {bar, true};
        } else if (sec_id == 2) {
            fine_evaluated.push_back(
                {bar.close, publication, current_request_delivery(2).replay});
        }
    }
    void clear_security(int sec_id) override {
        if (sec_id == 0) latest_period = {};
    }
    void on_source_bar(const Bar& bar) override {
        period_reads.push_back(latest_period);
        day_reads.push_back(latest_day);
        fine_before_chart.push_back(fine_evaluated.size());
        charts.push_back(bar);
    }
};

struct LaterInputForm {
    const char* name;
    std::vector<int> closes;  // the minute each supplied session-day stops at
    int hole_day;             // a session-day with supplied bars missing; -1: none
    int hole_from;            // the first missing minute of that day
    int hole_to;              // the first minute supplied again
};

#define LATER_INPUT_CHECK(condition) do { if (!(condition)) { \
    ++fixed_failures; std::fprintf(stderr, \
        "later-input line %d: %s [%s%s at %d]\n", \
        __LINE__, #condition, form.name, gaps ? " gaps" : "", at); } } while (false)

void check_later_input_tape(const LaterInputForm& form, bool gaps) {
    namespace day = native_fixed_period_fixture::exchange;
    constexpr int kStep = 5;        // the primary input, minutes
    constexpr int kChart = 60;      // the chart, minutes
    constexpr int kRequested = 75;  // site 0, minutes
    constexpr int kFine = 15;       // site 2, minutes
    const std::int64_t minute_ms = tv_fixed_period_75_fact::kMinuteMs;
    const int days = static_cast<int>(form.closes.size());
    int at = -1;

    // The supplied primary bars: every kStep minutes of each session-day up to
    // the minute it stops at, without the hole. Every close is distinct, so a
    // close names its bar.
    std::vector<Bar> bars;
    for (int d = 0; d < days; ++d) {
        for (int minute = day::kOpenMinute; minute < form.closes[static_cast<std::size_t>(d)];
             minute += kStep) {
            if (d == form.hole_day && minute >= form.hole_from && minute < form.hole_to) continue;
            const double value = 100.25 + d * 50 + (minute - day::kOpenMinute) * 0.125;
            bars.push_back({value - 0.5, value + 1, value - 1, value, 3.0, day::at(d, minute)});
        }
    }
    const std::int64_t feed_end = day::bar_close(bars.back(), kStep);
    // The aggregate of the supplied bars that open in [from, to): first open,
    // extreme high and low, last close, summed volume. False: none opens there.
    const auto supplied = [&bars](std::int64_t from, std::int64_t to, Bar& out) {
        bool any = false;
        for (const Bar& bar : bars) {
            if (bar.timestamp < from || bar.timestamp >= to) continue;
            if (!any) {
                out = bar;
                any = true;
                continue;
            }
            out.high = std::max(out.high, bar.high);
            out.low = std::min(out.low, bar.low);
            out.close = bar.close;
            out.volume += bar.volume;
        }
        return any;
    };

    // Site 0. `rows` is what the host is given: one sentinel row per requested
    // period that opened before its session-day stopped, except the second
    // period of each session-day. `series` is what the site must evaluate, in
    // order: the row where there is one, else the aggregate of the supplied
    // bars of that period, labelled with the period's open; a period with
    // neither is absent.
    std::vector<Bar> rows;
    std::vector<Bar> series;
    for (int d = 0; d < days; ++d) {
        int ordinal = 0;
        for (int minute = day::kOpenMinute; minute < day::kCloseMinute;
             minute += kRequested, ++ordinal) {
            const std::int64_t open = day::at(d, minute);
            if (minute < form.closes[static_cast<std::size_t>(d)] && ordinal != 1) {
                const double value = 5000.5 + d * 1000 + (minute - day::kOpenMinute);
                rows.push_back({value - 2, value + 4, value - 5, value,
                                7000.5 + static_cast<double>(rows.size()), open});
                series.push_back(rows.back());
                continue;
            }
            const std::int64_t end = std::min(open + kRequested * minute_ms,
                                              day::at(d, day::kCloseMinute));
            Bar aggregate{};
            if (!supplied(open, end, aggregate)) continue;
            aggregate.timestamp = open;
            series.push_back(aggregate);
        }
    }
    // When each of them is complete: the fixture's exchange-calendar rule.
    const auto closes = day::row_closes(series, kRequested, bars, kStep, true);

    // The chart bars the supplied bars form. A chart bar closes at its own
    // end on the session grid, clipped by the session close, whether or not
    // its supplied bars reach that far.
    struct ChartBar {
        std::int64_t open_ms;
        std::int64_t accepted_ms;  // the close of its last supplied bar
        std::int64_t close_ms;
        std::size_t first;         // its supplied bars: [first, last]
        std::size_t last;
    };
    std::vector<ChartBar> charts;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const int d = day::day_of(bars[i].timestamp);
        const int minute = static_cast<int>((bars[i].timestamp - day::at(d, 0)) / minute_ms);
        const std::int64_t open =
            day::at(d, day::kOpenMinute + (minute - day::kOpenMinute) / kChart * kChart);
        if (charts.empty() || charts.back().open_ms != open) {
            charts.push_back({open, 0, std::min(open + kChart * minute_ms,
                                                 day::at(d, day::kCloseMinute)), i, i});
        }
        charts.back().accepted_ms = day::bar_close(bars[i], kStep);
        charts.back().last = i;
    }

    // Site 2, every evaluation in order. One per supplied bar, carrying that
    // bar's close. A chart bar its supplied bars complete publishes on the
    // last of them. A chart bar they do not complete is calculated when a
    // later supplied bar is accepted: its last value is published again, once,
    // ahead of its body and ahead of that later bar. With no later supplied
    // bar the run's last chart bar is never calculated.
    std::vector<LaterInputProbe::FineEvaluation> fine;
    std::vector<std::size_t> fine_before_chart;
    std::size_t calculated = 0;
    for (std::size_t c = 0; c < charts.size(); ++c) {
        const ChartBar& chart = charts[c];
        const bool completes = chart.accepted_ms >= chart.close_ms;
        for (std::size_t i = chart.first; i <= chart.last; ++i)
            fine.push_back({bars[i].close, completes && i == chart.last, false});
        if (!completes) {
            if (c + 1 == charts.size()) break;
            fine.push_back({bars[chart.last].close, true, true});
        }
        fine_before_chart.push_back(fine.size());
        ++calculated;
    }

    // Site 1: the whole session-days. A session-day is complete on its last
    // supplied bar when that bar reaches the session close, or when a later
    // session-day follows it (an exchange's early close).
    struct DayBar {
        Bar bar;
        int session_day;
        std::int64_t close_ms;  // the close of its last supplied bar
    };
    std::vector<DayBar> day_bars;
    const int last_day = day::day_of(bars.back().timestamp);
    for (int d = 0; d < days; ++d) {
        Bar aggregate{};
        if (!supplied(day::at(d, 0), day::at(d + 1, 0), aggregate)) continue;
        std::int64_t close = 0;
        for (const Bar& bar : bars) {
            if (day::day_of(bar.timestamp) == d) close = day::bar_close(bar, kStep);
        }
        if (close < day::at(d, day::kCloseMinute) && d >= last_day) continue;
        day_bars.push_back({aggregate, d, close});
    }

    LaterInputProbe host;
    host.requested = std::to_string(kRequested);
    host.fine = std::to_string(kFine);
    host.gaps = gaps;
    host.set_syminfo_timezone(day::kTimezone);
    host.set_syminfo_session(day::kSession);
    host.set_syminfo_type("stock");
    LATER_INPUT_CHECK(strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&host), host.requested.c_str(),
        reinterpret_cast<const pf_bar_t*>(rows.data()),
        static_cast<int>(rows.size())) == 0);
    host.run(bars.data(), static_cast<int>(bars.size()), std::to_string(kStep),
             std::to_string(kChart), false, 4, MagnifierDistribution::ENDPOINTS);
    LATER_INPUT_CHECK(host.last_error().empty());

    LATER_INPUT_CHECK(host.charts.size() == calculated);
    std::int64_t previous = std::numeric_limits<std::int64_t>::min();
    for (std::size_t i = 0; i < calculated && i < host.charts.size(); ++i) {
        at = static_cast<int>(i);
        const ChartBar& chart = charts[i];
        LATER_INPUT_CHECK(host.charts[i].timestamp == chart.open_ms);

        // Site 0: the latest period complete by this chart bar's close; with
        // gaps, only one that completed after the previous chart bar's close.
        // Never a period a later input completes, and never a row in place of
        // the aggregate of a period that has no row.
        const int want = day::expected_row(closes, chart.close_ms, previous, gaps);
        previous = chart.close_ms;
        const auto& got = host.period_reads[i];
        LATER_INPUT_CHECK(got.present == (want >= 0));
        if (got.present && want >= 0) {
            const Bar& period = series[static_cast<std::size_t>(want)];
            LATER_INPUT_CHECK(got.bar.timestamp == period.timestamp);
            LATER_INPUT_CHECK(got.bar.open == period.open && got.bar.high == period.high);
            LATER_INPUT_CHECK(got.bar.low == period.low && got.bar.close == period.close);
            LATER_INPUT_CHECK(got.bar.volume == period.volume);
        }

        // Site 1: the latest session-day whose last supplied bar this chart
        // bar holds or follows.
        int day_want = -1;
        for (std::size_t k = 0; k < day_bars.size(); ++k) {
            if (day_bars[k].close_ms <= chart.accepted_ms) day_want = static_cast<int>(k);
        }
        const auto& day_got = host.day_reads[i];
        LATER_INPUT_CHECK(day_got.present == (day_want >= 0));
        if (day_got.present && day_want >= 0) {
            const DayBar& whole = day_bars[static_cast<std::size_t>(day_want)];
            LATER_INPUT_CHECK(day::day_of(day_got.bar.timestamp) == whole.session_day);
            LATER_INPUT_CHECK(day_got.bar.open == whole.bar.open
                              && day_got.bar.high == whole.bar.high);
            LATER_INPUT_CHECK(day_got.bar.low == whole.bar.low
                              && day_got.bar.close == whole.bar.close);
            LATER_INPUT_CHECK(day_got.bar.volume == whole.bar.volume);
        }

        // Site 2: exactly this chart bar's own inputs, and its one
        // publication, have been evaluated when its body runs.
        LATER_INPUT_CHECK(host.fine_before_chart[i] == fine_before_chart[i]);
    }

    // Site 0: every period complete within the supplied bars is evaluated
    // exactly once and in order, the aggregates included.
    std::vector<Bar> evaluated;
    for (std::size_t i = 0; i < series.size(); ++i) {
        if (closes[i] <= feed_end) evaluated.push_back(series[i]);
    }
    at = -1;
    LATER_INPUT_CHECK(host.period_evaluated.size() == evaluated.size());
    for (std::size_t i = 0; i < evaluated.size() && i < host.period_evaluated.size(); ++i) {
        at = static_cast<int>(i);
        const Bar& seen = host.period_evaluated[i];
        LATER_INPUT_CHECK(seen.timestamp == evaluated[i].timestamp);
        LATER_INPUT_CHECK(seen.open == evaluated[i].open && seen.high == evaluated[i].high);
        LATER_INPUT_CHECK(seen.low == evaluated[i].low && seen.close == evaluated[i].close);
        LATER_INPUT_CHECK(seen.volume == evaluated[i].volume);
    }

    // Site 1: every complete session-day exactly once and in order.
    at = -1;
    LATER_INPUT_CHECK(host.day_evaluated.size() == day_bars.size());
    for (std::size_t i = 0; i < day_bars.size() && i < host.day_evaluated.size(); ++i) {
        at = static_cast<int>(i);
        const Bar& seen = host.day_evaluated[i];
        LATER_INPUT_CHECK(day::day_of(seen.timestamp) == day_bars[i].session_day);
        LATER_INPUT_CHECK(seen.open == day_bars[i].bar.open && seen.high == day_bars[i].bar.high);
        LATER_INPUT_CHECK(seen.low == day_bars[i].bar.low && seen.close == day_bars[i].bar.close);
        LATER_INPUT_CHECK(seen.volume == day_bars[i].bar.volume);
    }

    // Site 2: every supplied bar exactly once and in order, each chart bar's
    // one publication in its place.
    at = -1;
    LATER_INPUT_CHECK(host.fine_evaluated.size() == fine.size());
    for (std::size_t i = 0; i < fine.size() && i < host.fine_evaluated.size(); ++i) {
        at = static_cast<int>(i);
        LATER_INPUT_CHECK(host.fine_evaluated[i].close == fine[i].close);
        LATER_INPUT_CHECK(host.fine_evaluated[i].publication == fine[i].publication);
        LATER_INPUT_CHECK(host.fine_evaluated[i].replay == fine[i].replay);
    }
}
#undef LATER_INPUT_CHECK

// The inputs that follow a chart bar calculated ahead of its completion keep
// reaching every request.security site. Each form leaves one 60-minute chart
// bar short of its close and supplies more bars after it:
//   - a session-day that closes at 13:00, then a whole one;
//   - bars missing from 13:00 to 14:30 inside a session-day;
//   - two early session-days in a row (13:00, then 12:30): the second one's
//     last input still ends the requested period it leaves open;
//   - a last session-day that stops at 11:00, inside a chart bar.
void test_inputs_after_a_chart_bar_calculated_ahead_reach_every_site() {
    namespace day = native_fixed_period_fixture::exchange;
    const int whole = day::kCloseMinute;
    const LaterInputForm forms[] = {
        {"early close", {whole, 13 * 60, whole}, -1, 0, 0},
        {"hole", {whole, whole, whole}, 0, 13 * 60, 14 * 60 + 30},
        {"two early closes", {whole, 13 * 60, 12 * 60 + 30, whole}, -1, 0, 0},
        {"partial last day", {whole, 13 * 60, 11 * 60}, -1, 0, 0},
    };
    for (const LaterInputForm& form : forms) {
        for (bool gaps : {false, true}) check_later_input_tape(form, gaps);
    }
}

// ── the report as filed: entry quantities of a fixed "75" request ───────────
//
// The public script, on a 60-minute chart and feed:
//     got_v = request.security(syminfo.tickerid, "75", close)
//     qty_v = 1 + int(got_v) % 5
//     if bar_index > 6 and bar_index % 7 == 3
//         strategy.entry("L", strategy.long, qty = qty_v)
//     if bar_index > 6 and bar_index % 7 == 5
//         strategy.close("L")
// FixedPeriodProbe is that script: one entry id, so its close is the whole
// position's.
//
// Every expectation is arithmetic on the supplied inputs; none is read back
// from the engine. The schedule is the script's own. A market order fills on
// the open of the bar after its decision. With lookahead off a chart bar
// reads the latest requested bar that has closed by the chart bar's own
// close, which the retained chart-60 mapping must confirm.
//
// The same chart twice. With the retained 75-minute bars supplied, every
// booked entry carries the quantity of the retained requested close. With
// the 60-minute feed alone -- the report's own condition -- the request reads
// the 60-minute close, so every booked entry carries that close's quantity:
// the reported fault, which the retained expectation tells apart on each one.
#define REPORTED_QUANTITY_CHECK(condition) do { if (!(condition)) { \
    ++fixed_failures; std::fprintf(stderr, \
        "reported-quantity line %d: %s [%s entry %d bar %d]\n", __LINE__, #condition, \
        supplied ? "75-minute bars supplied" : "60-minute feed alone", at, bar); } } while (false)

void test_native_fixed_period_reported_entry_quantities() {
    namespace fact = tv_fixed_period_75_fact;
    // 44 chart bars: the last scheduled entry (bar 38) is filled and closed
    // (bar 41) inside them, so the run ends flat with one closed trade each.
    constexpr int kChartBars = 44;
    const auto chart = native_fixed_period_fixture::input(60, kChartBars * 60);
    const auto rows = native_fixed_period_fixture::native();
    struct Scheduled {
        int bar;  // the chart bar the script decides the entry on
        int row;  // the requested bar that chart bar reads, lookahead off
    };
    std::vector<Scheduled> schedule;
    for (int bar = 0; bar < kChartBars; ++bar) {
        if (!(bar > 6 && bar % 7 == 3)) continue;
        int row = -1;
        for (int k = 0; k < fact::kBarCount; ++k) {
            if (fact::kBars[k].close_minute <= (bar + 1) * 60) row = k;
        }
        schedule.push_back({bar, row});
    }
    FIXED_CHECK(schedule.size() == 5);
    int told_apart = 0;
    for (bool supplied : {true, false}) {
        int at = -1;
        int bar = -1;
        FixedPeriodProbe host;
        if (supplied) install_fixed_native(host, rows);
        host.run(chart.data(), static_cast<int>(chart.size()), "60", "60", false, 4,
                 MagnifierDistribution::ENDPOINTS);
        REPORTED_QUANTITY_CHECK(host.last_error().empty());
        REPORTED_QUANTITY_CHECK(host.reads.size() == chart.size());
        // One booked trade per scheduled entry: none missing, none extra.
        const int booked = static_cast<int>(host.trade_count());
        REPORTED_QUANTITY_CHECK(booked == static_cast<int>(schedule.size()));
        if (host.reads.size() != chart.size()) continue;
        for (at = 0; at < booked && at < static_cast<int>(schedule.size()); ++at) {
            const Scheduled& entry = schedule[static_cast<std::size_t>(at)];
            bar = entry.bar;
            REPORTED_QUANTITY_CHECK(entry.row >= 0 && bar + 3 < kChartBars);
            if (entry.row < 0 || bar + 3 >= kChartBars) continue;
            REPORTED_QUANTITY_CHECK(entry.row == fact::kChart60LookaheadOff[bar]);
            const auto slot = static_cast<std::size_t>(bar);
            const double retained = fact::kBars[entry.row].value;
            const int want = 1 + static_cast<int>(retained) % 5;
            // What the script reads on its decision bar, and so orders: the
            // retained requested close, or the chart bar's own close when
            // only the 60-minute feed exists.
            const double read = supplied ? retained : chart[slot].close;
            const int ordered = 1 + static_cast<int>(read) % 5;
            REPORTED_QUANTITY_CHECK(host.reads[slot].present);
            REPORTED_QUANTITY_CHECK(host.reads[slot].bar.close == read);
            // This booked trade is that decision's: entered on the next open
            // and closed on the open after the script's close two bars on.
            const auto& trade = host.get_trade(at);
            REPORTED_QUANTITY_CHECK(near(trade.entry_price, chart[slot + 1].open));
            REPORTED_QUANTITY_CHECK(near(trade.exit_price, chart[slot + 3].open));
            if (trade.qty != ordered) {
                std::fprintf(stderr, "reported-quantity: booked %.17g, the close %.17g "
                             "read on bar %d orders %d\n", trade.qty, read, bar, ordered);
            }
            // With the 75-minute bars supplied, `ordered` is `want`.
            REPORTED_QUANTITY_CHECK(trade.qty == ordered);
            if (!supplied && trade.qty != want) ++told_apart;
        }
    }
    FIXED_CHECK(told_apart == static_cast<int>(schedule.size()));
}

// A fixed-period request reads its authoritative feed through the routing
// index prepare_native_security_feeds left. The Pine host's feed setter stays
// usable during a run, so a feed the script callback removes can leave that
// index past the feed store: the next selected period must refuse the run
// with the typed invariant, not read past the store.
#define FEED_BOUND_CHECK(condition) do { if (!(condition)) { \
    ++fixed_failures; std::fprintf(stderr, "feed-bound line %d: %s\n", \
                                 __LINE__, #condition); } } while (false)

class FeedBoundProbe final : public pineforge::source::PineStrategyHost {
public:
    bool removed = false;
    int index_after_removal = -2;
    std::size_t feeds_after_removal = 0;
    int evaluated = 0;
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "75", input_tf_, true, false);
    }
    void evaluate_security(int, const Bar&, bool) override { ++evaluated; }
    void clear_security(int) override {}
    void on_source_bar(const Bar&) override {
        // After the second chart bar, whose requested period is published:
        // the third chart bar opens the next requested period.
        if (bar_index_ != 1 || removed) return;
        removed = set_native_security_feed("90", nullptr, 0);
        index_after_removal = security_eval_states_.size() == 1
            ? security_eval_states_[0].native_feed_index : -2;
        feeds_after_removal = native_security_feeds_.size();
    }
};

void test_native_fixed_period_feed_removed_during_run() {
    using namespace tv_fixed_period_75_fact;
    const auto chart = native_fixed_period_fixture::input(60, 360);
    const auto rows = native_fixed_period_fixture::native();
    // A 90-minute feed no request reads, installed first: the requested
    // 75-minute feed is routed to the second slot of the store.
    std::vector<Bar> unread;
    for (int minute = 0; minute < 360; minute += 90) {
        const double value = 700.5 + minute;
        unread.push_back({value - 1, value + 2, value - 2, value, 9.0,
                          kOriginMs + minute * kMinuteMs});
    }
    FeedBoundProbe host;
    FEED_BOUND_CHECK(host.set_native_security_feed("90", unread.data(),
                                                   static_cast<int>(unread.size())));
    FEED_BOUND_CHECK(host.set_native_security_feed("75", rows.data(),
                                                   static_cast<int>(rows.size())));
    host.run(chart.data(), static_cast<int>(chart.size()), "60", "60", false, 4,
             MagnifierDistribution::ENDPOINTS);
    const std::string error = host.last_error();
    const bool refused =
        error.find("native security feeds changed during the run") != std::string::npos;
    std::printf("FEEDBOUND removed=%d index=%d feeds=%zu evaluated=%d refused=%d\n",
                host.removed ? 1 : 0, host.index_after_removal, host.feeds_after_removal,
                host.evaluated, refused ? 1 : 0);
    // Accepted during the run, the removal leaves the request's routing index
    // one past the store once the first requested bar was published.
    FEED_BOUND_CHECK(host.removed);
    FEED_BOUND_CHECK(host.index_after_removal == 1 && host.feeds_after_removal == 1);
    FEED_BOUND_CHECK(host.evaluated == 1);
    // The next selected period reaches the read with that index.
    FEED_BOUND_CHECK(error.find("native security feeds changed during the run") != std::string::npos);
}
#undef FEED_BOUND_CHECK

// The store holds 90, 75 and 120 minutes, installed in that order, so the
// request reads the 75-minute feed through the second slot. The script
// callback removes the 90-minute feed during the run, which moves the
// 120-minute feed into that slot, inside the store: the next selected period
// must refuse the run and never publish the 120-minute feed's bars. The
// control run changes nothing and publishes the supplied rows themselves.
#define FEED_SHIFT_CHECK(condition) do { if (!(condition)) { \
    ++fixed_failures; std::fprintf(stderr, "feed-shift line %d: %s\n", \
                                 __LINE__, #condition); } } while (false)

class FeedShiftProbe final : public pineforge::source::PineStrategyHost {
public:
    bool remove_ninety = false;
    bool removed = false;
    int index_after_removal = -2;
    std::size_t feeds_after_removal = 0;
    std::string slot_after_removal = "-";
    std::vector<Bar> published_bars;
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "75", input_tf_, true, false);
    }
    void evaluate_security(int, const Bar& bar, bool) override { published_bars.push_back(bar); }
    void clear_security(int) override {}
    void on_source_bar(const Bar&) override {
        // As in the feed-bound case: after the second chart bar.
        if (!remove_ninety || bar_index_ != 1 || removed) return;
        removed = set_native_security_feed("90", nullptr, 0);
        if (security_eval_states_.size() != 1) return;
        index_after_removal = security_eval_states_[0].native_feed_index;
        feeds_after_removal = native_security_feeds_.size();
        // The timeframe of the feed the request's routing slot now holds.
        if (index_after_removal >= 0
            && static_cast<std::size_t>(index_after_removal) < feeds_after_removal)
            slot_after_removal =
                native_security_feeds_[static_cast<std::size_t>(index_after_removal)].tf;
    }
};

void test_native_fixed_period_feed_shifted_during_run() {
    using namespace tv_fixed_period_75_fact;
    const auto chart = native_fixed_period_fixture::input(60, 360);
    const auto rows = native_fixed_period_fixture::native();
    // Two feeds no request reads, on their own grids, with prices of their own.
    const auto unread = [](int step) {
        std::vector<Bar> bars;
        for (int minute = 0; minute < 360; minute += step) {
            const double value = 600.5 + step + minute;
            bars.push_back({value - 1, value + 2, value - 2, value, 9.0,
                            kOriginMs + minute * kMinuteMs});
        }
        return bars;
    };
    const auto ninety = unread(90);
    const auto hundred_twenty = unread(120);
    // The supplied rows that open by the last chart bar: a run that reads its
    // own feed publishes at least these, in order.
    std::size_t expected = 0;
    while (expected < rows.size() && rows[expected].timestamp <= chart.back().timestamp)
        ++expected;
    for (const bool shifted : {false, true}) {
        FeedShiftProbe host;
        host.remove_ninety = shifted;
        FEED_SHIFT_CHECK(host.set_native_security_feed("90", ninety.data(),
                                                       static_cast<int>(ninety.size())));
        FEED_SHIFT_CHECK(host.set_native_security_feed("75", rows.data(),
                                                       static_cast<int>(rows.size())));
        FEED_SHIFT_CHECK(host.set_native_security_feed("120", hundred_twenty.data(),
                                                       static_cast<int>(hundred_twenty.size())));
        host.run(chart.data(), static_cast<int>(chart.size()), "60", "60", false, 4,
                 MagnifierDistribution::ENDPOINTS);
        const std::string error = host.last_error();
        const bool refused =
            error.find("native security feeds changed during the run") != std::string::npos;
        // The published bars that are the supplied rows, in order, each with
        // its own open time and its own close.
        std::size_t matched = 0;
        while (matched < host.published_bars.size() && matched < rows.size()
               && host.published_bars[matched].timestamp == rows[matched].timestamp
               && host.published_bars[matched].close == rows[matched].close)
            ++matched;
        std::printf("FEEDSHIFT run=%s removed=%d index=%d feeds=%zu slot=%s published=%zu "
                    "matched=%zu refused=%d bars=",
                    shifted ? "shift" : "control", host.removed ? 1 : 0,
                    host.index_after_removal, host.feeds_after_removal,
                    host.slot_after_removal.c_str(), host.published_bars.size(), matched,
                    refused ? 1 : 0);
        for (const Bar& bar : host.published_bars)
            std::printf("%lld:%.17g;", static_cast<long long>(bar.timestamp), bar.close);
        std::printf("\n");
        if (!shifted) {
            // Nothing changed: the run publishes the supplied rows themselves.
            FEED_SHIFT_CHECK(error.empty());
            FEED_SHIFT_CHECK(matched == host.published_bars.size() && matched >= expected);
            continue;
        }
        // The removal leaves the request's slot inside the store, on the
        // 120-minute feed.
        FEED_SHIFT_CHECK(host.removed);
        FEED_SHIFT_CHECK(host.index_after_removal == 1 && host.feeds_after_removal == 2);
        FEED_SHIFT_CHECK(host.slot_after_removal == "120");
        // Every bar the run published is a supplied row, and the run is refused.
        FEED_SHIFT_CHECK(matched == host.published_bars.size());
        FEED_SHIFT_CHECK(error.find("native security feeds changed during the run") != std::string::npos);
    }
}

void test_native_fixed_lookahead_missing_row() {
    using tv_fixed_period_75_fact::kOriginMs;
    constexpr int chart_minutes = 60;
    constexpr int fine_minutes = 5;
    constexpr int requested_minutes = 75;
    const auto missing_open = kOriginMs + requested_minutes * kMinute;
    const auto next_open = missing_open + requested_minutes * kMinute;
    const auto chart = native_fixed_period_fixture::input(chart_minutes, 4 * chart_minutes);
    const auto dense = native_fixed_period_fixture::input(fine_minutes, 4 * chart_minutes);
    const std::vector<Bar> native{
        {800, 820, 790, 810, 42, kOriginMs},
        {900, 920, 890, 910, 84, next_open},
    };
    const auto in_missing_period = [=](const Bar& bar) {
        return missing_open <= bar.timestamp && bar.timestamp < next_open;
    };
    auto sparse = dense;
    sparse.erase(std::remove_if(sparse.begin(), sparse.end(), in_missing_period),
                 sparse.end());

    // Independently roll up the real inputs of the missing period. Its first
    // children belong to the preceding chart bar and the chart selecting it
    // also contains children from the next requested period.
    Bar aggregate{};
    int contributors = 0;
    for (const Bar& child : dense) {
        if (!in_missing_period(child)) continue;
        if (contributors++ == 0) aggregate = child;
        else {
            aggregate.high = std::max(aggregate.high, child.high);
            aggregate.low = std::min(aggregate.low, child.low);
            aggregate.close = child.close;
            aggregate.volume += child.volume;
        }
    }
    FIXED_CHECK(contributors == requested_minutes / fine_minutes);
    const auto matches = [](const Bar& actual, const Bar& expected) {
        return actual.timestamp == expected.timestamp && actual.open == expected.open
            && actual.high == expected.high && actual.low == expected.low
            && actual.close == expected.close && actual.volume == expected.volume;
    };
    const auto run = [&](FixedPeriodProbe& host, const std::vector<Bar>& aux, bool gaps) {
        host.requested = std::to_string(requested_minutes);
        host.ahead = true;
        host.gaps = gaps;
        install_fixed_native(host, native);
        FIXED_CHECK(strategy_set_aux_security_feed(
            static_cast<pf_strategy_t>(&host),
            reinterpret_cast<const pf_bar_t*>(aux.data()), static_cast<int>(aux.size()),
            std::to_string(fine_minutes).c_str()) == 0);
        const auto chart_tf = std::to_string(chart_minutes);
        host.run(chart.data(), static_cast<int>(chart.size()), chart_tf, chart_tf);
        FIXED_CHECK(host.last_error().empty());
        FIXED_CHECK(host.native_security_substitutions()
                    == static_cast<std::int64_t>(native.size()));
        FIXED_CHECK(host.reads.size() == chart.size());
    };

    for (const bool gaps : {false, true}) {
        for (const bool observed : {false, true}) {
            FixedPeriodProbe host;
            run(host, observed ? dense : sparse, gaps);
            FIXED_CHECK(host.native_security_misses() == (observed ? 1 : 0));
            if (host.reads.size() != chart.size()) continue;

            const auto& selected = host.reads[2];
            FIXED_CHECK(selected.present == (observed || !gaps));
            if (selected.present)
                FIXED_CHECK(matches(selected.bar, observed ? aggregate : native.front()));
            FIXED_CHECK(host.reads.front().present
                        && matches(host.reads.front().bar, native.front()));
            FIXED_CHECK(host.reads[1].present == !gaps);
            if (host.reads[1].present)
                FIXED_CHECK(matches(host.reads[1].bar, native.front()));
            FIXED_CHECK(host.reads.back().present
                        && matches(host.reads.back().bar, native.back()));

            // Missing periods never allocate a history slot without inputs,
            // and the next native row cannot be evaluated one chart early.
            std::vector<Bar> completed;
            for (std::size_t i = 0; i < host.evaluated_bars.size(); ++i) {
                FIXED_CHECK(host.deliveries[i].requested_slot
                            == static_cast<std::int64_t>(completed.size()));
                if (host.deliveries[i].actual_complete)
                    completed.push_back(host.evaluated_bars[i]);
                FIXED_CHECK(observed || !in_missing_period(host.evaluated_bars[i]));
            }
            FIXED_CHECK(completed.size() == native.size() + (observed ? 1 : 0));
            if (completed.size() == native.size() + (observed ? 1 : 0)) {
                FIXED_CHECK(matches(completed.front(), native.front()));
                if (observed) FIXED_CHECK(matches(completed[1], aggregate));
                FIXED_CHECK(matches(completed.back(), native.back()));
            }
        }
    }

    // In projection mode the chart itself is the source of the fallback.
    // There is one chart child in this period, and no finer input opens there.
    FixedPeriodProbe projected;
    projected.set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
    run(projected, sparse, false);
    if (projected.reads.size() == chart.size()) {
        FIXED_CHECK(projected.reads[2].present
                    && matches(projected.reads[2].bar, chart[2]));
        FIXED_CHECK(projected.reads.back().present
                    && matches(projected.reads.back().bar, native.back()));
    }
}
#undef FEED_SHIFT_CHECK
#undef REPORTED_QUANTITY_CHECK
#undef FIXED_CHECK
} // namespace

int main() {
    test_completed_daily_bucket_carries_the_native_bar();
    test_a_bucket_without_a_native_bar_keeps_its_aggregate();
    test_without_a_native_feed_the_run_is_the_aggregate();
    test_feed_validation_fails_closed();
    test_split_aux_feed_path_substitutes_the_same_native_bar();
    test_overnight_cme_session_labels_by_session_day();
    test_tuple_request_reads_the_native_daily_row();
    test_tuple_request_without_the_feed_is_the_aggregate();
    test_native_fixed_period_retained_source_replays();
    test_native_fixed_period_sparse_chart_attribution();
    test_native_fixed_period_tick_admission_and_reset();
    test_native_fixed_period_unbacked_and_divisible_controls();
    test_native_fixed_period_grid_and_session_controls();
    test_native_fixed_period_provider_label_inside_a_break();
    test_native_fixed_period_nominal_label_of_a_clipped_slot();
    test_native_fixed_period_two_inputs_inside_a_break();
    test_native_fixed_period_break_input_is_not_folded_into_the_next_bucket();
    test_native_fixed_period_first_label_in_a_break_on_an_aggregated_chart();
    test_native_fixed_period_first_label_in_a_break_on_the_input_timeframe();
    test_native_fixed_period_first_label_in_the_second_break_of_a_day();
    test_native_fixed_period_exchange_early_close();
    test_native_fixed_period_early_close_gate();
    test_native_fixed_period_session_hole_is_not_a_close();
    test_native_divisible_period_early_close_is_kept();
    test_inputs_after_a_chart_bar_calculated_ahead_reach_every_site();
    test_native_fixed_period_reported_entry_quantities();
    test_native_fixed_period_feed_removed_during_run();
    test_native_fixed_period_feed_shifted_during_run();
    test_native_fixed_lookahead_missing_row();
    return fixed_failures == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
