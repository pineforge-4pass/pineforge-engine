#pragma once

// Pure report folds. No engine, point array, scratch region, allocation, or
// callback is retained here. The caller supplies chart-civil month keys after
// dispatch, without holding a timezone guard across an engine callback.
#include <pineforge/pineforge.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace pineforge {
namespace metrics {

// Same whole-second truncation and civil-month convention as the array path.
// Non-UTC calls take and release the timezone lock within this call; callers
// must not already hold that lock. The pure folds below also accept keys from
// an independently verified calendar provider.
int equity_month_key(std::int64_t time_ms, const std::string& chart_timezone);

struct EquityReturnSummary {
    std::size_t count = 0;
    double sum = 0.0;
    void fold(double value) { sum += value; ++count; }
};

struct EquityFoldSnapshot {
    std::int64_t point_count = 0;
    std::int64_t observation_count = 0;
    std::int64_t bars_in_market = 0;
    pf_equity_point_t first{};
    pf_equity_point_t last{};
    double max_drawdown = 0.0, max_drawdown_pct = 0.0;
    double max_runup = 0.0, max_runup_pct = 0.0;
    EquityReturnSummary bar_returns, month_returns;
    // Report-only ordered witness, independent of the broker-state hash.
    std::uint64_t point_witness = 1469598103934665603ULL;

    double span_years() const {
        return static_cast<double>(last.time_ms - first.time_ms)
            / (365.25 * 86400.0 * 1000.0);
    }
    bool needs_second_pass() const {
        return month_returns.count >= 2
            || (point_count >= 3 && span_years() > 0.0 && bar_returns.count >= 2);
    }
    bool same_input(const EquityFoldSnapshot& other) const {
        return point_count == other.point_count
            && observation_count == other.observation_count
            && bars_in_market == other.bars_in_market
            && point_witness == other.point_witness
            && bar_returns.count == other.bar_returns.count
            && month_returns.count == other.month_returns.count
            && std::memcmp(&bar_returns.sum, &other.bar_returns.sum, sizeof(double)) == 0
            && std::memcmp(&month_returns.sum, &other.month_returns.sum, sizeof(double)) == 0
            && std::memcmp(&first, &other.first, sizeof(first)) == 0
            && std::memcmp(&last, &other.last, sizeof(last)) == 0;
    }
};

namespace fold_detail {
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

// The state of the array path's two return generators, in their original
// order. A bucket closes on a KEY CHANGE, including nonmonotonic timestamps.
struct ReturnSequence {
    bool have_point = false;
    bool have_previous_month = false;
    int current_key = 0;
    double previous_equity = 0.0;
    double current_month_end = 0.0, previous_month_end = 0.0;

    template<class BarEmit, class MonthEmit>
    void fold(const pf_equity_point_t& point, int key, BarEmit bar, MonthEmit month) {
        if (!have_point) {
            have_point = true;
            current_key = key;
            previous_equity = current_month_end = point.equity;
            return;
        }
        if (previous_equity > 0.0) bar(point.equity / previous_equity - 1.0);
        previous_equity = point.equity;
        if (key != current_key) {
            finish_month(month);
            previous_month_end = current_month_end;
            have_previous_month = true;
            current_key = key;
        }
        current_month_end = point.equity;
    }
    template<class MonthEmit>
    void finish_month(MonthEmit month) const {
        if (have_previous_month && previous_month_end > 0.0)
            month(current_month_end / previous_month_end - 1.0);
    }
};

inline void witness_word(std::uint64_t& hash, std::uint64_t word) {
    for (unsigned i = 0; i < 8; ++i) {
        hash ^= (word >> (8 * i)) & 255u;
        hash *= 1099511628211ULL;
    }
}
inline void witness_double(std::uint64_t& hash, double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    witness_word(hash, bits);
}

struct FirstPrefix {
    EquityFoldSnapshot values;
    ReturnSequence returns;
    double peak = 0.0, trough = 0.0;

    void fold(const pf_equity_point_t& point, int key, bool observation, bool exposed) {
        auto& s = values;
        if (s.point_count == 0) {
            s.first = point;
            peak = trough = point.equity; // FIRST point, never initial capital
        }
        ++s.point_count;
        if (observation) ++s.observation_count;
        if (observation && exposed) ++s.bars_in_market;
        s.last = point;
        const double eq = point.equity;
        if (eq > peak) { peak = eq; trough = eq; }
        if (eq < trough) trough = eq;
        const double dd = peak - eq;
        if (dd > s.max_drawdown) {
            s.max_drawdown = dd;
            s.max_drawdown_pct = peak > 0.0 ? dd / peak * 100.0 : nan;
        }
        const double ru = eq - trough;
        if (ru > s.max_runup) {
            s.max_runup = ru;
            s.max_runup_pct = trough > 0.0 ? ru / trough * 100.0 : nan;
        }
        returns.fold(point, key,
            [&](double r) { s.bar_returns.fold(r); },
            [&](double r) { s.month_returns.fold(r); });
        witness_word(s.point_witness, static_cast<std::uint64_t>(point.time_ms));
        witness_double(s.point_witness, point.equity);
        witness_double(s.point_witness, point.open_profit);
        witness_word(s.point_witness, static_cast<std::uint64_t>(key));
        witness_word(s.point_witness, observation ? 1u : 0u);
        witness_word(s.point_witness, observation && exposed ? 1u : 0u);
    }
};
} // namespace fold_detail

// Prefix excludes the held point. Replacing that point therefore requires no
// subtraction, inverse sum, or regrouping, even across a civil-month boundary.
// first_pass() closes the held month in a value copy and is repeatable.
class EquityMetricsFold {
public:
    void fold(const pf_equity_point_t& point, int month_key,
              bool observation = true, bool exposed = false) {
        if (have_held_)
            prefix_.fold(held_, held_key_, held_observation_, held_exposed_);
        held_ = point;
        held_key_ = month_key;
        held_observation_ = observation;
        held_exposed_ = exposed;
        have_held_ = true;
    }
    // Returns false on an empty fold; it never invents an observation.
    bool replace_last(const pf_equity_point_t& point, int month_key) {
        if (!have_held_) return false;
        held_ = point;
        held_key_ = month_key;
        return true;
    }
    const pf_equity_point_t* held_point() const { return have_held_ ? &held_ : nullptr; }
    int held_month_key() const { return held_key_; }
    bool held_observation() const { return held_observation_; }
    bool held_exposed() const { return held_exposed_; }
    EquityFoldSnapshot first_pass() const {
        auto result = prefix_;
        if (have_held_)
            result.fold(held_, held_key_, held_observation_, held_exposed_);
        result.returns.finish_month(
            [&](double r) { result.values.month_returns.fold(r); });
        return result.values;
    }

private:
    fold_detail::FirstPrefix prefix_;
    pf_equity_point_t held_{};
    int held_key_ = 0;
    bool held_observation_ = false, held_exposed_ = false, have_held_ = false;
};

struct EquityMetricInputs {
    double initial_capital;
    double first_open;
    double last_close;
    double net_profit; // Report row-order sum, including presented terminal rows.
};

struct EquityRatioResult {
    EquityFoldSnapshot expected, replayed;
    double sharpe_monthly = fold_detail::nan, sortino_monthly = fold_detail::nan;
    double sharpe_bar = fold_detail::nan, sortino_bar = fold_detail::nan;
    bool complete = false;
    bool matches(const EquityFoldSnapshot& first) const {
        return complete && expected.same_input(first) && replayed.same_input(first);
    }
};

namespace fold_detail {
struct ReturnVariance {
    std::size_t expected_count = 0, count = 0;
    double mean = 0.0, rf = 0.0, ann = 0.0;
    double var = 0.0, down = 0.0;
    bool enabled = false;
    void prepare(const EquityReturnSummary& first, double risk_free, double annualization,
                 bool allowed) {
        expected_count = first.count;
        enabled = allowed && first.count >= 2;
        if (!enabled) return;
        mean = first.sum / static_cast<double>(first.count);
        rf = risk_free;
        ann = annualization;
    }
    void fold(double value) {
        if (!enabled) return;
        var += (value - mean) * (value - mean);
        const double d = std::min(0.0, value - rf);
        down += d * d;
        ++count;
    }
    bool complete() const { return !enabled || count == expected_count; }
    void finish(double& sharpe, double& sortino) const {
        sharpe = sortino = nan;
        if (!enabled || !complete()) return;
        const double sd = std::sqrt(var / static_cast<double>(expected_count - 1));
        const double dd = std::sqrt(down / static_cast<double>(expected_count));
        if (sd > 0.0) sharpe = (mean - rf) / sd * ann;
        if (dd > 0.0) sortino = (mean - rf) / dd * ann;
    }
};
struct SecondPrefix {
    ReturnSequence returns;
    ReturnVariance bars, months;
    void fold(const pf_equity_point_t& point, int key) {
        returns.fold(point, key, [&](double r) { bars.fold(r); },
                     [&](double r) { months.fold(r); });
    }
};
} // namespace fold_detail

// A deterministic replay feeds finalized points in their original order.
// This class does not execute an engine, retain an input, or invoke observers.
// It also holds its last point so a replay can apply terminal/presentation
// rewrites before finish(). Both return series share this single walk.
class EquityMetricsSecondPass {
public:
    explicit EquityMetricsSecondPass(const EquityFoldSnapshot& first) : expected_(first) {
        prefix_.months.prepare(first.month_returns, 0.02 / 12.0, std::sqrt(12.0), true);
        const double span = first.span_years();
        if (first.point_count >= 3 && span > 0.0) {
            const double bars_per_year = static_cast<double>(first.point_count - 1) / span;
            prefix_.bars.prepare(first.bar_returns, 0.02 / bars_per_year,
                                 std::sqrt(bars_per_year), true);
        }
    }
    void fold(const pf_equity_point_t& point, int month_key,
              bool observation = true, bool exposed = false) {
        if (const auto* held = replay_.held_point())
            prefix_.fold(*held, replay_.held_month_key());
        replay_.fold(point, month_key, observation, exposed);
    }
    bool replace_last(const pf_equity_point_t& point, int month_key) {
        return replay_.replace_last(point, month_key);
    }
    EquityRatioResult finish() const {
        auto final = prefix_;
        if (const auto* held = replay_.held_point())
            final.fold(*held, replay_.held_month_key());
        final.returns.finish_month([&](double r) { final.months.fold(r); });
        EquityRatioResult result;
        result.expected = expected_;
        result.replayed = replay_.first_pass();
        result.complete = expected_.same_input(result.replayed)
            && final.bars.complete() && final.months.complete();
        if (result.complete) {
            final.months.finish(result.sharpe_monthly, result.sortino_monthly);
            final.bars.finish(result.sharpe_bar, result.sortino_bar);
        }
        return result;
    }

private:
    EquityFoldSnapshot expected_;
    EquityMetricsFold replay_;
    fold_detail::SecondPrefix prefix_;
};

inline pf_equity_stats_t finalize_equity_metrics(
        const EquityFoldSnapshot& first, const EquityMetricInputs& inputs,
        const EquityRatioResult* ratios = nullptr) {
    using fold_detail::nan;
    pf_equity_stats_t e{};
    e.sharpe_monthly = nan; e.sortino_monthly = nan; e.sharpe_bar = nan; e.sortino_bar = nan;
    e.cagr = nan; e.calmar = nan; e.recovery_factor = nan;
    e.buy_hold_return = nan; e.buy_hold_return_pct = nan;
    e.time_in_market_pct = nan; e.open_pl = 0.0;
    if (std::isfinite(inputs.first_open) && inputs.first_open > 0.0
        && std::isfinite(inputs.last_close)) {
        e.buy_hold_return_pct = (inputs.last_close / inputs.first_open - 1.0) * 100.0;
        e.buy_hold_return = inputs.initial_capital * (inputs.last_close / inputs.first_open - 1.0);
    }
    if (first.point_count <= 0) return e;
    e.open_pl = first.last.open_profit;
    e.time_in_market_pct = first.observation_count > 0
        ? static_cast<double>(first.bars_in_market)
            / static_cast<double>(first.observation_count) * 100.0 : nan;
    e.max_equity_drawdown = first.max_drawdown;
    e.max_equity_drawdown_pct = first.max_drawdown_pct;
    e.max_equity_runup = first.max_runup;
    e.max_equity_runup_pct = first.max_runup_pct;
    const double span_years = first.span_years();
    if (span_years > 0.0 && inputs.initial_capital > 0.0 && first.last.equity > 0.0) {
        e.cagr = (std::pow(first.last.equity / inputs.initial_capital, 1.0 / span_years)
                  - 1.0) * 100.0;
    }
    if (e.max_equity_drawdown > 0.0) {
        e.recovery_factor = inputs.net_profit / e.max_equity_drawdown;
        if (!std::isnan(e.cagr) && e.max_equity_drawdown_pct > 0.0)
            e.calmar = e.cagr / e.max_equity_drawdown_pct;
    }
    if (ratios && ratios->matches(first)) {
        e.sharpe_monthly = ratios->sharpe_monthly;
        e.sortino_monthly = ratios->sortino_monthly;
        e.sharpe_bar = ratios->sharpe_bar;
        e.sortino_bar = ratios->sortino_bar;
    }
    return e;
}

// The trade blocks come from the existing fixed accumulators. Passing no
// second pass explicitly produces first-pass fields only; callers publish
// readiness separately (needs_second_pass), never describe pending ratios as
// a completed summary.
inline pf_metrics_t finalize_metrics(
        const EquityFoldSnapshot& first, const EquityMetricInputs& inputs,
        const pf_trade_stats_t& all, const pf_trade_stats_t& longs,
        const pf_trade_stats_t& shorts, const EquityRatioResult* ratios = nullptr) {
    pf_metrics_t result{};
    result.all = all;
    result.longs = longs;
    result.shorts = shorts;
    result.equity = finalize_equity_metrics(first, inputs, ratios);
    return result;
}

} // namespace metrics
} // namespace pineforge
