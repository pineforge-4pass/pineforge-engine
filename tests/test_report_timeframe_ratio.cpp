/*
 * test_report_timeframe_ratio.cpp -- lane FIX-E1E2 (E1): the report's
 * timeframe ratio fields.
 *
 * pf_report_t::script_tf_ratio is script_tf_seconds / input_tf_seconds and
 * needs_aggregation is 1 when the run aggregated its input bars up to the
 * script timeframe (include/pineforge/pineforge.h, docs/pages/report-schema.md,
 * docs/pages/mtf.md). The pre-#254 scheduler set both at every run begin;
 * #254 (73817c1d) retired that scheduler and every report has read 0 / 0 since,
 * the aggregated runs included. Each row below runs a Pine host through one
 * run route -- the bare run, the timeframe-aware run, auto-detected input,
 * the magnifier, a stream, a reused host -- and reads the report it fills.
 *
 * Fail-before (35db01c8): every row reads ratio 0 and needs_aggregation 0.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

constexpr std::int64_t kMinute = 60'000;
// 2025-04-07 00:00 UTC, a Monday.
constexpr std::int64_t kStart = 1743984000000LL;

// A script that enters on every tenth script bar and closes five bars later,
// so every route also books trades.
class Host final : public source::PineStrategyHost {
public:
    Host() {
        attach_pine_execution_adapter();
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ % 10 == 1) strategy_entry("L", true);
        if (bar_index_ % 10 == 6) strategy_close("L");
    }
};

std::vector<Bar> bars(int n, std::int64_t step_ms) {
    std::vector<Bar> out;
    double price = 1800.0;
    for (int i = 0; i < n; ++i) {
        Bar b{};
        b.timestamp = kStart + static_cast<std::int64_t>(i) * step_ms;
        b.open = price;
        b.close = price + 6.0 * std::sin(i / 7.0);
        b.high = std::max(b.open, b.close) + 1.5;
        b.low = std::min(b.open, b.close) - 1.5;
        b.volume = 10.0;
        price = b.close;
        out.push_back(b);
    }
    return out;
}

struct Expected {
    int input_s, script_s, ratio, aggregation, magnifier;
};

void check_report(const char* route, Host& host, const Expected& want) {
    ReportC report{};
    host.fill_report(&report);
    std::printf("%-34s input=%d script=%d ratio=%d agg=%d mag=%d trades=%d err='%s'\n", route,
                report.input_tf_seconds, report.script_tf_seconds, report.script_tf_ratio,
                report.needs_aggregation, report.bar_magnifier_enabled, report.trades_len,
                host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(report.trades_len > 0);
    CHECK(report.input_tf_seconds == want.input_s);
    CHECK(report.script_tf_seconds == want.script_s);
    CHECK(report.script_tf_ratio == want.ratio);
    CHECK(report.needs_aggregation == want.aggregation);
    CHECK(report.bar_magnifier_enabled == want.magnifier);
    BacktestEngine::free_report(&report);
}

void test_batch_routes() {
    const std::vector<Bar> m15 = bars(4 * 24 * 14, 15 * kMinute);  // 14 days of 15m bars
    const std::vector<Bar> daily = bars(10 * 7, 1440 * kMinute);    // 10 weeks of daily bars
    const int n15 = static_cast<int>(m15.size());
    {
        Host host;
        host.run(m15.data(), n15);
        check_report("run(bars, n)", host, {900, 900, 1, 0, 0});
    }
    {
        Host host;
        host.run(m15.data(), n15, "15", "15");
        check_report("run 15 -> 15", host, {900, 900, 1, 0, 0});
    }
    {
        Host host;
        host.run(m15.data(), n15, "15", "60");
        check_report("run 15 -> 60", host, {900, 3600, 4, 1, 0});
    }
    {
        Host host;
        host.run(m15.data(), n15, "15", "240");
        check_report("run 15 -> 240", host, {900, 14400, 16, 1, 0});
    }
    {
        Host host;
        host.run(m15.data(), n15, "15", "D");
        check_report("run 15 -> D", host, {900, 86400, 96, 1, 0});
    }
    {
        Host host;
        host.run(m15.data(), n15, "", "60");  // the input timeframe auto-detected
        check_report("run (detected 15) -> 60", host, {900, 3600, 4, 1, 0});
    }
    {
        Host host;
        host.run(m15.data(), n15, "15", "60", /*bar_magnifier=*/true);
        check_report("run 15 -> 60, magnifier", host, {900, 3600, 4, 1, 1});
    }
    {
        Host host;
        host.run(daily.data(), static_cast<int>(daily.size()), "D", "W");
        check_report("run D -> W", host, {86400, 604800, 7, 1, 0});
    }
    {
        // A reused host reports its latest run, not the one before it.
        Host host;
        host.run(m15.data(), n15, "15", "60");
        check_report("reused host: 15 -> 60", host, {900, 3600, 4, 1, 0});
        host.run(m15.data(), n15, "15", "15");
        check_report("reused host: then 15 -> 15", host, {900, 900, 1, 0, 0});
    }
}

void test_stream_route() {
    const std::vector<Bar> m15 = bars(4 * 24 * 14, 15 * kMinute);
    Host host;
    CHECK(host.stream_begin(m15.data(), static_cast<int>(m15.size()), "15", "60"));
    CHECK(host.stream_end());
    check_report("stream 15 -> 60", host, {900, 3600, 4, 1, 0});
}

}  // namespace

int main() {
    test_batch_routes();
    test_stream_route();
    std::printf("\n%s report timeframe ratio: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
