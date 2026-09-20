/*
 * engine_trade_accessors.cpp — open-trade introspection methods.
 *
 * Carved out of engine.cpp during the v0.1 file-split (phase 6) so the
 * BacktestEngine implementation becomes navigable. These are the
 * Pine `strategy.opentrades.*` accessors — getters that report stats
 * about the *currently open* pyramid entries (in contrast to
 * `closed_trade_*` accessors which are inline in engine.hpp because
 * they're cheap pure-getter wrappers around the trades_ vector).
 *
 * Each accessor checks position_side_ != FLAT and bounds-checks idx
 * before reading from pyramid_entries_; out-of-range queries return
 * na<T>() (or 0 for cumulative metrics) per Pine semantics.
 */

#include <pineforge/engine.hpp>

namespace pineforge {

double BacktestEngine::open_trade_profit(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<double>();
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    bool is_long = (position_side_ == PositionSide::LONG);
    double px = current_bar_.close;
    // Currency PnL: price-points × qty × pointvalue × account_currency_fx_,
    // consistent with the realized-trade path in emit_close_trade
    // (pointvalue=1, fx=1.0 leaves the corpus unchanged).
    double pnl = (is_long ? (px - pe.price) * pe.qty : (pe.price - px) * pe.qty)
                 * syminfo_.pointvalue * active_account_currency_fx();
    pnl -= open_entry_commission(pe);
    return pnl;
}

double BacktestEngine::open_trade_profit_percent(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<double>();
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    if (pe.price <= 0.0) return na<double>();
    // TV net return-on-cost convention (same shape as the closed-trade
    // pnl_pct fixed 2026-06-12 in emit_close_trade): the profit value this
    // accessor pairs with — open_trade_profit, which is net of the
    // entry-leg commission — as a percent of entry cost (entry_price * qty
    // * pointvalue * account_currency_fx_, matching open_trade_profit's
    // fx-scaled pnl so the ratio is currency-invariant). No new commission
    // handling is invented here; the percent just mirrors open_trade_profit.
    const double entry_cost = pe.price * pe.qty * syminfo_.pointvalue
                              * active_account_currency_fx();
    if (!(entry_cost > 0.0)) return na<double>();
    return open_trade_profit(idx) / entry_cost * 100.0;
}

double BacktestEngine::open_trade_commission(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<double>();
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    return open_entry_commission(pe);
}

int BacktestEngine::open_trade_entry_bar_index(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<int>();
    return pyramid_entries_[(size_t)idx].entry_bar_index;
}

std::string BacktestEngine::open_trade_entry_comment(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return std::string();
    return pyramid_entries_[(size_t)idx].entry_comment;
}

std::string BacktestEngine::open_trade_entry_id(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return std::string();
    return pyramid_entries_[(size_t)idx].entry_id;
}

double BacktestEngine::open_trade_entry_price(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<double>();
    return pyramid_entries_[(size_t)idx].price;
}

int64_t BacktestEngine::open_trade_entry_time(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return 0;
    return pyramid_entries_[(size_t)idx].time;
}

double BacktestEngine::open_trade_size(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<double>();
    return pyramid_entries_[(size_t)idx].qty;
}

// pe.max_drawdown / pe.max_runup are tracked in price-points × qty
// (update_per_trade_extremes); the currency accessors scale by pointvalue
// and account_currency_fx_ (default 1.0, no-op) so they match the
// closed-trade fields emitted by emit_close_trade. The percent accessors
// below intentionally use the UNSCALED excursion over the unscaled entry
// cost — pointvalue and fx both cancel in the ratio.
double BacktestEngine::open_trade_max_drawdown(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return 0.0;
    return pyramid_entries_[(size_t)idx].max_drawdown * syminfo_.pointvalue
           * active_account_currency_fx();
}

double BacktestEngine::open_trade_max_drawdown_percent(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return 0.0;
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    double cost = pe.price * pe.qty;
    return (cost > 0.0) ? (pe.max_drawdown / cost) * 100.0 : 0.0;
}

double BacktestEngine::open_trade_max_runup(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return 0.0;
    return pyramid_entries_[(size_t)idx].max_runup * syminfo_.pointvalue
           * active_account_currency_fx();
}

double BacktestEngine::open_trade_max_runup_percent(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return 0.0;
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    double cost = pe.price * pe.qty;
    return (cost > 0.0) ? (pe.max_runup / cost) * 100.0 : 0.0;
}

// ABI v4 live-runtime surface (task 9): classify why a REPORT-row closed
// trade exited. Report-row scope (get_report_trade spans trades_ then
// range_end_trades_). Final review F7: an out-of-range index returns -1,
// like strategy_pending_order_fill_qty/_level_resolved/_effective_levels
// and strategy_closed_trade_close_cause's own NULL-handle case
// (c_abi.cpp) -- every other indexed live accessor uses -1 for "bad
// index/handle", so 0 stays reserved purely for the documented UNKNOWN
// "no cause" value on a VALID trade (no live derivation below currently
// produces it -- every in-range row falls through to at worst SCRIPT).
//
// R5 lane L12 (2.ii l): the kernel no longer decodes any caller's exit
// strings. A closer that knows why it closed records execution::CloseCause on
// the row -- a kernel-originated liquidation or risk flatten through the
// settling Fill, a host forced-close policy (the Pine adapter's margin call,
// its max-intraday-loss close and its filled-order cap close) on the row it
// produced. Order matters for the remaining classification:
//   1. open_at_end -- the range-end synthetic row always wins, even if the
//      position happens to also carry a stale exit_id from an earlier
//      partial close of the same physical lot.
//   2. a recorded cause -- the closer's own statement.
//   3. exit_from_bracket -- set only at the shared exit-fill site
//      (the native execution application path) when the filling request was
//      an EXIT leg, i.e. a real strategy.exit leg.
//   4. Otherwise: a strategy.close/close_all market close or a
//      reversal-driven close -- SCRIPT.
int BacktestEngine::closed_trade_close_cause(int i) const {
    if (i < 0 || i >= report_trade_count()) return -1;
    const Trade& t = get_report_trade(i);
    if (t.open_at_end) return static_cast<int>(execution::CloseCause::RangeEnd);
    if (t.close_cause != execution::CloseCause::Unspecified)
        return static_cast<int>(t.close_cause);
    if (t.exit_from_bracket) return static_cast<int>(execution::CloseCause::Bracket);
    return static_cast<int>(execution::CloseCause::Script);
}

} // namespace pineforge
