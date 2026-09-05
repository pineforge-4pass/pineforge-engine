/*
 * engine_risk.cpp — risk management + per-trade extreme tracking.
 *
 * Carved out of engine.cpp during the v0.1 file-split (phase 6) so
 * the BacktestEngine implementation becomes navigable.
 *
 *   check_risk_allow_entry    - gate entries by direction / position cap / halt
 *   update_risk_state         - check drawdown / intraday loss / consecutive
 *                               loss thresholds; latch risk_halted_ when hit
 *   update_per_trade_extremes - per-pyramid-entry MFE/MAE tracking from H/L/C
 *
 * All functions are BacktestEngine instance methods; they access the
 * engine's private state declared in <pineforge/engine.hpp>.
 */

#include <pineforge/engine.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "engine_internal.hpp"
#include "timezone.hpp"

namespace pineforge {

// See declaration in include/pineforge/engine.hpp. Used only by the
// intraday-day rollover gates below and the analogous gates in
// engine_fills.cpp / engine_orders.cpp. When ``chart_timezone_`` is
// empty we keep the cheap UTC fast path; otherwise we route through
// ``ScopedTimezone`` + ``localtime_r`` so IANA names like "Asia/Taipei"
// resolve correctly (POSIX-numeric offsets inside the same string syntax
// would silently disagree with the rest of the engine's TZ handling).
BacktestEngine::BarTime BacktestEngine::_decompose_bar_time_chart_tz() const {
    if (chart_timezone_.empty() || chart_timezone_ == "UTC" ||
        chart_timezone_ == "Etc/UTC") {
        return _decompose_bar_time();
    }
    time_t secs = static_cast<time_t>(current_bar_.timestamp / 1000);
    struct tm tm_buf {};
    {
        pine_tz::ScopedTimezone guard(chart_timezone_);
        localtime_r(&secs, &tm_buf);
    }
    BarTime bt;
    bt.year = tm_buf.tm_year + 1900;
    bt.month = tm_buf.tm_mon + 1;
    bt.dayofmonth = tm_buf.tm_mday;
    bt.hour = tm_buf.tm_hour;
    bt.minute = tm_buf.tm_min;
    bt.second = tm_buf.tm_sec;
    bt.dayofweek = tm_buf.tm_wday + 1;
    bt.weekofyear = (tm_buf.tm_yday + 7 - ((tm_buf.tm_wday + 6) % 7)) / 7;
    return bt;
}

bool BacktestEngine::check_risk_allow_entry(bool is_long) const {
    if (risk_halted_) return false;
    if (risk_direction_ == RiskDirection::LONG_ONLY && !is_long) return false;
    if (risk_direction_ == RiskDirection::SHORT_ONLY && is_long) return false;
    if (risk_max_position_size_ > 0.0 && position_qty_ >= risk_max_position_size_) return false;
    return true;
}

void BacktestEngine::update_risk_state() {
    if (risk_halted_) return;

    // Check max_drawdown
    if (risk_max_drawdown_ > 0.0) {
        double threshold = risk_max_drawdown_;
        if (risk_max_drawdown_is_pct_) {
            // percent_of_equity: threshold is pct% of peak equity
            threshold = max_equity_ * (risk_max_drawdown_ / 100.0);
        }
        if (max_drawdown_ >= threshold) {
            risk_halted_ = true;
            return;
        }
    }

    // max_intraday_loss is TradingView's day-scoped rule, evaluated at the
    // broker's ticks (evaluate_max_intraday_loss below); it never latches
    // risk_halted_.

    // Check max_cons_loss_days
    if (risk_max_cons_loss_days_ > 0 && cons_loss_day_count_ >= risk_max_cons_loss_days_) {
        risk_halted_ = true;
        return;
    }
}

// --- strategy.risk.max_intraday_loss (TradingView's arithmetic) ------------
//
// Pinned 2026-09-05 (lab tv, BINANCE:BTCUSDT 1D, scratchpad/r8/pins/m45-*):
//   t1  short 0.11773 from the 01-31 open, limit exit 61319.37 filled
//       intrabar on 2026-02-06 (+2699): the rule fires at the exit for
//       thresholds <= 2.45% and not at 2.46% -- loss = 2513.61 = the short's
//       open profit at the day's open 62909.87, base = 102513.6 = the
//       day-start equity WITH that open profit (2.4520%); every order of the
//       fired day is dropped, incl. the close-calc one, the next day's fill.
//   t6  the short held through 02-06 with short adds: closed at the HIGH
//       71751.33 as "Close Position (Max intraday Loss)" at 1.0% and 1.1%
//       (loss at the high 1208.6 = 1.18%): open P&L marked at the extremes.
//   t9  after the +2699 exit a recalc-born short 0.15 (fills 60000, -1763 at
//       the high): no fire at 3.0% -- the booked +2699 counts at later ticks
//       (1578 = 1.54%); only the closing fill's own P&L is missing at its
//       own tick.
//   t3b a long opened at the 02-03 open, no exit: closed at the LOW 72945.5
//       (-682 = 0.68%) at 0.3% / 0.5% -- the fire lands on the first path
//       extreme whose mark breaches.
// JOAT (officialjackofalltrades aureate BTC@1D, 1.5%): the 02-06 fire drops
// the recalc-born short @60000 and the close-calc short (TV 7 is 02-08).

int BacktestEngine::intraday_loss_day_key() const {
    BarTime bt = _decompose_bar_time_chart_tz();
    return bt.dayofmonth * 100 + bt.month;
}

void BacktestEngine::intraday_loss_begin_bar(const Bar& bar) {
    if (risk_max_intraday_loss_ <= 0.0) return;
    const int cur_day = intraday_loss_day_key();
    if (cur_day == intraday_loss_day_) return;
    intraday_loss_day_ = cur_day;
    // The day's first tick, before any fill at it: realized equity plus the
    // carried position marked at the open.
    intraday_loss_day_start_equity_ = current_equity() + open_profit(bar.open);
}

bool BacktestEngine::intraday_loss_orders_blocked() const {
    if (risk_max_intraday_loss_ <= 0.0 || intraday_loss_block_day_ < 0) {
        return false;
    }
    return intraday_loss_day_key() == intraday_loss_block_day_;
}

bool BacktestEngine::evaluate_max_intraday_loss(double mark_price,
                                                double excluded_realized) {
    if (risk_max_intraday_loss_ <= 0.0 || intraday_loss_evaluating_) {
        return false;
    }
    if (std::isnan(intraday_loss_day_start_equity_) || std::isnan(mark_price)) {
        return false;
    }
    if (intraday_loss_orders_blocked()) return false;  // fired already today
    const double equity_now =
        current_equity() - excluded_realized + open_profit(mark_price);
    const double loss = intraday_loss_day_start_equity_ - equity_now;
    double threshold = risk_max_intraday_loss_;
    if (risk_max_intraday_loss_is_pct_) {
        threshold = intraday_loss_day_start_equity_
                    * (risk_max_intraday_loss_ / 100.0);
    }
    if (!(threshold > 0.0) || !(loss > 0.0)) return false;
    const double eps = 1e-9 * std::max(1.0, std::fabs(threshold));
    if (loss + eps < threshold) return false;

    intraday_loss_evaluating_ = true;
    intraday_loss_block_day_ = intraday_loss_day_key();
    intraday_loss_cancel_pending_ = true;
    if (position_side_ != PositionSide::FLAT) {
        const size_t trades_before = trades_.size();
        execute_market_exit(mark_price);
        for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
            trades_[ti].exit_comment = "Close Position (Max intraday Loss)";
            trades_[ti].exit_id = "";
        }
        ++broker_fill_event_seq_;
    }
    intraday_loss_evaluating_ = false;
    return true;
}

// Outside a fill loop the cancel is immediate; inside one the loop removes
// the orders it has not applied and calls this at its safe point.
void BacktestEngine::finish_intraday_loss_cancel() {
    if (!intraday_loss_cancel_pending_) return;
    intraday_loss_cancel_pending_ = false;
    strategy_cancel_all();
}

// The bar's assumed OHLC path, tick by tick, for a broker pass that applied
// its fills in one sweep (the non-calc_on_order_fills dispatch): the mark
// is the path point, the position the one the sweep left.
void BacktestEngine::evaluate_max_intraday_loss_over_path(const Bar& bar) {
    if (risk_max_intraday_loss_ <= 0.0) return;
    double path[4];
    internal::fill_bar_path_points(bar, path);
    for (double px : path) {
        if (evaluate_max_intraday_loss(px, 0.0)) break;
    }
    finish_intraday_loss_cancel();
}

// Tracks favorable (max_runup / MFE) and adverse (max_drawdown / MAE) price
// excursion per open pyramid entry.
//
// We sample three representative prices per call — high, low, close — so a
// single daily bar fully captures both extremes without requiring tick-level
// resolution. During bar magnifier the high/low are running_high/running_low
// of the sampled path so no double-counting occurs, and close is the current
// sampled price.
void BacktestEngine::update_per_trade_extremes() {
    bool is_long = (position_side_ == PositionSide::LONG);
    double hi = current_bar_.high;
    double lo = current_bar_.low;
    double cl = current_bar_.close;
    for (auto& pe : pyramid_entries_) {
        // Intrabar-fill masks: on the bar a priced entry filled mid-bar, an
        // extreme that the assumed OHLC path reaches BEFORE the fill is not
        // part of this trade's excursion — substitute the fill price (zero
        // excursion) for that extreme. Post-fill path beyond the masked
        // extreme is still captured by the close sample below. Later bars
        // (entry_bar_index != bar_index_) always sample the full range.
        double pe_hi = hi;
        double pe_lo = lo;
        if (pe.entry_bar_index == bar_index_) {
            if (pe.skip_entry_bar_high) pe_hi = pe.price;
            if (pe.skip_entry_bar_low) pe_lo = pe.price;
        }
        // Favorable price: long -> high, short -> low
        double fav_px = is_long ? pe_hi : pe_lo;
        double adv_px = is_long ? pe_lo : pe_hi;
        double favorable = is_long ? (fav_px - pe.price) * pe.qty
                                   : (pe.price - fav_px) * pe.qty;
        double adverse   = is_long ? (pe.price - adv_px) * pe.qty
                                   : (adv_px - pe.price) * pe.qty;
        if (favorable > pe.max_runup) pe.max_runup = favorable;
        if (adverse > pe.max_drawdown) pe.max_drawdown = adverse;

        // Also consider close — in the magnifier path high/low include the
        // running extremes but the final sampled price matters for mid-bar
        // exits that close the trade before the bar completes.
        double closing = is_long ? (cl - pe.price) * pe.qty
                                 : (pe.price - cl) * pe.qty;
        if (closing > pe.max_runup) pe.max_runup = closing;
        double closing_dd = -closing;
        if (closing_dd > pe.max_drawdown) pe.max_drawdown = closing_dd;
    }
}

} // namespace pineforge
