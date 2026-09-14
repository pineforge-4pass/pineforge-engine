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
#include <cstdint>
#include <string>
#include <vector>

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

// One NYSE-style session (0930-1600 America/New_York) of 15m bars: 26 bars
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

constexpr int64_t kNyDay1 = 1704205800000;  // 2024-01-02 09:30 America/New_York
constexpr int64_t kNyDay2 = kNyDay1 + kDay;

void test_completed_daily_bucket_carries_the_native_bar() {
    std::vector<Bar> chart = ny_session(0, kNyDay1);
    const std::vector<Bar> day2 = ny_session(1, kNyDay2);
    chart.insert(chart.end(), day2.begin(), day2.end());
    // TradingView's own daily bars: the official close differs from the last
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
    constexpr int64_t open1 = 1704236400000;  // 2024-01-02 17:00 America/Chicago
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
// Round 7 family K (mukhlisilahi universal-backtest-pro NYSE:F@15, campaign
// note log-20260905t084530z-66c3f27e): the probe's chart EMAs run on the
// CLOSE element of a five-field tuple request whose timeframe is an
// input.timeframe("D") variable. The codegen lowers the tuple body to one
// evaluator that assigns every element from the completed bucket
// (`_req_sec_0_3 = bar.close`), so the substitution the scalar path gets in
// feed_security_input applies to it unchanged — pinned here on the registry
// NYSE:F 15m bars of 2025-04-22 / 04-23 (feed 80f404ae85ef) against the
// exchange's own 1D rows (feed e3dd3a88e85b): 04-22 closes 9.65 where the
// last 15m print is 9.655; 04-23 is 9.835 / 10.0054 / 9.71 / 9.78 /
// 158,691,527 where the aggregate reads 9.83 / 10.00 / 9.715 / 9.765.
// (The probe's own runs never had the feed: the harness routes the native
// daily candidate only for a LITERAL "D"/"1D" timeframe argument —
// pineforge-lab verify_routing.py executable_uses_daily_security — so an
// input-bound timeframe kept the aggregate. That is the harness's fix.)
namespace {

constexpr int64_t kApr22_1330Z = 1745328600000LL;  // 2025-04-22 13:30Z
constexpr int64_t kApr23_1330Z = 1745415000000LL;  // 2025-04-23 13:30Z

Bar f15(double o, double h, double l, double c, double v) {
    return Bar{o, h, l, c, v, 0};
}

// NYSE:F 15m, the two full sessions (26 bars each), registry prints.
std::vector<Bar> f15_apr22_apr23() {
    std::vector<Bar> d1 = {
        f15(9.55, 9.635, 9.53, 9.605, 427205), f15(9.605, 9.63, 9.59, 9.59, 219496),
        f15(9.59, 9.595, 9.55, 9.56, 218153), f15(9.56, 9.595, 9.54, 9.57, 306707),
        f15(9.56, 9.62, 9.56, 9.615, 155463), f15(9.61, 9.615, 9.58, 9.605, 174014),
        f15(9.61, 9.67, 9.61, 9.665, 283781), f15(9.67, 9.67, 9.625, 9.63, 119196),
        f15(9.635, 9.66, 9.61, 9.615, 109087), f15(9.615, 9.66, 9.61, 9.66, 186877),
        f15(9.655, 9.7, 9.655, 9.675, 333614), f15(9.67, 9.71, 9.65, 9.695, 239144),
        f15(9.69, 9.72, 9.69, 9.705, 168025), f15(9.705, 9.705, 9.68, 9.7, 220522),
        f15(9.69, 9.695, 9.63, 9.63, 287554), f15(9.635, 9.64, 9.535, 9.545, 414666),
        f15(9.55, 9.6, 9.53, 9.6, 175623), f15(9.605, 9.605, 9.565, 9.565, 75129),
        f15(9.56, 9.62, 9.56, 9.615, 112723), f15(9.61, 9.64, 9.61, 9.63, 158776),
        f15(9.625, 9.64, 9.62, 9.62, 86466), f15(9.62, 9.65, 9.62, 9.65, 122109),
        f15(9.64, 9.65, 9.63, 9.63, 64055), f15(9.64, 9.64, 9.605, 9.615, 166832),
        f15(9.61, 9.665, 9.61, 9.66, 220974), f15(9.65, 9.67, 9.62, 9.655, 672067),
    };
    std::vector<Bar> d2 = {
        f15(9.83, 9.93, 9.81, 9.9, 914063), f15(9.895, 10, 9.86, 9.895, 1019468),
        f15(9.9, 9.955, 9.85, 9.895, 704662), f15(9.9, 9.92, 9.85, 9.9, 185224),
        f15(9.9, 9.92, 9.86, 9.89, 235711), f15(9.89, 9.895, 9.86, 9.88, 153999),
        f15(9.875, 9.9, 9.825, 9.9, 417687), f15(9.895, 9.895, 9.81, 9.83, 485316),
        f15(9.835, 9.87, 9.77, 9.835, 363176), f15(9.84, 9.845, 9.75, 9.785, 412994),
        f15(9.785, 9.8, 9.755, 9.77, 218683), f15(9.775, 9.775, 9.715, 9.765, 164273),
        f15(9.77, 9.835, 9.765, 9.83, 307265), f15(9.82, 9.835, 9.8, 9.835, 257656),
        f15(9.83, 9.87, 9.77, 9.865, 357318), f15(9.865, 9.885, 9.855, 9.86, 193816),
        f15(9.86, 9.865, 9.81, 9.84, 155121), f15(9.845, 9.89, 9.835, 9.88, 280372),
        f15(9.875, 9.9, 9.85, 9.88, 248374), f15(9.88, 9.895, 9.83, 9.835, 146731),
        f15(9.84, 9.87, 9.83, 9.83, 91255), f15(9.835, 9.84, 9.81, 9.81, 53131),
        f15(9.815, 9.835, 9.8, 9.81, 116294), f15(9.81, 9.81, 9.75, 9.755, 165863),
        f15(9.76, 9.785, 9.755, 9.77, 222441), f15(9.77, 9.795, 9.755, 9.765, 536554),
    };
    std::vector<Bar> chart;
    for (std::size_t i = 0; i < d1.size(); ++i) {
        d1[i].timestamp = kApr22_1330Z + static_cast<int64_t>(i) * kQuarter;
        chart.push_back(d1[i]);
    }
    for (std::size_t i = 0; i < d2.size(); ++i) {
        d2[i].timestamp = kApr23_1330Z + static_cast<int64_t>(i) * kQuarter;
        chart.push_back(d2[i]);
    }
    return chart;
}

// TradingView's own NYSE:F daily rows for the two sessions.
const Bar kNativeApr22{9.55, 9.72, 9.53, 9.65, 121387081.0, kApr22_1330Z};
const Bar kNativeApr23{9.835, 10.0054, 9.71, 9.78, 158691527.0, kApr23_1330Z};

// The generated strategy's shape for `[o, h, l, c, v] = request.security(
// syminfo.tickerid, tf_input, [open, high, low, close, volume])`: one
// evaluator assigning the five members from the bucket it is handed, the
// timeframe string an input value ("D" is input.timeframe's default; "1D"
// is what an exported inputs.json spells).
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
        std::vector<Bar> chart = f15_apr22_apr23();
        const Bar daily[] = {kNativeApr22, kNativeApr23};
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
        // na until the first daily completion (the 04-22 19:45Z bar).
        assert(std::isnan(probe.at_chart_close[24].c));
        // Every element of the tuple is the native row, verbatim: the
        // sub-penny close 9.65 (aggregate 9.655), and on 04-23 the
        // 10.0054 high / 9.835 open / 9.71 low / 9.78 close / the exchange
        // volume (aggregate 9.83 / 10.0 / 9.715 / 9.765).
        assert(row_is(probe.at_chart_close[25], kNativeApr22));
        assert(row_is(probe.at_chart_close[26], kNativeApr22));   // held
        assert(row_is(probe.at_chart_close[50], kNativeApr22));
        assert(row_is(probe.at_chart_close[51], kNativeApr23));
        assert(probe.at_chart_close[51].h == 10.0054);
        assert(probe.at_chart_close[51].c == 9.78);
        assert(probe.native_security_substitutions() == 2);
        assert(probe.native_security_misses() == 0);
    }
}

void test_tuple_request_without_the_feed_is_the_aggregate() {
    // The same run with no native feed — the shape the mukhlisilahi F@15
    // case actually ran under — reads the 15m aggregate: 04-22 close 9.655,
    // 04-23 9.83 / 10.0 / 9.715 / 9.765 and the summed volume.
    std::vector<Bar> chart = f15_apr22_apr23();
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
    assert(d1.o == 9.55 && d1.h == 9.72 && d1.l == 9.53 && d1.c == 9.655);
    const TupleProbe::Row& d2 = probe.at_chart_close[51];
    assert(d2.o == 9.83 && d2.h == 10.0 && d2.l == 9.715 && d2.c == 9.765);
    double vol = 0.0;
    for (std::size_t i = 26; i < 52; ++i) vol += chart[i].volume;
    assert(d2.v == vol);
    assert(probe.native_security_substitutions() == 0);
}

int main() {
    test_completed_daily_bucket_carries_the_native_bar();
    test_a_bucket_without_a_native_bar_keeps_its_aggregate();
    test_without_a_native_feed_the_run_is_the_aggregate();
    test_feed_validation_fails_closed();
    test_split_aux_feed_path_substitutes_the_same_native_bar();
    test_overnight_cme_session_labels_by_session_day();
    test_tuple_request_reads_the_native_daily_row();
    test_tuple_request_without_the_feed_is_the_aggregate();
    return 0;
}
