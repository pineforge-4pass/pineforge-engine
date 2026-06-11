/*
 * engine_metrics.cpp — pure metric computations (trade stats + equity
 * stats) consumed by fill_report. Conventions: see field docs in
 * <pineforge/pineforge.h> and docs/superpowers/specs/2026-06-11-*.md.
 */
#include <pineforge/metrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <vector>

#include "timezone.hpp"

namespace pineforge {
namespace metrics {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline bool match(const TradeC& t, TradeFilter f) {
    return f == TradeFilter::ALL || (f == TradeFilter::LONG) == (t.is_long != 0);
}

// (year*12 + month) bucket key for ts in chart tz. Caller hoists ONE
// ScopedTimezone guard around the whole walk — per-point guards are a
// process-global setenv/tzset round trip each (see timezone.hpp).
inline int month_key_utc(int64_t ts_ms) {
    // ts >= 0 assumed (pre-epoch truncation shifts month for negative ts)
    time_t secs = (time_t)(ts_ms / 1000);
    struct tm tb {};
    gmtime_r(&secs, &tb);
    return (tb.tm_year + 1900) * 12 + tb.tm_mon;
}
inline int month_key_local(int64_t ts_ms) {   // call ONLY under ScopedTimezone
    // ts >= 0 assumed (pre-epoch truncation shifts month for negative ts)
    time_t secs = (time_t)(ts_ms / 1000);
    struct tm tb {};
    localtime_r(&secs, &tb);
    return (tb.tm_year + 1900) * 12 + tb.tm_mon;
}

// Sharpe/Sortino over a simple-returns series. rf_period = risk-free per
// period; ann = sqrt(periods/year). Sample stddev (N-1) for Sharpe;
// population downside deviation vs rf for Sortino (arbitration item — see
// spec; corpus probe pins it).
inline void sharpe_sortino(const std::vector<double>& r, double rf_period,
                           double ann, double* sharpe, double* sortino) {
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

}  // namespace

pf_trade_stats_t compute_trade_stats(const TradeC* trades, int n,
                                     TradeFilter filter, double initial_capital) {
    pf_trade_stats_t s{};
    double pct_sum = 0.0, win_pct_sum = 0.0, loss_pct_sum = 0.0;
    double bars_sum = 0.0, win_bars_sum = 0.0, loss_bars_sum = 0.0;
    int streak_w = 0, streak_l = 0;
    double largest_win = 0.0, largest_win_pct = 0.0;
    double largest_loss = 0.0, largest_loss_pct = 0.0;  // magnitudes

    for (int i = 0; i < n; ++i) {
        const TradeC& t = trades[i];
        if (!match(t, filter)) continue;
        ++s.num_trades;
        s.net_profit += t.pnl;
        s.commission_paid += t.commission;
        pct_sum += t.pnl_pct;
        const double bars = (double)(t.exit_bar_index - t.entry_bar_index);
        bars_sum += bars;
        if (t.pnl > 0.0) {
            ++s.num_wins;
            s.gross_profit += t.pnl;
            win_pct_sum += t.pnl_pct;
            win_bars_sum += bars;
            streak_l = 0;
            if (++streak_w > s.max_consecutive_wins) s.max_consecutive_wins = streak_w;
            if (t.pnl > largest_win) { largest_win = t.pnl; largest_win_pct = t.pnl_pct; }
        } else if (t.pnl < 0.0) {
            ++s.num_losses;
            s.gross_loss += -t.pnl;            // positive magnitude
            loss_pct_sum += -t.pnl_pct;
            loss_bars_sum += bars;
            streak_w = 0;
            if (++streak_l > s.max_consecutive_losses) s.max_consecutive_losses = streak_l;
            if (-t.pnl > largest_loss) { largest_loss = -t.pnl; largest_loss_pct = -t.pnl_pct; }
        } else {
            ++s.num_even;                      // even trades break both streaks
            streak_w = 0;
            streak_l = 0;
        }
    }

    const bool cap_ok = initial_capital > 0.0;
    s.net_profit_pct   = cap_ok ? s.net_profit   / initial_capital * 100.0 : kNaN;
    s.gross_profit_pct = cap_ok ? s.gross_profit / initial_capital * 100.0 : kNaN;
    s.gross_loss_pct   = cap_ok ? s.gross_loss   / initial_capital * 100.0 : kNaN;
    s.percent_profitable = s.num_trades > 0
        ? (double)s.num_wins / (double)s.num_trades * 100.0 : kNaN;
    s.profit_factor = s.gross_loss > 0.0 ? s.gross_profit / s.gross_loss : kNaN;
    s.avg_trade     = s.num_trades > 0 ? s.net_profit / s.num_trades : kNaN;
    s.avg_trade_pct = s.num_trades > 0 ? pct_sum / s.num_trades : kNaN;
    s.avg_win       = s.num_wins   > 0 ? s.gross_profit / s.num_wins : kNaN;
    s.avg_win_pct   = s.num_wins   > 0 ? win_pct_sum / s.num_wins : kNaN;
    s.avg_loss      = s.num_losses > 0 ? s.gross_loss / s.num_losses : kNaN;
    s.avg_loss_pct  = s.num_losses > 0 ? loss_pct_sum / s.num_losses : kNaN;
    s.ratio_avg_win_avg_loss =
        (s.num_wins > 0 && s.num_losses > 0 && s.avg_loss > 0.0)
            ? s.avg_win / s.avg_loss : kNaN;
    s.largest_win      = s.num_wins   > 0 ? largest_win      : kNaN;
    s.largest_win_pct  = s.num_wins   > 0 ? largest_win_pct  : kNaN;
    s.largest_loss     = s.num_losses > 0 ? largest_loss     : kNaN;
    s.largest_loss_pct = s.num_losses > 0 ? largest_loss_pct : kNaN;
    s.expectancy = s.num_trades > 0
        ? ((double)s.num_wins / s.num_trades) * (s.num_wins > 0 ? s.avg_win : 0.0)
          - ((double)s.num_losses / s.num_trades) * (s.num_losses > 0 ? s.avg_loss : 0.0)
        : kNaN;
    s.avg_bars_in_trade  = s.num_trades > 0 ? bars_sum / s.num_trades : kNaN;
    s.avg_bars_in_wins   = s.num_wins   > 0 ? win_bars_sum / s.num_wins : kNaN;
    s.avg_bars_in_losses = s.num_losses > 0 ? loss_bars_sum / s.num_losses : kNaN;
    return s;
}

pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit) {
    pf_equity_stats_t e{};
    e.sharpe_tv = kNaN; e.sortino_tv = kNaN; e.sharpe_bar = kNaN; e.sortino_bar = kNaN;
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

    // --- Drawdown / runup walk. MUST mirror update_equity_extremes
    // (engine.hpp): trough resets to eq on every new peak. The walk over the
    // curve reproduces the engine's scalar extremes exactly (verified by the
    // engine-vs-walk integration test in test_metrics.cpp). Percent vs the
    // peak (dd) / trough (runup) in effect when the extreme was hit.
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

    // --- Calendar span (ms -> years) ---
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

    // --- TV-method monthly Sharpe/Sortino: last point of each chart-tz
    // (year,month) bucket; simple returns between consecutive month-ends.
    // TODO(perf): decompose only at month boundaries (O(months)) instead
    // of per point; shrinks the non-UTC critical section.
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
            pine_tz::ScopedTimezone guard(chart_tz);   // ONE guard for the whole walk
            walk(month_key_local);
        }
        std::vector<double> r;
        for (size_t i = 1; i < month_end.size(); ++i)
            if (month_end[i - 1] > 0.0) r.push_back(month_end[i] / month_end[i - 1] - 1.0);
        sharpe_sortino(r, 0.02 / 12.0, std::sqrt(12.0), &e.sharpe_tv, &e.sortino_tv);
    }

    // --- Per-bar variant: density-annualized (observed bars/year, NOT the
    // calendar tf formula — that inflates Sharpe ~sqrt(5) on non-24/7
    // sessions; see spec).
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

}  // namespace metrics
}  // namespace pineforge
