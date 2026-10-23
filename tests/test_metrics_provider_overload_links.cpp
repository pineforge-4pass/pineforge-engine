// F2: exercise both installed provider overloads, including their metric
// values. The base has no bodies to run; the brief exempts F2 from a base
// RED/link test. Run this row only with the implementation, then the mutation.
#include <pineforge/metrics_stream.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
        }                                                                       \
    } while (0)

using pineforge::metrics::ArrayCurve;
using pineforge::metrics::compute_equity_stats;
using pineforge::report::EquityPointSourceV1;
using pineforge::report::EquityVisitorV1;
using pineforge::report::LogicalIndexV1;
using pineforge::report::VisitResultV1;

// Four distinct UTC/chart-civil months, nonconstant returns and a loss:
// both ratio deviations are positive, so all four ratios must be finite.
constexpr pf_equity_point_t points[] = {
    {1706659200000LL, 1000.0, 0.0},  // 2024-01-31
    {1709164800000LL, 1100.0, 5.0},  // 2024-02-29
    {1711843200000LL,  990.0, -2.0}, // 2024-03-31
    {1714435200000LL, 1050.0, 7.0},  // 2024-04-30
};
constexpr LogicalIndexV1 point_count = sizeof(points) / sizeof(points[0]);

class ProviderReadFailure final : public std::runtime_error {
public:
    ProviderReadFailure() : std::runtime_error("test provider read failed") {}
};

class FixedCurveSource final : public EquityPointSourceV1 {
public:
    bool fail_reads = false;
    mutable std::size_t reads = 0;

    LogicalIndexV1 size() const override { return point_count; }

    pf_equity_point_t at(LogicalIndexV1 index) const override {
        if (index >= size()) throw std::out_of_range("test provider index");
        if (fail_reads) throw ProviderReadFailure();
        ++reads;
        return points[index];
    }

    std::size_t read_page(LogicalIndexV1 from, pf_equity_point_t* out,
                          std::size_t capacity) const override {
        if (from > size()) throw std::out_of_range("test provider page");
        if (!out && capacity != 0)
            throw std::invalid_argument("test provider buffer");
        const auto count = static_cast<std::size_t>(
            std::min<LogicalIndexV1>(capacity, size() - from));
        for (std::size_t i = 0; i < count; ++i) out[i] = at(from + i);
        return count;
    }

    VisitResultV1 visit(LogicalIndexV1 from, LogicalIndexV1 count,
                         void* context, EquityVisitorV1 visitor) const override {
        if (from > size() || count > size() - from)
            throw std::out_of_range("test provider visit");
        if (!visitor) throw std::invalid_argument("test provider visitor");
        for (LogicalIndexV1 i = 0; i < count; ++i) {
            if (!visitor(context, from + i, at(from + i)))
                return {i + 1, false};
        }
        return {count, true};
    }
};

bool same_bits(const pf_equity_stats_t& actual, const pf_equity_stats_t& expected) {
    return std::memcmp(&actual, &expected, sizeof(actual)) == 0;
}

void provider_overloads_match_array_bits() {
    FixedCurveSource provider;
    const EquityPointSourceV1& source = provider;
    const ArrayCurve array(points, static_cast<std::int64_t>(point_count));
    const std::string timezones[] = {"UTC", "America/New_York"};
    const std::int64_t lengths[] = {0, 1, 3, 4};
    for (const auto& timezone : timezones) {
        for (const std::int64_t n : lengths) {
            const std::int64_t bars_in_market = n / 2;
            const auto expected = compute_equity_stats<ArrayCurve>(
                array, n, 1000.0, timezone, 100.0, 105.0, bars_in_market, 50.0);
            const auto actual = compute_equity_stats(
                source, n, 1000.0, timezone, 100.0, 105.0, bars_in_market, 50.0);
            CHECK(same_bits(actual, expected));

            // A selected anchor adds a point but no observation. The
            // adapter must forward M independently of its M+1 points.
            const std::int64_t observations = n > 0 ? n - 1 : 0;
            const auto selected_expected = compute_equity_stats<ArrayCurve>(
                array, n, 1000.0, timezone, 100.0, 105.0,
                bars_in_market, 50.0, observations);
            const auto selected_actual = compute_equity_stats(
                source, n, 1000.0, timezone, 100.0, 105.0,
                bars_in_market, 50.0, observations);
            CHECK(same_bits(selected_actual, selected_expected));
            if (n == 4) {
                CHECK(actual.time_in_market_pct == 50.0);
                CHECK(selected_actual.time_in_market_pct == 2.0 / 3.0 * 100.0);
                CHECK(std::isfinite(actual.sharpe_monthly));
                CHECK(std::isfinite(actual.sortino_monthly));
                CHECK(std::isfinite(actual.sharpe_bar));
                CHECK(std::isfinite(actual.sortino_bar));
            }
        }
    }
    CHECK(provider.reads != 0);
}

void provider_failures_propagate() {
    FixedCurveSource provider;
    provider.fail_reads = true;
    const EquityPointSourceV1& source = provider;
    bool ordinary_refused = false;
    bool selected_refused = false;
    try {
        (void)compute_equity_stats(
            source, 4, 1000.0, "UTC", 100.0, 105.0, 2, 50.0);
    } catch (const ProviderReadFailure&) {
        ordinary_refused = true;
    }
    try {
        (void)compute_equity_stats(
            source, 4, 1000.0, "UTC", 100.0, 105.0, 2, 50.0, 3);
    } catch (const ProviderReadFailure&) {
        selected_refused = true;
    }
    CHECK(ordinary_refused);
    CHECK(selected_refused);
    CHECK(provider.reads == 0);
}

} // namespace

int main() {
    try {
        provider_overloads_match_array_bits();
        provider_failures_propagate();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "unexpected provider test failure: %s\n", error.what());
    }
    std::printf("metrics provider overloads: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
