/*
 * engine_metrics.cpp — pure metric computations (trade stats + equity
 * stats) consumed by fill_report. Conventions (NaN rules, sign/percent
 * bases, annualization) are pinned in the per-field doxygen of
 * <pineforge/pineforge.h>; docs/pages/report-schema.md summarizes them.
 */
#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_calendar.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory>
#include <vector>

#include "metrics_month_memo.hpp"
#include "timezone.hpp"

namespace pineforge {
namespace metrics {

ArrayCurve::ArrayCurve(const pf_equity_point_t* data, std::int64_t point_count)
    : data_(data), point_count_(point_count) {}

std::int64_t ArrayCurve::size() const {
    return point_count_;
}

pf_equity_point_t ArrayCurve::operator[](std::int64_t index) const {
    return data_[index];
}

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

// Sharpe/Sortino over a generated simple-returns series. The generator is
// called twice: once for the plain left-fold mean/count, then again for the
// variance/downside fold. This preserves the old vector walk's operation order
// without retaining either the month-end values or the returns.
template <class Generate>
void sharpe_sortino_generated(Generate generate, double rf_period, double ann,
                              double* sharpe, double* sortino) {
    *sharpe = kNaN;
    *sortino = kNaN;
    std::size_t n = 0;
    double sum = 0.0;
    generate([&](double value) {
        sum += value;
        ++n;
    });
    if (n < 2) return;
    const double mean = sum / (double)n;
    double var = 0.0, down = 0.0;
    generate([&](double value) {
        var += (value - mean) * (value - mean);
        const double d = std::min(0.0, value - rf_period);
        down += d * d;
    });
    const double sd = std::sqrt(var / (double)(n - 1));
    const double dd = std::sqrt(down / (double)n);
    if (sd > 0.0) *sharpe = (mean - rf_period) / sd * ann;
    if (dd > 0.0) *sortino = (mean - rf_period) / dd * ann;
}

// Emit the exact return sequence produced by the former month_end vector:
// close each civil bucket, compare it with the preceding closed bucket, and
// skip a return whose preceding equity is not positive. The first bucket only
// seeds the preceding value, so a one-bucket curve emits no return.
template <class PointSource, class KeyFn, class Emit>
void for_each_month_return(const PointSource& curve, std::int64_t n,
                           KeyFn key_fn, Emit emit) {
    if (n <= 0) return;

    const pf_equity_point_t first = curve[0];
    int current_key = key_fn(first.time_ms);
    double current_month_end = first.equity;
    double previous_month_end = 0.0;
    bool have_previous_month = false;

    for (std::int64_t i = 1; i < n; ++i) {
        const pf_equity_point_t point = curve[i];
        const int key = key_fn(point.time_ms);
        if (key != current_key) {
            if (have_previous_month && previous_month_end > 0.0)
                emit(current_month_end / previous_month_end - 1.0);
            previous_month_end = current_month_end;
            have_previous_month = true;
            current_key = key;
        }
        current_month_end = point.equity;
    }

    if (have_previous_month && previous_month_end > 0.0)
        emit(current_month_end / previous_month_end - 1.0);
}

// Emit the exact per-bar return sequence from the former temporary vector.
template <class PointSource, class Emit>
void for_each_bar_return(const PointSource& curve, std::int64_t n, Emit emit) {
    if (n <= 1) return;
    double previous_equity = curve[0].equity;
    for (std::int64_t i = 1; i < n; ++i) {
        const pf_equity_point_t point = curve[i];
        if (previous_equity > 0.0)
            emit(point.equity / previous_equity - 1.0);
        previous_equity = point.equity;
    }
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
    TradeStatsAccumulator accumulator(filter, initial_capital);
    for (int i = 0; i < n; ++i) accumulator.fold(trades[i]);
    return accumulator.finish();
}

// Report-only state, independent of broker decision counters. These are the
// old array loop's locals, retained across fold calls without regrouping sums.
struct TradeStatsAccumulator::State {
    TradeFilter filter;
    double initial_capital;
    pf_trade_stats_t stats{};
    double pct_sum = 0.0, win_pct_sum = 0.0, loss_pct_sum = 0.0;
    double bars_sum = 0.0, win_bars_sum = 0.0, loss_bars_sum = 0.0;
    std::int32_t streak_w = 0, streak_l = 0;
    double largest_win = 0.0, largest_win_pct = 0.0;
    double largest_loss = 0.0, largest_loss_pct = 0.0;  // magnitudes

    State(TradeFilter selected_filter, double capital)
        : filter(selected_filter), initial_capital(capital) {}

    void fold(const TradeC& t) {
        if (!match(t, filter)) return;
        pf_trade_stats_t& s = stats;
        ++s.num_trades;
        s.net_profit += t.pnl;
        s.commission_paid += t.commission;
        pct_sum += t.pnl_pct;
        // TV counts bars-in-trade inclusively of the entry bar (TV=9 vs the
        // exclusive diff 8, all blocks; validated 2026-06-12 vs TV export).
        const double bars = (double)(t.exit_bar_index - t.entry_bar_index) + 1.0;
        bars_sum += bars;
        if (t.pnl > 0.0) {
            ++s.num_wins;
            s.gross_profit += t.pnl;
            win_pct_sum += t.pnl_pct;
            win_bars_sum += bars;
            streak_l = 0;
            if (++streak_w > s.max_consecutive_wins) s.max_consecutive_wins = streak_w;
            // TV "Largest profit %" is the independent max of per-trade %,
            // NOT the % of the largest-USD win (validated 2026-06-12 vs TV
            // export). Track the two maxima separately.
            if (t.pnl > largest_win) largest_win = t.pnl;
            if (t.pnl_pct > largest_win_pct) largest_win_pct = t.pnl_pct;
        } else if (t.pnl < 0.0) {
            ++s.num_losses;
            s.gross_loss += -t.pnl;            // positive magnitude
            loss_pct_sum += -t.pnl_pct;
            loss_bars_sum += bars;
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

    pf_trade_stats_t finish() const {
        // Finalize a value copy: a subsequent fold, copy or report read must
        // still see the unfinalized prefix and its original left-fold sums.
        pf_trade_stats_t s = stats;
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
};

TradeStatsAccumulator::TradeStatsAccumulator(TradeFilter filter, double initial_capital)
    : state_(std::make_unique<State>(filter, initial_capital)) {}

TradeStatsAccumulator::~TradeStatsAccumulator() = default;

TradeStatsAccumulator::TradeStatsAccumulator(const TradeStatsAccumulator& other)
    : state_(std::make_unique<State>(*other.state_)) {}

TradeStatsAccumulator& TradeStatsAccumulator::operator=(const TradeStatsAccumulator& other) {
    if (this != &other) *state_ = *other.state_;
    return *this;
}

void TradeStatsAccumulator::fold(const TradeC& trade) {
    state_->fold(trade);
}

pf_trade_stats_t TradeStatsAccumulator::finish() const {
    return state_->finish();
}

template <class PointSource>
pf_equity_stats_t compute_equity_stats(const PointSource& curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit) {
    return compute_equity_stats<PointSource>(
        curve, n, initial_capital, chart_tz, first_open, last_close,
        bars_in_market, net_profit, n);
}

template <class PointSource>
pf_equity_stats_t compute_equity_stats(const PointSource& curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit,
                                       int64_t observation_count) {
    pf_equity_stats_t e{};
    e.sharpe_monthly = kNaN; e.sortino_monthly = kNaN; e.sharpe_bar = kNaN; e.sortino_bar = kNaN;
    e.cagr = kNaN; e.calmar = kNaN; e.recovery_factor = kNaN;
    e.buy_hold_return = kNaN; e.buy_hold_return_pct = kNaN;
    e.time_in_market_pct = kNaN; e.open_pl = 0.0;

    if (std::isfinite(first_open) && first_open > 0.0 && std::isfinite(last_close)) {
        e.buy_hold_return_pct = (last_close / first_open - 1.0) * 100.0;
        e.buy_hold_return = initial_capital * (last_close / first_open - 1.0);
    }
    if (n <= 0) return e;

    e.open_pl = curve[n - 1].open_profit;
    // Exposure is over observations, not curve points: a selected-window curve
    // carries its window anchor as point 0 (metrics.hpp), and the anchor is no
    // bar. Every other statistic below walks the n curve points.
    e.time_in_market_pct = observation_count > 0
        ? (double)bars_in_market / (double)observation_count * 100.0 : kNaN;

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

    // --- Monthly-resampled Sharpe/Sortino (sharpe_monthly / sortino_monthly,
    // the TV-calibrated construction): last point of each chart-tz
    // (year,month) bucket; simple returns between consecutive month-ends.
    // A UTC chart decomposes only at month boundaries (the key is kept with
    // its month's seconds, detail::month_key_utc); a chart timezone still
    // calls localtime_r per point under the one guard.
    {
        const bool utc = chart_tz.empty() || chart_tz == "UTC" || chart_tz == "Etc/UTC";
        if (utc) {
            const auto month_returns = [&](auto emit) {
                detail::UtcMonthMemo memo;
                for_each_month_return(
                    curve, n,
                    [&memo](int64_t ts_ms) { return detail::month_key_utc(memo, ts_ms); },
                    emit);
            };
            sharpe_sortino_generated(month_returns, 0.02 / 12.0, std::sqrt(12.0),
                                     &e.sharpe_monthly, &e.sortino_monthly);
        } else {
            tz_util::ScopedTimezone guard(chart_tz);   // ONE guard for both generated passes
            const auto month_returns = [&](auto emit) {
                for_each_month_return(curve, n, month_key_local, emit);
            };
            sharpe_sortino_generated(month_returns, 0.02 / 12.0, std::sqrt(12.0),
                                     &e.sharpe_monthly, &e.sortino_monthly);
        }
    }

    // --- Per-bar variant: density-annualized (observed bars/year, NOT the
    // calendar tf formula — that inflates Sharpe ~sqrt(5) on non-24/7
    // sessions; see spec).
    if (n >= 3 && span_years > 0.0) {
        const double bars_per_year = (double)(n - 1) / span_years;
        const auto bar_returns = [&](auto emit) {
            for_each_bar_return(curve, n, emit);
        };
        sharpe_sortino_generated(bar_returns, 0.02 / bars_per_year, std::sqrt(bars_per_year),
                                 &e.sharpe_bar, &e.sortino_bar);
    }
    return e;
}

// The public point-source contract is intentionally explicit-instantiated for
// the source owned by this lane.  L6b-4/L7c add the scratch source's matching
// instantiation once SpillCurve exists; the metric body remains one template.
template pf_equity_stats_t compute_equity_stats<ArrayCurve>(
    const ArrayCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double);

template pf_equity_stats_t compute_equity_stats<ArrayCurve>(
    const ArrayCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double, int64_t);

pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit,
                                       int64_t observation_count) {
    // Preserve the old pointer overload's early return for a null curve with
    // a positive count.  PointSource itself has a valid-range precondition.
    if (curve == nullptr && n > 0) {
        const ArrayCurve empty(nullptr, 0);
        return compute_equity_stats<ArrayCurve>(
            empty, 0, initial_capital, chart_tz, first_open, last_close,
            bars_in_market, net_profit, observation_count);
    }
    const ArrayCurve source(curve, n);
    return compute_equity_stats<ArrayCurve>(
        source, n, initial_capital, chart_tz, first_open, last_close,
        bars_in_market, net_profit, observation_count);
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

void BacktestEngine::fold_trade_percent_prefix(const Trade& trade) {
    trade_percent_prefix_.all_sum += trade.pnl_pct;
    ++trade_percent_prefix_.all_count;
    if (trade.pnl > 0.0) {
        trade_percent_prefix_.win_sum += trade.pnl_pct;
        ++trade_percent_prefix_.win_count;
    } else if (trade.pnl < 0.0) {
        trade_percent_prefix_.loss_sum += trade.pnl_pct;
        ++trade_percent_prefix_.loss_count;
    }
}

void BacktestEngine::reset_trade_percent_prefix() {
    trade_percent_prefix_ = {};
}

}  // namespace pineforge
