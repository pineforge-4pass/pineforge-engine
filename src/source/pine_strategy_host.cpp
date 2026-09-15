#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>
#include "../engine_internal.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace pineforge {
using namespace source;

source::PineStrategyHost::PineStrategyHost(compat::pine::CapAttachment cap)
    : NativeStrategyHost(), adapter_(*this, cap) {}

PineStrategyConfig source::PineStrategyHost::apply_overrides(
        PineStrategyConfig config, const StrategyOverrides& overrides) {
    if (!std::isnan(overrides.initial_capital)) config.initial_capital = overrides.initial_capital;
    if (!std::isnan(overrides.commission_value)) config.commission_value = overrides.commission_value;
    if (!std::isnan(overrides.default_qty_value)) config.default_qty_value = overrides.default_qty_value;
    if (overrides.pyramiding >= 0) config.pyramiding = overrides.pyramiding;
    if (overrides.slippage >= 0) config.slippage = overrides.slippage;
    if (overrides.commission_type >= 0) config.commission_type = overrides.commission_type;
    if (overrides.default_qty_type >= 0) config.default_qty_type = overrides.default_qty_type;
    if (overrides.process_orders_on_close >= 0)
        config.process_orders_on_close = overrides.process_orders_on_close != 0;
    if (overrides.calc_on_order_fills >= 0)
        config.calc_on_order_fills = overrides.calc_on_order_fills != 0;
    if (overrides.close_entries_rule >= 0)
        config.close_entries_rule_any = overrides.close_entries_rule != 0;
    return config;
}

StagedConfiguration source::PineStrategyHost::staged_configuration() const {
    StagedConfiguration staged;
    staged.syminfo = syminfo_;
    staged.syminfo.mintick = syminfo_mintick_;
    staged.inputs = inputs_;
    staged.chart_timezone = chart_timezone_;
    staged.account_fx = account_currency_fx_;
    staged.account_fx_effective_from_ms = account_currency_fx_timestamps_;
    staged.account_fx_per_quote = account_currency_fx_rates_;
    if (std::isfinite(qty_step_) && qty_step_ > 0.0) staged.quantity_grid = qty_step_;
    return staged;
}

void source::PineStrategyHost::prepare_native_begin(const NativeBeginArgs& args) {
    if (args.syminfo) {
        syminfo_ = *args.syminfo;
        syminfo_mintick_ = syminfo_.mintick;
        if (std::isfinite(syminfo_.qty_step) && syminfo_.qty_step > 0.0)
            qty_step_ = syminfo_.qty_step;
    }
    if (args.inputs) inputs_ = *args.inputs;

    PineStrategyConfig effective = config_;
    if (!source_configuration_captured_) {
        effective.process_orders_on_close = process_orders_on_close_;
        effective.calc_on_order_fills = calc_on_order_fills_;
        effective.initial_capital = initial_capital_;
        effective.default_qty_type = static_cast<int>(default_qty_type_);
        effective.default_qty_value = default_qty_value_;
        effective.pyramiding = pyramiding_;
        effective.commission_value = commission_value_;
        effective.commission_type = static_cast<int>(commission_type_);
        effective.slippage = slippage_;
        effective.margin_long = margin_long_;
        effective.margin_short = margin_short_;
        effective.close_entries_rule_any = close_entries_rule_any_;
        effective.src_series_active = _src_series_active_;
    }
    if (args.overrides_opaque) {
        const auto* overrides = static_cast<const StrategyOverrides*>(args.overrides_opaque);
        effective = apply_overrides(effective, *overrides);
    }
    const StagedConfiguration staged = staged_configuration();
    if (!staged.account_fx_effective_from_ms.empty() && effective.calc_on_order_fills)
        throw std::logic_error(
            "timestamped account-currency FX is not supported with calc_on_order_fills");
    if (!staged.account_fx_effective_from_ms.empty() && args.bar_magnifier)
        throw std::logic_error(
            "timestamped account-currency FX is not supported with bar magnifier");

    adapter_.reset_for_run();
    adapter_.set_configuration(effective);
    adapter_.set_staged_configuration(staged);
    adapter_.set_margin_call_enabled(margin_call_enabled_);
    scheduler_.capture_begin(args);
    bar_magnifier_enabled_ = args.bar_magnifier;
    diag_magnifier_sub_bars_processed_ = 0;
    diag_magnifier_sample_ticks_processed_ = 0;
    const NativeRunSpec spec = adapter_.project(effective, staged, args);
    const auto setup = configure_native(spec);
    if (setup.status != NativeSetupStatus::Applied)
        throw std::logic_error("Pine native adapter failed to configure projected run spec");
    config_ = effective;
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::on_native_run_begin() {
    source_bar_index_ = -1;
    source_last_bar_index_ = -1;
    source_callback_count_ = 0;
    scheduler_.run_begin(*this);
}

void source::PineStrategyHost::on_native_bar_open(
        const Bar& bar, const NativeDecisionContext& context) {
    bar_magnifier_enabled_ = context.driver_statistics.intrabar_path_enabled;
    diag_magnifier_sub_bars_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sub_bars_processed);
    diag_magnifier_sample_ticks_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sample_ticks_processed);
    adapter_.on_bar_open(bar, context);
    scheduler_.bar_open(bar, context, *this);
}

void source::PineStrategyHost::on_native_bar(
        const Bar& bar, const NativeDecisionContext& context) {
    bar_magnifier_enabled_ = context.driver_statistics.intrabar_path_enabled;
    diag_magnifier_sub_bars_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sub_bars_processed);
    diag_magnifier_sample_ticks_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sample_ticks_processed);
    adapter_.observe_terminal_receipts();
    scheduler_.bar(bar, context, *this);
}

void source::PineStrategyHost::on_native_applied(
        const native_order::ExecutionAppliedEvent& event,
        const NativeDecisionContext& context) {
    adapter_.on_applied(event, context);
    project_short_seed_report_rows(event);
    scheduler_.applied(event, context, *this);
    if (scheduler_.terminal_source_bar()) {
        const Bar terminal = scheduler_.current_script_bar()
            ? *scheduler_.current_script_bar() : current_bar_;
        scheduler_record_range_end(terminal);
    }
}

native_order::ExecutionTerms source::PineStrategyHost::resolve_execution_terms(
        const NativeExecutionTermsFacts& facts) const {
    return adapter_.resolve_terms(facts);
}

NativePrecommitVerdict source::PineStrategyHost::validate_execution_precommit(
        const NativePrecommitView& view) const {
    return adapter_.validate_precommit(view);
}

void source::PineStrategyHost::configure_pine_strategy(
        const PineStrategyConfig& config) {
    guard_native_mutation("configure_pine_strategy");
    config_ = config;
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
    adapter_.set_configuration(config_);
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::set_strategy_override(
        const StrategyOverrides& overrides) {
    guard_native_mutation("set_strategy_override");
    override_ = overrides;
    config_ = apply_overrides(config_, override_);
    process_orders_on_close_ = config_.process_orders_on_close;
    calc_on_order_fills_ = config_.calc_on_order_fills;
    initial_capital_ = config_.initial_capital;
    default_qty_type_ = static_cast<QtyType>(config_.default_qty_type);
    default_qty_value_ = config_.default_qty_value;
    pyramiding_ = config_.pyramiding;
    commission_value_ = config_.commission_value;
    commission_type_ = static_cast<CommissionType>(config_.commission_type);
    slippage_ = config_.slippage;
    close_entries_rule_any_ = config_.close_entries_rule_any;
    adapter_.set_configuration(config_);
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::set_pine_risk_direction(int direction) {
    risk_direction_ = direction > 0 ? RiskDirection::LONG_ONLY
        : direction < 0 ? RiskDirection::SHORT_ONLY : RiskDirection::BOTH;
    adapter_.set_risk_direction(direction);
}
void source::PineStrategyHost::set_pine_risk_max_cons_loss_days(int value) {
    risk_max_cons_loss_days_ = value;
    adapter_.set_risk_max_cons_loss_days(value);
}
void source::PineStrategyHost::set_pine_risk_max_drawdown(double value, bool percent) {
    risk_max_drawdown_ = value;
    if (percent) risk_max_drawdown_is_pct_ = true;
    adapter_.set_risk_max_drawdown(value, percent);
}
void source::PineStrategyHost::set_pine_risk_max_intraday_loss(double value, bool percent) {
    risk_max_intraday_loss_ = value;
    if (percent) risk_max_intraday_loss_is_pct_ = true;
    adapter_.set_risk_max_intraday_loss(value, percent);
}
void source::PineStrategyHost::set_pine_risk_max_intraday_filled_orders(int limit) {
    adapter_.cap = limit;
}
void source::PineStrategyHost::set_pine_risk_max_position_size(double value) {
    risk_max_position_size_ = value;
    adapter_.set_risk_max_position_size(value);
}

int source::PineStrategyHost::pine_bar_index() const { return source_bar_index_; }
int source::PineStrategyHost::pine_last_bar_index() const { return source_last_bar_index_; }
bool source::PineStrategyHost::is_first_tick() const noexcept { return scheduler_.is_first_tick(); }
bool source::PineStrategyHost::is_last_tick() const noexcept { return scheduler_.is_last_tick(); }
bool source::PineStrategyHost::history_advances_new_bar() const noexcept {
    return scheduler_.history_advances_new_bar();
}
bool source::PineStrategyHost::security_series_slot_is_new(int slot) const noexcept {
    return scheduler_.security_series_slot_is_new(slot);
}
double source::PineStrategyHost::prev_chart_close() const {
    return scheduler_.previous_chart_close();
}
int source::PineStrategyHost::last_bar_dual_entry_path() const {
    return adapter_.pending_intent_view().last_bar_dual_entry_path();
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
    return physical_position().signed_units;
}

int source::PineStrategyHost::pending_order_count() const {
    return pending_intent_view().size();
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
    adapter_.enable_intraday_cap();
}

void source::PineStrategyHost::attach_pine_execution_adapter() {
    adapter_.attach_execution_adapter();
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
    return pending_intent_view().last_bar_dual_entry_path();
}

int source::PineStrategyHost::observe_pending_count_v1() const {
    return pending_intent_view().size();
}

int source::PineStrategyHost::observe_pending_copy_v1(
        int index, pf_pending_order_v1_t* out) const {
    return pending_intent_view().copy_v1(index, out);
}

int source::PineStrategyHost::observe_probe_fill_qty(
        int index, double fill_price, double* qty, int* close_only,
        int* partition) const {
    return pending_intent_view().probe_fill_qty(index, fill_price, qty, close_only,
                                                partition);
}

int source::PineStrategyHost::observe_pending_level_resolved(int index) const {
    return pending_intent_view().level_resolved(index);
}

int source::PineStrategyHost::observe_pending_effective_levels(
        int index, double* stop, double* limit, double* trail_activation) const {
    return pending_intent_view().effective_levels(index, stop, limit, trail_activation);
}

double source::PineStrategyHost::observe_trail_best_price_v1() const {
    return adapter_.pending_intent_view().trail_best_price();
}

const PendingIntentView& source::PineStrategyHost::pending_intent_view() const noexcept {
    return adapter_.pending_intent_view();
}

int source::PineStrategyHost::short_seed_collision_role_v1(
        native_order::RequestHandle handle) const noexcept {
    return adapter_.short_seed_collision_role_v1(std::move(handle));
}

const std::vector<source::PineStrategyHost::FixturePendingOrder>&
source::PineStrategyHost::source_pending_view() const {
    source_pending_view_cache_.clear();
    source_pending_view_cache_.reserve(adapter_.pending_same_bar_commands_.size()
        + adapter_.source_shadow_pending_.size() + adapter_.live_handles_.size());
    const auto append = [&](const PlacementSnapshot& snapshot, const std::string& label) {
        FixturePendingOrderType type = FixturePendingOrderType::MARKET;
        switch (snapshot.family) {
        case PineOrderFamily::Close:
        case PineOrderFamily::CloseAll:
        case PineOrderFamily::ExitLimit:
        case PineOrderFamily::ExitStop:
        case PineOrderFamily::ExitTrail:
        case PineOrderFamily::Margin:
            type = FixturePendingOrderType::EXIT;
            break;
        case PineOrderFamily::Order:
            type = FixturePendingOrderType::RAW_ORDER;
            break;
        case PineOrderFamily::Entry:
            type = FixturePendingOrderType::MARKET;
            break;
        }
        const std::string& id = snapshot.frozen_market_targeted_close ? label : snapshot.source_id;
        source_pending_view_cache_.push_back({id, type,
            snapshot.sizing.frozen_units, snapshot.sizing.price});
    };
    for (const auto& command : adapter_.pending_same_bar_commands_)
        append(command.snapshot, command.request.label);
    for (const auto& shadow : adapter_.source_shadow_pending_)
        append(shadow.snapshot, shadow.label);
    for (const auto& handle : adapter_.live_handles_) {
        const auto found = adapter_.placement_.find(handle.incarnation);
        if (found != adapter_.placement_.end()) append(found->second, found->second.source_id);
    }
    return source_pending_view_cache_;
}

void source::PineStrategyHost::project_short_seed_report_rows(
        const native_order::ExecutionAppliedEvent& event) {
    // Keep the report projection independent of the adapter's mutable plan
    // while it touches report containers.
    const ShortSeedPlan plan = adapter_.short_seed_;
    if (!plan.report_swap_pending || event.closed_trade_count == 0
        || event.handle() == plan.final_short) {
        return;
    }
    std::optional<PlacementSnapshot> placement_snapshot;
    if (const auto placement = adapter_.placement_.find(event.handle().incarnation);
        placement != adapter_.placement_.end()) {
        placement_snapshot = placement->second;
    }
    if (!placement_snapshot || placement_snapshot->family != PineOrderFamily::Close
        || placement_snapshot->from_entry != "Short") {
        return;
    }
    for (auto& trade : trades_) {
        if (trade.entry_incarnation == plan.materialize_long.incarnation
            && trade.entry_id == "__close__Short") {
            trade.entry_incarnation = plan.final_short.incarnation;
        }
    }
    const std::size_t begin = event.first_trade_index;
    const std::size_t end = begin + event.closed_trade_count;
    for (std::size_t index = begin; index < end && index < trades_.size(); ++index) {
        if (trades_[index].entry_incarnation == plan.final_short.incarnation
            && trades_[index].entry_id == "Short") {
            trades_[index].entry_incarnation = plan.materialize_long.incarnation;
        }
    }
    adapter_.short_seed_.report_swap_pending = false;
}

void source::PineStrategyHost::scheduler_prepare_script_run(
        const std::vector<Bar>& bars, bool static_eligible, int expected_script_bars) {
    if (const auto state = native_state(); state.spec && !state.spec->timeframe_undetected) {
        input_tf_ = state.spec->input_tf;
        script_tf_ = state.spec->script_tf;
        script_tf_seconds_ = tf_to_seconds(script_tf_);
    }
    prepare_script_run(bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()),
                       static_eligible);
    source_last_bar_index_ = expected_script_bars - 1;
}

void source::PineStrategyHost::scheduler_configure_security_evaluators() {
    configure_security_evaluators();
}

void source::PineStrategyHost::scheduler_prepare_chart_day_partition(
        const std::vector<Bar>& bars) {
    prepare_chart_day_partition(bars.empty() ? nullptr : bars.data(),
                                static_cast<int>(bars.size()));
}

void source::PineStrategyHost::scheduler_record_range_end(const Bar& terminal_bar) {
    range_end_trades_.clear();
    if (stream_warmup_mode_ || position_side_ == PositionSide::FLAT || equity_curve_.empty()
        || !std::isfinite(terminal_bar.close)) return;
    const Bar saved = current_bar_;
    current_bar_ = terminal_bar;
    const bool was_long = position_side_ == PositionSide::LONG;
    const double fill_price = bar_fill_price(current_bar_.close);
    const auto saved_timestamp = current_bar_.timestamp;
    current_bar_.timestamp = equity_curve_.back().time_ms;
    double range_end_pnl = 0.0;
    for (const auto& lot : pyramid_entries_) {
        execution::PhysicalExecutionContext context;
        context.effective_time_ms = current_bar_.timestamp;
        context.interval_index = bar_index_;
        context.preceding_exit_path_prefix = fold_exit_path_extremes_;
        if (!std::isnan(fold_exit_trail_peak_))
            context.preceding_exit_trail_peak = fold_exit_trail_peak_;
        Trade row = build_close_trade_with_costs(
            lot, lot.qty, fill_price, was_long,
            allocated_entry_commission(lot, lot.qty), calc_commission(fill_price, lot.qty),
            context);
        row.open_at_end = true;
        range_end_pnl += row.pnl;
        range_end_trades_.push_back(std::move(row));
    }
    current_bar_.timestamp = saved_timestamp;
    auto& last = equity_curve_.back();
    last.open_profit = 0.0;
    last.equity = initial_capital_ + net_profit_sum_ + range_end_pnl;
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;
    max_drawdown_ = 0.0;
    max_runup_ = 0.0;
    for (const auto& point : equity_curve_) fold_equity_extreme(point.equity);
    current_bar_ = saved;
}

void source::PineStrategyHost::scheduler_publish_source_bar(
        const Bar& bar, bool, bool advance_source_index) {
    current_bar_ = bar;
    if (advance_source_index) ++source_bar_index_;
    ++source_callback_count_;
    bar_index_ = source_bar_index_;
    barstate_islast_ = source_bar_index_ == source_last_bar_index_;
    NativeDayPartitionScope chart_day_partition(
        chart_day_partition_.empty() ? nullptr : &chart_day_partition_);
    on_source_bar(bar);
    adapter_.flush_pending_entries();
    adapter_.flush_pending_bracket_legs();
    if (advance_source_index) {
        update_equity_extremes();
        record_equity_point(bar.timestamp);
        prev_bar_timestamp_ = bar.timestamp;
    }
}

} // namespace pineforge
