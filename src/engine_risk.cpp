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

namespace pineforge {

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

    // Check max_intraday_loss
    if (risk_max_intraday_loss_ > 0.0) {
        int cur_day = _decompose_bar_time().dayofmonth * 100 + _decompose_bar_time().month;
        if (cur_day != intraday_pnl_day_) {
            intraday_pnl_day_ = cur_day;
            intraday_pnl_ = 0.0;
        }
        double intraday_threshold = risk_max_intraday_loss_;
        if (risk_max_intraday_loss_is_pct_) {
            double eq = initial_capital_ + net_profit_sum_ + open_profit(current_bar_.close);
            intraday_threshold = eq * (risk_max_intraday_loss_ / 100.0);
        }
        if (intraday_pnl_ < 0.0 && (-intraday_pnl_) >= intraday_threshold) {
            risk_halted_ = true;
            return;
        }
    }

    // Check max_cons_loss_days
    if (risk_max_cons_loss_days_ > 0 && cons_loss_day_count_ >= risk_max_cons_loss_days_) {
        risk_halted_ = true;
        return;
    }
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
        // Favorable price: long -> high, short -> low
        double fav_px = is_long ? hi : lo;
        double adv_px = is_long ? lo : hi;
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
