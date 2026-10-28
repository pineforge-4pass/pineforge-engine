// The array point source and the allocation-free equity
// metric walks (DESIGN.md 3.3).  This is intentionally smaller than the L0
// sink harness: it exercises the public point-source contract against the
// existing, frozen-equivalent equity walk.
//
// RED ON ee3083d4a: metrics_stream.hpp declares ArrayCurve and the
// PointSource overloads, but this tree has no definitions or ArrayCurve
// instantiation.  The first fail-before is therefore the missing L7B
// implementation, not a changed metric expectation.

#include <pineforge/metrics_stream.hpp>

#include "equity_stats_reference.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                           \
        }                                                                       \
    } while (0)

using pineforge::metrics::ArrayCurve;
using pineforge::metrics::compute_equity_stats;

bool same_bytes(const pf_equity_stats_t& a, const pf_equity_stats_t& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

pf_equity_stats_t point_source_stats(const std::vector<pf_equity_point_t>& points,
                                     const std::string& chart_tz,
                                     std::int64_t bars_in_market,
                                     std::int64_t observation_count) {
    const std::int64_t n = static_cast<std::int64_t>(points.size());
    ArrayCurve source(points.empty() ? nullptr : points.data(), n);
    const double first_open = 100.0;
    const double last_close = points.empty() ? 100.0 : 100.0 + points.back().equity / 100.0;
    const double net_profit = points.empty() ? 0.0 : points.back().equity - 10000.0;
    return compute_equity_stats<ArrayCurve>(
        source, n, 10000.0, chart_tz, first_open, last_close, bars_in_market,
        net_profit, observation_count);
}

void source_copies_exact_points() {
    const pf_equity_point_t points[] = {
        {1704067199999LL, -0.0, std::numeric_limits<double>::quiet_NaN()},
        {1704067200000LL, 123.25, -4.5},
        {1706745600000LL, -7.0, std::numeric_limits<double>::infinity()},
    };
    ArrayCurve source(points, 3);
    CHECK(source.size() == 3);
    for (std::int64_t i = 0; i < source.size(); ++i) {
        const pf_equity_point_t got = source[i];
        CHECK(std::memcmp(&got, &points[i], sizeof(points[i])) == 0);
    }
}

void ordinary_point_source_entry_matches_reference() {
    const std::vector<pf_equity_point_t> points = {
        {1704067199000LL, 10000.0, 0.0},
        {1704067200000LL, 10100.0, 100.0},
        {1706745600000LL, 9900.0, -100.0},
        {1709251200000LL, 10050.0, 50.0},
    };
    const std::int64_t n = static_cast<std::int64_t>(points.size());
    ArrayCurve source(points.data(), n);
    const double last_close = 100.0 + points.back().equity / 100.0;
    const double net_profit = points.back().equity - 10000.0;
    const pf_equity_stats_t got = compute_equity_stats<ArrayCurve>(
        source, n, 10000.0, "UTC", 100.0, last_close, n / 2, net_profit);
    const pf_equity_stats_t want = equity_stats_reference::compute_equity_stats(
        points.data(), n, 10000.0, "UTC", 100.0, last_close, n / 2, net_profit);
    CHECK(same_bytes(got, want));
}

void matches_reference_for_empty_and_month_boundaries() {
    const std::vector<std::vector<pf_equity_point_t>> cases = {
        {},
        {{1704067199000LL, 10000.0, 0.0}},
        {{1704067199000LL, 10000.0, 0.0},
         {1704067200000LL, 10100.0, 100.0},
         {1706745600000LL, 9900.0, -100.0},
         {1709251200000LL, 10050.0, 50.0}},
    };

    for (const auto& points : cases) {
        const std::int64_t n = static_cast<std::int64_t>(points.size());
        const pf_equity_point_t* data = points.empty() ? nullptr : points.data();
        const pf_equity_stats_t want = equity_stats_reference::compute_equity_stats(
            data, n, 10000.0, "UTC", 100.0,
            points.empty() ? 100.0 : 100.0 + points.back().equity / 100.0,
            n / 2, points.empty() ? 0.0 : points.back().equity - 10000.0);
        const pf_equity_stats_t got = point_source_stats(points, "UTC", n / 2, n);
        CHECK(same_bytes(got, want));
    }
}

void matches_reference_for_zero_negative_and_zoned_curves() {
    std::vector<pf_equity_point_t> points;
    points.reserve(96);
    double equity = 10000.0;
    for (std::int64_t i = 0; i < 96; ++i) {
        if (i == 19) equity = 0.0;
        else if (i == 37) equity = -250.0;
        else if (i == 61) equity = 10250.0;
        else equity += (i % 2 == 0) ? 37.5 : -18.25;
        points.push_back({1577836800000LL + i * 86400000LL, equity,
                          (i % 3 == 0) ? -2.0 : 1.5});
    }

    const std::int64_t n = static_cast<std::int64_t>(points.size());
    const pf_equity_stats_t want = equity_stats_reference::compute_equity_stats(
        points.data(), n, 10000.0, "America/New_York", 100.0,
        100.0 + points.back().equity / 100.0, 31, points.back().equity - 10000.0);
    const pf_equity_stats_t got = point_source_stats(points, "America/New_York", 31, n);
    CHECK(same_bytes(got, want));
}

void observation_count_only_changes_exposure_denominator() {
    const std::vector<pf_equity_point_t> points = {
        {1700000000000LL, 100.0, 0.0},
        {1715760000000LL, 110.0, 10.0},
        {1731542400000LL, 104.5, -5.5},
    };
    const std::int64_t n = static_cast<std::int64_t>(points.size());
    const auto ordinary = point_source_stats(points, "UTC", 1, n);
    const auto selected = point_source_stats(points, "UTC", 1, n - 1);
    pf_equity_stats_t expected = ordinary;
    expected.time_in_market_pct = 100.0 / static_cast<double>(n - 1);
    CHECK(same_bytes(selected, expected));
    CHECK(selected.time_in_market_pct == 50.0);
}

}  // namespace

int main() {
    source_copies_exact_points();
    ordinary_point_source_entry_matches_reference();
    matches_reference_for_empty_and_month_boundaries();
    matches_reference_for_zero_negative_and_zoned_curves();
    observation_count_only_changes_exposure_denominator();
    std::printf("metrics point source: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
