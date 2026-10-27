/*
 * engine_metrics.cpp — pure metric computations (trade stats + equity
 * stats) consumed by fill_report. Conventions (NaN rules, sign/percent
 * bases, annualization) are pinned in the per-field doxygen of
 * <pineforge/pineforge.h>; docs/pages/report-schema.md summarizes them.
 */
#include <pineforge/metrics_stream.hpp>
#include <pineforge/metrics_fold.hpp>
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
#include "metrics_annualization.hpp"
#include "report_value_invalid.hpp"
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
                              double* sharpe, double* sortino,
                              const char* sharpe_field, const char* sortino_field) {
    *sharpe = kNaN;
    *sortino = kNaN;
    std::size_t n = 0;
    double sum = 0.0;
    const char* sum_failure = nullptr;
    generate([&](double value) {
        // Main leaves a series shorter than two undefined. Remember each
        // return/sum check until that count is known, without retaining rows.
        fold_detail::defer_nonfinite(value, sharpe_field, sum_failure);
        sum = fold_detail::defer_nonfinite(sum + value, sharpe_field, sum_failure);
        ++n;
    });
    if (n < 2) return;
    report::finite_value(rf_period, sharpe_field);
    report::finite_value(ann, sharpe_field);
    if (sum_failure) report::finite_value(kNaN, sum_failure);
    report::finite_value(sum, sharpe_field);
    const double mean = report::finite_value(sum / (double)n, sharpe_field);
    double var = 0.0, down = 0.0;
    generate([&](double value) {
        report::finite_value(value, sharpe_field);
        const double delta = report::finite_value(value - mean, sharpe_field);
        var = report::finite_value(var + delta * delta, sharpe_field);
        const double d = std::min(0.0, report::finite_value(value - rf_period, sortino_field));
        down = report::finite_value(down + d * d, sortino_field);
    });
    const double sd = report::finite_value(std::sqrt(var / (double)(n - 1)), sharpe_field);
    const double dd = report::finite_value(std::sqrt(down / (double)n), sortino_field);
    if (sd > 0.0)
        *sharpe = report::finite_value((mean - rf_period) / sd * ann, sharpe_field);
    if (dd > 0.0)
        *sortino = report::finite_value((mean - rf_period) / dd * ann, sortino_field);
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

int equity_month_key(std::int64_t time_ms, const std::string& chart_timezone) {
    if (chart_timezone.empty() || chart_timezone == "UTC" || chart_timezone == "Etc/UTC")
        return detail::month_key_utc(time_ms);
    // This scope contains only the civil-month lookup, never engine dispatch.
    tz_util::ScopedTimezone guard(chart_timezone);
    return month_key_local(time_ms);
}

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
    const char* nonfinite_field = nullptr;
    const char* nonfinite_prefix = nullptr;

    State(TradeFilter selected_filter, double capital)
        : filter(selected_filter), initial_capital(capital) {
        fold_check(initial_capital, "net_profit_pct");
    }

    const char* prefix() const {
        return filter == TradeFilter::ALL ? "metrics.all" :
               filter == TradeFilter::LONG ? "metrics.longs" : "metrics.shorts";
    }

    double check(double value, const char* field) const {
        return report::finite_value(value, field, prefix());
    }

    double defer(double value, const char* field, const char* field_prefix) {
        if (!std::isfinite(value) && nonfinite_field == nullptr) {
            nonfinite_field = field;
            nonfinite_prefix = field_prefix;
        }
        return value;
    }

    double fold_check(double value, const char* field) {
        return defer(value, field, prefix());
    }

    void fold(const TradeC& t) {
        if (!match(t, filter)) return;
        pf_trade_stats_t& s = stats;
        // Independent filters can be fed together, but the report finishes
        // ALL before LONG before SHORT. Retain each walk's first failure so
        // an eager LONG failure cannot overtake a later ALL failure.
#define PF_DEFER_TRADE(field) (void)defer(t.field, "trades." #field, nullptr)
        PF_DEFER_TRADE(entry_price);
        PF_DEFER_TRADE(exit_price);
        PF_DEFER_TRADE(qty);
        PF_DEFER_TRADE(pnl);
        PF_DEFER_TRADE(pnl_pct);
        PF_DEFER_TRADE(max_runup);
        PF_DEFER_TRADE(max_drawdown);
        PF_DEFER_TRADE(commission);
#undef PF_DEFER_TRADE
        ++s.num_trades;
        s.net_profit = fold_check(s.net_profit + t.pnl, "net_profit");
        s.commission_paid = fold_check(s.commission_paid + t.commission, "commission_paid");
        pct_sum = fold_check(pct_sum + t.pnl_pct, "avg_trade_pct");
        // TV counts bars-in-trade inclusively of the entry bar (TV=9 vs the
        // exclusive diff 8, all blocks; validated 2026-06-12 vs TV export).
        const double bars = (double)(static_cast<int64_t>(t.exit_bar_index)
                                     - static_cast<int64_t>(t.entry_bar_index)) + 1.0;
        bars_sum = fold_check(bars_sum + bars, "avg_bars_in_trade");
        if (t.pnl > 0.0) {
            ++s.num_wins;
            s.gross_profit = fold_check(s.gross_profit + t.pnl, "gross_profit");
            win_pct_sum = fold_check(win_pct_sum + t.pnl_pct, "avg_win_pct");
            win_bars_sum = fold_check(win_bars_sum + bars, "avg_bars_in_wins");
            streak_l = 0;
            if (++streak_w > s.max_consecutive_wins) s.max_consecutive_wins = streak_w;
            // TV "Largest profit %" is the independent max of per-trade %,
            // NOT the % of the largest-USD win (validated 2026-06-12 vs TV
            // export). Track the two maxima separately.
            if (t.pnl > largest_win) largest_win = t.pnl;
            if (t.pnl_pct > largest_win_pct) largest_win_pct = t.pnl_pct;
        } else if (t.pnl < 0.0) {
            ++s.num_losses;
            s.gross_loss = fold_check(s.gross_loss + -t.pnl, "gross_loss");  // positive magnitude
            loss_pct_sum = fold_check(loss_pct_sum + -t.pnl_pct, "avg_loss_pct");
            loss_bars_sum = fold_check(loss_bars_sum + bars, "avg_bars_in_losses");
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
        if (nonfinite_field)
            report::finite_value(kNaN, nonfinite_field, nonfinite_prefix);
        pf_trade_stats_t s = stats;
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

pf_equity_stats_t finalize_equity_metrics(
        const EquityFoldSnapshot& first, const EquityMetricInputs& inputs,
        const EquityRatioResult* ratios) {
    const auto check = [](double value, const char* field) {
        return report::finite_value(value, field, "metrics.equity");
    };
    check(inputs.initial_capital, "buy_hold_return");
    report::finite_value(inputs.net_profit, "summary.net_pnl");
    pf_equity_stats_t e{};
    e.sharpe_monthly = kNaN; e.sortino_monthly = kNaN; e.sharpe_bar = kNaN; e.sortino_bar = kNaN;
    e.cagr = kNaN; e.calmar = kNaN; e.recovery_factor = kNaN;
    e.buy_hold_return = kNaN; e.buy_hold_return_pct = kNaN;
    e.time_in_market_pct = kNaN; e.open_pl = 0.0;

    if (std::isinf(inputs.first_open)) check(inputs.first_open, "buy_hold_return_pct");
    if (std::isinf(inputs.last_close)) check(inputs.last_close, "buy_hold_return_pct");
    if (std::isfinite(inputs.first_open) && inputs.first_open > 0.0
        && std::isfinite(inputs.last_close)) {
        e.buy_hold_return_pct = check(
            (inputs.last_close / inputs.first_open - 1.0) * 100.0, "buy_hold_return_pct");
        e.buy_hold_return = check(
            inputs.initial_capital * (inputs.last_close / inputs.first_open - 1.0), "buy_hold_return");
    }
    if (first.point_count <= 0) return e;

    // Match the array producer's order on the COMPLETED curve. In particular,
    // a nonfinite final open profit precedes an earlier invalid equity point.
    // Prefix/replay walks only remember their first invalid field; no typed
    // exception is caught, converted to a sentinel, or lost on this path.
    e.open_pl = report::finite_value(first.last.open_profit, "equity_curve.open_profit");
    e.time_in_market_pct = first.observation_count > 0
        ? check(static_cast<double>(first.bars_in_market)
                / static_cast<double>(first.observation_count) * 100.0, "time_in_market_pct") : kNaN;
    if (first.nonfinite_field)
        report::finite_value(kNaN, first.nonfinite_field);
    e.max_equity_drawdown = first.max_drawdown;
    e.max_equity_drawdown_pct = first.max_drawdown_pct;
    e.max_equity_runup = first.max_runup;
    e.max_equity_runup_pct = first.max_runup_pct;

    const double span_ms = first.span_ms();
    const double span_years = check(first.span_years(), "cagr");
    if (span_years > 0.0 && inputs.initial_capital > 0.0 && first.last.equity > 0.0) {
        const double ratio = check(first.last.equity / inputs.initial_capital, "cagr");
        if (span_ms < static_cast<double>(detail::kMinAnnualizationSpanMs)) {
            e.cagr = check((ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        } else {
            const double compounded = (std::pow(ratio, 1.0 / span_years) - 1.0) * 100.0;
            e.cagr = check(std::isfinite(compounded) ? compounded
                           : (ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        }
    }
    if (e.max_equity_drawdown > 0.0) {
        e.recovery_factor = check(inputs.net_profit / e.max_equity_drawdown, "recovery_factor");
        if (!std::isnan(e.cagr) && e.max_equity_drawdown_pct > 0.0)
            e.calmar = check(e.cagr / e.max_equity_drawdown_pct, "calmar");
    }
    if (ratios && ratios->matches(first)) {
        if (ratios->nonfinite_field)
            report::finite_value(kNaN, ratios->nonfinite_field);
        e.sharpe_monthly = ratios->sharpe_monthly;
        e.sortino_monthly = ratios->sortino_monthly;
        e.sharpe_bar = ratios->sharpe_bar;
        e.sortino_bar = ratios->sortino_bar;
    }
    return e;
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
    if (n <= 0) return e;

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
            // inversion across the one-day boundary (compounded > simple).
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
                                     &e.sharpe_monthly, &e.sortino_monthly,
                                     "metrics.equity.sharpe_monthly", "metrics.equity.sortino_monthly");
        } else {
            tz_util::ScopedTimezone guard(chart_tz);   // ONE guard for both generated passes
            const auto month_returns = [&](auto emit) {
                for_each_month_return(curve, n, month_key_local, emit);
            };
            sharpe_sortino_generated(month_returns, 0.02 / 12.0, std::sqrt(12.0),
                                     &e.sharpe_monthly, &e.sortino_monthly,
                                     "metrics.equity.sharpe_monthly", "metrics.equity.sortino_monthly");
        }
    }

    // --- Per-bar variant: density-annualized (observed bars/year, NOT the
    // calendar tf formula — that inflates Sharpe ~sqrt(5) on non-24/7
    // sessions; see spec).
    if (n >= 3 && span_years > 0.0) {
        const double bars_per_year = check((double)(n - 1) / span_years, "sharpe_bar");
        const auto bar_returns = [&](auto emit) {
            for_each_bar_return(curve, n, emit);
        };
        sharpe_sortino_generated(bar_returns, 0.02 / bars_per_year, std::sqrt(bars_per_year),
                                 &e.sharpe_bar, &e.sortino_bar,
                                 "metrics.equity.sharpe_bar", "metrics.equity.sortino_bar");
    }
    return e;
}

// ArrayCurve and the L7c SpillCurve instantiations share this metric body.
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

// L7c: emit both scratch-source entry points from the SAME metric body and
// floating-point operation order as ArrayCurve.
template pf_equity_stats_t compute_equity_stats<SpillCurve>(
    const SpillCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double);

template pf_equity_stats_t compute_equity_stats<SpillCurve>(
    const SpillCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double, int64_t);

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

// CR-150 L6b-4 read-side binding. Keep the existing metric implementation and
// its array instantiations above unchanged; L7c owns the public SpillCurve
// instantiations. The internal window can exercise the real scratch reader
// before that downstream adapter is merged.
#include "report_scratch.hpp"

namespace pineforge {
namespace metrics {

template pf_equity_stats_t compute_equity_stats<report_sink::ScratchCurveWindow>(
    const report_sink::ScratchCurveWindow&, int64_t, double, const std::string&,
    double, double, int64_t, double);

template pf_equity_stats_t compute_equity_stats<report_sink::ScratchCurveWindow>(
    const report_sink::ScratchCurveWindow&, int64_t, double, const std::string&,
    double, double, int64_t, double, int64_t);

} // namespace metrics
} // namespace pineforge
