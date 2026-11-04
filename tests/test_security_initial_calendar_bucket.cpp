// Public synthetic inputs only. Expected values are arithmetic on the input
// candles, independent of the aggregator and calendar implementations.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;
int checks = 0;
int rows = 0;
constexpr std::int64_t quarter = 900'000;
constexpr std::int64_t day = 86'400'000;
// 2025-04-01 00:00 UTC, a Tuesday.
constexpr std::int64_t tuesday = 1'743'465'600'000;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

bool same(double actual, double expected) {
    return (std::isnan(actual) && std::isnan(expected)) || actual == expected;
}

void value(double actual, double expected, const char* label) {
    ++checks;
    if (!same(actual, expected)) {
        ++failures;
        std::printf("FAIL: %s: got %.17g, expected %.17g\n",
                    label, actual, expected);
    }
}

struct Observation {
    double delta = missing;
    double previous = missing;
    double mean = missing;
    double index = missing;
    int calls = 0;
    int completed = 0;
    std::size_t slots = 0;
    int inputs = 0;
    bool aggregating = false;
};

class Probe final : public source::PineStrategyHost {
public:
    explicit Probe(const char* tf, bool lookahead = false, bool gaps = false)
        : tf_(tf), lookahead_(lookahead), gaps_(gaps) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        samples.clear();
        chart.clear();
        current = Observation{};
        mean_ = ta::SMA(2);
        routed = false;
        register_security_eval(0, tf_, input_tf_, lookahead_, gaps_);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool complete) override {
        check(sec_id == 0, "requested evaluator identity");
        check(ta::bar_context().installed, "requested TA context installed");
        const double delta = bar.close - bar.open;
        const bool fresh = security_series_slot_is_new(sec_id);
        if (fresh) {
            samples.push_back(delta);
        } else {
            check(!samples.empty(), "recompute has an existing requested slot");
            if (samples.empty()) return;
            samples.back() = delta;
        }
        current.delta = delta;
        current.previous = samples.size() < 2
            ? missing : samples[samples.size() - 2];
        current.mean = fresh ? mean_.compute(delta) : mean_.recompute(delta);
        current.index = static_cast<double>(ta::bar_context().bar_index);
        ++current.calls;
        if (complete) ++current.completed;
        current.slots = samples.size();
    }

    void clear_security(int) override {
        current.delta = missing;
        current.previous = missing;
        current.mean = missing;
        current.index = missing;
    }

    void on_source_bar(const Bar&) override {
        auto observation = current;
        for (const auto& site : security_eval_states_) {
            if (site.sec_id != 0) continue;
            observation.inputs = site.feed_count;
            observation.aggregating = site.aggregator.is_active();
        }
        chart.push_back(observation);
        const auto state = native_state();
        if (state.spec && !state.spec->subscriptions.empty()) routed = true;
    }

    std::vector<double> samples;
    std::vector<Observation> chart;
    Observation current;
    bool routed = false;

private:
    std::string tf_;
    bool lookahead_;
    bool gaps_;
    ta::SMA mean_{2};
};

void append(std::vector<Bar>& bars, std::int64_t begin, std::int64_t end,
            double open, double final_close) {
    for (auto stamp = begin; stamp < end; stamp += quarter) {
        const double close = stamp + quarter == end ? final_close : open;
        bars.push_back(Bar{open, std::max(open, close) + 1,
                           std::min(open, close) - 1, close, 1, stamp});
    }
}

bool valid(const std::vector<Bar>& bars) {
    bool ok = !bars.empty();
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        ok = ok && std::isfinite(bar.open) && std::isfinite(bar.close)
            && std::isfinite(bar.high) && std::isfinite(bar.low)
            && bar.low <= std::min(bar.open, bar.close)
            && bar.high >= std::max(bar.open, bar.close) && bar.volume > 0;
        if (i) ok = ok && bar.timestamp - bars[i - 1].timestamp == quarter;
    }
    check(ok, "fixture has valid OHLC and dense UTC quarter-hour timestamps");
    return ok;
}

void run(Probe& probe, const std::vector<Bar>& bars) {
    if (!valid(bars)) return;
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    check(probe.last_error().empty(), "batch setup succeeds");
    check(probe.chart.size() == bars.size(), "all chart rows are populated");
}

void state(const Observation& actual, double delta, double previous, double mean,
           double index, int calls, int completed, std::size_t slots) {
    value(actual.delta, delta, "requested delta");
    value(actual.previous, previous, "requested one-bar history");
    value(actual.mean, mean, "requested two-sample mean");
    value(actual.index, index, "requested bar index");
    check(actual.calls == calls, "payload call count");
    check(actual.completed == completed, "completed payload count");
    check(actual.slots == slots, "requested history population");
}

std::vector<Bar> weekly_feed() {
    std::vector<Bar> bars;
    append(bars, tuesday, tuesday + 6 * day, 100, 103);
    append(bars, tuesday + 6 * day, tuesday + 13 * day, 200, 197);
    append(bars, tuesday + 13 * day, tuesday + 20 * day, 300, 301);
    append(bars, tuesday + 20 * day, tuesday + 20 * day + quarter, 400, 400);
    return bars;
}

void clipped_week(const char* tf, const char* session) {
    ++rows;
    Probe probe(tf);
    probe.set_syminfo_session(session);
    const auto bars = weekly_feed();
    run(probe, bars);
    if (probe.chart.size() != 1921) return;
    check(!probe.routed, "clipped calendar site stays on the source host");
    // The six-day prefix never evaluates or seeds requested history/TA.
    for (std::size_t i = 0; i < 1247; ++i) {
        state(probe.chart[i], missing, missing, missing, missing, 0, 0, 0);
        if (i < 576) {
            check(probe.chart[i].inputs == 0, "clipped children never enter evaluator feed");
            check(!probe.chart[i].aggregating, "clipped children never start aggregation");
        }
    }
    // 197 - 200 = -3 is the first retained sample, at requested index 0.
    state(probe.chart[1247], -3, missing, missing, 0, 1, 1, 1);
    state(probe.chart[1918], -3, missing, missing, 0, 1, 1, 1);
    // 301 - 300 = 1; (-3 + 1) / 2 = -1, with no clipped +3 seed.
    state(probe.chart[1919], 1, -3, -1, 1, 2, 2, 2);
    state(probe.chart[1920], 1, -3, -1, 1, 2, 2, 2);
    check(probe.samples.size() == 2, "only the two complete weeks enter history");

    // Running the same host again must not preserve a requested sample.
    run(probe, bars);
    if (probe.chart.size() == 1921)
        state(probe.chart[1247], -3, missing, missing, 0, 1, 1, 1);
}

void calendar_controls() {
    struct Control {
        const char* tf;
        std::int64_t begin;
        std::int64_t end;
    };
    const Control controls[] = {
        {"W", tuesday - day, tuesday + 6 * day},       // aligned Monday
        {"D", tuesday, tuesday + day},                // aligned midnight
        {"M", tuesday, tuesday + 30 * day},           // aligned April
        {"60", tuesday + quarter, tuesday + 4 * quarter}, // clipped intraday
    };
    for (const auto& control : controls) {
        ++rows;
        std::vector<Bar> bars;
        append(bars, control.begin, control.end, 20, 23);
        append(bars, control.end, control.end + quarter, 30, 30);
        Probe probe(control.tf);
        run(probe, bars);
        if (probe.chart.size() != bars.size()) continue;
        // Every retained aggregate is 23 - 20 = 3, including the first week.
        state(probe.chart[bars.size() - 2], 3, missing, missing, 0, 1, 1, 1);
    }

    // Neither a clipped D nor a clipped M may leak an extra +3 sample.
    for (const char* tf : {"D", "M"}) {
        ++rows;
        const bool monthly = std::string(tf) == "M";
        const auto begin = monthly ? tuesday + day : tuesday + 12 * quarter;
        const auto boundary = monthly ? tuesday + 30 * day : tuesday + day;
        const auto end = monthly ? boundary + 31 * day : boundary + day;
        std::vector<Bar> bars;
        append(bars, begin, boundary, 100, 103);
        append(bars, boundary, end, 200, 197);
        append(bars, end, end + quarter, 300, 300);
        Probe probe(tf);
        run(probe, bars);
        if (probe.chart.size() == bars.size()) {
            state(probe.chart[bars.size() - 2], -3, missing, missing, 0, 1, 1, 1);
            check(!probe.routed, "clipped day/month requires source cut");
        }
    }

    ++rows;
    // A weekday mask cannot prove that Monday traded. Keep existing handling
    // of a holiday-open Tuesday, with no newly inferred first-week cut.
    Probe holiday("W");
    holiday.set_syminfo_session("0000-0000:23456");
    std::vector<Bar> holiday_bars;
    append(holiday_bars, tuesday, tuesday + 4 * day, 100, 103);
    append(holiday_bars, tuesday + 6 * day, tuesday + 6 * day + quarter, 200, 200);
    // This control deliberately has a closed weekend.
    holiday.run(holiday_bars.data(), static_cast<int>(holiday_bars.size()), "15", "15");
    check(holiday.last_error().empty(), "noncontinuous calendar setup succeeds");
    check(holiday.chart.size() == holiday_bars.size(), "noncontinuous chart population");
    check(holiday.samples.size() == 1, "noncontinuous initial week retained");
    if (!holiday.samples.empty()) value(holiday.samples.front(), 3, "holiday week delta");
}

std::vector<Bar> hourly_feed() {
    const double opens[] = {10, 11, 13, 14, 8, 9, 6, 11};
    const double closes[] = {11, 13, 14, 8, 9, 6, 11, 13};
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) {
        bars.push_back(Bar{opens[i], std::max(opens[i], closes[i]) + 1,
                           std::min(opens[i], closes[i]) - 1, closes[i], 1,
                           tuesday + static_cast<std::int64_t>(i) * quarter});
    }
    return bars;
}

void projection_row(const char* tf, bool lookahead, int selector,
                    const std::vector<double>& expected, int calls) {
    ++rows;
    Probe probe(tf, lookahead);
    if (selector >= 0)
        probe.set_syminfo_metadata("historical_security_lookahead_projection", selector);
    const auto bars = hourly_feed();
    run(probe, bars);
    if (probe.chart.size() != expected.size()) return;
    for (std::size_t i = 0; i < expected.size(); ++i)
        value(probe.chart[i].delta, expected[i], "independent intraday value");
    check(probe.current.calls == calls, "intraday payload dispatch population");
    if (lookahead && calls == 2) {
        for (std::size_t i = 0; i < 4; ++i)
            state(probe.chart[i], -2, missing, missing, 0, 1, 1, 1);
        for (std::size_t i = 4; i < 8; ++i)
            state(probe.chart[i], 5, -2, 1.5, 1, 2, 2, 2);
    }
    if (calls == 2) {
        // The payload history and TA see the two hours once each.
        state(probe.chart[3], -2, missing, missing, 0, 1, 1, 1);
        state(probe.chart[7], 5, -2, 1.5, 1, 2, 2, 2);
    }
}

void projection_controls() {
    // Explicit off keeps each half-hour aggregate developing on every child.
    // Opens 10, 13, 8, 6 give (11, 13)-10, (14, 8)-13,
    // (9, 6)-8, (11, 13)-6, with eight payload dispatches.
    projection_row("30", true, 0, {1, 3, 1, -5, 1, -2, 5, 7}, 8);
    // Explicit on: 8 - 10 = -2 and 13 - 8 = 5, held for four children.
    projection_row("60", true, 1, {-2, -2, -2, -2, 5, 5, 5, 5}, 2);
    // Explicit on for half-hours: 13-10, 8-13, 6-8, 13-6.
    projection_row("30", true, 1, {3, 3, -5, -5, -2, -2, 7, 7}, 4);
    // Lookahead off remains completion-only with projection enabled.
    projection_row("60", false, 1,
                   {missing, missing, missing, -2, -2, -2, -2, 5}, 2);
    // Equal chart/request timeframes keep each candle's close - open.
    projection_row("15", true, 1, {1, 2, 1, -6, 1, -3, 5, 2}, 8);
    // Explicit off: hourly opens 10 and 8 retain developing values.
    projection_row("60", true, 0, {1, 3, 4, -2, 1, -2, 3, 5}, 8);
}

void phase_controls() {
    ++rows;
    Probe live("60", true);
    const auto bars = hourly_feed();
    check(valid(bars), "stream fixture valid");
    const bool begun = live.stream_begin(bars.data(), 4, "15", "15");
    check(begun, "stream warmup setup succeeds");
    if (!begun) return;
    check(live.chart.size() == 4, "all warmup chart rows populated");
    if (live.chart.size() == 4) {
        const double expected[] = {1, 3, 4, -2};
        for (std::size_t i = 0; i < 4; ++i)
            value(live.chart[i].delta, expected[i], "warmup stays developing");
    }
    // Ticks alone form each new bar: no future candle is supplied to the
    // stream. The second hour develops from 9 to 6 against its open of 9.
    check(live.stream_push_tick(TradeTick{bars[4].timestamp, 1, 9, 1}),
          "first realtime tick");
    check(live.stream_advance_time(bars[5].timestamp), "first realtime close");
    value(live.current.delta, 0, "first live delta is 9 - 9");
    check(live.stream_push_tick(TradeTick{bars[5].timestamp, 2, 6, 1}),
          "second realtime tick");
    check(live.stream_advance_time(bars[6].timestamp), "second realtime close");
    value(live.current.delta, -3, "second live delta is 6 - 9");
    check(live.current.calls == 6, "one warmup/live dispatch per input");
    check(live.stream_end(), "intraday stream ends");

    ++rows;
    Probe weekly("W");
    const auto week = weekly_feed();
    // stream_begin returns with the native phase already Realtime; its
    // warmup must retain the initial +3 sample instead of taking a Batch cut.
    const bool weekly_begun = weekly.stream_begin(week.data(), 576, "15", "15");
    check(weekly_begun, "weekly stream warmup setup succeeds");
    if (!weekly_begun) return;
    value(weekly.current.delta, 3, "warmup clipped week is retained");
    check(weekly.current.completed == 1, "warmup completion retained");
    check(weekly.stream_push_tick(TradeTick{tuesday + 6 * day, 1, 200, 1}),
          "weekly realtime tick");
    check(weekly.stream_advance_time(tuesday + 6 * day + quarter),
          "weekly realtime child closes");
    value(weekly.current.delta, 3, "live partial holds prior completed week");
    check(weekly.current.completed == 1, "Realtime does not recut warmup state");
    check(weekly.stream_end(), "weekly stream ends");
}

} // namespace

int main(int argc, char** argv) {
    const std::string group = argc == 1 ? "all" : argv[1];
    if (argc > 2 || (group != "all" && group != "calendar"
                    && group != "projection" && group != "phases")) {
        std::printf("usage: %s [all|calendar|projection|phases]\n", argv[0]);
        return 2;
    }
    if (group == "all" || group == "calendar") {
        clipped_week("W", "24x7");
        clipped_week("1W", "");
        clipped_week("W", "0000-0000:1234567");
        calendar_controls();
    }
    if (group == "all" || group == "projection") projection_controls();
    if (group == "all" || group == "phases") phase_controls();
    const int expected_rows = group == "all" ? 18
        : group == "calendar" ? 10 : group == "projection" ? 6 : 2;
    check(rows == expected_rows, "every selected scenario row executed");
    std::printf("security calendar/projection: rows=%d checks=%d failures=%d\n",
                rows, checks, failures);
    return failures == 0 ? 0 : 1;
}
