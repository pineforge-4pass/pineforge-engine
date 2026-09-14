#include <pineforge/source/pine_strategy_host.hpp>

namespace pineforge {
using namespace source;

source::PineStrategyHost::PineStrategyHost(compat::pine::CapAttachment cap)
    : BacktestEngine(cap) {}

void source::PineStrategyHost::on_bar(const Bar& bar) {
    on_source_bar(bar);
}

int source::PineStrategyHost::pine_bar_index() const {
    return bar_index_ + bar_index_offset_;
}

int source::PineStrategyHost::pine_last_bar_index() const {
    return last_bar_index_ + bar_index_offset_;
}

bool source::PineStrategyHost::history_advances_new_bar() const {
    return is_first_tick_ && history_slot_is_new_;
}

double source::PineStrategyHost::prev_chart_close() const {
    return prev_chart_close_;
}

void source::PineStrategyHost::_push_source_series() {
    if (history_advances_new_bar()) prev_chart_close_ = last_chart_close_;
    last_chart_close_ = current_bar_.close;
    if (!_src_series_active_) return;
    const double o = current_bar_.open;
    const double h = current_bar_.high;
    const double l = current_bar_.low;
    const double c = current_bar_.close;
    const double v = current_bar_.volume;
    const double hl2   = (h + l) / 2.0;
    const double hlc3  = (h + l + c) / 3.0;
    const double ohlc4 = (o + h + l + c) / 4.0;
    const double hlcc4 = (h + l + c + c) / 4.0;
    if (history_advances_new_bar()) {
        _src_open_.push(o);   _src_high_.push(h);   _src_low_.push(l);
        _src_close_.push(c);  _src_volume_.push(v);
        _src_hl2_.push(hl2);  _src_hlc3_.push(hlc3);
        _src_ohlc4_.push(ohlc4); _src_hlcc4_.push(hlcc4);
    } else {
        _src_open_.update(o);   _src_high_.update(h);   _src_low_.update(l);
        _src_close_.update(c);  _src_volume_.update(v);
        _src_hl2_.update(hl2);  _src_hlc3_.update(hlc3);
        _src_ohlc4_.update(ohlc4); _src_hlcc4_.update(hlcc4);
    }
}

double source::PineStrategyHost::signed_position_size() const {
    if (pos_view_freeze_bar_ == bar_index_) {
        if (pos_view_frozen_side_ == PositionSide::LONG) return pos_view_frozen_qty_;
        if (pos_view_frozen_side_ == PositionSide::SHORT) return -pos_view_frozen_qty_;
        return 0.0;
    }
    if (position_side_ == PositionSide::LONG) return position_qty_;
    if (position_side_ == PositionSide::SHORT) return -position_qty_;
    return 0.0;
}

void source::PineStrategyHost::freeze_script_position_view() {
    if (pos_view_freeze_bar_ == bar_index_) return;
    pos_view_freeze_bar_ = bar_index_;
    pos_view_frozen_side_ = position_side_;
    pos_view_frozen_qty_ = position_qty_;
    pos_view_frozen_entry_qty_.clear();
    for (const auto& entry : pyramid_entries_) {
        pos_view_frozen_entry_qty_[entry.entry_id] += entry.qty;
    }
}

void source::PineStrategyHost::clear_script_position_view() {
    pos_view_freeze_bar_ = -1;
}

void source::PineStrategyHost::reset_source_language_series() {
    PineLanguageState::reset_for_run();
}

double source::PineStrategyHost::live_position_size() const {
    return signed_position_size();
}

} // namespace pineforge
