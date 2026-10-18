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
    if (n <= 0 || curve == nullptr) return e;

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
        sharpe_sortino(r, 0.02 / 12.0, std::sqrt(12.0), &e.sharpe_monthly, &e.sortino_monthly);
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

#if defined(PINEFORGE_TEST_TRADE_PERCENT_PREFIX)
#include <cstdio>
#include <cstring>

namespace {

using pineforge::Trade;

int prefix_checks = 0;
int prefix_failures = 0;

void check_prefix(bool ok, const char* expression, int line) {
    ++prefix_checks;
    if (!ok) {
        ++prefix_failures;
        std::fprintf(stderr, "trade percent prefix: line %d: %s\n", line, expression);
    }
}

#define PREFIX_CHECK(expression) check_prefix((expression), #expression, __LINE__)

// The vector branch proves L8B without the container-swap lane. The other
// overload uses TradeLedger's frozen storage operation after integration.
template <class Rows>
auto retire_test_rows(Rows& rows, std::size_t count, int)
    -> decltype(rows.seal(rows.base() + count), void()) {
    rows.seal(rows.base() + count);
}

template <class Rows>
void retire_test_rows(Rows& rows, std::size_t count, long) {
    rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(count));
}

class PercentPrefixProbe final : public pineforge::BacktestEngine {
public:
    PercentPrefixProbe() : BacktestEngine(NativeConsumerBindTag{}) {}
    void on_bar(const pineforge::Bar&) override {}

    using BacktestEngine::avg_trade_percent;
    using BacktestEngine::avg_winning_trade_percent;
    using BacktestEngine::avg_losing_trade_percent;

    void append(const Trade& row) {
        trades_.push_back(row);
        if (row.pnl > 0.0) ++win_trades_count_;
        if (row.pnl < 0.0) ++loss_trades_count_;
    }

    void retire(std::size_t count) {
        std::size_t folded = 0;
        for (const auto& row : trades_) {
            if (folded == count) break;
            fold_trade_percent_prefix(row);
            ++folded;
        }
        PREFIX_CHECK(folded == count);
        retire_test_rows(trades_, count, 0);
    }

    std::size_t resident_count() const {
        std::size_t count = 0;
        for (const auto& row : trades_) { (void)row; ++count; }
        return count;
    }

    void replace_last(const Trade& row) {
        const auto& old = trades_.back();
        if (old.pnl > 0.0) --win_trades_count_;
        if (old.pnl < 0.0) --loss_trades_count_;
        trades_.back() = row;
        if (row.pnl > 0.0) ++win_trades_count_;
        if (row.pnl < 0.0) ++loss_trades_count_;
    }

    void truncate_last() {
        const auto& old = trades_.back();
        if (old.pnl > 0.0) --win_trades_count_;
        if (old.pnl < 0.0) --loss_trades_count_;
        trades_.resize(trades_.size() - 1);
    }

    void reset_prefix_and_rows() {
        reset_trade_percent_prefix();
        trades_.clear();
        win_trades_count_ = 0;
        loss_trades_count_ = 0;
    }

    void zero_win_loss_guards() {
        win_trades_count_ = 0;
        loss_trades_count_ = 0;
    }
};

Trade percent_row(double pnl, double pct) {
    Trade row{};
    row.pnl = pnl;
    row.pnl_pct = pct;
    return row;
}

// Frozen scalar loops from a6abb14e8, independent of the new prefix fold.
// Keep the signed losing percentage and the pnl-based filters verbatim.
double reference_percent(const std::vector<Trade>& rows, int filter) {
    if (filter == 0) {
        int n = static_cast<int>(rows.size());
        if (n <= 0) return 0.0;
        double s = 0.0;
        for (const auto& t : rows) s += t.pnl_pct;
        return s / static_cast<double>(n);
    }
    int guard = 0;
    for (const auto& t : rows) {
        if (filter > 0 ? t.pnl > 0.0 : t.pnl < 0.0) ++guard;
    }
    if (guard <= 0) return 0.0;
    double s = 0.0;
    int c = 0;
    for (const auto& t : rows) {
        if (filter > 0 ? t.pnl > 0.0 : t.pnl < 0.0) { s += t.pnl_pct; ++c; }
    }
    return (c > 0) ? (s / static_cast<double>(c)) : 0.0;
}

void compare_prefix(const PercentPrefixProbe& probe, const std::vector<Trade>& rows) {
    const double expected[] = {reference_percent(rows, 0), reference_percent(rows, 1),
                               reference_percent(rows, -1)};
    // Repeated reads must not consume or refold the sealed prefix.
    for (int read = 0; read != 2; ++read) {
        const double actual[] = {probe.avg_trade_percent(), probe.avg_winning_trade_percent(),
                                 probe.avg_losing_trade_percent()};
        for (int index = 0; index != 3; ++index)
            PREFIX_CHECK(std::memcmp(&actual[index], &expected[index], sizeof(double)) == 0);
    }
}

void every_prefix_split(const std::vector<Trade>& rows) {
    for (std::size_t split = 0; split <= rows.size(); ++split) {
        PercentPrefixProbe probe;
        for (const auto& row : rows) probe.append(row);
        compare_prefix(probe, rows);
        probe.retire(split);
        PREFIX_CHECK(probe.resident_count() == rows.size() - split);
        compare_prefix(probe, rows);
        const auto remainder = probe.resident_count();
        probe.retire(remainder);
        PREFIX_CHECK(probe.resident_count() == 0);
        compare_prefix(probe, rows);
    }
}

void test_literal_prefix_splits() {
    every_prefix_split({});
    every_prefix_split({percent_row(0.0, -0.0), percent_row(-0.0, 8.0)});
    every_prefix_split({percent_row(1.0, 1e16), percent_row(1.0, -1e16),
                        percent_row(1.0, 1.0)});
    every_prefix_split({percent_row(-1.0, -1e16), percent_row(-1.0, 1e16),
                        percent_row(-1.0, -1.0)});
    every_prefix_split({percent_row(2.0, -7.0), percent_row(-2.0, 11.0),
                        percent_row(0.0, 3.0), percent_row(5.0, 9.0),
                        percent_row(-3.0, -5.0)});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    every_prefix_split({percent_row(1.0, 2.0), percent_row(0.0, nan),
                        percent_row(-1.0, -3.0), percent_row(nan, 4.0)});
    every_prefix_split({percent_row(1.0, nan), percent_row(-1.0, nan),
                        percent_row(1.0, 5.0), percent_row(-1.0, -6.0)});
    every_prefix_split({percent_row(1.0, inf), percent_row(-1.0, -inf)});
}

void test_many_prefix_splits() {
    std::vector<Trade> rows;
    std::uint64_t bits = 0xb6a8e5c7113d92f1ULL;
    for (int index = 0; index != 256; ++index) {
        bits ^= bits << 13;
        bits ^= bits >> 7;
        bits ^= bits << 17;
        const double pnl = static_cast<double>(static_cast<int>(bits % 7) - 3);
        const int exponent = static_cast<int>((bits >> 8) % 201) - 100;
        const double pct = std::ldexp(static_cast<double>(static_cast<int>(bits >> 48) - 32768),
                                      exponent);
        rows.push_back(percent_row(pnl, pct));
    }
    every_prefix_split(rows);
}

void test_append_rewrite_truncate_and_reset() {
    PercentPrefixProbe probe;
    std::vector<Trade> rows;
    for (int index = 0; index != 96; ++index) {
        const auto row = percent_row(static_cast<double>(index % 3 - 1),
                                     static_cast<double>(index - 40) / 7.0);
        probe.append(row);
        rows.push_back(row);
        compare_prefix(probe, rows);
        if (probe.resident_count() > 2) probe.retire(probe.resident_count() - 2);
        compare_prefix(probe, rows);
    }
    rows.back() = percent_row(1.0, -17.0);
    probe.replace_last(rows.back());
    compare_prefix(probe, rows);
    rows.pop_back();
    probe.truncate_last();
    compare_prefix(probe, rows);
    probe.retire(probe.resident_count());
    compare_prefix(probe, rows);

    probe.zero_win_loss_guards();
    PREFIX_CHECK(probe.avg_winning_trade_percent() == 0.0);
    PREFIX_CHECK(probe.avg_losing_trade_percent() == 0.0);
    probe.reset_prefix_and_rows();
    compare_prefix(probe, {});
    rows = {percent_row(-1.0, -12.0), percent_row(1.0, 3.0)};
    for (const auto& row : rows) probe.append(row);
    compare_prefix(probe, rows);
    probe.retire(1);
    compare_prefix(probe, rows);
    probe.reset_prefix_and_rows();
    compare_prefix(probe, {});
}

#undef PREFIX_CHECK

}  // namespace

int main() {
    test_literal_prefix_splits();
    test_many_prefix_splits();
    test_append_rewrite_truncate_and_reset();
    std::printf("trade percent prefix: %d checks, %d failures\n", prefix_checks, prefix_failures);
    return prefix_failures == 0 ? 0 : 1;
}
#endif
