set(TEST_SOURCES
    # R4-D L4a native-route behavioral twins.  These retain the legacy
    # oracle literals while driving the switched host only through its public
    # source commands and projections.
    test_aapl15_margin_brackets_l4a
    test_carried_pooc_short_margin_state_l4a
    test_l9a_carried_pooc_competing_checkpoint
    test_l9b_open_marketable_order
    test_l9b_open_marketable_shapes
    test_l9g_same_bar_exit_sibling_parents
    test_l10b_excursion_parity
    test_l10e_opposite_entry_close_same_pass
    test_l10d_entry_bar_excursion_masks
    test_l10h_corpus_parity
    test_l9e_zero_price_presence
    test_l10f_sizing_basis_stop_limit
    test_l10g_oca_reduce_deferred_flip
    test_l10j_corpus_parity
    test_l10m_corpus_parity
    test_l10k_corpus_parity
    test_l11a_host_excursion
    test_l11a_host_excursion_twin
    test_e6_entry_bar_mask_declaration
    test_e10_dead_path_prefix
    test_f6_dead_kernel_members
    test_timeframe_trace_switch_once
    test_e19_excursion_path_order
    test_l11b_same_bar_stopout_excursion
    test_l10l_corpus_parity
    test_l10n_exit_reissue_after_close_all
    test_l10w_zero_qty_entry
    test_l10x_reissued_limit_touch_bar
    test_l10q_pyramid_terrace_exit
    test_l10s_pyramiding_same_bar_ids
    test_l10t_partial_exit_residual
    test_l10z_review_fixes
    test_l10ae_allowance_refresh_liveness
    test_l10y_full_equity_entry_split
    test_l10ac_full_equity_single_lot
    test_l10r_dual_stop_same_bar_order
    test_l10p_three_way_entry_bar_exit
    test_l10v_first_partial_leg_timing
    test_l10ad_tick_bracket_stuck_entry
    test_l10ab_same_bar_reentry_exit_level
    test_l10ag_reversal_close_cycle
    test_l10aj_margin_full_exit_flat
    test_l10al_reversal_single_lot
    test_l10ak_same_bar_reversal_close
    test_l10ao_percent_equity_first_lot
    test_l10aq_margin_reissue_liveness
    test_l10ar_priced_entry_fill
    test_l10as_priced_exit_fill
    test_l10ap_coof_chained_fill_price
    test_l10am_entry_bar_priced_exit
    test_l10recon1_merge_fill_liveness
    test_engine_risk_l4a
    test_entry_bar_margin_path_l4a
    test_high_value_fractional_money_margin_l4a
    test_integer_short_margin_state_l4a
    test_intraday_cap_auto_close_l4a
    test_intraday_order_session_day_l4a
    test_intraday_rollover_chart_tz_l4a
    test_margin_admission_gate_l4a
    test_margin_call_l4a
    test_margin_call_1x_long_entry_fill_l4a
    test_margin_call_gap_open_l4a
    test_margin_call_intrabar_chronology_l4a
    test_margin_call_trail_exit_chronology_l4a
    test_margin_stop_admission_l4a
    test_pine_cap_boundary_l4a
    test_risk_max_intraday_loss_tv_l4a
    test_short_margin_script_state_l4a
    test_small_money_margin_residual_l4a
    test_stop_open_margin_script_state_l4a
    test_tv_money_long_margin_call_eth_l4a
    test_unbounded_margin_admission_l4a
    test_unbounded_margin_admission
    test_resolved_execution
    test_native_current_execution
    test_native_fx_curve
    test_native_order_terms_core
    test_native_execution_terms
    test_native_report_truth
    test_native_host_hash_extension
    test_native_report_truth_twin
    test_native_open_lots
    test_native_margin_model
    test_native_margin_hooks
    test_native_margin_hooks_twin
    test_native_margin_fx_roll
    test_native_margin_fx_clock
    test_adapter_report_relower
    test_adapter_margin_relower
    test_adapter_risk_relower
    test_adapter_range_end_relower
    test_native_margin_kernel_r5
    test_adapter_brackets_relower
    test_adapter_grid_relower
    test_adapter_htf_relower
    test_native_queued_reverse_to
    test_native_precommit_view
    test_native_fx_activation
    test_native_fx_curve_c
    test_native_selected_requests
    test_native_selected_core
    test_native_scoped_close
    test_native_selected_settlement
    test_native_settlement_projection
    test_native_settlement_callers
    test_native_reversal_contract
    test_native_reverse_to
    test_source_exact_reversal
    test_source_fifo_endpoints
    test_settlement_observation_boundary
    test_order_action
    test_native_order_core
    test_native_order_terminal
    test_native_order_ergonomics
    test_native_limit_fill_through
    test_native_toolkit_bracket
    test_native_anchored_legs
    test_native_anchored_trail_absent_arm
    test_native_arm_options
    test_native_order_resting_core
    test_native_resting_contract
    test_native_resting_acceptance
    test_native_resting_driver_contract
    test_native_resting_owner_contract
    test_native_resting_group_contract
    test_native_resting_replay_contract
    test_native_resting_matching_contract
    test_native_auto_path
    test_native_market_vertical
    test_native_market_vertical_contract
    test_native_calendar_driver_contract
    test_native_htf_subscriptions
    test_native_htf_subscriptions_twin
    test_native_calc_timing
    test_native_calc_timing_twin
    test_native_htf_subscriptions_stream
    test_native_auxiliary_feed
    test_native_auxiliary_feed_stream
    test_native_auxiliary_feed_twin
    test_native_risk_limits
    test_native_host_repairs
    test_l8c_short_seed_guards
    test_l8c_adapter_ordering
    test_l8c_broker_hash_projection
    test_l8c_kernel_delta_rulings
    test_l9c_c_abi_fx_setter_after_realtime
    test_l9c_broker_hash_recording_independent
    test_native_remaining_path_l5k
    test_native_trail_state_l5k
    test_native_trail_best_seed
    test_native_continuation_portable
    test_native_trail_stop_ladder
    test_native_zero_price_tick_l5o
    test_native_price_grid
    test_native_price_grid_twin
    test_native_adapter_lowering_l1
    test_native_adapter_lowering_l2
    test_native_c_abi_stream_l3a
    test_native_pending_projection_l3a
    test_native_oracle_deferred_any_witnesses_l2
    test_native_oracle_more_than_64_fills_l2
    test_native_oracle_magnifier_barstate_l2
    test_native_oracle_magnifier_distribution_l2
    test_native_oracle_coof_l2
    test_native_sampled_point_activation_l1g
    test_native_sampled_point_ordinal_l1g
    test_native_oracle_pooc_immediate_l2
    test_native_oracle_reversal_l2
    test_native_oracle_frozen_size_l2
    test_native_oracle_deferred_birth_l2
    test_native_oracle_pooc_freeze_l2
    test_native_oracle_relative_exit_l2
    test_native_oracle_reversal_close_only_l2
    test_native_oracle_reversal_later_tick_l2
    test_native_oracle_stop_snapshot_l2
    test_native_oracle_short_seed_l2
    test_native_oracle_short_seed_percent_l2
    test_native_oracle_fx_l2
    test_native_oracle_day_key_l2
    test_native_oracle_reversal_replaced_percent_l2
    test_native_oracle_reversal_same_bar_tx_l2
    test_native_oracle_frozen_size_full_l2
    test_native_oracle_stop_snapshot_full_l2
    test_native_oracle_short_seed_full_l2
    test_native_oracle_short_seed_percent_full_l2
    test_source_layer_native_defaults
    test_source_fingerprint
    test_source_host_intraday_cap_setter
    test_order_action_integration
    test_placement_facts
    test_order_cancellation
    test_market_admission_causality
    test_opposite_intent_facts
    test_pending_order_mirror_no_alloc
    test_market_admission_decisions
    test_market_admission_matrix
    test_pine_transaction_settlement
    test_exit_leg_activation
    test_exit_leg_lifecycle
    test_exit_leg_lifecycle_integration
    test_root_cancel_sole_stop
    test_root_barrier_targeted
    test_exit_barrier_identity
    test_exit_lifecycle_clock
    test_exit_lifecycle_reflection
    test_exit_lifecycle_availability
    test_exit_activation_routes
    test_pending_order_core
    test_pending_order_v1_canary
    test_bulk_preflight
    test_opening_obligation
    test_high_value_price_admission
    test_integer_opening_budget
    test_script_run_prepare
    test_pooc_open_money_event
    test_live_abort
    test_placement_rejection_bracket_ownership
    test_explicit_market_price_admission
    test_integer_short_margin_state
    test_intraday_order_session_day
    test_open_money_before_priced_exit
    test_stop_open_margin_script_state
    test_carried_pooc_short_margin_state
    test_rounded_carried_short_trail
    test_pooc_long_money_before_trail
    test_pooc_flat_signal_cost
    test_small_money_margin_residual
    test_high_value_signal_cost
    test_short_margin_script_state
    test_ringbuffer
    test_timeframe
    test_magnifier
    test_magnifier_real_bars
    test_magnifier_distributions
    test_path_at
    test_na_runtime
    test_math_sum
    test_recompute
    test_str_utils
    test_ta
    test_mfi_source_direction
    test_ta_indicators_extras
    test_kc
    test_ta_rma_warmup
    test_ta_sma_exact_order
    test_ta_sma_window_residual
    test_ta_sma_stochrsi_ties
    test_ta_window_underapplied
    test_ta_window_round10
    test_ta_na_source_rules
    test_matrix
    test_matrix_identity
    test_matrix_na
    test_matrix_snapshot_compat
    test_session_time
    test_native_calendar
    test_native_run_spec
    test_session_day_anchors
    test_time_tradingday
    test_chart_timezone
    test_intraday_rollover_chart_tz
    test_intraday_cap_auto_close
    test_dmi_parity
    test_integration
    test_request_security
    test_aux_security_feed
    test_native_security_feed
    test_split_feed_partial_bucket
    test_ltf_lookahead_first_bucket
    test_htf_weekly_lookahead
    test_htf_chart_close_completion
    test_native_wm_buckets
    test_native_daily_holiday
    test_o_close_pct_day_anchor
    test_oanda_lazy_close
    test_historical_security_lookahead_projection
    test_security_range_start_na_warmup
    test_security_range_start_bucket_gating
    test_chart_ema_na_warmup
    test_security_tf_validation
    test_security_lower_tf_input_passthrough
    test_ltf_buffer_no_leak
    test_security_lower_tf_script_bound
    test_lower_tf_seconds_suffix
    test_engine_trade_accessors
    test_engine_risk
    test_strategy_oca
    test_pending_order_identity
    test_oca_raw_pyramid_add
    test_strategy_pyramiding
    test_pyramiding_count_partial_drain
    test_same_id_stop_replace
    test_exit_id_scoped_erase
    test_max_contracts_held
    test_generic_matrix_primitives
    test_generic_matrix_udt
    test_generic_matrix_bool_proxy
    test_generic_matrix_series
    test_generic_matrix_bool_dispatch
    test_map
    test_session_predicates
    test_syminfo_type
    test_vwap_bands
    test_get_input_int64
    test_get_input_source
    test_syminfo_metadata
    test_affordability_fx
    test_determinism_reproducibility
    test_exit_path_segment_tiebreak
    test_accounting_reconciliation
    test_ta_volume_state_oracle
    test_market_structure_fills
    test_handle_reuse_reset
    test_calendar_aggregation_wm
    test_calendar_wm_open_utc_fastpath
    test_htf_session_close_completion
    test_chart_bar_bucket_label
    test_htf_bucket_real_end
    test_oanda_day_stamp_grid
    test_pine_time_day_stamp_grid
    test_adversarial_ohlcv
    test_report_trace
    test_path_resolve_extra
    test_security_validation_throws
    test_session_calendar_extra
    test_run_inputs_overrides
    test_matrix_ops_extra
    test_timeframe_aggregator_extra
    test_c_abi_setters
    test_fills_edge
    test_default_qty_signal_freeze
    test_sizing_basis_mintick
    test_qty_step_epsilon_floor
    test_margin_admission_gate
    test_reversal_admission_float_guard
    test_direct_short_reversal_affordability
    test_short_reversal_emission
    test_margin_stop_admission
    test_stop_entry_admission
    test_stop_entry_placement_open_qty
    test_default_pct_stop_sizing
    test_stop_decline_continue_path
    test_frozen_flat_gap_reject
    test_market_admission_commission
    test_explicit_qty_fill_admission
    test_market_entry_affordability
    test_percent_equity_affordability
    test_stop_tick_rounding
    test_coof_chart_tick_touch
    test_level_grid_snap
    test_zero_lot_entry_decline
    test_pooc_coof_reversal_gross_admission
    test_limit_fill_slippage
    test_sparse_atr_prev_close
    test_limit_exact_touch_level_residue
    test_tv_fill_rounding
    test_strategy_commands_extra
    test_multi_tier_exit_precedence
    test_same_tick_multi_entry_race
    test_replaced_percent_short_market
    test_full_close_while_pyramiding
    test_integer_lot_percent_exit_min_step
    test_deferred_flip_carry_close_only
    test_close_all_coqueued_entry
    test_same_bar_add_exit_coverage
    test_declined_reversal_close_leg
    test_bracket_lifecycle_declined_reversal
    test_exit_bracket_pending_entry_leg
    test_dual_entry_placement_sizing
    test_fams_same_bar_market_tx
    test_default_flat_market_gross_admission
    test_tv_money_precision
    test_cent_lot_decimal_scale
    test_tv_money_band
    test_famag_close_survives
    test_close_percent_calltime_basis
    test_famag_close_first_admission
    test_famag_opening_money
    test_taro_price_gap_admission
    test_integer_flat_budget_tie
    test_taro_mc_close_residue
    test_live_position_market_gross_admission
    test_lower_tf_parse_extra
    test_ta_ma_warmup_extra
    test_ta_osc_edge
    test_ta_extremes_edge
    test_ta_extremes_ring
    test_ta_voltrend_edge
    test_timezone_concurrency
    test_pointvalue
    test_metrics
    test_drawing
    test_margin_call
    test_high_value_fractional_money_margin
    test_margin_call_intrabar_chronology
    test_margin_call_trail_exit_chronology
    test_margin_call_1x_long_entry_fill
    test_tv_money_long_margin_call_eth
    test_tv_money_carried_pooc
    test_margin_call_gap_open
    test_entry_bar_margin_path
    test_m_admission_36
    test_chart_tf_security_split_feed
    test_risk_max_intraday_loss_tv
    test_aapl15_margin_brackets
    test_percent_equity_open_entry_fee
    test_streaming
    test_stream_preflight_rejections
    test_calc_on_order_fills
    test_coof_cascade_eligibility
    test_coof_open_recalc_context
    test_cascade_exit_gapjump
    test_coof_market_limit_recross
    test_pooc_position_visibility
    test_pooc_short_close_tick
    test_prearmed_market_parent_gap_exit
    test_prearmed_bracket_fill_bar
    test_relative_exit_after_limit_parent
    test_short_seed_close_collision
    test_short_seed_collision_percent
    test_exit_bracket_position_cycle_lifetime
    test_range_end_close
    test_close_id_retires_ledger
    test_trail_open_arm_subtick_offset
    test_trail_fill_snap
    test_zero_offset_trail_rides
    test_trail_ref_entry_bar_extreme
    test_trail_close_restart_no_fold
    test_pooc_retained_trail_path
    test_coof_resting_stop_siblings
    test_famx_declined_reversal_trail_leg
    test_famae_lot_sizing_ten_digit_equity
    test_famae_declined_reversal_trail_gap
    test_trail_activation_tick_bar
    test_session_predicates_daily_chart
    test_pooc_money_admission
    test_dual_stop_transactions
    test_live_run_status
    test_live_realtime_tail
    test_live_probe_suppress_tail
    test_live_path_order
    test_pyramid_entry_state_hash
    test_pine_security_state_hash
    test_live_state_hash_recording
    test_live_order_derived
    test_live_trade_accessors
    test_live_flags_off_identity
    test_live_flags_lane_positive
    test_native_l4c_policy_modules
    test_native_l4c_coof_literals
    test_adapter_coof_relower
    test_native_l4c_pending_mirror
    test_native_l4c_oracle_reversal_literals
    test_l8b_exit_activation
    test_l8b_trail_lifecycle
    test_l8b_reservation_birth
    test_l8b_pending_mirror
    test_l8b_exact_reversal_literals
    test_live_pending_order_mirror
    test_oracle_coof_first_open
    test_oracle_reversal
    test_trail_close_restart_no_fold_l4c
    test_bracket_lifecycle_declined_reversal_l4c
    test_calc_on_order_fills_l4c
    test_cascade_exit_gapjump_l4c
    test_coof_chart_tick_touch_l4c
    test_coof_market_limit_recross_l4c
    test_coof_open_recalc_context_l4c
    test_coof_resting_stop_siblings_l4c
    test_declined_reversal_close_leg_l4c
    test_dropped_reversal_mc_first_l4c
    test_exit_activation_routes_l4c
    test_exit_bracket_position_cycle_lifetime_l4c
    test_exit_id_scoped_erase_l4c
    test_exit_leg_activation_l4c
    test_exit_leg_lifecycle_integration_l4c
    test_exit_lifecycle_availability_l4c
    test_exit_lifecycle_clock_l4c
    test_exit_lifecycle_reflection_l4c
    test_famae_declined_reversal_trail_gap_l4c
    test_famx_declined_reversal_trail_leg_l4c
    test_multi_tier_exit_precedence_l4c
    test_order_birth_provenance_l4c
    test_placement_rejection_bracket_ownership_l4c
    test_pooc_coof_reversal_gross_admission_l4c
    test_pooc_global_full_exit_l4c
    test_pooc_retained_trail_path_l4c
    test_prearmed_bracket_fill_bar_l4c
    test_prearmed_exit_path_cursor_l4c
    test_prearmed_market_parent_gap_exit_l4c
    test_reservation_expansion_l4c
    test_reversal_admission_float_guard_l4c
    test_rounded_carried_short_trail_l4c
    test_same_bar_add_exit_coverage_l4c
    test_strategy_oca_l4c
    test_trail_activation_tick_bar_l4c
    test_trail_fill_snap_l4c
    test_trail_open_arm_subtick_offset_l4c
    test_trail_ref_entry_bar_extreme_l4c
    test_zero_offset_trail_rides_l4c
    test_l4e_trade_export_provenance
    test_l4e_native_settlement_h07
    test_l4e_native_install_h08
    test_l4f_security_magnifier
    test_l4g_runtime_budget
    test_l4h_native_settlement_arithmetic
)

# L3b deletes the former compatibility-owner bodies. These targets directly
# drove that owner (or mutated its book/seams outside a native callback), so
# they have no truthful switched-route observation. Native adapter/oracle twins
# cover their public behavior; the L3b disposition ledger records each mapping.
set(L3A_LEGACY_OWNER_TEST_SOURCES
    test_unbounded_margin_admission
    test_settlement_observation_boundary
    test_placement_facts
    test_pending_order_mirror_no_alloc
    test_market_admission_decisions
    test_market_admission_matrix
    test_pine_transaction_settlement
    test_exit_leg_activation
    test_exit_leg_lifecycle_integration
    test_root_cancel_sole_stop
    test_exit_lifecycle_clock
    test_exit_lifecycle_reflection
    test_exit_lifecycle_availability
    test_exit_activation_routes
    test_pending_order_core
    test_pending_order_v1_canary
    test_bulk_preflight
    test_high_value_price_admission
    test_integer_opening_budget
    test_script_run_prepare
    test_pooc_open_money_event
    test_live_abort
    test_placement_rejection_bracket_ownership
    test_explicit_market_price_admission
    test_integer_short_margin_state
    test_intraday_order_session_day
    test_open_money_before_priced_exit
    test_stop_open_margin_script_state
    test_carried_pooc_short_margin_state
    test_rounded_carried_short_trail
    test_pooc_long_money_before_trail
    test_pooc_flat_signal_cost
    test_small_money_margin_residual
    test_high_value_signal_cost
    test_short_margin_script_state
    test_timeframe
    test_magnifier_real_bars
    test_intraday_cap_auto_close
    test_integration
    test_request_security
    test_aux_security_feed
    test_native_security_feed
    test_split_feed_partial_bucket
    test_ltf_lookahead_first_bucket
    test_htf_weekly_lookahead
    test_htf_chart_close_completion
    test_native_wm_buckets
    test_native_daily_holiday
    test_oanda_lazy_close
    test_historical_security_lookahead_projection
    test_security_range_start_na_warmup
    test_security_range_start_bucket_gating
    test_chart_ema_na_warmup
    test_security_tf_validation
    test_security_lower_tf_input_passthrough
    test_ltf_buffer_no_leak
    test_security_lower_tf_script_bound
    test_engine_risk
    test_strategy_oca
    test_pending_order_identity
    test_strategy_pyramiding
    test_pyramiding_count_partial_drain
    test_exit_id_scoped_erase
    test_get_input_source
    test_syminfo_metadata
    test_market_structure_fills
    test_security_validation_throws
    test_run_inputs_overrides
    test_fills_edge
    test_sizing_basis_mintick
    test_qty_step_epsilon_floor
    test_margin_admission_gate
    test_reversal_admission_float_guard
    test_direct_short_reversal_affordability
    test_margin_stop_admission
    test_stop_entry_admission
    test_stop_entry_placement_open_qty
    test_default_pct_stop_sizing
    test_stop_decline_continue_path
    test_frozen_flat_gap_reject
    test_market_admission_commission
    test_explicit_qty_fill_admission
    test_market_entry_affordability
    test_percent_equity_affordability
    test_stop_tick_rounding
    test_coof_chart_tick_touch
    test_level_grid_snap
    test_pooc_coof_reversal_gross_admission
    test_limit_fill_slippage
    test_sparse_atr_prev_close
    test_limit_exact_touch_level_residue
    test_tv_fill_rounding
    test_strategy_commands_extra
    test_multi_tier_exit_precedence
    test_full_close_while_pyramiding
    test_integer_lot_percent_exit_min_step
    test_close_all_coqueued_entry
    test_same_bar_add_exit_coverage
    test_declined_reversal_close_leg
    test_bracket_lifecycle_declined_reversal
    test_dual_entry_placement_sizing
    test_default_flat_market_gross_admission
    test_tv_money_precision
    test_cent_lot_decimal_scale
    test_tv_money_band
    test_famag_close_survives
    test_famag_close_first_admission
    test_famag_opening_money
    test_taro_price_gap_admission
    test_integer_flat_budget_tie
    test_taro_mc_close_residue
    test_live_position_market_gross_admission
    test_metrics
    test_margin_call
    test_high_value_fractional_money_margin
    test_margin_call_intrabar_chronology
    test_margin_call_trail_exit_chronology
    test_margin_call_1x_long_entry_fill
    test_tv_money_long_margin_call_eth
    test_tv_money_carried_pooc
    test_margin_call_gap_open
    test_entry_bar_margin_path
    test_m_admission_36
    test_chart_tf_security_split_feed
    test_risk_max_intraday_loss_tv
    test_aapl15_margin_brackets
    test_percent_equity_open_entry_fee
    test_streaming
    test_stream_preflight_rejections
    test_calc_on_order_fills
    test_coof_open_recalc_context
    test_cascade_exit_gapjump
    test_coof_market_limit_recross
    test_pooc_short_close_tick
    test_prearmed_market_parent_gap_exit
    test_prearmed_bracket_fill_bar
    test_short_seed_close_collision
    test_exit_bracket_position_cycle_lifetime
    test_range_end_close
    test_close_id_retires_ledger
    test_trail_open_arm_subtick_offset
    test_trail_fill_snap
    test_zero_offset_trail_rides
    test_trail_ref_entry_bar_extreme
    test_trail_close_restart_no_fold
    test_pooc_retained_trail_path
    test_coof_resting_stop_siblings
    test_famx_declined_reversal_trail_leg
    test_famae_declined_reversal_trail_gap
    test_trail_activation_tick_bar
    test_session_predicates_daily_chart
    test_pooc_money_admission
    test_dual_stop_transactions
    test_live_realtime_tail
    test_live_probe_suppress_tail
    test_live_path_order
    test_live_state_hash_recording
    test_live_order_derived
    test_live_trade_accessors
    test_live_flags_lane_positive
)
# A24: these source-host security/timeframe/admission tests are behavioral
# coverage, not approved legacy-owner inventory removals.  They run unchanged
# against the switched route.
list(REMOVE_ITEM L3A_LEGACY_OWNER_TEST_SOURCES
    test_chart_tf_security_split_feed
    test_get_input_source
    test_htf_chart_close_completion
    test_htf_weekly_lookahead
    test_live_abort
    test_ltf_buffer_no_leak
    test_ltf_lookahead_first_bucket
    test_market_admission_decisions
    test_oanda_lazy_close
    test_security_lower_tf_input_passthrough
    test_security_lower_tf_script_bound
    test_security_range_start_bucket_gating
    test_security_range_start_na_warmup
    test_security_tf_validation
    test_security_validation_throws
    test_split_feed_partial_bucket
    test_syminfo_metadata
    test_timeframe
)
list(REMOVE_ITEM TEST_SOURCES ${L3A_LEGACY_OWNER_TEST_SOURCES})

# L3b removes the two remaining direct owner-seam probes. Their behavioural
# coverage is carried by the registered native settlement and reversal suites.
list(REMOVE_ITEM TEST_SOURCES
    test_native_reversal_contract
    test_native_reverse_to
    test_native_settlement_callers
    test_source_exact_reversal
    test_source_fifo_endpoints
    test_order_action_integration
    test_opposite_intent_facts
    test_exit_leg_lifecycle
    test_exit_barrier_identity
    test_fams_same_bar_market_tx
    test_path_resolve_extra
    test_same_id_stop_replace
    test_short_seed_collision_percent
    test_intraday_rollover_chart_tz
)

# L4d switched-route restoration twins. Keep the original L3A inventory
# immutable: each restored witness has a separately named native-route TU.
list(APPEND TEST_SOURCES
    test_native_live_order_derived_l4d
    test_native_live_pending_order_mirror_l4d
    test_native_pending_order_v1_canary_l4d
    test_native_source_fifo_endpoints_l4d
    test_native_session_key_derivation_l4d
    test_native_market_admission_projection_l4d
    test_native_live_state_hash_l4d
    test_native_oracle_reversal_f8_l4d
    test_live_flags_lane_positive_l4d
    test_live_path_order_l4d
    test_live_realtime_tail_l4d
    test_live_state_hash_recording_l4d
    test_m_admission_36_l4d
    test_pending_order_mirror_no_alloc_l4d
    test_stream_preflight_rejections_l4d
    test_taro_price_gap_admission_l4d
    test_aux_security_feed_l4d
    test_chart_ema_na_warmup_l4d
    test_famag_close_survives_l4d
    test_frozen_flat_gap_reject_l4d
    test_full_close_while_pyramiding_l4d
    test_historical_security_lookahead_projection_l4d
    test_integer_flat_budget_tie_l4d
    test_integer_opening_budget_l4d
    test_level_grid_snap_l4d
    test_limit_exact_touch_level_residue_l4d
    test_limit_fill_slippage_l4d
    test_market_structure_fills_l4d
    test_metrics_l4d
    test_native_daily_holiday_l4d
    test_native_security_feed_l4d
    test_native_wm_buckets_l4d
    test_pooc_short_close_tick_l4d
    test_pyramiding_count_partial_drain_l4d
    test_range_end_close_l4d
    test_session_predicates_daily_chart_l4d
    test_stop_tick_rounding_l4d
    test_tv_fill_rounding_l4d
    test_cancellation_mirror_coverage_l4d
    test_default_flat_market_gross_admission_l4d
    test_explicit_market_price_admission_l4d
    test_famag_close_first_admission_l4d
    test_high_value_price_admission_l4d
    test_live_order_derived_l4d
    test_live_pending_order_mirror_l4d
    test_live_position_market_gross_admission_l4d
    test_live_probe_suppress_tail_l4d
    test_live_state_hash_l4d
    test_live_trade_accessors_l4d
    test_market_admission_matrix_l4d
    test_market_admission_state_l4d
    test_pending_order_core_l4d
    test_pending_order_identity_l4d
    test_pending_order_v1_canary_l4d
    test_pending_placement_receipts_l4d
    test_pending_quantity_intent_l4d
    test_placement_facts_l4d
    test_stop_entry_admission_l4d
    test_streaming_l4d
    test_bulk_preflight_l4d
    test_close_all_coqueued_entry_l4d
    test_close_id_retires_ledger_l4d
    test_dual_stop_transactions_l4d
    test_fills_edge_l4d
    test_frozen_market_instruction_l4d
    test_high_value_signal_cost_l4d
    test_integration_l4d
    test_pine_transaction_settlement_l4d
    test_pooc_flat_signal_cost_l4d
    test_request_security_l4d
    test_root_cancel_sole_stop_l4d
    test_run_inputs_overrides_l4d
    test_script_run_prepare_l4d
    test_settlement_observation_boundary_l4d
    test_sparse_atr_prev_close_l4d
    test_stop_decline_continue_path_l4d
    test_strategy_commands_extra_l4d
    test_strategy_pyramiding_l4d
    test_taro_mc_close_residue_l4d
)
# R4-D L4b public native-route policy witness.  Kept as an appended target so
# the retired owner inventory above remains mechanically intact for merge.
list(APPEND TEST_SOURCES test_l4b_policy_regressions)

# R5 L3 kernel sizing bases and fractional reduces.  Appended for the same
# reason: the inventory above stays mechanically intact.
list(APPEND TEST_SOURCES test_native_sizing_bases)

# R5 L3b generic sizing-price rule, placement-time sizing and the reversal
# shapes a kernel-sized opening may name.
list(APPEND TEST_SOURCES test_native_sizing_price_rule)

# R5 R2 re-lowering of the Pine adapter's default quantity onto the core's
# Sized intent: the classification, the neutrality of the re-lowering and the
# core-plus-override differential.
list(APPEND TEST_SOURCES test_adapter_sizing_relower)

# R5 E9: TradingView's trail activation tapes (tests/fixtures/
# trail_activation_tick_reach) replayed through the Pine adapter, with the
# kernel's record of the one-shot's resting limit and booked price.
list(APPEND TEST_SOURCES test_trail_activation_tick_reach)

# R5 N14: the live-runtime tail overrides are the source host's; the kernel
# exposes only the virtual seams behind the frozen C setters. Source-free,
# so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_tail_override_seam)

# R5 E5: TradingView's offset-trail arm tapes (tests/fixtures/offset_trail_arm)
# replayed through the Pine adapter, with the kernel's record of the arm.
list(APPEND TEST_SOURCES test_offset_trail_quantized_arm)

# R5 E24: the continuation digest folds an append-only log's tail, so a host
# that reads the continuation once per bar stays linear in the bar count.
list(APPEND TEST_SOURCES test_native_continuation_digest_tail)
# R5 E25: session.islastbar on an aggregated chart, read as the chart-timeframe
# path reads it, and TradingView's session-bar tapes (tests/fixtures/
# session_islastbar) replayed through the Pine adapter on both paths.
list(APPEND TEST_SOURCES test_session_islastbar_aggregation)

# R5 E27: where a process_orders_on_close fill is dated on the aggregated path,
# against the chart-timeframe path and against TradingView's own tape.
list(APPEND TEST_SOURCES test_pooc_fill_stamp_aggregation)

# R5 F3: the contracts the kernel documents for every host, witnessed on bare
# hosts. Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_bare_host_contracts)
# R5 F5: session-day facts on the native decision context, read by a bare
# host from the kernel alone: the E25/E26 TradingView tapes, the audit's
# overnight Tokyo session, streams and a fill recalculation. Source-free, so
# it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_session_day_facts)

# R5 F5: the Pine adapter selects those facts instead of computing its own:
# the same tapes flag for flag through the adapter, its live-probe tail
# policy, and the close flags of a calc_on_order_fills run. Source-bound.
list(APPEND TEST_SOURCES test_session_day_facts_adapter)
# R5 F1: the aggregated-path regressions of R4 slice C — trade rows on input
# bars, close fills dated at the bucket close, and calc_on_order_fills
# recalculations reading the previous bar's session flags — against ab9714be.
list(APPEND TEST_SOURCES test_aggregated_path_regressions)
# R5 F14: kernel buckets at session boundaries -- a session with breaks
# aggregates on its whole session day. Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_session_boundaries)
# R5 F9: every recording-mode broker-state hash of six Pine scenarios, pinned
# on fd785928 -- the values an incremental adapter fold must reproduce.
list(APPEND TEST_SOURCES test_adapter_recording_hash_witness)

# R4-D L4b-fix restores every removed ShortSeed, affordability, sizing and
# deferred-close oracle as an individually compiled native-route twin.  Keep
# this list at the end of TEST_SOURCES: the retired-owner inventory above is
# intentionally immutable for the parallel landing merge.
list(APPEND TEST_SOURCES
    test_cent_lot_decimal_scale_l4b
    test_default_pct_stop_sizing_l4b
    test_direct_short_reversal_affordability_l4b
    test_dual_entry_placement_sizing_l4b
    test_explicit_qty_fill_admission_l4b
    test_famag_opening_money_l4b
    test_integer_lot_percent_exit_min_step_l4b
    test_magnifier_real_bars_l4b
    test_market_admission_commission_l4b
    test_market_entry_affordability_l4b
    test_open_money_before_priced_exit_l4b
    test_percent_equity_affordability_l4b
    test_percent_equity_open_entry_fee_l4b
    test_pooc_long_money_before_trail_l4b
    test_pooc_money_admission_l4b
    test_pooc_open_money_event_l4b
    test_qty_step_epsilon_floor_l4b
    test_short_seed_close_collision_l4b
    test_sizing_basis_mintick_l4b
    test_stop_entry_placement_open_qty_l4b
    test_tv_money_band_l4b
    test_tv_money_carried_pooc_l4b
    test_tv_money_precision_l4b
    test_short_seed_collision_percent_l4b
    test_source_fifo_endpoints_l4b
    test_native_p7d_l4b
    test_l8a_adapter_policy_delta
)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# === Kernel-only registration =========================================
# With PINEFORGE_BUILD_SOURCE_LAYER=OFF libpineforge.a holds the kernel
# alone, so a test TU that reaches a pineforge/source or compat/pine header
# has nothing to link against. The reach is resolved here instead of being
# listed by hand: a switched-route twin that includes its L0 carrier
# verbatim inherits whatever that carrier includes.
#
# A lane's adapter twin therefore lives in a sibling *_twin.cpp, with what
# both halves share in a source-free *_fixture.hpp, so the lane's native
# witnesses keep running here while the twin is the TU this filter drops.
# scripts/ci_verify.py pins the resulting kernel-only row count as
# KERNEL_MIN_TESTS; a TU that silently becomes source-bound fails that gate.
function(pineforge_test_reaches_source_layer out_var entry)
    set(_pending "${entry}")
    set(_seen)
    while(_pending)
        list(POP_FRONT _pending _current)
        if(_current IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_current}")
        if(NOT EXISTS "${_current}")
            continue()
        endif()
        get_filename_component(_directory "${_current}" DIRECTORY)
        file(STRINGS "${_current}" _includes REGEX "^[ \t]*#[ \t]*include")
        foreach(_line IN LISTS _includes)
            if(_line MATCHES "pineforge/(source|compat/pine)/")
                set(${out_var} TRUE PARENT_SCOPE)
                return()
            endif()
            if(_line MATCHES "\"([^\"]+)\"")
                list(APPEND _pending "${_directory}/${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endwhile()
    set(${out_var} FALSE PARENT_SCOPE)
endfunction()

# add_test/set_tests_properties rows that name a skipped twin would either
# break configuration or register a command that is not a target, so both
# are routed through existence-checked wrappers. Under the default build
# every row still exists and nothing is filtered.
function(pineforge_add_twin_test name target)
    if(TARGET ${target})
        add_test(NAME ${name} COMMAND ${target} ${ARGN})
    endif()
endfunction()

function(pineforge_clear_test_labels)
    set(_rows)
    foreach(_row IN LISTS ARGN)
        if(TEST ${_row})
            list(APPEND _rows ${_row})
        endif()
    endforeach()
    if(_rows)
        set_tests_properties(${_rows} PROPERTIES LABELS "")
    endif()
endfunction()

if(NOT PINEFORGE_BUILD_SOURCE_LAYER)
    set(_pf_source_bound_tests)
    foreach(test_name IN LISTS TEST_SOURCES)
        pineforge_test_reaches_source_layer(_pf_reaches
            "${CMAKE_CURRENT_SOURCE_DIR}/${test_name}.cpp")
        if(_pf_reaches)
            list(APPEND _pf_source_bound_tests ${test_name})
        endif()
    endforeach()
    if(_pf_source_bound_tests)
        list(REMOVE_ITEM TEST_SOURCES ${_pf_source_bound_tests})
        list(LENGTH _pf_source_bound_tests _pf_source_bound_count)
        message(STATUS
            "Kernel-only tests: ${_pf_source_bound_count} source-bound test TUs "
            "skipped (PINEFORGE_BUILD_SOURCE_LAYER is OFF)")
    endif()
endif()

add_test(NAME test_cmake_version_source
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_cmake_version_source.py)
add_test(NAME test_ci_verify
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_verify.py)
add_test(NAME test_ci_preflight
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py)
add_test(NAME test_twin_parity_mutations
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_twin_parity.py)

add_test(
    NAME test_aggregate_cpp_versions
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_aggregate_cpp_versions.py
)

set(_pf_script_cpp_abi_flags)
set(_pf_abi_receipt_mode --skip-if-receipt-missing)
if(PINEFORGE_REQUIRE_ABI_RECEIPTS)
    set(_pf_abi_receipt_mode --require-receipts)
endif()
if(PINEFORGE_ENABLE_SANITIZERS)
    list(APPEND _pf_script_cpp_abi_flags --extra-flag=-fsanitize=address,undefined)
endif()
if(PINEFORGE_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _pf_script_cpp_abi_flags
            --extra-flag=-fprofile-instr-generate --extra-flag=-fcoverage-mapping)
    else()
        list(APPEND _pf_script_cpp_abi_flags --extra-flag=--coverage)
    endif()
endif()
# The script ABI pair derives from pineforge::source::PineStrategyHost.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_test(
        NAME test_script_cpp_abi
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_script_cpp_abi.py
                --compiler ${CMAKE_CXX_COMPILER}
                --library $<TARGET_FILE:pineforge>
                --include ${PROJECT_SOURCE_DIR}/include
                --generated-include ${PROJECT_BINARY_DIR}/include
                --v16-frozen-receipt ${PROJECT_BINARY_DIR}/native-abi-v16-frozen/receipt.json
                --receipt ${PROJECT_BINARY_DIR}/script-abi-receipt.json
                ${_pf_abi_receipt_mode}
                ${_pf_script_cpp_abi_flags}
    )
    set_tests_properties(test_script_cpp_abi PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()

add_test(
    NAME test_script_cpp_abi_tooling
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_script_cpp_abi.py
)

add_test(
    NAME test_native_cpp_versions
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_native_cpp_versions.py
)
add_test(
    NAME test_native_include_independence_tooling
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_native_include_independence.py
)
add_test(
    NAME test_native_source_guard
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_native_source_guard.py
)
add_test(
    NAME test_native_cpp_abi
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_native_cpp_abi.py
            --compiler ${CMAKE_CXX_COMPILER}
            --library $<TARGET_FILE:pineforge>
            --include ${PROJECT_SOURCE_DIR}/include
            --generated-include ${PROJECT_BINARY_DIR}/include
            --receipt ${PROJECT_BINARY_DIR}/native-abi-receipt.json
            ${_pf_script_cpp_abi_flags}
)
set_tests_properties(test_native_cpp_abi PROPERTIES TIMEOUT 240)

# The actual historical archive is prepared explicitly before CTest so the
# compatibility check itself stays offline and cannot silently use a stub.
set(PINEFORGE_SETTLEMENT_ABI_BASE_RECEIPT
    "${PROJECT_BINARY_DIR}/settlement-abi-base/receipt.json"
    CACHE FILEPATH "Prepared actual R2 ABI provider receipt")
set(PINEFORGE_SETTLEMENT_ABI_PRIOR_RECEIPT
    "${PROJECT_BINARY_DIR}/settlement-abi-prior/receipt.json"
    CACHE FILEPATH "Prepared actual 0e18690 ABI provider receipt")
set(PINEFORGE_NATIVE_ABI_V13_RECEIPT
    "${PROJECT_BINARY_DIR}/native-abi-v13/receipt.json"
    CACHE FILEPATH "Prepared actual c3ed455 epoch 13 ABI provider receipt")
set(PINEFORGE_NATIVE_ABI_V14_RECEIPT
    "${PROJECT_BINARY_DIR}/native-abi-v14/receipt.json"
    CACHE FILEPATH "Prepared actual f736676 epoch 14 ABI provider receipt")
set(PINEFORGE_NATIVE_ABI_V15_FROZEN_RECEIPT
    "${PROJECT_BINARY_DIR}/native-abi-v15-frozen/receipt.json"
    CACHE FILEPATH "Prepared actual e7cdf052 frozen epoch 15 ABI provider receipt")
set(PINEFORGE_NATIVE_ABI_V16_FROZEN_RECEIPT
    "${PROJECT_BINARY_DIR}/native-abi-v16-frozen/receipt.json"
    CACHE FILEPATH "Prepared actual ab9714b frozen epoch 16 ABI provider receipt")
# Same pairing TU, against the frozen settlement providers.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_test(NAME test_settlement_cpp_abi
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_settlement_cpp_abi.py
            --compiler ${CMAKE_CXX_COMPILER}
            --library $<TARGET_FILE:pineforge>
            --include ${PROJECT_SOURCE_DIR}/include
            --generated-include ${PROJECT_BINARY_DIR}/include
            --base-receipt ${PINEFORGE_SETTLEMENT_ABI_BASE_RECEIPT}
            --prior-receipt ${PINEFORGE_SETTLEMENT_ABI_PRIOR_RECEIPT}
            --v13-receipt ${PINEFORGE_NATIVE_ABI_V13_RECEIPT}
            --v14-receipt ${PINEFORGE_NATIVE_ABI_V14_RECEIPT}
            --v15-frozen-receipt ${PINEFORGE_NATIVE_ABI_V15_FROZEN_RECEIPT}
            --v16-frozen-receipt ${PINEFORGE_NATIVE_ABI_V16_FROZEN_RECEIPT}
            --receipt ${PROJECT_BINARY_DIR}/settlement-abi-receipt.json
            ${_pf_abi_receipt_mode}
            ${_pf_script_cpp_abi_flags})
    set_tests_properties(test_settlement_cpp_abi PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endif()

add_test(NAME test_settlement_cpp_abi_tooling
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_settlement_cpp_abi.py)
# A receipt-gated row above skips (exit 77) wherever
# PINEFORGE_REQUIRE_ABI_RECEIPTS is OFF and its providers are not prepared in
# this build tree, and CTest counts a skipped row as passed.
# scripts/check_abi_receipt_skips.py names those rows and prepares the
# providers (the recipe in CLAUDE.md and docs/ci.md); this row is its test,
# against the real ctest.
add_test(NAME test_abi_receipt_skips
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_abi_receipt_skips.py
        --ctest ${CMAKE_CTEST_COMMAND})
# Same pairing TU, for the v16/v17 ownership control.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_test(NAME test_aggregate_cpp_versions_runtime
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_aggregate_cpp_versions.py
            --compiler ${CMAKE_CXX_COMPILER}
            --library $<TARGET_FILE:pineforge>
            --include ${PROJECT_SOURCE_DIR}/include
            --generated-include ${PROJECT_BINARY_DIR}/include
            --v16-frozen-receipt ${PINEFORGE_NATIVE_ABI_V16_FROZEN_RECEIPT}
            --receipt ${PROJECT_BINARY_DIR}/aggregate-abi-receipt.json
            ${_pf_abi_receipt_mode}
            ${_pf_script_cpp_abi_flags})
    set_tests_properties(test_aggregate_cpp_versions_runtime PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()

add_test(
    NAME test_derive_corpus_feeds
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/derive_corpus_feeds_self_test.py
)

add_test(
    NAME test_fingerprint
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/fingerprint_self_test.py
)

add_test(
    NAME test_run_corpus
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/run_corpus_self_test.py
)
add_test(
    NAME test_regen_corpus_cpp_pin
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_regen_corpus_cpp_pin.py
)

add_test(
    NAME test_verify_corpus_metrics
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_verify_corpus_metrics.py
)

add_test(
    NAME test_verify_corpus_open_mark
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_verify_corpus_open_mark.py
)

add_test(
    NAME test_run_strategy_range_end
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_range_end.py
)

add_test(
    NAME test_run_strategy_identity
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_identity.py
)

add_test(
    NAME test_run_strategy_aux_feed
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_aux_feed.py
)

add_test(
    NAME test_run_strategy_emit_window
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_emit_window.py
)

add_test(
    NAME test_run_strategy_dump_book
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_dump_book.py
)

# The live-flags lane's own pure bookkeeping (strip_open_at_end,
# interior_diff, hash_diff_reason, fold_reasons) against synthetic
# rows/hash arrays -- complements test_live_flags_lane_positive's engine-
# level pin by proving the SCRIPT's comparison logic, not just the engine
# mechanism it measures.
add_test(
    NAME live_flags_lane_selftest
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/live_flags_lane.py --self-test
)

# The frozen public pending-row POD must retain its exact immutable layout;
# values are projected through PendingIntentView and verified here and in CI.
add_test(
    NAME test_pending_order_mirror_check
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/gen_pending_order_mirror.py --check
)
add_test(
    NAME test_pending_order_v1_full_prefix
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/check_pending_order_prefix.py
)
add_test(
    NAME test_pending_order_v1_compiler_prefix
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/check_pending_order_prefix_compiler.py
)
add_test(
    NAME test_pending_intent_view_schema
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_pending_intent_view.py
)
add_test(
    NAME test_pending_intent_view_mutations
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_pending_intent_view_mutations.py
)

# Spec §3.4's reflection gate: every hashed-region struct member must be
# either hashed by engine_state_hash.cpp or explicitly waived. Previously
# CI-only (.github/workflows/ci.yml); registering it here means a local
# `ctest` run (the plan's stated workflow) also catches an unhashed member.
add_test(
    NAME test_broker_state_hash_coverage
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/check_broker_state_hash_coverage.py
)
add_test(
    NAME test_broker_state_hash_coverage_mutations
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_broker_state_hash_coverage.py
)

# When PINEFORGE_ENABLE_COVERAGE is ON we also instrument the test
# binaries — header-only code (Series, na, color, log, math::pine_random)
# is otherwise reported as 0% because it's only inlined into test TUs.
if(PINEFORGE_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_pf_cov_test_flags -fprofile-instr-generate -fcoverage-mapping)
    else()
        set(_pf_cov_test_flags --coverage -fprofile-arcs -ftest-coverage)
    endif()
endif()

file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/test_native_market_vertical.cpp"
     PINEFORGE_NATIVE_MARKET_VERTICAL_SHA256)

foreach(test_name ${TEST_SOURCES})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE pineforge)
    # Match the engine's FMA-disabled FP semantics so bit-exact reference
    # values in tests like test_ta_rma_warmup don't drift due to
    # contracted multiply-adds emitted in the test TU itself.
    target_compile_options(${test_name} PRIVATE -ffp-contract=off)
    # Tests are assert()-based: strip NDEBUG (appended after the build-type
    # flags, so it wins over Release's -DNDEBUG) or every assertion in the
    # suite compiles to a no-op and ctest passes vacuously.
    target_compile_options(${test_name} PRIVATE -UNDEBUG)
    if(PINEFORGE_ENABLE_COVERAGE)
        target_compile_options(${test_name} PRIVATE ${_pf_cov_test_flags})
    endif()
    if(NOT test_name STREQUAL "test_l4g_runtime_budget")
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()

pineforge_add_twin_test(test_l8c_short_seed_p0_10 test_l8c_short_seed_guards p0-10)
pineforge_add_twin_test(test_l8c_short_seed_p1_16 test_l8c_short_seed_guards p1-16)
pineforge_add_twin_test(test_l8c_short_seed_p1_18 test_l8c_short_seed_guards p1-18)
pineforge_add_twin_test(test_l8c_ordering_p0_11 test_l8c_adapter_ordering p0-11)
pineforge_add_twin_test(test_l8c_ordering_p1_13 test_l8c_adapter_ordering p1-13)
pineforge_add_twin_test(test_l8c_ordering_p1_14 test_l8c_adapter_ordering p1-14)
pineforge_add_twin_test(test_l8c_kernel_p1_21 test_l8c_kernel_delta_rulings p1-21)
pineforge_add_twin_test(test_l8c_kernel_p1_22 test_l8c_kernel_delta_rulings p1-22)
pineforge_add_twin_test(test_l8c_kernel_p1_24 test_l8c_kernel_delta_rulings p1-24)

# R4-D L8a: each Fable-delta finding remains an independently addressable
# native-route witness while sharing one compile unit for the common host
# fixtures.  The aggregate executable is also retained by TEST_SOURCES.
foreach(l8a_case
        drawdown_once
        risk_latch_scope
        cap_prefill
        cap_stream_phase
        direction_gate
        explicit_percent_sizing
        margin_call_latch
        affordability_surplus
        cap_residuals
        product_day_key)
    pineforge_add_twin_test(test_l8a_${l8a_case} test_l8a_adapter_policy_delta ${l8a_case})
endforeach()

# The runtime-budget candidate is a Pine-bound TU: no target, no row.
if(TARGET test_l4g_runtime_budget)
    target_compile_definitions(test_l4g_runtime_budget PRIVATE
        PINEFORGE_L4G_TUTORIAL_CSV="${PROJECT_SOURCE_DIR}/tutorial/data/btcusdt_15m_7d.csv")
    set(_pf_runtime_budget_mode)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        set(_pf_runtime_budget_mode --candidate-only)
    elseif("$ENV{PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY}" STREQUAL "1")
        # The escape for a CI lane whose host cannot time the replay. A40 rev 6
        # set it for hosted macOS while the gated sample was wall clock (the
        # same tree measured 12.8x and 17.2x best-of-five an hour apart); since
        # A40 rev 7 the sample is process CPU time and the CI workflow sets it
        # to 0 on every lane, so the relative gate runs on every Release build.
        set(_pf_runtime_budget_mode --candidate-only)
    endif()
    add_test(NAME test_l4g_runtime_budget
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_runtime_budget.py
            --candidate $<TARGET_FILE:test_l4g_runtime_budget>
            --source ${CMAKE_CURRENT_SOURCE_DIR}/test_l4g_runtime_budget.cpp
            --csv ${PROJECT_SOURCE_DIR}/tutorial/data/btcusdt_15m_7d.csv
            --compiler ${CMAKE_CXX_COMPILER}
            --generated-include ${PROJECT_BINARY_DIR}/include
            --compile-commands ${PROJECT_BINARY_DIR}/compile_commands.json
            --v16-frozen-receipt ${PINEFORGE_NATIVE_ABI_V16_FROZEN_RECEIPT}
            ${_pf_abi_receipt_mode}
            ${_pf_runtime_budget_mode})
    set_tests_properties(test_l4g_runtime_budget PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()

add_test(NAME test_l4g_runtime_budget_mutations
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_runtime_budget.py)


# Contract §3.4 / deferred-ANY pin witnesses retain their historical CTest
# names while their executable twins make the switched route explicit.
pineforge_add_twin_test(test_live_order_derived test_native_live_order_derived_l4d)
pineforge_add_twin_test(test_pending_order_v1_canary
                        test_native_pending_order_v1_canary_l4d)
pineforge_add_twin_test(test_source_fifo_endpoints
                        test_native_source_fifo_endpoints_l4d)

# These restored twins already execute green through the switched route. Keep
# their CTest rows in the required profile rather than hiding green evidence
# behind the pending policy label.
pineforge_clear_test_labels(
test_native_pending_order_v1_canary_l4d
    test_native_session_key_derivation_l4d
    test_native_live_state_hash_l4d
    test_metrics_l4d
    test_native_daily_holiday_l4d
    test_native_wm_buckets_l4d
    test_live_state_hash_l4d
    test_market_admission_matrix_l4d
    test_market_admission_state_l4d
    test_pending_order_core_l4d
    test_pending_order_identity_l4d
    test_pending_order_v1_canary_l4d
    test_pending_placement_receipts_l4d
    test_pending_quantity_intent_l4d
    test_placement_facts_l4d
    test_close_id_retires_ledger_l4d
    test_frozen_market_instruction_l4d
    test_pine_transaction_settlement_l4d
    test_root_cancel_sole_stop_l4d
    test_run_inputs_overrides_l4d
    test_settlement_observation_boundary_l4d
    test_stop_decline_continue_path_l4d
    test_taro_mc_close_residue_l4d)

# R4-D L0 carriers are included verbatim by their switched-route twin targets.
# There is deliberately no ORACLE_TEST_SOURCES list: a source list that does
# not build targets is not executable oracle coverage.
pineforge_add_twin_test(test_oracle_deferred_birth test_native_oracle_deferred_birth_l2)
pineforge_add_twin_test(test_oracle_relative_exit test_native_oracle_relative_exit_l2)
pineforge_add_twin_test(test_oracle_reversal_close_only
                        test_native_oracle_reversal_close_only_l2)
pineforge_add_twin_test(test_oracle_reversal_same_bar_tx
                        test_native_oracle_reversal_same_bar_tx_l2)
pineforge_add_twin_test(test_oracle_reversal_replaced_percent
                        test_native_oracle_reversal_replaced_percent_l2)
pineforge_add_twin_test(test_oracle_reversal_later_tick
                        test_native_oracle_reversal_later_tick_l2)
pineforge_add_twin_test(test_oracle_short_seed_percent
                        test_native_oracle_short_seed_percent_full_l2)
pineforge_add_twin_test(test_oracle_fx test_native_oracle_fx_l2)
pineforge_add_twin_test(test_oracle_coof test_native_oracle_coof_l2)
pineforge_add_twin_test(test_oracle_pooc_freeze test_native_oracle_pooc_freeze_l2)
pineforge_add_twin_test(test_oracle_pooc_immediate test_native_oracle_pooc_immediate_l2)
pineforge_add_twin_test(test_oracle_magnifier_distribution
                        test_native_oracle_magnifier_distribution_l2)
pineforge_add_twin_test(test_oracle_day_key test_native_oracle_day_key_l2)
pineforge_add_twin_test(test_oracle_frozen_size test_native_oracle_frozen_size_full_l2)
pineforge_add_twin_test(test_oracle_deferred_any_witnesses
                        test_native_oracle_deferred_any_witnesses_l2)
pineforge_add_twin_test(test_oracle_magnifier_barstate
                        test_native_oracle_magnifier_barstate_l2)
pineforge_add_twin_test(test_oracle_more_than_64_fills
                        test_native_oracle_more_than_64_fills_l2)

# A20 fixture-facade projections preserve the original oracle literals while
# replacing retired owner reads with the adapter's read-only view.
pineforge_add_twin_test(test_oracle_short_seed test_native_oracle_short_seed_full_l2)
pineforge_add_twin_test(test_oracle_stop_snapshot
                        test_native_oracle_stop_snapshot_full_l2)

# R4-D L5c/L6c: these assigned twins now reproduce their legacy source-policy
# literals on the switched native route. L6c also completed the path-order,
# cancellation, integration-close, and explicit off-grid reduction rows.
pineforge_clear_test_labels(
test_aapl15_margin_brackets_l4a
    test_carried_pooc_short_margin_state_l4a
    test_integer_short_margin_state_l4a
    test_intraday_cap_auto_close_l4a
    test_margin_call_intrabar_chronology_l4a
    test_margin_call_trail_exit_chronology_l4a
    test_pine_cap_boundary_l4a
    test_risk_max_intraday_loss_tv_l4a
    test_short_margin_script_state_l4a
    test_stop_open_margin_script_state_l4a
    test_tv_money_long_margin_call_eth_l4a
    test_native_oracle_stop_snapshot_full_l2
    test_native_live_order_derived_l4d
    test_live_order_derived
    test_live_path_order_l4d
    test_pending_order_mirror_no_alloc_l4d
    test_full_close_while_pyramiding_l4d
    test_pooc_short_close_tick_l4d
    test_pyramiding_count_partial_drain_l4d
    test_live_order_derived_l4d
    test_cancellation_mirror_coverage_l4d
    test_fills_edge_l4d
    test_integration_l4d
    test_strategy_commands_extra_l4d
    test_strategy_pyramiding_l4d
    test_dual_entry_placement_sizing_l4b
    test_integer_lot_percent_exit_min_step_l4b
    test_market_entry_affordability_l4b
    test_qty_step_epsilon_floor_l4b
    test_stop_entry_placement_open_qty_l4b
    test_oracle_stop_snapshot)
add_test(NAME test_oracle_twin_census
         COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_oracle_twin_census.py)
add_test(NAME test_oracle_sha256_pin
         COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_oracle_sha256.py)

target_compile_definitions(test_native_market_vertical PRIVATE
    PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256="${PINEFORGE_NATIVE_MARKET_VERTICAL_SHA256}")

# R5 L3 adapter-neutrality witness: the sizing-bases test reads the whole
# source layer and proves no Pine translation unit names the new value kinds.
# '|' separates the paths because ';' would split the compile definition.
file(GLOB_RECURSE PINEFORGE_SOURCE_LAYER_FILES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/source/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/source/*.hpp"
     "${PROJECT_SOURCE_DIR}/src/compat/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/compat/*.hpp"
     "${PROJECT_SOURCE_DIR}/include/pineforge/source/*.hpp"
     "${PROJECT_SOURCE_DIR}/include/pineforge/compat/*.hpp")
list(LENGTH PINEFORGE_SOURCE_LAYER_FILES PINEFORGE_SOURCE_LAYER_COUNT)
if(PINEFORGE_SOURCE_LAYER_COUNT LESS 4)
    message(FATAL_ERROR "source-layer neutrality glob found no translation units")
endif()
string(REPLACE ";" "|" PINEFORGE_SOURCE_LAYER_JOINED "${PINEFORGE_SOURCE_LAYER_FILES}")
target_compile_definitions(test_native_sizing_bases PRIVATE
    PINEFORGE_SOURCE_LAYER_FILES="${PINEFORGE_SOURCE_LAYER_JOINED}")

# R5 E9 reads its TradingView tapes from the fixture tree; source-bound too.
if(TARGET test_trail_activation_tick_reach)
    target_compile_definitions(test_trail_activation_tick_reach PRIVATE
        PINEFORGE_E9_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trail_activation_tick_reach")
endif()

# R5 R2's witness is the mirror image: the adapter DOES name the sizing basis
# it was re-lowered onto, and the core files that own the basis name no source
# language at all.
string(REPLACE ";" "|" PINEFORGE_R2_CORE_JOINED
       "${PROJECT_SOURCE_DIR}/include/pineforge/native_order.hpp;${PROJECT_SOURCE_DIR}/src/native_execution_consumer.cpp")
# The witness is adapter-bound, so the kernel-only build never registers it.
if(TARGET test_adapter_sizing_relower)
    target_compile_definitions(test_adapter_sizing_relower PRIVATE
        PINEFORGE_R2_ADAPTER_FILE="${PROJECT_SOURCE_DIR}/src/source/pine_adapter.cpp"
        PINEFORGE_R2_CORE_FILES="${PINEFORGE_R2_CORE_JOINED}")
endif()

# R5 F7: the margin witness pins that no never-called predicate survives.
if(TARGET test_adapter_margin_relower)
    target_compile_definitions(test_adapter_margin_relower PRIVATE
        PINEFORGE_F7_ADAPTER_FILE="${PROJECT_SOURCE_DIR}/src/source/pine_adapter.cpp")
endif()

# R5 F7 (M25): the path-order witness reads the two source files it pins.
if(TARGET test_e19_excursion_path_order)
    target_compile_definitions(test_e19_excursion_path_order PRIVATE
        PINEFORGE_F7_ADAPTER_FILE="${PROJECT_SOURCE_DIR}/src/source/pine_adapter.cpp"
        PINEFORGE_F7_PATH_RESOLVE_FILE="${PROJECT_SOURCE_DIR}/src/source/pine_path_resolve.cpp")
endif()

# R5 E5 reads its TradingView tapes from the fixture tree; source-bound too.
if(TARGET test_offset_trail_quantized_arm)
    target_compile_definitions(test_offset_trail_quantized_arm PRIVATE
        PINEFORGE_E5_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/offset_trail_arm")
endif()

# R5 E21 reads the kernel's own sources to prove BacktestEngine::marked_equity_at
# is the one implementation of the marked equity. Kernel files only, so the
# kernel-only profile runs the row too.
file(GLOB PINEFORGE_E21_KERNEL_FILES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/*.hpp"
     "${PROJECT_SOURCE_DIR}/include/pineforge/*.hpp")
string(REPLACE ";" "|" PINEFORGE_E21_KERNEL_JOINED "${PINEFORGE_E21_KERNEL_FILES}")
if(TARGET test_native_margin_fx_clock)
    target_compile_definitions(test_native_margin_fx_clock PRIVATE
        PINEFORGE_E21_KERNEL_FILES="${PINEFORGE_E21_KERNEL_JOINED}")
endif()

# R5 F3 reads the kernel's own code (headers included) to prove it carries no
# source-adapter order-id sentinel. Kernel files only, like E21's set.
file(GLOB PINEFORGE_F3_KERNEL_FILES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/*.hpp"
     "${PROJECT_SOURCE_DIR}/include/pineforge/*.hpp"
     "${PROJECT_SOURCE_DIR}/include/pineforge/*.h")
string(REPLACE ";" "|" PINEFORGE_F3_KERNEL_JOINED "${PINEFORGE_F3_KERNEL_FILES}")
if(TARGET test_native_bare_host_contracts)
    target_compile_definitions(test_native_bare_host_contracts PRIVATE
        PINEFORGE_F3_KERNEL_FILES="${PINEFORGE_F3_KERNEL_JOINED}")
endif()

# R5 E25 reads its TradingView tapes from the fixture tree; source-bound too.
if(TARGET test_session_islastbar_aggregation)
    target_compile_definitions(test_session_islastbar_aggregation PRIVATE
        PINEFORGE_E25_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_islastbar")
endif()

# R5 E27 replays the same tape tree for the POOC fill stamp; source-bound too.
if(TARGET test_pooc_fill_stamp_aggregation)
    target_compile_definitions(test_pooc_fill_stamp_aggregation PRIVATE
        PINEFORGE_E27_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_islastbar")
endif()

# R5 F5 replays the same tapes through a bare host; kernel-only, so this row
# exists in every profile.
if(TARGET test_native_session_day_facts)
    target_compile_definitions(test_native_session_day_facts PRIVATE
        PINEFORGE_F5_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_islastbar")
endif()
if(TARGET test_session_day_facts_adapter)
    target_compile_definitions(test_session_day_facts_adapter PRIVATE
        PINEFORGE_F5_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_islastbar")
endif()

# std::thread needs an explicit pthread link on some toolchains.
target_link_libraries(test_timezone_concurrency PRIVATE Threads::Threads)

# Pure-C sanity test for <pineforge/pineforge.h>. Compiled with the C
# compiler so any C++-only syntax leaking into the public header
# triggers a hard build failure. Linked with CXX so it can pull in
# libstdc++ / libc++ symbols transitively through libpineforge.a.
add_executable(test_c_abi test_c_abi.c)
set_target_properties(test_c_abi PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(test_c_abi PRIVATE pineforge)
target_compile_options(test_c_abi PRIVATE -UNDEBUG)
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_c_abi PRIVATE ${_pf_cov_test_flags})
endif()
pineforge_add_twin_test(test_c_abi test_c_abi)

# R5 lane F6: the P2c historical spellings -- pf_equity_stats_t::sharpe_tv /
# sortino_tv and exit_legs::Domain::Coof / MagnifierCoof -- are
# compiler-deprecated aliases (ADR-0001 "Deprecated public spellings"). These
# TUs name them on purpose: test_c_abi exercises the C alias, and the two
# twins keep their frozen base CHECK texts verbatim. They build without the
# diagnostic; test_deprecated_public_spellings holds that every other use
# draws it, that the generic spellings compile clean, and that the aliases
# still compile.
foreach(_pf_deprecated_spelling_test
        test_c_abi test_metrics_l4d test_exit_lifecycle_clock_l4c)
    if(TARGET ${_pf_deprecated_spelling_test})
        target_compile_options(${_pf_deprecated_spelling_test}
            PRIVATE -Wno-deprecated-declarations)
    endif()
endforeach()
add_test(NAME test_deprecated_public_spellings
    COMMAND ${CMAKE_COMMAND}
        -DPF_CC=${CMAKE_C_COMPILER}
        -DPF_CXX=${CMAKE_CXX_COMPILER}
        "-DPF_INCLUDES=${PROJECT_SOURCE_DIR}/include|${PROJECT_BINARY_DIR}/include"
        -DPF_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DPF_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/deprecated_spellings
        -P ${CMAKE_CURRENT_SOURCE_DIR}/deprecated_spellings.cmake)

# R5 L13: the C-level native host API. One ctest row, two translation units.
# test_native_c_api.c is compiled by the C compiler, so any C++ syntax leaking
# into <pineforge/native_c_api.h> is a hard build failure and the "pure-C
# consumer" claim is executed rather than asserted; the .cpp half drives
# NativeStrategyHost directly and compares the two runs field by field.
# Neither TU reaches a source or compat header, so both survive the
# kernel-only profile.
add_executable(test_native_c_api
    test_native_c_api_twin.cpp
    test_native_c_api.c)
target_link_libraries(test_native_c_api PRIVATE pineforge)
target_compile_options(test_native_c_api PRIVATE -UNDEBUG -ffp-contract=off)

# The frozen v1 header closure, compiled against the current runtime.
add_executable(test_native_c_api_frozen_header test_native_c_api_frozen_header.cpp)
target_link_libraries(test_native_c_api_frozen_header PRIVATE pineforge)
target_compile_options(test_native_c_api_frozen_header PRIVATE -UNDEBUG -ffp-contract=off)

if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_native_c_api PRIVATE ${_pf_cov_test_flags})
    target_compile_options(test_native_c_api_frozen_header PRIVATE ${_pf_cov_test_flags})
endif()

pineforge_add_twin_test(test_native_c_api test_native_c_api)
pineforge_add_twin_test(test_native_c_api_frozen_header test_native_c_api_frozen_header)

# R5 lane F4: the public C headers as a strict C99 consumer compiles them --
# ADR-0001's P2c portability note. The one C11 construct they hold, the
# anonymous union behind sharpe_tv / sortino_tv, is PF_ANONYMOUS_UNION; with
# extensions off and -pedantic-errors, any other C11-only construct in
# <pineforge/pineforge.h> or <pineforge/native_c_api.h> is a build failure.
# The row executes the alias and one native C run.
add_executable(test_native_c_api_c99 test_native_c_api_c99.c)
set_target_properties(test_native_c_api_c99 PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
target_link_libraries(test_native_c_api_c99 PRIVATE pineforge)
# It reads P2c's historical spellings (sharpe_tv, sortino_tv) on purpose, to
# hold that the aliases share storage, so like test_c_abi it builds without
# their deprecation diagnostic.
target_compile_options(test_native_c_api_c99 PRIVATE -UNDEBUG -pedantic-errors -Wall -Wextra
    -Wno-deprecated-declarations)
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_native_c_api_c99 PRIVATE ${_pf_cov_test_flags})
endif()
pineforge_add_twin_test(test_native_c_api_c99 test_native_c_api_c99)

add_test(NAME test_market_admission_schema COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/scripts/check_market_admission_schema.py)
add_test(NAME test_market_admission_schema_mutations COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/scripts/test_market_admission_schema.py)
add_test(NAME test_cancellation_hash_coverage COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_cancellation_hash_coverage.py)

# R5 N11: no NativeRunSpec field the Pine adapter's project() sets is then
# unconditionally shadowed by one of its own host hooks (audit D2), plus the
# check's mutation self-test. Since lane P7 the check is also a source guard
# of every scripts/ci_verify.py profile, so it fails before configure.
add_test(NAME test_adapter_spec_shadowing COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_adapter_spec_shadowing.py)
add_test(NAME test_adapter_spec_shadowing_mutations COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_adapter_spec_shadowing.py)

# R5 P2: the kernel-only archive names no TradingView vocabulary outside the
# ADR-0001 residual table (the symbol table and the string literals of
# libpineforge_kernel.a, which every profile builds), plus the checker's own
# must-be-able-to-fail self-tests. P2b: --cxx gives the archive-surface cases
# this build's own compiler, so they build their `-g` probe archive instead of
# skipping on a host whose PATH has no c++.
add_test(NAME test_kernel_residuals COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_check_kernel_residuals.py
    --archive $<TARGET_FILE:pineforge_kernel>
    --cxx ${CMAKE_CXX_COMPILER})
# R5 P6: every NativeRunSpec field the Pine adapter's project() never assigns
# has a ruling row in ADR-0001 naming executed native consumers (the price grid
# and the risk limits are native-only by ruling), plus the check's mutation
# self-test. Python guards over the sources, so they register in every profile.
add_test(NAME test_native_feature_rulings COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_native_feature_rulings.py)
add_test(NAME test_native_feature_rulings_mutations COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_native_feature_rulings.py)

# R5 L14-A: the documentation anchor gate's own must-fail suite. Only the
# self-test is a row: the gate itself is a binding ci_preflight stage.
add_test(NAME test_doc_anchors COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_check_doc_anchors.py)
add_test(NAME test_doc_lint COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_check_doc_lint.py)

# R5 F2: the worked migration of docs/pages/pine-to-native.md, executed. The
# extractor copies the page's six C++ blocks out verbatim at build time and
# fails the build when the page stops holding them; the TU compiles them
# against PineForge::kernel, runs them, and checks the page's own claims with
# the expected values computed from the page's Pine block. Its TU reaches no
# source-layer header, so it is a row in every profile, the kernel one too.
set(_pf_p2n_page ${PROJECT_SOURCE_DIR}/docs/pages/pine-to-native.md)
set(_pf_p2n_out ${CMAKE_CURRENT_BINARY_DIR}/pine_to_native_worked)
set(_pf_p2n_generated
    ${_pf_p2n_out}/pine_to_native_worked_page.hpp
    ${_pf_p2n_out}/pine_to_native_worked_spec.inc
    ${_pf_p2n_out}/pine_to_native_worked_members.inc)
add_custom_command(
    OUTPUT ${_pf_p2n_generated}
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/extract_pine_to_native_worked.py
            --page ${_pf_p2n_page} --out-dir ${_pf_p2n_out}
    DEPENDS ${_pf_p2n_page} ${CMAKE_CURRENT_SOURCE_DIR}/extract_pine_to_native_worked.py
    COMMENT "Extracting the worked migration from docs/pages/pine-to-native.md"
    VERBATIM)
add_executable(test_pine_to_native_worked test_pine_to_native_worked.cpp ${_pf_p2n_generated})
target_include_directories(test_pine_to_native_worked PRIVATE ${_pf_p2n_out})
target_link_libraries(test_pine_to_native_worked PRIVATE PineForge::kernel)
target_compile_options(test_pine_to_native_worked PRIVATE -ffp-contract=off -UNDEBUG)
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_pine_to_native_worked PRIVATE ${_pf_cov_test_flags})
endif()
add_test(NAME test_pine_to_native_worked COMMAND test_pine_to_native_worked)

# Independent native vertical contract fixture identity.
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/test_native_market_vertical_contract.cpp"
     PINEFORGE_NATIVE_MARKET_VERTICAL_CONTRACT_SHA256)
target_compile_definitions(test_native_market_vertical_contract PRIVATE
    PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256="${PINEFORGE_NATIVE_MARKET_VERTICAL_CONTRACT_SHA256}")

# Reconfigure source-bound native evidence when either fixture changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_native_market_vertical_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_native_calendar_driver_contract.cpp")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/test_native_calendar_driver_contract.cpp"
     PINEFORGE_NATIVE_CALENDAR_DRIVER_SHA256)
target_compile_definitions(test_native_calendar_driver_contract PRIVATE
    PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256="${PINEFORGE_NATIVE_CALENDAR_DRIVER_SHA256}")
# R4-D L5a: these restored twins now pass on the merged switched route.  Keep
# only the root-visible generic-kernel STOP rows under the interim label.
pineforge_clear_test_labels(
test_bracket_lifecycle_declined_reversal_l4c
    test_dropped_reversal_mc_first_l4c
    test_exit_bracket_position_cycle_lifetime_l4c
    test_prearmed_exit_path_cursor_l4c
    test_rounded_carried_short_trail_l4c
    test_live_flags_lane_positive_l4d
    test_live_realtime_tail_l4d
    test_m_admission_36_l4d
    test_script_run_prepare_l4d
    test_sparse_atr_prev_close_l4d
    test_famag_opening_money_l4b
    test_open_money_before_priced_exit_l4b
    test_pooc_long_money_before_trail_l4b
    test_sizing_basis_mintick_l4b
    test_tv_money_band_l4b
    test_tv_money_carried_pooc_l4b
    test_tv_money_precision_l4b)

# R4-D L5b: these restored sizing, money, admission, deferred-close and
# source-projection twins now pass on the switched native route. Keep the
# required rows in the unfiltered profile without any exclusion label.
pineforge_clear_test_labels(
test_declined_reversal_close_leg_l4c
    test_famae_declined_reversal_trail_gap_l4c
    test_famx_declined_reversal_trail_leg_l4c
    test_multi_tier_exit_precedence_l4c
    test_placement_rejection_bracket_ownership_l4c
    test_pooc_coof_reversal_gross_admission_l4c
    test_prearmed_market_parent_gap_exit_l4c
    test_reversal_admission_float_guard_l4c
    test_native_source_fifo_endpoints_l4d
    test_native_market_admission_projection_l4d
    test_native_oracle_reversal_f8_l4d
    test_live_state_hash_recording_l4d
    test_taro_price_gap_admission_l4d
    test_famag_close_survives_l4d
    test_frozen_flat_gap_reject_l4d
    test_integer_flat_budget_tie_l4d
    test_integer_opening_budget_l4d
    test_level_grid_snap_l4d
    test_limit_exact_touch_level_residue_l4d
    test_limit_fill_slippage_l4d
    test_session_predicates_daily_chart_l4d
    test_tv_fill_rounding_l4d
    test_default_flat_market_gross_admission_l4d
    test_explicit_market_price_admission_l4d
    test_famag_close_first_admission_l4d
    test_high_value_price_admission_l4d
    test_live_position_market_gross_admission_l4d
    test_live_probe_suppress_tail_l4d
    test_live_trade_accessors_l4d
    test_stop_entry_admission_l4d
    test_close_all_coqueued_entry_l4d
    test_dual_stop_transactions_l4d
    test_high_value_signal_cost_l4d
    test_pooc_flat_signal_cost_l4d
    test_default_pct_stop_sizing_l4b
    test_percent_equity_open_entry_fee_l4b
    test_source_fifo_endpoints)

# R4-D L6a: the merged-tree risk, margin, admission, stream-preflight and
# verification-apparatus rows are complete.  Keep the explicit final override
# adjacent to the earlier lane labels so every repaired witness is required.
pineforge_clear_test_labels(
test_bracket_lifecycle_declined_reversal_l4c
    test_ci_verify
    test_direct_short_reversal_affordability_l4b
    test_dropped_reversal_mc_first_l4c
    test_explicit_qty_fill_admission_l4b
    test_famag_opening_money_l4b
    test_high_value_fractional_money_margin_l4a
    test_margin_call_gap_open_l4a
    test_market_admission_commission_l4b
    test_pooc_money_admission_l4b
    test_pooc_open_money_event_l4b
    test_script_run_prepare_l4d
    test_streaming_l4d)

# R4-D L7r: the final residual trail booking row now composes with the
# round-3c adapter policies and is part of every unfiltered profile.
pineforge_clear_test_labels(test_trail_fill_snap_l4c)

# R4-D L6b: the round-3c sizing/ShortSeed composition regressions and the
# remaining COOF source-cadence rows are repaired on the fully merged tree.
pineforge_clear_test_labels(
test_calc_on_order_fills_l4c
    test_coof_market_limit_recross_l4c
    test_coof_open_recalc_context_l4c
    test_direct_short_reversal_affordability_l4b)
# R4-D L6f: the merged-tree sampled-path, tick-price, trail-state,
# request.security and stream-cadence policies now satisfy their restored rows.
pineforge_clear_test_labels(
test_trail_close_restart_no_fold_l4c
    test_cascade_exit_gapjump_l4c
    test_coof_chart_tick_touch_l4c
    test_coof_resting_stop_siblings_l4c
    test_pooc_retained_trail_path_l4c
    test_prearmed_bracket_fill_bar_l4c
    test_trail_ref_entry_bar_extreme_l4c
    test_zero_offset_trail_rides_l4c
    test_magnifier_real_bars_l4b
    test_streaming_l4d)
