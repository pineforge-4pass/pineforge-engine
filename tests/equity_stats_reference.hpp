#pragma once

// R5 lane D2-D: the report's equity statistics as src/engine_metrics.cpp
// computed them at 6c081f5d, before the monthly walk read its UTC month key
// through a memo -- month_key_utc, month_key_local, sharpe_sortino and
// compute_equity_stats transcribed verbatim, renamed into this namespace. The
// rows that hold the memoized walk to it (tests/test_utc_month_memo.cpp,
// tests/test_chart_day_memo.cpp) compare every field bit for bit. Source-free.

#include <pineforge/pineforge.h>

#include "../src/timezone.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

namespace equity_stats_reference {

using pineforge::tz_util::ScopedTimezone;

inline int month_key_local(int64_t ts_ms) {   // call ONLY under ScopedTimezone
    time_t secs = (time_t)(ts_ms / 1000);
    struct tm tb {};
    localtime_r(&secs, &tb);
    return (tb.tm_year + 1900) * 12 + tb.tm_mon;
}

inline void sharpe_sortino(const std::vector<double>& r, double rf_period,
                           double ann, double* sharpe, double* sortino) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    *sharpe = kNaN;
    *sortino = kNaN;
    const size_t n = r.size();
    if (n < 2) return;
    double mean = 0.0;
    for (double x : r) mean += x;
    mean /= (double)n;
    double var = 0.0, down = 0.0;
    for (double x : r) {
        var += (x - mean) * (x - mean);
        const double d = std::min(0.0, x - rf_period);
        down += d * d;
    }
    const double sd = std::sqrt(var / (double)(n - 1));
    const double dd = std::sqrt(down / (double)n);
    if (sd > 0.0) *sharpe = (mean - rf_period) / sd * ann;
    if (dd > 0.0) *sortino = (mean - rf_period) / dd * ann;
}

inline int month_key_utc(int64_t ts_ms) {
    time_t secs = (time_t)(ts_ms / 1000);
    constexpr int64_t kCivilSpan = int64_t{1} << 40;
    if (secs > -kCivilSpan && secs < kCivilSpan) {
        const int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
        const int64_t z = days + 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const int64_t doe = z - era * 146097;
        const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const int64_t mp = (5 * doy + 2) / 153;
        const int64_t month = mp < 10 ? mp + 3 : mp - 9;
        const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
        return static_cast<int>(year * 12 + month - 1);
    }
    struct tm tb {};
    gmtime_r(&secs, &tb);
    return (tb.tm_year + 1900) * 12 + tb.tm_mon;
}

inline pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                              double initial_capital,
                                              const std::string& chart_tz,
                                              double first_open, double last_close,
                                              int64_t bars_in_market, double net_profit) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    pf_equity_stats_t e{};
    e.sharpe_monthly = kNaN; e.sortino_monthly = kNaN; e.sharpe_bar = kNaN; e.sortino_bar = kNaN;
    e.cagr = kNaN; e.calmar = kNaN; e.recovery_factor = kNaN;
    e.buy_hold_return = kNaN; e.buy_hold_return_pct = kNaN;
    e.time_in_market_pct = kNaN; e.open_pl = 0.0;

    if (std::isfinite(first_open) && first_open > 0.0 && std::isfinite(last_close)) {
        e.buy_hold_return_pct = (last_close / first_open - 1.0) * 100.0;
        e.buy_hold_return = initial_capital * (last_close / first_open - 1.0);
    }
    if (n <= 0 || curve == nullptr) return e;

    e.open_pl = curve[n - 1].open_profit;
    e.time_in_market_pct = (double)bars_in_market / (double)n * 100.0;

    double peak = curve[0].equity, trough = curve[0].equity;
    for (int64_t i = 0; i < n; ++i) {
        const double eq = curve[i].equity;
        if (eq > peak) { peak = eq; trough = eq; }
        if (eq < trough) trough = eq;
        const double dd = peak - eq;
        if (dd > e.max_equity_drawdown) {
            e.max_equity_drawdown = dd;
            e.max_equity_drawdown_pct = peak > 0.0 ? dd / peak * 100.0 : kNaN;
        }
        const double ru = eq - trough;
        if (ru > e.max_equity_runup) {
            e.max_equity_runup = ru;
            e.max_equity_runup_pct = trough > 0.0 ? ru / trough * 100.0 : kNaN;
        }
    }

    const double span_years =
        (double)(curve[n - 1].time_ms - curve[0].time_ms) / (365.25 * 86400.0 * 1000.0);

    if (span_years > 0.0 && initial_capital > 0.0 && curve[n - 1].equity > 0.0) {
        e.cagr = (std::pow(curve[n - 1].equity / initial_capital, 1.0 / span_years)
                  - 1.0) * 100.0;
    }
    if (e.max_equity_drawdown > 0.0) {
        e.recovery_factor = net_profit / e.max_equity_drawdown;
        if (!std::isnan(e.cagr) && e.max_equity_drawdown_pct > 0.0)
            e.calmar = e.cagr / e.max_equity_drawdown_pct;
    }

    {
        std::vector<double> month_end;
        const bool utc = chart_tz.empty() || chart_tz == "UTC" || chart_tz == "Etc/UTC";
        auto walk = [&](auto key_fn) {
            int cur = key_fn(curve[0].time_ms);
            double last_eq = curve[0].equity;
            for (int64_t i = 1; i < n; ++i) {
                const int k = key_fn(curve[i].time_ms);
                if (k != cur) { month_end.push_back(last_eq); cur = k; }
                last_eq = curve[i].equity;
            }
            month_end.push_back(last_eq);
        };
        if (utc) {
            walk(month_key_utc);
        } else {
            ScopedTimezone guard(chart_tz);   // ONE guard for the whole walk
            walk(month_key_local);
        }
        std::vector<double> r;
        for (size_t i = 1; i < month_end.size(); ++i)
            if (month_end[i - 1] > 0.0) r.push_back(month_end[i] / month_end[i - 1] - 1.0);
        sharpe_sortino(r, 0.02 / 12.0, std::sqrt(12.0), &e.sharpe_monthly, &e.sortino_monthly);
    }

    if (n >= 3 && span_years > 0.0) {
        const double bars_per_year = (double)(n - 1) / span_years;
        std::vector<double> r;
        r.reserve((size_t)(n - 1));
        for (int64_t i = 1; i < n; ++i)
            if (curve[i - 1].equity > 0.0) r.push_back(curve[i].equity / curve[i - 1].equity - 1.0);
        sharpe_sortino(r, 0.02 / bars_per_year, std::sqrt(bars_per_year),
                       &e.sharpe_bar, &e.sortino_bar);
    }
    return e;
}

// Every field of two statistics blocks, bit for bit (pf_equity_stats_t is
// fifteen doubles, no padding). The first differing field's name, or null.
inline const char* first_difference(const pf_equity_stats_t& a, const pf_equity_stats_t& b) {
    static_assert(sizeof(pf_equity_stats_t) == 15 * sizeof(double), "fifteen doubles");
    static const char* const names[] = {
        "max_equity_drawdown", "max_equity_drawdown_pct", "max_equity_runup",
        "max_equity_runup_pct", "buy_hold_return", "buy_hold_return_pct",
        "sharpe_monthly", "sortino_monthly", "sharpe_bar", "sortino_bar", "cagr",
        "calmar", "recovery_factor", "time_in_market_pct", "open_pl",
    };
    double x[15];
    double y[15];
    std::memcpy(x, &a, sizeof x);
    std::memcpy(y, &b, sizeof y);
    for (int i = 0; i < 15; ++i) {
        if (std::memcmp(&x[i], &y[i], sizeof(double)) != 0) return names[i];
    }
    return nullptr;
}

}  // namespace equity_stats_reference
