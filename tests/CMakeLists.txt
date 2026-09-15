set(TEST_SOURCES
    # R4-D L4a native-route behavioral twins.  These retain the legacy
    # oracle literals while driving the switched host only through its public
    # source commands and projections.
    test_aapl15_margin_brackets_l4a
    test_carried_pooc_short_margin_state_l4a
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
    test_native_host_repairs
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
    test_live_state_hash_recording
    test_live_order_derived
    test_live_trade_accessors
    test_live_flags_off_identity
    test_live_flags_lane_positive
    test_native_l4c_policy_modules
    test_native_l4c_coof_literals
    test_native_l4c_pending_mirror
    test_native_l4c_oracle_reversal_literals
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
)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
add_test(
    NAME test_script_cpp_abi
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_script_cpp_abi.py
            --compiler ${CMAKE_CXX_COMPILER}
            --library $<TARGET_FILE:pineforge>
            --include ${PROJECT_SOURCE_DIR}/include
            --generated-include ${PROJECT_BINARY_DIR}/include
            --v16-frozen-receipt ${PROJECT_BINARY_DIR}/native-abi-v16-frozen/receipt.json
            --receipt ${PROJECT_BINARY_DIR}/script-abi-receipt.json
            ${_pf_script_cpp_abi_flags}
)
set_tests_properties(test_script_cpp_abi PROPERTIES TIMEOUT 300)
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
        ${_pf_script_cpp_abi_flags})
set_tests_properties(test_settlement_cpp_abi PROPERTIES TIMEOUT 600)
add_test(NAME test_settlement_cpp_abi_tooling
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_settlement_cpp_abi.py)
add_test(NAME test_aggregate_cpp_versions_runtime
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_aggregate_cpp_versions.py
        --compiler ${CMAKE_CXX_COMPILER}
        --library $<TARGET_FILE:pineforge>
        --include ${PROJECT_SOURCE_DIR}/include
        --generated-include ${PROJECT_BINARY_DIR}/include
        --v16-frozen-receipt ${PINEFORGE_NATIVE_ABI_V16_FROZEN_RECEIPT}
        --receipt ${PROJECT_BINARY_DIR}/aggregate-abi-receipt.json
        ${_pf_script_cpp_abi_flags})
set_tests_properties(test_aggregate_cpp_versions_runtime PROPERTIES TIMEOUT 300)

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
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# A28(1): these remain executable native-route witnesses, but their named
# rows depend on L4c's exit-leg lifecycle / OCA / retained-parent ordering.
# They are deliberately excluded only from the interim L4a verification set;
# the report enumerates the individual assertions and L4c re-enables them.
set(L4_PENDING_TESTS
    test_aapl15_margin_brackets_l4a
    test_carried_pooc_short_margin_state_l4a
    test_integer_short_margin_state_l4a
    test_intraday_cap_auto_close_l4a
    test_margin_call_intrabar_chronology_l4a
    test_margin_call_trail_exit_chronology_l4a
    test_pine_cap_boundary_l4a
    test_risk_max_intraday_loss_tv_l4a
    test_native_oracle_stop_snapshot_full_l2
    test_short_margin_script_state_l4a
    test_stop_open_margin_script_state_l4a
    test_tv_money_long_margin_call_eth_l4a
)
set_tests_properties(${L4_PENDING_TESTS} PROPERTIES LABELS l4-pending)
# These L4b CHECK-parity twins are registered and compiled on the switched
# route.  Their remaining RED literals are owned by parallel L4a/L4c/L4d/L4f
# policy landings, so the interim L4b verification excludes only this label.
# The L4b report records ownership for every remaining CHECK line.
set(L4B_PENDING_TESTS
    test_direct_short_reversal_affordability_l4b
    test_dual_entry_placement_sizing_l4b
    test_explicit_qty_fill_admission_l4b
    test_famag_opening_money_l4b
    test_integer_lot_percent_exit_min_step_l4b
    test_magnifier_real_bars_l4b
    test_market_admission_commission_l4b
    test_market_entry_affordability_l4b
    test_open_money_before_priced_exit_l4b
    test_percent_equity_open_entry_fee_l4b
    test_pooc_long_money_before_trail_l4b
    test_pooc_money_admission_l4b
    test_pooc_open_money_event_l4b
    test_qty_step_epsilon_floor_l4b
    test_sizing_basis_mintick_l4b
    test_stop_entry_placement_open_qty_l4b
    test_tv_money_band_l4b
    test_tv_money_carried_pooc_l4b
    test_tv_money_precision_l4b
)
set_tests_properties(${L4B_PENDING_TESTS} PROPERTIES LABELS l4-pending)
target_compile_definitions(test_l4g_runtime_budget PRIVATE
    PINEFORGE_L4G_TUTORIAL_CSV="${PROJECT_SOURCE_DIR}/tutorial/data/btcusdt_15m_7d.csv")
set_tests_properties(test_l4g_runtime_budget PROPERTIES TIMEOUT 30)

# R4-D L0 captures literal values from the ab9714be legacy source route in
# dedicated translation units.  They intentionally live outside the ordinary
# `tests/test_*.cpp` inventory so later lowering work cannot silently rewrite
# a characterization test while porting a legacy fixture.
set(ORACLE_TEST_SOURCES
    test_oracle_deferred_birth
    test_oracle_relative_exit
    test_oracle_reversal_close_only
    test_oracle_reversal_same_bar_tx
    test_oracle_reversal_replaced_percent
    test_oracle_reversal_later_tick
    test_oracle_short_seed_percent
    test_oracle_fx
    test_oracle_coof
    test_oracle_pooc_freeze
    test_oracle_pooc_immediate
    test_oracle_magnifier_distribution
    test_oracle_day_key
    test_oracle_frozen_size
    test_oracle_deferred_any_witnesses
    test_oracle_magnifier_barstate
    test_oracle_more_than_64_fills
)
# The switched-route fixture twins include the frozen oracle source unchanged
# and expose only adapter/native projections at its former owner-private seams.
# Every restored L4d twin is compiled and registered immediately.  Its policy
# owner is another active L4 lane, so release/native record the real RED
# diagnostics without allowing unfinished policy work to mask the required
# non-pending suite.  Green twins are removed from this label below as they
# are verified on the switched route.
foreach(test_name ${TEST_SOURCES})
    if(test_name MATCHES "_l4d$")
        set_tests_properties(${test_name} PROPERTIES LABELS l4-pending)
    endif()
endforeach()

# Contract §3.4 / deferred-ANY pin witnesses retain their historical CTest
# names while their executable twins make the switched route explicit.
add_test(NAME test_live_order_derived COMMAND test_native_live_order_derived_l4d)
add_test(NAME test_pending_order_v1_canary COMMAND test_native_pending_order_v1_canary_l4d)
add_test(NAME test_source_fifo_endpoints COMMAND test_native_source_fifo_endpoints_l4d)
set_tests_properties(
    test_native_live_order_derived_l4d
    test_live_order_derived
    test_native_live_pending_order_mirror_l4d
    test_live_pending_order_mirror
    test_native_source_fifo_endpoints_l4d
    test_source_fifo_endpoints
    test_native_market_admission_projection_l4d
    test_native_oracle_reversal_f8_l4d
    PROPERTIES LABELS l4-pending)

# These restored twins already execute green through the switched route. Keep
# their CTest rows in the required profile rather than hiding green evidence
# behind the pending policy label.
set_tests_properties(
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
    test_taro_mc_close_residue_l4d
    PROPERTIES LABELS "")

# R4-D L0 carriers are included verbatim by their switched-route twin targets.
# There is deliberately no ORACLE_TEST_SOURCES list: a source list that does
# not build targets is not executable oracle coverage.
add_test(NAME test_oracle_deferred_birth COMMAND test_native_oracle_deferred_birth_l2)
add_test(NAME test_oracle_relative_exit COMMAND test_native_oracle_relative_exit_l2)
add_test(NAME test_oracle_reversal_close_only COMMAND test_native_oracle_reversal_close_only_l2)
add_test(NAME test_oracle_reversal_same_bar_tx COMMAND test_native_oracle_reversal_same_bar_tx_l2)
add_test(NAME test_oracle_reversal_replaced_percent COMMAND test_native_oracle_reversal_replaced_percent_l2)
add_test(NAME test_oracle_reversal_later_tick COMMAND test_native_oracle_reversal_later_tick_l2)
add_test(NAME test_oracle_short_seed_percent COMMAND test_native_oracle_short_seed_percent_full_l2)
add_test(NAME test_oracle_fx COMMAND test_native_oracle_fx_l2)
add_test(NAME test_oracle_coof COMMAND test_native_oracle_coof_l2)
add_test(NAME test_oracle_pooc_freeze COMMAND test_native_oracle_pooc_freeze_l2)
add_test(NAME test_oracle_pooc_immediate COMMAND test_native_oracle_pooc_immediate_l2)
add_test(NAME test_oracle_magnifier_distribution COMMAND test_native_oracle_magnifier_distribution_l2)
add_test(NAME test_oracle_day_key COMMAND test_native_oracle_day_key_l2)
add_test(NAME test_oracle_frozen_size COMMAND test_native_oracle_frozen_size_full_l2)
add_test(NAME test_oracle_deferred_any_witnesses COMMAND test_native_oracle_deferred_any_witnesses_l2)
add_test(NAME test_oracle_magnifier_barstate COMMAND test_native_oracle_magnifier_barstate_l2)
add_test(NAME test_oracle_more_than_64_fills COMMAND test_native_oracle_more_than_64_fills_l2)

# A20 fixture-facade projections preserve the original oracle literals while
# replacing retired owner reads with the adapter's read-only view.
add_test(NAME test_oracle_short_seed
         COMMAND test_native_oracle_short_seed_full_l2)
add_test(NAME test_oracle_stop_snapshot
         COMMAND test_native_oracle_stop_snapshot_full_l2)
set_tests_properties(test_oracle_stop_snapshot PROPERTIES LABELS l4-pending)
add_test(NAME test_oracle_twin_census
         COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_oracle_twin_census.py)
add_test(NAME test_oracle_sha256_pin
         COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_oracle_sha256.py)

target_compile_definitions(test_native_market_vertical PRIVATE
    PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256="${PINEFORGE_NATIVE_MARKET_VERTICAL_SHA256}")

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
add_test(NAME test_c_abi COMMAND test_c_abi)

add_test(NAME test_market_admission_schema COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/scripts/check_market_admission_schema.py)
add_test(NAME test_market_admission_schema_mutations COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/scripts/test_market_admission_schema.py)
add_test(NAME test_cancellation_hash_coverage COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_cancellation_hash_coverage.py)

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

# The twins remain registered while their independently-owned input facts are
# repaired in the concurrent L4a/L4b/L4e/L4f lanes.  L4c's own adapter
# lowering remains compiled in every one; the landing report names the owner
# and first failing literal for each pending row.
set_tests_properties(
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
    test_exit_bracket_position_cycle_lifetime_l4c
    test_famae_declined_reversal_trail_gap_l4c
    test_famx_declined_reversal_trail_leg_l4c
    test_multi_tier_exit_precedence_l4c
    test_placement_rejection_bracket_ownership_l4c
    test_pooc_coof_reversal_gross_admission_l4c
    test_pooc_retained_trail_path_l4c
    test_prearmed_bracket_fill_bar_l4c
    test_prearmed_exit_path_cursor_l4c
    test_prearmed_market_parent_gap_exit_l4c
    test_reversal_admission_float_guard_l4c
    test_rounded_carried_short_trail_l4c
    test_trail_activation_tick_bar_l4c
    test_trail_fill_snap_l4c
    test_trail_open_arm_subtick_offset_l4c
    test_trail_ref_entry_bar_extreme_l4c
    test_zero_offset_trail_rides_l4c
    PROPERTIES LABELS l4-pending)
