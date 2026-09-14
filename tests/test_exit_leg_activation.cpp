// Literal native source-boundary witness. No Pine, reference tape or grader.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cstddef>
namespace prior_leg_mirror {
#include "fixtures/leg_activation/149f77c_pending_mirror.hpp"
}
#include <cstdio>
#include <stdexcept>
#include <string>
static_assert(offsetof(pf_pending_order_v1_t, struct_version) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, struct_version), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, size) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, size), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, id) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, id), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, id_truncated) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, id_truncated), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, id_hash64) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, id_hash64), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, from_entry) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, from_entry), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, from_entry_truncated) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, from_entry_truncated), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, from_entry_hash64) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, from_entry_hash64), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, type) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, type), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, is_long) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, is_long), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, limit_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, limit_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, stop_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, stop_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, trail_points) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, trail_points), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, trail_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, trail_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, trail_offset) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, trail_offset), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, profit_ticks) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, profit_ticks), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, loss_ticks) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, loss_ticks), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, qty_type) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, qty_type), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, qty_percent) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, qty_percent), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_name) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, oca_name), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_name_truncated) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, oca_name_truncated), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_name_hash64) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, oca_name_hash64), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_type) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, oca_type), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_seq) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_seq), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_by_same_id_replacement) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_by_same_id_replacement), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, replaced_default_market_incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, replaced_default_market_incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, declined_by_replaced_short_market) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, declined_by_replaced_short_market), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, replaced_exit_order_incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, replaced_exit_order_incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, recreated_after_named_cancelled_entry_incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, recreated_after_named_cancelled_entry_incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, named_cancel_surviving_exit_incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, named_cancel_surviving_exit_incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, stop_limit_activated) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, stop_limit_activated), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_suppress_stop_on_entry_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, coof_suppress_stop_on_entry_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_suppress_limit_on_entry_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, coof_suppress_limit_on_entry_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_during_coof_recalc) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_during_coof_recalc), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_born_at_close_recalc) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, coof_born_at_close_recalc), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_born_mid_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, coof_born_mid_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_cascade_seg_i) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, coof_cascade_seg_i), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_cascade_inflight_fires) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, coof_cascade_inflight_fires), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_position_side) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_position_side), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_position_cycle_seq) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_position_cycle_seq), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_after_position_close_in_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_after_position_close_in_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, over_pyramiding_cap_at_placement) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, over_pyramiding_cap_at_placement), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, same_id_stop_deferred_close_all_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, same_id_stop_deferred_close_all_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, same_id_stop_deferred_close_all_incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, same_id_stop_deferred_close_all_incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, reverses_same_bar_market_from_flat) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, reverses_same_bar_market_from_flat), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_candidate) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_candidate), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_own_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_own_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_close) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_signal_close), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_equity) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_signal_equity), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_margin_pct) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_signal_margin_pct), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_pointvalue) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_signal_pointvalue), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_fx) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_signal_fx), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_peer_seq) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_peer_seq), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_transaction_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, paired_flat_market_transaction_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, default_flat_market_gross_candidate) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, default_flat_market_gross_candidate), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, tv_carry_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, tv_carry_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, frozen_default_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, frozen_default_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_placement_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, default_stop_placement_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_placement_equity) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, default_stop_placement_equity), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_placement_signal_close) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, default_stop_placement_signal_close), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_sizing_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, default_stop_sizing_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_equity) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sizing_equity), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sizing_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_fx) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sizing_fx), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_mark) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sizing_mark), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, opening_affordability_exemption_candidate) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, opening_affordability_exemption_candidate), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, explicit_flat_admission_candidate) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, explicit_flat_admission_candidate), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, explicit_placement_equity) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, explicit_placement_equity), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, explicit_slipped_signal_close) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, explicit_slipped_signal_close), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_placement_equity) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, affordability_placement_equity), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_signal_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, affordability_signal_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_held_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, affordability_held_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_close_only) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, affordability_close_only), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, rounded_signal_cost_close_only) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, rounded_signal_cost_close_only), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, signal_close_mc_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_entry_incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, signal_close_mc_entry_incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_fill_seq) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, signal_close_mc_fill_seq), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_remaining_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, signal_close_mc_remaining_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, comment) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, comment), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, comment_truncated) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, comment_truncated), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, comment_hash64) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, comment_hash64), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, requested_partial) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, requested_partial), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, full_percent_exit_request) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, full_percent_exit_request), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, pooc_global_full_exit_dynamic_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, pooc_global_full_exit_dynamic_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, pooc_global_full_exit_tracks_bound_adds) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, pooc_global_full_exit_tracks_bound_adds), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, pooc_global_full_exit_bound_add) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, pooc_global_full_exit_bound_add), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, created_while_in_position) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, created_while_in_position), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_member) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sbmt_member), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_own_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sbmt_own_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_tx_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sbmt_tx_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_kept_over_cap) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sbmt_kept_over_cap), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_close_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sbmt_close_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_close_buy) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, sbmt_close_buy), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, suppress_as_declined_reversal_close) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, suppress_as_declined_reversal_close), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_bracket) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_bracket), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_reissue_pending) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_reissue_pending), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_original_stop_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_original_stop_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_hold_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_hold_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_reversal_kill_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_reversal_kill_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_trail_best) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_trail_best), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_trail_best_start) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_trail_best_start), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_trail_leg_dead) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, dormant_trail_leg_dead), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, suppressed_close_consumed_ledger_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, suppressed_close_consumed_ledger_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, suppressed_close_retired_ledger_qty) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, suppressed_close_retired_ledger_qty), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, short_seed_collision_role) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, short_seed_collision_role), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, replaced_order_incarnation) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, replaced_order_incarnation), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_timestamp) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_timestamp), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_cause) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_cause), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_bar) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_bar), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_cursor_domain) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_cursor_domain), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_cursor_position) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_cursor_position), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_cursor_index) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_cursor_index), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_cursor_count) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_cursor_count), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_cursor_price) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_cursor_price), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_first_fill) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_first_fill), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_last_fill) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_last_fill), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, birth_evaluation_ordinal) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, birth_evaluation_ordinal), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, pine_birth_reach) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, pine_birth_reach), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, quantity_intent_kind) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, quantity_intent_kind), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, quantity_intent_units) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, quantity_intent_units), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, quantity_intent_numerator) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, quantity_intent_numerator), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, quantity_intent_denominator) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, quantity_intent_denominator), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, quantity_reservation_present) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, quantity_reservation_present), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, quantity_reservation_units) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, quantity_reservation_units), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, quantity_reservation_basis_units) == offsetof(prior_leg_mirror::pf_pending_order_v1_t, quantity_reservation_basis_units), "preserve existing v1 prefix offset");
static_assert(offsetof(pf_pending_order_v1_t, leg_activation_owner_cycle) >= sizeof(prior_leg_mirror::pf_pending_order_v1_t), "append after old full prefix");
using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
const double nan = std::numeric_limits<double>::quiet_NaN();
const Bar bars[] = {
    {100, 101, 99, 100, 1, 0},
    {100, 105, 95, 100, 1, 60000},
    {90, 112, 80, 100, 1, 120000},
    {100, 112, 80, 100, 1, 180000},
};
class FutureOwner : public pineforge::source::PineStrategyHost {
public:
    explicit FutureOwner(bool stop) : use_stop(stop) {
        calc_on_order_fills_ = true;
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        pyramiding_ = 10;
        syminfo_mintick_ = 0.01;
        commission_value_ = 0;
    }
    bool use_stop;
    bool armed = false;
    int born_bar = -1;
    int bound_bar = -1;
    int64_t born_cycle = 0, bound_cycle = 0;
    uint64_t exit_incarnation = 0, bound_exit_incarnation = 0;
    bool stop_flag_at_birth = false, limit_flag_at_birth = false;
    bool stop_flag_at_binding = false, limit_flag_at_binding = false;
    bool persisted_on_target_entry_bar = false;
    int64_t first_stop_bar = -1, first_limit_bar = -1;
    int64_t rebound_stop_bar = -1, rebound_limit_bar = -1;
    int64_t first_activation_cycle = 0, rebound_activation_cycle = 0;
    int seen_entry_callbacks = 0;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("A", true, nan, nan, 1);
            return;
        }
        if (bar_index_ == 1 && coof_fill_recalc_active_ && !armed) {
            armed = true;
            strategy_entry("A", false, nan, 90, 1);
            strategy_exit("EY", "A", use_stop ? nan : 85, use_stop ? 110 : nan);
            for (const auto& o : pending_orders_) if (o.id == "EY") {
                born_bar = o.created_bar;
                born_cycle = position_cycle_seq_;
                exit_incarnation = o.incarnation;
                stop_flag_at_birth = o.pine_exit_activation.holds_stop();
                limit_flag_at_birth = o.pine_exit_activation.holds_limit();
                if (o.leg_activation.bounds()) {
                    first_stop_bar = o.leg_activation.bounds()->stop_first_bar;
                    first_limit_bar = o.leg_activation.bounds()->limit_first_bar;
                    first_activation_cycle = o.leg_activation.bounds()->position_cycle;
                }
            }
        }
        if (bar_index_ == 2 && coof_fill_recalc_active_
            && position_side_ == PositionSide::SHORT) {
            ++seen_entry_callbacks;
            for (const auto& o : pending_orders_) if (o.id == "EY") {
                bound_bar = position_open_bar_;
                bound_cycle = position_cycle_seq_;
                bound_exit_incarnation = o.incarnation;
                stop_flag_at_binding = o.pine_exit_activation.holds_stop();
                limit_flag_at_binding = o.pine_exit_activation.holds_limit();
                if (o.leg_activation.bounds()) {
                    rebound_stop_bar = o.leg_activation.bounds()->stop_first_bar;
                    rebound_limit_bar = o.leg_activation.bounds()->limit_first_bar;
                    rebound_activation_cycle = o.leg_activation.bounds()->position_cycle;
                }
            }
        }
        if (bar_index_ == 2 && !coof_fill_recalc_active_) {
            persisted_on_target_entry_bar = position_side_ == PositionSide::SHORT;
        }
    }
    void print() const {
        std::printf("%s born_bar=%d bound_bar=%d born_cycle=%lld bound_cycle=%lld exit_inc=%llu bound_exit_inc=%llu birth_stop=%d birth_limit=%d binding_stop=%d binding_limit=%d held_at_target_bar_close=%d target_callbacks=%d\n",
            use_stop ? "stop" : "limit", born_bar, bound_bar,
            (long long)born_cycle, (long long)bound_cycle,
            (unsigned long long)exit_incarnation, (unsigned long long)bound_exit_incarnation,
            stop_flag_at_birth, limit_flag_at_birth, stop_flag_at_binding,
            limit_flag_at_binding, persisted_on_target_entry_bar, seen_entry_callbacks);
        for (const auto& t : trades_)
            std::printf("trade entry=%s exit=%s entry_bar=%d exit_bar=%d qty=%.6f entry_price=%.6f exit_price=%.6f\n",
                t.entry_id.c_str(), t.exit_id.c_str(), t.entry_bar_index,
                t.exit_bar_index, t.qty, t.entry_price, t.exit_price);
    }
    bool contract_holds() const {
        if (born_bar != 1 || bound_bar != 2 || born_cycle != 1 || bound_cycle != 2
            || exit_incarnation != 3 || bound_exit_incarnation != exit_incarnation
            || !persisted_on_target_entry_bar || seen_entry_callbacks != 1
            || first_activation_cycle != born_cycle || rebound_activation_cycle != bound_cycle
            || first_stop_bar != (use_stop ? 2 : 1) || first_limit_bar != (use_stop ? 1 : 2)
            || rebound_stop_bar != (use_stop ? 3 : 2) || rebound_limit_bar != (use_stop ? 2 : 3)
            || stop_flag_at_birth != use_stop || limit_flag_at_birth == use_stop
            || stop_flag_at_binding != stop_flag_at_birth
            || limit_flag_at_binding != limit_flag_at_birth || trades_.size() != 2)
            return false;
        const auto& exit = trades_.back();
        return exit.entry_id == "A" && exit.exit_id == "EY"
            && exit.entry_bar_index == 2 && exit.exit_bar_index == 3
            && exit.qty == 1 && exit.entry_price == 90
            && exit.exit_price == (use_stop ? 110 : 85)
            // A fixed next-bar deadline from original birth is already past
            // when the same exit binds the new position and is held again.
            && bound_bar >= born_bar + 1;
    }
};
}
int preserved_rebinding_controls() {
    int failures = 0;
    for (bool stop : {true, false}) {
        FutureOwner engine(stop);
        engine.run(bars, 4);
        engine.print();
        if (!engine.contract_holds()) ++failures;
    }
    std::printf("literal owner-rebinding contracts: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

namespace {
int native_checks = 0, native_failures = 0;
#define CHECK(x) do { ++native_checks; if (!(x)) { ++native_failures; std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); } } while (0)
class NativeBook : public pineforge::source::PineStrategyHost {
public:
    NativeBook() {
        initial_capital_=100000; commission_value_=0; margin_long_=margin_short_=0;
        pyramiding_=10; current_bar_={100,100,100,100,1,0};
    }
    void on_source_bar(const Bar&) override {}
    void step(double price=100) {
        ++bar_index_; current_bar_={price,price,price,price,1,bar_index_*60000LL};
        process_pending_orders(current_bar_);
    }
    void raw_open(){strategy_order("A",true,1);step();}
    void bracket(){strategy_exit("X","A",120,90);}
    void add(){strategy_order("A",true,1);}
    void partial(){strategy_close("A","",0.5);}
    PendingOrder& exit(){for(auto& o:pending_orders_)if(o.id=="X")return o;throw std::logic_error("missing exit");}
    int64_t cycle()const{return position_cycle_seq_;}
    double quantity()const{return position_qty_;}
    int bar()const{return bar_index_;}
    void replace(){bracket();}
    void cancel(){strategy_cancel("X");}
};
void native_values_and_connected_constraints() {
    ExitLegActivation activation;
    CHECK(activation.stop_ready(1,0));
    activation.bind({7,3,5});
    CHECK(!activation.stop_ready(7,2)&&activation.stop_ready(7,3));
    CHECK(!activation.limit_ready(7,4)&&activation.limit_ready(7,5));
    CHECK(!activation.stop_ready(8,100)&&!activation.limit_ready(8,100));
    bool refused=false;try{activation.bind({0,1,1});}catch(const std::invalid_argument&){refused=true;}
    CHECK(refused&&activation.bounds()->position_cycle==7);
    activation.unbind();CHECK(!activation.bounds());
    NativeBook retained;retained.bracket();
    CHECK(!retained.exit().leg_activation.bounds());
    retained.raw_open();
    CHECK(retained.exit().leg_activation.bounds().has_value());
    CHECK(retained.exit().leg_activation.bounds()->position_cycle==retained.cycle());
    CHECK(retained.exit().leg_activation.bounds()->stop_first_bar==retained.bar());
    NativeBook b;b.raw_open();b.bracket();
    CHECK(b.exit().leg_activation.bounds()->position_cycle==b.cycle());
    CHECK(b.exit().leg_activation.bounds()->stop_first_bar==1);
    CHECK(!b.exit().pine_exit_activation.evidence());
    const auto cycle=b.cycle();
    b.exit().leg_activation.bind({cycle,4,4});
    b.add();b.step();
    CHECK(b.quantity()==2&&b.cycle()==cycle);
    CHECK(b.exit().leg_activation.bounds()->stop_first_bar==4);
    b.partial();b.step();
    CHECK(b.quantity()==1.5&&b.cycle()==cycle);
    CHECK(b.exit().leg_activation.bounds()->stop_first_bar==4);
    const auto before=b.exit().incarnation;
    b.replace();
    CHECK(b.exit().incarnation!=before&&b.exit().replaced_order_incarnation==before);
    CHECK(b.exit().leg_activation.bounds()->stop_first_bar==1);
    b.cancel();b.bracket();CHECK(b.exit().replaced_order_incarnation==0);
    NativeBook bound;bound.raw_open();bound.bracket();
    bound.exit().leg_activation.bind({bound.cycle(),3,3});
    bound.step(90);CHECK(bound.quantity()==1); // actual matcher cannot refresh/delete the bound
    bound.step(90);CHECK(bound.quantity()==0);
}
void original_policy_evidence_survives_new_owner_side() {
    bool invalid=false;
    try { (void)PineExitActivationPolicy({1,1,0,100,110,nan,std::nullopt}); }
    catch(const std::invalid_argument&) { invalid=true; }
    CHECK(invalid);
    PineExitActivationPolicy policy({1,1,1,100,110,nan,std::nullopt});
    CHECK(policy.holds_stop()&&!policy.holds_limit());
    const auto one=policy.resolve(1,1), two=policy.resolve(2,5);
    CHECK(one.position_cycle==1&&one.stop_first_bar==2&&one.limit_first_bar==1);
    CHECK(two.position_cycle==2&&two.stop_first_bar==6&&two.limit_first_bar==5);
    CHECK(policy.evidence()->position_cycle==1&&policy.evidence()->stop_level==110);
    PineExitActivationPolicy recross({1,1,1,110,nan,105,
        compat::pine::LimitContinuation{compat::pine::LimitContinuationCause::FirstHighRecross,9}});
    CHECK(!recross.holds_limit()&&!recross.continues_at_later_open());
    PineExitActivationPolicy later({1,1,1,110,nan,105,
        compat::pine::LimitContinuation{compat::pine::LimitContinuationCause::LaterSameOpen,9}});
    CHECK(!later.holds_limit()&&later.continues_at_later_open());
}
void named_pine_continuations_keep_their_guards() {
    const Bar bar{100,105,90,104,1,120000};
    const std::string id="A";
    PendingOrder order{}; order.type=OrderType::EXIT; order.from_entry=id;
    order.legs.set_stop_price(nan); order.legs.set_limit_price(102); order.legs.set_trail_points(nan); order.legs.set_trail_price(nan);
    order.qty=1; order.quantity_request.request(QuantityIntent::all());
    order.quantity_request.reserve(1,1);
    order.pine_birth_reach=PineHistoricalBirthReach::ExtremeWaypoints;
    compat::pine::ExitActivationContext context{
        bar,PositionSide::LONG,1,2,2,1,1.0,0,1,id,7,
        true,true,105,false,1,false,true,1,7,9,9,
        false,false,false,true,true,0,1,1,true,105};
    const auto high=compat::pine::select_exit_activation(order,nan,102,context);
    CHECK(high.evidence()&&high.evidence()->limit_continuation);
    CHECK(high.evidence()->limit_continuation->cause==compat::pine::LimitContinuationCause::FirstHighRecross);
    CHECK(!high.holds_limit()&&!high.continues_at_later_open());
    context.pending_empty=false;
    const auto competitor=compat::pine::select_exit_activation(order,nan,102,context);
    CHECK(competitor.holds_limit()&&!competitor.evidence()->limit_continuation);
    context.pending_empty=true; context.cursor_price=100; context.after_first_open_fill=true;
    context.recalc_leg=0; context.historical_point=0; context.at_extreme=false;
    order.legs.set_stop_price(101);order.legs.set_limit_price(99);
    const auto later=compat::pine::select_exit_activation(order,101,99,context);
    CHECK(later.holds_stop()&&!later.holds_limit()&&later.continues_at_later_open());
    order.legs.set_trail_points(5);
    const auto trailing=compat::pine::select_exit_activation(order,101,99,context);
    CHECK(trailing.holds_stop()&&trailing.holds_limit()&&!trailing.continues_at_later_open());
    context.fill_recalc=false;
    const auto direct=compat::pine::select_exit_activation(order,101,99,context);
    CHECK(!direct.evidence());
    // Later trigger neutralization cannot mutate the original policy evidence.
    order.legs.set_stop_price(order.legs.set_limit_price(nan));
    CHECK(later.holds_stop()&&later.evidence()->stop_level==101);
}

}
int main(){
    const int preserved=preserved_rebinding_controls();
    try{native_values_and_connected_constraints();original_policy_evidence_survives_new_owner_side();named_pine_continuations_keep_their_guards();}
    catch(const std::exception& e){++native_failures;std::fprintf(stderr,"native exception: %s\n",e.what());}
    std::printf("native leg activation: %d checks, %d failures\n",native_checks,native_failures);
    return preserved||native_failures?1:0;
}
