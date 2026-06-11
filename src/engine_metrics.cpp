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

}  // namespace metrics
}  // namespace pineforge
