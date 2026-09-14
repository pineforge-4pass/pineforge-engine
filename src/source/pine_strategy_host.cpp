#include <pineforge/source/pine_strategy_host.hpp>
#include "../engine_internal.hpp"

namespace pineforge {
using namespace source;

source::PineStrategyHost::PineStrategyHost(compat::pine::CapAttachment cap)
    : BacktestEngine(), adapter_(cap) {}

void source::PineStrategyHost::on_bar(const Bar& bar) {
    on_source_bar(bar);
}

void source::PineStrategyHost::configure_pine_strategy(
        const PineStrategyConfig& config) {
    process_orders_on_close_ = config.process_orders_on_close;
    calc_on_order_fills_ = config.calc_on_order_fills;
    initial_capital_ = config.initial_capital;
    default_qty_type_ = static_cast<QtyType>(config.default_qty_type);
    default_qty_value_ = config.default_qty_value;
    pyramiding_ = config.pyramiding;
    commission_value_ = config.commission_value;
    commission_type_ = static_cast<CommissionType>(config.commission_type);
    slippage_ = config.slippage;
    margin_long_ = config.margin_long;
    margin_short_ = config.margin_short;
    close_entries_rule_any_ = config.close_entries_rule_any;
    _src_series_active_ = config.src_series_active;
}

void source::PineStrategyHost::set_strategy_override(
        const StrategyOverrides& overrides) {
    if (!std::isnan(overrides.initial_capital)) initial_capital_ = overrides.initial_capital;
    if (overrides.pyramiding >= 0) pyramiding_ = overrides.pyramiding;
    if (overrides.slippage >= 0) slippage_ = overrides.slippage;
    if (!std::isnan(overrides.commission_value)) commission_value_ = overrides.commission_value;
    if (overrides.commission_type >= 0)
        commission_type_ = static_cast<CommissionType>(overrides.commission_type);
    if (!std::isnan(overrides.default_qty_value))
        default_qty_value_ = overrides.default_qty_value;
    if (overrides.default_qty_type >= 0)
        default_qty_type_ = static_cast<QtyType>(overrides.default_qty_type);
    if (overrides.process_orders_on_close >= 0)
        process_orders_on_close_ = overrides.process_orders_on_close != 0;
    if (overrides.calc_on_order_fills >= 0)
        calc_on_order_fills_ = overrides.calc_on_order_fills != 0;
    if (overrides.close_entries_rule >= 0)
        close_entries_rule_any_ = overrides.close_entries_rule != 0;
}

void source::PineStrategyHost::set_pine_risk_direction(int direction) {
    risk_direction_ = direction > 0
        ? RiskDirection::LONG_ONLY
        : (direction < 0 ? RiskDirection::SHORT_ONLY : RiskDirection::BOTH);
}

void source::PineStrategyHost::set_pine_risk_max_cons_loss_days(int value) {
    risk_max_cons_loss_days_ = value;
}

void source::PineStrategyHost::set_pine_risk_max_drawdown(
        double value, bool percent) {
    risk_max_drawdown_ = value;
    if (percent) risk_max_drawdown_is_pct_ = true;
}

void source::PineStrategyHost::set_pine_risk_max_intraday_loss(
        double value, bool percent) {
    risk_max_intraday_loss_ = value;
    if (percent) risk_max_intraday_loss_is_pct_ = true;
}

void source::PineStrategyHost::set_pine_risk_max_intraday_filled_orders(int limit) {
    adapter_.cap = limit;
}

void source::PineStrategyHost::set_pine_risk_max_position_size(double value) {
    risk_max_position_size_ = value;
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

int source::PineStrategyHost::last_bar_dual_entry_path() const {
    return static_cast<int>(last_bar_dual_entry_decision_);
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

void source::PineStrategyHost::reset_source_pending_book() {
    pending_orders_.clear();
}

void source::PineStrategyHost::reset_source_order_and_close_state() {
    exit_leg_event_seq_ = 0;
    next_order_seq_ = 1;
    adapter_.admission_journal.reset();
    named_entry_cancelled_incarnation_in_current_eval_.clear();
    pending_close_qty_in_bar_ = 0.0;
    pos_view_freeze_bar_ = -1;
    pos_view_frozen_side_ = PositionSide::FLAT;
    pos_view_frozen_qty_ = 0.0;
    pos_view_frozen_entry_qty_.clear();
    sb_close_active_ = false;
    sb_close_bar_ = -1;
    sb_close_calls_ = 0;
    sb_close_first_id_.clear();
    sb_close_first_target_ = 0.0;
    sb_close_first_carry_valid_ = false;
    sb_close_first_carry_qty_ = 0.0;
    sb_close_id_.clear();
    sb_close_comment_.clear();
    close_reserved_qty_.clear();
    close_two_call_first_qty_.clear();
    callsite_close_bar_ = -1;
    callsite_close_queue_seq_ = 0;
    callsite_close_callsites_.clear();
    callsite_close_admitted_total_ = 0.0;
    callsite_close_reserved_qty_.clear();
    callsite_close_two_call_first_qty_.clear();
    last_exit_fill_was_trail_ = false;
    trail_best_before_bar_ = std::numeric_limits<double>::quiet_NaN();
    trail_best_before_bar_index_ = -1;
    trail_best_before_bar_position_cycle_ = 0;
    trail_best_before_bar_fill_seq_ = 0;
    priced_entry_activity_bar_ = -1;
    priced_entry_filled_this_bar_ = false;
    open_margin_slice_bar_ = -1;
}

void source::PineStrategyHost::reset_source_risk_and_cap() {
    risk_halted_ = false;
    cons_loss_day_count_ = 0;
    last_loss_day_ = -1;
    intraday_pnl_ = 0.0;
    intraday_pnl_day_ = -1;
    intraday_loss_day_start_equity_ = std::numeric_limits<double>::quiet_NaN();
    intraday_loss_day_ = -1;
    intraday_loss_block_day_ = -1;
    intraday_loss_evaluating_ = false;
    intraday_loss_cancel_pending_ = false;
    adapter_.cap.reset_run();
}

void source::PineStrategyHost::reset_source_margin_and_coof() {
    last_margin_call_event_bar_ = -1;
    intrabar_exit_margin_call_bar_ = -1;
    coof_scheduler_active_ = false;
    coof_fill_recalc_active_ = false;
    coof_recalc_at_bar_open_ = false;
    coof_recalc_after_first_open_fill_ = false;
    coof_market_entry_recalc_incarnation_ = 0;
    coof_market_entry_recalc_fill_seq_ = 0;
    coof_cursor_is_bar_close_ = false;
    coof_evaluating_path_segment_ = false;
    coof_at_extreme_waypoint_ = false;
    coof_hist_is_segment_ = false;
    coof_hist_path_index_ = -1;
    coof_cascade_recalc_leg_ = -1;
    coof_cascade_force_wp_gap_ = false;
    coof_cursor_price_ = std::numeric_limits<double>::quiet_NaN();
    coof_direct_fill_events_remaining_ = 0;
    coof_checkpoint_contains_current_bar_ = false;
    history_slot_is_new_ = true;
}

void source::PineStrategyHost::reset_source_bar_projections() {
    last_bar_dual_entry_decision_ = internal::DualEntryStopPathWinner::None;
    trail_close_restart_bar_ = -1;
}

void source::PineStrategyHost::reset_source_language_series() {
    PineLanguageState::reset_for_run();
}

void source::PineStrategyHost::reset_source_exit_activations_before_flatten() {
    unbind_exit_activations();
}

void source::PineStrategyHost::reset_source_position_ledgers_after_book_clear() {
    id_unclosed_qty_.clear();
    cycle_filled_entry_ids_.clear();
    close_reserved_qty_.clear();
    close_two_call_first_qty_.clear();
    callsite_close_reserved_qty_.clear();
    callsite_close_two_call_first_qty_.clear();
    consumed_partial_exit_ids_.clear();
}

void source::PineStrategyHost::on_source_append_quoted_lot_after_book(
        const PyramidEntry& lot) {
    id_unclosed_qty_[lot.entry_id] += lot.qty;
    cycle_filled_entry_ids_.insert(lot.entry_id);
}

void source::PineStrategyHost::reset_source_open_position_ledgers_before_book(
        const PyramidEntry&) {
    id_unclosed_qty_.clear();
    cycle_filled_entry_ids_.clear();
    close_reserved_qty_.clear();
    close_two_call_first_qty_.clear();
    callsite_close_reserved_qty_.clear();
    callsite_close_two_call_first_qty_.clear();
    consumed_partial_exit_ids_.clear();
}

void source::PineStrategyHost::on_source_open_position_booked(
        const PyramidEntry& lot) {
    id_unclosed_qty_[lot.entry_id] += lot.qty;
    cycle_filled_entry_ids_.insert(lot.entry_id);
    bind_retained_exit_activations();
}

double source::PineStrategyHost::live_position_size() const {
    return signed_position_size();
}

int source::PineStrategyHost::pending_order_count() const {
    return static_cast<int>(pending_orders_.size());
}

const MarketAdmissionJournal& source::PineStrategyHost::market_admission_journal() const {
    return adapter_.admission_journal;
}

MarketAdmissionJournal& source::PineStrategyHost::market_admission_journal() {
    return adapter_.admission_journal;
}

const source::PendingOrder& source::PineStrategyHost::pending_order_at(int i) const {
    return pending_orders_[static_cast<size_t>(i)];
}

void source::PineStrategyHost::enable_pine_intraday_cap() {
    guard_native_mutation("enable_pine_intraday_cap");
    adapter_.cap.attach();
}

void source::PineStrategyHost::attach_pine_execution_adapter() {
    guard_native_mutation("attach_pine_execution_adapter");
    adapter_.cap.attach();
    adapter_.priority.attach();
}

void source::PineStrategyHost::set_syminfo_metadata(
        const std::string& key, double value) {
    BacktestEngine::set_syminfo_metadata(key, value);
    if (key == "bar_index_offset") {
        bar_index_offset_ = std::isfinite(value)
            ? static_cast<int>(std::llround(value))
            : 0;
    }
    if (key == "security_range_start_na_warmup") {
        if (std::isfinite(value) && value > 0.0) {
            security_range_start_na_warmup_ = true;
            security_range_start_ms_ = static_cast<int64_t>(std::llround(value));
        } else {
            security_range_start_na_warmup_ = false;
            security_range_start_ms_ = 0;
        }
    }
    if (key == "chart_ema_na_warmup") {
        chart_ema_na_warmup_ = std::isfinite(value) && value > 0.0;
    }
    if (key == "historical_security_lookahead_projection") {
        historical_security_lookahead_projection_ =
            std::isfinite(value) && value > 0.0;
    }
    if (key == "margin_zero_cover_full_liquidation") {
        margin_zero_cover_full_liquidation_ =
            std::isfinite(value) && value > 0.0;
    }
    adapter_.priority.metadata(key, value);
    adapter_.cap.metadata(key, value);
    if (key == "margin_long" && margin_long_ == 100.0) {
        margin_long_ = (std::isfinite(value) && value > 0.0) ? value : 100.0;
    }
    if (key == "margin_short" && margin_short_ == 100.0) {
        margin_short_ = (std::isfinite(value) && value > 0.0) ? value : 100.0;
    }
}

int source::PineStrategyHost::observe_last_bar_dual_entry_path_v1() const {
    return static_cast<int>(last_bar_dual_entry_decision_);
}

int source::PineStrategyHost::observe_pending_count_v1() const {
    return static_cast<int>(pending_orders_.size());
}

int source::PineStrategyHost::observe_pending_copy_v1(
        int index, pf_pending_order_v1_t* out) const {
    if (!out || index < 0 || index >= static_cast<int>(pending_orders_.size())) return -1;
    fill_pending_order_mirror(pending_orders_[static_cast<size_t>(index)],
                              &adapter_.admission_journal, out);
    return 0;
}

int source::PineStrategyHost::observe_probe_fill_qty(
        int index, double fill_price, double* qty, int* close_only,
        int* partition) const {
    return probe_fill_qty(index, fill_price, qty, close_only, partition);
}

int source::PineStrategyHost::observe_pending_level_resolved(int index) const {
    return pending_order_level_resolved(index);
}

int source::PineStrategyHost::observe_pending_effective_levels(
        int index, double* stop, double* limit, double* trail_activation) const {
    return pending_order_effective_levels(index, stop, limit, trail_activation);
}

double source::PineStrategyHost::observe_trail_best_price_v1() const {
    return trail_best_price_;
}

} // namespace pineforge
