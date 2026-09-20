#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#include "../broker_state_hash_internal.hpp"

#include <algorithm>

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
    f.d(value.requested_qty); f.d(value.projection_remaining_qty);
    f.d(value.qty_percent); f.b(value.is_long); f.b(value.immediately);
    f.b(value.opening); f.b(value.deferred_cohort);
    f.b(value.reservation_deferred_to_pending_entry);
    f.b(value.fixed_exit_reservation);
    f.b(value.frozen_market_instruction);
    f.d(value.frozen_market_own_units); f.d(value.frozen_market_transaction_units);
    f.b(value.frozen_market_targeted_close); f.b(value.frozen_market_target_was_long);
    f.b(value.direction_gate); f.b(value.affordability_policy_active);
    f.b(value.affordability_close_only);
    f.b(value.rounded_signal_cost_close_only);
    f.b(value.affordability_keep_mc_close_surplus);
    f.b(value.reverse_to); f.b(value.replaced_opening); f.b(value.replacement_predecessor_market);
    f.b(value.terms_priced_reverse);
    f.d(value.frozen_reversal_transaction);
    f.i(value.placement_cycle); f.u(value.sequential_group); f.u(value.sequential_rank);
    f.b(value.has_full_entry_bracket);
    hash_source_run_identity(f, value.paired_reversal_parent.run);
    f.u(value.paired_reversal_parent.incarnation);
    hash_source_run_identity(f, value.preserved_by_close_all.run);
    f.u(value.preserved_by_close_all.incarnation);
    f.i(value.preserved_close_all_bar);
    hash_source_run_identity(f, value.bracket_origin.run);
    f.u(value.bracket_origin.incarnation);
    f.u(value.source_sequence);
    f.u(value.command_ordinal); f.u(value.placement_open_epoch);
    f.u(value.command_sequence);
    f.u(value.close_callsite_token); f.u(value.close_batch_calls);
    f.s(value.close_first_id); f.d(value.close_first_target);
    f.b(value.close_first_ledger_consumed); f.b(value.close_first_carry_valid);
    f.d(value.close_first_carry_qty); f.b(value.close_retire_ledger_whole);
    f.d(value.close_pending_later_qty);
    f.i(value.placement_script_open_ms);
    f.i(value.placement_sub_open_ms); f.i(value.projection_created_bar);
    f.b(value.projection_created_bar_pinned);
    f.b(value.post_parent_calc_level_fill);
    f.i(value.projection_position_side); f.b(value.projection_after_close);
    f.b(value.projection_over_pyramiding);
    f.b(value.projection_opposite_market_predecessor);
    f.u(value.projection_predecessor);
    f.u(value.recreated_after_named_cancelled_entry_incarnation);
    f.u(value.named_cancel_surviving_exit_incarnation);
    f.b(value.retained_parent_topology);
    f.b(value.defer_until_post_parent_calculation);
    f.b(value.projection_predecessor_market); f.b(value.projection_predecessor_exit);
    f.b(value.projection_created_during_coof); f.b(value.projection_coof_at_terminal);
    f.b(value.projection_coof_mid_bar); f.d(value.forced_execution_price);
    f.d(value.projection_tv_carry_qty);
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
    f.d(value.trail_activation_level); f.d(value.retained_trail_best);
    admission::reflect(value.market_admission, "placement.market_admission",
        [&](const admission::Field& field) { hash_admission_field(f, field); });
    f.i(static_cast<std::int64_t>(value.birth.cause())); f.i(value.birth.bar());
    f.i(value.birth.timestamp()); f.i(static_cast<std::int64_t>(value.birth.cursor().domain()));
    f.i(static_cast<std::int64_t>(value.birth.cursor().position()));
    f.i(value.birth.cursor().index()); f.i(value.birth.cursor().count());
    f.d(value.birth.cursor_price()); f.u(value.birth.first_fill()); f.u(value.birth.last_fill());
    f.u(value.birth.evaluation_ordinal()); f.i(static_cast<std::int64_t>(value.birth_reach));
    f.b(value.leg_activation.bounds().has_value());
    if (value.leg_activation.bounds()) {
        f.i(value.leg_activation.bounds()->position_cycle);
        f.i(value.leg_activation.bounds()->stop_first_bar);
        f.i(value.leg_activation.bounds()->limit_first_bar);
    }
    f.b(value.exit_activation.evidence().has_value());
    if (value.exit_activation.evidence()) {
        const auto& evidence = *value.exit_activation.evidence();
        f.i(evidence.position_cycle); f.i(evidence.entry_bar); f.i(evidence.direction);
        f.d(evidence.cursor_price); f.d(evidence.stop_level); f.d(evidence.limit_level);
        f.b(evidence.limit_continuation.has_value());
        if (evidence.limit_continuation) {
            f.i(static_cast<std::int64_t>(evidence.limit_continuation->cause));
            f.u(evidence.limit_continuation->observed_fill_sequence);
        }
    }
    value.legs.visit(f);
    f.b(value.restored_after_margin);
    f.b(value.reservation_expansion.capture().has_value());
    if (value.reservation_expansion.capture()) {
        const auto& capture = *value.reservation_expansion.capture();
        f.i(capture.position_cycle); f.i(static_cast<std::int64_t>(capture.side));
        f.b(capture.first_later_admission.has_value());
        if (capture.first_later_admission) f.u(*capture.first_later_admission);
    }
    f.b(value.reservation_growth_source.reservation_owner().has_value());
    if (value.reservation_growth_source.reservation_owner())
        f.u(*value.reservation_growth_source.reservation_owner());
    f.u(value.reservation_growth_owner_incarnation);
    f.b(value.stop_limit_activated); f.i(value.coof_cascade_seg_i);
    f.b(value.coof_cascade_inflight_fires); f.b(value.paired_flat_market_candidate);
    f.d(value.paired_flat_market_own_qty); f.d(value.paired_flat_market_signal_close);
    f.d(value.paired_flat_market_signal_equity);
    f.d(value.paired_flat_market_signal_margin_pct);
    f.d(value.paired_flat_market_signal_pointvalue); f.d(value.paired_flat_market_signal_fx);
    f.i(value.paired_flat_market_peer_seq); f.d(value.paired_flat_market_transaction_qty);
    f.i(value.signal_close_mc_bar); f.u(value.signal_close_mc_entry_incarnation);
    f.u(value.signal_close_mc_fill_seq); f.d(value.signal_close_mc_remaining_qty);
    f.b(value.pooc_global_full_exit_dynamic_qty);
    f.b(value.pooc_global_full_exit_tracks_bound_adds);
    f.b(value.pooc_global_full_exit_bound_add);
    f.i(static_cast<std::int32_t>(value.cancellation.cause)); f.i(value.cancellation.state);
    f.i(value.cancellation.close_claim_release); f.u(value.cancellation.source_incarnation);
    f.i(value.cancellation.source_sequence); f.u(value.cancellation.target_incarnation);
    f.i(value.cancellation.target_owner); f.u(value.cancellation.target_revision);
    f.d(value.cancellation.close_claim_consumed); f.d(value.cancellation.close_claim_retired);
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
    } else if (const auto* core_sized = std::get_if<native_order::Sized>(&request.intent)) {
        // A core-sized opening carries its whole quantity in the basis, so the
        // source fingerprint must separate two commands that differ only there.
        f.i(static_cast<std::int64_t>(core_sized->side));
        f.u(core_sized->basis.index());
        if (const auto* cash = std::get_if<native_order::CashValue>(&core_sized->basis)) {
            f.d(cash->cash);
        } else {
            f.d(std::get<native_order::EquityFraction>(core_sized->basis).fraction);
        }
        f.i(static_cast<std::int64_t>(core_sized->time));
        f.i(static_cast<std::int64_t>(core_sized->price));
        f.i(static_cast<std::int64_t>(core_sized->grid_policy));
        f.b(core_sized->reserve_percent_fee);
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
    f.u(source_sequence_); f.u(command_ordinal_); f.u(broker_open_epoch_);
    f.i(last_broker_open_ms_); f.u(source_command_sequence_); f.b(host_ != nullptr);
    f.b(config_.process_orders_on_close); f.b(config_.calc_on_order_fills);
    f.d(config_.initial_capital); f.i(config_.default_qty_type); f.d(config_.default_qty_value);
    f.i(config_.pyramiding); f.d(config_.commission_value); f.i(config_.commission_type);
    f.i(config_.slippage); f.d(config_.margin_long); f.d(config_.margin_short);
    f.b(config_.close_entries_rule_any); f.b(config_.src_series_active);
    f.i(static_cast<std::int64_t>(path_order_));
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
        hash_native_handle_vector(f, cohort.origins.members()); hash_native_handle_vector(f, cohort.opened);
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
    for (const auto key : bracket_keys) { f.u(key); hash_native_handle_vector(f, bracket_families_.at(key).members()); }
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
    f.u(delayed_market_orders_.size());
    for (const auto& order : delayed_market_orders_) {
        hash_native_request(f, order.request); hash_placement(f, order.snapshot);
        f.s(order.replacement_key); f.u(order.release_open_epoch);
        f.b(order.execute_at_open);
    }
    f.u(deferred_open_marketable_sells_.size());
    for (const auto& sell : deferred_open_marketable_sells_) {
        hash_placement(f, sell.snapshot); f.s(sell.replacement_key); f.d(sell.fill_price);
        f.d(sell.path_position); f.b(sell.open_marketable);
    }
    f.u(throttled_reopen_rearm_.size());
    for (const auto& snapshot : throttled_reopen_rearm_) hash_placement(f, snapshot);
    f.i(entry_openings_interval_index_); f.i(entry_openings_this_interval_);
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
        f.b(pending.next_open);
    }
    f.u(pending_margin_revivals_.size());
    for (const auto& pending : pending_margin_revivals_) {
        hash_placement(f, pending.snapshot); f.i(pending.decline_bar);
    }
    hash_native_handle_vector(f, live_handles_); hash_native_handle_vector(f, first_open_newborns_);
    // PendingIntentView is a derived read-only alias of live_handles_. Keep
    // its historical fingerprint position and bytes without copying the
    // roster at every mutation boundary.
    hash_native_handle_vector(f, live_handles_);
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
    f.u(trade_exit_phase_.size());
    for (const auto phase : trade_exit_phase_) f.u(phase);
    std::vector<std::uint64_t> current_debit_ordinals;
    current_debit_ordinals.reserve(current_debited_applied_ordinals_.size());
    for (const auto ordinal : current_debited_applied_ordinals_) current_debit_ordinals.push_back(ordinal);
    std::sort(current_debit_ordinals.begin(), current_debit_ordinals.end());
    f.u(current_debit_ordinals.size());
    for (const auto ordinal : current_debit_ordinals) f.u(ordinal);
    std::vector<std::uint64_t> intraday_relabel_ordinals;
    intraday_relabel_ordinals.reserve(intraday_loss_relabel_ordinals_.size());
    for (const auto ordinal : intraday_loss_relabel_ordinals_)
        intraday_relabel_ordinals.push_back(ordinal);
    std::sort(intraday_relabel_ordinals.begin(), intraday_relabel_ordinals.end());
    f.u(intraday_relabel_ordinals.size());
    for (const auto ordinal : intraday_relabel_ordinals) f.u(ordinal);
    std::vector<std::string> consumed_partial_keys;
    consumed_partial_keys.reserve(consumed_partial_exit_cycles_.size());
    for (const auto& row : consumed_partial_exit_cycles_) consumed_partial_keys.push_back(row.first);
    std::sort(consumed_partial_keys.begin(), consumed_partial_keys.end());
    f.u(consumed_partial_keys.size());
    for (const auto& key : consumed_partial_keys) {
        f.s(key); f.i(consumed_partial_exit_cycles_.at(key));
    }
    std::vector<std::uint64_t> shadowed_openings(
        bracket_shadowed_openings_.begin(), bracket_shadowed_openings_.end());
    std::sort(shadowed_openings.begin(), shadowed_openings.end());
    f.u(shadowed_openings.size());
    for (const auto opening : shadowed_openings) f.u(opening);
    std::vector<std::string> named_cancel_keys;
    named_cancel_keys.reserve(named_entry_cancel_tokens_.size());
    for (const auto& row : named_entry_cancel_tokens_) named_cancel_keys.push_back(row.first);
    std::sort(named_cancel_keys.begin(), named_cancel_keys.end());
    f.u(named_cancel_keys.size());
    for (const auto& key : named_cancel_keys) {
        const auto& token = named_entry_cancel_tokens_.at(key);
        f.s(key); f.u(token.entry_incarnation); f.u(token.surviving_exit_incarnation);
    }
    const auto hash_close_units = [&](const auto& values) {
        f.u(values.size());
        for (const auto& row : values) { f.s(row.first); f.d(row.second); }
    };
    hash_close_units(close_logical_units_);
    hash_close_units(close_reserved_units_);
    hash_close_units(close_first_units_);
    const auto hash_close_owners = [&](const auto& owners) {
        f.u(owners.size());
        for (const auto& owner : owners) {
            f.u(owner.first); hash_close_units(owner.second);
        }
    };
    hash_close_owners(close_callsite_reserved_units_);
    hash_close_owners(close_callsite_first_units_);
    f.u(close_batch_callsites_.size());
    for (const auto& row : close_batch_callsites_) {
        const auto& site = row.second;
        f.u(row.first); f.b(site.active); f.u(site.token); f.i(site.calls);
        f.s(site.first_id); f.d(site.first_target);
        f.b(site.first_ledger_consumed); f.b(site.first_carry_valid);
        f.d(site.first_carry_qty); f.s(site.id); f.s(site.comment);
        f.d(site.target); f.b(site.retire_ledger_whole);
        f.u(site.queue_sequence); f.u(site.deferred_cleanup_ids.size());
        for (const auto& id : site.deferred_cleanup_ids) f.s(id);
    }
    f.i(close_batch_bar_); f.u(close_batch_queue_sequence_);
    f.d(close_batch_pending_debt_); f.d(close_batch_admitted_total_);
    f.u(receipt_cursor_);
    f.u(last_applied_ordinal_);
    f.u(terminal_receipt_cursor_);
    f.i(entry_attempt_bar_); f.u(entry_attempts_on_bar_);
    f.b(materializing_relative_);
    f.i(current_position_cycle_);
    f.i(current_position_sign_);
    f.u(next_sequential_group_);
    f.b(source_batch_mutated_);
    f.b(coof_recalc_active_); f.b(coof_first_open_);
    f.u(coof_market_entry_recalc_incarnation_);
    f.u(coof_market_entry_recalc_fill_seq_); f.u(coof_current_fill_seq_);
    // Live only inside a fill recalculation (NaN otherwise); folded then so
    // the idle digest keeps its prior form.
    if (coof_recalc_active_) f.d(coof_fill_cursor_t_);
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
    f.d(last_fx_rate_); f.i(position_open_script_bar_); f.u(position_open_epoch_); f.i(position_open_bar_index_);
    f.u(static_cast<std::uint64_t>(position_open_phase_));
    f.b(position_open_priced_);
    f.i(last_margin_call_script_bar_); f.i(pooc_close_checkpoint_deferred_ms_);
    f.u(last_margin_call_event_ordinal_);
    f.u(last_margin_call_entry_incarnation_); f.i(last_margin_call_position_cycle_);
    f.b(last_margin_call_at_script_close_);
    f.d(last_margin_call_closed_units_); f.d(last_margin_call_remaining_units_);
    f.i(signal_close_mc_event_bar_);
    f.i(signal_close_mc_position_cycle_); f.u(signal_close_mc_entry_incarnation_);
    f.u(signal_close_mc_fill_seq_); f.d(signal_close_mc_before_qty_);
    f.d(signal_close_mc_remaining_qty_); f.i(risk_coof_direct_script_bar_);
    f.u(cap_latest_fill_);
    f.b(source_margin_call_enabled_);
    f.d(policy_script_bar_.open); f.d(policy_script_bar_.high);
    f.d(policy_script_bar_.low); f.d(policy_script_bar_.close);
    f.d(policy_script_bar_.volume); f.i(policy_script_bar_.timestamp);
    f.b(policy_script_bar_valid_);
    // KI-62 market-add provenance (R5 lane L12, 2.ii j). It used to ride the
    // generic lot as PyramidEntry::market_pyramid_add; it is adapter state
    // now, so the source extension folds it. Unordered container: sort the
    // keys, exactly like trail_state_at_open_ below.
    std::vector<std::uint64_t> market_add_keys(market_pyramid_adds_.begin(),
                                               market_pyramid_adds_.end());
    std::sort(market_add_keys.begin(), market_add_keys.end());
    f.u(market_add_keys.size());
    for (const auto key : market_add_keys) f.u(key);
    std::vector<std::uint64_t> trail_open_keys;
    trail_open_keys.reserve(trail_state_at_open_.size());
    for (const auto& row : trail_state_at_open_) trail_open_keys.push_back(row.first);
    std::sort(trail_open_keys.begin(), trail_open_keys.end());
    f.u(trail_open_keys.size());
    for (const auto key : trail_open_keys) {
        const auto& state = trail_state_at_open_.at(key);
        f.u(key); f.b(state.activated); f.d(state.best_price);
        f.d(state.current_level); f.u(state.activation_ordinal);
    }
    f.b(stream_mode_);
    f.i(day_ledger_.current_day); f.i(day_ledger_.last_loss_day); f.i(day_ledger_.consecutive_loss_days);
    f.i(day_ledger_.intraday_loss_day); f.d(day_ledger_.intraday_start_equity);
    f.d(day_ledger_.intraday_realized); f.u(day_ledger_.observed_applied_ordinal);
    f.i(risk_.direction); f.i(risk_.max_cons_loss_days); f.d(risk_.max_drawdown);
    f.b(risk_.max_drawdown_percent); f.d(risk_.max_intraday_loss);
    f.b(risk_.max_intraday_loss_percent); f.d(risk_.max_position_size); f.b(risk_.halted);
    f.d(risk_.observed_peak_equity); f.d(risk_.observed_max_drawdown);
    f.i(risk_.intraday_block_day); f.b(risk_.intraday_cancel_pending);
    hash_short_seed_plan(f, short_seed_);
    hash_short_seed_plan(f, pending_short_seed_.plan);
    f.u(pending_short_seed_.expected_open_epoch); f.b(pending_short_seed_.ready);
    hash_native_handle(f, short_seed_long_candidate_);
    f.i(last_bar_dual_entry_path_); f.i(last_bar_dual_entry_script_open_ms_);
    f.b(pending_view_.owner_ != nullptr);
    f.i(static_cast<std::int64_t>(cap.attachment())); f.i(cap.configuration().limit);
    f.b(cap.configuration().skip_noop_market); f.b(cap.configuration().defer_pooc_close);
    f.b(cap.configuration().count_pooc_full_close);
    const auto& cap_budget = cap.budget();
    f.b(cap_budget.day().has_value());
    if (cap_budget.day()) f.i(cap_budget.day()->key);
    f.i(cap_budget.charged_slots()); f.b(cap_budget.latched());
    f.b(cap_budget.transfer().has_value());
    if (const auto& transfer = cap_budget.transfer()) {
        f.i(transfer->day.key); f.u(transfer->close_fill);
        f.i(transfer->source_bar); f.u(transfer->inheritor);
    }
    f.b(cap.due_cause().has_value());
    if (const auto& due = cap.due_cause()) {
        f.u(due->action_id); f.i(due->charged_day.key); f.i(due->charged_slots);
        f.i(due->trigger_bar); f.u(due->trigger_order);
    }
    f.u(cap.next_action()); f.b(priority.attached());
    f.b(priority.retained_parent_first());
    admission_journal.reflect("journal", [&](const auto& field) { hash_admission_field(f, field); });
}

void source::PineScheduler::hash_state(BrokerStateHashSink& f) const {
    // ab9714be test_live_state_hash_recording: a shorter run is a hash prefix
    // of the same longer feed.  Retained future input is provider transport,
    // not broker continuation state, so fold only the consumed source prefix.
    const std::size_t consumed = std::min(
        retained_.bars.size(), static_cast<std::size_t>(std::max(source_bar_count_, 0)));
    f.s("pineforge-pine-scheduler/v2"); f.u(consumed);
    for (std::size_t index = 0; index < consumed; ++index) {
        const auto& bar = retained_.bars[index];
        f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close); f.d(bar.volume); f.i(bar.timestamp);
    }
    f.s(retained_.input_tf); f.s(retained_.script_tf); f.b(retained_.bar_magnifier);
    f.i(retained_.magnifier_samples); f.i(static_cast<std::int64_t>(retained_.distribution));
    f.b(retained_.volume_weighted); f.i(retained_.volume_weighted_min_samples);
    f.i(retained_.volume_weighted_max_samples); f.b(retained_.is_stream); f.i(retained_.warmup_n);
    f.b(retained_.simple_run);
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
    f.b(current_script_bar_valid_); f.b(saw_open_fill_); f.i(open_point_fills_); f.i(source_bar_count_);
    f.b(expected_source_bars_ >= source_bar_count_);
    f.u(applied_cursor_); f.i(coof_callback_script_open_);
    f.i(last_published_script_open_ms_);
    f.i(prior_input_script_open_ms_);
    f.i(awaiting_legacy_script_open_ms_);
    f.i(last_stream_input_open_ms_);
    const std::size_t completion_prefix = std::min(input_script_completes_.size(), consumed);
    f.u(completion_prefix);
    for (std::size_t index = 0; index < completion_prefix; ++index)
        f.u(input_script_completes_[index]);
    const std::size_t boundary_prefix = std::min(input_script_boundary_completes_.size(), consumed);
    f.u(boundary_prefix);
    for (std::size_t index = 0; index < boundary_prefix; ++index)
        f.u(input_script_boundary_completes_[index]);
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
    // Transient excursion-sampler cache (see pine_strategy_host.hpp): it is
    // re-derived at every precommit, but it is next-decision visible to the
    // excursion sampler inside an in-flight execution, so it is folded here
    // rather than waived.
    f.b(excursion_level_fill_);
    f.i(source_bar_index_); f.u(source_callback_count_);
    f.b(source_configuration_captured_); f.b(source_prepare_failed_);
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
    // Per-site request.security semantics, in sec_id order. Folded under
    // their own domain and only when a site is registered, so a run without
    // request.security keeps the hash it had.
    if (!pine_security_states_.empty()) {
        f.s(kSourceSecurityDomain);
        f.u(pine_security_states_.size());
        for (const auto& [sec_id, pine] : pine_security_states_) {
            f.i(sec_id);
            f.b(pine.lookahead_on);
            f.b(pine.gaps_on);
            f.i(pine.publish_gate_tf_seconds);
            f.b(pine.calling_close_completes_partial);
            f.b(pine.heikinashi);
            f.d(pine.ha_prev_open);
            f.d(pine.ha_prev_close);
            f.b(pine.ha_seeded);
            // The projection in hand, not the prepared list: later buckets
            // are a function of input the run has not consumed yet, and a
            // prefix run must hash like the full run up to its last bar.
            f.b(!pine.historical_projections.empty());
            if (!pine.historical_projections.empty()) {
                f.u(pine.historical_projection_cursor);
                f.b(pine.historical_projection_dispatched);
                const auto& projection = pine.historical_projections[std::min(
                    pine.historical_projection_cursor,
                    pine.historical_projections.size() - 1)];
                f.d(projection.bar.open); f.d(projection.bar.high);
                f.d(projection.bar.low); f.d(projection.bar.close);
                f.d(projection.bar.volume); f.i(projection.bar.timestamp);
                f.i(projection.first_child_ms);
                f.b(projection.is_complete);
            }
            f.b(pine.lower_tf_requested);
            f.b(pine.lower_tf_emulation);
            f.i(pine.lower_tf_ratio);
            f.i(pine.lower_tf_seconds);
            f.b(pine.lower_tf_array_requested);
            f.i(pine.lower_tf_sub_bar_index);
            f.b(pine.lower_tf_use_input);
            f.i(pine.lower_tf_input_aggregation_ratio);
            f.u(pine.lower_tf_input_buffer.size());
            for (const Bar& bar : pine.lower_tf_input_buffer) {
                f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close);
                f.d(bar.volume); f.i(bar.timestamp);
            }
        }
    }
    // The margin slice's sampling chronology is resolved once per pending
    // slice in the precommit pass and read back by the host's own excursion
    // sampler at settlement, so it is folded rather than waived.
    f.b(excursion_margin_prefix_);
    f.b(excursion_margin_fill_only_);
    // The TRAIL peak basis is the precommit view's pre-slip matcher price,
    // which no durable snapshot re-derives at settlement.
    f.d(excursion_trail_raw_price_);
    adapter_.hash_state(f); scheduler_.hash_state(f);
}

} // namespace pineforge
