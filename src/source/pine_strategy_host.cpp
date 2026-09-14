#include <pineforge/source/pine_strategy_host.hpp>
#include "../engine_internal.hpp"

namespace pineforge {
using namespace source;

void fill_pending_order_mirror(const source::PendingOrder&,
                               const MarketAdmissionJournal*,
                               pf_pending_order_v1_t*);

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

void source::PineStrategyHost::reset_source_pending_book() {
    pending_orders_.clear();
}

void source::PineStrategyHost::reset_source_order_and_close_state() {
    exit_leg_event_seq_ = 0;
    next_order_seq_ = 1;
    market_admission_journal_.reset();
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
    fold_exit_path_extremes_ = false;
    fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
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
    max_intraday_filled_orders_.reset_run();
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

void source::PineStrategyHost::reset_source_trail_after_flatten() {
    trail_best_price_ = std::numeric_limits<double>::quiet_NaN();
    trail_close_restart_bar_ = -1;
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

void source::PineStrategyHost::on_source_append_quoted_lot_before_book(
        const PyramidEntry& lot) {
    trail_best_price_ = lot.price;
}

void source::PineStrategyHost::on_source_append_quoted_lot_after_book(
        const PyramidEntry& lot) {
    id_unclosed_qty_[lot.entry_id] += lot.qty;
    cycle_filled_entry_ids_.insert(lot.entry_id);
}

void source::PineStrategyHost::reset_source_open_position_trail_before_book_clear(
        const PyramidEntry& lot) {
    trail_best_price_ = lot.price;
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
    return market_admission_journal_;
}

const source::PendingOrder& source::PineStrategyHost::pending_order_at(int index) const {
    return pending_orders_[static_cast<size_t>(index)];
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
                              &market_admission_journal_, out);
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
