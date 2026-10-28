// Civil-month keys across the int64 millisecond domain. The independent
// oracle counts Gregorian cycles from January 1, year 1; production uses
// March-based civil_from_days. The existing in-range sweeps also compare
// with libc. Extreme stamps never ask libc for an expected value.
#include <pineforge/metrics.hpp>
#include <pineforge/metrics_fold.hpp>
#include <pineforge/native_host.hpp>

#include "../src/metrics_month_memo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <vector>

namespace {

int failures = 0;
long long checks = 0;

namespace metrics = pineforge::metrics;
namespace detail = pineforge::metrics::detail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond) && ++failures <= 20)                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);            \
    } while (0)

void check_key(std::int64_t ts_ms, std::int64_t expected, std::int64_t got) {
    ++checks;
    if (expected == got) return;
    if (++failures <= 20) {
        std::fprintf(stderr, "FAIL ts_ms=%lld expected %lld got %lld\n",
                     static_cast<long long>(ts_ms), static_cast<long long>(expected),
                     static_cast<long long>(got));
    }
}

// 1970-01-01 is 1969 * 365 + 1969 / 4 - 1969 / 100 + 1969 / 400
// = 719162 days after 0001-01-01. Each Gregorian 400-year cycle has
// 146097 days. Split into centuries, four-year groups and individual years,
// keeping the final leap day in its cycle; then subtract actual month lengths.
// This oracle uses neither native_civil_date nor any month-key helper.
std::int64_t gregorian_key(std::int64_t secs) {
    const std::int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
    const std::int64_t ordinal = days + 719162;
    const std::int64_t cycles = ordinal / 146097 - (ordinal % 146097 < 0 ? 1 : 0);
    std::int64_t day = ordinal - cycles * 146097;
    const std::int64_t centuries = std::min(day / 36524, std::int64_t{3});
    day -= centuries * 36524;
    const std::int64_t fours = day / 1461;
    day -= fours * 1461;
    const std::int64_t years = std::min(day / 365, std::int64_t{3});
    day -= years * 365;
    const std::int64_t year = cycles * 400 + centuries * 100 + fours * 4 + years + 1;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int lengths[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    std::int64_t month0 = 0;
    while (month0 < 11 && day >= lengths[month0]) {
        day -= lengths[month0];
        ++month0;
    }
    return year * 12 + month0;
}

// Secondary in-range witness for the unchanged historical truncation.
std::int64_t libc_month_key(std::int64_t ts_ms) {
    time_t secs = (time_t)(ts_ms / 1000);
    struct tm tb {};
    CHECK(gmtime_r(&secs, &tb) != nullptr);
    return (static_cast<std::int64_t>(tb.tm_year) + 1900) * 12 + tb.tm_mon;
}

void check_at(std::int64_t ts_ms) {
    const std::int64_t expected = gregorian_key(ts_ms / 1000);
    check_key(ts_ms, expected, detail::month_key_utc(ts_ms));
    check_key(ts_ms, expected, libc_month_key(ts_ms));
}

// Under AddressSanitizer the year 1-9999 sweep reads every 15th day (every libc
// call is instrumented there); every other profile reads every day. The
// 1970-2100 sweeps run whole everywhere.
#if defined(__SANITIZE_ADDRESS__)
constexpr std::int64_t kFarStride = 15;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr std::int64_t kFarStride = 15;
#else
constexpr std::int64_t kFarStride = 1;
#endif
#else
constexpr std::int64_t kFarStride = 1;
#endif

constexpr std::int64_t kHourMs = 3600LL * 1000;
constexpr std::int64_t kDayMs = 86400LL * 1000;

void every_hour_1970_to_2100() {
    const std::int64_t end = 4133980800LL * 1000;  // 2101-01-01T00:00:00Z
    std::uint64_t mix = 0x9e3779b97f4a7c15ull;
    for (std::int64_t hour = 0; hour * kHourMs < end; ++hour) {
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        check_at(hour * kHourMs + static_cast<std::int64_t>(mix % kHourMs));
    }
}

void every_day_year_1_to_9999() {
    const std::int64_t first = -62135596800LL;  // 0001-01-01T00:00:00Z
    const std::int64_t last = 253402300799LL;   // 9999-12-31T23:59:59Z
    for (std::int64_t day = first / 86400; day * 86400 <= last; day += kFarStride) {
        const std::int64_t ms = day * kDayMs;
        check_at(ms);
        check_at(ms - 1);          // a negative stamp truncates toward zero
        check_at(ms + kDayMs - 1);
    }
}

// 00:00:00 UTC of (year, month), used only to place probes. Expectations
// come from the January-based oracle above and, in range, gmtime_r.
std::int64_t month_open_seconds(int year, int month) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t yoe = year - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400;
}

void every_month_boundary_1900_to_2100() {
    for (int year = 1900; year <= 2100; ++year) {
        for (int month = 1; month <= 12; ++month) {
            const std::int64_t ms = month_open_seconds(year, month) * 1000;
            for (std::int64_t delta : {-1001LL, -1000LL, -999LL, -1LL, 0LL, 1LL, 999LL, 1000LL}) {
                check_at(ms + delta);
            }
        }
    }
}

void both_sides_of_the_cache_edge() {
    const std::int64_t edge = std::int64_t{1} << 40;
    for (std::int64_t secs : {edge - 2, edge - 1, edge, edge + 1, -edge - 1, -edge,
                              -edge + 1, -edge + 2, edge * 2, -edge * 2, edge * 64,
                              -edge * 64}) {
        for (std::int64_t ms : {0LL, 1LL, 999LL}) {
            check_at(secs * 1000 + (secs < 0 ? -ms : ms));
        }
    }
}

void full_timestamp_range() {
    constexpr std::int64_t low = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t high = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t edge = std::int64_t{1} << 40;
    const std::int64_t stamps[] = {
        low, low + 1, -(edge * 2) * 1000, (edge * 2) * 1000, high,
        -(edge - 1) * 1000, (edge - 1) * 1000, -edge * 1000, edge * 1000,
    };
    detail::UtcMonthMemo memo;
    for (const std::int64_t ts : stamps) {
        const std::int64_t expected = gregorian_key(ts / 1000);
        check_key(ts, expected, detail::month_key_utc(ts));
        check_key(ts, expected, detail::month_key_utc(memo, ts));
        for (const char* zone : {"", "UTC", "Etc/UTC", "UTC0"})
            check_key(ts, expected, metrics::equity_month_key(ts, zone));
        // POSIX fixed offsets force month_key_local, with an independently
        // shifted second count. Division precedes addition to preserve the
        // historical truncation and avoid overflowing an extreme ms stamp.
        check_key(ts, gregorian_key(ts / 1000 - 12 * 3600),
                  metrics::equity_month_key(ts, "EST12"));
        check_key(ts, gregorian_key(ts / 1000 + 14 * 3600),
                  metrics::equity_month_key(ts, "EET-14"));
    }
}

void epoch_and_leap_days() {
    for (std::int64_t ms = -5000; ms <= 5000; ++ms) check_at(ms);
    for (int year : {1900, 2000, 2100}) {
        const std::int64_t march = month_open_seconds(year, 3) * 1000;
        for (std::int64_t hour = -72; hour <= 48; ++hour)
            for (std::int64_t delta : {-1001LL, -1LL, 0LL, 1LL, 999LL})
                check_at(march + hour * kHourMs + delta);
    }
    // Local bucketing also floors pre-epoch days and crosses month boundaries.
    for (int year : {1900, 1970, 2000, 2100}) {
        for (int month = 1; month <= 12; ++month) {
            const std::int64_t at = month_open_seconds(year, month) * 1000;
            for (std::int64_t delta : {-1001LL, -1LL, 0LL, 1LL, 999LL}) {
                const std::int64_t ts = at + delta;
                check_key(ts, gregorian_key(ts / 1000 - 12 * 3600),
                          metrics::equity_month_key(ts, "EST12"));
                check_key(ts, gregorian_key(ts / 1000 + 14 * 3600),
                          metrics::equity_month_key(ts, "EET-14"));
            }
        }
    }
}

void check_ratio(double expected, double got) {
    CHECK(std::isfinite(expected) && std::isfinite(got));
    CHECK(std::abs(expected - got) <= 1e-12 * std::max(1.0, std::abs(expected)));
}

// Supply report-producer rows directly, without asking the execution calendar
// to admit extreme feed timestamps. This exercises the real report section.
class MonthReportHost final : public pineforge::NativeStrategyHost {
public:
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}

    pf_equity_stats_t read(const std::vector<pf_equity_point_t>& points, const char* zone,
                           const metrics::EquityMetricInputs& inputs) {
        equity_curve_ = points;
        initial_capital_ = inputs.initial_capital;
        chart_timezone_ = zone;
        first_bar_open_ = inputs.first_open;
        current_bar_.close = inputs.last_close;
        pineforge::ReportC report{};
        report.net_profit = inputs.net_profit;
        try {
            fill_metrics_section(&report);
        } catch (...) {
            free_report(&report);
            throw;
        }
        CHECK(report.equity_curve_len == static_cast<std::int64_t>(points.size()));
        const pf_equity_stats_t result = report.metrics.equity;
        free_report(&report);
        return result;
    }
};

// Public metric producer used by fill_report, plus both installed summary
// folds. Month ends and both expected ratios are built independently here.
void check_report_walk(const std::vector<pf_equity_point_t>& points,
                       const char* zone, std::int64_t offset_seconds) {
    std::vector<double> month_ends;
    std::int64_t previous_key = 0;
    metrics::EquityMetricsFold first;
    for (const auto& point : points) {
        const std::int64_t expected = gregorian_key(point.time_ms / 1000 + offset_seconds);
        if (month_ends.empty() || expected != previous_key)
            month_ends.push_back(point.equity);
        else
            month_ends.back() = point.equity;
        previous_key = expected;
        first.fold(point, metrics::equity_month_key(point.time_ms, zone));
        check_key(point.time_ms, expected, first.held_month_key());
    }
    std::vector<double> returns;
    double sum = 0.0;
    for (std::size_t i = 1; i < month_ends.size(); ++i) {
        if (month_ends[i - 1] > 0.0) {
            returns.push_back(month_ends[i] / month_ends[i - 1] - 1.0);
            sum += returns.back();
        }
    }
    CHECK(returns.size() >= 2);
    const double mean = sum / static_cast<double>(returns.size());
    double variance = 0.0, downside = 0.0;
    for (double r : returns) {
        variance += (r - mean) * (r - mean);
        const double d = std::min(0.0, r - 0.02 / 12.0);
        downside += d * d;
    }
    const double sharpe = (mean - 0.02 / 12.0)
        / std::sqrt(variance / static_cast<double>(returns.size() - 1)) * std::sqrt(12.0);
    const double sortino = (mean - 0.02 / 12.0)
        / std::sqrt(downside / static_cast<double>(returns.size())) * std::sqrt(12.0);
    const metrics::EquityMetricInputs inputs{
        points.front().equity, 1.0, 1.0, points.back().equity - points.front().equity};
    const auto array = metrics::compute_equity_stats(
        points.data(), static_cast<std::int64_t>(points.size()),
        inputs.initial_capital, zone, inputs.first_open, inputs.last_close, 0, inputs.net_profit);
    check_ratio(sharpe, array.sharpe_monthly);
    check_ratio(sortino, array.sortino_monthly);
    MonthReportHost host;
    const auto report = host.read(points, zone, inputs);
    check_ratio(sharpe, report.sharpe_monthly);
    check_ratio(sortino, report.sortino_monthly);

    // Replacement must carry the full key too, in both passes.
    const auto& last = points.back();
    CHECK(first.replace_last(last, metrics::equity_month_key(last.time_ms, zone)));
    check_key(last.time_ms, previous_key, first.held_month_key());
    const auto snapshot = first.first_pass();
    CHECK(snapshot.month_returns.count == returns.size());
    CHECK(snapshot.month_returns.sum == sum);
    metrics::EquityMetricsSecondPass second(snapshot);
    for (const auto& point : points)
        second.fold(point, metrics::equity_month_key(point.time_ms, zone));
    CHECK(second.replace_last(last, metrics::equity_month_key(last.time_ms, zone)));
    const auto ratios = second.finish();
    CHECK(ratios.matches(snapshot));
    const auto folded = metrics::finalize_equity_metrics(snapshot, inputs, &ratios);
    check_ratio(sharpe, folded.sharpe_monthly);
    check_ratio(sortino, folded.sortino_monthly);
}

void report_walks() {
    constexpr std::int64_t low = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t high = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t edge_ms = (std::int64_t{1} << 40) * 1000;
    const std::vector<pf_equity_point_t> extremes = {
        {low, 100.0, 0.0}, {low + 1, 120.0, 0.0}, {-2 * edge_ms, 90.0, 0.0},
        {-edge_ms, 110.0, 0.0}, {-edge_ms + 1000, 105.0, 0.0},
        {-1000, 100.0, 0.0}, {0, 125.0, 0.0},
        {edge_ms - 1000, 110.0, 0.0}, {edge_ms, 100.0, 0.0},
        {2 * edge_ms, 120.0, 0.0}, {high, 90.0, 0.0},
    };
    // These January/May keys differ by 357913941 * 12 + 4 = 2^32.
    // An int key anywhere in either walk would merge distinct months.
    const std::int64_t a = month_open_seconds(-178956970, 1) * 1000;
    const std::int64_t b = month_open_seconds(178956971, 5) * 1000;
    const std::int64_t c = month_open_seconds(178956971, 6) * 1000;
    CHECK(gregorian_key(b / 1000) - gregorian_key(a / 1000) == (std::int64_t{1} << 32));
    const std::vector<pf_equity_point_t> collisions = {
        {a, 100.0, 0.0}, {a + 1000, 120.0, 0.0},
        {b, 110.0, 0.0}, {b + 1000, 90.0, 0.0}, {c, 125.0, 0.0},
    };
    for (const auto* points : {&extremes, &collisions}) {
        for (const char* zone : {"", "UTC", "Etc/UTC", "UTC0"})
            check_report_walk(*points, zone, 0);
        check_report_walk(*points, "EST12", -12 * 3600);
        check_report_walk(*points, "EET-14", 14 * 3600);
    }
}

}  // namespace

int main() {
    // Run the overflow witness first: on the old implementation UBSan reports
    // signed int overflow in (tm_year + 1900) * 12, before the long sweeps.
    full_timestamp_range();
    report_walks();
    epoch_and_leap_days();
    every_hour_1970_to_2100();
    every_day_year_1_to_9999();
    every_month_boundary_1900_to_2100();
    both_sides_of_the_cache_edge();
    if (checks < 4000000 / kFarStride) {
        std::fprintf(stderr, "FAIL too few checks: %lld\n", checks);
        ++failures;
    }
    if (failures == 0) {
        std::printf("test_utc_month_key_arithmetic: ok (%lld checks against Gregorian cycles "
                    "and in-range gmtime_r)\n", checks);
    }
    return failures == 0 ? 0 : 1;
}
