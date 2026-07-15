// Pins the default-off historical batch projection for regular HTF
// request.security(..., gaps_off, lookahead_on) sites.

#include <pineforge/engine.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond, tag) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", (tag), __LINE__); \
        ++failures; \
    } \
} while (0)

bool same(double lhs, double rhs) {
    if (is_na(lhs) && is_na(rhs)) return true;
    if (is_na(lhs) || is_na(rhs)) return false;
    return lhs == rhs;
}

struct Dispatch {
    Bar bar;
    bool complete;
};

class ProjectionHarness final : public BacktestEngine {
public:
    explicit ProjectionHarness(bool lookahead_on = true, bool gaps_on = false,
                               const char* requested_tf = "60",
                               bool heikinashi = false) {
        register_security_eval(0, requested_tf, "15", lookahead_on, gaps_on,
                               heikinashi);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        CHECK(sec_id == 0, "security id");
        dispatches.push_back(Dispatch{bar, is_complete});
        visible_close = bar.close;
    }

    void on_bar(const Bar&) override {
        chart_values.push_back(visible_close);
    }

    std::vector<Dispatch> dispatches;
    std::vector<double> chart_values;
    double visible_close = na<double>();
};

std::vector<Bar> make_feed() {
    // One complete 60m bucket (01:00..01:45) and one incomplete tail
    // (02:00..02:15), both composed from 15m chart bars.
    return {
        Bar{10.0, 11.0,  9.0, 10.0, 1.0,  3'600'000},
        Bar{10.0, 22.0,  8.0, 20.0, 2.0,  4'500'000},
        Bar{20.0, 33.0,  7.0, 30.0, 3.0,  5'400'000},
        Bar{30.0, 44.0,  6.0, 40.0, 4.0,  6'300'000},
        Bar{40.0, 55.0, 35.0, 50.0, 5.0,  7'200'000},
        Bar{50.0, 66.0, 34.0, 60.0, 6.0,  8'100'000},
    };
}

std::vector<Bar> make_5m_feed() {
    std::vector<Bar> bars;
    bars.reserve(12);
    for (int i = 0; i < 12; ++i) {
        const double close = static_cast<double>(i + 1);
        const int64_t timestamp = 3'600'000
            + static_cast<int64_t>(i) * 300'000;
        bars.push_back(Bar{close, close, close, close, 1.0, timestamp});
    }
    return bars;
}

void test_default_remains_progressive() {
    ProjectionHarness harness;
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.last_error().empty(), "default run succeeds");
    CHECK(harness.dispatches.size() == 6,
          "default lookahead dispatches every progressive child");
    const double expected[] = {10, 20, 30, 40, 50, 60};
    for (std::size_t i = 0; i < harness.dispatches.size() && i < 6; ++i) {
        CHECK(same(harness.dispatches[i].bar.close, expected[i]),
              "default progressive close sequence");
        CHECK(harness.dispatches[i].complete == (i == 3),
              "default completion cadence");
        CHECK(same(harness.chart_values[i], expected[i]),
              "default chart sees progressive value");
    }
}

void test_flag_projects_full_bucket_then_holds() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.last_error().empty(), "projected run succeeds");
    CHECK(harness.dispatches.size() == 2,
          "one projected dispatch per HTF bucket");
    CHECK(harness.dispatches[0].complete,
          "full historical bucket is committed");
    CHECK(same(harness.dispatches[0].bar.open, 10.0), "projected full open");
    CHECK(same(harness.dispatches[0].bar.high, 44.0), "projected full high");
    CHECK(same(harness.dispatches[0].bar.low, 6.0), "projected full low");
    CHECK(same(harness.dispatches[0].bar.close, 40.0), "projected full close");
    CHECK(same(harness.dispatches[0].bar.volume, 10.0), "projected full volume");

    const double expected_chart[] = {40, 40, 40, 40, 60, 60};
    CHECK(harness.chart_values.size() == 6, "all chart children dispatched");
    for (std::size_t i = 0; i < harness.chart_values.size() && i < 6; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "projection is visible on first child and held");
    }
}

void test_incomplete_tail_projects_available_aggregate() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 2, "tail projection exists");
    if (harness.dispatches.size() == 2) {
        const Dispatch& tail = harness.dispatches[1];
        CHECK(!tail.complete, "incomplete tail does not commit history");
        CHECK(same(tail.bar.open, 40.0), "tail available open");
        CHECK(same(tail.bar.high, 66.0), "tail available high");
        CHECK(same(tail.bar.low, 34.0), "tail available low");
        CHECK(same(tail.bar.close, 60.0), "tail available close");
        CHECK(same(tail.bar.volume, 11.0), "tail available volume");
    }
}

void test_lookahead_off_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/false, /*gaps_on=*/false);
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 1,
          "lookahead_off keeps completion-only behavior");
    CHECK(harness.dispatches.empty() || harness.dispatches[0].complete,
          "lookahead_off dispatch is committed");
    CHECK(harness.dispatches.empty()
              || same(harness.dispatches[0].bar.close, 40.0),
          "lookahead_off completed close unchanged");
    const double expected_chart[] = {
        na<double>(), na<double>(), na<double>(), 40.0, 40.0, 40.0,
    };
    for (std::size_t i = 0; i < harness.chart_values.size() && i < 6; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "lookahead_off chart sequence unchanged");
    }
}

void test_gaps_on_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/true, /*gaps_on=*/true);
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 6,
          "gaps_on keeps progressive lookahead behavior");
    const double expected[] = {10, 20, 30, 40, 50, 60};
    for (std::size_t i = 0; i < harness.dispatches.size() && i < 6; ++i) {
        CHECK(same(harness.dispatches[i].bar.close, expected[i]),
              "gaps_on progressive close sequence unchanged");
    }
}

void test_equal_timeframe_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/true, /*gaps_on=*/false,
                              /*requested_tf=*/"15");
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 6,
          "equal timeframe remains passthrough");
    for (std::size_t i = 0; i < harness.dispatches.size() && i < bars.size(); ++i) {
        CHECK(same(harness.dispatches[i].bar.close, bars[i].close),
              "equal timeframe close unchanged");
        CHECK(harness.dispatches[i].complete,
              "equal timeframe dispatch stays complete");
    }
}

void test_heikinashi_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/true, /*gaps_on=*/false,
                              /*requested_tf=*/"60", /*heikinashi=*/true);
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 6,
          "Heikin-Ashi security remains on its established progressive path");
}

void test_input_tf_below_script_tf_ignores_projection_flag() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_5m_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "5", "15");

    CHECK(harness.last_error().empty(), "5m-to-15m run succeeds");
    CHECK(harness.dispatches.size() == 12,
          "input-to-script aggregation keeps security progressive");
    for (std::size_t i = 0;
         i < harness.dispatches.size() && i < bars.size(); ++i) {
        CHECK(same(harness.dispatches[i].bar.close, bars[i].close),
              "raw input security close remains progressive");
    }

    const double expected_chart[] = {3.0, 6.0, 9.0, 12.0};
    CHECK(harness.chart_values.size() == 4,
          "5m input produces four 15m script bars");
    for (std::size_t i = 0;
         i < harness.chart_values.size() && i < 4; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "15m script sees latest progressive security close");
    }
}

void test_range_start_warmup_composes_with_projection() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    // Drop the first 15m input bar. The finite projection must aggregate only
    // the retained range-start feed: a three-child historical 60m bucket,
    // followed by the available two-child tail. The skipped chart child stays
    // na because neither the range-start evaluator nor the projection has run.
    harness.set_syminfo_metadata(
        "security_range_start_na_warmup", 4'500'000.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.last_error().empty(), "range-start composition run succeeds");
    CHECK(harness.dispatches.size() == 2,
          "range-start feed projects once per retained HTF bucket");
    if (harness.dispatches.size() == 2) {
        const Dispatch& historical = harness.dispatches[0];
        CHECK(historical.complete, "trimmed historical bucket is complete");
        CHECK(same(historical.bar.open, 10.0), "trimmed projection open");
        CHECK(same(historical.bar.high, 44.0), "trimmed projection high");
        CHECK(same(historical.bar.low, 6.0), "trimmed projection low");
        CHECK(same(historical.bar.close, 40.0), "trimmed projection close");
        CHECK(same(historical.bar.volume, 9.0), "trimmed projection volume");

        const Dispatch& tail = harness.dispatches[1];
        CHECK(!tail.complete, "trimmed tail remains incomplete");
        CHECK(same(tail.bar.close, 60.0), "trimmed tail available close");
    }

    const double expected_chart[] = {
        na<double>(), 40.0, 40.0, 40.0, 60.0, 60.0,
    };
    CHECK(harness.chart_values.size() == 6,
          "range-start composition preserves every chart child");
    for (std::size_t i = 0;
         i < harness.chart_values.size() && i < 6; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "range-start projected chart sequence");
    }
}

void test_stream_warmup_and_continuation_stay_progressive() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();

    CHECK(harness.stream_begin(bars.data(), 4, "15", "15"),
          "stream begin succeeds");
    CHECK(harness.dispatches.size() == 4,
          "stream warmup ignores historical projection");
    const double warmup_expected[] = {10, 20, 30, 40};
    for (std::size_t i = 0; i < harness.dispatches.size() && i < 4; ++i) {
        CHECK(same(harness.dispatches[i].bar.close, warmup_expected[i]),
              "stream warmup stays progressive");
    }

    CHECK(harness.stream_push_tick(
              TradeTick{7'200'000, 1, 50.0, 5.0}),
          "first realtime tick accepted");
    CHECK(harness.stream_advance_time(8'100'000),
          "first realtime input bar finalized");
    CHECK(harness.dispatches.size() == 5,
          "realtime continuation dispatches next partial");
    CHECK(same(harness.dispatches.back().bar.close, 50.0),
          "realtime continuation exposes available close");
    CHECK(!harness.dispatches.back().complete,
          "realtime continuation remains partial");

    CHECK(harness.stream_push_tick(
              TradeTick{8'100'000, 2, 60.0, 6.0}),
          "second realtime tick accepted");
    CHECK(harness.stream_advance_time(9'000'000),
          "second realtime input bar finalized");
    CHECK(harness.dispatches.size() == 6,
          "second realtime partial dispatched");
    CHECK(same(harness.dispatches.back().bar.close, 60.0),
          "realtime aggregation advances progressively");
    CHECK(!harness.dispatches.back().complete,
          "second realtime bar is still partial");
    CHECK(harness.stream_end(), "stream ends cleanly");
}

}  // namespace

int main() {
    test_default_remains_progressive();
    test_flag_projects_full_bucket_then_holds();
    test_incomplete_tail_projects_available_aggregate();
    test_lookahead_off_ignores_projection_flag();
    test_gaps_on_ignores_projection_flag();
    test_equal_timeframe_ignores_projection_flag();
    test_heikinashi_ignores_projection_flag();
    test_input_tf_below_script_tf_ignores_projection_flag();
    test_range_start_warmup_composes_with_projection();
    test_stream_warmup_and_continuation_stay_progressive();
    if (failures != 0) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_historical_security_lookahead_projection passed.\n");
    return 0;
}
