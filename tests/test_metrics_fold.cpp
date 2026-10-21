// O1W fail-before: compile this unit against 583c2217d2's header closure.
// The first diagnostic must be the missing pineforge/metrics_fold.hpp.
// The supervisor records that remote diagnostic; no local build is allowed.
#include <pineforge/metrics_fold.hpp>

// metrics_fold.hpp must remain usable without the engine header.
#ifdef PINEFORGE_HAS_SCRIPT_RUN_PREPARE_V1
#error metrics_fold.hpp unexpectedly includes the engine
#endif

#include <pineforge/metrics_stream.hpp>
#include "equity_stats_reference.hpp"
#include "trade_stats_reference.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace pineforge::metrics;
int failures = 0;
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

std::uint64_t random_state = 0x243f6a8885a308d3ULL;
std::uint64_t random_word() {
    random_state += 0x9e3779b97f4a7c15ULL;
    auto x = random_state;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

std::vector<pineforge::TradeC> trades() {
    std::vector<pineforge::TradeC> rows(29);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto& row = rows[i];
        row.pnl = i % 3 == 0 ? -13.25 : i % 3 == 1 ? 19.5 : -0.0;
        row.pnl_pct = row.pnl * 0.03125;
        row.commission = 0.125;
        row.is_long = static_cast<int>(i % 2);
        row.entry_bar_index = static_cast<int>(i * 3);
        row.exit_bar_index = row.entry_bar_index + static_cast<int>(i % 5);
    }
    return rows;
}

void compare(std::vector<pf_equity_point_t> points, const std::string& zone,
             double capital, bool anchor, bool rewrite) {
    const auto rows = trades();
    const auto n = static_cast<std::int64_t>(points.size());
    std::int64_t exposed = 0;
    EquityMetricsFold fold;
    for (std::int64_t i = 0; i < n; ++i) {
        const bool observation = !anchor || i != 0;
        const bool in_market = observation && i % 3 == 1;
        if (in_market) ++exposed;
        auto original = points[static_cast<std::size_t>(i)];
        if (rewrite && i == n - 1) {
            // A terminal mark and then a presentation mark can change the
            // last value AND its month. Neither provisional value may leak.
            original.equity += 123456.0;
            original.time_ms += 70LL * 86400000LL;
        }
        fold.fold(original, equity_month_key(original.time_ms, zone),
                  observation, in_market);
        if (rewrite && i == n - 1) {
            auto terminal = points.back();
            terminal.equity -= 33.0;
            fold.replace_last(terminal, equity_month_key(terminal.time_ms, zone));
            fold.replace_last(points.back(), equity_month_key(points.back().time_ms, zone));
        }
        if (i % 17 == 0) CHECK(fold.first_pass().point_count == i + 1);
    }
    const auto first = fold.first_pass();
    CHECK(first.point_count == n);
    CHECK(first.observation_count == n - (anchor && n > 0 ? 1 : 0));
    CHECK(first.bars_in_market == exposed);
    const EquityMetricInputs inputs{capital, 101.0, 97.25, -31.5};

    // Exercise the actual production array entry points, then independently
    // pin them to the test-only, frozen vector implementations.
    const pf_equity_point_t* array_points = points.empty() ? nullptr : points.data();
    pf_metrics_t array{};
    array.all = compute_trade_stats(rows.data(), static_cast<int>(rows.size()),
                                   TradeFilter::ALL, capital);
    array.longs = compute_trade_stats(rows.data(), static_cast<int>(rows.size()),
                                     TradeFilter::LONG, capital);
    array.shorts = compute_trade_stats(rows.data(), static_cast<int>(rows.size()),
                                      TradeFilter::SHORT, capital);
    array.equity = compute_equity_stats(array_points, n, capital, zone,
        inputs.first_open, inputs.last_close, exposed, inputs.net_profit,
        first.observation_count);
    pf_metrics_t frozen{};
    frozen.all = trade_stats_reference::compute_trade_stats(
        rows.data(), static_cast<int>(rows.size()), TradeFilter::ALL, capital);
    frozen.longs = trade_stats_reference::compute_trade_stats(
        rows.data(), static_cast<int>(rows.size()), TradeFilter::LONG, capital);
    frozen.shorts = trade_stats_reference::compute_trade_stats(
        rows.data(), static_cast<int>(rows.size()), TradeFilter::SHORT, capital);
    frozen.equity = equity_stats_reference::compute_equity_stats(
        array_points, n, capital, zone, inputs.first_open, inputs.last_close,
        exposed, inputs.net_profit);
    if (n > 0)
        frozen.equity.time_in_market_pct = first.observation_count > 0
            ? static_cast<double>(exposed) / static_cast<double>(first.observation_count) * 100.0
            : std::numeric_limits<double>::quiet_NaN();
    CHECK(std::memcmp(&array, &frozen, sizeof(array)) == 0);

    // First-pass fields compare as a complete metrics block after masking
    // ONLY the four ratios in the array oracle. No expected metric is typed
    // into this test or computed by the new fold.
    auto first_only = array;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    first_only.equity.sharpe_monthly = first_only.equity.sortino_monthly = nan;
    first_only.equity.sharpe_bar = first_only.equity.sortino_bar = nan;
    const auto got_first = finalize_metrics(
        first, inputs, array.all, array.longs, array.shorts);
    CHECK(std::memcmp(&got_first, &first_only, sizeof(array)) == 0);

    EquityMetricsSecondPass second(first);
    for (std::int64_t i = 0; i < n; ++i) {
        const bool observation = !anchor || i != 0;
        const bool in_market = observation && i % 3 == 1;
        auto point = points[static_cast<std::size_t>(i)];
        if (rewrite && i == n - 1) point.equity += 9999.0;
        second.fold(point, equity_month_key(point.time_ms, zone), observation, in_market);
        if (rewrite && i == n - 1)
            second.replace_last(points.back(), equity_month_key(points.back().time_ms, zone));
        if (i % 31 == 0 && i < n - 1) CHECK(!second.finish().complete);
    }
    const auto ratios = second.finish();
    CHECK(ratios.matches(first));
    const auto got = finalize_metrics(
        first, inputs, array.all, array.longs, array.shorts, &ratios);
    static_assert(sizeof(pf_metrics_t) == 768, "raw metric contract");
    CHECK(std::memcmp(&got, &array, 768) == 0);
    const auto again = second.finish();
    const auto repeated = finalize_metrics(
        fold.first_pass(), inputs, array.all, array.longs, array.shorts, &again);
    CHECK(std::memcmp(&repeated, &array, 768) == 0);

    if (n > 0) {
        auto different = fold;
        auto point = points.back();
        point.open_profit += 1.0;
        different.replace_last(point, equity_month_key(point.time_ms, zone));
        CHECK(!ratios.matches(different.first_pass()));
    }
    if (n > 3) {
        EquityMetricsSecondPass reordered(first);
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int64_t index = i == 1 ? 2 : i == 2 ? 1 : i;
            const auto& point = points[static_cast<std::size_t>(index)];
            const bool observation = !anchor || index != 0;
            reordered.fold(point, equity_month_key(point.time_ms, zone),
                           observation, observation && index % 3 == 1);
        }
        // Count and endpoint equality alone cannot authorize a second walk.
        CHECK(!reordered.finish().matches(first));
    }
}

void cases() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<std::vector<pf_equity_point_t>> edge{
        {},
        {{1706745600000LL, 123.0, -0.0}},
        {{1706745600000LL, 0.0, 0.0}},
        {{1706745600000LL, -7.0, -3.0}},
        {{1706745599999LL, 101.0, 0.0}, {1706745600000LL, 99.0, -2.0}},
        {{1706745599999LL, 0.0, 0.0}, {1706745600000LL, -1.0, -1.0}},
        {{1704067200000LL, 101.0, 0.0}, {1706745600000LL, -3.0, -1.0},
         {1709251200000LL, 0.0, -0.0}, {1711929600000LL, 97.0, 4.0},
         {1714521600000LL, 103.0, 3.0}},
        // Civil months differ from UTC around New York's midnight.
        {{1706751000000LL, 100.0, 0.0}, {1706763600000LL, 98.0, -2.0},
         {1709253000000LL, 104.0, 4.0}, {1709269200000LL, 101.0, 1.0},
         {1711944000000LL, 96.0, -4.0}, {1714536000000LL, 102.0, 2.0}},
        {{1704067200000LL, nan, nan}, {1706745600000LL, 1.0, -0.0},
         {1709251200000LL, inf, 1.0}, {1711929600000LL, 0.0, -2.0}},
        {{1704067200000LL, 100.0, -0.0}, {1706745600000LL, 100.0, 0.0},
         {1709251200000LL, 100.0, -0.0}, {1711929600000LL, 100.0, 0.0}},
        {{1704067200000LL, 100.0, 0.0}, {1704067200000LL, 97.0, -3.0},
         {1704067200000LL, 101.0, 1.0}},
        {{1704067200000LL, 0x1p53, 0.0}, {1706745600000LL, 1.0, -0.0},
         {1709251200000LL, 0x1p53, 0.0}, {1711929600000LL, 1.0, -0.0},
         {1714521600000LL, 0x1p53, 0.0}},
        // A nonpositive span still walks monthly returns.
        {{1711929600000LL, 100.0, 0.0}, {1709251200000LL, 97.0, -3.0},
         {1706745600000LL, 101.0, 1.0}},
    };
    for (const auto& zone : {"UTC", "Etc/UTC", "America/New_York", "Asia/Taipei"}) {
        for (const auto& curve : edge) {
            for (const auto capital : {10000.0, 0.0, -1.0, nan}) {
                compare(curve, zone, capital, false, false);
                compare(curve, zone, capital, true, true);
            }
        }
        for (int trial = 0; trial < 24; ++trial) {
            std::vector<pf_equity_point_t> points;
            const std::size_t count = 3 + random_word() % 503;
            double equity = 100.0;
            for (std::size_t i = 0; i < count; ++i) {
                if (i % 43 == 0) equity = 0.0;
                else if (i % 61 == 0) equity = -11.0;
                else equity += static_cast<double>(random_word() % 801) * 0.125 - 37.0;
                points.push_back({1577836800000LL + static_cast<std::int64_t>(i) * 86400000LL,
                                  equity, (i % 2 == 0 ? 1.0 : -1.0) * 0.25});
            }
            compare(points, zone, 10000.0, trial % 2 == 0, true);
        }
    }
}
} // namespace

int main() {
    cases();
    std::printf("metrics fold: first=%zu bytes, second=%zu bytes; %d checks, %d failures\n",
                sizeof(EquityMetricsFold), sizeof(EquityMetricsSecondPass), checks, failures);
    return failures ? 1 : 0;
}
