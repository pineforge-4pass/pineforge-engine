// Public synthetic expectations; no TradingView measurements or private data.
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
const char* row = "";
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        std::printf("FAIL [%s] line %d: %s\n", row, __LINE__, #expr);            \
        ++failures;                                                            \
    }                                                                          \
} while (false)

constexpr int kLength = 3;
constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kHour = 60 * kMinute;

bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

std::vector<Bar> chart_feed() {
    std::vector<Bar> bars;
    // Nonlinear closes exercise the recursion after the SMA seed.
    for (const double close : {8.0, 14.0, 26.0, 18.0, 42.0, 30.0})
        bars.push_back(Bar{close, close, close, close, 1.0,
                           kHour + static_cast<std::int64_t>(bars.size()) * kMinute});
    return bars;
}

// Definition-only oracle: never reads an engine value.
std::vector<double> expected_ema(const std::vector<Bar>& bars, bool sma_seed) {
    std::vector<double> values(bars.size(), na<double>());
    const double weight = 2.0 / (kLength + 1);
    double previous = na<double>(), sum = 0.0;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (sma_seed && i < kLength) {
            sum += bars[i].close;
            if (i + 1 == kLength) previous = sum / kLength;
        } else if (i == 0) {
            previous = bars[i].close;
        } else {
            previous = weight * bars[i].close + (1.0 - weight) * previous;
        }
        values[i] = previous;
    }
    return values;
}

class ChartHost final : public source::PineStrategyHost {
public:
    ta::EMA ema{kLength};
    std::vector<double> values;
    double origin_metadata() const { return get_syminfo_metadata("series_origin_ms"); }
    void on_source_bar(const Bar& bar) override {
        values.push_back(ema.compute(bar.close));
    }
};

void chart_row(const char* name,
               const std::vector<std::pair<std::string, double>>& metadata,
               bool sma_seed) {
    row = name;
    const int before = failures;
    const auto bars = chart_feed();
    const auto expected = expected_ema(bars, sma_seed);
    ChartHost host;
    for (const auto& item : metadata) {
        const double previous_origin = host.origin_metadata();
        host.set_syminfo_metadata(item.first, item.second);
        if (item.first == "series_origin_ms"
            && (!std::isfinite(item.second) || item.second <= 0.0))
            CHECK(same(host.origin_metadata(), previous_origin));
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.values.size() == expected.size());
    for (std::size_t i = 0; i < host.values.size() && i < expected.size(); ++i)
        CHECK(same(host.values[i], expected[i]));
    std::printf("%s [%s]\n", failures == before ? "PASS" : "FAIL", row);
}

class SecurityHost final : public source::PineStrategyHost {
public:
    ta::EMA chart{kLength}, security{kLength};
    double plain = na<double>();
    std::vector<double> chart_values, plain_values, completed_closes, security_values;
    std::vector<std::int64_t> completed_opens;
    std::vector<bool> security_flags;

    SecurityHost() {
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        register_security_eval(0, "60", "15", false, false);
    }
    void evaluate_security(int id, const Bar& bar, bool complete) override {
        CHECK(id == 0);
        CHECK(complete);
        plain = bar.close;  // the plain request.security(close), gaps_off latch
        completed_opens.push_back(bar.timestamp);
        completed_closes.push_back(bar.close);
        security_flags.push_back(ta::ema_na_warmup_flag());
        security_values.push_back(security.compute(bar.close));
    }
    void on_source_bar(const Bar& bar) override {
        chart_values.push_back(chart.compute(bar.close));
        plain_values.push_back(plain);
    }
};

void security_row(bool disable_chart) {
    row = disable_chart ? "e last writer: chart off, security on"
                        : "d EXPECTATION: engine behaviour, not a TradingView measurement";
    const int before = failures;
    std::vector<Bar> bars;
    // Start at 01:00, declare 01:15: even the fed 01:15..01:45 remainder
    // must be absent from the hourly series. First whole bucket is 02:00.
    for (int quarter = 4; quarter < 24; ++quarter) {
        const double close = 2.0 * quarter + 10.0;
        bars.push_back(Bar{close, close, close, close, 1.0,
                           quarter * 15 * kMinute});
    }
    const std::int64_t origin = bars.front().timestamp + 15 * kMinute;
    const std::int64_t first_whole = ((origin + kHour - 1) / kHour) * kHour;
    SecurityHost host;
    host.set_syminfo_metadata("series_origin_ms", static_cast<double>(origin));
    if (disable_chart) host.set_syminfo_metadata("chart_ema_na_warmup", 0.0);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().empty());
    CHECK(host.plain_values.size() == bars.size());
    std::vector<Bar> expected_htf;
    // Independent whole-bucket OHLC close construction from synthetic inputs.
    for (std::int64_t open = first_whole;
         open + kHour <= bars.back().timestamp + 15 * kMinute; open += kHour) {
        const auto index = static_cast<std::size_t>(
            (open + kHour - 15 * kMinute - bars.front().timestamp) / (15 * kMinute));
        expected_htf.push_back(Bar{0, 0, 0, bars[index].close, 0, open});
    }
    const auto expected_security = expected_ema(expected_htf, true);
    CHECK(host.completed_opens.size() == expected_htf.size());
    CHECK(host.security_values.size() == expected_security.size());
    CHECK(host.completed_closes.size() == expected_htf.size());
    CHECK(host.security_flags.size() == expected_htf.size());
    for (std::size_t i = 0; i < host.completed_opens.size() && i < expected_htf.size(); ++i) {
        CHECK(host.completed_opens[i] == expected_htf[i].timestamp);
        CHECK(host.completed_closes[i] == expected_htf[i].close);
        CHECK(host.security_flags[i]);
        CHECK(same(host.security_values[i], expected_security[i]));
    }
    // Completion is visible on the last 15m child of the first whole hour.
    // The preceding origin-straddling hour must never publish a plain close.
    double expected_plain = na<double>();
    for (std::size_t i = 0; i < host.plain_values.size() && i < bars.size(); ++i) {
        const auto close_time = bars[i].timestamp + 15 * kMinute;
        if (close_time >= first_whole + kHour && close_time % kHour == 0)
            expected_plain = bars[i].close;
        CHECK(same(host.plain_values[i], expected_plain));
    }
    const auto expected_chart = expected_ema(bars, !disable_chart);
    CHECK(host.chart_values.size() == expected_chart.size());
    for (std::size_t i = 0; i < host.chart_values.size() && i < expected_chart.size(); ++i)
        CHECK(same(host.chart_values[i], expected_chart[i]));
    std::printf("%s [%s]\n", failures == before ? "PASS" : "FAIL", row);
}
}  // namespace

int main() {
    chart_row("a undeclared first-value definition", {}, false);
    chart_row("b declared SMA-seed definition", {{"series_origin_ms", kHour}}, true);
    // Earlier origins are accepted; they do not invent missing chart inputs.
    chart_row("b earlier declared origin", {{"series_origin_ms", kHour - 1}}, true);
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
        chart_row("c invalid origin leaves default", {{"series_origin_ms", invalid}}, false);
        chart_row("c invalid origin leaves declaration",
                  {{"series_origin_ms", kHour}, {"series_origin_ms", invalid}}, true);
    }
    security_row(false);
    security_row(true);
    std::printf("series-origin host: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
