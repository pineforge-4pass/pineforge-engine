#include <pineforge/source/pine_strategy_host.hpp>

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

} // namespace pineforge
