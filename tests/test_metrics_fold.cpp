// O1W fail-before: compile this unit against 583c2217d2's header closure.
// The first diagnostic must be the missing pineforge/metrics_fold.hpp.
// The supervisor records that remote diagnostic; no local build is allowed.
#include <pineforge/metrics_fold.hpp>

// metrics_fold.hpp must remain usable without the engine header.
#ifdef PINEFORGE_HAS_SCRIPT_RUN_PREPARE_V1
#error metrics_fold.hpp unexpectedly includes the engine
#endif

#include <pineforge/metrics_stream.hpp>
#include <pineforge/run_failure.hpp>

#include "equity_stats_reference.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
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

template<class Compute>
void nonfinite(const char* field, Compute compute) {
    try {
        compute();
        CHECK(false);
    } catch (const std::exception& error) {
        const auto failure = pineforge::classify_run_failure(error);
        CHECK(failure.code == pineforge::RunFailureCode::report_result_invalid);
        CHECK(pineforge::run_failure_code_class(failure.code)
              == pineforge::RunFailureClass::engine_fault);
        CHECK(!pineforge::run_failure_code_retryable(failure.code));
        CHECK(failure.args && *failure.args ==
              std::string("{\"field\":\"") + field + "\",\"reason\":\"nonfinite\"}");
    }
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
            // A provisional held defect must disappear with its replacement,
            // even after first_pass() has inspected it in a value copy.
            auto invalid = points.back();
            invalid.equity = std::numeric_limits<double>::quiet_NaN();
            invalid.open_profit = std::numeric_limits<double>::infinity();
            CHECK(fold.replace_last(invalid, equity_month_key(invalid.time_ms, zone)));
            CHECK(fold.first_pass().point_count == n);
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

    // R13: the production array producers in this run are the oracle.
    // Nonfinite inputs have explicit typed-failure expectations; the frozen
    // pre-change NaN propagation and compounded sub-day CAGR are superseded.
    const pf_equity_point_t* array_points = points.empty() ? nullptr : points.data();
    pf_metrics_t array{};
    pf_metrics_t streamed{};
    struct Block {
        TradeFilter filter;
        pf_trade_stats_t pf_metrics_t::*member;
        const char* capital_field;
    };
    const Block blocks[] = {
        {TradeFilter::ALL, &pf_metrics_t::all, "metrics.all.net_profit_pct"},
        {TradeFilter::LONG, &pf_metrics_t::longs, "metrics.longs.net_profit_pct"},
        {TradeFilter::SHORT, &pf_metrics_t::shorts, "metrics.shorts.net_profit_pct"},
    };
    for (const auto& block : blocks) {
        const auto array_stats = [&] {
            return compute_trade_stats(rows.data(), static_cast<int>(rows.size()),
                                       block.filter, capital);
        };
        const auto folded_stats = [&] {
            TradeStatsAccumulator accumulator(block.filter, capital);
            for (const auto& row : rows) accumulator.fold(row);
            return accumulator.finish();
        };
        if (!std::isfinite(capital)) {
            nonfinite(block.capital_field, array_stats);
            nonfinite(block.capital_field, folded_stats);
        } else {
            array.*block.member = array_stats();
            streamed.*block.member = folded_stats();
            CHECK(std::memcmp(&(array.*block.member), &(streamed.*block.member),
                              sizeof(pf_trade_stats_t)) == 0);
        }
    }
    const auto array_equity = [&] {
        return compute_equity_stats(array_points, n, capital, zone,
            inputs.first_open, inputs.last_close, exposed, inputs.net_profit,
            first.observation_count);
    };
    const char* equity_failure = nullptr;
    if (!std::isfinite(capital)) {
        equity_failure = "metrics.equity.buy_hold_return";
    } else if (n > 0 && !std::isfinite(points.back().open_profit)) {
        equity_failure = "equity_curve.open_profit";
    } else {
        for (const auto& point : points) {
            if (!std::isfinite(point.equity)) {
                equity_failure = "equity_curve.equity";
                break;
            }
            if (!std::isfinite(point.open_profit)) {
                equity_failure = "equity_curve.open_profit";
                break;
            }
        }
    }
    if (equity_failure) {
        nonfinite(equity_failure, array_equity);
        nonfinite(equity_failure, [&] {
            return finalize_metrics(first, inputs, streamed.all, streamed.longs, streamed.shorts);
        });
    } else {
        array.equity = array_equity();
        // Mask ONLY the four pending ratios in the live array oracle.
        auto first_only = array;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        first_only.equity.sharpe_monthly = first_only.equity.sortino_monthly = nan;
        first_only.equity.sharpe_bar = first_only.equity.sortino_bar = nan;
        const auto got_first = finalize_metrics(
            first, inputs, streamed.all, streamed.longs, streamed.shorts);
        CHECK(std::memcmp(&got_first, &first_only, sizeof(array)) == 0);
    }

    EquityMetricsSecondPass second(first);
    for (std::int64_t i = 0; i < n; ++i) {
        const bool observation = !anchor || i != 0;
        const bool in_market = observation && i % 3 == 1;
        auto point = points[static_cast<std::size_t>(i)];
        if (rewrite && i == n - 1) point.equity += 9999.0;
        second.fold(point, equity_month_key(point.time_ms, zone), observation, in_market);
        if (rewrite && i == n - 1) {
            auto invalid = points.back();
            invalid.equity = std::numeric_limits<double>::quiet_NaN();
            invalid.open_profit = std::numeric_limits<double>::infinity();
            CHECK(second.replace_last(invalid, equity_month_key(invalid.time_ms, zone)));
            CHECK(!second.finish().matches(first));
            second.replace_last(points.back(), equity_month_key(points.back().time_ms, zone));
        }
        if (i % 31 == 0 && i < n - 1) CHECK(!second.finish().complete);
    }
    const auto ratios = second.finish();
    CHECK(ratios.matches(first));
    static_assert(sizeof(pf_metrics_t) == 768, "raw metric contract");
    const auto again = second.finish();
    if (equity_failure) {
        nonfinite(equity_failure, [&] {
            return finalize_metrics(first, inputs, streamed.all, streamed.longs,
                                    streamed.shorts, &ratios);
        });
        nonfinite(equity_failure, [&] {
            return finalize_metrics(fold.first_pass(), inputs, streamed.all, streamed.longs,
                                    streamed.shorts, &again);
        });
    } else {
        const auto got = finalize_metrics(
            first, inputs, streamed.all, streamed.longs, streamed.shorts, &ratios);
        CHECK(std::memcmp(&got, &array, 768) == 0);
        const auto repeated = finalize_metrics(
            fold.first_pass(), inputs, streamed.all, streamed.longs, streamed.shorts, &again);
        CHECK(std::memcmp(&repeated, &array, 768) == 0);
    }

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

void finalize_annualization_span_rule() {
    constexpr std::int64_t time = 1704067200000LL;
    constexpr std::int64_t day = 86'400'000LL; // Report schema's strict boundary.
    constexpr double capital = 10000.0;
    struct Case {
        const char* name;
        std::int64_t span;
        double final_equity;
        bool finite_compounded;
        bool proportional;
    };
    const Case cases[] = {
        {"seven_minutes_pow_overflow", 7 * 60'000, 10800.0, false, true},
        {"six_hours_finite_pow", 6 * 60 * 60'000, 10050.0, true, true},
        {"one_ms_under_day", day - 1, 10050.0, true, true},
        {"exactly_one_day", day, 10050.0, true, false},
        {"one_ms_over_day", day + 1, 10050.0, true, false},
        {"one_day_pow_overflow", day, 100000.0, false, true},
    };
    for (const auto& test : cases) {
        const int failures_before = failures;
        const pf_equity_point_t curve[] = {
            {time, capital, 0.0}, {time + 60'000, 9900.0, 0.0},
            {time + test.span, test.final_equity, 0.0},
        };
        EquityMetricsFold fold;
        for (const auto& point : curve)
            fold.fold(point, equity_month_key(point.time_ms, "UTC"));
        const auto first = fold.first_pass();
        const EquityMetricInputs inputs{capital, 1.0, 1.0, test.final_equity - capital};
        CHECK(first.point_count == 3 && first.first.equity == inputs.initial_capital);
        CHECK(first.last.time_ms - first.first.time_ms == test.span);
        CHECK(first.nonfinite_field == nullptr);

        // Independent C9 transcription of report-schema.md:554-559, not the
        // live array producer. Explicit row expectations also pin the rule
        // without sharing the reference's annualization boundary constant.
        const auto want = equity_stats_reference::compute_equity_stats(
            curve, 3, capital, "UTC", 1.0, 1.0, 0, inputs.net_profit);
        // Document arithmetic: 31,557,600,000 ms/year, percent = 100 * return.
        // Compute both forms even for sub-day rows to prove discrimination.
        const double periods = 1.0 / (static_cast<double>(test.span) / 31'557'600'000.0);
        const double gain = test.final_equity / capital - 1.0;
        const double simple = 100.0 * (periods * gain);
        const double power = std::pow(test.final_equity / capital, periods);
        const double compounded = 100.0 * (power - 1.0);
        CHECK(std::isfinite(simple));
        CHECK(std::isfinite(power) == test.finite_compounded);
        CHECK(std::isfinite(compounded) == test.finite_compounded);
        CHECK(simple != compounded);
        const double expected_cagr = test.proportional ? simple : compounded;
        const double drawdown_pct = (capital - curve[1].equity) / capital * 100.0;
        const double expected_calmar = expected_cagr / drawdown_pct;
        CHECK(drawdown_pct > 0.0);
        CHECK(std::isfinite(want.cagr) && std::isfinite(want.calmar));
        CHECK(std::memcmp(&want.cagr, &expected_cagr, sizeof(double)) == 0);
        CHECK(std::memcmp(&want.calmar, &expected_calmar, sizeof(double)) == 0);
        try {
            // Exercise the ordinary report's finalizer directly; no second
            // pass is needed for CAGR/Calmar. This must cover the first site
            // even when compute_equity_stats retains its correct branch.
            const auto got = finalize_equity_metrics(first, inputs);
            CHECK(std::isfinite(got.cagr) && std::isfinite(got.calmar));
            CHECK(std::memcmp(&got.cagr, &want.cagr, sizeof(double)) == 0);
            CHECK(std::memcmp(&got.calmar, &want.calmar, sizeof(double)) == 0);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "annualization finalizer: %s\n", error.what());
            CHECK(false);
        }
        if (failures != failures_before)
            std::fprintf(stderr, "annualization control: %s\n", test.name);
    }
}

void failure_order() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double max = std::numeric_limits<double>::max();
    constexpr std::int64_t time = 1704067200000LL;
    constexpr std::int64_t year = 31'557'600'000LL;
    constexpr std::int64_t day = 86'400'000LL;
    struct Case {
        std::vector<pf_equity_point_t> points;
        EquityMetricInputs inputs;
        const char* field;
    };
    const Case cases[] = {
        // The completed curve's final open profit precedes an earlier equity
        // defect. Within the ordered walk, the first point/operation wins.
        {{{time, 1, 0}, {time + 1, nan, 0}, {time + 2, 1, 0}, {time + 3, 1, inf}},
         {1, 1, 1, 0}, "equity_curve.open_profit"},
        {{{time, 1, 0}, {time + 1, 1, nan}, {time + 2, inf, 0}, {time + 3, 1, 0}},
         {1, 1, 1, 0}, "equity_curve.open_profit"},
        {{{time, 1, 0}, {time + 1, nan, inf}, {time + 2, 1, 0}},
         {1, 1, 1, 0}, "equity_curve.equity"},
        {{{time, max, 0}, {time + 1, -max, 0}, {time + 2, nan, 0}},
         {1, 1, 1, 0}, "metrics.equity.max_equity_drawdown"},
        {{{time, nan, 0}, {time + 1, 1, inf}},
         {nan, 1, 1, 0}, "metrics.equity.buy_hold_return"},
        {{{time, nan, 0}, {time + 1, 1, inf}},
         {1, inf, 1, 0}, "metrics.equity.buy_hold_return_pct"},
        {{{time, nan, 0}, {time + 1, 1, inf}},
         {1, inf, 1, inf}, "summary.net_pnl"},
        // Main's overflow proof supplies these variance/downside inputs and
        // exact failure fields. Monthly completion precedes bar completion.
        {{{time, 1, 0}, {time + year, 1e200, 0}, {time + 2 * year, 1, 0}},
         {1, 1, 1, 0}, "metrics.equity.sharpe_monthly"},
        {{{time, 1, 0}, {time + day, 1e200, 0}, {time + 2 * day, 1, 0}},
         {1, 1, 1, 0}, "metrics.equity.sharpe_bar"},
        {{{time, 1, 0}, {time + year, -1e200, 0},
          {time + 2 * year, 1, 0}, {time + 3 * year, -1e200, 0}},
         {1, 1, 1, 0}, "metrics.equity.sortino_monthly"},
        {{{time, 1, 0}, {time + day, -1e200, 0},
          {time + 2 * day, 1, 0}, {time + 3 * day, -1e200, 0}},
         {1, 1, 1, 0}, "metrics.equity.sortino_bar"},
    };
    for (const auto& test : cases) {
        EquityMetricsFold fold;
        for (const auto& point : test.points)
            fold.fold(point, equity_month_key(point.time_ms, "UTC"));
        const auto first = fold.first_pass();
        EquityMetricsSecondPass second(first);
        for (const auto& point : test.points)
            second.fold(point, equity_month_key(point.time_ms, "UTC"));
        const auto ratios = second.finish();
        CHECK(ratios.matches(first));
        nonfinite(test.field, [&] {
            return compute_equity_stats(test.points.data(),
                static_cast<std::int64_t>(test.points.size()), test.inputs.initial_capital,
                "UTC", test.inputs.first_open, test.inputs.last_close, 0, test.inputs.net_profit);
        });
        nonfinite(test.field, [&] { return finalize_equity_metrics(first, test.inputs, &ratios); });
        nonfinite(test.field, [&] {
            const auto again = second.finish();
            return finalize_equity_metrics(fold.first_pass(), test.inputs, &again);
        });
    }
    // A LONG-only commission sum overflows before a later row violates ALL's
    // input domain. Main finishes ALL first even when the folds are fed together.
    std::vector<pineforge::TradeC> rows(4);
    rows[0].is_long = rows[2].is_long = 1;
    rows[0].commission = rows[2].commission = 1e308;
    rows[1].commission = -1e308;
    rows[3].entry_price = inf;
    nonfinite("trades.entry_price", [&] {
        (void)compute_trade_stats(rows.data(), static_cast<int>(rows.size()), TradeFilter::ALL, 1);
        (void)compute_trade_stats(rows.data(), static_cast<int>(rows.size()), TradeFilter::LONG, 1);
    });
    nonfinite("trades.entry_price", [&] {
        TradeStatsAccumulator all(TradeFilter::ALL, 1), longs(TradeFilter::LONG, 1);
        for (const auto& row : rows) {
            all.fold(row);
            longs.fold(row);
        }
        (void)all.finish();
        (void)longs.finish();
    });
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
        // Main's compounded-overflow fallback inputs, compared with its live
        // array producer rather than a hard-coded expected CAGR.
        compare({{1704067200000LL, 1, 0}, {1704153600000LL, 1e300, 0}},
                zone, 1.0, false, true);
        compare({{-5'000'000'000'000'000'000LL, 1, 0}, {0, 1, 0},
                 {5'000'000'000'000'000'000LL, 1, 0}},
                zone, 1.0, false, true);
    }
}
} // namespace

int main() {
    finalize_annualization_span_rule();
    cases();
    failure_order();
    std::printf("metrics fold: first=%zu bytes, second=%zu bytes; %d checks, %d failures\n",
                sizeof(EquityMetricsFold), sizeof(EquityMetricsSecondPass), checks, failures);
    return failures ? 1 : 0;
}
