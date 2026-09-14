#pragma once

#include <pineforge/engine.hpp>

namespace pineforge::source {

// Data only.  Stateful helpers remain PineStrategyHost members so they retain
// unqualified access to the generic broker state they read.
struct PineLanguageState {
protected:
    // @source-state begin
    int pos_view_freeze_bar_ = -1;
    PositionSide pos_view_frozen_side_ = PositionSide::FLAT;
    double pos_view_frozen_qty_ = 0.0;
    std::unordered_map<std::string, double> pos_view_frozen_entry_qty_;

    bool _src_series_active_ = false;
    Series<double> _src_open_;
    Series<double> _src_high_;
    Series<double> _src_low_;
    Series<double> _src_close_;
    Series<double> _src_volume_;
    Series<double> _src_hl2_;
    Series<double> _src_hlc3_;
    Series<double> _src_ohlc4_;
    Series<double> _src_hlcc4_;
    double prev_chart_close_ = std::numeric_limits<double>::quiet_NaN();
    double last_chart_close_ = std::numeric_limits<double>::quiet_NaN();
    int bar_index_offset_ = 0;

    bool is_first_tick_ = true;
    bool is_last_tick_ = true;
    bool history_slot_is_new_ = true;
    bool coof_checkpoint_contains_current_bar_ = false;
    Series<double> coof_checkpoint_src_open_;
    Series<double> coof_checkpoint_src_high_;
    Series<double> coof_checkpoint_src_low_;
    Series<double> coof_checkpoint_src_close_;
    Series<double> coof_checkpoint_src_volume_;
    Series<double> coof_checkpoint_src_hl2_;
    Series<double> coof_checkpoint_src_hlc3_;
    Series<double> coof_checkpoint_src_ohlc4_;
    Series<double> coof_checkpoint_src_hlcc4_;
    double coof_checkpoint_prev_chart_close_ = std::numeric_limits<double>::quiet_NaN();
    double coof_checkpoint_last_chart_close_ = std::numeric_limits<double>::quiet_NaN();
    // @source-state end

    void reset_for_run() {
        _src_open_.clear();
        _src_high_.clear();
        _src_low_.clear();
        _src_close_.clear();
        _src_volume_.clear();
        prev_chart_close_ = std::numeric_limits<double>::quiet_NaN();
        last_chart_close_ = std::numeric_limits<double>::quiet_NaN();
        coof_checkpoint_prev_chart_close_ = std::numeric_limits<double>::quiet_NaN();
        coof_checkpoint_last_chart_close_ = std::numeric_limits<double>::quiet_NaN();
        _src_hl2_.clear();
        _src_hlc3_.clear();
        _src_ohlc4_.clear();
        _src_hlcc4_.clear();
    }
};

} // namespace pineforge::source
