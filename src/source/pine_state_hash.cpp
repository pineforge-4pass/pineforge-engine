#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#include "../broker_state_hash_internal.hpp"

namespace pineforge {
namespace {

void hash_source_series(BrokerStateHashSink& f, const Series<double>& series) {
    f.i(series.size());
    for (int i = 0; i < series.size(); ++i) f.d(series[i]);
}

} // namespace

namespace {

void hash_source_run_identity(BrokerStateHashSink& f,
                              const native_order::RunIdentity& identity) {
    f.s(identity.session_key);
    // The monotonically increasing native generation protects stale handles;
    // it does not change the source-visible state of a fresh run.
    f.u(0);
}
void hash_source_run_epoch(BrokerStateHashSink& f, std::uint64_t run_counter) {
    // Retain the anti-stale counter as an explicit projection input while
    // keeping source broker fingerprints independent of handle reuse.
    (void)run_counter;
    f.u(0);
}
void hash_native_handle(BrokerStateHashSink& f, const native_order::RequestHandle& handle) {
    hash_source_run_identity(f, handle.run); f.u(handle.incarnation);
}
void hash_native_handle_vector(BrokerStateHashSink& f,
                               const std::vector<native_order::RequestHandle>& handles) {
    f.u(handles.size());
    for (const auto& handle : handles) hash_native_handle(f, handle);
}
void hash_placement(BrokerStateHashSink& f, const source::PlacementSnapshot& value) {
    f.i(static_cast<std::int64_t>(value.family)); f.s(value.source_id); f.s(value.from_entry);
    f.s(value.comment); f.s(value.oca_name); f.i(value.oca_type); f.i(value.qty_type);
    f.d(value.requested_qty); f.d(value.qty_percent); f.b(value.is_long); f.b(value.immediately);
    f.b(value.opening); f.b(value.deferred_cohort); f.b(value.frozen_market_instruction);
    f.d(value.frozen_market_own_units); f.d(value.frozen_market_transaction_units);
    f.b(value.frozen_market_targeted_close); f.b(value.frozen_market_target_was_long);
    f.b(value.direction_gate); f.b(value.affordability_policy_active);
    f.b(value.affordability_close_only);
    f.b(value.affordability_keep_mc_close_surplus);
    f.b(value.reverse_to); f.b(value.replaced_opening); f.b(value.replacement_predecessor_market);
    f.b(value.terms_priced_reverse);
    f.d(value.frozen_reversal_transaction);
    f.i(value.placement_cycle); f.u(value.sequential_group); f.u(value.sequential_rank);
    f.b(value.has_full_entry_bracket);
    hash_source_run_identity(f, value.bracket_origin.run);
    f.u(value.bracket_origin.incarnation);
    f.u(value.source_sequence);
    f.u(value.command_ordinal); f.u(value.placement_open_epoch);
    f.i(value.placement_script_open_ms);
    f.i(value.placement_sub_open_ms); f.i(value.projection_created_bar);
    f.i(value.projection_position_side); f.b(value.projection_after_close);
    f.b(value.projection_over_pyramiding); f.u(value.projection_predecessor);
    f.b(value.projection_predecessor_market); f.b(value.projection_predecessor_exit);
    f.b(value.projection_created_during_coof); f.b(value.projection_coof_at_terminal);
    f.b(value.projection_coof_mid_bar); f.d(value.projection_tv_carry_qty);
    f.d(value.projection_default_stop_equity);
    f.d(value.projection_default_stop_signal_close);
    f.d(value.projection_explicit_equity); f.d(value.projection_explicit_signal_close);
    f.d(value.projection_affordability_equity);
    f.d(value.projection_affordability_signal_price);
    f.d(value.projection_affordability_held_qty);
    f.d(value.sizing.equity); f.d(value.sizing.price);
    f.d(value.sizing.fx); f.d(value.sizing.mark); f.d(value.sizing.frozen_units);
    f.b(value.sizing.at_fill); f.d(value.exit_levels.limit); f.d(value.exit_levels.stop);
    f.d(value.exit_levels.trail_points); f.d(value.exit_levels.trail_offset);
    f.d(value.exit_levels.trail_price); f.d(value.exit_levels.profit_ticks);
    f.d(value.exit_levels.loss_ticks);
}

void hash_short_seed_plan(BrokerStateHashSink& f, const source::ShortSeedPlan& value) {
    hash_native_handle(f, value.long_entry); hash_native_handle(f, value.materialize_long);
    hash_native_handle(f, value.final_short); f.s(value.seed_id); f.s(value.long_entry_id);
    f.s(value.final_short_id); f.s(value.materialize_label); f.d(value.seed_qty);
    f.i(value.seed_cycle); f.b(value.active); f.b(value.report_swap_pending);
}

void hash_native_request(BrokerStateHashSink& f, const native_order::Request& request) {
    f.u(request.intent.index());
    if (const auto* reduce = std::get_if<native_order::Reduce>(&request.intent)) {
        f.u(reduce->size.index());
        if (const auto* units = std::get_if<native_order::ExplicitUnits>(&reduce->size)) f.d(units->units);
    } else if (const auto* transact = std::get_if<native_order::Transact>(&request.intent)) {
        f.d(transact->signed_units);
    } else if (const auto* reverse = std::get_if<native_order::ReverseTo>(&request.intent)) {
        f.d(reverse->signed_units);
    } else if (const auto* sized = std::get_if<native_order::HostSized>(&request.intent)) {
        f.i(static_cast<std::int64_t>(sized->kind)); f.b(sized->side.has_value());
        if (sized->side) f.i(static_cast<std::int64_t>(*sized->side));
    }
    f.s(request.label); f.s(request.comment);
    f.u(request.trigger.index());
    if (const auto* limit = std::get_if<native_order::Limit>(&request.trigger)) f.d(limit->price);
    else if (const auto* stop = std::get_if<native_order::Stop>(&request.trigger)) f.d(stop->price);
    else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&request.trigger)) {
        f.d(stop_limit->stop); f.d(stop_limit->limit);
    } else if (const auto* trail = std::get_if<native_order::Trail>(&request.trigger)) {
        f.d(trail->offset); f.b(trail->arm_price.has_value());
        if (trail->arm_price) f.d(*trail->arm_price);
    }
    f.u(request.capacity.index());
    if (const auto* budget = std::get_if<native_order::PointBudget>(&request.capacity)) f.d(budget->units);
    f.u(request.owner.index());
    if (const auto* wait = std::get_if<native_order::WaitForApplied>(&request.owner)) {
        hash_native_handle(f, wait->parent);
    } else if (const auto* opening = std::get_if<native_order::BindOpening>(&request.owner)) {
        hash_native_handle(f, opening->opening); f.i(opening->cycle);
    } else if (const auto* openings = std::get_if<native_order::BindOpenings>(&request.owner)) {
        hash_native_handle_vector(f, openings->openings); f.i(openings->cycle);
    } else if (const auto* cohort = std::get_if<native_order::BindCohort>(&request.owner)) {
        f.u(cohort->cohort.value);
    }
    f.u(request.group.index());
    if (const auto* group = std::get_if<native_order::Member>(&request.group)) {
        f.u(group->group); f.i(group->cohort); f.i(static_cast<std::int64_t>(group->effect));
    }
}

} // namespace

void source::PineExecutionAdapter::hash_state(BrokerStateHashSink& f) const {
    f.s(kSourceAdapterDomain);
    hash_source_run_epoch(f, run_counter_);
    f.u(source_sequence_); f.u(command_ordinal_); f.u(broker_open_epoch_);
    f.i(last_broker_open_ms_); f.b(host_ != nullptr);
    f.b(config_.process_orders_on_close); f.b(config_.calc_on_order_fills);
    f.d(config_.initial_capital); f.i(config_.default_qty_type); f.d(config_.default_qty_value);
    f.i(config_.pyramiding); f.d(config_.commission_value); f.i(config_.commission_type);
    f.i(config_.slippage); f.d(config_.margin_long); f.d(config_.margin_short);
    f.b(config_.close_entries_rule_any); f.b(config_.src_series_active);
    f.s(staged_.syminfo.ticker); f.s(staged_.syminfo.tickerid); f.s(staged_.syminfo.currency);
    f.s(staged_.syminfo.basecurrency); f.s(staged_.syminfo.type); f.s(staged_.syminfo.timezone);
    f.s(staged_.syminfo.session); f.s(staged_.syminfo.volumetype); f.s(staged_.syminfo.description);
    f.d(staged_.syminfo.mintick); f.d(staged_.syminfo.pointvalue); f.d(staged_.syminfo.qty_step);
    f.s(staged_.chart_timezone); f.d(staged_.account_fx);
    f.u(staged_.account_fx_effective_from_ms.size());
    for (const auto timestamp : staged_.account_fx_effective_from_ms) f.i(timestamp);
    f.u(staged_.account_fx_per_quote.size());
    for (const auto rate : staged_.account_fx_per_quote) f.d(rate);
    f.b(staged_.quantity_grid.has_value()); if (staged_.quantity_grid) f.d(*staged_.quantity_grid);
    std::vector<std::string> input_keys;
    for (const auto& pair : staged_.inputs) input_keys.push_back(pair.first);
    std::sort(input_keys.begin(), input_keys.end()); f.u(input_keys.size());
    for (const auto& key : input_keys) { f.s(key); f.s(staged_.inputs.at(key)); }
    std::vector<std::string> cohort_keys;
    for (const auto& pair : cohorts_by_id_) cohort_keys.push_back(pair.first);
    std::sort(cohort_keys.begin(), cohort_keys.end()); f.u(cohort_keys.size());
    for (const auto& key : cohort_keys) {
        const auto& cohort = cohorts_by_id_.at(key);
        f.s(key); f.u(cohort.handle.value); f.i(cohort.cycle);
        hash_native_handle_vector(f, cohort.origins); hash_native_handle_vector(f, cohort.opened);
        std::vector<std::uint64_t> live_origin_keys;
        live_origin_keys.reserve(cohort.live_units_by_origin.size());
        for (const auto& row : cohort.live_units_by_origin) live_origin_keys.push_back(row.first);
        std::sort(live_origin_keys.begin(), live_origin_keys.end());
        f.u(live_origin_keys.size());
        for (const auto incarnation : live_origin_keys) {
            f.u(incarnation); f.d(cohort.live_units_by_origin.at(incarnation));
        }
    }
    f.u(cohort_order_.size());
    for (const auto& key : cohort_order_) f.s(key);
    std::vector<std::uint64_t> placement_keys;
    for (const auto& pair : placement_) placement_keys.push_back(pair.first);
    std::sort(placement_keys.begin(), placement_keys.end()); f.u(placement_keys.size());
    for (const auto key : placement_keys) { f.u(key); hash_placement(f, placement_.at(key)); }
    std::vector<std::uint64_t> live_keys;
    for (const auto& pair : live_by_source_key_) live_keys.push_back(pair.first);
    std::sort(live_keys.begin(), live_keys.end()); f.u(live_keys.size());
    for (const auto key : live_keys) { f.u(key); hash_native_handle(f, live_by_source_key_.at(key)); }
    std::vector<std::uint64_t> bracket_keys;
    for (const auto& pair : bracket_families_) bracket_keys.push_back(pair.first);
    std::sort(bracket_keys.begin(), bracket_keys.end()); f.u(bracket_keys.size());
    for (const auto key : bracket_keys) { f.u(key); hash_native_handle_vector(f, bracket_families_.at(key)); }
    f.u(pending_bracket_legs_.size());
    for (const auto& leg : pending_bracket_legs_) {
        hash_native_request(f, leg.request); hash_placement(f, leg.snapshot);
        f.s(leg.replacement_key); f.u(leg.family_key);
    }
    f.u(pending_entries_.size());
    for (const auto& entry : pending_entries_) {
        hash_native_request(f, entry.request); hash_placement(f, entry.snapshot);
        f.s(entry.replacement_key);
    }
    f.u(pending_same_bar_commands_.size());
    for (const auto& command : pending_same_bar_commands_) {
        hash_native_request(f, command.request); hash_placement(f, command.snapshot);
        f.s(command.replacement_key); f.b(command.opening);
    }
    f.u(source_shadow_pending_.size());
    for (const auto& shadow : source_shadow_pending_) {
        hash_placement(f, shadow.snapshot); f.s(shadow.label);
    }
    f.d(pending_same_bar_close_qty_);
    f.u(pending_relative_exits_.size());
    for (const auto& exit : pending_relative_exits_) {
        f.s(exit.exit_id); f.s(exit.from_entry); f.d(exit.trail_points); f.d(exit.trail_offset);
        f.d(exit.trail_price); f.d(exit.qty_percent); f.s(exit.comment); f.d(exit.qty);
        f.s(exit.oca_name); f.d(exit.profit_ticks); f.d(exit.loss_ticks);
    }
    f.u(pending_coof_requests_.size());
    for (const auto& pending : pending_coof_requests_) {
        hash_native_request(f, pending.request); hash_placement(f, pending.snapshot);
        f.s(pending.replacement_key); f.b(pending.opening); f.u(pending.family_key);
    }
    hash_native_handle_vector(f, live_handles_); hash_native_handle_vector(f, first_open_newborns_);
    hash_native_handle_vector(f, pending_view_handles_);
    f.u(dropped_close_receipts_.size());
    for (const auto& receipt : dropped_close_receipts_) {
        f.s(receipt.source_id); f.s(receipt.comment); f.d(receipt.qty);
        f.d(receipt.qty_percent); f.b(receipt.immediately); f.u(receipt.callsite_token);
        f.u(receipt.command_ordinal);
    }
    f.u(open_entry_fees_.size());
    for (const auto& fee : open_entry_fees_) {
        hash_native_handle(f, fee.opening); f.s(fee.source_id); f.d(fee.units);
        f.d(fee.nonpercent_fee);
    }
    std::vector<std::uint64_t> current_debit_ordinals;
    current_debit_ordinals.reserve(current_debited_applied_ordinals_.size());
    for (const auto ordinal : current_debited_applied_ordinals_) current_debit_ordinals.push_back(ordinal);
    std::sort(current_debit_ordinals.begin(), current_debit_ordinals.end());
    f.u(current_debit_ordinals.size());
    for (const auto ordinal : current_debit_ordinals) f.u(ordinal);
    f.u(receipt_cursor_);
    f.b(materializing_relative_);
    f.i(current_position_cycle_);
    f.i(current_position_sign_);
    f.u(next_sequential_group_);
    f.b(coof_recalc_active_); f.b(coof_first_open_);
    const auto& coof_coord = coof_context_.coordinate;
    f.u(coof_coord.ordinal); f.i(coof_coord.interval_index); f.i(coof_coord.open_ms);
    f.i(coof_coord.eligible_open_ms); f.i(coof_coord.last_traded_close_ms);
    f.i(coof_coord.next_period_open_ms); f.i(coof_coord.next_input_open_ms);
    f.i(coof_coord.effective_time_ms); f.i(coof_coord.source_price_time_ms);
    f.i(static_cast<std::int64_t>(coof_coord.provenance));
    f.i(static_cast<std::int64_t>(coof_coord.path_phase));
    f.i(static_cast<std::int64_t>(coof_coord.completion)); f.i(coof_context_.decision_floor_ms);
    const auto hash_coof_interval = [&](const native_calendar::NativeInterval& interval) {
        f.i(interval.open_ms); f.i(interval.eligible_open_ms); f.i(interval.last_traded_close_ms);
        f.i(interval.next_period_open_ms); f.i(interval.next_input_open_ms);
    };
    hash_coof_interval(coof_context_.input_interval);
    hash_coof_interval(coof_context_.script_interval);
    f.i(coof_context_.sub_index); f.i(coof_context_.sub_count);
    f.b(coof_context_.is_terminal_sub_bar); f.i(coof_context_.sub_bar_open_ms);
    f.i(coof_context_.script_bar_open_ms);
    f.b(coof_context_.driver_statistics.intrabar_path_enabled);
    f.i(coof_context_.driver_statistics.sub_bars_per_script_bar);
    f.i(coof_context_.driver_statistics.samples_per_sub_bar);
    f.u(coof_context_.driver_statistics.sub_bars_processed);
    f.u(coof_context_.driver_statistics.sample_ticks_processed);
    f.d(coof_script_bar_.open); f.d(coof_script_bar_.high); f.d(coof_script_bar_.low);
    f.d(coof_script_bar_.close); f.d(coof_script_bar_.volume); f.i(coof_script_bar_.timestamp);
    f.b(coof_script_bar_valid_);
    std::vector<std::int64_t> pooc_basis_keys;
    for (const auto& pair : pooc_close_basis_by_script_bar_) pooc_basis_keys.push_back(pair.first);
    std::sort(pooc_basis_keys.begin(), pooc_basis_keys.end()); f.u(pooc_basis_keys.size());
    for (const auto key : pooc_basis_keys) { f.i(key); f.d(pooc_close_basis_by_script_bar_.at(key)); }
    f.d(pooc_open_basis_); f.i(pooc_open_script_bar_); f.i(close_all_pending_script_bar_);
    f.d(last_fx_rate_); f.i(position_open_script_bar_); f.b(source_margin_call_enabled_);
    f.i(day_ledger_.current_day); f.i(day_ledger_.last_loss_day); f.i(day_ledger_.consecutive_loss_days);
    f.i(day_ledger_.intraday_loss_day); f.d(day_ledger_.intraday_start_equity);
    f.d(day_ledger_.intraday_realized); f.u(day_ledger_.observed_applied_ordinal);
    f.i(risk_.direction); f.i(risk_.max_cons_loss_days); f.d(risk_.max_drawdown);
    f.b(risk_.max_drawdown_percent); f.d(risk_.max_intraday_loss);
    f.b(risk_.max_intraday_loss_percent); f.d(risk_.max_position_size); f.b(risk_.halted);
    hash_short_seed_plan(f, short_seed_);
    hash_short_seed_plan(f, pending_short_seed_.plan);
    f.u(pending_short_seed_.expected_open_epoch); f.b(pending_short_seed_.ready);
    hash_native_handle(f, short_seed_long_candidate_);
    f.i(last_bar_dual_entry_path_); f.b(pending_view_.owner_ != nullptr);
    f.i(static_cast<std::int64_t>(cap.attachment())); f.i(cap.configuration().limit);
    f.b(cap.configuration().skip_noop_market); f.b(cap.configuration().defer_pooc_close);
    f.b(cap.configuration().count_pooc_full_close); f.b(priority.attached());
    f.b(priority.retained_parent_first());
    admission_journal.reflect("journal", [&](const auto& field) { hash_admission_field(f, field); });
}

void source::PineScheduler::hash_state(BrokerStateHashSink& f) const {
    f.s("pineforge-pine-scheduler/v2"); f.u(retained_.bars.size());
    for (const auto& bar : retained_.bars) {
        f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close); f.d(bar.volume); f.i(bar.timestamp);
    }
    f.s(retained_.input_tf); f.s(retained_.script_tf); f.b(retained_.bar_magnifier);
    f.i(retained_.magnifier_samples); f.i(static_cast<std::int64_t>(retained_.distribution));
    f.b(retained_.volume_weighted); f.i(retained_.volume_weighted_min_samples);
    f.i(retained_.volume_weighted_max_samples); f.b(retained_.is_stream); f.i(retained_.warmup_n);
    f.i(language_.pos_view_freeze_bar_); f.i(static_cast<std::int64_t>(language_.pos_view_frozen_side_));
    f.d(language_.pos_view_frozen_qty_); hash_str_double_map(f, language_.pos_view_frozen_entry_qty_);
    f.b(language_._src_series_active_); hash_source_series(f, language_._src_open_);
    hash_source_series(f, language_._src_high_); hash_source_series(f, language_._src_low_);
    hash_source_series(f, language_._src_close_); hash_source_series(f, language_._src_volume_);
    hash_source_series(f, language_._src_hl2_); hash_source_series(f, language_._src_hlc3_);
    hash_source_series(f, language_._src_ohlc4_); hash_source_series(f, language_._src_hlcc4_);
    f.d(language_.prev_chart_close_); f.d(language_.last_chart_close_); f.i(language_.bar_index_offset_);
    f.b(language_.is_first_tick_); f.b(language_.is_last_tick_); f.b(language_.history_slot_is_new_);
    f.b(language_.coof_checkpoint_contains_current_bar_);
    hash_source_series(f, language_.coof_checkpoint_src_open_);
    hash_source_series(f, language_.coof_checkpoint_src_high_);
    hash_source_series(f, language_.coof_checkpoint_src_low_);
    hash_source_series(f, language_.coof_checkpoint_src_close_);
    hash_source_series(f, language_.coof_checkpoint_src_volume_);
    hash_source_series(f, language_.coof_checkpoint_src_hl2_);
    hash_source_series(f, language_.coof_checkpoint_src_hlc3_);
    hash_source_series(f, language_.coof_checkpoint_src_ohlc4_);
    hash_source_series(f, language_.coof_checkpoint_src_hlcc4_);
    f.d(language_.coof_checkpoint_prev_chart_close_); f.d(language_.coof_checkpoint_last_chart_close_);
    f.i(current_script_open_ms_);
    f.d(current_script_bar_.open); f.d(current_script_bar_.high); f.d(current_script_bar_.low);
    f.d(current_script_bar_.close); f.d(current_script_bar_.volume); f.i(current_script_bar_.timestamp);
    f.b(current_script_bar_valid_); f.b(saw_open_fill_); f.i(source_bar_count_);
    f.i(expected_source_bars_); f.u(applied_cursor_); f.i(coof_callback_script_open_);
    f.i(prior_input_script_open_ms_);
    f.i(awaiting_legacy_script_open_ms_);
    f.u(input_script_completes_.size());
    for (const auto value : input_script_completes_) f.u(value);
    f.u(input_script_boundary_completes_.size());
    for (const auto value : input_script_boundary_completes_) f.u(value);
    f.b(uses_aux_security_feed_);
    f.d(deferred_boundary_input_.bar.open); f.d(deferred_boundary_input_.bar.high);
    f.d(deferred_boundary_input_.bar.low); f.d(deferred_boundary_input_.bar.close);
    f.d(deferred_boundary_input_.bar.volume); f.i(deferred_boundary_input_.bar.timestamp);
    f.i(deferred_boundary_input_.next_input_ms);
    f.i(deferred_boundary_input_.prior_script_open_ms);
    f.b(deferred_boundary_input_.calling_bar_complete);
    f.b(deferred_boundary_input_.all_security_states);
    f.b(deferred_boundary_input_.active);
}

void source::PineStrategyHost::hash_source_extension(BrokerStateHashSink& f) const {
    f.s(kSourceAdapterDomain);
    f.b(config_.process_orders_on_close); f.b(config_.calc_on_order_fills);
    f.d(config_.initial_capital); f.i(config_.default_qty_type); f.d(config_.default_qty_value);
    f.i(config_.pyramiding); f.d(config_.commission_value); f.i(config_.commission_type);
    f.i(config_.slippage); f.d(config_.margin_long); f.d(config_.margin_short);
    f.b(config_.close_entries_rule_any); f.b(config_.src_series_active);
    f.d(override_.initial_capital); f.d(override_.commission_value); f.d(override_.default_qty_value);
    f.i(override_.pyramiding); f.i(override_.slippage); f.i(override_.commission_type);
    f.i(override_.default_qty_type); f.i(override_.process_orders_on_close);
    f.i(override_.calc_on_order_fills); f.i(override_.close_entries_rule);
    f.i(source_bar_index_); f.i(source_last_bar_index_); f.u(source_callback_count_);
    f.b(source_configuration_captured_);
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    f.u(aux_security_bars_.size());
    for (const auto& bar : aux_security_bars_) {
        f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close);
        f.d(bar.volume); f.i(bar.timestamp);
    }
    f.s(aux_security_input_tf_);
    f.u(aux_security_chart_begin_.size());
    for (const auto value : aux_security_chart_begin_) f.u(value);
    f.u(aux_security_chart_end_.size());
    for (const auto value : aux_security_chart_end_) f.u(value);
#endif
    adapter_.hash_state(f); scheduler_.hash_state(f);
}

} // namespace pineforge
