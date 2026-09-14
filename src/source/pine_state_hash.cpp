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

void source::PineStrategyHost::hash_source_extension(BrokerStateHashSink& f) const {
    f.s(kSourceAdapterDomain);

    f.u(cycle_filled_entry_ids_.size());
    for (const auto& id : cycle_filled_entry_ids_) f.s(id);
    hash_str_double_map(f, id_unclosed_qty_);
    hash_str_double_map(f, close_reserved_qty_);
    hash_str_double_map(f, close_two_call_first_qty_);
    hash_token_owned_map(f, callsite_close_reserved_qty_);
    hash_token_owned_map(f, callsite_close_two_call_first_qty_);
    f.b(sb_close_active_);
    f.i(sb_close_bar_);
    f.i(sb_close_calls_);
    f.s(sb_close_first_id_);
    f.d(sb_close_first_target_);
    f.b(sb_close_first_carry_valid_);
    f.d(sb_close_first_carry_qty_);
    f.s(sb_close_id_);
    f.s(sb_close_comment_);
    f.i(callsite_close_bar_);
    f.u(callsite_close_queue_seq_);
    f.d(callsite_close_admitted_total_);
    std::vector<uint64_t> callsite_tokens;
    callsite_tokens.reserve(callsite_close_callsites_.size());
    for (const auto& pair : callsite_close_callsites_) callsite_tokens.push_back(pair.first);
    std::sort(callsite_tokens.begin(), callsite_tokens.end());
    f.u(callsite_tokens.size());
    for (uint64_t token : callsite_tokens) {
        const auto& site = callsite_close_callsites_.at(token);
        f.u(token); f.u(site.token); f.b(site.active); f.i(site.calls);
        f.s(site.first_id); f.d(site.first_target); f.b(site.first_ledger_consumed);
        f.b(site.first_carry_valid); f.d(site.first_carry_qty); f.s(site.id);
        f.s(site.comment); f.d(site.target); f.u(site.deferred_cleanup_ids.size());
        for (const auto& id : site.deferred_cleanup_ids) f.s(id);
        f.u(site.queue_seq); f.b(site.retire_ledger_whole);
    }
    f.i(static_cast<int64_t>(default_qty_type_));
    f.d(default_qty_value_);
    f.i(pyramiding_);
    f.b(margin_zero_cover_full_liquidation_);
    f.b(close_entries_rule_any_);
    f.d(margin_long_);
    f.d(margin_short_);

    f.i(pos_view_freeze_bar_);
    f.i(static_cast<int64_t>(pos_view_frozen_side_));
    f.d(pos_view_frozen_qty_);
    hash_str_double_map(f, pos_view_frozen_entry_qty_);
    f.d(pending_close_qty_in_bar_);

    f.u(pending_orders_.size());
    for (const auto& o : pending_orders_) {
        f.s(o.id); f.s(o.from_entry); f.i(static_cast<int64_t>(o.type)); f.b(o.is_long);
        o.legs.visit(f); f.d(o.qty);
        f.i(static_cast<int64_t>(o.qty_type)); f.d(o.qty_percent); f.s(o.oca_name);
        f.i(static_cast<int64_t>(o.oca_type));
        f.i(static_cast<int64_t>(o.created_bar)); f.i(o.created_seq); f.u(o.incarnation);
        f.b(o.stop_limit_activated);
        f.d(o.default_stop_placement_qty); f.d(o.frozen_default_qty);
        f.d(o.sizing_equity); f.d(o.sizing_price); f.d(o.sizing_fx); f.d(o.sizing_mark);
        f.d(o.default_stop_placement_equity); f.d(o.default_stop_sizing_price);
        f.d(o.tv_carry_qty);
        f.b(o.affordability_close_only);
        f.i(static_cast<int64_t>(o.created_position_cycle_seq));
        f.b(o.quantity_request.intent().has_value());
        if (const auto& intent = o.quantity_request.intent()) {
            f.i(static_cast<int64_t>(intent->kind()));
            if (intent->kind() == QuantityIntent::Kind::Units) f.d(intent->units());
            else if (intent->kind() == QuantityIntent::Kind::Fraction) {
                f.d(intent->numerator()); f.d(intent->denominator());
            }
        }
        f.b(o.quantity_request.reservation().has_value());
        if (const auto& reservation = o.quantity_request.reservation()) {
            f.d(reservation->units); f.d(reservation->basis_units);
        }
        f.i(static_cast<int64_t>(o.signal_close_mc_bar));
        f.u(o.signal_close_mc_entry_incarnation);
        f.u(o.signal_close_mc_fill_seq);
        f.i(static_cast<int64_t>(o.pine_frozen_market_instruction.kind()));
        if (const auto* transaction = o.pine_frozen_market_instruction.transaction()) {
            f.d(transaction->own_units); f.d(transaction->transaction_units);
        }
        if (const auto* close = o.pine_frozen_market_instruction.targeted_close()) {
            f.s(close->target_id);
        }
        admission::reflect(o.market_admission, "draft", [&](const auto& field) {
            hash_admission_field(f, field);
        });
        f.d(o.paired_flat_market_own_qty);
        f.d(o.paired_flat_market_signal_close);
        f.d(o.paired_flat_market_signal_equity);
        f.d(o.paired_flat_market_signal_margin_pct);
        f.d(o.paired_flat_market_signal_pointvalue);
        f.d(o.paired_flat_market_signal_fx);
        f.i(o.paired_flat_market_peer_seq);
        f.d(o.paired_flat_market_transaction_qty);
        f.i(static_cast<int64_t>(o.short_seed_collision_role));
        f.i(static_cast<int64_t>(o.cancellation.cause()));
        f.i(static_cast<int64_t>(o.cancellation.state()));
        f.i(static_cast<int64_t>(o.cancellation.close_claim_release()));
        f.u(o.cancellation.source_incarnation());
        f.i(o.cancellation.source_sequence());
        f.u(o.cancellation.target_incarnation());
        f.i(o.cancellation.target_owner());
        f.u(o.cancellation.target_revision());
        f.d(o.cancellation.close_claim_consumed());
        f.d(o.cancellation.close_claim_retired());
        f.i(static_cast<int64_t>(o.created_position_side));
        f.b(o.rounded_signal_cost_close_only);
        f.d(o.signal_close_mc_remaining_qty);
        f.u(o.replaced_order_incarnation);
        f.u(o.replaced_default_market_incarnation);
        f.u(o.recreated_after_named_cancelled_entry_incarnation);
        f.u(o.named_cancel_surviving_exit_incarnation);
        f.b(o.leg_activation.bounds().has_value());
        if (const auto& bounds = o.leg_activation.bounds()) {
            f.i(bounds->position_cycle); f.i(bounds->stop_first_bar); f.i(bounds->limit_first_bar);
        }
        f.b(o.pine_exit_activation.evidence().has_value());
        if (const auto& evidence = o.pine_exit_activation.evidence()) {
            f.i(evidence->position_cycle); f.i(evidence->entry_bar); f.i(evidence->direction);
            f.d(evidence->cursor_price); f.d(evidence->stop_level); f.d(evidence->limit_level);
            f.b(evidence->limit_continuation.has_value());
            if (const auto& continuation = evidence->limit_continuation) {
                f.i(static_cast<int64_t>(continuation->cause)); f.u(continuation->observed_fill_sequence);
            }
        }
        f.i(static_cast<int64_t>(o.birth.cause()));
        f.i(o.birth.bar());
        f.i(o.birth.timestamp());
        f.i(static_cast<int64_t>(o.birth.cursor().domain()));
        f.i(static_cast<int64_t>(o.birth.cursor().position()));
        f.i(o.birth.cursor().index());
        f.i(o.birth.cursor().count());
        f.d(o.birth.cursor_price());
        f.u(o.birth.first_fill());
        f.u(o.birth.last_fill());
        f.u(o.birth.evaluation_ordinal());
        f.i(static_cast<int64_t>(o.pine_birth_reach));
        f.i(static_cast<int64_t>(o.coof_cascade_seg_i));
        f.b(o.coof_cascade_inflight_fires);
        f.i(static_cast<int64_t>(o.same_id_stop_deferred_close_all_bar));
        f.u(o.same_id_stop_deferred_close_all_incarnation);
        f.d(o.affordability_placement_equity);
        f.d(o.affordability_signal_price);
        f.d(o.affordability_held_qty);
        f.d(o.explicit_placement_equity);
        f.d(o.explicit_slipped_signal_close);
        f.d(o.default_stop_placement_signal_close);
        f.b(o.reservation_expansion.capture().has_value());
        if (const auto& capture = o.reservation_expansion.capture()) {
            f.i(capture->position_cycle);
            f.i(static_cast<int64_t>(capture->side));
            f.b(capture->first_later_admission.has_value());
            if (const auto& admission = capture->first_later_admission) {
                f.u(*admission);
            }
        }
        f.b(o.reservation_growth_source.reservation_owner().has_value());
        if (const auto& receiver = o.reservation_growth_source.reservation_owner()) {
            f.u(*receiver);
        }
    }
    adapter_.admission_journal.reflect("journal", [&](const auto& field) {
        hash_admission_field(f, field);
    });

    std::vector<std::string> cancelled_ids;
    cancelled_ids.reserve(named_entry_cancelled_incarnation_in_current_eval_.size());
    for (const auto& pair : named_entry_cancelled_incarnation_in_current_eval_)
        cancelled_ids.push_back(pair.first);
    std::sort(cancelled_ids.begin(), cancelled_ids.end());
    f.u(cancelled_ids.size());
    for (const auto& id : cancelled_ids) {
        const auto& value = named_entry_cancelled_incarnation_in_current_eval_.at(id);
        f.s(id); f.u(value.entry_incarnation); f.u(value.surviving_exit_incarnation);
    }

    hash_str_set(f, consumed_partial_exit_ids_);
    hash_str_set(f, scratch_skip_ids_);
    f.u(scratch_filled_incarnations_.size());
    for (uint64_t incarnation : scratch_filled_incarnations_) f.u(incarnation);
    f.i(static_cast<int64_t>(last_bar_dual_entry_decision_));
    f.i(trail_close_restart_bar_);
    f.d(trail_best_before_bar_);
    f.i(trail_best_before_bar_index_);
    f.i(trail_best_before_bar_position_cycle_);
    f.u(trail_best_before_bar_fill_seq_);
    f.b(last_exit_fill_was_trail_);
    f.b(current_fill_is_limit_);
    f.i(static_cast<int64_t>(dual_entry_path_));
    f.i(priced_entry_activity_bar_);
    f.b(priced_entry_filled_this_bar_);

    f.u(compat::pine::OrderPriority::schema_version);
    f.b(adapter_.priority.attached());
    f.b(adapter_.priority.retained_parent_first());
    f.u(compat::pine::IntradayCap::schema_version);
    f.i(static_cast<int64_t>(adapter_.cap.attachment()));
    f.i(adapter_.cap.configuration().limit);
    f.b(adapter_.cap.configuration().skip_noop_market);
    f.b(adapter_.cap.configuration().defer_pooc_close);
    f.b(adapter_.cap.configuration().count_pooc_full_close);
    f.b(adapter_.cap.budget().day().has_value());
    if (const auto& day = adapter_.cap.budget().day()) {
        f.i(day->key);
    }
    f.i(adapter_.cap.budget().charged_slots());
    f.b(adapter_.cap.budget().latched());
    f.b(adapter_.cap.budget().transfer().has_value());
    if (const auto& transfer = adapter_.cap.budget().transfer()) {
        f.i(transfer->day.key); f.u(transfer->close_fill);
        f.i(transfer->source_bar); f.u(transfer->inheritor);
    }
    f.b(adapter_.cap.due_cause().has_value());
    if (const auto& due = adapter_.cap.due_cause()) {
        f.u(due->action_id); f.i(due->charged_day.key); f.i(due->charged_slots);
        f.i(due->trigger_bar); f.u(due->trigger_order);
    }
    f.u(adapter_.cap.next_action());

    f.i(static_cast<int64_t>(risk_direction_));
    f.i(risk_max_cons_loss_days_);
    f.d(risk_max_drawdown_);
    f.b(risk_max_drawdown_is_pct_);
    f.d(risk_max_intraday_loss_);
    f.b(risk_max_intraday_loss_is_pct_);
    f.d(risk_max_position_size_);
    f.i(cons_loss_day_count_);
    f.i(last_loss_day_);
    f.b(risk_halted_);
    f.d(intraday_pnl_);
    f.i(intraday_pnl_day_);
    f.d(intraday_loss_day_start_equity_);
    f.i(intraday_loss_day_);
    f.i(intraday_loss_block_day_);
    f.b(intraday_loss_evaluating_);
    f.b(intraday_loss_cancel_pending_);
    f.i(last_margin_call_event_bar_);
    f.i(intrabar_exit_margin_call_bar_);
    f.i(open_margin_slice_bar_);
    f.i(next_order_seq_);
    f.u(exit_leg_event_seq_);
    f.b(coof_scheduler_active_);
    f.b(coof_fill_recalc_active_);
    f.b(coof_cursor_is_bar_close_);
    f.b(coof_cursor_is_bar_point_);
    f.b(coof_evaluating_path_segment_);
    f.b(coof_recalc_at_bar_open_);
    f.b(coof_recalc_after_first_open_fill_);
    f.u(coof_market_entry_recalc_incarnation_);
    f.u(coof_market_entry_recalc_fill_seq_);
    f.b(coof_at_extreme_waypoint_);
    f.b(coof_hist_is_segment_);
    f.i(coof_hist_path_index_);
    f.i(coof_cascade_recalc_leg_);
    f.b(coof_cascade_force_wp_gap_);
    f.d(coof_cursor_price_);
    f.u(coof_direct_fill_events_remaining_);

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    f.u(aux_security_bars_.size());
    for (const auto& bar : aux_security_bars_) {
        f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close);
        f.d(bar.volume); f.i(bar.timestamp);
    }
    f.s(aux_security_input_tf_);
    f.u(aux_security_chart_begin_.size());
    for (std::size_t value : aux_security_chart_begin_) f.u(value);
    f.u(aux_security_chart_end_.size());
    for (std::size_t value : aux_security_chart_end_) f.u(value);
#endif

    f.b(_src_series_active_);
    hash_source_series(f, _src_open_);
    hash_source_series(f, _src_high_);
    hash_source_series(f, _src_low_);
    hash_source_series(f, _src_close_);
    hash_source_series(f, _src_volume_);
    hash_source_series(f, _src_hl2_);
    hash_source_series(f, _src_hlc3_);
    hash_source_series(f, _src_ohlc4_);
    hash_source_series(f, _src_hlcc4_);
    f.d(prev_chart_close_);
    f.d(last_chart_close_);
    f.i(bar_index_offset_);
    f.b(is_first_tick_);
    f.b(is_last_tick_);
    f.b(history_slot_is_new_);
    f.b(coof_checkpoint_contains_current_bar_);
    hash_source_series(f, coof_checkpoint_src_open_);
    hash_source_series(f, coof_checkpoint_src_high_);
    hash_source_series(f, coof_checkpoint_src_low_);
    hash_source_series(f, coof_checkpoint_src_close_);
    hash_source_series(f, coof_checkpoint_src_volume_);
    hash_source_series(f, coof_checkpoint_src_hl2_);
    hash_source_series(f, coof_checkpoint_src_hlc3_);
    hash_source_series(f, coof_checkpoint_src_ohlc4_);
    hash_source_series(f, coof_checkpoint_src_hlcc4_);
    f.d(coof_checkpoint_prev_chart_close_);
    f.d(coof_checkpoint_last_chart_close_);
}

namespace {

void hash_native_handle(BrokerStateHashSink& f, const native_order::RequestHandle& handle) {
    f.s(handle.run.session_key); f.u(handle.run.run_number); f.u(handle.incarnation);
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
    f.b(value.reverse_to); f.s(value.bracket_origin.run.session_key);
    f.u(value.bracket_origin.run.run_number); f.u(value.bracket_origin.incarnation);
    f.u(value.source_sequence);
    f.i(value.placement_script_open_ms);
    f.i(value.placement_sub_open_ms); f.d(value.sizing.equity); f.d(value.sizing.price);
    f.d(value.sizing.fx); f.d(value.sizing.mark); f.d(value.sizing.frozen_units);
    f.b(value.sizing.at_fill); f.d(value.exit_levels.limit); f.d(value.exit_levels.stop);
    f.d(value.exit_levels.trail_points); f.d(value.exit_levels.trail_offset);
    f.d(value.exit_levels.trail_price); f.d(value.exit_levels.profit_ticks);
    f.d(value.exit_levels.loss_ticks);
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
    f.s(kSourceAdapterDomain); f.u(run_counter_); f.u(source_sequence_); f.b(host_ != nullptr);
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
    hash_native_handle_vector(f, live_handles_); hash_native_handle_vector(f, first_open_newborns_);
    hash_native_handle_vector(f, pending_view_handles_);
    std::vector<std::uint64_t> current_debit_ordinals;
    current_debit_ordinals.reserve(current_debited_applied_ordinals_.size());
    for (const auto ordinal : current_debited_applied_ordinals_) current_debit_ordinals.push_back(ordinal);
    std::sort(current_debit_ordinals.begin(), current_debit_ordinals.end());
    f.u(current_debit_ordinals.size());
    for (const auto ordinal : current_debit_ordinals) f.u(ordinal);
    f.u(receipt_cursor_);
    std::vector<std::int64_t> pooc_basis_keys;
    for (const auto& pair : pooc_close_basis_by_script_bar_) pooc_basis_keys.push_back(pair.first);
    std::sort(pooc_basis_keys.begin(), pooc_basis_keys.end()); f.u(pooc_basis_keys.size());
    for (const auto key : pooc_basis_keys) { f.i(key); f.d(pooc_close_basis_by_script_bar_.at(key)); }
    f.d(pooc_open_basis_); f.i(pooc_open_script_bar_);
    f.i(day_ledger_.current_day); f.i(day_ledger_.last_loss_day); f.i(day_ledger_.consecutive_loss_days);
    f.i(day_ledger_.intraday_loss_day); f.d(day_ledger_.intraday_start_equity);
    f.d(day_ledger_.intraday_realized); f.u(day_ledger_.observed_applied_ordinal);
    f.i(risk_.direction); f.i(risk_.max_cons_loss_days); f.d(risk_.max_drawdown);
    f.b(risk_.max_drawdown_percent); f.d(risk_.max_intraday_loss);
    f.b(risk_.max_intraday_loss_percent); f.d(risk_.max_position_size); f.b(risk_.halted);
    hash_native_handle(f, short_seed_.long_entry); hash_native_handle(f, short_seed_.materialize_long);
    hash_native_handle(f, short_seed_.final_short); f.b(short_seed_.active);
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
    f.u(coof_.size());
    for (const auto& interval : coof_) { f.u(interval.applied_ordinal); f.i(interval.script_open_ms); f.b(interval.first_open); }
    f.i(current_script_open_ms_); f.b(saw_open_fill_); f.i(source_bar_count_);
    f.i(expected_source_bars_); f.u(applied_cursor_);
}

void source::PineNativeHost::hash_source_extension(BrokerStateHashSink& f) const {
    f.s("pineforge-source-native-fixture/v2");
    f.b(config_.process_orders_on_close); f.b(config_.calc_on_order_fills);
    f.d(config_.initial_capital); f.i(config_.default_qty_type); f.d(config_.default_qty_value);
    f.i(config_.pyramiding); f.d(config_.commission_value); f.i(config_.commission_type);
    f.i(config_.slippage); f.d(config_.margin_long); f.d(config_.margin_short);
    f.b(config_.close_entries_rule_any); f.b(config_.src_series_active);
    f.d(override_.initial_capital); f.d(override_.commission_value); f.d(override_.default_qty_value);
    f.i(override_.pyramiding); f.i(override_.slippage); f.i(override_.commission_type);
    f.i(override_.default_qty_type); f.i(override_.process_orders_on_close);
    f.i(override_.calc_on_order_fills); f.i(override_.close_entries_rule);
    f.i(static_cast<std::int64_t>(default_qty_type_)); f.d(default_qty_value_);
    f.i(pyramiding_); f.b(close_entries_rule_any_);
    f.i(source_bar_index_); f.i(source_last_bar_index_); f.u(source_callback_count_);
    f.b(source_configuration_captured_);
    adapter_.hash_state(f); scheduler_.hash_state(f);
}

} // namespace pineforge
