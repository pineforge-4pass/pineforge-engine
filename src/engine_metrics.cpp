/*
 * engine_metrics.cpp — pure metric computations (trade stats + equity
 * stats) consumed by fill_report. Conventions (NaN rules, sign/percent
 * bases, annualization) are pinned in the per-field doxygen of
 * <pineforge/pineforge.h>; docs/pages/report-schema.md summarizes them.
 */
#include <pineforge/metrics.hpp>
#include <pineforge/native_calendar.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <vector>

#include "metrics_month_memo.hpp"
#include "metrics_annualization.hpp"
#include "report_value_invalid.hpp"
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
// process-global setenv/tzset round trip each (see timezone.hpp). The UTC
// key is detail::month_key_utc below.
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
                           double ann, double* sharpe, double* sortino,
                           const char* sharpe_field, const char* sortino_field) {
    *sharpe = kNaN;
    *sortino = kNaN;
    const size_t n = r.size();
    if (n < 2) return;
    report::finite_value(rf_period, sharpe_field);
    report::finite_value(ann, sharpe_field);
    double mean = 0.0;
    for (double x : r) {
        report::finite_value(x, sharpe_field);
        mean = report::finite_value(mean + x, sharpe_field);
    }
    mean = report::finite_value(mean / (double)n, sharpe_field);
    double var = 0.0, down = 0.0;
    for (double x : r) {
        const double delta = report::finite_value(x - mean, sharpe_field);
        var = report::finite_value(var + delta * delta, sharpe_field);
        const double d = std::min(0.0, report::finite_value(x - rf_period, sortino_field));
        down = report::finite_value(down + d * d, sortino_field);
    }
    const double sd = report::finite_value(std::sqrt(var / (double)(n - 1)), sharpe_field);
    const double dd = report::finite_value(std::sqrt(down / (double)n), sortino_field);
    if (sd > 0.0)
        *sharpe = report::finite_value((mean - rf_period) / sd * ann, sharpe_field);
    if (dd > 0.0)
        *sortino = report::finite_value((mean - rf_period) / dd * ann, sortino_field);
}

}  // namespace

namespace detail {

// month_key_local's UTC twin: gmtime_r's (tm_year + 1900) * 12 + tm_mon for
// the point's whole seconds, by Howard Hinnant's civil_from_days on the
// floor day instead of one libc call per point. Exact for every second
// within +/-2^40 of the epoch (about 34,800 years each way); gmtime_r keeps
// the rest. External linkage only so tests/test_utc_month_key_arithmetic.cpp
// can sweep it against libc; not part of the installed API.
int month_key_utc(int64_t ts_ms) {
    // ts >= 0 assumed (pre-epoch truncation shifts month for negative ts)
    time_t secs = (time_t)(ts_ms / 1000);
    constexpr int64_t kCivilSpan = int64_t{1} << 40;
    if (secs > -kCivilSpan && secs < kCivilSpan) {
        const int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
        const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(days);
        return static_cast<int>(date.year * 12 + date.month - 1);
    }
    struct tm tb {};
    gmtime_r(&secs, &tb);
    return (tb.tm_year + 1900) * 12 + tb.tm_mon;
}

// R5 lane D2-D: the walk below keys every equity point, and consecutive points
// share their month for weeks of bars, so the key is kept with the seconds of
// its month. The range is the arithmetic's own civil month of the second's
// floor day, clamped to the arithmetic's span; it is kept only once both of its
// ends answer the key, and the key never decreases as the second increases (it
// is the civil order), so every second between the ends answers it too. A
// second outside the span is gmtime_r's and is never kept.
int keep_utc_month(UtcMonthMemo& memo, int64_t ts_ms) {
    const int64_t secs = static_cast<int64_t>(static_cast<time_t>(ts_ms / 1000));
    const int key = month_key_utc(ts_ms);
    constexpr int64_t kCivilSpan = int64_t{1} << 40;
    if (secs > -kCivilSpan && secs < kCivilSpan) {
        const int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
        // The civil month of the floor day, and the first day of the next.
        const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(days);
        const int64_t first = native_calendar::native_civil_days(date.year, date.month, 1);
        const int64_t next = date.month < 12
            ? native_calendar::native_civil_days(date.year, date.month + 1, 1)
            : native_calendar::native_civil_days(date.year + 1, 1, 1);
        const int64_t lo = std::max(first * 86400, -kCivilSpan + 1);
        const int64_t hi = std::min(next * 86400, kCivilSpan);
        if (month_key_utc(lo * 1000) == key && month_key_utc((hi - 1) * 1000) == key) {
            memo.lo = lo;
            memo.hi = hi;
            memo.key = key;
        }
    }
    return key;
}

}  // namespace detail

pf_trade_stats_t compute_trade_stats(const TradeC* trades, int n,
                                     TradeFilter filter, double initial_capital) {
    const char* prefix = filter == TradeFilter::ALL ? "metrics.all" :
                         filter == TradeFilter::LONG ? "metrics.longs" : "metrics.shorts";
    const auto check = [prefix](double value, const char* field) {
        return report::finite_value(value, field, prefix);
    };
    check(initial_capital, "net_profit_pct");
    pf_trade_stats_t s{};
    double pct_sum = 0.0, win_pct_sum = 0.0, loss_pct_sum = 0.0;
    double bars_sum = 0.0, win_bars_sum = 0.0, loss_bars_sum = 0.0;
    int streak_w = 0, streak_l = 0;
    double largest_win = 0.0, largest_win_pct = 0.0;
    double largest_loss = 0.0, largest_loss_pct = 0.0;  // magnitudes

    for (int i = 0; i < n; ++i) {
        const TradeC& t = trades[i];
        if (!match(t, filter)) continue;
        report::validate_trade(t);
        ++s.num_trades;
        s.net_profit = check(s.net_profit + t.pnl, "net_profit");
        s.commission_paid = check(s.commission_paid + t.commission, "commission_paid");
        pct_sum = check(pct_sum + t.pnl_pct, "avg_trade_pct");
        // TV counts bars-in-trade inclusively of the entry bar (TV=9 vs the
        // exclusive diff 8, all blocks; validated 2026-06-12 vs TV export).
        const double bars = (double)(static_cast<int64_t>(t.exit_bar_index)
                                     - static_cast<int64_t>(t.entry_bar_index)) + 1.0;
        bars_sum = check(bars_sum + bars, "avg_bars_in_trade");
        if (t.pnl > 0.0) {
            ++s.num_wins;
            s.gross_profit = check(s.gross_profit + t.pnl, "gross_profit");
            win_pct_sum = check(win_pct_sum + t.pnl_pct, "avg_win_pct");
            win_bars_sum = check(win_bars_sum + bars, "avg_bars_in_wins");
            streak_l = 0;
            if (++streak_w > s.max_consecutive_wins) s.max_consecutive_wins = streak_w;
            // TV "Largest profit %" is the independent max of per-trade %,
            // NOT the % of the largest-USD win (validated 2026-06-12 vs TV
            // export). Track the two maxima separately.
            if (t.pnl > largest_win) largest_win = t.pnl;
            if (t.pnl_pct > largest_win_pct) largest_win_pct = t.pnl_pct;
        } else if (t.pnl < 0.0) {
            ++s.num_losses;
            s.gross_loss = check(s.gross_loss + -t.pnl, "gross_loss");  // positive magnitude
            loss_pct_sum = check(loss_pct_sum + -t.pnl_pct, "avg_loss_pct");
            loss_bars_sum = check(loss_bars_sum + bars, "avg_bars_in_losses");
            streak_w = 0;
            if (++streak_l > s.max_consecutive_losses) s.max_consecutive_losses = streak_l;
            // Independent maxima, mirroring the win side. pnl_pct is net
            // return-on-cost (sign == pnl sign), so -pnl_pct is a true
            // positive magnitude for losses.
            if (-t.pnl > largest_loss) largest_loss = -t.pnl;
            if (-t.pnl_pct > largest_loss_pct) largest_loss_pct = -t.pnl_pct;
        } else {
            ++s.num_even;                      // even trades break both streaks
            streak_w = 0;
            streak_l = 0;
        }
    }

    const bool cap_ok = initial_capital > 0.0;
    s.net_profit_pct   = cap_ok ? check(s.net_profit   / initial_capital * 100.0, "net_profit_pct") : kNaN;
    s.gross_profit_pct = cap_ok ? check(s.gross_profit / initial_capital * 100.0, "gross_profit_pct") : kNaN;
    s.gross_loss_pct   = cap_ok ? check(s.gross_loss   / initial_capital * 100.0, "gross_loss_pct") : kNaN;
    s.percent_profitable = s.num_trades > 0
        ? check((double)s.num_wins / (double)s.num_trades * 100.0, "percent_profitable") : kNaN;
    s.profit_factor = s.gross_loss > 0.0 ? check(s.gross_profit / s.gross_loss, "profit_factor") : kNaN;
    s.avg_trade     = s.num_trades > 0 ? check(s.net_profit / s.num_trades, "avg_trade") : kNaN;
    s.avg_trade_pct = s.num_trades > 0 ? check(pct_sum / s.num_trades, "avg_trade_pct") : kNaN;
    s.avg_win       = s.num_wins   > 0 ? check(s.gross_profit / s.num_wins, "avg_win") : kNaN;
    s.avg_win_pct   = s.num_wins   > 0 ? check(win_pct_sum / s.num_wins, "avg_win_pct") : kNaN;
    s.avg_loss      = s.num_losses > 0 ? check(s.gross_loss / s.num_losses, "avg_loss") : kNaN;
    s.avg_loss_pct  = s.num_losses > 0 ? check(loss_pct_sum / s.num_losses, "avg_loss_pct") : kNaN;
    s.ratio_avg_win_avg_loss =
        (s.num_wins > 0 && s.num_losses > 0 && s.avg_loss > 0.0)
            ? check(s.avg_win / s.avg_loss, "ratio_avg_win_avg_loss") : kNaN;
    s.largest_win      = s.num_wins   > 0 ? largest_win      : kNaN;
    s.largest_win_pct  = s.num_wins   > 0 ? largest_win_pct  : kNaN;
    s.largest_loss     = s.num_losses > 0 ? largest_loss     : kNaN;
    s.largest_loss_pct = s.num_losses > 0 ? largest_loss_pct : kNaN;
    s.expectancy = s.num_trades > 0
        ? check(((double)s.num_wins / s.num_trades) * (s.num_wins > 0 ? s.avg_win : 0.0)
          - ((double)s.num_losses / s.num_trades) * (s.num_losses > 0 ? s.avg_loss : 0.0), "expectancy")
        : kNaN;
    s.avg_bars_in_trade  = s.num_trades > 0 ? check(bars_sum / s.num_trades, "avg_bars_in_trade") : kNaN;
    s.avg_bars_in_wins   = s.num_wins   > 0 ? check(win_bars_sum / s.num_wins, "avg_bars_in_wins") : kNaN;
    s.avg_bars_in_losses = s.num_losses > 0 ? check(loss_bars_sum / s.num_losses, "avg_bars_in_losses") : kNaN;
    return s;
}

pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit,
                                       int64_t observation_count) {
    const auto check = [](double value, const char* field) {
        return report::finite_value(value, field, "metrics.equity");
    };
    check(initial_capital, "buy_hold_return");
    report::finite_value(net_profit, "summary.net_pnl");
    pf_equity_stats_t e{};
    e.sharpe_monthly = kNaN; e.sortino_monthly = kNaN; e.sharpe_bar = kNaN; e.sortino_bar = kNaN;
    e.cagr = kNaN; e.calmar = kNaN; e.recovery_factor = kNaN;
    e.buy_hold_return = kNaN; e.buy_hold_return_pct = kNaN;
    e.time_in_market_pct = kNaN; e.open_pl = 0.0;

    // NaN is the existing missing-price sentinel. Infinity is never missing.
    if (std::isinf(first_open)) check(first_open, "buy_hold_return_pct");
    if (std::isinf(last_close)) check(last_close, "buy_hold_return_pct");
    if (std::isfinite(first_open) && first_open > 0.0 && std::isfinite(last_close)) {
        e.buy_hold_return_pct = check((last_close / first_open - 1.0) * 100.0, "buy_hold_return_pct");
        e.buy_hold_return = check(initial_capital * (last_close / first_open - 1.0), "buy_hold_return");
    }
    if (n <= 0 || curve == nullptr) return e;

    e.open_pl = report::finite_value(curve[n - 1].open_profit, "equity_curve.open_profit");
    // Exposure is over observations, not curve points: a selected-window curve
    // carries its window anchor as point 0 (metrics.hpp), and the anchor is no
    // bar. Every other statistic below walks the n curve points.
    e.time_in_market_pct = observation_count > 0
        ? check((double)bars_in_market / (double)observation_count * 100.0, "time_in_market_pct") : kNaN;

    // --- Drawdown / runup walk. MUST mirror update_equity_extremes
    // (engine.hpp): trough resets to eq on every new peak. The walk over the
    // curve reproduces the engine's scalar extremes exactly (verified by the
    // engine-vs-walk integration test in test_metrics.cpp). Percent vs the
    // peak (dd) / trough (runup) in effect when the extreme was hit.
    double peak = curve[0].equity, trough = curve[0].equity;
    for (int64_t i = 0; i < n; ++i) {
        const double eq = report::finite_value(curve[i].equity, "equity_curve.equity");
        report::finite_value(curve[i].open_profit, "equity_curve.open_profit");
        if (eq > peak) { peak = eq; trough = eq; }
        if (eq < trough) trough = eq;
        const double dd = check(peak - eq, "max_equity_drawdown");
        if (dd > e.max_equity_drawdown) {
            e.max_equity_drawdown = dd;
            e.max_equity_drawdown_pct = peak > 0.0 ? check(dd / peak * 100.0, "max_equity_drawdown_pct") : kNaN;
        }
        const double ru = check(eq - trough, "max_equity_runup");
        if (ru > e.max_equity_runup) {
            e.max_equity_runup = ru;
            e.max_equity_runup_pct = trough > 0.0 ? check(ru / trough * 100.0, "max_equity_runup_pct") : kNaN;
        }
    }

    // --- Measured curve span on the UTC epoch-ms bar clock (ms -> years) ---
    // Unsigned differences avoid signed overflow even for the full int64
    // timestamp domain. For a representable old difference the conversion
    // and the floating-point operation order are unchanged.
    const int64_t first_time = curve[0].time_ms, last_time = curve[n - 1].time_ms;
    const double span_ms = last_time >= first_time
        ? (double)(static_cast<uint64_t>(last_time) - static_cast<uint64_t>(first_time))
        : -(double)(static_cast<uint64_t>(first_time) - static_cast<uint64_t>(last_time));
    const double span_years = check(span_ms / (365.25 * 86400.0 * 1000.0), "cagr");

    if (span_years > 0.0 && initial_capital > 0.0 && curve[n - 1].equity > 0.0) {
        const double ratio = check(curve[n - 1].equity / initial_capital, "cagr");
        if (span_ms < static_cast<double>(detail::kMinAnnualizationSpanMs)) {
            e.cagr = check((ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        } else {
            const double compounded = (std::pow(ratio, 1.0 / span_years) - 1.0) * 100.0;
            // A nonfinite compounded value alone permits the proportional
            // fallback. The simple/compounded switch can cause a rare ranking
            // inversion across the one-day boundary (simple > compounded).
            e.cagr = check(std::isfinite(compounded) ? compounded
                           : (ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        }
    }
    if (e.max_equity_drawdown > 0.0) {
        e.recovery_factor = check(net_profit / e.max_equity_drawdown, "recovery_factor");
        if (!std::isnan(e.cagr) && e.max_equity_drawdown_pct > 0.0)
            e.calmar = check(e.cagr / e.max_equity_drawdown_pct, "calmar");
    }

    // --- Monthly-resampled Sharpe/Sortino (sharpe_monthly / sortino_monthly,
    // the TV-calibrated construction): last point of each chart-tz
    // (year,month) bucket; simple returns between consecutive month-ends.
    // A UTC chart decomposes only at month boundaries (the key is kept with
    // its month's seconds, detail::month_key_utc); a chart timezone still
    // calls localtime_r per point under the one guard.
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
            detail::UtcMonthMemo memo;
            walk([&memo](int64_t ts_ms) { return detail::month_key_utc(memo, ts_ms); });
        } else {
            tz_util::ScopedTimezone guard(chart_tz);   // ONE guard for the whole walk
            walk(month_key_local);
        }
        std::vector<double> r;
        for (size_t i = 1; i < month_end.size(); ++i)
            if (month_end[i - 1] > 0.0) r.push_back(month_end[i] / month_end[i - 1] - 1.0);
        sharpe_sortino(r, 0.02 / 12.0, std::sqrt(12.0), &e.sharpe_monthly, &e.sortino_monthly,
                       "metrics.equity.sharpe_monthly", "metrics.equity.sortino_monthly");
    }

    // --- Per-bar variant: density-annualized (observed bars/year, NOT the
    // calendar tf formula — that inflates Sharpe ~sqrt(5) on non-24/7
    // sessions; see spec).
    if (n >= 3 && span_years > 0.0) {
        const double bars_per_year = check((double)(n - 1) / span_years, "sharpe_bar");
        std::vector<double> r;
        r.reserve((size_t)(n - 1));
        for (int64_t i = 1; i < n; ++i)
            if (curve[i - 1].equity > 0.0) r.push_back(curve[i].equity / curve[i - 1].equity - 1.0);
        sharpe_sortino(r, 0.02 / bars_per_year, std::sqrt(bars_per_year),
                       &e.sharpe_bar, &e.sortino_bar,
                       "metrics.equity.sharpe_bar", "metrics.equity.sortino_bar");
    }
    return e;
}

// The ordinary entry: every point of the curve is an observation.
pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit) {
    return compute_equity_stats(curve, n, initial_capital, chart_tz, first_open, last_close,
                                bars_in_market, net_profit, n);
}

}  // namespace metrics
}  // namespace pineforge
