// Lane K-RUNERR: a plain request.security whose requested timeframe is finer
// than the finest feed the host supplied.
//
// TradingView reads a lower timeframe from that timeframe's own bars and keeps
// the last 2,000,000 of them: on BINANCE:ETHUSDT.P 15m and 1D over
// 2025-04-01..2026-05-01, request.security(tickerid, "5S", close) is na up to
// 2026-01-04 22:45 UTC and "15S" up to 2025-05-18 03:15 UTC, and a gaps_on
// read is na on the 27 chart bars that hold no 5S bar (lab tv
// pf-krunerr-ltf-horizon, -ltf-values, -ltf-count2, 2026-09-26). A chart bar
// holding no loaded bar of the requested timeframe reads na.
//
// The campaign lanes' finest bars are 1m, so a "5S" or "15S" site has no
// loaded bar on any chart bar. Where the host already supplied bars finer than
// the chart -- the auxiliary slice, or an input finer than the script
// timeframe -- such a site now reads na throughout (it is never evaluated)
// instead of stopping the run. A chart fed without intrabar bars keeps the
// refusal, so its host can supply finer bars (the verifier's split-feed retry
// keys on it).
//
//   1. 15m chart + 1m auxiliary slice: "5S" (lookahead_off, gaps_off), "15S"
//      (lookahead_on, gaps_on) and "30S" are never evaluated and read na; the
//      "1" site beside them is served as before.
//   2. 1D chart + 1m auxiliary slice: the same for "5S".
//   3. single 1m input aggregated to a 15m script: the same for "5S".
//   4. a 15m chart fed alone (input == script) still refuses "5S" and "5" with
//      the unchanged message.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the finer-than-feed test drives the auxiliary security feed"
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                \
        }                                                                      \
    } while (0)

struct Site {
    std::string tf;
    bool lookahead_on;
    bool gaps_on;
};

class FinerProbe final : public source::PineStrategyHost {
    std::vector<Site> sites_;

public:
    explicit FinerProbe(std::vector<Site> sites)
        : sites_(std::move(sites)),
          dispatches(sites_.size(), 0),
          latest(sites_.size(), na<double>()) {}

    std::vector<int> dispatches;
    std::vector<double> latest;
    std::vector<std::vector<double>> at_chart_bar;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (std::size_t i = 0; i < sites_.size(); ++i) {
            register_security_eval(static_cast<int>(i), sites_[i].tf, input_tf_,
                                   sites_[i].lookahead_on, sites_[i].gaps_on);
        }
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        const auto i = static_cast<std::size_t>(sec_id);
        ++dispatches[i];
        if (is_complete) latest[i] = bar.close;
    }

    void on_source_bar(const Bar&) override { at_chart_bar.push_back(latest); }
};

constexpr int64_t kMinute = 60000;
constexpr int64_t kQuarter = 15 * kMinute;
constexpr int64_t kT0 = 1767566700000;  // 2026-01-04 22:45 UTC

// Minute bars whose close is their minute's ordinal, so the chart bar's last
// minute is readable off the "1" site.
std::vector<Bar> minutes(int64_t from, int count) {
    std::vector<Bar> out;
    for (int i = 0; i < count; ++i) {
        const double v = 100.0 + i;
        out.push_back({v, v + 0.5, v - 0.5, v, 1.0, from + i * kMinute});
    }
    return out;
}

void report(const char* scenario, int failures_before) {
    std::printf("%s: %s\n", scenario,
                g_failures == failures_before ? "ok" : "FAILED");
}

bool all_na(const std::vector<std::vector<double>>& rows, std::size_t site) {
    for (const auto& row : rows) {
        if (!std::isnan(row[site])) return false;
    }
    return !rows.empty();
}

void test_split_feed_intraday_chart() {
    const int before = g_failures;
    const Bar chart[] = {
        {100.0, 114.5, 99.5, 114.0, 15.0, kT0},
        {115.0, 129.5, 114.5, 129.0, 15.0, kT0 + kQuarter},
        {130.0, 144.5, 129.5, 144.0, 15.0, kT0 + 2 * kQuarter},
    };
    const std::vector<Bar> aux = minutes(kT0, 45);
    FinerProbe probe({{"5S", false, false},
                      {"15S", true, true},
                      {"1", false, false},
                      {"30S", false, false}});
    CHECK(strategy_set_aux_security_feed(
              static_cast<pf_strategy_t>(&probe),
              reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "1") == 0);
    probe.run(chart, 3, "15", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    if (!probe.last_error().empty()) std::printf("  error: %s\n", probe.last_error().c_str());
    CHECK(probe.at_chart_bar.size() == 3);
    CHECK(probe.dispatches[0] == 0);
    CHECK(probe.dispatches[1] == 0);
    CHECK(probe.dispatches[3] == 0);
    CHECK(all_na(probe.at_chart_bar, 0));
    CHECK(all_na(probe.at_chart_bar, 1));
    CHECK(all_na(probe.at_chart_bar, 3));
    // The 1m site beside them is served by the slice: every minute is
    // evaluated and each chart bar reads its own last minute.
    CHECK(probe.dispatches[2] == 45);
    if (probe.at_chart_bar.size() == 3) {
        CHECK(probe.at_chart_bar[0][2] == 114.0);
        CHECK(probe.at_chart_bar[1][2] == 129.0);
        CHECK(probe.at_chart_bar[2][2] == 144.0);
    }
    report("split feed, 15m chart", before);
}

void test_split_feed_daily_chart() {
    const int before = g_failures;
    constexpr int64_t day = 86400000;
    constexpr int64_t d0 = 1767484800000;  // 2026-01-04 00:00 UTC
    const Bar chart[] = {
        {100.0, 101.0, 99.0, 100.5, 1.0, d0},
        {101.0, 102.0, 100.0, 101.5, 1.0, d0 + day},
    };
    std::vector<Bar> aux = minutes(d0, 3);
    for (const Bar& b : minutes(d0 + day, 3)) aux.push_back(b);
    FinerProbe probe({{"5S", false, false}, {"1", false, false}});
    CHECK(strategy_set_aux_security_feed(
              static_cast<pf_strategy_t>(&probe),
              reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "1") == 0);
    probe.run(chart, 2, "1D", "1D", false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.dispatches[0] == 0);
    CHECK(all_na(probe.at_chart_bar, 0));
    CHECK(probe.dispatches[1] == 6);
    report("split feed, 1D chart", before);
}

void test_single_minute_input_aggregated_chart() {
    const int before = g_failures;
    const std::vector<Bar> input = minutes(kT0, 45);
    FinerProbe probe({{"5S", false, false}, {"1", false, false}});
    probe.run(input.data(), static_cast<int>(input.size()), "1", "15", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.at_chart_bar.size() == 3);
    CHECK(probe.dispatches[0] == 0);
    CHECK(all_na(probe.at_chart_bar, 0));
    CHECK(probe.dispatches[1] == 45);
    report("single 1m input, 15m script", before);
}

void test_chart_without_intrabar_bars_still_refuses() {
    const int before = g_failures;
    const Bar chart[] = {
        {100.0, 101.0, 99.0, 100.5, 1.0, kT0},
        {101.0, 102.0, 100.0, 101.5, 1.0, kT0 + kQuarter},
    };
    {
        FinerProbe probe({{"5S", false, false}});
        probe.run(chart, 2, "15", "15", false, 4, MagnifierDistribution::ENDPOINTS);
        CHECK(probe.last_error()
              == "request.security: requested timeframe '5S' is finer than input "
                 "'15'. Use request.security_lower_tf for sub-input timeframes.");
    }
    {
        FinerProbe probe({{"5", false, false}});
        probe.run(chart, 2, "15", "15", false, 4, MagnifierDistribution::ENDPOINTS);
        CHECK(probe.last_error()
              == "request.security: requested timeframe '5' is finer than input "
                 "'15'. Use request.security_lower_tf for sub-input timeframes.");
    }
    report("15m chart fed alone", before);
}

}  // namespace

int main() {
    test_split_feed_intraday_chart();
    test_split_feed_daily_chart();
    test_single_minute_input_aggregated_chart();
    test_chart_without_intrabar_bars_still_refuses();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
