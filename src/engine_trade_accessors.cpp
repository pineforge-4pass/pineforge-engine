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
    double pnl = is_long ? (px - pe.price) * pe.qty : (pe.price - px) * pe.qty;
    pnl -= calc_commission(pe.price, pe.qty);
    return pnl;
}

double BacktestEngine::open_trade_profit_percent(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<double>();
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    if (pe.price <= 0.0) return na<double>();
    bool is_long = (position_side_ == PositionSide::LONG);
    double px = current_bar_.close;
    if (is_long) return (px / pe.price - 1.0) * 100.0;
    return (pe.price / px - 1.0) * 100.0;
}

double BacktestEngine::open_trade_commission(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return na<double>();
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    return calc_commission(pe.price, pe.qty);
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

double BacktestEngine::open_trade_max_drawdown(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return 0.0;
    return pyramid_entries_[(size_t)idx].max_drawdown;
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
    return pyramid_entries_[(size_t)idx].max_runup;
}

double BacktestEngine::open_trade_max_runup_percent(int idx) const {
    if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size())
        return 0.0;
    const PyramidEntry& pe = pyramid_entries_[(size_t)idx];
    double cost = pe.price * pe.qty;
    return (cost > 0.0) ? (pe.max_runup / cost) * 100.0 : 0.0;
}

} // namespace pineforge
