if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_library(native_live_equivalence_observer OBJECT native_live_equivalence_observer.cpp)
    target_link_libraries(native_live_equivalence_observer PRIVATE pineforge)
    target_include_directories(native_live_equivalence_observer PRIVATE ${PROJECT_SOURCE_DIR}/runner)
    set_target_properties(native_live_equivalence_observer PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

set(TEST_SOURCES
    test_value_invalid_relative_risk
    test_value_invalid_relative_risk_source
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
    test_report_terminal_quote
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
    test_native_settlement_row_finite
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
    test_native_calc_plan
    test_native_calc_identity
    test_native_htf_subscriptions_stream
    test_native_auxiliary_feed
    test_native_auxiliary_feed_stream
    test_native_auxiliary_feed_twin
    test_native_instrument_feed
    test_native_instrument_feed_twin
    test_symbol_request_data
    test_symbol_feed_copies
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
    test_session_many_windows
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
    test_aux_security_range_capacity
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
    test_calendar_bucket_closes_once
    test_intraday_grid_late_day_open
    test_native_day_labels
    test_daily_magnifier_tapes
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
    test_drawing_checkpoint
    test_coof_drawing_checkpoint_cost
    test_coof_drawing_rollback_tape
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
list(APPEND TEST_SOURCES test_stream_report_equivalence)

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
list(APPEND TEST_SOURCES test_adapter_fifo_lot_boundary)
list(APPEND TEST_SOURCES test_margin_residual_resolved_fill)

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

# Lane TV-DEFAULTS: TradingView's Pine v6 strategy() defaults for an omitted
# capital and order size (tests/fixtures/tv_strategy_defaults), replayed under
# the configuration the generated constructor declares.
list(APPEND TEST_SOURCES test_tv_strategy_defaults)

# Lane TVDEF-DROPS: the three reversal rules a 100%-of-equity default exposed
# (tests/fixtures/tvdef_drops), on TradingView's tapes.
list(APPEND TEST_SOURCES test_tvdef_drops_tapes)

# Lane W3-ENG-EXIT-ALLOC: strategy.exit orders reserve an entry's quantity in
# the order they were created (tests/fixtures/exit_queue), on TradingView's
# tapes.
list(APPEND TEST_SOURCES test_exit_queue_tapes)

# Lane W3B-ENG-GRID: strategy.close(id) under the FIFO rule sizes and books
# from the close ledger (tests/fixtures/grid_close), on TradingView's tapes.
list(APPEND TEST_SOURCES test_grid_close_tapes)

# Lane W5-ENG-MARGIN-V6: margin calls and entry admission at Pine v6's
# defaults (tests/fixtures/margin_v6), on TradingView's tapes.
list(APPEND TEST_SOURCES test_margin_v6_tapes)

# Lane W6-ENG-FILL-ORDER: which order fills first at one fill point -- a flat
# opposite entry pair, a reversing stop against a protective stop
# (tests/fixtures/same_point_entries), on TradingView's tapes.
list(APPEND TEST_SOURCES test_same_point_entries_tapes)

# Fixed, unmargined same-bar entries under pyramiding, with close/entry
# regression tapes and independently recorded out-of-scope controls.
list(APPEND TEST_SOURCES test_samebar_pyramiding_entries_tapes)

# Lane W8C-SECURITY: a finer lookahead_on request.security merges the
# requested bar opening at or before the calling bar's open
# (tests/fixtures/ltf_lookahead_calling_open), on TradingView's tapes.
list(APPEND TEST_SOURCES test_ltf_lookahead_calling_open_tapes)

# Lane TAIL-A: an N-day request.security bar groups the symbol's trading dates
# N at a time from each year's first (tests/fixtures/multi_day_buckets), on
# TradingView's tapes.
list(APPEND TEST_SOURCES test_security_multi_day_buckets_tapes)

# Lane W8D-NOTRADES: an entry a mid-leg calc_on_order_fills recalculation places
# is live from the leg's end (tests/fixtures/coof_refill_waypoint), on
# TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_refill_waypoint_tapes)

# Lane TAIL-C: a refill the recalculation of a fill forced onto a leg's end
# places is live from the next bar's open (second extreme) or rests from the
# second extreme (first extreme) (tests/fixtures/coof_w2_refill), on
# TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_w2_refill_tapes)
# ... and an exit stop the recalculation of a later open fill places already
# through is live from the bar's first extreme
# (tests/fixtures/coof_open_newborn_exit), on TradingView's tape.
list(APPEND TEST_SOURCES test_coof_open_newborn_exit_tape)
# ... and, with the bar magnifier on, an entry the recalculation of a fill
# places is live from the next point of TradingView's intrabar path, the next
# bar's open after the chart bar's last one
# (tests/fixtures/magnified_coof_refill), on TradingView's tape.
list(APPEND TEST_SOURCES test_magnified_coof_refill_tape)

# Lane W8E-EXITS: the KI-62 cover takes only a same-id pyramid add
# (tests/fixtures/ki62_same_id_cover), on TradingView's tapes.
list(APPEND TEST_SOURCES test_ki62_same_id_cover)

# Lane W4-ENG-POOC-SAMEPASS: under process_orders_on_close the close pass
# decides the calculation's exits after its market orders
# (tests/fixtures/pooc_samepass), on TradingView's tapes.
list(APPEND TEST_SOURCES test_pooc_samepass_tapes)
# Lanes W10-DIAG-UNKNOWN (diag-w5r) and TAIL-C: the close pass also fills
# limit/stop adds and reversing limits the close reaches, then decides the
# exits after them (tests/fixtures/pooc_add_close), on TradingView's tape.
list(APPEND TEST_SOURCES test_pooc_add_close_tape)
# ... and a from_entry "" exit without a quantity closes its share of the
# position it fills against, a market add's lot included
# (tests/fixtures/global_exit_fill_size), on TradingView's tapes.
list(APPEND TEST_SOURCES test_global_exit_fill_size_tapes)
# ... and under calc_on_order_fills an exit a later fill's recalculation
# re-issues unchanged keeps resting at its level
# (tests/fixtures/coof_reissued_exit), on TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_reissued_exit_tapes)
# ... and under calc_on_order_fills the exit its entry's fill recalculation
# places is live for the rest of that bar (tests/fixtures/coof_fill_bar_exit),
# on TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_fill_bar_exit_tapes)
# ... and a default-quantity entry sizes on the equity its bar's margin call
# leaves (tests/fixtures/pooc_margin_call_sizing), on TradingView's tapes.
list(APPEND TEST_SOURCES test_pooc_margin_call_sizing_tapes)
# ... and a slipped default percent short opens only while its margin at the
# close fits (tests/fixtures/pooc_short_slip_admission), on TradingView's tapes.
list(APPEND TEST_SOURCES test_pooc_short_slip_admission_tapes)
# Lane TAIL-D: under calc_on_order_fills a close the recalculation of a
# mid-leg fill places, `immediately` or not, fills at the end of that leg,
# where an off-grid extreme is touched at its raw print
# (tests/fixtures/coof_immediate_close), on TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_immediate_close_tapes)
# ... and every exit of one entry the same path point triggers fills there
# before that point recalculates (tests/fixtures/coof_same_point_exits), on
# TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_same_point_exits_tapes)
# ... and what that recalculation cancels or closes is applied at the leg's
# end, so an exit the rest of the leg reaches still fills at its level
# (tests/fixtures/coof_inflight_cancel), on TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_inflight_cancel_tapes)
# ... and a pending entry's bracket the recalculation of its mid-leg fill
# re-issues keeps its stop live at its level on the rest of the leg
# (tests/fixtures/coof_resting_bracket), on TradingView's tapes.
list(APPEND TEST_SOURCES test_coof_resting_bracket_tapes)
# ... and timeframe.change() compares every calculation of a bar a fill
# recalculated with the bar before it (tests/fixtures/coof_timeframe_change),
# on TradingView's tape.
list(APPEND TEST_SOURCES test_coof_timeframe_change_tapes)
# ... and a qty= exit of an entry a reversal holds in the adapter waits for
# that entry, so a FIFO close of its lot by another id's exit leaves its own
# reservation (tests/fixtures/exit_fifo_cross), on TradingView's tapes.
list(APPEND TEST_SOURCES test_exit_fifo_cross_tapes)
# ... and the children of a global strategy.exit one path point triggers fill
# in the order of TradingView's entry-id table, each a transaction settled
# FIFO or against its own entry, and a named stop already through at its
# limit entry's fill fills at that fill (tests/fixtures/global_exit_children),
# on TradingView's tapes.
list(APPEND TEST_SOURCES test_global_exit_children_tapes)
# ... and a strategy.exit binds when it is called: to its entry's open lots,
# else to the entry id while an order of it is working (surviving the flat,
# a cancel and a re-entry), else the call is ignored; an earlier bar's stop
# entry survives a close's flat and an add called at the pyramiding cap is
# never placed (tests/fixtures/exit_binding), on TradingView's tapes.
list(APPEND TEST_SOURCES test_exit_binding_tapes)
# ... and under calc_on_order_fills an exit limit the recalculation of an
# entry's open fill places at or through the open print fills there at once
# (tests/fixtures/coof_open_limit), on TradingView's tape.
list(APPEND TEST_SOURCES test_coof_open_limit_tapes)
# ... and a strategy.exit call that names a level replaces the whole order:
# the price leg it leaves na is withdrawn (tests/fixtures/exit_reissue_na),
# on TradingView's tape.
list(APPEND TEST_SOURCES test_exit_reissue_na_tapes)
# Adapter opening-print and callback projections, independently taped.
list(APPEND TEST_SOURCES
    test_carried_trail_open_tapes
    test_pooc_pending_stop_tapes
    test_coof_marketable_stop_tapes
    test_coof_close_slippage_tapes
    test_coof_forced_waypoint_tapes)
# ... and a flat pair of opposite fixed-quantity market entries whose gross is
# over the equity keeps only its earlier call under process_orders_on_close
# and with costs (tests/fixtures/pooc_pair_gross), on TradingView's tapes.
list(APPEND TEST_SOURCES test_pooc_pair_gross_tapes)
# ... and a default 100 % entry whose whole lots tie the signal's rounded
# equity is dropped flat and keeps only its closing leg as a reversal
# (tests/fixtures/whole_lot_tie), on TradingView's tapes.
list(APPEND TEST_SOURCES test_whole_lot_tie_tapes)
# Source-time money and market-price bands (tests/fixtures/source_buying_power).
list(APPEND TEST_SOURCES test_source_buying_power_tapes)

# Lane W8A-SIGSTATE-1 R4: a fill at the day's first tick does not trip
# strategy.risk.max_intraday_loss (tests/fixtures/intraday_loss_open_fill), on
# TradingView's tape.
list(APPEND TEST_SOURCES test_intraday_loss_open_fill_tape)

# Lane W8A-SIGSTATE-1 R-B and R-A: the pyramiding count of pending entries
# and the order same-side entries fill in at one opening price
# (tests/fixtures/pyramiding_open_order), on TradingView's tape.
list(APPEND TEST_SOURCES test_pyramiding_open_order_tape)

# Lane W8A-SIGSTATE-1 R2: an entry after a same-pass close under
# process_orders_on_close (tests/fixtures/pooc_same_pass_close), on
# TradingView's tape.
list(APPEND TEST_SOURCES test_pooc_same_pass_close_tape)

# Lane W8A-SIGSTATE-1 R3: a default-quantity LIMIT entry opening from flat is
# checked against the calling close at every call
# (tests/fixtures/limit_entry_affordability), on TradingView's tape.
list(APPEND TEST_SOURCES test_limit_entry_affordability_tape)

# Lane R1-CONSOLIDATE (lanes W8A-SIGSTATE-1 and W8B-SIGSTATE-2): a same-bar
# reissue of a reversing MARKET entry moves only its own quantity
# (tests/fixtures/same_bar_reissue), on TradingView's tapes.
list(APPEND TEST_SOURCES test_same_bar_reissue_tapes)

# Lane R1-CONSOLIDATE, rule R-A: at one opening price the MARKET orders fill
# before the buy LIMIT entries the open reached, lowest limit first
# (tests/fixtures/open_fill_order), on TradingView's tapes.
list(APPEND TEST_SOURCES test_open_fill_order_tapes)

# Lane RUN-HARNESS: TradingView computes a deep backtest from its first bar and
# no earlier one, its broker live from there (tests/fixtures/run_harness_window),
# replayed with run_strategy.py's window for a run that starts on that bar.
list(APPEND TEST_SOURCES test_run_harness_window_tapes)

# INT28: two wave-J lanes' rules acting in one run, and at one fill
# (tests/fixtures/int28_compositions), on TradingView's tapes.
list(APPEND TEST_SOURCES test_int28_rule_compositions)
# INT29: lane W10-DIAG-UNKNOWN's COQ and SAMEOPEN-REV beside lane
# W5B-ENG-MARGIN-RESIDUAL's SB and PA at one open
# (tests/fixtures/int29_compositions), on TradingView's tape.
list(APPEND TEST_SOURCES test_int29_rule_compositions)

# Lane INT28-FIX (CA): a strategy.close_all() placed after the same bar's
# all-in reversal entry belongs to that reversal
# (tests/fixtures/reversal_close_all), on TradingView's tapes.
list(APPEND TEST_SOURCES test_reversal_close_all_tapes)
# Lane W5B-ENG-MARGIN-RESIDUAL: the margin-family residuals at Pine v6's
# defaults (tests/fixtures/margin_residual), on TradingView's tapes.
list(APPEND TEST_SOURCES test_margin_residual_tapes)
# Margin-call, admission and sizing rules on 172 TradingView tapes of synthetic
# probes (tests/fixtures/margin_call_rules, README.md there).
list(APPEND TEST_SOURCES test_margin_call_rules_tapes)
# The margin-call schedule rules (MarginScheduleSwitches) on 39 TradingView
# tapes of synthetic controls (tests/fixtures/margin_schedule_rules, README.md
# there), in a backtest and in a forward (stream) run.
list(APPEND TEST_SOURCES test_margin_schedule_rules_tapes)
# The opening-call rules (MarginOpeningSwitches) on 99 TradingView tapes of
# synthetic controls (tests/fixtures/margin_open_rules, README.md there), in a
# backtest and in a forward (stream) run.
list(APPEND TEST_SOURCES test_margin_open_rules_tapes)
# One engine, two modes: the margin rules decide the same trades in a batch run
# and in a forward (stream) run, on a sample of those tapes.
list(APPEND TEST_SOURCES test_margin_rules_forward_replay)
# Margin, sizing and admission rules on 381 TradingView tapes of synthetic
# event-ledger probes (tests/fixtures/margin_ledger_rules, README.md there).
list(APPEND TEST_SOURCES test_margin_ledger_rules_tapes)
# The admission's fill half, the close-first shape, the point order, the exit
# tombstones and the pyramiding records on 73 TradingView tapes of synthetic
# probes (tests/fixtures/admission_rules).
list(APPEND TEST_SOURCES test_admission_rules_tapes)
# The point order's margin call at a gapped open: a call sized at the previous
# close before the stop, and a partial stop that leaves a short remainder.
list(APPEND TEST_SOURCES test_point_order_open_call)
# The carried-trail tick reach's copy of the kernel's trailing-stop level,
# held to native_matching::checked_trail_stop bit for bit.
list(APPEND TEST_SOURCES test_kernel_trail_stop_replica)
# Lane INT28-FIX: a slipped all-in short's admission and its one-unit margin
# calls (tests/fixtures/slipped_short), on TradingView's tapes.
list(APPEND TEST_SOURCES test_slipped_short_tapes)
# Lane W13-ENG-MARGIN-OPP: a margin call at a bar's close after the script,
# and orders sized at placement that cross zero (tests/fixtures/margin_opposite
# and tests/fixtures/slipped_short), on TradingView's tapes.
list(APPEND TEST_SOURCES test_margin_opposite_tapes)
# Lane TAIL-B: several MARKET adds one bar places on a held position, their
# admission and the grown book's margin calls (tests/fixtures/add_sibling_margin),
# on TradingView's tapes.
list(APPEND TEST_SOURCES test_add_sibling_margin_tapes)
# INT30: lane TAIL-B's calls for a bar's adds (AS, AC, LF) beside lane TAIL-H's
# rule PK, which re-reserves the exits a call shrank
# (tests/fixtures/int30_compositions), on TradingView's tapes.
list(APPEND TEST_SOURCES test_int30_rule_compositions)
# Lane TAIL-G: requests of other symbols on a daily chart stamped in its
# session's break read the chart bar's own period (tests/fixtures/
# daily_break_close), on TradingView's tapes.
list(APPEND TEST_SOURCES test_foreign_break_stamp_tapes)
# Lane TAIL-H: a percentage exit's share keeps one quantity step and holds
# what it reserved when margin calls shrink the position
# (tests/fixtures/exit_reservation), on TradingView's tapes.
list(APPEND TEST_SOURCES test_exit_reservation_tapes)
list(APPEND TEST_SOURCES test_percent_entry_reservation_tapes)
list(APPEND TEST_SOURCES test_cross_mechanism_tapes)
# Lane TAIL-C: an explicit-quantity market buy from flat is admitted at its
# signal close's tick plus the slippage ticks its fill carries
# (tests/fixtures/explicit_market_admission), on TradingView's tapes.
list(APPEND TEST_SOURCES test_explicit_market_admission_tapes)
# Lane TAIL-I: an all-in commissioned and slipped explicit short is margin-
# called on its bar's path from the opening print, and under a percent
# commission a call of any size is followed (tests/fixtures/margin_open_print),
# on TradingView's tapes.
list(APPEND TEST_SOURCES test_margin_open_print_tapes)
# ... and an infinite strategy.entry quantity trades the default quantity, as
# na does (tests/fixtures/nonfinite_entry_qty), on TradingView's tape.
list(APPEND TEST_SOURCES test_nonfinite_entry_qty_tape)
# Lane INT28-FIX (RC): a calc_on_order_fills recalculation reads the whole
# bar's finer request.security values (tests/fixtures/coof_recalc_security),
# on TradingView's tape.
list(APPEND TEST_SOURCES test_coof_recalc_security_tapes)
# Lane W10-DIAG-UNKNOWN rule COQ: each MARKET entry filled at an open is judged
# against the position held when it was placed, not a same-open sibling's fill
# (tests/fixtures/coqueued_open_margin), on TradingView's tapes.
list(APPEND TEST_SOURCES test_coqueued_open_margin_tapes)

# Lane W10-DIAG-UNKNOWN rules CAP-ORDER and CAP-ON: TradingView's
# strategy.risk.max_intraday_filled_orders count (tests/fixtures/
# intraday_cap_tv), on TradingView's tapes.
list(APPEND TEST_SOURCES test_intraday_cap_tv_tapes)

# Lane W10-DIAG-UNKNOWN rule GAPSTOP: under process_orders_on_close a default
# percent stop entry the next open gaps through is refused when that open costs
# more than its sizing equity (tests/fixtures/pooc_gap_stop), on TradingView's
# tapes.
list(APPEND TEST_SOURCES test_pooc_gap_stop_tapes)

# Lane W10-DIAG-UNKNOWN rule SAMEOPEN-REV: a default-size entry placed flat or
# on its own side that reverses the position a same-bar entry opened at the
# same open must fit with that position still margined
# (tests/fixtures/same_open_reversal), on TradingView's tapes.
list(APPEND TEST_SOURCES test_same_open_reversal_tapes)
# Lane W8E-EXITS: strategy.netprofit and strategy.equity charge the open lots'
# paid entry fees (tests/fixtures/open_entry_fee_charged), on TradingView's
# tapes.
list(APPEND TEST_SOURCES test_open_entry_fee_charged)

# R5 F3: the contracts the kernel documents for every host, witnessed on bare
# hosts. Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_bare_host_contracts)
# R5 F5: session-day facts on the native decision context, read by a bare
# host from the kernel alone: the E25/E26 TradingView tapes, the audit's
# overnight Tokyo session, streams and a fill recalculation. Source-free, so
# it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_session_day_facts)
# Wave G KERNEL-EDGE: one source-free row with fail-before witnesses for FX
# rolls, session-day neighbours, final calendar-closed buckets and host marks.
list(APPEND TEST_SOURCES test_native_kernel_edge)
# R5 lane PAR-MARGIN (item 3): the margin model on an intrabar path is offered
# NativeMarginCheckKind::IntrabarSample at every delivered sample after the
# bar's first. Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_margin_intrabar_samples)
# R5 lane PAR-MARGIN-2: the post-fill margin check of a request matched on the
# segment into a waypoint measures from that segment's origin, so the waypoint
# the fill was reached on the way to (a limit filled on its way down to the
# bar's low) is in the scan. Source-free, so it also runs in the kernel-only
# profile.
list(APPEND TEST_SOURCES test_native_margin_post_fill_path)
# R5 PERF-KEDGE: on a UTC calendar the session-day facts read an in-session
# instant without resolving its interval -- the zone path's facts at every
# decision point, and the per-bar resolution cost pinned. Source-free, so it
# also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_session_day_utc)
# R5 PERF-ZONED: on a certified cycle of an exchange zone the facts read an
# instant off its session day -- certificates at real transitions, the POSIX
# twin's facts at every decision point, the per-bar cost pinned. Source-free,
# so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_session_day_zoned)
# R5 lane MAG-INTRABAR: a retained lower bar the direct four-sample path
# declines (a zero-length leg, an open at its own low) is still walked through
# its four turning points, so a margin call armed at such an open is booked
# there, and a bar that traded nothing at one price is one point. Source-free,
# so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_intrabar_turning_points)
# Lane W11-ENG-TIME-COLOR: colors as TradingView reads them back -- the named
# constants, color.new / color.rgb over a transparency sweep, every alpha
# byte (tests/fixtures/color_tv). Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_color_tapes)

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
# R5 PERF-P1: the latched continuation at every read that could separate it
# from the live fold, pinned (eager again since V19-A, re-pinned there). On a
# bare host: source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_continuation_view)
# The same through the Pine host's own terminal capture: every capture site
# (chart, magnifier, aggregated, calc_on_order_fills, recording, streams).
# Source-bound.
list(APPEND TEST_SOURCES test_adapter_continuation_view)
# R5 PERF-P23: where a host's write to a projected field is caught -- every
# NativeStrategyHost hook, every projected field -- pinned at the lane's base
# before the projection check was trimmed. Source-free, so it also runs in
# the kernel-only profile.
list(APPEND TEST_SOURCES test_native_projection_witness)
# R5 PERF-P23: the projection check's string equality against std::string's
# own, exhaustively by position and over randomized pairs. Source-free, so it
# also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_projection_compare)
# R5 PERF-P23: the consumer's per-run caches of the callback plumbing against
# the lookups they replace, over randomized hosts, specs and lifecycles.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_callback_caches)
# R5 PERF-P23: the Pine adapter's memoised host view, run solo, interleaved,
# round-robin and rebuilt in place, pinned at the lane's base. Source-bound.
list(APPEND TEST_SOURCES test_adapter_host_view_memo)
# R5 PERF-P4: the kernel's in-place read of its command history is the
# materialised native_events read, row for row and cursor for cursor.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_command_history_read)
# R5 PERF-P4: the Pine adapter's in-place receipt reads, pinned value by value
# on fc7aad62 (receipt cursors, recorded broker-state hashes, trades), and a
# commandless read that allocates nothing. Source-bound.
list(APPEND TEST_SOURCES test_adapter_receipts_in_place)
# R5 PERF-L4: the Pine adapter's quiet-bar gates move no value -- the whole
# battery's observations pinned on f71cd820 -- and a bar with nothing to act
# on allocates nothing in the adapter's hooks. Source-bound. Its ungated and
# probed twins are built from the same TU below the target loop.
list(APPEND TEST_SOURCES test_adapter_quiet_bar)
# R5 PERF-K3: the matcher reuses its candidate rows after an allowance
# refresh. The cost stays linear in the live requests (scaling), the reuse is
# the full rescan bit for bit over randomized books (differential), and the
# values of fixed books are the ones fc7aad62 produced (witness). All three
# are source-free, so they also run in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_match_rescan_scaling)
list(APPEND TEST_SOURCES test_native_match_row_reuse)
list(APPEND TEST_SOURCES test_native_match_hash_witness)
# R5 PERF-K24 (K2): a retained lower feed hands each script bar its sub-bars
# by binary search, so a lower-timeframe run stays linear in its length; the
# values the selection feeds are pinned. Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_intrabar_lower_lookup)
# R5 PERF-K24 (K4): a batch sizes its driver log and command history for its
# own length; the values the pump feeds are pinned. Source-free, so it also
# runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_batch_log_presize)
# R5 V19-A: the v19 continuation folds live state only -- every folded member
# moves it, two histories to one state share it, a Cancelled reason still
# moves it -- and the v19 broker-state hash's running closed-row digest keeps
# kernel-recorded per-bar hashes linear and takes a row once it is final.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_state_continuation)
list(APPEND TEST_SOURCES test_native_tick_volume)
# R5 V19-B: the order core's journal is a window (native_order_v7) -- absolute
# journal positions, the chain index against a journal oracle over randomized
# command streams, chain membership that is not a chain walk (scaling), and a
# live deferred group-adjustment chain pinning the window. Source-free, so it
# also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_journal_window)
# R5 V19-B: what a run keeps of its event record (NativeRunSpec::
# event_retention) and the host acknowledgement that closes a polling host's
# window. Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_event_retention)
# R5 PERF-K1: every calendar-derived value a bare host observes over twelve
# scenarios (intervals, coordinates, session-day facts, series buckets, driver
# rows), pinned on fc7aad62 -- the values the session-day memo must reproduce.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_calendar_hash_witness)
# R5 PERF-K1: the session-day memo (src/native_calendar_memo.hpp) answers
# interval_containing / session_day_at / in_session bit for bit as the
# memo-free calendar does, over walks and random instants across clock
# changes, year ends and session breaks in six zones. Source-free.
list(APPEND TEST_SOURCES test_native_calendar_memo)
# R5 PERF-K1: canonical slot labels no longer resolve the calendar per bar --
# an idle run allocates on the heap within 0.25 per bar of the FeedTolerant
# run (84 per bar before the memo). Source-free.
list(APPEND TEST_SOURCES test_native_calendar_memo_cost)
# R5 lane PERF-P5611 (P5): the monthly-Sharpe walk's UTC month key by integer
# civil-from-days arithmetic, swept against gmtime_r. Source-free, so it also
# runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_utc_month_key_arithmetic)
# R5 lane D2-D: the monthly walk keeps its UTC month key with the month's
# seconds -- the kept key against month_key_utc at every point of fixed-step,
# session-shaped and random-order walks, the kept range against the civil
# month, and compute_equity_stats bit for bit against the 6c081f5d walk.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_utc_month_memo)
# Pure report view: the selected window's equity curve (the window anchor,
# then the M script-bar observations copied byte for byte) as one owned new[]
# array that a report takes by release() and free_report frees, and the
# observation count compute_equity_stats divides exposure by -- hand
# arithmetic for 100 -> 110 -> 104.5, the first bar at T or after a gap, the
# undefined domains, and the ordinary overload bit for bit against the
# reference walk (tests/equity_stats_reference.hpp). Source-free, so it also
# runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_selected_report_view)
# R5 lane PERF-P5611 (P5): the adapter's chart-day key on a UTC chart by the
# same arithmetic, swept against gmtime_r through the host. Source-bound.
list(APPEND TEST_SOURCES test_chart_day_key_arithmetic)
# R5 lane D2-D: the chart-day key keeps its UTC day with the consumer -- the
# key at every bar of stepped, session-shaped, daily, weekly and monthly
# sequences over the K1 zones across DST gaps, overlaps and year ends, every
# civil-day boundary, and day-ledger runs under the intraday loss,
# consecutive-loss-days and filled-orders rules, kept == computed at every
# source callback. Source-bound.
list(APPEND TEST_SOURCES test_chart_day_memo)
# R5 lane D2-D: the host's source publication asks flush_pending_entries' and
# the same-bar exit sort's first questions before calling them -- every
# configuration field written by the script at seeded bars and by the host
# between runs, entries queued behind closes, reversals, batches and fill
# recalculations, and bracket legs filled together, pinned on 6c081f5d.
# Source-bound.
list(APPEND TEST_SOURCES test_publication_witness)
# R5 lane PERF-P5611 (P6): whether a Pine chart aggregates input bars, the
# one definition's equal-literal answer judged against tf_ratio over every
# admitted literal pair. Source-bound.
list(APPEND TEST_SOURCES test_aggregates_input_bars_literals)
# R5 lane PERF-P5611 (P11): the language time built-ins' decomposition --
# UTC by civil arithmetic, other zones from memoised offset intervals --
# against libc field by field, and a memoised zoned read that no longer waits
# for the process timezone lock. Source-free, so both also run in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_local_time_fields)
list(APPEND TEST_SOURCES test_local_time_lock_free_hit)
# R5 lane PERF-L5: the matcher passes over a priced trigger its point's path
# cannot reach before reading the rest of the row, and takes its next winner
# from an ascending run of rows. Both are the full evaluation bit for bit over
# books aimed at the band's edges and K3's randomized books (differential).
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_match_band_precheck)
# R5 lane PERF-L5: the values six books aimed at the band's edges produce,
# pinned on f71cd820 (witness). Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_match_band_witness)
# R5 lane PERF-L5: a warm matcher allocates nothing per driver point -- its
# snapshot, provenance, skip keys and trail handles live in the consumer's
# scratch. Steady books over a window of bars allocate what an idle run does
# (fails on f71cd820). Source-free, so it also runs in the kernel-only
# profile.
list(APPEND TEST_SOURCES test_native_match_allocations)
# R5 lane PERF-L5: the Pine adapter's command path reads the predecessor's
# placement row in place, builds the L4c priority candidates only when a
# selection can use them and carries the source-key node over to a
# successor; four re-issuing scripts' values pinned on f71cd820, and a
# re-issued strategy.exit's allocations bounded (fails on f71cd820).
# Source-bound.
list(APPEND TEST_SOURCES test_adapter_command_allocations)
list(APPEND TEST_SOURCES test_adapter_exit_scratch)
# R5 lane PERF-L1: the NativeStrategyHost reads -- the consumer read off the
# engine's slot, the inline book aggregate, the running lifecycle view --
# against the lookups they replaced, inside every host hook of randomized
# runs. Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_host_reads)
# R5 lane PERF-L1: the exact guards in front of the per-point bodies against
# the bodies themselves over fixed and randomized runs, and every value a host
# reads inside every hook of the fixed runs, pinned on f71cd820 -- the frames
# built in place and the magnifier's scratch buffers move none. Source-free,
# so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_lean_path)
# R5 lane PERF-L1: a synthesized or lower intrabar path delivers its script
# bars without a heap allocation per bar (two to five at f71cd820). Source-free,
# so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_intrabar_scratch)
list(APPEND TEST_SOURCES test_parent_terminal_scratch)
list(APPEND TEST_SOURCES test_same_bar_trade_scratch)
list(APPEND TEST_SOURCES test_global_exit_recut_scratch)
# R5 lane PERF-L1: the interval cache's prior answers and the session-point
# memo hold only what a fresh, memo-free resolution answers, in every host hook
# of randomized zoned, aggregated and streamed runs. Source-free, so it also
# runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_lookup_memos)
# R5 lane PERF-L1: the inline abort and projection checks fail every boundary
# as f71cd820 did (pinned), the array-free margin sizing price is the array
# walk bit for bit, and a frame constructed from its parts is the aggregate
# the callbacks built. Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_point_checks)
# R5 lane TA1: ta::AnchoredVWAP / AnchoredVWAPBands against a naive
# recomputation from the last anchored bar, and against ta::VWAP's session-day
# reset. Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_ta_anchored_vwap)
# R5 lane TA1: ta::ALMA's floor input against TradingView's spelled-out ALMA.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_ta_alma_floor)
# R5 lane TA1: ta::KC's middle band is ta::EMA of its source on every bar and
# its range is ta.tr(false), against TradingView's spelled-out f_kc / f_kcw.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_ta_kc_basis)
# R5 lane TA1: ta::KC / ta::KCW's useTrueRange against f_kc / f_kcw.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_ta_kc_range)
# R5 lane TA1: ta::PivotPointLevels, held and developing, against the pivot
# formulas re-aggregated naively, and against ta::pivot_point_levels.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_ta_pivot_point_levels)
# R5 lane K-COHORT: a cohort close selects each opening on its roster once,
# however many lots that opening's fills left, and books what the same close
# bound without a roster books (BindOpenings over the members, and one
# BindOpening per member). Source-free, so it also runs in the kernel-only
# profile.
list(APPEND TEST_SOURCES test_native_cohort_sliced_close)
# R5 lane PERF-P7: the lookups the lane turns from history walks into
# indexes, as the values four Pine scenarios reach through them -- every
# recorded broker-state row, every read, every trade -- pinned on f71cd820
# before the indexes existed. Source-bound.
list(APPEND TEST_SOURCES test_adapter_lookup_index_witness)
# R5 lane PERF-P7 (P7a), re-targeted by INT20 when V19-B's chain index took
# the place of P7a's definition index: the request core's cohort receipts,
# rosters and membership answers against an oracle folded over the whole
# journal, and a twin whose journal retires at every script-bar boundary equal
# to it, over the randomized books of test_native_match_row_reuse and replace
# chains joined to a cohort at every re-issue. Source-free, so it also runs in
# the kernel-only profile.
list(APPEND TEST_SOURCES test_native_definition_index)
# R5 lane PERF-P7 (P7b): the per-cohort side index behind the applied-reversal
# purge and bracket_belongs_to_reversal, and the closed-row walk over the
# roster positions that carry units, against the whole-roster walks, bit for
# bit, over randomized Pine command streams (differential). Source-bound.
list(APPEND TEST_SOURCES test_adapter_purge_index)
# R5 lane PERF-P7: the consumed-leg index exit() reads (exit legs by origin and
# leg kind, folded over the retained placement table) against the walk of the
# whole exit family, bit for bit, over randomized Pine command streams
# (differential); and the cost of both lookups staying linear in the bars --
# 4x bars must cost < 5x -- for applied reversals and per-origin brackets
# (scaling). Source-bound.
list(APPEND TEST_SOURCES test_adapter_exit_leg_index)
list(APPEND TEST_SOURCES test_adapter_lookup_index_scaling)
# R5 lane PERF-L2: the fused settlement of a book of at most one lot against
# the staged chain, bit for bit -- an admission table with each condition
# flipped, randomized direct books through every fused-capable entry,
# randomized matcher runs (fee forms, FX, margin, magnifier, streams, host-owned
# excursions), K3's and L5's books -- and the counter witness that the common
# case takes the one pass. Source-free, so it also runs in the kernel-only
# profile.
list(APPEND TEST_SOURCES test_native_fused_settlement)
# R5 lane PERF-L2: a common-case fill is inspected, projected, previewed and
# settled without a heap allocation on a warm engine (fails on 6976a808, where
# each call staged into heap vectors). Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_fused_settlement_allocations)
# R5 lane V19-E: the Pine adapter erases the placement row of a request that
# no longer works once nothing can read it. Recording mode over N and 4N bars
# costs < 5x and retains a live-bounded number of rows (scaling); randomized
# Pine command scripts show the same transcript -- every command event, pending
# order, working request, position, trade, equity point and admission field --
# as on the lane's base and as with the erasure switched off (equivalence).
# Source-bound.
list(APPEND TEST_SOURCES test_adapter_live_state_scaling)
list(APPEND TEST_SOURCES test_adapter_live_state_equivalence)
# R5 lane V19-D: a bracket family's roster parks the erased members behind a
# retained one (the v4 fold reads what is retained): randomized append /
# erase / settle / cancel streams against a plain model -- the same members
# in order, the retained ones as the working tail, the model's fold words,
# the same words whenever the settles came. Source-bound.
list(APPEND TEST_SOURCES test_bracket_roster_parking)
# R5 lane L3: the order core's direct forms (WorkingRequestCore::apply_*)
# against the prepare/install pairs they stand for, bit for bit after every
# command -- two bare cores over randomized command streams (answers,
# exceptions, every event and live row, receipts, chain index, counters and
# token staleness), and whole books on the consumer with the direct forms on
# and off (K3's and L2's randomized hosts and a scripted host for anchored
# legs, deferred reductions, retaining replaces, bound openings, the risk
# flatten and current executions, under Window and Full). Source-free, so it
# also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_direct_mutation)
# R5 lane L3: on warm bare books a command allocates its definition and
# nothing else, and the kernel's own work -- fills, their dependency drains,
# the matcher's mutations, the journal's retirement -- nothing but the
# calendar's session-day spans; a Full read of the whole record stays within
# one result vector plus the definitions decoded from spilled journal pages
# (fails on 10f20197, several per fill). Source-free: runs in kernel-only too.
list(APPEND TEST_SOURCES test_native_direct_mutation_allocations)
# R5 lane V19-D: a replace that keeps its handle or carries a close's book
# binding (ReplaceOptions::keep_handle / keep_binding) matches exactly what a
# plain replace matches -- every fill, journal outcome, queue order, answer and
# closed trade, entry incarnations included -- over randomized re-issue scripts
# (reversals before and after the re-price, gaps, OCA siblings, trails,
# brackets, cohorts, ties, fill-callback re-prices) on both core paths; the
# directed rows pin each mechanism, and a continuation read every bar of a run
# that keeps handles stays flat per bar at 4x the bars. Source-free, so it also
# runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_handle_stable_replace)
# R5 lane K-ULP2: a close that spans lots and ends inside one of them --
# Reduce or Transact, two or three lots, a selected scope, execute_current, a
# C host, the settlement's own calls on an opening scope, a seeded battery of
# books sized from cash at tick prices -- is one terminal fill whose closed
# units are exactly the request, whichever way the whole lots and the rest add
# up (it stopped, or left a 2^-51 rest, on db98990c); requests no binary64
# split can carry are still refused. Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_partial_close_split)
# R5 lane K-ULP3: a close whose binary64 running sum reaches its request exactly
# at a lot -- a Reduce or Transact of the book's own sum, of a FIFO prefix of
# it, or of a prefix of a bound selection; execute_current; a C host; the
# settlement's own calls on an opening scope; a seeded battery of books sized
# from cash at tick prices -- closes that lot whole: one terminal fill of the
# request, rows that are the whole lots and add up to it, no dust lot (it kept
# fl(q - r) on 07249e3b). Flatten, a selection's exact consume, a one-lot book
# and the refusals of unrepresentable splits are controls. Source-free, so it
# also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_exact_sum_close)
# R5 lane K-ULP4: a request the settlement cannot book exactly on its book --
# a close split binary64 cannot hold, a reduction the position absorbs, an
# opening beside a dust lot -- ends with a typed
# MatchRejected(UnrepresentableQuantity) and the run goes on (it stopped with
# code 6 / discriminator 5 on 91d65ad6), staged and direct, execute_current
# and its preview, a C host; on a quantity grid a whole-scope fraction and a
# close of one lot's own size are the book's own quantities and settle.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_unrepresentable_refusal)
# R5 lane K-ULP4 (ruling part b): NativeRunSpec::quantity_tolerance -- a close
# within the tolerance of a FIFO boundary ends at it, charged its request; a
# dust lot a close meets at the head of the book closes whole; a dust book
# beside an opening stays; an overshoot opens nothing -- staged and direct,
# fused and staged-only, the spec's validation and digest (unchanged when
# absent, pinned from 91d65ad6), and the C extension's tail. Source-free, so it
# also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_quantity_tolerance)
# R5 lane K-ULP5: an OCA-Reduce deduction binary64 cannot take off its
# recipient -- off its remaining units, into its pending total, at its terms or
# at its owner's fill -- is absorbed and the run goes on (it stopped with code 6
# / discriminator 7 on f1af50dc): K-ULP4's p5b probe, each of the four sites,
# a pending total that rounds into a later fill, the overflow that still stops,
# and a seeded battery of OCA-Reduce groups at ulp scale, staged and direct.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_group_absorption)
# R5 lane K-OCA-KEEP: a re-price that keeps its handle inside a group, then a
# sibling's fill, drains the group in queue order and the run goes on (it
# stopped with code 2 / discriminator 1 on 2a03c658): K-ULP5's keep_handle
# probe under Cancel and Reduce, a retained trail and a carried binding, the
# order core's receipts in queue order and in reverse, and a seeded battery of
# groups re-priced in random subsets and orders under every option set against
# the plain replace, staged and direct; and K-ULP5's finding 2 -- on a quantity
# grid, a whole-scope close whose pending group total its units absorb closes
# the scope whole (a fraction floored and left dust, a host-sized close was
# refused InvalidTerms, on 2a03c658). Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_group_keep_handle)
# R5 lane D2-A (PB): the consumer's O(1) has_command_after against the two
# journal searches the Pine adapter's quiet receipt read compared, for every
# kind of cursor, at every callback of windowed randomized runs
# (differential). Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_command_after)
# R5 lane D2-A (PQ1): a driver point with nothing live matched in front of
# match_path against match_path itself, bit for bit, and the points the
# short-cut took (differential). Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_quiet_point)
# R5 lane D2-A (PQ2): the magnifier sampler handed its leg order and keeping
# no scratch between calls against the base routine, bit for bit, at every
# sample count and distribution, and the declared order the consumer hands it
# (differential). Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_magnifier_ordered_sampler)
# R5 lane D2-A (PQ2): the four-sample ENDPOINTS path computed directly against
# the general and the base routine, bit for bit, the bars it declines, and
# runs with the direct path on and off (differential). Source-free, so it also
# runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_magnifier_endpoints4)
# R5 lane D2-A (PA): a host that declares no bar-open hook against one whose
# hook is empty, C hosts included (differential). Source-free, so it also runs
# in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_bar_open_hook)
# R5 lane D2-A (PC): a host that declares no precommit hook against one whose
# hook admits every view, excursion owners and C hosts included, and the
# settlement previews each run builds (differential). Source-free, so it also
# runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_precommit_hook)
# EN-132: source-free native ownership witnesses and the source companion.
list(APPEND TEST_SOURCES test_value_invalid_sizing_margin)
list(APPEND TEST_SOURCES test_value_invalid_sizing_margin_source)
# R5 lane D2-C: the library's per-thread runtime state (the TA bar context,
# the EMA seeding default, the chart day partition) written on the pump's
# runtime block against thread-local storage at every write -- bare hosts
# whose every callback opens the Pine publication's scopes over K3's books,
# lower-timeframe sub-bars walked -- every value read and every outcome, the
# thread's own state kept as a window, streams interleaved on one thread, a
# nested run and two threads (differential). Source-free, so it also runs in
# the kernel-only profile.
list(APPEND TEST_SOURCES test_native_runtime_ambient)
# R5 lane H-DOCGATES (AUDIT4 N-7): the runtime blocks' LIFO order -- the legal
# order in process, and in a forked child the outer block leaving first, which
# a library with its debug checks compiled in must answer with SIGABRT and any
# other library by returning (PINEFORGE_LIBRARY_DEBUG_CHECKS, below).
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_runtime_ambient_lifo)
# R5 lane D2-C: the Pine host's publications, its suppressed probe tail and
# its request.security evaluations (host-driven, kernel-routed, the
# auxiliary lower-timeframe array) set the chart day partition, the EMA
# seeding default and the TA bar context on the pump's runtime block: every
# value the script and its evaluators read, every trade and the final hashes
# against thread-local storage at every write, line for line (differential).
# Source-bound.
list(APPEND TEST_SOURCES test_adapter_runtime_ambient)
# R5 lane D2-C: the consumer's in-place reads (the run's lifecycle, spec and
# phase, the callback's point, the book, the bound consumer) against
# NativeStrategyHost's accessors at every callback of K3's books and PERF-L2's
# hosts, in the policy hooks, after a current execution and in every
# lifecycle state (differential). Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_in_place_reads)
# R5 lane D2-C: the Pine layer's in-place reads (src/source/pine_host_reads.hpp)
# against the host's accessors in every script bar, request.security
# evaluation and script preparation of the D2-C scenarios, and the running
# spec's fact kept once per run in the adapter's host cache against its
# computation; each scenario with and without the host cache, transcripts
# identical and the kept fact answering (differential). Source-bound.
list(APPEND TEST_SOURCES test_adapter_in_place_reads)
# R5 lane D2-C (stage once): a fill settled from the one-lot stage its
# precommit preview took against the fill staged again -- PERF-L2's hosts
# (partial closes, margin, account FX, excursion owners, streams), K3's books
# with current executions, hooks that rewrite a quote input inside the pump,
# a host that declares no hook -- every value, the settlement path counts and
# the carried count (differential). Source-free, so it also runs in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_settlement_carry)
# R5 lane D2-C (stage once): the Pine host's fills over the D2-C scenarios
# settled from their precommit preview's stage against staged again: every
# script read, trade and hash line for line, and the carried count
# (differential). Source-bound.
list(APPEND TEST_SOURCES test_adapter_settlement_carry)
# R5 lane B-ENGINE (K-ULP1): a Transact that crosses a fractional position --
# one lot or two, explicit, kernel-sized or host-sized, either side, C host
# included -- completes with one lot and one terminal fill of exactly its
# units, whichever way closed + opened rounds (it stopped, or left a 2^-49-unit
# dust lot, on 8cf3be58). Source-free, so it also runs in the kernel-only
# profile.
list(APPEND TEST_SOURCES test_native_crossing_transact)
# R5 lane B-ENGINE: str_tostring and str_format_values render numbers by the
# rule TradingView's tapes pin (the codegen formatter's, lane C6b) -- its
# 42-case edge battery, every numeric string of the C6 exports, text-argument
# MessageFormat, and the mintick mode byte-identical to 8cf3be58. Source-free,
# so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_str_number_format)
# R5 lane B-ADAPTER (item 2, AUDIT3-opus2 H4): the pending-order fill-quantity
# probe's AT_FILL partition answers the quantity a default cash / percent entry
# books at that fill -- fee 0 / 0.1 %, grid none / 0.001, limit and slipped
# stop entries (differential). Source-bound.
list(APPEND TEST_SOURCES test_adapter_fill_qty_probe)
# R5 lane B-ADAPTER (item 1): a typed per-call entry (qty_type cash /
# percent_of_equity) is admitted, booked and probed as the contracts its money
# buys -- the run that declares the same type and value as its default books
# the same contracts, bit for bit (differential). Source-bound.
list(APPEND TEST_SOURCES test_adapter_typed_entry_admission)
# R5 lane B-ADAPTER (item 3, AUDIT3-opus2 H7): the range-end rows convert at
# the equity curve's last point through the producer's explicit rate, and no
# source-layer file writes the presented clock -- a stepped FX curve at seven
# instants around the range end, plain and aggregated charts. Source-bound.
list(APPEND TEST_SOURCES test_adapter_range_end_fx)
# R5 lane B-ADAPTER (item 4, M26): the kernel answers how an input span
# aggregates into script bars before a run (the scheduler's preview), equal to
# the preview the Pine scheduler ran on the K1 zones, across DST, on windowed
# sessions and 1D/1W/1M; plus why the calendar's sealing is not that answer.
# Source-free, so it also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_script_bucket_completions)
# R5 lane B-ADAPTER (item 7, V19D-P1): a margin call revives a dormant exit
# the script left standing and never one it cancelled -- the lab tv tapes
# badapter-v19dp1-control / -cancel replayed on their NYSE:F bars; R5 lane
# V19-FIX adds the tapes v19fix-cancel-before-pair / -at-1000 (an exit
# cancelled while live) and the other cancel spellings. Source-bound.
list(APPEND TEST_SOURCES test_adapter_margin_revival_cancel)
# R5 lane B-ADAPTER (item 5, F3 finding 2): a current execution converts at its
# cursor's rate, threaded through the terms facts, inspection, preview and
# settlement contexts, and presents no clock -- its own hooks and the host after
# it see the frame's; plus the kernel sources that say so. Source-free, so it
# also runs in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_current_execution_rate)
# R5 INT24 (ruling on B-ADAPTER finding 1): strategy_last_bar_dual_entry_path
# names the entry the run fills first -- on a tie bar the kernel walks the low
# leg first (|H - O| < |O - L| is strict), and so does the observation.
# Source-bound.
list(APPEND TEST_SOURCES test_adapter_dual_entry_tie)
# R5 lane H-MEASURE (AUDIT4 X14): every adapter retention AUDIT4 §6 still read
# as "ruled, unmeasured", measured -- the same bars and orders through the Pine
# adapter and through a bare kernel host (a paired differential), and/or
# TradingView's own trades (`lab tv` tapes under tests/fixtures/, each
# directory's README.md names them). All source-bound.
#   G2-09/-10/-12/-13 margin scheduling, market-execution checkpoints, the
#   pre-open slice, opening admission; E20 f1's probe-tail margin call.
list(APPEND TEST_SOURCES test_adapter_margin_schedule_differential)
#   G2-15 pyramiding as a per-cycle count against max_open_lots and the tape.
list(APPEND TEST_SOURCES test_pyramiding_count_differential)
#   G2-18 the sibling Stop beside an explicit-zero trail.
list(APPEND TEST_SOURCES test_zero_trail_sibling_stop)
#   E5's pending-entry arm and E14's anchored-leg seed (AUDIT3 H6 tapes).
list(APPEND TEST_SOURCES test_pending_entry_trail_tapes)
list(APPEND TEST_SOURCES test_pine_input_domain)
list(APPEND TEST_SOURCES test_pine_confirmed_source_state)
#   G2-22 / E19 the host-owned excursion model against the kernel sampler.
list(APPEND TEST_SOURCES test_e19_excursion_tape)
#   G2-21 the short-seed report swaps against the kernel's lot identities.
list(APPEND TEST_SOURCES test_short_seed_report_swap)
#   G2-32 the 1e-10 dust sweep against a bare host on the same fills.
list(APPEND TEST_SOURCES test_pine_dust_sweep_paired)
#   G2-36 session.ismarket: the generated predicate, the kernel's in_session.
list(APPEND TEST_SOURCES test_session_ismarket_tape)
#   K-SESSION-WINDOWS F6: pre- and post-market over every session window.
list(APPEND TEST_SOURCES test_session_prepost_windows_tape)
#   K-SESSION-WINDOWS F1: a script bar's session-day facts at its own label.
list(APPEND TEST_SOURCES test_native_session_day_bar_open)
#   K-SESSION-WINDOWS F2: the chart's session day and the regular one.
list(APPEND TEST_SOURCES test_session_extended_day_tape)
#   TAIL-E: time() / time_close() reading another bar (bars_back,
#   timeframe_bars_back) against TradingView's tapes.
list(APPEND TEST_SOURCES test_time_bars_back_tapes)
#   time(tf, session, tz) across a daylight-saving switch,
#   TradingView's session clock against its tapes
#   (tests/fixtures/session_transition_clock).
list(APPEND TEST_SOURCES test_session_transition_clock_tapes)
#   explicit strategy.entry quantities floored on their
#   shortest decimal, and strategy.equity marked at the tick-built close,
#   against TradingView's tapes (tests/fixtures/explicit_qty_floor).
list(APPEND TEST_SOURCES test_explicit_qty_floor_tapes)
#   under process_orders_on_close the close-time script sees
#   a stop entry and its bracket exit filled in its bar, against TradingView's
#   tapes (tests/fixtures/pooc_close_bar_fills).
list(APPEND TEST_SOURCES test_pooc_close_bar_fills_tapes)
#   TAIL-E: a close the bar's own calculation executes is final for the bar.
list(APPEND TEST_SOURCES test_coof_immediate_close_final_tapes)
# Lane XAU-CAL: TradingView's symbol calendar against its tapes.
list(APPEND TEST_SOURCES test_symbol_calendar_tapes)
#   K-SESSION-WINDOWS F7: an aggregated bar opening inside a session break.
list(APPEND TEST_SOURCES test_session_break_open_bar_tape)
#   F1(e) an aggregated process_orders_on_close script reading entry_bar_index.
list(APPEND TEST_SOURCES test_aggregated_entry_bar_index_tape)
#   F1's magnified residual on the aggregated chart.
list(APPEND TEST_SOURCES test_magnified_aggregated_tape)
#   G2-23 every condition that keeps request.security on the source evaluator.
list(APPEND TEST_SOURCES test_adapter_security_route_conditions)
# R5 lane PAR-ORDERS-2: a current execution refuses a request bound to a host
# roster (BindCohort) instead of failing the run. Source-free, so it also runs
# in the kernel-only profile.
list(APPEND TEST_SOURCES test_native_current_cohort_refusal)
# R5 lane PAR-ORDERS-2: TradingView's own trades (`lab tv` tapes under
# tests/fixtures/, each directory's README.md names them) replayed through the
# Pine adapter. All source-bound.
#   A calc_on_order_fills exit placed while flat, for a pending or a filled parent.
list(APPEND TEST_SOURCES test_flat_coof_exit_tapes)
#   An order a recalculation places on a bar's second extreme.
list(APPEND TEST_SOURCES test_second_extreme_order_tapes)
#   A process_orders_on_close stop entry placed while the book is still open.
list(APPEND TEST_SOURCES test_pooc_reversing_stop_tapes)
# R5 lane PAR-CASHFEE: a percent-of-equity default quantity under a cash
# commission (per order, per contract) against TradingView's own trades -- the
# `lab tv` tapes under tests/fixtures/cash_fee_sizing (README.md there names
# them) replayed on their bars through the Pine adapter. Source-bound.
list(APPEND TEST_SOURCES test_cash_fee_sizing_tapes)
# R5 lane INT26 (round 2): a process_orders_on_close stop re-entry its close
# reached keeps its fresh bracket when the previous bracket's same-bar limit is
# observed at the next open -- the `lab tv` tapes under
# tests/fixtures/pooc_stop_reentry_bracket (README.md there names them) replayed
# through the Pine adapter. Source-bound.
list(APPEND TEST_SOURCES test_pooc_stop_reentry_bracket_tapes)
# R5 K-RUNERR: a plain request.security finer than the finest feed the host
# supplied ("5S" beside a 1m auxiliary slice) reads na on every chart bar
# instead of stopping the run; a chart fed without intrabar bars still
# refuses. Source-bound.
list(APPEND TEST_SOURCES test_security_finer_than_feed)
# R5 lane MAG-INTRABAR: the intrabars TradingView's bar magnifier walks --
# its table's timeframe, the session's grid, each owned by the chart bar of
# its last minute, between the chart bar's own open and close -- as the
# adapter builds them from the host's 1-minute feed. Source-bound.
list(APPEND TEST_SOURCES test_magnifier_tradingview_intrabars)
# ... and on TradingView's tapes (tests/fixtures/magnifier_intrabars): its own
# request.security_lower_tf intrabars per chart timeframe, magnified bracket
# tapes and calc_on_order_fills refill cascades replayed through the adapter.
# Source-bound.
list(APPEND TEST_SOURCES test_adapter_magnifier_intrabar_tapes)
# R5 lane PAR-ORDERS-3: a process_orders_on_close limit entry at the close that
# placed it (TradingView's tapes, tests/fixtures/pooc_limit_close). Source-bound.
list(APPEND TEST_SOURCES test_pooc_limit_close_tapes)
# R5 lane FIX-E1E2: the report's script_tf_ratio and needs_aggregation on
# every run route (E1), and an aggregated chart's admission receipts in their
# command's bar space, on TradingView's tapes
# (tests/fixtures/aggregated_admission_receipt, E2). Source-bound.
list(APPEND TEST_SOURCES test_report_timeframe_ratio)
list(APPEND TEST_SOURCES test_aggregated_admission_receipt_tapes)
# Recorded outputs (docs/outputs.md): a bare host's record read through the
# strategy_outputs_* C exports -- hand values, the row and run-constant rules
# and every refusal (recorder); a stream records what a batch records; a
# batch cut at bar t records bars <= t as the whole batch does; the run
# boundary on a reused handle; and three trading hosts whose trades, equity
# and hashes are equal with recording undeclared, off and on. All source-free,
# so they also run in the kernel-only profile.
list(APPEND TEST_SOURCES test_outputs_recorder)
list(APPEND TEST_SOURCES test_outputs_stream_equivalence)
list(APPEND TEST_SOURCES test_outputs_truncation)
list(APPEND TEST_SOURCES test_outputs_run_reuse)
list(APPEND TEST_SOURCES test_outputs_off_identity)

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

# Selected-window component TUs. test_selected_report_view is registered above
# and is not repeated here. These are ordinary mandatory rows: their sources
# are integrated before configure, and a missing
# source fails configure rather than being skipped. Every TEST_SOURCES row links
# pineforge with the FP and NDEBUG flags; the kernel-only reach filter further
# down decides whether each one runs without the source layer.
list(APPEND TEST_SOURCES
    test_selected_primary_plan
    test_selected_window_plan_c
    test_selected_window_native_counts
    test_selected_retained_count
    test_selected_window_report_integration
    test_selected_window_routes
    test_selected_production_routes
    test_value_admission_arith
    test_value_admission_decl
    test_value_admission_risk
    # EN-132 source command admission: public host, synthetic feed, no fixtures.
    test_value_admission_doors
    test_pine_composite_id_admission
    # The source of test_selected_quote_seal is supplied by a separate change
    # and integrated with this registration before configure.
    test_selected_quote_seal
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

# The private rows whose TU reaches the pineforge/source or
# pineforge/compat/pine layer, RECORDED from the fixture-complete (overlay)
# tree -- the one world where every private TU is present to be walked. The
# predicate below answers BOUND for a row in this list whatever the tree
# holds, so the kernel-only filter and every guarded block read the same
# answer in both worlds: without the record, a fixture-less tree cannot read
# a removed TU (or a header in its closure), answers "not source-bound", and
# re-registers source-layer rows the kernel profile must not build -- the
# fixture-absent kernel run registered 93 rows the overlay run did not (91
# private rows plus the two oracle twins that follow their base). One list,
# read by the one predicate both the filter and the guarded blocks call.
# Public rows keep the include-closure walk below; their TUs are always in
# the tree, so the world cannot change the walk's answer for them.
set(_pf_source_bound_private_rows
    test_aapl15_margin_brackets_l4a
    test_adapter_coof_relower
    test_adapter_fifo_lot_boundary
    test_adapter_in_place_reads
    test_adapter_magnifier_intrabar_tapes
    test_adapter_margin_relower
    test_adapter_margin_revival_cancel
    test_adapter_margin_schedule_differential
    test_adapter_receipts_in_place
    test_adapter_runtime_ambient
    test_adapter_settlement_carry
    test_adapter_sizing_relower
    test_add_sibling_margin_tapes
    test_admission_rules_tapes
    test_aggregated_admission_receipt_tapes
    test_aggregated_entry_bar_index_tape
    test_aggregated_path_regressions
    test_aux_security_feed_l4d
    test_carried_pooc_short_margin_state_l4a
    test_carried_trail_open_tapes
    test_cash_fee_sizing_tapes
    test_chart_tf_security_split_feed
    test_coof_chart_tick_touch_l4c
    test_coof_close_slippage_tapes
    test_coof_drawing_rollback_tape
    test_coof_fill_bar_exit_tapes
    test_coof_forced_waypoint_tapes
    test_coof_immediate_close_final_tapes
    test_coof_immediate_close_tapes
    test_coof_inflight_cancel_tapes
    test_coof_marketable_stop_tapes
    test_coof_open_limit_tapes
    test_coof_open_newborn_exit_tape
    test_coof_open_recalc_context_l4c
    test_coof_recalc_security_tapes
    test_coof_refill_waypoint_tapes
    test_coof_reissued_exit_tapes
    test_coof_resting_bracket_tapes
    test_coof_resting_stop_siblings_l4c
    test_coof_same_point_exits_tapes
    test_coof_timeframe_change_tapes
    test_coof_w2_refill_tapes
    test_coqueued_open_margin_tapes
    test_cross_mechanism_tapes
    test_daily_magnifier_tapes
    test_default_flat_market_gross_admission_l4d
    test_default_pct_stop_sizing_l4b
    test_default_qty_signal_freeze
    test_dropped_reversal_mc_first_l4c
    test_dual_stop_transactions_l4d
    test_e19_excursion_tape
    test_engine_trade_accessors
    test_entry_bar_margin_path_l4a
    test_exit_binding_tapes
    test_exit_fifo_cross_tapes
    test_exit_queue_tapes
    test_exit_reissue_na_tapes
    test_exit_reservation_tapes
    test_explicit_market_admission_tapes
    test_explicit_qty_fill_admission_l4b
    test_explicit_qty_floor_tapes
    test_famae_declined_reversal_trail_gap_l4c
    test_famae_lot_sizing_ten_digit_equity
    test_famag_close_survives_l4d
    test_famx_declined_reversal_trail_leg_l4c
    test_flat_coof_exit_tapes
    test_foreign_break_stamp_tapes
    test_global_exit_children_tapes
    test_global_exit_fill_size_tapes
    test_grid_close_tapes
    test_high_value_signal_cost_l4d
    test_htf_weekly_lookahead
    test_int28_rule_compositions
    test_int29_rule_compositions
    test_int30_rule_compositions
    test_intraday_cap_tv_tapes
    test_intraday_loss_open_fill_tape
    test_kernel_trail_stop_replica
    test_ki62_same_id_cover
    test_l10ab_same_bar_reentry_exit_level
    test_l10ac_full_equity_single_lot
    test_l10ad_tick_bracket_stuck_entry
    test_l10ag_reversal_close_cycle
    test_l10aj_margin_full_exit_flat
    test_l10al_reversal_single_lot
    test_l10am_entry_bar_priced_exit
    test_l10ao_percent_equity_first_lot
    test_l10ap_coof_chained_fill_price
    test_l10ar_priced_entry_fill
    test_l10as_priced_exit_fill
    test_l10b_excursion_parity
    test_l10d_entry_bar_excursion_masks
    test_l10e_opposite_entry_close_same_pass
    test_l10f_sizing_basis_stop_limit
    test_l10g_oca_reduce_deferred_flip
    test_l10h_corpus_parity
    test_l10j_corpus_parity
    test_l10k_corpus_parity
    test_l10l_corpus_parity
    test_l10m_corpus_parity
    test_l10p_three_way_entry_bar_exit
    test_l10q_pyramid_terrace_exit
    test_l10r_dual_stop_same_bar_order
    test_l10recon1_merge_fill_liveness
    test_l10s_pyramiding_same_bar_ids
    test_l10t_partial_exit_residual
    test_l10v_first_partial_leg_timing
    test_l10w_zero_qty_entry
    test_l10x_reissued_limit_touch_bar
    test_l10y_full_equity_entry_split
    test_l10z_review_fixes
    test_l11a_host_excursion_twin
    test_l11b_same_bar_stopout_excursion
    test_l4b_policy_regressions
    test_l4f_security_magnifier
    test_l9a_carried_pooc_competing_checkpoint
    test_limit_entry_affordability_tape
    test_live_trade_accessors_l4d
    test_ltf_lookahead_calling_open_tapes
    test_ltf_lookahead_first_bucket
    test_m_admission_36_l4d
    test_magnified_aggregated_tape
    test_magnified_coof_refill_tape
    test_magnifier_tradingview_intrabars
    test_margin_call_1x_long_entry_fill_l4a
    test_margin_call_rules_tapes
    test_margin_ledger_rules_tapes
    test_margin_open_print_tapes
    test_margin_open_rules_tapes
    test_margin_opposite_tapes
    test_margin_residual_resolved_fill
    test_margin_residual_tapes
    test_margin_rules_forward_replay
    test_margin_schedule_rules_tapes
    test_margin_v6_tapes
    test_market_admission_commission_l4b
    test_native_auto_path
    test_native_calc_timing_twin
    test_native_daily_holiday_l4d
    test_native_margin_hooks_twin
    test_native_oracle_day_key_l2
    test_native_oracle_reversal_same_bar_tx_l2
    test_native_oracle_stop_snapshot_full_l2
    test_native_pending_order_v1_canary_l4d
    test_native_price_grid_twin
    test_native_wm_buckets_l4d
    test_nonfinite_entry_qty_tape
    test_o_close_pct_day_anchor
    test_oanda_lazy_close
    test_offset_trail_quantized_arm
    test_open_entry_fee_charged
    test_open_fill_order_tapes
    test_pending_entry_trail_tapes
    test_pending_order_v1_canary_l4d
    test_percent_entry_reservation_tapes
    test_pine_dust_sweep_paired
    test_point_order_open_call
    test_pooc_add_close_tape
    test_pooc_close_bar_fills_tapes
    test_pooc_fill_stamp_aggregation
    test_pooc_gap_stop_tapes
    test_pooc_limit_close_tapes
    test_pooc_margin_call_sizing_tapes
    test_pooc_open_money_event_l4b
    test_pooc_pair_gross_tapes
    test_pooc_pending_stop_tapes
    test_pooc_reversing_stop_tapes
    test_pooc_same_pass_close_tape
    test_pooc_samepass_tapes
    test_pooc_short_slip_admission_tapes
    test_pooc_stop_reentry_bracket_tapes
    test_publication_witness
    test_pyramiding_count_differential
    test_pyramiding_open_order_tape
    test_reversal_admission_float_guard_l4c
    test_reversal_close_all_tapes
    test_risk_max_intraday_loss_tv_l4a
    test_rounded_carried_short_trail_l4c
    test_run_harness_window_tapes
    test_same_bar_reissue_tapes
    test_same_open_reversal_tapes
    test_same_point_entries_tapes
    test_samebar_pyramiding_entries_tapes
    test_second_extreme_order_tapes
    test_security_multi_day_buckets_tapes
    test_session_break_open_bar_tape
    test_session_day_facts_adapter
    test_session_extended_day_tape
    test_session_islastbar_aggregation
    test_session_ismarket_tape
    test_session_transition_clock_tapes
    test_short_seed_report_swap
    test_slipped_short_tapes
    test_small_money_margin_residual_l4a
    test_source_buying_power_tapes
    test_split_feed_partial_bucket
    test_symbol_calendar_tapes
    test_ta_series_length_extremes
    test_ta_series_length_supertrend
    test_ta_simple_length
    test_taro_price_gap_admission_l4d
    test_time_bars_back_tapes
    test_trail_activation_tick_bar_l4c
    test_trail_activation_tick_reach
    test_trail_close_restart_no_fold_l4c
    test_trail_fill_snap_l4c
    test_trail_ref_entry_bar_extreme_l4c
    test_tv_fill_rounding_l4d
    test_tv_money_band_l4b
    test_tv_money_carried_pooc_l4b
    test_tv_money_long_margin_call_eth_l4a
    test_tv_money_precision_l4b
    test_tv_strategy_defaults
    test_tvdef_drops_tapes
    test_whole_lot_tie_tapes
    test_zero_offset_trail_rides_l4c
    test_zero_trail_sibling_stop
)

# The one source-bound predicate the kernel-only filter below and every
# private-row re-registration block read: resolved once per entry and
# memoised for the rest of configure, so the filter and the guarded blocks
# can never drift into two lists. TRUE when the entry is one of the recorded
# private rows above, else when the entry's include closure (walked live)
# reaches a pineforge/source or pineforge/compat/pine header.
function(pineforge_test_is_source_bound out_var entry)
    get_property(_pf_bound_answer GLOBAL PROPERTY "pf_source_bound::${entry}")
    if(NOT _pf_bound_answer)
        get_filename_component(_pf_bound_row "${entry}" NAME_WE)
        if(_pf_bound_row IN_LIST _pf_source_bound_private_rows)
            set(_pf_bound_answer BOUND)
        else()
            pineforge_test_reaches_source_layer(_pf_reaches "${entry}")
            if(_pf_reaches)
                set(_pf_bound_answer BOUND)
            else()
                set(_pf_bound_answer CLEAR)
            endif()
        endif()
        set_property(GLOBAL PROPERTY "pf_source_bound::${entry}"
            "${_pf_bound_answer}")
    endif()
    if(_pf_bound_answer STREQUAL "BOUND")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
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
        pineforge_test_is_source_bound(_pf_reaches
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
# Compute each slice from real unittest collection at invocation, never from
# a handwritten method list. One parent becomes eight slices plus a cover: +8.
set(PINEFORGE_CI_VERIFY_SLICE_COUNT 8)
set(PINEFORGE_CI_VERIFY_SLICES)
foreach(_pf_ci_verify_slice RANGE 1 ${PINEFORGE_CI_VERIFY_SLICE_COUNT})
    add_test(NAME test_ci_verify_slice_${_pf_ci_verify_slice}
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_verify.py
            --slice ${_pf_ci_verify_slice}/${PINEFORGE_CI_VERIFY_SLICE_COUNT})
    list(APPEND PINEFORGE_CI_VERIFY_SLICES test_ci_verify_slice_${_pf_ci_verify_slice})
endforeach()
add_test(NAME test_ci_verify_slices_cover
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_verify_split.py
        --slice-count ${PINEFORGE_CI_VERIFY_SLICE_COUNT})
add_test(NAME test_ci_preflight_slice_1
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 1/8)
add_test(NAME test_ci_preflight_slice_2
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 2/8)
add_test(NAME test_ci_preflight_slice_3
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 3/8)
add_test(NAME test_ci_preflight_slice_4
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 4/8)
add_test(NAME test_ci_preflight_slice_5
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 5/8)
add_test(NAME test_ci_preflight_slice_6
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 6/8)
add_test(NAME test_ci_preflight_slice_7
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 7/8)
add_test(NAME test_ci_preflight_slice_8
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py --slice 8/8)
add_test(NAME test_ci_preflight_slices_cover
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight_split.py)
# F10's source-only benchmark witnesses require the pinned Git history.
add_test(NAME test_benchmark_harness
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s benchmarks/tests)
add_test(NAME test_benchmark_provenance
    COMMAND ${Python3_EXECUTABLE} benchmarks/check_provenance.py)
set_tests_properties(test_benchmark_harness test_benchmark_provenance PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME test_docs_doxygen_retry
    COMMAND bash ${PROJECT_SOURCE_DIR}/docs/build.sh --self-test-retry)
add_test(NAME test_twin_parity_mutations
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_twin_parity.py)

add_test(
    NAME test_aggregate_cpp_versions
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_aggregate_cpp_versions.py
)

# Selected-window Python unittest files, run as direct scripts under the
# configured Python3 with -B (no bytecode written into the tree). Unconditional:
# no source-layer or opt-in gate.
add_test(NAME test_run_phase_transport
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_run_phase_transport.py)
add_test(NAME test_run_execution_observer
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_run_execution_observer.py)
add_test(NAME test_request_feed_inventory_admission
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_request_feed_inventory_admission.py)
add_test(NAME test_selected_window_plan_python
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_selected_window_plan_python.py)

# Selected-window files that use pytest: Python3 -B -m pytest on the literal
# path, from the repository root. No check that pytest is installed: the test
# environment must provide it, and a missing pytest fails these rows.
add_test(NAME test_run_json_selected_window
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_window.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME test_run_json_nonfinite_report
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_nonfinite_report.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME test_run_json_post_execution
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_post_execution.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME test_compile_inventory_pipeline
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_compile_inventory_pipeline.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# The seven ABI-provider manifests and the four receipt-gated rows. The
# private-fixture PRs take every manifest below out of the public tree, and
# scripts/prepare_settlement_cpp_abi_base.py answers a missing one with the
# pinned SKIP literal and exit 77, so without them no provider can be
# prepared and a row registered as usual would fail its receipt under
# --require-receipts (PINEFORGE_REQUIRE_ABI_RECEIPTS=ON in every ci_verify
# profile) -- scripts/check_abi_receipt_skips.py would then name it a failure
# although its input is simply absent. Each receipt-gated row is therefore
# registered on the same EXISTS guard its providers' manifests gate on:
# present, the row is registered exactly as before; absent, it is the pinned
# SKIP placeholder (the literal, the private_fixtures label, SKIP_RETURN_CODE
# 77 kept), the shape every other private-fixture row in this file uses.
set(_pf_manifest_v18_frozen ${PROJECT_SOURCE_DIR}/tests/fixtures/native_cpp_abi/host-fc7aad6/manifest.json)
set(_pf_manifest_v16_frozen ${PROJECT_SOURCE_DIR}/tests/fixtures/native_cpp_abi/host-ab9714b/manifest.json)
set(_pf_manifest_v15_frozen ${PROJECT_SOURCE_DIR}/tests/fixtures/native_cpp_abi/host-e7cdf05/manifest.json)
set(_pf_manifest_v14 ${PROJECT_SOURCE_DIR}/tests/fixtures/native_cpp_abi/host-f736676/manifest.json)
set(_pf_manifest_v13 ${PROJECT_SOURCE_DIR}/tests/fixtures/native_cpp_abi/host-c3ed455/manifest.json)
set(_pf_manifest_base ${PROJECT_SOURCE_DIR}/tests/fixtures/settlement_cpp_abi/e60e571/manifest.json)
set(_pf_manifest_prior ${PROJECT_SOURCE_DIR}/tests/fixtures/settlement_cpp_abi/0e18690/manifest.json)

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
    if(EXISTS ${_pf_manifest_v18_frozen})
        add_test(
            NAME test_script_cpp_abi
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_script_cpp_abi.py
                    --compiler ${CMAKE_CXX_COMPILER}
                    --library $<TARGET_FILE:pineforge>
                    --include ${PROJECT_SOURCE_DIR}/include
                    --generated-include ${PROJECT_BINARY_DIR}/include
                    --v18-frozen-receipt ${PROJECT_BINARY_DIR}/native-abi-v18-frozen/receipt.json
                    --receipt ${PROJECT_BINARY_DIR}/script-abi-receipt.json
                    ${_pf_abi_receipt_mode}
                    ${_pf_script_cpp_abi_flags}
        )
        set_tests_properties(test_script_cpp_abi PROPERTIES SKIP_RETURN_CODE 77)
    else()
        add_test(NAME test_script_cpp_abi COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/native_cpp_abi/host-fc7aad6/manifest.json")
        set_tests_properties(test_script_cpp_abi PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
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
# The checker reads tests/fixtures/native_cpp_abi/ (the pinned header tars and
# gzip-JSON closures): absent, its gate prints the pinned SKIP literal and
# exits 77, so the row skips here and the private_fixtures label marks it in
# tests/private_fixture_rows.txt; present, the row runs as before.
set_tests_properties(test_native_cpp_abi PROPERTIES SKIP_RETURN_CODE 77 LABELS private_fixtures)

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
set(PINEFORGE_NATIVE_ABI_V18_FROZEN_RECEIPT
    "${PROJECT_BINARY_DIR}/native-abi-v18-frozen/receipt.json"
    CACHE FILEPATH "Prepared actual fc7aad6 frozen epoch 18 ABI provider receipt")
# Same pairing TU, against the frozen settlement providers. It reads every
# provider's receipt, so its guard requires every provider's manifest.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    if(EXISTS ${_pf_manifest_base} AND EXISTS ${_pf_manifest_prior}
            AND EXISTS ${_pf_manifest_v13} AND EXISTS ${_pf_manifest_v14}
            AND EXISTS ${_pf_manifest_v15_frozen} AND EXISTS ${_pf_manifest_v16_frozen}
            AND EXISTS ${_pf_manifest_v18_frozen})
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
                --v18-frozen-receipt ${PINEFORGE_NATIVE_ABI_V18_FROZEN_RECEIPT}
                --receipt ${PROJECT_BINARY_DIR}/settlement-abi-receipt.json
                ${_pf_abi_receipt_mode}
                ${_pf_script_cpp_abi_flags})
        set_tests_properties(test_settlement_cpp_abi PROPERTIES SKIP_RETURN_CODE 77)
    else()
        add_test(NAME test_settlement_cpp_abi COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/settlement_cpp_abi/e60e571/manifest.json")
        set_tests_properties(test_settlement_cpp_abi PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
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
# Same pairing TU, for the v18/v19 ownership control.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    if(EXISTS ${_pf_manifest_v18_frozen})
        add_test(NAME test_aggregate_cpp_versions_runtime
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_aggregate_cpp_versions.py
                --compiler ${CMAKE_CXX_COMPILER}
                --library $<TARGET_FILE:pineforge>
                --include ${PROJECT_SOURCE_DIR}/include
                --generated-include ${PROJECT_BINARY_DIR}/include
                --v18-frozen-receipt ${PINEFORGE_NATIVE_ABI_V18_FROZEN_RECEIPT}
                --receipt ${PROJECT_BINARY_DIR}/aggregate-abi-receipt.json
                ${_pf_abi_receipt_mode}
                ${_pf_script_cpp_abi_flags})
        set_tests_properties(test_aggregate_cpp_versions_runtime PROPERTIES SKIP_RETURN_CODE 77)
    else()
        add_test(NAME test_aggregate_cpp_versions_runtime COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/native_cpp_abi/host-fc7aad6/manifest.json")
        set_tests_properties(test_aggregate_cpp_versions_runtime PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
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

# Lane XSYM-D: the probe's pinned request data (PINEFORGE_REQUESTS_ROOT).
add_test(
    NAME test_run_strategy_requests
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_requests.py
)

add_test(
    NAME test_run_strategy_emit_window
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_emit_window.py
)
set_tests_properties(test_run_strategy_emit_window PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)

add_test(
    NAME test_run_strategy_dump_book
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_dump_book.py
)

add_test(
    NAME test_run_strategy_magnifier
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_magnifier.py
)
set_tests_properties(test_run_strategy_magnifier PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)

add_test(
    NAME test_run_strategy_mincontract
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_mincontract.py
)
set_tests_properties(test_run_strategy_mincontract PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)

# Lane XAU-CAL: TradingView's symbol calendar as a run input (the tape
# converter, the loader and the harness's syminfo-metadata transport).
add_test(
    NAME test_symbol_calendar
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_symbol_calendar.py
)
set_tests_properties(test_symbol_calendar PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)

# L5 control for the four private-fixture probe guards above: it builds both
# worlds itself (fixture absent -> the skip literal and exit 77; fixture
# present -> the guard does not fire), so it needs no fixture and carries no
# label -- it runs on every tree.
add_test(
    NAME test_private_fixture_guards
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_private_fixture_guards.py
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
# Both prefix checkers read the frozen closure under
# tests/fixtures/script_cpp_abi/: absent, each prints the pinned SKIP literal
# naming the missing directory and exits 77, so the rows skip here and carry
# the private_fixtures label (tests/private_fixture_rows.txt); present, both
# run exactly as before.
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
set_tests_properties(test_pending_order_v1_full_prefix
    test_pending_order_v1_compiler_prefix PROPERTIES
    SKIP_RETURN_CODE 77 LABELS private_fixtures)
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

if(PINEFORGE_BUILD_SOURCE_LAYER)
    # PR 1b (private fixtures): each case's frozen generated.cpp is removed by
    # the second private-fixture removal. A case whose file is absent
    # registers no target (a missing add_executable source fails configure),
    # and its two rows register as the pinned SKIP placeholders instead
    # (tests/private_fixture_rows.txt); with every case present the loop is
    # the registration it always was.
    foreach(security_case htf5_close htf15_sma_pooc htf60_close daily_close heikinashi5 htf60_ema_gaps period_previous
            dual_stop pooc_dual_stop pooc_limit_reversal pooc_slipped_short slipped_short all_in_reversal whole_all_in_reversal pooc_short_exit)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stream_security/${security_case}/generated.cpp")
        add_executable(test_stream_security_${security_case}
            test_stream_security_equivalence.cpp
            fixtures/stream_security/${security_case}/generated.cpp)
        target_link_libraries(test_stream_security_${security_case} PRIVATE pineforge)
        target_compile_options(test_stream_security_${security_case} PRIVATE -UNDEBUG -ffp-contract=off)
        target_compile_definitions(test_stream_security_${security_case} PRIVATE
            PINEFORGE_SECURITY_CASE="${security_case}")
        if(PINEFORGE_ENABLE_COVERAGE)
            target_compile_options(test_stream_security_${security_case} PRIVATE ${_pf_cov_test_flags})
        endif()
        if(security_case STREQUAL "all_in_reversal" OR security_case STREQUAL "whole_all_in_reversal"
                OR security_case STREQUAL "slipped_short")
            add_test(NAME test_stream_security_${security_case}
                COMMAND test_stream_security_${security_case} 1 3)
            add_test(NAME test_stream_security_${security_case}_early
                COMMAND test_stream_security_${security_case} 1 1)
        else()
            add_test(NAME test_stream_security_${security_case}
                COMMAND test_stream_security_${security_case} 1 500)
            add_test(NAME test_stream_security_${security_case}_early
                COMMAND test_stream_security_${security_case} 1 30)
        endif()
    else()
        add_test(NAME test_stream_security_${security_case} COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/stream_security/${security_case}/generated.cpp")
        set_tests_properties(test_stream_security_${security_case} PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
        add_test(NAME test_stream_security_${security_case}_early COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/stream_security/${security_case}/generated.cpp")
        set_tests_properties(test_stream_security_${security_case}_early PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
    endforeach()
    # The four control rows reuse the per-case targets, so each registers only
    # while its case's generated.cpp is in the tree; absent, it is the pinned
    # SKIP placeholder too.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stream_security/htf5_close/generated.cpp")
        add_test(NAME test_stream_security_control_tf5 COMMAND test_stream_security_htf5_close 5 500)
    else()
        add_test(NAME test_stream_security_control_tf5 COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/stream_security/htf5_close/generated.cpp")
        set_tests_properties(test_stream_security_control_tf5 PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stream_security/htf60_close/generated.cpp")
        add_test(NAME test_stream_security_control_tf15 COMMAND test_stream_security_htf60_close 15 500)
    else()
        add_test(NAME test_stream_security_control_tf15 COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/stream_security/htf60_close/generated.cpp")
        set_tests_properties(test_stream_security_control_tf15 PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stream_security/pooc_dual_stop/generated.cpp")
        add_test(NAME test_stream_security_pooc_dual_stop_tf5 COMMAND test_stream_security_pooc_dual_stop 5 500)
    else()
        add_test(NAME test_stream_security_pooc_dual_stop_tf5 COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/stream_security/pooc_dual_stop/generated.cpp")
        set_tests_properties(test_stream_security_pooc_dual_stop_tf5 PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stream_security/daily_close/generated.cpp")
        add_test(NAME test_stream_security_daily_close_late COMMAND test_stream_security_daily_close 1 2000)
    else()
        add_test(NAME test_stream_security_daily_close_late COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/stream_security/daily_close/generated.cpp")
        set_tests_properties(test_stream_security_daily_close_late PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()

    # PR 1b (private fixtures): the targets below compile the checked-settings
    # generated module and the strategy-capabilities fixture source, both of
    # which the second private-fixture removal takes; the two rows register as
    # the pinned SKIP placeholders instead (tests/private_fixture_rows.txt).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/checked_settings/generated.cpp"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/strategy_capabilities.cpp")
    add_library(checked_settings_fixture OBJECT fixtures/checked_settings/generated.cpp)
    target_link_libraries(checked_settings_fixture PRIVATE pineforge)
    target_compile_options(checked_settings_fixture PRIVATE -ffp-contract=off)
    set_target_properties(checked_settings_fixture PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_executable(test_checked_settings test_checked_settings.cpp
        $<TARGET_OBJECTS:checked_settings_fixture>)
    target_link_libraries(test_checked_settings PRIVATE pineforge)
    target_compile_options(test_checked_settings PRIVATE -UNDEBUG -ffp-contract=off)
    add_test(NAME test_checked_settings COMMAND test_checked_settings)
    add_executable(test_strategy_capabilities test_strategy_capabilities.cpp
        fixtures/strategy_capabilities.cpp $<TARGET_OBJECTS:checked_settings_fixture>)
    target_link_libraries(test_strategy_capabilities PRIVATE pineforge)
    target_compile_options(test_strategy_capabilities PRIVATE -UNDEBUG -ffp-contract=off)
    add_test(NAME test_strategy_capabilities COMMAND test_strategy_capabilities)
    else()
        add_test(NAME test_checked_settings COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/checked_settings/generated.cpp")
        set_tests_properties(test_checked_settings PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
        add_test(NAME test_strategy_capabilities COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/strategy_capabilities.cpp")
        set_tests_properties(test_strategy_capabilities PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
    # The lot-grid regression case (tests/fixtures/qty_step_lot_grid): the
    # case strategy's codegen output, run through the C ABI with and without
    # the qty_step grid; every recorded row must come back byte-identical.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/qty_step_lot_grid/generated.cpp")
    add_library(qty_step_lot_grid_fixture OBJECT fixtures/qty_step_lot_grid/generated.cpp)
    target_link_libraries(qty_step_lot_grid_fixture PRIVATE pineforge)
    target_compile_options(qty_step_lot_grid_fixture PRIVATE -ffp-contract=off)
    set_target_properties(qty_step_lot_grid_fixture PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_executable(test_qty_step_lot_grid_case test_qty_step_lot_grid_case.cpp
        $<TARGET_OBJECTS:qty_step_lot_grid_fixture>)
    target_link_libraries(test_qty_step_lot_grid_case PRIVATE pineforge)
    target_compile_options(test_qty_step_lot_grid_case PRIVATE -UNDEBUG -ffp-contract=off)
    target_compile_definitions(test_qty_step_lot_grid_case PRIVATE
        PINEFORGE_QTY_STEP_LOT_GRID_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/qty_step_lot_grid")
    add_test(NAME test_qty_step_lot_grid_case COMMAND test_qty_step_lot_grid_case)
    else()
        add_test(NAME test_qty_step_lot_grid_case COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/qty_step_lot_grid/generated.cpp")
        set_tests_properties(test_qty_step_lot_grid_case PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# TradingView's calc_on_order_fills competing-order tapes
# (tests/fixtures/coof_competing_tick): an exit stop or limit leg created in
# a fill recalculation beside another order of the book books as it does
# alone. Every tape's frozen generated strategy goes into ONE module
# (coof_competing_tick_tapes.cpp), each in a namespace of its own with its C
# entry points prefixed coof_competing_tick_<tape>__; the test binds a tape by
# that prefix through scripts/run_strategy.py. The module loads into the
# Python interpreter, so it is not built under the sanitizer or coverage
# instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_cct_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_competing_tick)
    set(_pf_cct_dir ${CMAKE_BINARY_DIR}/coof_competing_tick)
    # A tape the list still names whose frozen generated.cpp is not in the tree
    # (the private-fixture PR removes some) is skipped, so the generated module
    # never names a file that is not there; the module, and the run that loads
    # it, are registered only when every listed tape is present -- a partial
    # module would name tapes the run cannot bind, an empty one no tape at all.
    set(_pf_cct_emitted 0)
    set(_pf_cct_listed 0)
    set(_pf_cct_missing "")
    if(EXISTS "${_pf_cct_fixtures}/tapes.txt")
        file(STRINGS ${_pf_cct_fixtures}/tapes.txt _pf_cct_tapes)
        list(LENGTH _pf_cct_tapes _pf_cct_listed)
        set(_pf_cct_entry_points strategy_create run_backtest run_backtest_full
            strategy_free report_free strategy_set_input strategy_set_override
            strategy_set_magnifier_volume_weighted strategy_settings_api_version
            strategy_create_checked strategy_set_input_checked
            strategy_set_override_checked strategy_get_effective_settings
            run_backtest_full_checked strategy_declares_bar_magnifier
            strategy_capabilities_api_version strategy_capabilities_receipt
            strategy_confirmed_bar_api_version strategy_confirmed_bar_receipt
            strategy_order_shapes_api_version strategy_order_shapes_receipt
            _pf_run_backtest_full_impl)
        set(_pf_cct_inc "// Generated by tests/CMakeLists.txt from fixtures/coof_competing_tick/tapes.txt.\n")
        foreach(_pf_cct_tape IN LISTS _pf_cct_tapes)
            if(EXISTS "${_pf_cct_fixtures}/${_pf_cct_tape}/generated.cpp")
                string(MAKE_C_IDENTIFIER "${_pf_cct_tape}" _pf_cct_id)
                foreach(_pf_cct_name IN LISTS _pf_cct_entry_points)
                    string(APPEND _pf_cct_inc "#define ${_pf_cct_name} coof_competing_tick_${_pf_cct_id}__${_pf_cct_name}\n")
                endforeach()
                string(APPEND _pf_cct_inc "namespace coof_competing_tick_${_pf_cct_id} {\n"
                    "#include \"${_pf_cct_fixtures}/${_pf_cct_tape}/generated.cpp\"\n"
                    "}  // namespace coof_competing_tick_${_pf_cct_id}\n")
                foreach(_pf_cct_name IN LISTS _pf_cct_entry_points)
                    string(APPEND _pf_cct_inc "#undef ${_pf_cct_name}\n")
                endforeach()
                string(APPEND _pf_cct_inc "#undef PF_PINE_TIME_SESSION_DAY_ARGS\n")
                math(EXPR _pf_cct_emitted "${_pf_cct_emitted} + 1")
            else()
                # The first listed tape that is not in the tree names the skip.
                if(NOT _pf_cct_missing)
                    set(_pf_cct_missing "tests/fixtures/coof_competing_tick/${_pf_cct_tape}/generated.cpp")
                endif()
            endif()
        endforeach()
    endif()
    if(_pf_cct_emitted GREATER 0 AND _pf_cct_emitted EQUAL _pf_cct_listed)
        file(GENERATE OUTPUT ${_pf_cct_dir}/coof_competing_tick_tapes.inc CONTENT "${_pf_cct_inc}")
        add_library(coof_competing_tick_tapes MODULE coof_competing_tick_tapes.cpp)
        target_include_directories(coof_competing_tick_tapes PRIVATE
            ${_pf_cct_dir} ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(coof_competing_tick_tapes PRIVATE pineforge)
        if(APPLE)
            target_link_options(coof_competing_tick_tapes PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(coof_competing_tick_tapes PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        target_compile_options(coof_competing_tick_tapes PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        set_target_properties(coof_competing_tick_tapes PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_cct_dir} PREFIX "" SUFFIX ".so")
        add_test(NAME test_coof_competing_tick_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_coof_competing_tick_tapes.py
                ${_pf_cct_fixtures} $<TARGET_FILE:coof_competing_tick_tapes>
                ${PROJECT_SOURCE_DIR}/scripts)
    else()
        if(NOT _pf_cct_missing)
            set(_pf_cct_missing "tests/fixtures/coof_competing_tick/tapes.txt")
        endif()
        add_test(NAME test_coof_competing_tick_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: ${_pf_cct_missing}")
        set_tests_properties(test_coof_competing_tick_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# TradingView's callback and lifecycle synthetic tapes
# (tests/fixtures/callback_lifecycle). Every tape's frozen generated
# strategy goes into ONE module (callback_lifecycle_tapes.cpp: one
# translation unit, one copy of the library), each in a namespace of its own
# with its C entry points prefixed callback_lifecycle_<tape>__; the test binds a tape by that
# prefix through scripts/run_strategy.py. The module loads into the Python
# interpreter, so it is not built under the sanitizer or coverage
# instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_clt_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callback_lifecycle)
    set(_pf_clt_dir ${CMAKE_BINARY_DIR}/callback_lifecycle)
    # tapes.txt is a distributed list, so it survives the private-fixture PR,
    # but 60 of the cases it names do not. A listed tape whose generated.cpp is
    # absent is skipped, so the generated module never names a file that is not
    # there, and the module -- with the run that reads the whole list -- is
    # registered only when every listed tape is present; otherwise the row is
    # the private-fixture SKIP placeholder.
    set(_pf_clt_emitted 0)
    set(_pf_clt_listed 0)
    set(_pf_clt_missing "")
    if(EXISTS "${_pf_clt_fixtures}/tapes.txt")
        file(STRINGS ${_pf_clt_fixtures}/tapes.txt _pf_clt_tapes)
        list(LENGTH _pf_clt_tapes _pf_clt_listed)
        set(_pf_clt_entry_points strategy_create run_backtest run_backtest_full
            strategy_free report_free strategy_set_input strategy_set_override
            strategy_set_magnifier_volume_weighted strategy_settings_api_version
            strategy_create_checked strategy_set_input_checked
            strategy_set_override_checked strategy_get_effective_settings
            run_backtest_full_checked strategy_declares_bar_magnifier
            _pf_run_backtest_full_impl)
        set(_pf_clt_inc "// Generated by tests/CMakeLists.txt from fixtures/callback_lifecycle/tapes.txt.\n")
        foreach(_pf_clt_tape IN LISTS _pf_clt_tapes)
            if(EXISTS "${_pf_clt_fixtures}/${_pf_clt_tape}/generated.cpp")
                string(MAKE_C_IDENTIFIER "${_pf_clt_tape}" _pf_clt_id)
                foreach(_pf_clt_name IN LISTS _pf_clt_entry_points)
                    string(APPEND _pf_clt_inc "#define ${_pf_clt_name} callback_lifecycle_${_pf_clt_id}__${_pf_clt_name}\n")
                endforeach()
                string(APPEND _pf_clt_inc "namespace callback_lifecycle_${_pf_clt_id} {\n"
                    "#include \"${_pf_clt_fixtures}/${_pf_clt_tape}/generated.cpp\"\n"
                    "}  // namespace callback_lifecycle_${_pf_clt_id}\n")
                foreach(_pf_clt_name IN LISTS _pf_clt_entry_points)
                    string(APPEND _pf_clt_inc "#undef ${_pf_clt_name}\n")
                endforeach()
                string(APPEND _pf_clt_inc "#undef PF_PINE_TIME_SESSION_DAY_ARGS\n")
                math(EXPR _pf_clt_emitted "${_pf_clt_emitted} + 1")
            else()
                # The first listed tape that is not in the tree names the skip.
                if(NOT _pf_clt_missing)
                    set(_pf_clt_missing "tests/fixtures/callback_lifecycle/${_pf_clt_tape}/generated.cpp")
                endif()
            endif()
        endforeach()
    endif()
    if(_pf_clt_emitted GREATER 0 AND _pf_clt_emitted EQUAL _pf_clt_listed)
        file(GENERATE OUTPUT ${_pf_clt_dir}/callback_lifecycle_tapes.inc CONTENT "${_pf_clt_inc}")
        add_library(callback_lifecycle_tapes MODULE callback_lifecycle_tapes.cpp)
        target_include_directories(callback_lifecycle_tapes PRIVATE
            ${_pf_clt_dir} ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(callback_lifecycle_tapes PRIVATE pineforge)
        if(APPLE)
            target_link_options(callback_lifecycle_tapes PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(callback_lifecycle_tapes PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        target_compile_options(callback_lifecycle_tapes PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        set_target_properties(callback_lifecycle_tapes PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_clt_dir} PREFIX "" SUFFIX ".so")
        add_test(NAME test_callback_lifecycle_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_callback_lifecycle_tapes.py
                ${_pf_clt_fixtures} $<TARGET_FILE:callback_lifecycle_tapes>
                ${PROJECT_SOURCE_DIR}/scripts)
    else()
        if(NOT _pf_clt_missing)
            set(_pf_clt_missing "tests/fixtures/callback_lifecycle/tapes.txt")
        endif()
        add_test(NAME test_callback_lifecycle_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: ${_pf_clt_missing}")
        set_tests_properties(test_callback_lifecycle_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# TradingView's per-entry exit tapes (tests/fixtures/per_entry_exit): a
# global strategy.exit with a relative leg called while a position is held,
# beside an add of the held side that fills later. Every tape's frozen
# generated strategy goes into ONE module (per_entry_exit_tapes.cpp), each in
# a namespace of its own with its C entry points prefixed
# per_entry_exit_<tape>__; the test binds a tape by that prefix through
# scripts/run_strategy.py. The module loads into the Python interpreter, so it
# is not built under the sanitizer or coverage instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_pee_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/per_entry_exit)
    set(_pf_pee_dir ${CMAKE_BINARY_DIR}/per_entry_exit)
    set(_pf_pee_emitted 0)
    set(_pf_pee_listed 0)
    set(_pf_pee_missing "")
    if(EXISTS "${_pf_pee_fixtures}/tapes.txt")
        file(STRINGS ${_pf_pee_fixtures}/tapes.txt _pf_pee_tapes)
        list(LENGTH _pf_pee_tapes _pf_pee_listed)
        set(_pf_pee_entry_points strategy_create run_backtest run_backtest_full
            strategy_free report_free strategy_set_input strategy_set_override
            strategy_set_magnifier_volume_weighted strategy_settings_api_version
            strategy_create_checked strategy_set_input_checked
            strategy_set_override_checked strategy_get_effective_settings
            run_backtest_full_checked strategy_declares_bar_magnifier
            strategy_capabilities_api_version strategy_capabilities_receipt
            strategy_confirmed_bar_api_version strategy_confirmed_bar_receipt
            strategy_order_shapes_api_version strategy_order_shapes_receipt
            _pf_run_backtest_full_impl)
        set(_pf_pee_inc "// Generated by tests/CMakeLists.txt from fixtures/per_entry_exit/tapes.txt.\n")
        foreach(_pf_pee_tape IN LISTS _pf_pee_tapes)
            if(EXISTS "${_pf_pee_fixtures}/${_pf_pee_tape}/generated.cpp")
                string(MAKE_C_IDENTIFIER "${_pf_pee_tape}" _pf_pee_id)
                foreach(_pf_pee_name IN LISTS _pf_pee_entry_points)
                    string(APPEND _pf_pee_inc "#define ${_pf_pee_name} per_entry_exit_${_pf_pee_id}__${_pf_pee_name}\n")
                endforeach()
                string(APPEND _pf_pee_inc "namespace per_entry_exit_${_pf_pee_id} {\n"
                    "#include \"${_pf_pee_fixtures}/${_pf_pee_tape}/generated.cpp\"\n"
                    "}  // namespace per_entry_exit_${_pf_pee_id}\n")
                foreach(_pf_pee_name IN LISTS _pf_pee_entry_points)
                    string(APPEND _pf_pee_inc "#undef ${_pf_pee_name}\n")
                endforeach()
                string(APPEND _pf_pee_inc "#undef PF_PINE_TIME_SESSION_DAY_ARGS\n")
                math(EXPR _pf_pee_emitted "${_pf_pee_emitted} + 1")
            else()
                # The first listed tape that is not in the tree names the skip.
                if(NOT _pf_pee_missing)
                    set(_pf_pee_missing "tests/fixtures/per_entry_exit/${_pf_pee_tape}/generated.cpp")
                endif()
            endif()
        endforeach()
    endif()
    if(_pf_pee_emitted GREATER 0 AND _pf_pee_emitted EQUAL _pf_pee_listed)
        file(GENERATE OUTPUT ${_pf_pee_dir}/per_entry_exit_tapes.inc CONTENT "${_pf_pee_inc}")
        add_library(per_entry_exit_tapes MODULE per_entry_exit_tapes.cpp)
        target_include_directories(per_entry_exit_tapes PRIVATE
            ${_pf_pee_dir} ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(per_entry_exit_tapes PRIVATE pineforge)
        if(APPLE)
            target_link_options(per_entry_exit_tapes PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(per_entry_exit_tapes PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        target_compile_options(per_entry_exit_tapes PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        set_target_properties(per_entry_exit_tapes PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_pee_dir} PREFIX "" SUFFIX ".so")
        add_test(NAME test_per_entry_exit_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_per_entry_exit_tapes.py
                ${_pf_pee_fixtures} $<TARGET_FILE:per_entry_exit_tapes>
                ${PROJECT_SOURCE_DIR}/scripts)
    else()
        if(NOT _pf_pee_missing)
            set(_pf_pee_missing "tests/fixtures/per_entry_exit/tapes.txt")
        endif()
        add_test(NAME test_per_entry_exit_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: ${_pf_pee_missing}")
        set_tests_properties(test_per_entry_exit_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# Recorded outputs through docker/run_json.py (--outputs): two strategy
# libraries run as a process, one that records outputs
# (outputs_fixture_strategy.cpp, a bare host with the per-library outputs
# exports) and one frozen generated strategy that records none. Both load
# into the Python interpreter, so neither is built under the sanitizer or
# coverage instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_outputs_dir ${CMAKE_BINARY_DIR}/outputs_fixture)
    add_library(outputs_fixture_strategy MODULE outputs_fixture_strategy.cpp)
    set(_pf_outputs_libraries outputs_fixture_strategy)
    # The frozen generated strategy is a private fixture; only register its
    # module -- and the process run that needs both modules -- when it is
    # present, so a tree without the private fixtures still configures.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callback_lifecycle/carried-static-stop/generated.cpp")
        add_library(outputs_fixture_generated MODULE
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callback_lifecycle/carried-static-stop/generated.cpp)
        target_include_directories(outputs_fixture_generated PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_options(outputs_fixture_generated PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        list(APPEND _pf_outputs_libraries outputs_fixture_generated)
    endif()
    foreach(_pf_outputs_library IN LISTS _pf_outputs_libraries)
        target_link_libraries(${_pf_outputs_library} PRIVATE pineforge)
        if(APPLE)
            target_link_options(${_pf_outputs_library} PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(${_pf_outputs_library} PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        set_target_properties(${_pf_outputs_library} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_outputs_dir} PREFIX "" SUFFIX ".so")
    endforeach()
    if(TARGET outputs_fixture_generated)
        add_test(NAME test_run_json_outputs
            COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test_run_json_outputs.py
                $<TARGET_FILE:outputs_fixture_strategy> $<TARGET_FILE:outputs_fixture_generated>)
    else()
        add_test(NAME test_run_json_outputs COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/callback_lifecycle/carried-static-stop/generated.cpp")
        set_tests_properties(test_run_json_outputs PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# TradingView's same-side exit tapes (tests/fixtures/same_side_exit): a
# global strategy.exit called while a position is held beside an add of the
# held side (a resting limit or stop, or a market add at the pyramiding cap).
# Every tape's frozen generated strategy goes into ONE module
# (same_side_exit_tapes.cpp), each in a namespace of its own with its C entry
# points prefixed same_side_exit_<tape>__; the test binds a tape by that
# prefix through scripts/run_strategy.py. The module loads into the
# Python interpreter, so it is not built under the sanitizer or coverage
# instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_sse_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/same_side_exit)
    set(_pf_sse_dir ${CMAKE_BINARY_DIR}/same_side_exit)
    set(_pf_sse_emitted 0)
    set(_pf_sse_listed 0)
    set(_pf_sse_missing "")
    if(EXISTS "${_pf_sse_fixtures}/tapes.txt")
        file(STRINGS ${_pf_sse_fixtures}/tapes.txt _pf_sse_tapes)
        list(LENGTH _pf_sse_tapes _pf_sse_listed)
        set(_pf_sse_entry_points strategy_create run_backtest run_backtest_full
            strategy_free report_free strategy_set_input strategy_set_override
            strategy_set_magnifier_volume_weighted strategy_settings_api_version
            strategy_create_checked strategy_set_input_checked
            strategy_set_override_checked strategy_get_effective_settings
            run_backtest_full_checked strategy_declares_bar_magnifier
            strategy_capabilities_api_version strategy_capabilities_receipt
            strategy_confirmed_bar_api_version strategy_confirmed_bar_receipt
            _pf_run_backtest_full_impl)
        set(_pf_sse_inc "// Generated by tests/CMakeLists.txt from fixtures/same_side_exit/tapes.txt.\n")
        foreach(_pf_sse_tape IN LISTS _pf_sse_tapes)
            if(EXISTS "${_pf_sse_fixtures}/${_pf_sse_tape}/generated.cpp")
                string(MAKE_C_IDENTIFIER "${_pf_sse_tape}" _pf_sse_id)
                foreach(_pf_sse_name IN LISTS _pf_sse_entry_points)
                    string(APPEND _pf_sse_inc "#define ${_pf_sse_name} same_side_exit_${_pf_sse_id}__${_pf_sse_name}\n")
                endforeach()
                string(APPEND _pf_sse_inc "namespace same_side_exit_${_pf_sse_id} {\n"
                    "#include \"${_pf_sse_fixtures}/${_pf_sse_tape}/generated.cpp\"\n"
                    "}  // namespace same_side_exit_${_pf_sse_id}\n")
                foreach(_pf_sse_name IN LISTS _pf_sse_entry_points)
                    string(APPEND _pf_sse_inc "#undef ${_pf_sse_name}\n")
                endforeach()
                string(APPEND _pf_sse_inc "#undef PF_PINE_TIME_SESSION_DAY_ARGS\n")
                math(EXPR _pf_sse_emitted "${_pf_sse_emitted} + 1")
            else()
                # The first listed tape that is not in the tree names the skip.
                if(NOT _pf_sse_missing)
                    set(_pf_sse_missing "tests/fixtures/same_side_exit/${_pf_sse_tape}/generated.cpp")
                endif()
            endif()
        endforeach()
    endif()
    if(_pf_sse_emitted GREATER 0 AND _pf_sse_emitted EQUAL _pf_sse_listed)
        file(GENERATE OUTPUT ${_pf_sse_dir}/same_side_exit_tapes.inc CONTENT "${_pf_sse_inc}")
        add_library(same_side_exit_tapes MODULE same_side_exit_tapes.cpp)
        target_include_directories(same_side_exit_tapes PRIVATE
            ${_pf_sse_dir} ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(same_side_exit_tapes PRIVATE pineforge)
        if(APPLE)
            target_link_options(same_side_exit_tapes PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(same_side_exit_tapes PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        target_compile_options(same_side_exit_tapes PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        set_target_properties(same_side_exit_tapes PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_sse_dir} PREFIX "" SUFFIX ".so")
        add_test(NAME test_same_side_exit_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_same_side_exit_tapes.py
                ${_pf_sse_fixtures} $<TARGET_FILE:same_side_exit_tapes>
                ${PROJECT_SOURCE_DIR}/scripts)
    else()
        if(NOT _pf_sse_missing)
            set(_pf_sse_missing "tests/fixtures/same_side_exit/tapes.txt")
        endif()
        add_test(NAME test_same_side_exit_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: ${_pf_sse_missing}")
        set_tests_properties(test_same_side_exit_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# TradingView's cross-side exit tapes (tests/fixtures/cross_side_exit): a
# global strategy.exit called while a position is held beside a resting entry
# order of the other side. Every tape's frozen generated strategy goes into
# ONE module (cross_side_exit_tapes.cpp), each in a namespace of its own with
# its C entry points prefixed cross_side_exit_<tape>__; the test binds a tape
# by that prefix through scripts/run_strategy.py. The module loads into the
# Python interpreter, so it is not built under the sanitizer or coverage
# instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_cse_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cross_side_exit)
    set(_pf_cse_dir ${CMAKE_BINARY_DIR}/cross_side_exit)
    set(_pf_cse_emitted 0)
    set(_pf_cse_listed 0)
    set(_pf_cse_missing "")
    if(EXISTS "${_pf_cse_fixtures}/tapes.txt")
        file(STRINGS ${_pf_cse_fixtures}/tapes.txt _pf_cse_tapes)
        list(LENGTH _pf_cse_tapes _pf_cse_listed)
        set(_pf_cse_entry_points strategy_create run_backtest run_backtest_full
            strategy_free report_free strategy_set_input strategy_set_override
            strategy_set_magnifier_volume_weighted strategy_settings_api_version
            strategy_create_checked strategy_set_input_checked
            strategy_set_override_checked strategy_get_effective_settings
            run_backtest_full_checked strategy_declares_bar_magnifier
            strategy_capabilities_api_version strategy_capabilities_receipt
            _pf_run_backtest_full_impl)
        set(_pf_cse_inc "// Generated by tests/CMakeLists.txt from fixtures/cross_side_exit/tapes.txt.\n")
        foreach(_pf_cse_tape IN LISTS _pf_cse_tapes)
            if(EXISTS "${_pf_cse_fixtures}/${_pf_cse_tape}/generated.cpp")
                string(MAKE_C_IDENTIFIER "${_pf_cse_tape}" _pf_cse_id)
                foreach(_pf_cse_name IN LISTS _pf_cse_entry_points)
                    string(APPEND _pf_cse_inc "#define ${_pf_cse_name} cross_side_exit_${_pf_cse_id}__${_pf_cse_name}\n")
                endforeach()
                string(APPEND _pf_cse_inc "namespace cross_side_exit_${_pf_cse_id} {\n"
                    "#include \"${_pf_cse_fixtures}/${_pf_cse_tape}/generated.cpp\"\n"
                    "}  // namespace cross_side_exit_${_pf_cse_id}\n")
                foreach(_pf_cse_name IN LISTS _pf_cse_entry_points)
                    string(APPEND _pf_cse_inc "#undef ${_pf_cse_name}\n")
                endforeach()
                string(APPEND _pf_cse_inc "#undef PF_PINE_TIME_SESSION_DAY_ARGS\n")
                math(EXPR _pf_cse_emitted "${_pf_cse_emitted} + 1")
            else()
                # The first listed tape that is not in the tree names the skip.
                if(NOT _pf_cse_missing)
                    set(_pf_cse_missing "tests/fixtures/cross_side_exit/${_pf_cse_tape}/generated.cpp")
                endif()
            endif()
        endforeach()
    endif()
    if(_pf_cse_emitted GREATER 0 AND _pf_cse_emitted EQUAL _pf_cse_listed)
        file(GENERATE OUTPUT ${_pf_cse_dir}/cross_side_exit_tapes.inc CONTENT "${_pf_cse_inc}")
        add_library(cross_side_exit_tapes MODULE cross_side_exit_tapes.cpp)
        target_include_directories(cross_side_exit_tapes PRIVATE
            ${_pf_cse_dir} ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(cross_side_exit_tapes PRIVATE pineforge)
        if(APPLE)
            target_link_options(cross_side_exit_tapes PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(cross_side_exit_tapes PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        target_compile_options(cross_side_exit_tapes PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        set_target_properties(cross_side_exit_tapes PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_cse_dir} PREFIX "" SUFFIX ".so")
        add_test(NAME test_cross_side_exit_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_cross_side_exit_tapes.py
                ${_pf_cse_fixtures} $<TARGET_FILE:cross_side_exit_tapes>
                ${PROJECT_SOURCE_DIR}/scripts)
    else()
        if(NOT _pf_cse_missing)
            set(_pf_cse_missing "tests/fixtures/cross_side_exit/tapes.txt")
        endif()
        add_test(NAME test_cross_side_exit_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: ${_pf_cse_missing}")
        set_tests_properties(test_cross_side_exit_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# TradingView's POOC same-pass position-view tapes
# (tests/fixtures/pooc_close_fill_view): an order that fills at the bar's close
# stays invisible to the script pass that placed it. Every tape's frozen
# generated strategy goes into ONE module (pooc_close_fill_view_tapes.cpp),
# each in a namespace of its own with its C entry points prefixed
# pooc_close_fill_view_<tape>__; the test binds a tape by that prefix through
# scripts/run_strategy.py. The module loads into the Python interpreter, so it
# is not built under the sanitizer or coverage instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_pcv_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_close_fill_view)
    set(_pf_pcv_dir ${CMAKE_BINARY_DIR}/pooc_close_fill_view)
    # tapes.txt and every tape's generated.cpp are private fixtures (PR 1b
    # removes this whole directory); a listed tape whose generated.cpp is not
    # in the tree is skipped either way, and the module is registered only
    # when every listed tape is present (the run reads the whole list, so a
    # partial module would name tapes it cannot bind). The row follows the
    # module, and the block below pins the row as a SKIP when no module is.
    set(_pf_pcv_emitted 0)
    set(_pf_pcv_listed 0)
    if(EXISTS "${_pf_pcv_fixtures}/tapes.txt")
        file(STRINGS ${_pf_pcv_fixtures}/tapes.txt _pf_pcv_tapes)
        list(LENGTH _pf_pcv_tapes _pf_pcv_listed)
        set(_pf_pcv_entry_points strategy_create run_backtest run_backtest_full
            strategy_free report_free strategy_set_input strategy_set_override
            strategy_set_magnifier_volume_weighted strategy_settings_api_version
            strategy_create_checked strategy_set_input_checked
            strategy_set_override_checked strategy_get_effective_settings
            run_backtest_full_checked strategy_declares_bar_magnifier
            strategy_capabilities_api_version strategy_capabilities_receipt
            strategy_confirmed_bar_api_version strategy_confirmed_bar_receipt
            _pf_run_backtest_full_impl)
        set(_pf_pcv_inc "// Generated by tests/CMakeLists.txt from fixtures/pooc_close_fill_view/tapes.txt.\n")
        foreach(_pf_pcv_tape IN LISTS _pf_pcv_tapes)
            if(EXISTS "${_pf_pcv_fixtures}/${_pf_pcv_tape}/generated.cpp")
                string(MAKE_C_IDENTIFIER "${_pf_pcv_tape}" _pf_pcv_id)
                foreach(_pf_pcv_name IN LISTS _pf_pcv_entry_points)
                    string(APPEND _pf_pcv_inc "#define ${_pf_pcv_name} pooc_close_fill_view_${_pf_pcv_id}__${_pf_pcv_name}\n")
                endforeach()
                string(APPEND _pf_pcv_inc "namespace pooc_close_fill_view_${_pf_pcv_id} {\n"
                    "#include \"${_pf_pcv_fixtures}/${_pf_pcv_tape}/generated.cpp\"\n"
                    "}  // namespace pooc_close_fill_view_${_pf_pcv_id}\n")
                foreach(_pf_pcv_name IN LISTS _pf_pcv_entry_points)
                    string(APPEND _pf_pcv_inc "#undef ${_pf_pcv_name}\n")
                endforeach()
                string(APPEND _pf_pcv_inc "#undef PF_PINE_TIME_SESSION_DAY_ARGS\n")
                math(EXPR _pf_pcv_emitted "${_pf_pcv_emitted} + 1")
            endif()
        endforeach()
    endif()
    if(_pf_pcv_emitted GREATER 0 AND _pf_pcv_emitted EQUAL _pf_pcv_listed)
        file(GENERATE OUTPUT ${_pf_pcv_dir}/pooc_close_fill_view_tapes.inc CONTENT "${_pf_pcv_inc}")
        add_library(pooc_close_fill_view_tapes MODULE pooc_close_fill_view_tapes.cpp)
        target_include_directories(pooc_close_fill_view_tapes PRIVATE
            ${_pf_pcv_dir} ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(pooc_close_fill_view_tapes PRIVATE pineforge)
        if(APPLE)
            target_link_options(pooc_close_fill_view_tapes PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(pooc_close_fill_view_tapes PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        target_compile_options(pooc_close_fill_view_tapes PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        set_target_properties(pooc_close_fill_view_tapes PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_pcv_dir} PREFIX "" SUFFIX ".so")
    endif()
    if(TARGET pooc_close_fill_view_tapes)
        add_test(NAME test_pooc_close_fill_view_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_pooc_close_fill_view_tapes.py
                ${_pf_pcv_fixtures} $<TARGET_FILE:pooc_close_fill_view_tapes>
                ${PROJECT_SOURCE_DIR}/scripts)
    else()
        add_test(NAME test_pooc_close_fill_view_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_close_fill_view/tapes.txt")
        set_tests_properties(test_pooc_close_fill_view_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# TradingView's explicit-quantity and close-time bar-fill tapes
# (tests/fixtures/explicit_qty_floor, tests/fixtures/pooc_close_bar_fills),
# run through each tape's frozen generated strategy: ONE module
# (script_rule_tapes.cpp: one translation unit, one copy of the library), each
# tape in a namespace of its own with its C entry points prefixed
# script_rule_<root>_<tape>__; the test binds a tape by that prefix through
# scripts/run_strategy.py. The module loads into the Python interpreter, so it
# is not built under the sanitizer or coverage instrumentation.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    set(_pf_srt_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
    set(_pf_srt_dir ${CMAKE_BINARY_DIR}/script_rule_tapes)
    set(_pf_srt_entry_points strategy_create run_backtest run_backtest_full
        strategy_free report_free strategy_set_input strategy_set_override
        strategy_set_magnifier_volume_weighted strategy_settings_api_version
        strategy_create_checked strategy_set_input_checked
        strategy_set_override_checked strategy_get_effective_settings
        run_backtest_full_checked strategy_declares_bar_magnifier
        _pf_run_backtest_full_impl)
    set(_pf_srt_inc "// Generated by tests/CMakeLists.txt from the roots' tapes.txt.\n")
    set(_pf_srt_emitted 0)
    set(_pf_srt_listed 0)
    foreach(_pf_srt_root IN ITEMS explicit_qty_floor pooc_close_bar_fills)
        if(EXISTS "${_pf_srt_fixtures}/${_pf_srt_root}/tapes.txt")
            file(STRINGS ${_pf_srt_fixtures}/${_pf_srt_root}/tapes.txt _pf_srt_tapes)
            list(LENGTH _pf_srt_tapes _pf_srt_root_listed)
            math(EXPR _pf_srt_listed "${_pf_srt_listed} + ${_pf_srt_root_listed}")
            foreach(_pf_srt_tape IN LISTS _pf_srt_tapes)
                if(EXISTS "${_pf_srt_fixtures}/${_pf_srt_root}/${_pf_srt_tape}/generated.cpp")
                    string(MAKE_C_IDENTIFIER "${_pf_srt_root}_${_pf_srt_tape}" _pf_srt_id)
                    foreach(_pf_srt_name IN LISTS _pf_srt_entry_points)
                        string(APPEND _pf_srt_inc
                            "#define ${_pf_srt_name} script_rule_${_pf_srt_id}__${_pf_srt_name}\n")
                    endforeach()
                    string(APPEND _pf_srt_inc "namespace script_rule_${_pf_srt_id} {\n"
                        "#include \"${_pf_srt_fixtures}/${_pf_srt_root}/${_pf_srt_tape}/generated.cpp\"\n"
                        "}  // namespace script_rule_${_pf_srt_id}\n")
                    foreach(_pf_srt_name IN LISTS _pf_srt_entry_points)
                        string(APPEND _pf_srt_inc "#undef ${_pf_srt_name}\n")
                    endforeach()
                    string(APPEND _pf_srt_inc "#undef PF_PINE_TIME_SESSION_DAY_ARGS\n")
                    math(EXPR _pf_srt_emitted "${_pf_srt_emitted} + 1")
                endif()
            endforeach()
        endif()
    endforeach()
    # The module is registered only when every listed tape of every present
    # root is in the tree: a listed tape whose generated.cpp is not there is
    # skipped, and the runs read the roots' whole lists, so a partial module
    # would name tapes they cannot bind (an empty one no tape at all).
    if(_pf_srt_emitted GREATER 0 AND _pf_srt_emitted EQUAL _pf_srt_listed)
        file(GENERATE OUTPUT ${_pf_srt_dir}/script_rule_tapes.inc CONTENT "${_pf_srt_inc}")
        add_library(script_rule_tapes MODULE script_rule_tapes.cpp)
        target_include_directories(script_rule_tapes PRIVATE
            ${_pf_srt_dir} ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(script_rule_tapes PRIVATE pineforge)
        if(APPLE)
            target_link_options(script_rule_tapes PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(script_rule_tapes PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        target_compile_options(script_rule_tapes PRIVATE -O0 -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        set_target_properties(script_rule_tapes PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${_pf_srt_dir} PREFIX "" SUFFIX ".so")
    endif()
    if(TARGET script_rule_tapes AND EXISTS "${_pf_srt_fixtures}/explicit_qty_floor/tapes.txt")
        add_test(NAME test_explicit_qty_floor_generated_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_script_rule_tapes.py
                ${_pf_srt_fixtures} $<TARGET_FILE:script_rule_tapes>
                ${PROJECT_SOURCE_DIR}/scripts --root explicit_qty_floor)
    else()
        add_test(NAME test_explicit_qty_floor_generated_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/explicit_qty_floor/tapes.txt")
        set_tests_properties(test_explicit_qty_floor_generated_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
    if(TARGET script_rule_tapes AND EXISTS "${_pf_srt_fixtures}/pooc_close_bar_fills/tapes.txt")
        add_test(NAME test_pooc_close_bar_fills_generated_tapes
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_script_rule_tapes.py
                ${_pf_srt_fixtures} $<TARGET_FILE:script_rule_tapes>
                ${PROJECT_SOURCE_DIR}/scripts --root pooc_close_bar_fills)
    else()
        add_test(NAME test_pooc_close_bar_fills_generated_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_close_bar_fills/tapes.txt")
        set_tests_properties(test_pooc_close_bar_fills_generated_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# Register one unit-test executable and its ctest row. Extracted from the
# loop below so the private-fixture guard (see tests/private_fixture_rows.txt)
# can register the identical real test on the PRESENT branch below.
function(pineforge_add_unit_test test_name)
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
    if(test_name STREQUAL "test_native_command_after")
        # Slice 8 runs book 67 alone; the other heavy books go to slices 1..3.
        # These rows share the executable above and keep their slow labels.
        add_test(NAME test_native_command_after_slice_1
            COMMAND ${test_name} --slice 1/8)
        add_test(NAME test_native_command_after_slice_2
            COMMAND ${test_name} --slice 2/8)
        add_test(NAME test_native_command_after_slice_3
            COMMAND ${test_name} --slice 3/8)
        add_test(NAME test_native_command_after_slice_4
            COMMAND ${test_name} --slice 4/8)
        add_test(NAME test_native_command_after_slice_5
            COMMAND ${test_name} --slice 5/8)
        add_test(NAME test_native_command_after_slice_6
            COMMAND ${test_name} --slice 6/8)
        add_test(NAME test_native_command_after_slice_7
            COMMAND ${test_name} --slice 7/8)
        add_test(NAME test_native_command_after_slice_8
            COMMAND ${test_name} --slice 8/8)
        # COST orders starts by expected work; it does not impose a time limit.
        set_tests_properties(test_native_command_after_slice_8 PROPERTIES COST 600)
        set_tests_properties(test_native_command_after_slice_1 PROPERTIES COST 60)
        set_tests_properties(test_native_command_after_slice_2 PROPERTIES COST 60)
        set_tests_properties(test_native_command_after_slice_3 PROPERTIES COST 60)
        add_test(NAME test_native_command_after_slices_cover
            COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test_native_command_after_split.py)
    elseif(NOT test_name STREQUAL "test_l4g_runtime_budget"
            AND NOT test_name STREQUAL "test_open_report_rows_flat")
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endfunction()

# Rows whose source, fixture or quoted #include the private-fixture PR
# removes are registered by guarded blocks (mostly below, or beside their
# twin registration for the oracle rows; the coof tape row sits before the
# define loop that consumes its target) instead of by this loop, so a tree
# without the private fixtures still configures and every affected row
# appears as a loud SKIP.
set(_pf_private_fixture_rows
    test_aapl15_margin_brackets_l4a
    test_adapter_coof_relower
    test_adapter_in_place_reads
    test_adapter_magnifier_intrabar_tapes
    test_adapter_margin_schedule_differential
    test_adapter_receipts_in_place
    test_adapter_runtime_ambient
    test_adapter_settlement_carry
    test_adapter_sizing_relower
    test_add_sibling_margin_tapes
    test_admission_rules_tapes
    test_aggregated_path_regressions
    test_carried_pooc_short_margin_state_l4a
    test_carried_trail_open_tapes
    test_cash_fee_sizing_tapes
    test_chart_tf_security_split_feed
    test_coof_chart_tick_touch_l4c
    test_coof_close_slippage_tapes
    test_coof_forced_waypoint_tapes
    test_coof_immediate_close_final_tapes
    test_coof_immediate_close_tapes
    test_coof_recalc_security_tapes
    test_cross_mechanism_tapes
    test_default_flat_market_gross_admission_l4d
    test_default_pct_stop_sizing_l4b
    test_default_qty_signal_freeze
    test_engine_trade_accessors
    test_exit_binding_tapes
    test_exit_reservation_tapes
    test_explicit_market_admission_tapes
    test_explicit_qty_fill_admission_l4b
    test_explicit_qty_floor_tapes
    test_famae_declined_reversal_trail_gap_l4c
    test_famae_lot_sizing_ten_digit_equity
    test_famx_declined_reversal_trail_leg_l4c
    test_flat_coof_exit_tapes
    test_foreign_break_stamp_tapes
    test_high_value_signal_cost_l4d
    test_int28_rule_compositions
    test_int30_rule_compositions
    test_kernel_trail_stop_replica
    test_l10ad_tick_bracket_stuck_entry
    test_l4b_policy_regressions
    test_l9a_carried_pooc_competing_checkpoint
    test_live_trade_accessors_l4d
    test_local_time_fields
    test_ltf_lookahead_calling_open_tapes
    test_magnified_aggregated_tape
    test_magnifier_tradingview_intrabars
    test_margin_call_1x_long_entry_fill_l4a
    test_margin_call_rules_tapes
    test_margin_ledger_rules_tapes
    test_margin_open_print_tapes
    test_margin_open_rules_tapes
    test_margin_opposite_tapes
    test_margin_residual_tapes
    test_margin_rules_forward_replay
    test_margin_schedule_rules_tapes
    test_margin_v6_tapes
    test_native_calc_timing_twin
    test_native_daily_holiday_l4d
    test_native_instrument_feed
    test_native_margin_hooks_twin
    test_native_margin_post_fill_path
    test_native_oracle_day_key_l2
    test_native_oracle_reversal_same_bar_tx_l2
    test_native_oracle_stop_snapshot_full_l2
    test_native_price_grid
    test_native_session_day_bar_open
    test_native_session_day_facts
    test_native_trail_stop_ladder
    test_native_wm_buckets_l4d
    test_o_close_pct_day_anchor
    test_oanda_lazy_close
    test_offset_trail_quantized_arm
    test_open_fill_order_tapes
    test_pending_entry_trail_tapes
    test_pine_time_day_stamp_grid
    test_point_order_open_call
    test_pooc_fill_stamp_aggregation
    test_pooc_gap_stop_tapes
    test_pooc_limit_close_tapes
    test_pooc_open_money_event_l4b
    test_pooc_reversing_stop_tapes
    test_pooc_samepass_tapes
    test_pooc_stop_reentry_bracket_tapes
    test_publication_witness
    test_pyramiding_count_differential
    test_reversal_admission_float_guard_l4c
    test_reversal_close_all_tapes
    test_risk_max_intraday_loss_tv_l4a
    test_rounded_carried_short_trail_l4c
    test_run_harness_window_tapes
    test_same_open_reversal_tapes
    test_second_extreme_order_tapes
    test_security_multi_day_buckets_tapes
    test_session_break_open_bar_tape
    test_session_calendar_extra
    test_session_day_anchors
    test_session_day_facts_adapter
    test_session_extended_day_tape
    test_session_islastbar_aggregation
    test_session_ismarket_tape
    test_session_predicates
    test_session_prepost_windows_tape
    test_slipped_short_tapes
    test_source_buying_power_tapes
    test_split_feed_partial_bucket
    test_symbol_calendar_tapes
    test_taro_price_gap_admission_l4d
    test_time_bars_back_tapes
    test_trail_activation_tick_bar_l4c
    test_trail_activation_tick_reach
    test_trail_fill_snap_l4c
    test_tv_money_carried_pooc_l4b
    test_whole_lot_tie_tapes
    test_zero_offset_trail_rides_l4c
    test_zero_trail_sibling_stop)
# PR 1b: rows the second private-fixture removal guards (same registered-only-
# by-a-guarded-block rule above).
list(APPEND _pf_private_fixture_rows
    test_adapter_fifo_lot_boundary
    test_adapter_margin_relower
    test_adapter_margin_revival_cancel
    test_aggregated_admission_receipt_tapes
    test_aggregated_entry_bar_index_tape
    test_aux_security_feed_l4d
    test_color_tapes
    test_coof_drawing_rollback_tape
    test_coof_fill_bar_exit_tapes
    test_coof_inflight_cancel_tapes
    test_coof_marketable_stop_tapes
    test_coof_open_limit_tapes
    test_coof_open_newborn_exit_tape
    test_coof_open_recalc_context_l4c
    test_coof_refill_waypoint_tapes
    test_coof_reissued_exit_tapes
    test_coof_resting_bracket_tapes
    test_coof_resting_stop_siblings_l4c
    test_coof_same_point_exits_tapes
    test_coof_timeframe_change_tapes
    test_coof_w2_refill_tapes
    test_coqueued_open_margin_tapes
    test_daily_magnifier_tapes
    test_dropped_reversal_mc_first_l4c
    test_dual_stop_transactions_l4d
    test_e19_excursion_tape
    test_entry_bar_margin_path_l4a
    test_exit_fifo_cross_tapes
    test_exit_queue_tapes
    test_exit_reissue_na_tapes
    test_famag_close_survives_l4d
    test_global_exit_children_tapes
    test_global_exit_fill_size_tapes
    test_grid_close_tapes
    test_htf_weekly_lookahead
    test_int29_rule_compositions
    test_intraday_cap_tv_tapes
    test_intraday_loss_open_fill_tape
    test_ki62_same_id_cover
    test_l10ab_same_bar_reentry_exit_level
    test_l10ac_full_equity_single_lot
    test_l10ag_reversal_close_cycle
    test_l10aj_margin_full_exit_flat
    test_l10al_reversal_single_lot
    test_l10am_entry_bar_priced_exit
    test_l10ao_percent_equity_first_lot
    test_l10ap_coof_chained_fill_price
    test_l10ar_priced_entry_fill
    test_l10as_priced_exit_fill
    test_l10b_excursion_parity
    test_l10d_entry_bar_excursion_masks
    test_l10e_opposite_entry_close_same_pass
    test_l10f_sizing_basis_stop_limit
    test_l10g_oca_reduce_deferred_flip
    test_l10h_corpus_parity
    test_l10j_corpus_parity
    test_l10k_corpus_parity
    test_l10l_corpus_parity
    test_l10m_corpus_parity
    test_l10p_three_way_entry_bar_exit
    test_l10q_pyramid_terrace_exit
    test_l10r_dual_stop_same_bar_order
    test_l10recon1_merge_fill_liveness
    test_l10s_pyramiding_same_bar_ids
    test_l10t_partial_exit_residual
    test_l10v_first_partial_leg_timing
    test_l10w_zero_qty_entry
    test_l10x_reissued_limit_touch_bar
    test_l10y_full_equity_entry_split
    test_l10z_review_fixes
    test_l11a_host_excursion_twin
    test_l11b_same_bar_stopout_excursion
    test_l4f_security_magnifier
    test_limit_entry_affordability_tape
    test_ltf_lookahead_first_bucket
    test_m_admission_36_l4d
    test_magnified_coof_refill_tape
    test_margin_residual_resolved_fill
    test_market_admission_commission_l4b
    test_native_auto_path
    test_native_host_repairs
    test_native_pending_order_v1_canary_l4d
    test_native_price_grid_twin
    test_native_selected_settlement
    test_native_settlement_projection
    test_nonfinite_entry_qty_tape
    test_open_entry_fee_charged
    test_pending_order_v1_canary_l4d
    test_percent_entry_reservation_tapes
    test_pine_dust_sweep_paired
    test_pooc_add_close_tape
    test_pooc_close_bar_fills_tapes
    test_pooc_margin_call_sizing_tapes
    test_pooc_pair_gross_tapes
    test_pooc_pending_stop_tapes
    test_pooc_same_pass_close_tape
    test_pooc_short_slip_admission_tapes
    test_pyramiding_open_order_tape
    test_same_bar_reissue_tapes
    test_same_point_entries_tapes
    test_samebar_pyramiding_entries_tapes
    test_session_transition_clock_tapes
    test_short_seed_report_swap
    test_small_money_margin_residual_l4a
    test_source_layer_native_defaults
    test_ta_indicators_extras
    test_ta_series_length_extremes
    test_ta_series_length_supertrend
    test_ta_simple_length
    test_ta_sma_stochrsi_ties
    test_ta_voltrend_edge
    test_ta_window_round10
    test_ta_window_underapplied
    test_trail_close_restart_no_fold_l4c
    test_trail_ref_entry_bar_extreme_l4c
    test_tv_fill_rounding_l4d
    test_tv_money_band_l4b
    test_tv_money_long_margin_call_eth_l4a
    test_tv_money_precision_l4b
    test_tv_strategy_defaults
    test_tvdef_drops_tapes
)

# The one list the kernel-only filter above and every guarded private block
# below both read: the private rows whose TU reaches the source layer,
# resolved by the same predicate (pineforge_test_is_source_bound), once. In a
# profile that does not build the source layer these rows register NOTHING --
# no target, no row, no placeholder, no label -- which is main's own
# kernel-only behaviour; every profile that builds the layer registers them
# through their private-fixture guard as today. The list is computed from the
# tree configure sees: in the fixture-less tree a private row whose public TU
# the removal took is listed through the recorded source-bound list that names
# it, and with the private fixtures overlaid its restored TU is read and listed.
set(_pf_kernel_absent_rows)
if(NOT PINEFORGE_BUILD_SOURCE_LAYER)
    foreach(_pf_kernel_row IN LISTS _pf_private_fixture_rows)
        pineforge_test_is_source_bound(_pf_kernel_bound
            "${CMAKE_CURRENT_SOURCE_DIR}/${_pf_kernel_row}.cpp")
        if(_pf_kernel_bound)
            list(APPEND _pf_kernel_absent_rows ${_pf_kernel_row})
        endif()
    endforeach()
    if(_pf_kernel_absent_rows)
        list(LENGTH _pf_kernel_absent_rows _pf_kernel_absent_count)
        message(STATUS
            "Kernel-only tests: ${_pf_kernel_absent_count} source-bound private "
            "rows not registered (PINEFORGE_BUILD_SOURCE_LAYER is OFF)")
    endif()
endif()

list(REMOVE_ITEM TEST_SOURCES ${_pf_private_fixture_rows})

# Report arithmetic and C-host failure ownership, with source-free includes.
list(APPEND TEST_SOURCES test_metrics_overflow)
list(APPEND TEST_SOURCES test_native_c_host_report_failure)
list(APPEND TEST_SOURCES test_native_unreachable_book)

# CR-150: engine-free pipe flag control beside the ordinary C++ unit rows.
if(UNIX)
    list(APPEND TEST_SOURCES test_pipe_writer_fd_flags)
endif()

foreach(test_name ${TEST_SOURCES})
    pineforge_add_unit_test(${test_name})
endforeach()

if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_executable(test_pending_observer_walk test_pending_observer_walk.cpp)
    target_link_libraries(test_pending_observer_walk PRIVATE pineforge)
    target_compile_options(test_pending_observer_walk PRIVATE -ffp-contract=off -UNDEBUG)
    if(PINEFORGE_ENABLE_COVERAGE)
        target_compile_options(test_pending_observer_walk PRIVATE ${_pf_cov_test_flags})
    endif()
    foreach(_pf_pending_walk_case IN ITEMS
            pending_walk_oracle pending_walk_cost pending_walk_growth)
        add_test(NAME ${_pf_pending_walk_case}
            COMMAND test_pending_observer_walk ${_pf_pending_walk_case})
    endforeach()
    # Match the P7/V19-E scaling rows: keep both workloads and correctness
    # checks everywhere, but gate the ratio and run serially only in Release.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        set_tests_properties(pending_walk_growth PROPERTIES
            ENVIRONMENT "PINEFORGE_PENDING_WALK_SCALING_GATE=0")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_tests_properties(pending_walk_cost pending_walk_growth PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()

# EN-134: each public C ABI fault run is also addressable for red-before and
# mutant checks. The aggregate and these rows run in the kernel-only profile.
foreach(_pf_settlement_row_case
        closed_runup_nan closed_drawdown_nan
        closed_runup_posinf closed_drawdown_posinf
        closed_runup_neginf closed_drawdown_neginf
        range_runup_nan range_drawdown_nan
        range_runup_posinf range_drawdown_posinf
        range_runup_neginf range_drawdown_neginf
        closed_pnl_pct_overflow range_pnl_pct_overflow)
    pineforge_add_twin_test(test_native_settlement_row_${_pf_settlement_row_case}
        test_native_settlement_row_finite ${_pf_settlement_row_case})
endforeach()
pineforge_add_twin_test(test_native_settlement_row_controls
    test_native_settlement_row_finite --controls)

# EN-132 round b: real one-lot fused runs, individually selectable. All
# round-a targets/rows retain their source-free kernel include closure.
foreach(_pf_one_lot_row_case
        one_lot_runup_nan one_lot_drawdown_nan
        one_lot_runup_posinf one_lot_drawdown_posinf
        one_lot_runup_neginf one_lot_drawdown_neginf
        one_lot_pnl_pct_overflow
        finite_one_lot_zero finite_one_lot_owned finite_one_lot_signed
        kernel_one_lot_normal)
    pineforge_add_twin_test(test_native_settlement_row_${_pf_one_lot_row_case}
        test_native_settlement_row_finite ${_pf_one_lot_row_case})
endforeach()

# The adapter rows live in the same TU as requested, compiled a second time
# with an explicit public source-header dependency. Only this target sees it;
# the native target above cannot include/link the source layer in any profile.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_executable(test_source_settlement_row_finite test_native_settlement_row_finite.cpp)
    target_link_libraries(test_source_settlement_row_finite PRIVATE pineforge)
    target_compile_options(test_source_settlement_row_finite PRIVATE -ffp-contract=off -UNDEBUG)
    if(PINEFORGE_ENABLE_COVERAGE)
        target_compile_options(test_source_settlement_row_finite PRIVATE ${_pf_cov_test_flags})
    endif()
    target_compile_definitions(test_source_settlement_row_finite PRIVATE
        "PINEFORGE_SETTLEMENT_SOURCE_HEADER=\"pineforge/source/pine_strategy_host.hpp\"")
    add_test(NAME test_source_settlement_row_finite COMMAND test_source_settlement_row_finite)
    foreach(_pf_source_settlement_row_case
            terminal_quote_pnl_pct_overflow terminal_quote_price_overflow
            finite_terminal_quote amended_trade_commission_overflow finite_amended_trade)
        pineforge_add_twin_test(test_source_settlement_row_${_pf_source_settlement_row_case}
            test_source_settlement_row_finite ${_pf_source_settlement_row_case})
    endforeach()
    pineforge_add_twin_test(test_source_settlement_row_controls
        test_source_settlement_row_finite --controls)
endif()

if(TARGET test_value_invalid_relative_risk_source)
    target_compile_definitions(test_value_invalid_relative_risk_source PRIVATE
        PINEFORGE_VALUE_INVALID_FIXTURES)
endif()

# EN-132 G2: value-injection fixtures. The fee, slippage and counter sites are
# not reachable from any public input, so this opt-in build compiles the
# library with PINEFORGE_VALUE_INVALID_FIXTURES (injection hooks that exist
# only under that macro) and registers the test that drives them. The default
# profiles never define it, so their archives carry no hook.
option(PINEFORGE_VALUE_INVALID_FIXTURES "Build EN-132 value-injection fixtures" OFF)
if(PINEFORGE_VALUE_INVALID_FIXTURES)
    target_compile_definitions(pineforge PRIVATE PINEFORGE_VALUE_INVALID_FIXTURES)
    if(TARGET pineforge_kernel)
        target_compile_definitions(pineforge_kernel PRIVATE PINEFORGE_VALUE_INVALID_FIXTURES)
    endif()
    pineforge_add_unit_test(test_value_invalid_fee_slip_settle)
    target_compile_definitions(test_value_invalid_fee_slip_settle PRIVATE
        PINEFORGE_VALUE_INVALID_FIXTURES)
    if(NOT PINEFORGE_BUILD_SOURCE_LAYER)
        target_compile_definitions(test_value_invalid_fee_slip_settle PRIVATE
            PINEFORGE_VALUE_INVALID_KERNEL_ONLY)
    endif()
endif()

# EN-132 M: source-only ownership proof, controls, and an independent D4 route.
add_test(NAME test_selected_window_ownership_manifest
    COMMAND ${Python3_EXECUTABLE} -I -B ${PROJECT_SOURCE_DIR}/scripts/check_selected_window_ownership.py)
# CR-196: lab release/debug/sanitizers/kernel took 57.0/65.9/85.5/56.9 s.
# Loaded pool release/kernel/native-live took 125.7/126.8/132.3 s; debug and
# sanitizers timed out at 180.02 s. Four shards keep the same 180 s budget.
# The cases are unequal: the heaviest shard (3) holds 15 of the 39 checker
# runs. ESTIMATE, not measured: 132.3 s x 15/39 = 50.9 s on the loaded pool in
# Release; scaling by the lab sanitizers/release ratio (85.5/57.0 = 1.5) gives
# about 76 s for sanitizers, i.e. 2.4x headroom against 180 s. The lab run of
# the four shard rows is the measurement; each shard also fails closed when it
# runs a number of tests different from the number it selected.
set(_pf_ownership_control_shards)
foreach(_pf_ownership_shard 1 2 3 4)
    add_test(NAME test_selected_window_ownership_manifest_controls_shard_${_pf_ownership_shard}of4
        COMMAND ${Python3_EXECUTABLE} -I -B ${PROJECT_SOURCE_DIR}/tests/ownership/test_manifest_checker.py
            --shard ${_pf_ownership_shard}/4)
    list(APPEND _pf_ownership_control_shards
        test_selected_window_ownership_manifest_controls_shard_${_pf_ownership_shard}of4)
endforeach()
add_test(NAME test_selected_window_ownership_d4
    COMMAND ${Python3_EXECUTABLE} -I -B ${PROJECT_SOURCE_DIR}/tests/ownership/test_manifest_checker.py
        ManifestCheckerTests.test_d4_independent_reconstruction_matches_manifest)
set_tests_properties(test_selected_window_ownership_manifest
    ${_pf_ownership_control_shards} test_selected_window_ownership_d4
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# The C bridge test also links the shared window-plan helper, whose bridge
# (src/selected_window_plan_c.cpp) is not in the kernel archive. The loop above
# supplies pineforge and the -ffp-contract=off and -UNDEBUG flags.
target_link_libraries(test_selected_window_plan_c PRIVATE pineforge_window_plan)

# R5 PERF-L4: the witness's two references. The source layer is compiled
# twice more, with the kernel archive below it: once with every quiet-bar gate
# compiled out (PINEFORGE_PINE_QUIET_BAR_GATES=0), once gated; both carry the
# gate probe (src/source/pine_quiet_bar.hpp). The differential row requires
# the shipped library's transcript to be the ungated one, line for line; the
# probe row requires a quiet bar to run no hook body and every gate to be
# taken both ways across the battery. Neither archive is installed.
if(PINEFORGE_BUILD_SOURCE_LAYER AND TARGET test_adapter_quiet_bar)
    get_target_property(_pf_quiet_library_options pineforge COMPILE_OPTIONS)
    set(_pf_quiet_sources)
    foreach(_pf_quiet_source IN LISTS PINEFORGE_SOURCE_LAYER_SOURCES)
        list(APPEND _pf_quiet_sources ${PROJECT_SOURCE_DIR}/${_pf_quiet_source})
    endforeach()
    foreach(_pf_quiet_variant ungated probe)
        set(_pf_quiet_gates 1)
        if(_pf_quiet_variant STREQUAL "ungated")
            set(_pf_quiet_gates 0)
        endif()
        set(_pf_quiet_library pineforge_source_quiet_${_pf_quiet_variant})
        add_library(${_pf_quiet_library} STATIC ${_pf_quiet_sources})
        target_compile_options(${_pf_quiet_library} PRIVATE ${_pf_quiet_library_options})
        target_include_directories(${_pf_quiet_library} PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(${_pf_quiet_library} PRIVATE
            PINEFORGE_PINE_QUIET_BAR_GATES=${_pf_quiet_gates}
            PINEFORGE_PINE_QUIET_BAR_PROBE=1)
        target_link_libraries(${_pf_quiet_library} PUBLIC pineforge_kernel)
        set(_pf_quiet_test test_adapter_quiet_bar_${_pf_quiet_variant})
        add_executable(${_pf_quiet_test} test_adapter_quiet_bar.cpp)
        target_link_libraries(${_pf_quiet_test} PRIVATE ${_pf_quiet_library})
        target_compile_definitions(${_pf_quiet_test} PRIVATE
            PINEFORGE_QUIET_BAR_WITNESS_PROBE
            PINEFORGE_PINE_QUIET_BAR_GATES=${_pf_quiet_gates}
            PINEFORGE_PINE_QUIET_BAR_PROBE=1)
        target_compile_options(${_pf_quiet_test} PRIVATE -ffp-contract=off -UNDEBUG)
        if(PINEFORGE_ENABLE_COVERAGE)
            target_compile_options(${_pf_quiet_test} PRIVATE ${_pf_cov_test_flags})
        endif()
    endforeach()
    add_test(NAME test_adapter_quiet_bar_probe COMMAND test_adapter_quiet_bar_probe)
    add_test(NAME test_adapter_quiet_bar_differential
        COMMAND ${CMAKE_COMMAND}
            -DGATED=$<TARGET_FILE:test_adapter_quiet_bar>
            -DUNGATED=$<TARGET_FILE:test_adapter_quiet_bar_ungated>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/quiet_bar_differential
            -P ${CMAKE_CURRENT_SOURCE_DIR}/quiet_bar_differential.cmake)
endif()

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
    if(EXISTS ${_pf_manifest_v16_frozen})
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
        set_tests_properties(test_l4g_runtime_budget PROPERTIES SKIP_RETURN_CODE 77)
    else()
        add_test(NAME test_l4g_runtime_budget COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/native_cpp_abi/host-ab9714b/manifest.json")
        set_tests_properties(test_l4g_runtime_budget PROPERTIES SKIP_RETURN_CODE 77 SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

add_test(NAME test_l4g_runtime_budget_mutations
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_runtime_budget.py)

# R5 lane PERF-P7: the lookup-index scaling row's ratio is a Release-only
# gate, as the runtime budget's is. A non-Release library asserts every
# indexed lookup equal to the walk it replaces (pine_adapter.cpp,
# native_order.cpp), which restores the walk's cost by design; there the row
# still runs both workloads and checks what they produced.
if(TARGET test_adapter_lookup_index_scaling AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    set_tests_properties(test_adapter_lookup_index_scaling PROPERTIES
        ENVIRONMENT "PINEFORGE_P7_SCALING_GATE=0")
endif()
# R5 lane V19-E: the same Release-only gate for the recording-mode ratio.
if(TARGET test_adapter_live_state_scaling AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    set_tests_properties(test_adapter_live_state_scaling PROPERTIES
        ENVIRONMENT "PINEFORGE_V19E_SCALING_GATE=0")
endif()
# The lookup-index and live-state scaling rows run alone in Release, where
# their ratios gate (the blocks above turn the gate off elsewhere). Under a
# parallel ctest, other rows load the box and a best-of-rounds ratio can
# favour the short leg: the lookup-index row once read 6.15 against its 5.0
# bound under load, and 4.08 when run by itself. The other rows built on
# tests/ratio_timing.hpp stay parallel.
foreach(_pf_ratio_test test_adapter_lookup_index_scaling test_adapter_live_state_scaling)
    if(TARGET ${_pf_ratio_test} AND CMAKE_BUILD_TYPE STREQUAL "Release")
        set_tests_properties(${_pf_ratio_test} PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()


# Contract §3.4 / deferred-ANY pin witnesses retain their historical CTest
# names while their executable twins make the switched route explicit.
pineforge_add_twin_test(test_live_order_derived test_native_live_order_derived_l4d)
pineforge_add_twin_test(test_source_fifo_endpoints
                        test_native_source_fifo_endpoints_l4d)
# The pending_order_v1 canary twin cannot register here: its base target is a
# private-fixture row, created by the guarded block below (after the label
# clear), because pineforge_add_twin_test registers only an existing target.

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

# The pending_order_v1 canary is one of the restored twins above, but its base
# TU (#include of fixtures/pending_order_prefix/c45-fields.inc) was removed
# with the private fixtures, so the base target and the twin register together
# inside the base's own guard -- the test_oracle_day_key shape. The block sits
# after the label clear above because the pinned SKIP placeholder the absent
# branch registers must keep the private_fixtures label the pin file records
# (tests/private_fixture_rows.txt), while the real base row, registered before
# the clear in main, carries no pending-policy label at all.
if(NOT test_native_pending_order_v1_canary_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pending_order_prefix/c45-fields.inc")
    pineforge_add_unit_test(test_native_pending_order_v1_canary_l4d)
    pineforge_add_twin_test(test_pending_order_v1_canary test_native_pending_order_v1_canary_l4d)
elseif(NOT test_native_pending_order_v1_canary_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_pending_order_v1_canary_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pending_order_prefix/c45-fields.inc")
    set_tests_properties(test_native_pending_order_v1_canary_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_pending_order_v1_canary COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pending_order_prefix/c45-fields.inc")
    set_tests_properties(test_pending_order_v1_canary PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

# R4-D L0 carriers are included verbatim by their switched-route twin targets.
# There is deliberately no ORACLE_TEST_SOURCES list: a source list that does
# not build targets is not executable oracle coverage.
pineforge_add_twin_test(test_oracle_deferred_birth test_native_oracle_deferred_birth_l2)
pineforge_add_twin_test(test_oracle_relative_exit test_native_oracle_relative_exit_l2)
pineforge_add_twin_test(test_oracle_reversal_close_only
                        test_native_oracle_reversal_close_only_l2)
if(NOT test_native_oracle_reversal_same_bar_tx_l2 IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/oracle/test_oracle_reversal_same_bar_tx.cpp")
    pineforge_add_unit_test(test_native_oracle_reversal_same_bar_tx_l2)
    pineforge_add_twin_test(test_oracle_reversal_same_bar_tx test_native_oracle_reversal_same_bar_tx_l2)
elseif(NOT test_native_oracle_reversal_same_bar_tx_l2 IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_oracle_reversal_same_bar_tx_l2 COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/oracle/test_oracle_reversal_same_bar_tx.cpp")
    set_tests_properties(test_native_oracle_reversal_same_bar_tx_l2 PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_oracle_reversal_same_bar_tx COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/oracle/test_oracle_reversal_same_bar_tx.cpp")
    set_tests_properties(test_oracle_reversal_same_bar_tx PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()
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
if(NOT test_native_oracle_day_key_l2 IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/oracle/test_oracle_day_key.cpp")
    pineforge_add_unit_test(test_native_oracle_day_key_l2)
    pineforge_add_twin_test(test_oracle_day_key test_native_oracle_day_key_l2)
elseif(NOT test_native_oracle_day_key_l2 IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_oracle_day_key_l2 COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/oracle/test_oracle_day_key.cpp")
    set_tests_properties(test_native_oracle_day_key_l2 PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_oracle_day_key COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/oracle/test_oracle_day_key.cpp")
    set_tests_properties(test_oracle_day_key PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()
# The frozen-size twin is NOT a private-fixture row: its base
# (test_native_oracle_frozen_size_full_l2) is a public TEST_SOURCES row whose
# sources read no margin_call_rules file, so main registers this twin
# unconditionally and the row runs -- not skips -- on the fixture-less tree
# (the guard it used to carry named a file neither of its sources reads).
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
# test_oracle_stop_snapshot is registered with its base target below (the
# block after the label clear): the twin's base TU is a private-fixture row,
# so pineforge_add_twin_test cannot see its target here.

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

# A20 stop-snapshot twin: both its base TU (test_native_oracle_stop_snapshot_
# full_l2.cpp, which #includes oracle/test_oracle_stop_snapshot.cpp, a removed
# private fixture) and the twin register inside the base's own guard -- the
# test_oracle_day_key shape. The block sits after the label clear above for
# the same reason as the canary block: the absent branch's pinned placeholder
# must keep the private_fixtures label the pin file records, and the clear
# list above names both rows because main registers them earlier.
if(NOT test_native_oracle_stop_snapshot_full_l2 IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_oracle_stop_snapshot_full_l2.cpp")
    pineforge_add_unit_test(test_native_oracle_stop_snapshot_full_l2)
    pineforge_add_twin_test(test_oracle_stop_snapshot test_native_oracle_stop_snapshot_full_l2)
elseif(NOT test_native_oracle_stop_snapshot_full_l2 IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_oracle_stop_snapshot_full_l2 COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_oracle_stop_snapshot_full_l2.cpp")
    set_tests_properties(test_native_oracle_stop_snapshot_full_l2 PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_oracle_stop_snapshot COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_oracle_stop_snapshot_full_l2.cpp")
    set_tests_properties(test_oracle_stop_snapshot PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()
add_test(NAME test_oracle_twin_census
         COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_oracle_twin_census.py)
add_test(NAME test_oracle_sha256_pin
         COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_oracle_sha256.py)

target_compile_definitions(test_native_market_vertical PRIVATE
    PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256="${PINEFORGE_NATIVE_MARKET_VERTICAL_SHA256}")

# R5 L3 adapter-neutrality witness: test_native_sizing_bases reads the source
# layer and forbids the scope-reduction value kinds outside the kernel codec's
# payload_fields body in exit_scratch_versions_production.cpp (H2-11).
# '|' separates the paths because ';' would split the compile definition.
file(GLOB_RECURSE PINEFORGE_SOURCE_LAYER_FILES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/source/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/source/*.hpp"
     "${PROJECT_SOURCE_DIR}/src/compat/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/compat/*.hpp"
     "${PROJECT_SOURCE_DIR}/include/pineforge/source/*.hpp"
     "${PROJECT_SOURCE_DIR}/include/pineforge/compat/*.hpp")
# Keep the codec translation unit in the witness. Only its payload_fields
# dispatcher may name those alternatives; all surrounding source is checked.
list(LENGTH PINEFORGE_SOURCE_LAYER_FILES PINEFORGE_SOURCE_LAYER_COUNT)
if(PINEFORGE_SOURCE_LAYER_COUNT LESS 4)
    message(FATAL_ERROR "source-layer neutrality glob found no translation units")
endif()
string(REPLACE ";" "|" PINEFORGE_SOURCE_LAYER_JOINED "${PINEFORGE_SOURCE_LAYER_FILES}")
target_compile_definitions(test_native_sizing_bases PRIVATE
    PINEFORGE_SOURCE_LAYER_FILES="${PINEFORGE_SOURCE_LAYER_JOINED}")

# std::thread needs an explicit pthread link on some toolchains.
target_link_libraries(test_timezone_concurrency PRIVATE Threads::Threads)
target_link_libraries(test_local_time_lock_free_hit PRIVATE Threads::Threads)
target_link_libraries(test_native_runtime_ambient PRIVATE Threads::Threads)

# The LIFO row's expectation is the library's build type. The library's
# NDEBUG, which compiles uninstall_runtime_ambient's order check out, comes
# from CMake's build-type flags -- Release, RelWithDebInfo and MinSizeRel
# define it; Debug, the debug and sanitizers profiles' type, does not -- and
# the row's TU strips NDEBUG like every test's (-UNDEBUG above), so it is
# told. Flags that disagree with the build type fail the row; none passes it
# without the check.
target_compile_definitions(test_native_runtime_ambient_lifo PRIVATE
    PINEFORGE_LIBRARY_DEBUG_CHECKS=$<CONFIG:Debug>)

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

# Lane REL10: the four spellings ADR-0001 "Deprecated public spellings"
# scheduled for removal -- pf_equity_stats_t::sharpe_tv / sortino_tv and
# exit_legs::Domain::Coof / MagnifierCoof -- are gone at 1.0. The row compiles
# their 1.0 spellings clean (-Werror) in strict C99 and in C++17, and each
# removed spelling alone, which must fail with the unknown-member diagnostic
# that names it (R5 lane F6 registered this row as the deprecation check).
add_test(NAME test_removed_public_spellings
    COMMAND ${CMAKE_COMMAND}
        -DPF_CC=${CMAKE_C_COMPILER}
        -DPF_CXX=${CMAKE_CXX_COMPILER}
        "-DPF_INCLUDES=${PROJECT_SOURCE_DIR}/include|${PROJECT_BINARY_DIR}/include"
        -DPF_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DPF_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/removed_spellings
        -P ${CMAKE_CURRENT_SOURCE_DIR}/removed_spellings.cmake)

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
# The row has no private input, so its coverage flags sit outside the frozen-
# header guard below: a fixture-absent coverage build instruments it too.
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_native_c_api PRIVATE ${_pf_cov_test_flags})
endif()

# The frozen v1 header closure, compiled against the current runtime.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native_c_api/v1/native_c_api.h")
add_executable(test_native_c_api_frozen_header test_native_c_api_frozen_header.cpp)
target_link_libraries(test_native_c_api_frozen_header PRIVATE pineforge)
target_compile_options(test_native_c_api_frozen_header PRIVATE -UNDEBUG -ffp-contract=off)

if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_native_c_api_frozen_header PRIVATE ${_pf_cov_test_flags})
endif()
else()
    add_test(NAME test_native_c_api_frozen_header COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/native_c_api/v1/native_c_api.h")
    set_tests_properties(test_native_c_api_frozen_header PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

pineforge_add_twin_test(test_native_c_api test_native_c_api)
pineforge_add_twin_test(test_native_c_api_frozen_header test_native_c_api_frozen_header)

# C-SURFACE-1: an INT23 object compiled with the db98990c C header closure
# must receive the decision tail its 168-byte policy-hook table published.
# The fixture include path precedes the live headers for this target only.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native_c_api/int23/pineforge/native_c_api.h")
add_executable(test_native_c_api_int23_header test_native_c_api_int23_header.c)
target_include_directories(test_native_c_api_int23_header BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native_c_api/int23)
set_target_properties(test_native_c_api_int23_header PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(test_native_c_api_int23_header PRIVATE pineforge)
target_compile_options(test_native_c_api_int23_header PRIVATE -UNDEBUG)
else()
    add_test(NAME test_native_c_api_int23_header COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/native_c_api/int23/pineforge/native_c_api.h")
    set_tests_properties(test_native_c_api_int23_header PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()
pineforge_add_twin_test(test_native_c_api_int23_header test_native_c_api_int23_header)

# R5 lane F4: the public C headers as a strict C99 consumer compiles them --
# extensions off and -pedantic-errors, so any C11-only construct in
# <pineforge/pineforge.h> or <pineforge/native_c_api.h> is a build failure.
# The one C11 construct they held, the anonymous union behind P2c's
# sharpe_tv / sortino_tv aliases, left with those spellings at 1.0 (lane
# REL10). The row checks the equity offsets and executes one native C run.
add_executable(test_native_c_api_c99 test_native_c_api_c99.c)
set_target_properties(test_native_c_api_c99 PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
target_link_libraries(test_native_c_api_c99 PRIVATE pineforge)
target_compile_options(test_native_c_api_c99 PRIVATE -UNDEBUG -pedantic-errors -Wall -Wextra)
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_native_c_api_c99 PRIVATE ${_pf_cov_test_flags})
endif()
pineforge_add_twin_test(test_native_c_api_c99 test_native_c_api_c99)

# Recorded outputs: group pf_outputs of <pineforge/pineforge.h> (its runtime
# readers and the per-library declarations) as a strict C99 consumer compiles
# it, the event record's layout pinned at compile time, and a native C host's
# handle answering the readers as a module that declares no outputs.
add_executable(test_outputs_c_api_c99 test_outputs_c_api_c99.c)
set_target_properties(test_outputs_c_api_c99 PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
target_link_libraries(test_outputs_c_api_c99 PRIVATE pineforge)
target_compile_options(test_outputs_c_api_c99 PRIVATE -UNDEBUG -pedantic-errors -Wall -Wextra)
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_outputs_c_api_c99 PRIVATE ${_pf_cov_test_flags})
endif()
pineforge_add_twin_test(test_outputs_c_api_c99 test_outputs_c_api_c99)

# R5 lane H-MEASURE (AUDIT4-codex A4-ACID-COVERAGE): the G1 composite witness.
# One ctest row, two translation units, the test_native_c_api pattern. The C++
# half drives one Pine-free NativeStrategyHost through every feature the G1
# acid test names (a PASS line per feature) in batch and in stream, runs the
# keep_binding variant and a lower_tf-magnified batch, and accounts for the
# driving-mode hash rules line by line. The C half drives the same commands
# through <pineforge/native_c_api.h> alone, and the C++ half compares the two
# FIELD sets in process: every shared ledger fact identical, every C++-only
# family named by an EXCLUDED line. Neither TU reaches a source or compat
# header, so the row runs in the kernel-only profile too.
add_executable(test_native_acid_composite
    test_native_acid_composite.cpp
    test_native_acid_composite_c.c)
target_compile_definitions(test_native_acid_composite PRIVATE ACID_WITH_C=1 ACID_C_NO_MAIN=1)
target_link_libraries(test_native_acid_composite PRIVATE pineforge)
target_compile_options(test_native_acid_composite PRIVATE -UNDEBUG -ffp-contract=off)
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_native_acid_composite PRIVATE ${_pf_cov_test_flags})
endif()
pineforge_add_twin_test(test_native_acid_composite test_native_acid_composite)

add_test(NAME test_market_admission_schema COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/scripts/check_market_admission_schema.py)
add_test(NAME test_market_admission_schema_mutations COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/scripts/test_market_admission_schema.py)
add_test(NAME test_cancellation_hash_coverage COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_cancellation_hash_coverage.py)
add_test(NAME test_native_live_equivalence_harness COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_native_live_equivalence_harness.py)
add_test(NAME test_pine_input_domain_gates COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_pine_input_domain_gates.py)
add_test(NAME test_native_confirmed_bar_stream_comparator COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_native_confirmed_bar_stream_scan.py)

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
# PR 1b class 1: no target the kernel profile builds links without the source
# layer -- every guarded private block reads the kernel-only filter's own
# predicate list, the same predicate the filter calls (single source), so a
# source-bound private row registers nothing there, in either fixture mode;
# the second row is the sweep's own controls, which must close on the tree and
# fail loudly on a construct it cannot read.
add_test(NAME test_kernel_profile_rows COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_kernel_profile_rows.py)
add_test(NAME test_kernel_profile_rows_controls COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_check_kernel_profile_rows.py)
# The private-fixture class sweeps: every registered row that names removed
# data is pinned or guarded, no Python row reads an absent private file, and
# the two oracle tree checks keep their controls. They only read the tree, so
# they register in every profile and in both fixture modes.
add_test(NAME test_private_fixture_reads COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_private_fixture_reads.py)
# The controls of both sweeps: each runs the REAL sweep script on tampered
# copies of the tree under a temporary root, so the sweeps' own proof runs in
# CI instead of surviving only in a lane's scratch. They build their tamper
# trees through tempfile.TemporaryDirectory, which honours TMPDIR, so the
# environment pins TMPDIR to a configure-time directory under the build dir
# instead of letting the rows write into the host's /tmp.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
add_test(NAME test_private_fixture_reads_controls COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_private_fixture_reads.py)
set_tests_properties(test_private_fixture_reads_controls PROPERTIES
    ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp")
add_test(NAME test_kernel_python_private_reads COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_kernel_python_private_reads.py)
# The in-tree two-world registration invariant: the absent world is evaluated
# on every tree (its evaluation, the recorded source-bound list and the
# recorded refusal-shard list against the pin); the overlay half compares both
# worlds and runs the drift guards, and where the overlay is not present it
# prints `overlay half not run: private fixtures not present: <marker>` and
# exits 0. The row never skips, so it carries no SKIP_RETURN_CODE; with
# PINEFORGE_PRIVATE_FIXTURES set and the overlay absent it fails. It names no
# private fixture in this file, so it is no pinned placeholder row.
add_test(NAME test_two_world_registration COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/check_two_world_registration.py --both)
# (The row deliberately carries no set_tests_properties: no skip code.)
add_test(NAME test_two_world_registration_controls COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_two_world_registration.py)
set_tests_properties(test_two_world_registration_controls PROPERTIES
    ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp")
add_test(NAME test_oracle_fixture_checks COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_oracle_fixture_checks.py)
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
# R5 lane H-DOCGATES (AUDIT4-opus X13): the doc-revert gate's must-fail suite,
# over throwaway git repositories; the gate itself is a ci_preflight stage.
add_test(NAME test_doc_reverts COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_check_doc_reverts.py)
# R5 lane H-DOCGATES (AUDIT4-opus X10): the kernel seam-row gate's must-fail
# suite, over throwaway trees; the gate itself is a ci_preflight stage.
add_test(NAME test_kernel_seam_rows COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/scripts/test_check_kernel_seam_rows.py)

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

# DOC-TRUTH-4: compile the guide's Complete host block verbatim. The CTest
# wrapper also checks that the built source still matches the live page.
set(_pf_ne_page ${PROJECT_SOURCE_DIR}/docs/pages/native-engine.md)
set(_pf_ne_complete ${CMAKE_CURRENT_BINARY_DIR}/native_engine_complete_host.cpp)
add_custom_command(
    OUTPUT ${_pf_ne_complete}
    COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/extract_native_engine_complete_host.py
            --page ${_pf_ne_page} --out ${_pf_ne_complete}
    DEPENDS ${_pf_ne_page} ${CMAKE_CURRENT_SOURCE_DIR}/extract_native_engine_complete_host.py
    COMMENT "Extracting Complete host from docs/pages/native-engine.md"
    VERBATIM)
add_executable(test_native_engine_complete_host ${_pf_ne_complete})
target_link_libraries(test_native_engine_complete_host PRIVATE PineForge::kernel)
target_compile_options(test_native_engine_complete_host PRIVATE -ffp-contract=off -UNDEBUG)
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_native_engine_complete_host PRIVATE ${_pf_cov_test_flags})
endif()
add_test(NAME test_native_engine_complete_host
    COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/extract_native_engine_complete_host.py
            --page ${_pf_ne_page} --out ${_pf_ne_complete}
            --run $<TARGET_FILE:test_native_engine_complete_host>)

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
    ${PINEFORGE_CI_VERIFY_SLICES}
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

# R5 lane V19-D: the adapter re-issues with a carried binding
# (ReplaceOptions::keep_binding). The V19-E battery's randomized Pine scripts,
# each run carrying and with plain re-issues: every transcript but its
# CloseBoundEvents matches bar for bar -- command events with their ordinals,
# pending-order rows, working requests, position, equity, trades, the equity
# curve, the admission journal, the rows placed, erased and retained -- and
# carrying spares CloseBoundEvents. Source-bound.
if(PINEFORGE_BUILD_SOURCE_LAYER AND TARGET test_adapter_live_state_equivalence)
    add_test(NAME test_adapter_reissue_binding
             COMMAND test_adapter_live_state_equivalence --reissue-binding)
endif()
# R5 lane V19-D: a margin revival reads the same legs whatever the sweep
# erased -- a short's exit held dormant by a dropped reversal pair,
# cancelled and re-placed at other stops, then a margin call: the revival
# skips it past an erased second re-placement (K3) or after K1 released it,
# and restores it when nothing supersedes it, each run equal to its
# every-row-retained twin; plus seeded runs of the same moves. Source-bound.
if(PINEFORGE_BUILD_SOURCE_LAYER AND TARGET test_adapter_live_state_equivalence)
    add_test(NAME test_adapter_margin_revival_erasure
             COMMAND test_adapter_live_state_equivalence --margin-revival)
    # The fragments header is generated from the retirement cursor walks of pine_adapter.cpp by a regex over their
    # formatting (see the header of retirement_cursor_fragments.py): reformatting a walk fails this build with an
    # #error until the generator follows.
    set(_pf_retirement_fragments "${CMAKE_CURRENT_BINARY_DIR}/retirement_cursor_fragments.hpp")
    add_custom_command(OUTPUT "${_pf_retirement_fragments}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/retirement_cursor_fragments.py"
            "${PROJECT_SOURCE_DIR}/src/source/pine_adapter.cpp" "${_pf_retirement_fragments}"
        DEPENDS retirement_cursor_fragments.py "${PROJECT_SOURCE_DIR}/src/source/pine_adapter.cpp"
        VERBATIM)
    target_sources(test_adapter_live_state_equivalence PRIVATE "${_pf_retirement_fragments}")
    target_include_directories(test_adapter_live_state_equivalence PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME test_adapter_retirement_cursors
        COMMAND test_adapter_live_state_equivalence --retirement-cursors)
endif()

# PR CI-LITE timing selection: INT23 sanitizer job 107895095058 and INT24
# sanitizer/Debug jobs 107996509646, 107996511076, 107996511419.
# A row is slow when it exceeded 60 s in either sanitizer run or 30 s in
# either Debug run. This is the single home of the PR exclusion population;
# full push, dispatch, and maintainer verification still register and run it.
set(PINEFORGE_PR_SLOW_TESTS
    test_adapter_live_state_equivalence
    test_adapter_live_state_scaling
    test_adapter_reissue_binding
    test_aggregate_cpp_versions
    test_broker_state_hash_coverage_mutations
    test_chart_day_memo
    test_ci_verify_slice_1
    test_ci_verify_slice_2
    test_ci_verify_slice_3
    test_ci_verify_slice_4
    test_ci_verify_slice_5
    test_ci_verify_slice_6
    test_ci_verify_slice_7
    test_ci_verify_slice_8
    test_l10recon1_merge_fill_liveness
    test_local_time_fields
    test_magnifier_endpoints4
    test_native_bar_open_hook
    test_native_command_after_slice_1
    test_native_command_after_slice_2
    test_native_command_after_slice_3
    test_native_command_after_slice_4
    test_native_command_after_slice_5
    test_native_command_after_slice_6
    test_native_command_after_slice_7
    test_native_command_after_slice_8
    test_native_cpp_abi
    test_native_cpp_versions
    test_native_definition_index
    test_native_direct_mutation
    test_native_event_retention
    test_native_group_absorption
    test_native_group_keep_handle
    test_native_handle_stable_replace
    test_native_host_reads
    test_native_lookup_memos
    test_native_match_band_precheck
    test_native_match_row_reuse
    test_native_precommit_hook
    test_native_quiet_point
    test_native_runtime_ambient
    test_native_script_bucket_completions
    test_native_settlement_carry
)

# The guard and the slow label for PINEFORGE_PR_SLOW_TESTS run near the end of
# this file, after the guarded private-fixture re-registrations: a slow row
# whose source or fixture the private-fixture PR removes (test_local_time_fields)
# is registered by its own conditional block, not by the TEST_SOURCES loop, so
# the guard must run once that block has run.

# Stable run-failure codes (include/pineforge/run_failure.hpp) through real
# generated strategies and the C ABI. tests/fixtures/run_failure_codes holds
# each case's Pine source, its frozen codegen output and its provenance;
# tests/run_failure_codes_strategies.hpp compiles every case into this one
# translation unit, each in a namespace of its own with its C entry points
# prefixed. The test also reads the published catalog
# (docker/run_failure_codes.json) and holds the library's registry to it.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    # tests/run_failure_codes_strategies.hpp #includes every case's
    # generated.cpp -- helper_stops among them -- so the target compiles only
    # while that include does; the row is the pinned placeholder otherwise.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/run_failure_codes/helper_stops/generated.cpp")
        add_executable(test_run_failure_codes test_run_failure_codes.cpp)
        target_link_libraries(test_run_failure_codes PRIVATE pineforge)
        target_compile_options(test_run_failure_codes PRIVATE -UNDEBUG -ffp-contract=off
            -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter
            -Wno-unused-function -Wno-sign-compare)
        target_compile_definitions(test_run_failure_codes PRIVATE
            PINEFORGE_RUN_FAILURE_CATALOG="${PROJECT_SOURCE_DIR}/docker/run_failure_codes.json")
        if(PINEFORGE_ENABLE_COVERAGE)
            target_compile_options(test_run_failure_codes PRIVATE ${_pf_cov_test_flags})
        endif()
        add_test(NAME test_run_failure_codes COMMAND test_run_failure_codes)
    else()
        add_test(NAME test_run_failure_codes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/run_failure_codes/helper_stops/generated.cpp")
        set_tests_properties(test_run_failure_codes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

# H2-3: this MODULE loads into Python, so its row shares the profile fence.
if(PINEFORGE_BUILD_SOURCE_LAYER AND PINEFORGE_BUILD_LIVE_RUNNER
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/same_calc_bracket/generated.cpp")
    add_library(same_calc_bracket_strategy MODULE
        fixtures/same_calc_bracket/generated.cpp
        $<TARGET_OBJECTS:native_live_equivalence_observer>)
    target_link_libraries(same_calc_bracket_strategy PRIVATE pineforge)
    target_compile_options(same_calc_bracket_strategy PRIVATE -ffp-contract=off)
    if(APPLE)
        target_link_options(same_calc_bracket_strategy PRIVATE "LINKER:-u,_pf_abi_version")
    else()
        target_link_options(same_calc_bracket_strategy PRIVATE "LINKER:-u,pf_abi_version")
    endif()
    set_target_properties(same_calc_bracket_strategy PROPERTIES PREFIX "")
    add_test(NAME confirmed_capabilities_same_calc_bracket_e2e COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/same_calc_bracket_e2e.py $<TARGET_FILE:pineforge-live>
        $<TARGET_FILE:native_live_report_oracle> $<TARGET_FILE:same_calc_bracket_strategy>)
    else()
        add_test(NAME confirmed_capabilities_same_calc_bracket_e2e COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/same_calc_bracket/generated.cpp")
        set_tests_properties(confirmed_capabilities_same_calc_bracket_e2e PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()

if(PINEFORGE_BUILD_SOURCE_LAYER AND PINEFORGE_BUILD_LIVE_RUNNER)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_live_observer_exit_order.cpp")
    add_executable(test_native_live_observer_exit_order test_native_live_observer_exit_order.cpp
        $<TARGET_OBJECTS:native_live_equivalence_observer>)
    target_link_libraries(test_native_live_observer_exit_order PRIVATE pineforge)
    target_include_directories(test_native_live_observer_exit_order PRIVATE ${PROJECT_SOURCE_DIR}/runner)
    target_compile_options(test_native_live_observer_exit_order PRIVATE -UNDEBUG -ffp-contract=off)
    add_test(NAME test_native_live_observer_exit_order COMMAND test_native_live_observer_exit_order)
    else()
        add_test(NAME test_native_live_observer_exit_order COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_live_observer_exit_order.cpp")
        set_tests_properties(test_native_live_observer_exit_order PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    endif()
endif()
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_executable(test_lv_d8_tick_stop_activation test_lv_d8_tick_stop_activation.cpp)
    target_link_libraries(test_lv_d8_tick_stop_activation PRIVATE pineforge)
    target_compile_options(test_lv_d8_tick_stop_activation PRIVATE -UNDEBUG -ffp-contract=off)
    add_test(NAME test_lv_d8_tick_stop_activation COMMAND test_lv_d8_tick_stop_activation)
    if(PINEFORGE_BUILD_LIVE_RUNNER)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lv_d8_stop_activation/generated.cpp")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_library(lv_d8_stop_strategy SHARED fixtures/lv_d8_stop_activation/generated.cpp
            lv_d8_candidate_observer.cpp $<TARGET_OBJECTS:native_live_equivalence_observer>)
        target_link_libraries(lv_d8_stop_strategy PRIVATE pineforge)
        target_compile_options(lv_d8_stop_strategy PRIVATE -ffp-contract=off)
        if(APPLE)
            target_link_options(lv_d8_stop_strategy PRIVATE "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        elseif(UNIX)
            target_link_options(lv_d8_stop_strategy PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        set(_pf_d8_equivalence_command)
        if(PINEFORGE_ENABLE_SANITIZERS)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                set(_pf_d8_runtime_name libasan.so)
            elseif(APPLE)
                set(_pf_d8_runtime_name libclang_rt.asan_osx_dynamic.dylib)
            else()
                set(_pf_d8_runtime_name "libclang_rt.asan-${CMAKE_SYSTEM_PROCESSOR}.so")
            endif()
            execute_process(COMMAND ${CMAKE_CXX_COMPILER} "-print-file-name=${_pf_d8_runtime_name}"
                OUTPUT_VARIABLE _pf_d8_sanitizer_runtime OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _pf_d8_runtime_result)
            if(NOT _pf_d8_runtime_result EQUAL 0 OR NOT EXISTS "${_pf_d8_sanitizer_runtime}")
                message(FATAL_ERROR "The stop-equivalence Python test requires the compiler's shared ASan runtime")
            endif()
            if(APPLE)
                set(_pf_d8_runtime_environment "DYLD_INSERT_LIBRARIES=${_pf_d8_sanitizer_runtime}")
            else()
                set(_pf_d8_runtime_environment "LD_PRELOAD=${_pf_d8_sanitizer_runtime}")
            endif()
            list(APPEND _pf_d8_equivalence_command ${CMAKE_COMMAND} -E env
                "${_pf_d8_runtime_environment}"
                "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1"
                "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
        endif()
        list(APPEND _pf_d8_equivalence_command ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/lv_d8_tick_stop_equivalence.py
            --library $<TARGET_FILE:lv_d8_stop_strategy> --runner $<TARGET_FILE:pineforge-live>
            --require-terminal-receipts)
        add_test(NAME test_lv_d8_tick_stop_equivalence COMMAND ${_pf_d8_equivalence_command})
        # 38 s on an idle 32-vCPU host; the row spawns many ctypes scenarios and
        # carries no TIMEOUT property, so a loaded or instrumented run is never
        # cut off (owner rule, 2026-10-12).
        else()
            add_test(NAME test_lv_d8_tick_stop_equivalence COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/lv_d8_stop_activation/generated.cpp")
            set_tests_properties(test_lv_d8_tick_stop_equivalence PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
        endif()
    endif()
endif()

# Private-fixture guarded rows (tests/private_fixture_rows.txt).
if(NOT test_aapl15_margin_brackets_l4a IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_aapl15_margin_brackets_l4a.cpp")
    pineforge_add_unit_test(test_aapl15_margin_brackets_l4a)
elseif(NOT test_aapl15_margin_brackets_l4a IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_aapl15_margin_brackets_l4a COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_aapl15_margin_brackets_l4a.cpp")
    set_tests_properties(test_aapl15_margin_brackets_l4a PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_coof_relower IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    pineforge_add_unit_test(test_adapter_coof_relower)
elseif(NOT test_adapter_coof_relower IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_coof_relower COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    set_tests_properties(test_adapter_coof_relower PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_in_place_reads IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pine_d2c_scenarios_fixture.hpp")
    pineforge_add_unit_test(test_adapter_in_place_reads)
elseif(NOT test_adapter_in_place_reads IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_in_place_reads COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/pine_d2c_scenarios_fixture.hpp")
    set_tests_properties(test_adapter_in_place_reads PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_magnifier_intrabar_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    pineforge_add_unit_test(test_adapter_magnifier_intrabar_tapes)
elseif(NOT test_adapter_magnifier_intrabar_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_magnifier_intrabar_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    set_tests_properties(test_adapter_magnifier_intrabar_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_margin_schedule_differential IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_adapter_margin_schedule_differential.cpp")
    pineforge_add_unit_test(test_adapter_margin_schedule_differential)
elseif(NOT test_adapter_margin_schedule_differential IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_margin_schedule_differential COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_adapter_margin_schedule_differential.cpp")
    set_tests_properties(test_adapter_margin_schedule_differential PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_receipts_in_place IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_reservation/es15_bars.inc")
    pineforge_add_unit_test(test_adapter_receipts_in_place)
elseif(NOT test_adapter_receipts_in_place IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_receipts_in_place COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/exit_reservation/es15_bars.inc")
    set_tests_properties(test_adapter_receipts_in_place PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_runtime_ambient IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pine_d2c_scenarios_fixture.hpp")
    pineforge_add_unit_test(test_adapter_runtime_ambient)
elseif(NOT test_adapter_runtime_ambient IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_runtime_ambient COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/pine_d2c_scenarios_fixture.hpp")
    set_tests_properties(test_adapter_runtime_ambient PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_settlement_carry IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pine_d2c_scenarios_fixture.hpp")
    pineforge_add_unit_test(test_adapter_settlement_carry)
elseif(NOT test_adapter_settlement_carry IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_settlement_carry COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/pine_d2c_scenarios_fixture.hpp")
    set_tests_properties(test_adapter_settlement_carry PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_sizing_relower IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    pineforge_add_unit_test(test_adapter_sizing_relower)
elseif(NOT test_adapter_sizing_relower IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_sizing_relower COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    set_tests_properties(test_adapter_sizing_relower PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_add_sibling_margin_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_v6/w5-rv-global-held/meta.json")
    pineforge_add_unit_test(test_add_sibling_margin_tapes)
elseif(NOT test_add_sibling_margin_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_add_sibling_margin_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_v6/w5-rv-global-held/meta.json")
    set_tests_properties(test_add_sibling_margin_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_admission_rules_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/admission_rules/bars/f-2025-04-29-2025-04-30.csv")
    pineforge_add_unit_test(test_admission_rules_tapes)
elseif(NOT test_admission_rules_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_admission_rules_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/admission_rules/bars/f-2025-04-29-2025-04-30.csv")
    set_tests_properties(test_admission_rules_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_aggregated_path_regressions IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_aggregated_path_regressions.cpp")
    pineforge_add_unit_test(test_aggregated_path_regressions)
elseif(NOT test_aggregated_path_regressions IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_aggregated_path_regressions COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_aggregated_path_regressions.cpp")
    set_tests_properties(test_aggregated_path_regressions PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_carried_pooc_short_margin_state_l4a IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_residual/es15_bars.inc")
    pineforge_add_unit_test(test_carried_pooc_short_margin_state_l4a)
elseif(NOT test_carried_pooc_short_margin_state_l4a IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_carried_pooc_short_margin_state_l4a COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_residual/es15_bars.inc")
    set_tests_properties(test_carried_pooc_short_margin_state_l4a PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_cash_fee_sizing_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cash_fee_sizing/pcf-es-contract-p50-m25/meta.json")
    pineforge_add_unit_test(test_cash_fee_sizing_tapes)
elseif(NOT test_cash_fee_sizing_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_cash_fee_sizing_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/cash_fee_sizing/pcf-es-contract-p50-m25/meta.json")
    set_tests_properties(test_cash_fee_sizing_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_chart_tf_security_split_feed IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_chart_tf_security_split_feed.cpp")
    pineforge_add_unit_test(test_chart_tf_security_split_feed)
elseif(NOT test_chart_tf_security_split_feed IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_chart_tf_security_split_feed COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_chart_tf_security_split_feed.cpp")
    set_tests_properties(test_chart_tf_security_split_feed PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_chart_tick_touch_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_competing_tick/README.md")
    pineforge_add_unit_test(test_coof_chart_tick_touch_l4c)
elseif(NOT test_coof_chart_tick_touch_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_chart_tick_touch_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_competing_tick/README.md")
    set_tests_properties(test_coof_chart_tick_touch_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_immediate_close_final_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_immediate_close/bars_ford.inc")
    pineforge_add_unit_test(test_coof_immediate_close_final_tapes)
elseif(NOT test_coof_immediate_close_final_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_immediate_close_final_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_immediate_close/bars_ford.inc")
    set_tests_properties(test_coof_immediate_close_final_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_immediate_close_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_coof_immediate_close_tapes.cpp")
    pineforge_add_unit_test(test_coof_immediate_close_tapes)
elseif(NOT test_coof_immediate_close_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_immediate_close_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_coof_immediate_close_tapes.cpp")
    set_tests_properties(test_coof_immediate_close_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_recalc_security_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_immediate_close/bars_ford.inc")
    pineforge_add_unit_test(test_coof_recalc_security_tapes)
elseif(NOT test_coof_recalc_security_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_recalc_security_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_immediate_close/bars_ford.inc")
    set_tests_properties(test_coof_recalc_security_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_default_flat_market_gross_admission_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/same_open_reversal/README.md")
    pineforge_add_unit_test(test_default_flat_market_gross_admission_l4d)
elseif(NOT test_default_flat_market_gross_admission_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_default_flat_market_gross_admission_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/same_open_reversal/README.md")
    set_tests_properties(test_default_flat_market_gross_admission_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_default_pct_stop_sizing_l4b IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_default_pct_stop_sizing_l4b.cpp")
    pineforge_add_unit_test(test_default_pct_stop_sizing_l4b)
elseif(NOT test_default_pct_stop_sizing_l4b IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_default_pct_stop_sizing_l4b COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_default_pct_stop_sizing_l4b.cpp")
    set_tests_properties(test_default_pct_stop_sizing_l4b PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_default_qty_signal_freeze IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    pineforge_add_unit_test(test_default_qty_signal_freeze)
elseif(NOT test_default_qty_signal_freeze IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_default_qty_signal_freeze COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    set_tests_properties(test_default_qty_signal_freeze PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_engine_trade_accessors IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/daily_break_close/tg-xau1d-requests/meta.json")
    pineforge_add_unit_test(test_engine_trade_accessors)
elseif(NOT test_engine_trade_accessors IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_engine_trade_accessors COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/daily_break_close/tg-xau1d-requests/meta.json")
    set_tests_properties(test_engine_trade_accessors PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_exit_binding_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cross_side_exit/README.md")
    pineforge_add_unit_test(test_exit_binding_tapes)
elseif(NOT test_exit_binding_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_exit_binding_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/cross_side_exit/README.md")
    set_tests_properties(test_exit_binding_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_exit_reservation_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_exit_reservation_tapes.cpp")
    pineforge_add_unit_test(test_exit_reservation_tapes)
elseif(NOT test_exit_reservation_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_exit_reservation_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_exit_reservation_tapes.cpp")
    set_tests_properties(test_exit_reservation_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_explicit_market_admission_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_explicit_market_admission_tapes.cpp")
    pineforge_add_unit_test(test_explicit_market_admission_tapes)
elseif(NOT test_explicit_market_admission_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_explicit_market_admission_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_explicit_market_admission_tapes.cpp")
    set_tests_properties(test_explicit_market_admission_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_explicit_qty_fill_admission_l4b IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    pineforge_add_unit_test(test_explicit_qty_fill_admission_l4b)
elseif(NOT test_explicit_qty_fill_admission_l4b IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_explicit_qty_fill_admission_l4b COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    set_tests_properties(test_explicit_qty_fill_admission_l4b PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_explicit_qty_floor_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/explicit_qty_floor/README.md")
    pineforge_add_unit_test(test_explicit_qty_floor_tapes)
elseif(NOT test_explicit_qty_floor_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_explicit_qty_floor_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/explicit_qty_floor/README.md")
    set_tests_properties(test_explicit_qty_floor_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_famae_declined_reversal_trail_gap_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_famae_declined_reversal_trail_gap_l4c.cpp")
    pineforge_add_unit_test(test_famae_declined_reversal_trail_gap_l4c)
elseif(NOT test_famae_declined_reversal_trail_gap_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_famae_declined_reversal_trail_gap_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_famae_declined_reversal_trail_gap_l4c.cpp")
    set_tests_properties(test_famae_declined_reversal_trail_gap_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_famae_lot_sizing_ten_digit_equity IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_famae_lot_sizing_ten_digit_equity.cpp")
    pineforge_add_unit_test(test_famae_lot_sizing_ten_digit_equity)
elseif(NOT test_famae_lot_sizing_ten_digit_equity IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_famae_lot_sizing_ten_digit_equity COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_famae_lot_sizing_ten_digit_equity.cpp")
    set_tests_properties(test_famae_lot_sizing_ten_digit_equity PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_famx_declined_reversal_trail_leg_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_famx_declined_reversal_trail_leg_l4c.cpp")
    pineforge_add_unit_test(test_famx_declined_reversal_trail_leg_l4c)
elseif(NOT test_famx_declined_reversal_trail_leg_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_famx_declined_reversal_trail_leg_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_famx_declined_reversal_trail_leg_l4c.cpp")
    set_tests_properties(test_famx_declined_reversal_trail_leg_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_flat_coof_exit_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_flat_coof_exit_tapes.cpp")
    pineforge_add_unit_test(test_flat_coof_exit_tapes)
elseif(NOT test_flat_coof_exit_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_flat_coof_exit_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_flat_coof_exit_tapes.cpp")
    set_tests_properties(test_flat_coof_exit_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_foreign_break_stamp_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/daily_break_close/tg-xau1d-requests/meta.json")
    pineforge_add_unit_test(test_foreign_break_stamp_tapes)
elseif(NOT test_foreign_break_stamp_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_foreign_break_stamp_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/daily_break_close/tg-xau1d-requests/meta.json")
    set_tests_properties(test_foreign_break_stamp_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_high_value_signal_cost_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/whole_lot_tie/README.md")
    pineforge_add_unit_test(test_high_value_signal_cost_l4d)
elseif(NOT test_high_value_signal_cost_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_high_value_signal_cost_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/whole_lot_tie/README.md")
    set_tests_properties(test_high_value_signal_cost_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_int28_rule_compositions IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_v6/w5-rv-global-held/meta.json")
    pineforge_add_unit_test(test_int28_rule_compositions)
elseif(NOT test_int28_rule_compositions IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_int28_rule_compositions COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_v6/w5-rv-global-held/meta.json")
    set_tests_properties(test_int28_rule_compositions PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_int30_rule_compositions IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_v6/w5-rv-global-held/meta.json")
    pineforge_add_unit_test(test_int30_rule_compositions)
elseif(NOT test_int30_rule_compositions IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_int30_rule_compositions COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_v6/w5-rv-global-held/meta.json")
    set_tests_properties(test_int30_rule_compositions PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_kernel_trail_stop_replica IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callback_lifecycle/carried-static-stop/bars.csv")
    pineforge_add_unit_test(test_kernel_trail_stop_replica)
elseif(NOT test_kernel_trail_stop_replica IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_kernel_trail_stop_replica COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/callback_lifecycle/carried-static-stop/bars.csv")
    set_tests_properties(test_kernel_trail_stop_replica PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10ad_tick_bracket_stuck_entry IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10ad_tick_bracket_stuck_entry.cpp")
    pineforge_add_unit_test(test_l10ad_tick_bracket_stuck_entry)
elseif(NOT test_l10ad_tick_bracket_stuck_entry IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10ad_tick_bracket_stuck_entry COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10ad_tick_bracket_stuck_entry.cpp")
    set_tests_properties(test_l10ad_tick_bracket_stuck_entry PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l4b_policy_regressions IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cash_fee_sizing/pcf-es-contract-p50-m25/meta.json")
    pineforge_add_unit_test(test_l4b_policy_regressions)
elseif(NOT test_l4b_policy_regressions IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l4b_policy_regressions COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/cash_fee_sizing/pcf-es-contract-p50-m25/meta.json")
    set_tests_properties(test_l4b_policy_regressions PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l9a_carried_pooc_competing_checkpoint IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_residual/es15_bars.inc")
    pineforge_add_unit_test(test_l9a_carried_pooc_competing_checkpoint)
elseif(NOT test_l9a_carried_pooc_competing_checkpoint IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l9a_carried_pooc_competing_checkpoint COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_residual/es15_bars.inc")
    set_tests_properties(test_l9a_carried_pooc_competing_checkpoint PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_live_trade_accessors_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_live_trade_accessors_l4d.cpp")
    pineforge_add_unit_test(test_live_trade_accessors_l4d)
elseif(NOT test_live_trade_accessors_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_live_trade_accessors_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_live_trade_accessors_l4d.cpp")
    set_tests_properties(test_live_trade_accessors_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_local_time_fields IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/time_close_function/w12-tclose-aapl15/meta.json")
    pineforge_add_unit_test(test_local_time_fields)
elseif(NOT test_local_time_fields IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_local_time_fields COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/time_close_function/w12-tclose-aapl15/meta.json")
    set_tests_properties(test_local_time_fields PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ltf_lookahead_calling_open_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ltf_lookahead_calling_open/w8c-ltfon-aapl1d/meta.json")
    pineforge_add_unit_test(test_ltf_lookahead_calling_open_tapes)
elseif(NOT test_ltf_lookahead_calling_open_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ltf_lookahead_calling_open_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/ltf_lookahead_calling_open/w8c-ltfon-aapl1d/meta.json")
    set_tests_properties(test_ltf_lookahead_calling_open_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_magnified_aggregated_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_magnified_aggregated_tape.cpp")
    pineforge_add_unit_test(test_magnified_aggregated_tape)
elseif(NOT test_magnified_aggregated_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_magnified_aggregated_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_magnified_aggregated_tape.cpp")
    set_tests_properties(test_magnified_aggregated_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_magnifier_tradingview_intrabars IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    pineforge_add_unit_test(test_magnifier_tradingview_intrabars)
elseif(NOT test_magnifier_tradingview_intrabars IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_magnifier_tradingview_intrabars COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    set_tests_properties(test_magnifier_tradingview_intrabars PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_call_1x_long_entry_fill_l4a IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_open_rules/admission/short-refused-at-close/metrics.json")
    pineforge_add_unit_test(test_margin_call_1x_long_entry_fill_l4a)
elseif(NOT test_margin_call_1x_long_entry_fill_l4a IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_call_1x_long_entry_fill_l4a COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_open_rules/admission/short-refused-at-close/metrics.json")
    set_tests_properties(test_margin_call_1x_long_entry_fill_l4a PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_call_rules_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    pineforge_add_unit_test(test_margin_call_rules_tapes)
elseif(NOT test_margin_call_rules_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_call_rules_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    set_tests_properties(test_margin_call_rules_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_ledger_rules_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_ledger_rules/aapl-explicit-flat-q356-above/plan.json")
    pineforge_add_unit_test(test_margin_ledger_rules_tapes)
elseif(NOT test_margin_ledger_rules_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_ledger_rules_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_ledger_rules/aapl-explicit-flat-q356-above/plan.json")
    set_tests_properties(test_margin_ledger_rules_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_open_print_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_margin_open_print_tapes.cpp")
    pineforge_add_unit_test(test_margin_open_print_tapes)
elseif(NOT test_margin_open_print_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_open_print_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_margin_open_print_tapes.cpp")
    set_tests_properties(test_margin_open_print_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_open_rules_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_open_rules/admission/short-refused-at-close/metrics.json")
    pineforge_add_unit_test(test_margin_open_rules_tapes)
elseif(NOT test_margin_open_rules_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_open_rules_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_open_rules/admission/short-refused-at-close/metrics.json")
    set_tests_properties(test_margin_open_rules_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_opposite_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_margin_opposite_tapes.cpp")
    pineforge_add_unit_test(test_margin_opposite_tapes)
elseif(NOT test_margin_opposite_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_opposite_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_margin_opposite_tapes.cpp")
    set_tests_properties(test_margin_opposite_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_residual_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_margin_residual_tapes.cpp")
    pineforge_add_unit_test(test_margin_residual_tapes)
elseif(NOT test_margin_residual_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_residual_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_margin_residual_tapes.cpp")
    set_tests_properties(test_margin_residual_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_schedule_rules_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_schedule_rules/add/signal-close-admitted-pooc/metrics.json")
    pineforge_add_unit_test(test_margin_schedule_rules_tapes)
elseif(NOT test_margin_schedule_rules_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_schedule_rules_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_schedule_rules/add/signal-close-admitted-pooc/metrics.json")
    set_tests_properties(test_margin_schedule_rules_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_v6_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_v6/w5-rv-global-held/meta.json")
    pineforge_add_unit_test(test_margin_v6_tapes)
elseif(NOT test_margin_v6_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_v6_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_v6/w5-rv-global-held/meta.json")
    set_tests_properties(test_margin_v6_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_calc_timing_twin IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    pineforge_add_unit_test(test_native_calc_timing_twin)
elseif(NOT test_native_calc_timing_twin IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_calc_timing_twin COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json")
    set_tests_properties(test_native_calc_timing_twin PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_daily_holiday_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_daily_holiday_l4d.cpp")
    pineforge_add_unit_test(test_native_daily_holiday_l4d)
elseif(NOT test_native_daily_holiday_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_daily_holiday_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_daily_holiday_l4d.cpp")
    set_tests_properties(test_native_daily_holiday_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_instrument_feed IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/daily_break_close/tg-xau1d-requests/meta.json")
    pineforge_add_unit_test(test_native_instrument_feed)
elseif(NOT test_native_instrument_feed IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_instrument_feed COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/daily_break_close/tg-xau1d-requests/meta.json")
    set_tests_properties(test_native_instrument_feed PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_margin_hooks_twin IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/half_tick_rounding/README.md")
    pineforge_add_unit_test(test_native_margin_hooks_twin)
elseif(NOT test_native_margin_hooks_twin IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_margin_hooks_twin COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/half_tick_rounding/README.md")
    set_tests_properties(test_native_margin_hooks_twin PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_margin_post_fill_path IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_entry_bar/m10_bars.inc")
    pineforge_add_unit_test(test_native_margin_post_fill_path)
elseif(NOT test_native_margin_post_fill_path IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_margin_post_fill_path COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_entry_bar/m10_bars.inc")
    set_tests_properties(test_native_margin_post_fill_path PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_price_grid IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_price_grid.cpp")
    pineforge_add_unit_test(test_native_price_grid)
elseif(NOT test_native_price_grid IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_price_grid COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_price_grid.cpp")
    set_tests_properties(test_native_price_grid PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_session_day_bar_open IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    pineforge_add_unit_test(test_native_session_day_bar_open)
elseif(NOT test_native_session_day_bar_open IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_session_day_bar_open COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    set_tests_properties(test_native_session_day_bar_open PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_session_day_facts IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_session_day_facts.cpp")
    pineforge_add_unit_test(test_native_session_day_facts)
elseif(NOT test_native_session_day_facts IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_session_day_facts COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_session_day_facts.cpp")
    set_tests_properties(test_native_session_day_facts PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_trail_stop_ladder IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trail_activation_tick_reach/bars.inc")
    pineforge_add_unit_test(test_native_trail_stop_ladder)
elseif(NOT test_native_trail_stop_ladder IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_trail_stop_ladder COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/trail_activation_tick_reach/bars.inc")
    set_tests_properties(test_native_trail_stop_ladder PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_wm_buckets_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_wm_buckets_l4d.cpp")
    pineforge_add_unit_test(test_native_wm_buckets_l4d)
elseif(NOT test_native_wm_buckets_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_wm_buckets_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_wm_buckets_l4d.cpp")
    set_tests_properties(test_native_wm_buckets_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_o_close_pct_day_anchor IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_o_close_pct_day_anchor.cpp")
    pineforge_add_unit_test(test_o_close_pct_day_anchor)
elseif(NOT test_o_close_pct_day_anchor IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_o_close_pct_day_anchor COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_o_close_pct_day_anchor.cpp")
    set_tests_properties(test_o_close_pct_day_anchor PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_oanda_lazy_close IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_oanda_lazy_close.cpp")
    pineforge_add_unit_test(test_oanda_lazy_close)
elseif(NOT test_oanda_lazy_close IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_oanda_lazy_close COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_oanda_lazy_close.cpp")
    set_tests_properties(test_oanda_lazy_close PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_offset_trail_quantized_arm IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/offset_trail_arm/e5-f-long-arm-qhigh/meta.json")
    pineforge_add_unit_test(test_offset_trail_quantized_arm)
elseif(NOT test_offset_trail_quantized_arm IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_offset_trail_quantized_arm COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/offset_trail_arm/e5-f-long-arm-qhigh/meta.json")
    set_tests_properties(test_offset_trail_quantized_arm PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_open_fill_order_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_open_fill_order_tapes.cpp")
    pineforge_add_unit_test(test_open_fill_order_tapes)
elseif(NOT test_open_fill_order_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_open_fill_order_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_open_fill_order_tapes.cpp")
    set_tests_properties(test_open_fill_order_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pending_entry_trail_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pending_entry_trail_tapes.cpp")
    pineforge_add_unit_test(test_pending_entry_trail_tapes)
elseif(NOT test_pending_entry_trail_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pending_entry_trail_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pending_entry_trail_tapes.cpp")
    set_tests_properties(test_pending_entry_trail_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pine_time_day_stamp_grid IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_period/w12-ctclose-aapl45/meta.json")
    pineforge_add_unit_test(test_pine_time_day_stamp_grid)
elseif(NOT test_pine_time_day_stamp_grid IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pine_time_day_stamp_grid COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_period/w12-ctclose-aapl45/meta.json")
    set_tests_properties(test_pine_time_day_stamp_grid PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_point_order_open_call IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/admission_rules/bars/f-2025-04-29-2025-04-30.csv")
    pineforge_add_unit_test(test_point_order_open_call)
elseif(NOT test_point_order_open_call IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_point_order_open_call COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/admission_rules/bars/f-2025-04-29-2025-04-30.csv")
    set_tests_properties(test_point_order_open_call PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_fill_stamp_aggregation IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pooc_fill_stamp_aggregation.cpp")
    pineforge_add_unit_test(test_pooc_fill_stamp_aggregation)
elseif(NOT test_pooc_fill_stamp_aggregation IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_fill_stamp_aggregation COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pooc_fill_stamp_aggregation.cpp")
    set_tests_properties(test_pooc_fill_stamp_aggregation PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_gap_stop_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pooc_gap_stop_tapes.cpp")
    pineforge_add_unit_test(test_pooc_gap_stop_tapes)
elseif(NOT test_pooc_gap_stop_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_gap_stop_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pooc_gap_stop_tapes.cpp")
    set_tests_properties(test_pooc_gap_stop_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_limit_close_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pooc_limit_close_tapes.cpp")
    pineforge_add_unit_test(test_pooc_limit_close_tapes)
elseif(NOT test_pooc_limit_close_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_limit_close_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pooc_limit_close_tapes.cpp")
    set_tests_properties(test_pooc_limit_close_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_open_money_event_l4b IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_v6/w5-rv-global-held/meta.json")
    pineforge_add_unit_test(test_pooc_open_money_event_l4b)
elseif(NOT test_pooc_open_money_event_l4b IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_open_money_event_l4b COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_v6/w5-rv-global-held/meta.json")
    set_tests_properties(test_pooc_open_money_event_l4b PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_reversing_stop_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pooc_reversing_stop_tapes.cpp")
    pineforge_add_unit_test(test_pooc_reversing_stop_tapes)
elseif(NOT test_pooc_reversing_stop_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_reversing_stop_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pooc_reversing_stop_tapes.cpp")
    set_tests_properties(test_pooc_reversing_stop_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_samepass_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pooc_samepass_tapes.cpp")
    pineforge_add_unit_test(test_pooc_samepass_tapes)
elseif(NOT test_pooc_samepass_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_samepass_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pooc_samepass_tapes.cpp")
    set_tests_properties(test_pooc_samepass_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_stop_reentry_bracket_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pooc_stop_reentry_bracket_tapes.cpp")
    pineforge_add_unit_test(test_pooc_stop_reentry_bracket_tapes)
elseif(NOT test_pooc_stop_reentry_bracket_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_stop_reentry_bracket_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pooc_stop_reentry_bracket_tapes.cpp")
    set_tests_properties(test_pooc_stop_reentry_bracket_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_publication_witness IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cash_fee_sizing/pcf-es-contract-p50-m25/meta.json")
    pineforge_add_unit_test(test_publication_witness)
elseif(NOT test_publication_witness IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_publication_witness COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/cash_fee_sizing/pcf-es-contract-p50-m25/meta.json")
    set_tests_properties(test_publication_witness PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pyramiding_count_differential IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_pyramiding_count_differential.cpp")
    pineforge_add_unit_test(test_pyramiding_count_differential)
elseif(NOT test_pyramiding_count_differential IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pyramiding_count_differential COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_pyramiding_count_differential.cpp")
    set_tests_properties(test_pyramiding_count_differential PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_reversal_admission_float_guard_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_residual/es15_bars.inc")
    pineforge_add_unit_test(test_reversal_admission_float_guard_l4c)
elseif(NOT test_reversal_admission_float_guard_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_reversal_admission_float_guard_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_residual/es15_bars.inc")
    set_tests_properties(test_reversal_admission_float_guard_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_reversal_close_all_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_v6/w5-rv-global-held/meta.json")
    pineforge_add_unit_test(test_reversal_close_all_tapes)
elseif(NOT test_reversal_close_all_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_reversal_close_all_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_v6/w5-rv-global-held/meta.json")
    set_tests_properties(test_reversal_close_all_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_risk_max_intraday_loss_tv_l4a IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_risk_max_intraday_loss_tv_l4a.cpp")
    pineforge_add_unit_test(test_risk_max_intraday_loss_tv_l4a)
elseif(NOT test_risk_max_intraday_loss_tv_l4a IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_risk_max_intraday_loss_tv_l4a COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_risk_max_intraday_loss_tv_l4a.cpp")
    set_tests_properties(test_risk_max_intraday_loss_tv_l4a PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_rounded_carried_short_trail_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_residual/es15_bars.inc")
    pineforge_add_unit_test(test_rounded_carried_short_trail_l4c)
elseif(NOT test_rounded_carried_short_trail_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_rounded_carried_short_trail_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_residual/es15_bars.inc")
    set_tests_properties(test_rounded_carried_short_trail_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_run_harness_window_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/run_harness_window/rh-diag-firstbar2-es115/meta.json")
    pineforge_add_unit_test(test_run_harness_window_tapes)
elseif(NOT test_run_harness_window_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_run_harness_window_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/run_harness_window/rh-diag-firstbar2-es115/meta.json")
    set_tests_properties(test_run_harness_window_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_same_open_reversal_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_same_open_reversal_tapes.cpp")
    pineforge_add_unit_test(test_same_open_reversal_tapes)
elseif(NOT test_same_open_reversal_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_same_open_reversal_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_same_open_reversal_tapes.cpp")
    set_tests_properties(test_same_open_reversal_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_second_extreme_order_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_second_extreme_order_tapes.cpp")
    pineforge_add_unit_test(test_second_extreme_order_tapes)
elseif(NOT test_second_extreme_order_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_second_extreme_order_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_second_extreme_order_tapes.cpp")
    set_tests_properties(test_second_extreme_order_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_security_multi_day_buckets_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/multi_day_buckets/pf-taila-ndgrid-aapl-d/meta.json")
    pineforge_add_unit_test(test_security_multi_day_buckets_tapes)
elseif(NOT test_security_multi_day_buckets_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_security_multi_day_buckets_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/multi_day_buckets/pf-taila-ndgrid-aapl-d/meta.json")
    set_tests_properties(test_security_multi_day_buckets_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_break_open_bar_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    pineforge_add_unit_test(test_session_break_open_bar_tape)
elseif(NOT test_session_break_open_bar_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_break_open_bar_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    set_tests_properties(test_session_break_open_bar_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_calendar_extra IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/time_close_function/w12-tclose-aapl15/meta.json")
    pineforge_add_unit_test(test_session_calendar_extra)
elseif(NOT test_session_calendar_extra IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_calendar_extra COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/time_close_function/w12-tclose-aapl15/meta.json")
    set_tests_properties(test_session_calendar_extra PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_day_anchors IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_period/w12-ctclose-aapl45/meta.json")
    pineforge_add_unit_test(test_session_day_anchors)
elseif(NOT test_session_day_anchors IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_day_anchors COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_period/w12-ctclose-aapl45/meta.json")
    set_tests_properties(test_session_day_anchors PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_day_facts_adapter IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_session_day_facts_adapter.cpp")
    pineforge_add_unit_test(test_session_day_facts_adapter)
elseif(NOT test_session_day_facts_adapter IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_day_facts_adapter COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_session_day_facts_adapter.cpp")
    set_tests_properties(test_session_day_facts_adapter PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_extended_day_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    pineforge_add_unit_test(test_session_extended_day_tape)
elseif(NOT test_session_extended_day_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_extended_day_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    set_tests_properties(test_session_extended_day_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_islastbar_aggregation IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_session_islastbar_aggregation.cpp")
    pineforge_add_unit_test(test_session_islastbar_aggregation)
elseif(NOT test_session_islastbar_aggregation IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_islastbar_aggregation COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_session_islastbar_aggregation.cpp")
    set_tests_properties(test_session_islastbar_aggregation PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_ismarket_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_ismarket/hm-g236-es1-60-dst-mar/meta.json")
    pineforge_add_unit_test(test_session_ismarket_tape)
elseif(NOT test_session_ismarket_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_ismarket_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_ismarket/hm-g236-es1-60-dst-mar/meta.json")
    set_tests_properties(test_session_ismarket_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_predicates IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_period/w12-ctclose-aapl45/meta.json")
    pineforge_add_unit_test(test_session_predicates)
elseif(NOT test_session_predicates IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_predicates COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_period/w12-ctclose-aapl45/meta.json")
    set_tests_properties(test_session_predicates PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_prepost_windows_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    pineforge_add_unit_test(test_session_prepost_windows_tape)
elseif(NOT test_session_prepost_windows_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_prepost_windows_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_windows/cgim-flags-aapl-60-ext/metrics.json")
    set_tests_properties(test_session_prepost_windows_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_slipped_short_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_slipped_short_tapes.cpp")
    pineforge_add_unit_test(test_slipped_short_tapes)
elseif(NOT test_slipped_short_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_slipped_short_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_slipped_short_tapes.cpp")
    set_tests_properties(test_slipped_short_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_split_feed_partial_bucket IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ltf_lookahead_calling_open/w8c-ltfon-aapl1d/meta.json")
    pineforge_add_unit_test(test_split_feed_partial_bucket)
elseif(NOT test_split_feed_partial_bucket IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_split_feed_partial_bucket COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/ltf_lookahead_calling_open/w8c-ltfon-aapl1d/meta.json")
    set_tests_properties(test_split_feed_partial_bucket PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_symbol_calendar_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/symbol_calendar/xc-cal-aapl15/calendar.json")
    pineforge_add_unit_test(test_symbol_calendar_tapes)
elseif(NOT test_symbol_calendar_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_symbol_calendar_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/symbol_calendar/xc-cal-aapl15/calendar.json")
    set_tests_properties(test_symbol_calendar_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_taro_price_gap_admission_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    pineforge_add_unit_test(test_taro_price_gap_admission_l4d)
elseif(NOT test_taro_price_gap_admission_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_taro_price_gap_admission_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_call_rules/bars/f-2025-09-17-2025-09-18.csv")
    set_tests_properties(test_taro_price_gap_admission_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_time_bars_back_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/time_bars_back/te-time-bb-chart-aapl15/meta.json")
    pineforge_add_unit_test(test_time_bars_back_tapes)
elseif(NOT test_time_bars_back_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_time_bars_back_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/time_bars_back/te-time-bb-chart-aapl15/meta.json")
    set_tests_properties(test_time_bars_back_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_trail_activation_tick_bar_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/offset_trail_arm/e5-f-long-arm-qhigh/meta.json")
    pineforge_add_unit_test(test_trail_activation_tick_bar_l4c)
elseif(NOT test_trail_activation_tick_bar_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_trail_activation_tick_bar_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/offset_trail_arm/e5-f-long-arm-qhigh/meta.json")
    set_tests_properties(test_trail_activation_tick_bar_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_trail_activation_tick_reach IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_trail_activation_tick_reach.cpp")
    pineforge_add_unit_test(test_trail_activation_tick_reach)
elseif(NOT test_trail_activation_tick_reach IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_trail_activation_tick_reach COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_trail_activation_tick_reach.cpp")
    set_tests_properties(test_trail_activation_tick_reach PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_trail_fill_snap_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_trail_fill_snap_l4c.cpp")
    pineforge_add_unit_test(test_trail_fill_snap_l4c)
elseif(NOT test_trail_fill_snap_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_trail_fill_snap_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_trail_fill_snap_l4c.cpp")
    set_tests_properties(test_trail_fill_snap_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_tv_money_carried_pooc_l4b IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_residual/es15_bars.inc")
    pineforge_add_unit_test(test_tv_money_carried_pooc_l4b)
elseif(NOT test_tv_money_carried_pooc_l4b IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_tv_money_carried_pooc_l4b COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_residual/es15_bars.inc")
    set_tests_properties(test_tv_money_carried_pooc_l4b PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_whole_lot_tie_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_whole_lot_tie_tapes.cpp")
    pineforge_add_unit_test(test_whole_lot_tie_tapes)
elseif(NOT test_whole_lot_tie_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_whole_lot_tie_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_whole_lot_tie_tapes.cpp")
    set_tests_properties(test_whole_lot_tie_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_zero_offset_trail_rides_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_zero_offset_trail_rides_l4c.cpp")
    pineforge_add_unit_test(test_zero_offset_trail_rides_l4c)
elseif(NOT test_zero_offset_trail_rides_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_zero_offset_trail_rides_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_zero_offset_trail_rides_l4c.cpp")
    set_tests_properties(test_zero_offset_trail_rides_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_zero_trail_sibling_stop IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_zero_trail_sibling_stop.cpp")
    pineforge_add_unit_test(test_zero_trail_sibling_stop)
elseif(NOT test_zero_trail_sibling_stop IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_zero_trail_sibling_stop COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_zero_trail_sibling_stop.cpp")
    set_tests_properties(test_zero_trail_sibling_stop PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

# The five tape rows the PR-1 proof box caught running (and failing) against a
# removed fixture. All five are named by _pf_private_fixture_rows above, so
# the TEST_SOURCES loop does not register them; each is registered here.
#
# All five read their fixture data at run time, and the private-fixture
# removals take the whole fixture directories the rows read (the removed file
# beside distributed ones in PR 1, the remaining files in PR 1b), so on the
# fixture-less tree none of the five can run: every row is pinned whole, the
# real test on the PRESENT branch and the pinned SKIP placeholder otherwise
# (the literal names a removed file the row reads).
#
# The four rows whose binary carries a distributed half also SPLIT: beside the
# row's own name (the default run, the distributed part) sits a <row>_private
# row running the same binary with --private-only (the removed half). Both
# names register on the SAME EXISTS guard so the *_private command never names
# a target the tree did not create.
if(NOT test_carried_trail_open_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/carried_trail_open/bars.csv")
    pineforge_add_unit_test(test_carried_trail_open_tapes)
elseif(NOT test_carried_trail_open_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_carried_trail_open_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/carried_trail_open/bars.csv")
    set_tests_properties(test_carried_trail_open_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

# The three tape rows scripts/check_private_fixture_reads.py found running
# against removed fixtures while unpinned: each composes the ORDER_PRINT
# fixture root with case literals under a removed directory, so every case it
# replays is gone on the fixture-less tree (the abort the release sweep hit).
# Each pins whole on a removed file it reads -- no distributed half is left.
if(NOT test_coof_marketable_stop_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_marketable_stop/bars.csv")
    pineforge_add_unit_test(test_coof_marketable_stop_tapes)
elseif(NOT test_coof_marketable_stop_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_marketable_stop_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_marketable_stop/bars.csv")
    set_tests_properties(test_coof_marketable_stop_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_close_bar_fills_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_close_bar_fills/README.md")
    pineforge_add_unit_test(test_pooc_close_bar_fills_tapes)
elseif(NOT test_pooc_close_bar_fills_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_close_bar_fills_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_close_bar_fills/README.md")
    set_tests_properties(test_pooc_close_bar_fills_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_pending_stop_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_pending_stop/bitcoin/bars.csv")
    pineforge_add_unit_test(test_pooc_pending_stop_tapes)
elseif(NOT test_pooc_pending_stop_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_pending_stop_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_pending_stop/bitcoin/bars.csv")
    set_tests_properties(test_pooc_pending_stop_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_close_slippage_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_close_slippage/bars.csv")
    pineforge_add_unit_test(test_coof_close_slippage_tapes)
    add_test(NAME test_coof_close_slippage_tapes_private COMMAND test_coof_close_slippage_tapes --private-only)
elseif(NOT test_coof_close_slippage_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_close_slippage_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_close_slippage/partial/bars.csv")
    set_tests_properties(test_coof_close_slippage_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_coof_close_slippage_tapes_private COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_close_slippage/bars.csv")
    set_tests_properties(test_coof_close_slippage_tapes_private PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_cross_mechanism_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cross_mechanism/batch_carried_trail/bars.csv")
    pineforge_add_unit_test(test_cross_mechanism_tapes)
    add_test(NAME test_cross_mechanism_tapes_private COMMAND test_cross_mechanism_tapes --private-only)
elseif(NOT test_cross_mechanism_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_cross_mechanism_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/cross_mechanism/bracket_one/tv_trades.csv")
    set_tests_properties(test_cross_mechanism_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_cross_mechanism_tapes_private COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/cross_mechanism/batch_carried_trail/bars.csv")
    set_tests_properties(test_cross_mechanism_tapes_private PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

# test_source_buying_power_tapes reads every variant through its fixture-dir
# define at run time, so its whole row pins on the fixture directory marker.
if(NOT test_source_buying_power_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/source_buying_power/README.md")
    pineforge_add_unit_test(test_source_buying_power_tapes)
    add_test(NAME test_source_buying_power_tapes_private COMMAND test_source_buying_power_tapes --private-only)
elseif(NOT test_source_buying_power_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_source_buying_power_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/source_buying_power/README.md")
    set_tests_properties(test_source_buying_power_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_source_buying_power_tapes_private COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/source_buying_power/flat-under/bars.csv")
    set_tests_properties(test_source_buying_power_tapes_private PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_rules_forward_replay IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_ledger_rules/c1-r1-exp-l2s-reject/r1/tv_trades.csv")
    pineforge_add_unit_test(test_margin_rules_forward_replay)
    add_test(NAME test_margin_rules_forward_replay_private COMMAND test_margin_rules_forward_replay --private-only)
elseif(NOT test_margin_rules_forward_replay IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_rules_forward_replay COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_call_rules/sizing/size-btc-p3u/tv_trades.csv")
    set_tests_properties(test_margin_rules_forward_replay PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
    add_test(NAME test_margin_rules_forward_replay_private COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/margin_ledger_rules/c1-r1-exp-l2s-reject/r1/tv_trades.csv")
    set_tests_properties(test_margin_rules_forward_replay_private PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()


# PR 1b private-fixture guarded rows: rows whose source, fixture or quoted
# #include reaches a path the second private-fixture removal takes (the
# PR-1 manifest's guards above stay untouched). Same shape as above: with
# the fixtures present the real row registers, without them the row is the
# pinned SKIP placeholder (tests/private_fixture_rows.txt).
if(NOT test_adapter_fifo_lot_boundary IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_adapter_fifo_lot_boundary.cpp")
    pineforge_add_unit_test(test_adapter_fifo_lot_boundary)
elseif(NOT test_adapter_fifo_lot_boundary IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_fifo_lot_boundary COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_adapter_fifo_lot_boundary.cpp")
    set_tests_properties(test_adapter_fifo_lot_boundary PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_margin_relower IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_adapter_margin_relower.cpp")
    pineforge_add_unit_test(test_adapter_margin_relower)
elseif(NOT test_adapter_margin_relower IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_margin_relower COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_adapter_margin_relower.cpp")
    set_tests_properties(test_adapter_margin_relower PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_adapter_margin_revival_cancel IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_adapter_margin_revival_cancel_data.hpp")
    pineforge_add_unit_test(test_adapter_margin_revival_cancel)
elseif(NOT test_adapter_margin_revival_cancel IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_adapter_margin_revival_cancel COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_adapter_margin_revival_cancel_data.hpp")
    set_tests_properties(test_adapter_margin_revival_cancel PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_aggregated_admission_receipt_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/aggregated_admission_receipt/bars.inc")
    pineforge_add_unit_test(test_aggregated_admission_receipt_tapes)
elseif(NOT test_aggregated_admission_receipt_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_aggregated_admission_receipt_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/aggregated_admission_receipt/bars.inc")
    set_tests_properties(test_aggregated_admission_receipt_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_aggregated_entry_bar_index_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/aggregated_entry_bar_index/bars.inc")
    pineforge_add_unit_test(test_aggregated_entry_bar_index_tape)
elseif(NOT test_aggregated_entry_bar_index_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_aggregated_entry_bar_index_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/aggregated_entry_bar_index/bars.inc")
    set_tests_properties(test_aggregated_entry_bar_index_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_color_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/color_tv/w11-color-alpha-eth15/meta.json")
    pineforge_add_unit_test(test_color_tapes)
elseif(NOT test_color_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_color_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/color_tv/w11-color-alpha-eth15/meta.json")
    set_tests_properties(test_color_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()
if(NOT test_aux_security_feed_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_aux_security_feed_l4d.cpp")
    pineforge_add_unit_test(test_aux_security_feed_l4d)
elseif(NOT test_aux_security_feed_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_aux_security_feed_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_aux_security_feed_l4d.cpp")
    set_tests_properties(test_aux_security_feed_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_drawing_rollback_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_drawing_rollback/bars.inc")
    pineforge_add_unit_test(test_coof_drawing_rollback_tape)
elseif(NOT test_coof_drawing_rollback_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_drawing_rollback_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_drawing_rollback/bars.inc")
    set_tests_properties(test_coof_drawing_rollback_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_fill_bar_exit_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_fill_bar_exit/bars.inc")
    pineforge_add_unit_test(test_coof_fill_bar_exit_tapes)
elseif(NOT test_coof_fill_bar_exit_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_fill_bar_exit_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_fill_bar_exit/bars.inc")
    set_tests_properties(test_coof_fill_bar_exit_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_inflight_cancel_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_inflight_cancel/bars.inc")
    pineforge_add_unit_test(test_coof_inflight_cancel_tapes)
elseif(NOT test_coof_inflight_cancel_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_inflight_cancel_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_inflight_cancel/bars.inc")
    set_tests_properties(test_coof_inflight_cancel_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_open_limit_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_open_limit/bars.inc")
    pineforge_add_unit_test(test_coof_open_limit_tapes)
elseif(NOT test_coof_open_limit_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_open_limit_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_open_limit/bars.inc")
    set_tests_properties(test_coof_open_limit_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_open_newborn_exit_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_open_newborn_exit/bars.inc")
    pineforge_add_unit_test(test_coof_open_newborn_exit_tape)
elseif(NOT test_coof_open_newborn_exit_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_open_newborn_exit_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_open_newborn_exit/bars.inc")
    set_tests_properties(test_coof_open_newborn_exit_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_open_recalc_context_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_coof_open_recalc_context_data.hpp")
    pineforge_add_unit_test(test_coof_open_recalc_context_l4c)
elseif(NOT test_coof_open_recalc_context_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_open_recalc_context_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_coof_open_recalc_context_data.hpp")
    set_tests_properties(test_coof_open_recalc_context_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_refill_waypoint_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_refill_waypoint/bars.inc")
    pineforge_add_unit_test(test_coof_refill_waypoint_tapes)
elseif(NOT test_coof_refill_waypoint_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_refill_waypoint_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_refill_waypoint/bars.inc")
    set_tests_properties(test_coof_refill_waypoint_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_reissued_exit_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_reissued_exit/bars.inc")
    pineforge_add_unit_test(test_coof_reissued_exit_tapes)
elseif(NOT test_coof_reissued_exit_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_reissued_exit_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_reissued_exit/bars.inc")
    set_tests_properties(test_coof_reissued_exit_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_resting_bracket_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_resting_bracket/bars.inc")
    pineforge_add_unit_test(test_coof_resting_bracket_tapes)
elseif(NOT test_coof_resting_bracket_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_resting_bracket_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_resting_bracket/bars.inc")
    set_tests_properties(test_coof_resting_bracket_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_resting_stop_siblings_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_coof_resting_stop_siblings_l4c.cpp")
    pineforge_add_unit_test(test_coof_resting_stop_siblings_l4c)
elseif(NOT test_coof_resting_stop_siblings_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_resting_stop_siblings_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_coof_resting_stop_siblings_l4c.cpp")
    set_tests_properties(test_coof_resting_stop_siblings_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_same_point_exits_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_same_point_exits/bars.inc")
    pineforge_add_unit_test(test_coof_same_point_exits_tapes)
elseif(NOT test_coof_same_point_exits_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_same_point_exits_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_same_point_exits/bars.inc")
    set_tests_properties(test_coof_same_point_exits_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_timeframe_change_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_timeframe_change/bars.inc")
    pineforge_add_unit_test(test_coof_timeframe_change_tapes)
elseif(NOT test_coof_timeframe_change_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_timeframe_change_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_timeframe_change/bars.inc")
    set_tests_properties(test_coof_timeframe_change_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coof_w2_refill_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_refill_waypoint/bars.inc")
    pineforge_add_unit_test(test_coof_w2_refill_tapes)
elseif(NOT test_coof_w2_refill_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_w2_refill_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_refill_waypoint/bars.inc")
    set_tests_properties(test_coof_w2_refill_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_coqueued_open_margin_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coqueued_open_margin/bars.inc")
    pineforge_add_unit_test(test_coqueued_open_margin_tapes)
elseif(NOT test_coqueued_open_margin_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coqueued_open_margin_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coqueued_open_margin/bars.inc")
    set_tests_properties(test_coqueued_open_margin_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_daily_magnifier_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/daily_magnifier/bars.inc")
    pineforge_add_unit_test(test_daily_magnifier_tapes)
elseif(NOT test_daily_magnifier_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_daily_magnifier_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/daily_magnifier/bars.inc")
    set_tests_properties(test_daily_magnifier_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_dropped_reversal_mc_first_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_dropped_reversal_mc_first_l4c.cpp")
    pineforge_add_unit_test(test_dropped_reversal_mc_first_l4c)
elseif(NOT test_dropped_reversal_mc_first_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_dropped_reversal_mc_first_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_dropped_reversal_mc_first_l4c.cpp")
    set_tests_properties(test_dropped_reversal_mc_first_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_dual_stop_transactions_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_dual_stop_transactions_l4d.cpp")
    pineforge_add_unit_test(test_dual_stop_transactions_l4d)
elseif(NOT test_dual_stop_transactions_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_dual_stop_transactions_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_dual_stop_transactions_l4d.cpp")
    set_tests_properties(test_dual_stop_transactions_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_e19_excursion_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/e19_excursion_tape/bars.inc")
    pineforge_add_unit_test(test_e19_excursion_tape)
elseif(NOT test_e19_excursion_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_e19_excursion_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/e19_excursion_tape/bars.inc")
    set_tests_properties(test_e19_excursion_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_entry_bar_margin_path_l4a IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_entry_bar_margin_path_l4a.cpp")
    pineforge_add_unit_test(test_entry_bar_margin_path_l4a)
elseif(NOT test_entry_bar_margin_path_l4a IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_entry_bar_margin_path_l4a COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_entry_bar_margin_path_l4a.cpp")
    set_tests_properties(test_entry_bar_margin_path_l4a PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_exit_fifo_cross_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_fifo_cross/bars.inc")
    pineforge_add_unit_test(test_exit_fifo_cross_tapes)
elseif(NOT test_exit_fifo_cross_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_exit_fifo_cross_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/exit_fifo_cross/bars.inc")
    set_tests_properties(test_exit_fifo_cross_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_exit_queue_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_queue/bars.inc")
    pineforge_add_unit_test(test_exit_queue_tapes)
elseif(NOT test_exit_queue_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_exit_queue_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/exit_queue/bars.inc")
    set_tests_properties(test_exit_queue_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_exit_reissue_na_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_reissue_na/bars.inc")
    pineforge_add_unit_test(test_exit_reissue_na_tapes)
elseif(NOT test_exit_reissue_na_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_exit_reissue_na_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/exit_reissue_na/bars.inc")
    set_tests_properties(test_exit_reissue_na_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_famag_close_survives_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_famag_close_survives_data.hpp")
    pineforge_add_unit_test(test_famag_close_survives_l4d)
elseif(NOT test_famag_close_survives_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_famag_close_survives_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_famag_close_survives_data.hpp")
    set_tests_properties(test_famag_close_survives_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_global_exit_children_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/global_exit_children/known_divergences.inc")
    pineforge_add_unit_test(test_global_exit_children_tapes)
elseif(NOT test_global_exit_children_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_global_exit_children_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/global_exit_children/known_divergences.inc")
    set_tests_properties(test_global_exit_children_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_global_exit_fill_size_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/global_exit_fill_size/bars.inc")
    pineforge_add_unit_test(test_global_exit_fill_size_tapes)
elseif(NOT test_global_exit_fill_size_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_global_exit_fill_size_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/global_exit_fill_size/bars.inc")
    set_tests_properties(test_global_exit_fill_size_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_grid_close_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_queue/bars.inc")
    pineforge_add_unit_test(test_grid_close_tapes)
elseif(NOT test_grid_close_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_grid_close_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/exit_queue/bars.inc")
    set_tests_properties(test_grid_close_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_htf_weekly_lookahead IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_htf_weekly_lookahead_data.hpp")
    pineforge_add_unit_test(test_htf_weekly_lookahead)
elseif(NOT test_htf_weekly_lookahead IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_htf_weekly_lookahead COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_htf_weekly_lookahead_data.hpp")
    set_tests_properties(test_htf_weekly_lookahead PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_int29_rule_compositions IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/int29_compositions/bars.inc")
    pineforge_add_unit_test(test_int29_rule_compositions)
elseif(NOT test_int29_rule_compositions IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_int29_rule_compositions COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/int29_compositions/bars.inc")
    set_tests_properties(test_int29_rule_compositions PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_intraday_cap_tv_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/intraday_cap_tv/bars.inc")
    pineforge_add_unit_test(test_intraday_cap_tv_tapes)
elseif(NOT test_intraday_cap_tv_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_intraday_cap_tv_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/intraday_cap_tv/bars.inc")
    set_tests_properties(test_intraday_cap_tv_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_intraday_loss_open_fill_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/intraday_loss_open_fill/bars.inc")
    pineforge_add_unit_test(test_intraday_loss_open_fill_tape)
elseif(NOT test_intraday_loss_open_fill_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_intraday_loss_open_fill_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/intraday_loss_open_fill/bars.inc")
    set_tests_properties(test_intraday_loss_open_fill_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ki62_same_id_cover IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ki62_same_id_cover/bars.inc")
    pineforge_add_unit_test(test_ki62_same_id_cover)
elseif(NOT test_ki62_same_id_cover IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ki62_same_id_cover COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/ki62_same_id_cover/bars.inc")
    set_tests_properties(test_ki62_same_id_cover PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10ab_same_bar_reentry_exit_level IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10ab_same_bar_reentry_exit_level.cpp")
    pineforge_add_unit_test(test_l10ab_same_bar_reentry_exit_level)
elseif(NOT test_l10ab_same_bar_reentry_exit_level IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10ab_same_bar_reentry_exit_level COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10ab_same_bar_reentry_exit_level.cpp")
    set_tests_properties(test_l10ab_same_bar_reentry_exit_level PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10ac_full_equity_single_lot IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10ac_full_equity_single_lot.cpp")
    pineforge_add_unit_test(test_l10ac_full_equity_single_lot)
elseif(NOT test_l10ac_full_equity_single_lot IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10ac_full_equity_single_lot COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10ac_full_equity_single_lot.cpp")
    set_tests_properties(test_l10ac_full_equity_single_lot PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10ag_reversal_close_cycle IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10ag_reversal_close_cycle.cpp")
    pineforge_add_unit_test(test_l10ag_reversal_close_cycle)
elseif(NOT test_l10ag_reversal_close_cycle IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10ag_reversal_close_cycle COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10ag_reversal_close_cycle.cpp")
    set_tests_properties(test_l10ag_reversal_close_cycle PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10aj_margin_full_exit_flat IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10aj_margin_full_exit_flat.cpp")
    pineforge_add_unit_test(test_l10aj_margin_full_exit_flat)
elseif(NOT test_l10aj_margin_full_exit_flat IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10aj_margin_full_exit_flat COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10aj_margin_full_exit_flat.cpp")
    set_tests_properties(test_l10aj_margin_full_exit_flat PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10al_reversal_single_lot IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10al_reversal_single_lot.cpp")
    pineforge_add_unit_test(test_l10al_reversal_single_lot)
elseif(NOT test_l10al_reversal_single_lot IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10al_reversal_single_lot COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10al_reversal_single_lot.cpp")
    set_tests_properties(test_l10al_reversal_single_lot PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10am_entry_bar_priced_exit IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10am_entry_bar_priced_exit.cpp")
    pineforge_add_unit_test(test_l10am_entry_bar_priced_exit)
elseif(NOT test_l10am_entry_bar_priced_exit IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10am_entry_bar_priced_exit COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10am_entry_bar_priced_exit.cpp")
    set_tests_properties(test_l10am_entry_bar_priced_exit PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10ao_percent_equity_first_lot IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10ao_percent_equity_first_lot.cpp")
    pineforge_add_unit_test(test_l10ao_percent_equity_first_lot)
elseif(NOT test_l10ao_percent_equity_first_lot IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10ao_percent_equity_first_lot COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10ao_percent_equity_first_lot.cpp")
    set_tests_properties(test_l10ao_percent_equity_first_lot PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10ap_coof_chained_fill_price IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10ap_coof_chained_fill_price.cpp")
    pineforge_add_unit_test(test_l10ap_coof_chained_fill_price)
elseif(NOT test_l10ap_coof_chained_fill_price IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10ap_coof_chained_fill_price COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10ap_coof_chained_fill_price.cpp")
    set_tests_properties(test_l10ap_coof_chained_fill_price PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10ar_priced_entry_fill IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10ar_priced_entry_fill.cpp")
    pineforge_add_unit_test(test_l10ar_priced_entry_fill)
elseif(NOT test_l10ar_priced_entry_fill IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10ar_priced_entry_fill COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10ar_priced_entry_fill.cpp")
    set_tests_properties(test_l10ar_priced_entry_fill PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10as_priced_exit_fill IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10as_priced_exit_fill.cpp")
    pineforge_add_unit_test(test_l10as_priced_exit_fill)
elseif(NOT test_l10as_priced_exit_fill IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10as_priced_exit_fill COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10as_priced_exit_fill.cpp")
    set_tests_properties(test_l10as_priced_exit_fill PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10b_excursion_parity IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10b_excursion_parity.cpp")
    pineforge_add_unit_test(test_l10b_excursion_parity)
elseif(NOT test_l10b_excursion_parity IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10b_excursion_parity COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10b_excursion_parity.cpp")
    set_tests_properties(test_l10b_excursion_parity PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10d_entry_bar_excursion_masks IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10d_entry_bar_excursion_masks.cpp")
    pineforge_add_unit_test(test_l10d_entry_bar_excursion_masks)
elseif(NOT test_l10d_entry_bar_excursion_masks IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10d_entry_bar_excursion_masks COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10d_entry_bar_excursion_masks.cpp")
    set_tests_properties(test_l10d_entry_bar_excursion_masks PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10e_opposite_entry_close_same_pass IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10e_opposite_entry_close_same_pass.cpp")
    pineforge_add_unit_test(test_l10e_opposite_entry_close_same_pass)
elseif(NOT test_l10e_opposite_entry_close_same_pass IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10e_opposite_entry_close_same_pass COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10e_opposite_entry_close_same_pass.cpp")
    set_tests_properties(test_l10e_opposite_entry_close_same_pass PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10f_sizing_basis_stop_limit IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10f_sizing_basis_stop_limit.cpp")
    pineforge_add_unit_test(test_l10f_sizing_basis_stop_limit)
elseif(NOT test_l10f_sizing_basis_stop_limit IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10f_sizing_basis_stop_limit COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10f_sizing_basis_stop_limit.cpp")
    set_tests_properties(test_l10f_sizing_basis_stop_limit PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10g_oca_reduce_deferred_flip IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10g_oca_reduce_deferred_flip.cpp")
    pineforge_add_unit_test(test_l10g_oca_reduce_deferred_flip)
elseif(NOT test_l10g_oca_reduce_deferred_flip IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10g_oca_reduce_deferred_flip COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10g_oca_reduce_deferred_flip.cpp")
    set_tests_properties(test_l10g_oca_reduce_deferred_flip PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10h_corpus_parity IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10h_corpus_parity.cpp")
    pineforge_add_unit_test(test_l10h_corpus_parity)
elseif(NOT test_l10h_corpus_parity IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10h_corpus_parity COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10h_corpus_parity.cpp")
    set_tests_properties(test_l10h_corpus_parity PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10j_corpus_parity IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10j_corpus_parity.cpp")
    pineforge_add_unit_test(test_l10j_corpus_parity)
elseif(NOT test_l10j_corpus_parity IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10j_corpus_parity COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10j_corpus_parity.cpp")
    set_tests_properties(test_l10j_corpus_parity PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10k_corpus_parity IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10k_corpus_parity.cpp")
    pineforge_add_unit_test(test_l10k_corpus_parity)
elseif(NOT test_l10k_corpus_parity IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10k_corpus_parity COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10k_corpus_parity.cpp")
    set_tests_properties(test_l10k_corpus_parity PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10l_corpus_parity IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10l_corpus_parity.cpp")
    pineforge_add_unit_test(test_l10l_corpus_parity)
elseif(NOT test_l10l_corpus_parity IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10l_corpus_parity COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10l_corpus_parity.cpp")
    set_tests_properties(test_l10l_corpus_parity PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10m_corpus_parity IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10m_corpus_parity.cpp")
    pineforge_add_unit_test(test_l10m_corpus_parity)
elseif(NOT test_l10m_corpus_parity IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10m_corpus_parity COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10m_corpus_parity.cpp")
    set_tests_properties(test_l10m_corpus_parity PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10p_three_way_entry_bar_exit IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10p_three_way_entry_bar_exit.cpp")
    pineforge_add_unit_test(test_l10p_three_way_entry_bar_exit)
elseif(NOT test_l10p_three_way_entry_bar_exit IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10p_three_way_entry_bar_exit COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10p_three_way_entry_bar_exit.cpp")
    set_tests_properties(test_l10p_three_way_entry_bar_exit PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10q_pyramid_terrace_exit IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10q_pyramid_terrace_exit.cpp")
    pineforge_add_unit_test(test_l10q_pyramid_terrace_exit)
elseif(NOT test_l10q_pyramid_terrace_exit IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10q_pyramid_terrace_exit COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10q_pyramid_terrace_exit.cpp")
    set_tests_properties(test_l10q_pyramid_terrace_exit PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10r_dual_stop_same_bar_order IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10r_dual_stop_same_bar_order.cpp")
    pineforge_add_unit_test(test_l10r_dual_stop_same_bar_order)
elseif(NOT test_l10r_dual_stop_same_bar_order IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10r_dual_stop_same_bar_order COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10r_dual_stop_same_bar_order.cpp")
    set_tests_properties(test_l10r_dual_stop_same_bar_order PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10recon1_merge_fill_liveness IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10recon1_merge_fill_liveness.cpp")
    pineforge_add_unit_test(test_l10recon1_merge_fill_liveness)
elseif(NOT test_l10recon1_merge_fill_liveness IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10recon1_merge_fill_liveness COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10recon1_merge_fill_liveness.cpp")
    set_tests_properties(test_l10recon1_merge_fill_liveness PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10s_pyramiding_same_bar_ids IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10s_pyramiding_same_bar_ids.cpp")
    pineforge_add_unit_test(test_l10s_pyramiding_same_bar_ids)
elseif(NOT test_l10s_pyramiding_same_bar_ids IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10s_pyramiding_same_bar_ids COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10s_pyramiding_same_bar_ids.cpp")
    set_tests_properties(test_l10s_pyramiding_same_bar_ids PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10t_partial_exit_residual IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10t_partial_exit_residual.cpp")
    pineforge_add_unit_test(test_l10t_partial_exit_residual)
elseif(NOT test_l10t_partial_exit_residual IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10t_partial_exit_residual COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10t_partial_exit_residual.cpp")
    set_tests_properties(test_l10t_partial_exit_residual PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10v_first_partial_leg_timing IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10v_first_partial_leg_timing.cpp")
    pineforge_add_unit_test(test_l10v_first_partial_leg_timing)
elseif(NOT test_l10v_first_partial_leg_timing IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10v_first_partial_leg_timing COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10v_first_partial_leg_timing.cpp")
    set_tests_properties(test_l10v_first_partial_leg_timing PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10w_zero_qty_entry IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10w_zero_qty_entry.cpp")
    pineforge_add_unit_test(test_l10w_zero_qty_entry)
elseif(NOT test_l10w_zero_qty_entry IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10w_zero_qty_entry COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10w_zero_qty_entry.cpp")
    set_tests_properties(test_l10w_zero_qty_entry PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10x_reissued_limit_touch_bar IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10x_reissued_limit_touch_bar.cpp")
    pineforge_add_unit_test(test_l10x_reissued_limit_touch_bar)
elseif(NOT test_l10x_reissued_limit_touch_bar IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10x_reissued_limit_touch_bar COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10x_reissued_limit_touch_bar.cpp")
    set_tests_properties(test_l10x_reissued_limit_touch_bar PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10y_full_equity_entry_split IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10y_full_equity_entry_split.cpp")
    pineforge_add_unit_test(test_l10y_full_equity_entry_split)
elseif(NOT test_l10y_full_equity_entry_split IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10y_full_equity_entry_split COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10y_full_equity_entry_split.cpp")
    set_tests_properties(test_l10y_full_equity_entry_split PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l10z_review_fixes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l10z_review_fixes.cpp")
    pineforge_add_unit_test(test_l10z_review_fixes)
elseif(NOT test_l10z_review_fixes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l10z_review_fixes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l10z_review_fixes.cpp")
    set_tests_properties(test_l10z_review_fixes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l11a_host_excursion_twin IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l11a_host_excursion_twin.cpp")
    pineforge_add_unit_test(test_l11a_host_excursion_twin)
elseif(NOT test_l11a_host_excursion_twin IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l11a_host_excursion_twin COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l11a_host_excursion_twin.cpp")
    set_tests_properties(test_l11a_host_excursion_twin PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l11b_same_bar_stopout_excursion IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l11b_same_bar_stopout_excursion.cpp")
    pineforge_add_unit_test(test_l11b_same_bar_stopout_excursion)
elseif(NOT test_l11b_same_bar_stopout_excursion IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l11b_same_bar_stopout_excursion COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l11b_same_bar_stopout_excursion.cpp")
    set_tests_properties(test_l11b_same_bar_stopout_excursion PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_l4f_security_magnifier IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_l4f_security_magnifier.cpp")
    pineforge_add_unit_test(test_l4f_security_magnifier)
elseif(NOT test_l4f_security_magnifier IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_l4f_security_magnifier COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_l4f_security_magnifier.cpp")
    set_tests_properties(test_l4f_security_magnifier PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_limit_entry_affordability_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/limit_entry_affordability/bars.inc")
    pineforge_add_unit_test(test_limit_entry_affordability_tape)
elseif(NOT test_limit_entry_affordability_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_limit_entry_affordability_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/limit_entry_affordability/bars.inc")
    set_tests_properties(test_limit_entry_affordability_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ltf_lookahead_first_bucket IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ltf_lookahead_first_bucket_data.hpp")
    pineforge_add_unit_test(test_ltf_lookahead_first_bucket)
elseif(NOT test_ltf_lookahead_first_bucket IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ltf_lookahead_first_bucket COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_ltf_lookahead_first_bucket_data.hpp")
    set_tests_properties(test_ltf_lookahead_first_bucket PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_m_admission_36_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_m_admission_36_data.hpp")
    pineforge_add_unit_test(test_m_admission_36_l4d)
elseif(NOT test_m_admission_36_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_m_admission_36_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_m_admission_36_data.hpp")
    set_tests_properties(test_m_admission_36_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_magnified_coof_refill_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnified_coof_refill/bars_1m.inc")
    pineforge_add_unit_test(test_magnified_coof_refill_tape)
elseif(NOT test_magnified_coof_refill_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_magnified_coof_refill_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/magnified_coof_refill/bars_1m.inc")
    set_tests_properties(test_magnified_coof_refill_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_margin_residual_resolved_fill IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_margin_residual_resolved_fill.cpp")
    pineforge_add_unit_test(test_margin_residual_resolved_fill)
elseif(NOT test_margin_residual_resolved_fill IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_margin_residual_resolved_fill COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_margin_residual_resolved_fill.cpp")
    set_tests_properties(test_margin_residual_resolved_fill PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_market_admission_commission_l4b IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_market_admission_commission_data.hpp")
    pineforge_add_unit_test(test_market_admission_commission_l4b)
elseif(NOT test_market_admission_commission_l4b IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_market_admission_commission_l4b COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_market_admission_commission_data.hpp")
    set_tests_properties(test_market_admission_commission_l4b PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_auto_path IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_auto_path.cpp")
    pineforge_add_unit_test(test_native_auto_path)
elseif(NOT test_native_auto_path IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_auto_path COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_auto_path.cpp")
    set_tests_properties(test_native_auto_path PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_host_repairs IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_host_repairs.cpp")
    pineforge_add_unit_test(test_native_host_repairs)
elseif(NOT test_native_host_repairs IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_host_repairs COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_host_repairs.cpp")
    set_tests_properties(test_native_host_repairs PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_price_grid_twin IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_price_grid_twin.cpp")
    pineforge_add_unit_test(test_native_price_grid_twin)
elseif(NOT test_native_price_grid_twin IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_price_grid_twin COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_price_grid_twin.cpp")
    set_tests_properties(test_native_price_grid_twin PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_selected_settlement IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_selected_settlement.cpp")
    pineforge_add_unit_test(test_native_selected_settlement)
elseif(NOT test_native_selected_settlement IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_selected_settlement COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_selected_settlement.cpp")
    set_tests_properties(test_native_selected_settlement PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_native_settlement_projection IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_native_settlement_projection.cpp")
    pineforge_add_unit_test(test_native_settlement_projection)
elseif(NOT test_native_settlement_projection IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_native_settlement_projection COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_native_settlement_projection.cpp")
    set_tests_properties(test_native_settlement_projection PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_nonfinite_entry_qty_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_refill_waypoint/bars.inc")
    pineforge_add_unit_test(test_nonfinite_entry_qty_tape)
elseif(NOT test_nonfinite_entry_qty_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_nonfinite_entry_qty_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_refill_waypoint/bars.inc")
    set_tests_properties(test_nonfinite_entry_qty_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_open_entry_fee_charged IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/open_entry_fee_charged/bars.inc")
    pineforge_add_unit_test(test_open_entry_fee_charged)
elseif(NOT test_open_entry_fee_charged IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_open_entry_fee_charged COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/open_entry_fee_charged/bars.inc")
    set_tests_properties(test_open_entry_fee_charged PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pending_order_v1_canary_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pending_order_prefix/c45-fields.inc")
    pineforge_add_unit_test(test_pending_order_v1_canary_l4d)
elseif(NOT test_pending_order_v1_canary_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pending_order_v1_canary_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pending_order_prefix/c45-fields.inc")
    set_tests_properties(test_pending_order_v1_canary_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_percent_entry_reservation_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/percent_entry_reservation/bars.inc")
    pineforge_add_unit_test(test_percent_entry_reservation_tapes)
elseif(NOT test_percent_entry_reservation_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_percent_entry_reservation_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/percent_entry_reservation/bars.inc")
    set_tests_properties(test_percent_entry_reservation_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pine_dust_sweep_paired IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/g232_decimal_dust/bars.inc")
    pineforge_add_unit_test(test_pine_dust_sweep_paired)
elseif(NOT test_pine_dust_sweep_paired IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pine_dust_sweep_paired COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/g232_decimal_dust/bars.inc")
    set_tests_properties(test_pine_dust_sweep_paired PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_add_close_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_add_close/bars.inc")
    pineforge_add_unit_test(test_pooc_add_close_tape)
elseif(NOT test_pooc_add_close_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_add_close_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_add_close/bars.inc")
    set_tests_properties(test_pooc_add_close_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_margin_call_sizing_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_margin_call_sizing/bars.inc")
    pineforge_add_unit_test(test_pooc_margin_call_sizing_tapes)
elseif(NOT test_pooc_margin_call_sizing_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_margin_call_sizing_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_margin_call_sizing/bars.inc")
    set_tests_properties(test_pooc_margin_call_sizing_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_pair_gross_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_pair_gross/bars.inc")
    pineforge_add_unit_test(test_pooc_pair_gross_tapes)
elseif(NOT test_pooc_pair_gross_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_pair_gross_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_pair_gross/bars.inc")
    set_tests_properties(test_pooc_pair_gross_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_same_pass_close_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_same_pass_close/bars.inc")
    pineforge_add_unit_test(test_pooc_same_pass_close_tape)
elseif(NOT test_pooc_same_pass_close_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_same_pass_close_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_same_pass_close/bars.inc")
    set_tests_properties(test_pooc_same_pass_close_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pooc_short_slip_admission_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_short_slip_admission/bars.inc")
    pineforge_add_unit_test(test_pooc_short_slip_admission_tapes)
elseif(NOT test_pooc_short_slip_admission_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pooc_short_slip_admission_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pooc_short_slip_admission/bars.inc")
    set_tests_properties(test_pooc_short_slip_admission_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_pyramiding_open_order_tape IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pyramiding_open_order/bars.inc")
    pineforge_add_unit_test(test_pyramiding_open_order_tape)
elseif(NOT test_pyramiding_open_order_tape IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_pyramiding_open_order_tape COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/pyramiding_open_order/bars.inc")
    set_tests_properties(test_pyramiding_open_order_tape PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_same_bar_reissue_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tvdef_drops/bars.inc")
    pineforge_add_unit_test(test_same_bar_reissue_tapes)
elseif(NOT test_same_bar_reissue_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_same_bar_reissue_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/tvdef_drops/bars.inc")
    set_tests_properties(test_same_bar_reissue_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_same_point_entries_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tvdef_drops/bars.inc")
    pineforge_add_unit_test(test_same_point_entries_tapes)
elseif(NOT test_same_point_entries_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_same_point_entries_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/tvdef_drops/bars.inc")
    set_tests_properties(test_same_point_entries_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_session_transition_clock_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_transition_clock/london-autumn-round2/provenance.json")
    pineforge_add_unit_test(test_session_transition_clock_tapes)
elseif(NOT test_session_transition_clock_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_session_transition_clock_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/session_transition_clock/london-autumn-round2/provenance.json")
    set_tests_properties(test_session_transition_clock_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()
if(NOT test_samebar_pyramiding_entries_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/samebar_pyramiding_entries/fallback-S2/case.hpp")
    pineforge_add_unit_test(test_samebar_pyramiding_entries_tapes)
elseif(NOT test_samebar_pyramiding_entries_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_samebar_pyramiding_entries_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/samebar_pyramiding_entries/fallback-S2/case.hpp")
    set_tests_properties(test_samebar_pyramiding_entries_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_short_seed_report_swap IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/g221_short_seed/bars.inc")
    pineforge_add_unit_test(test_short_seed_report_swap)
elseif(NOT test_short_seed_report_swap IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_short_seed_report_swap COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/g221_short_seed/bars.inc")
    set_tests_properties(test_short_seed_report_swap PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_small_money_margin_residual_l4a IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_small_money_margin_residual_l4a.cpp")
    pineforge_add_unit_test(test_small_money_margin_residual_l4a)
elseif(NOT test_small_money_margin_residual_l4a IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_small_money_margin_residual_l4a COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_small_money_margin_residual_l4a.cpp")
    set_tests_properties(test_small_money_margin_residual_l4a PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_source_layer_native_defaults IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_source_layer_native_defaults.cpp")
    pineforge_add_unit_test(test_source_layer_native_defaults)
elseif(NOT test_source_layer_native_defaults IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_source_layer_native_defaults COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_source_layer_native_defaults.cpp")
    set_tests_properties(test_source_layer_native_defaults PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_indicators_extras IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/percentile_window/bars.inc")
    pineforge_add_unit_test(test_ta_indicators_extras)
elseif(NOT test_ta_indicators_extras IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_indicators_extras COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/percentile_window/bars.inc")
    set_tests_properties(test_ta_indicators_extras PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_series_length_extremes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ta_series_length_extremes.cpp")
    pineforge_add_unit_test(test_ta_series_length_extremes)
elseif(NOT test_ta_series_length_extremes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_series_length_extremes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_ta_series_length_extremes.cpp")
    set_tests_properties(test_ta_series_length_extremes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_series_length_supertrend IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ta_series_length_supertrend.cpp")
    pineforge_add_unit_test(test_ta_series_length_supertrend)
elseif(NOT test_ta_series_length_supertrend IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_series_length_supertrend COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_ta_series_length_supertrend.cpp")
    set_tests_properties(test_ta_series_length_supertrend PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_simple_length IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ta_simple_length.cpp")
    pineforge_add_unit_test(test_ta_simple_length)
elseif(NOT test_ta_simple_length IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_simple_length COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_ta_simple_length.cpp")
    set_tests_properties(test_ta_simple_length PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_sma_stochrsi_ties IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ta_sma_stochrsi_ties.cpp")
    pineforge_add_unit_test(test_ta_sma_stochrsi_ties)
elseif(NOT test_ta_sma_stochrsi_ties IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_sma_stochrsi_ties COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_ta_sma_stochrsi_ties.cpp")
    set_tests_properties(test_ta_sma_stochrsi_ties PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_voltrend_edge IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/supertrend_warmup/bars.inc")
    pineforge_add_unit_test(test_ta_voltrend_edge)
elseif(NOT test_ta_voltrend_edge IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_voltrend_edge COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/supertrend_warmup/bars.inc")
    set_tests_properties(test_ta_voltrend_edge PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_window_round10 IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ta_window_round10.cpp")
    pineforge_add_unit_test(test_ta_window_round10)
elseif(NOT test_ta_window_round10 IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_window_round10 COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_ta_window_round10.cpp")
    set_tests_properties(test_ta_window_round10 PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_ta_window_underapplied IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ta_window_underapplied.cpp")
    pineforge_add_unit_test(test_ta_window_underapplied)
elseif(NOT test_ta_window_underapplied IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_ta_window_underapplied COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_ta_window_underapplied.cpp")
    set_tests_properties(test_ta_window_underapplied PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_trail_close_restart_no_fold_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_trail_close_restart_no_fold_data.hpp")
    pineforge_add_unit_test(test_trail_close_restart_no_fold_l4c)
elseif(NOT test_trail_close_restart_no_fold_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_trail_close_restart_no_fold_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_trail_close_restart_no_fold_data.hpp")
    set_tests_properties(test_trail_close_restart_no_fold_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_trail_ref_entry_bar_extreme_l4c IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_trail_ref_entry_bar_extreme_l4c.cpp")
    pineforge_add_unit_test(test_trail_ref_entry_bar_extreme_l4c)
elseif(NOT test_trail_ref_entry_bar_extreme_l4c IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_trail_ref_entry_bar_extreme_l4c COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_trail_ref_entry_bar_extreme_l4c.cpp")
    set_tests_properties(test_trail_ref_entry_bar_extreme_l4c PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_tv_fill_rounding_l4d IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_tv_fill_rounding_l4d.cpp")
    pineforge_add_unit_test(test_tv_fill_rounding_l4d)
elseif(NOT test_tv_fill_rounding_l4d IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_tv_fill_rounding_l4d COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_tv_fill_rounding_l4d.cpp")
    set_tests_properties(test_tv_fill_rounding_l4d PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_tv_money_band_l4b IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_tv_money_band_data.hpp")
    pineforge_add_unit_test(test_tv_money_band_l4b)
elseif(NOT test_tv_money_band_l4b IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_tv_money_band_l4b COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_tv_money_band_data.hpp")
    set_tests_properties(test_tv_money_band_l4b PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_tv_money_long_margin_call_eth_l4a IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_tv_money_long_margin_call_eth_l4a.cpp")
    pineforge_add_unit_test(test_tv_money_long_margin_call_eth_l4a)
elseif(NOT test_tv_money_long_margin_call_eth_l4a IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_tv_money_long_margin_call_eth_l4a COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_tv_money_long_margin_call_eth_l4a.cpp")
    set_tests_properties(test_tv_money_long_margin_call_eth_l4a PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_tv_money_precision_l4b IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_tv_money_precision_data.hpp")
    pineforge_add_unit_test(test_tv_money_precision_l4b)
elseif(NOT test_tv_money_precision_l4b IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_tv_money_precision_l4b COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/test_tv_money_precision_data.hpp")
    set_tests_properties(test_tv_money_precision_l4b PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_tv_strategy_defaults IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tv_strategy_defaults/bars.inc")
    pineforge_add_unit_test(test_tv_strategy_defaults)
elseif(NOT test_tv_strategy_defaults IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_tv_strategy_defaults COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/tv_strategy_defaults/bars.inc")
    set_tests_properties(test_tv_strategy_defaults PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

if(NOT test_tvdef_drops_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tvdef_drops/bars.inc")
    pineforge_add_unit_test(test_tvdef_drops_tapes)
elseif(NOT test_tvdef_drops_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_tvdef_drops_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/tvdef_drops/bars.inc")
    set_tests_properties(test_tvdef_drops_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()

# The fixture-dir compile definitions. They apply to targets that exist,
# so the pass runs here -- after every guarded registration above, including
# the private-fixture rows and the split rows beside them. Before PR 1 the
# if(TARGET ...) guards below ran ahead of those registrations, so a row
# registered on a fixture-present branch compiled without its
# PINEFORGE_*_FIXTURE_DIR definition and hit its own #error. Here each
# pinned row's definition still runs after its target exists, and the
# ORDER_PRINT loop sees every tape target it names.

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

# R5 lane B-ADAPTER (item 5): the current-execution witness reads the kernel.
if(TARGET test_native_current_execution_rate)
    target_compile_definitions(test_native_current_execution_rate PRIVATE
        PINEFORGE_B_ADAPTER_KERNEL_FILES="${PROJECT_SOURCE_DIR}/src/engine_execution.cpp|${PROJECT_SOURCE_DIR}/src/native_execution_consumer.cpp")
endif()
# R5 lane B-ADAPTER (item 3): the range-end witness reads the source layer.
if(TARGET test_adapter_range_end_fx)
    target_compile_definitions(test_adapter_range_end_fx PRIVATE
        PINEFORGE_B_ADAPTER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/source")
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

# R5 lane H-MEASURE: the rows that read TradingView tapes from the fixture tree.
foreach(_pf_hm_row IN ITEMS
        "test_pyramiding_count_differential|PINEFORGE_PYRAMIDING_FIXTURE_DIR|pyramiding_count"
        "test_zero_trail_sibling_stop|PINEFORGE_ZERO_TRAIL_FIXTURE_DIR|zero_trail_sibling_stop"
        "test_pending_entry_trail_tapes|PINEFORGE_PENDING_TRAIL_FIXTURE_DIR|pending_entry_trail"
        "test_short_seed_report_swap|PINEFORGE_G221_FIXTURE_DIR|g221_short_seed"
        "test_pine_dust_sweep_paired|PINEFORGE_G232_FIXTURE_DIR|g232_decimal_dust"
        "test_session_ismarket_tape|PINEFORGE_G236_FIXTURE_DIR|session_ismarket"
        "test_aggregated_entry_bar_index_tape|PINEFORGE_HM_F1E_FIXTURE_DIR|aggregated_entry_bar_index"
        "test_magnified_aggregated_tape|PINEFORGE_HM_MAG_FIXTURE_DIR|magnified_aggregated"
        "test_adapter_margin_schedule_differential|PINEFORGE_HM_M7A_FIXTURE_DIR|margin_entry_bar"
        # R5 lane PAR-ORDERS-2's tape rows.
        "test_flat_coof_exit_tapes|PINEFORGE_PA2_FLAT_COOF_FIXTURE_DIR|flat_coof_exit"
        "test_second_extreme_order_tapes|PINEFORGE_PA2_SECOND_EXTREME_FIXTURE_DIR|second_extreme_orders"
        "test_pooc_reversing_stop_tapes|PINEFORGE_PA2_REVERSING_STOP_FIXTURE_DIR|pooc_reversing_stop"
        # R5 lane MAG-INTRABAR's tape row.
        "test_adapter_magnifier_intrabar_tapes|PINEFORGE_MI_FIXTURE_DIR|magnifier_intrabars"
        # R5 lane PAR-ORDERS-3's tape row.
        "test_pooc_limit_close_tapes|PINEFORGE_PA3_POOC_LIMIT_FIXTURE_DIR|pooc_limit_close"
        # R5 lane XAU-CAL's tape row.
        "test_daily_magnifier_tapes|PINEFORGE_DAILY_MAGNIFIER_FIXTURE_DIR|daily_magnifier"
        # R5 lane FIX-E1E2's tape row.
        "test_aggregated_admission_receipt_tapes|PINEFORGE_FIX_E1E2_FIXTURE_DIR|aggregated_admission_receipt")
    string(REPLACE "|" ";" _pf_hm_fields "${_pf_hm_row}")
    list(GET _pf_hm_fields 0 _pf_hm_target)
    list(GET _pf_hm_fields 1 _pf_hm_macro)
    list(GET _pf_hm_fields 2 _pf_hm_dir)
    if(TARGET ${_pf_hm_target})
        target_compile_definitions(${_pf_hm_target} PRIVATE
            ${_pf_hm_macro}="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_pf_hm_dir}")
    endif()
endforeach()

# R5 lane PAR-CASHFEE reads its TradingView tapes from the fixture tree too.
if(TARGET test_cash_fee_sizing_tapes)
    target_compile_definitions(test_cash_fee_sizing_tapes PRIVATE
        PINEFORGE_CASH_FEE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cash_fee_sizing")
endif()
# R5 lane INT26 (round 2) reads its TradingView tapes from the fixture tree too.
if(TARGET test_pooc_stop_reentry_bracket_tapes)
    target_compile_definitions(test_pooc_stop_reentry_bracket_tapes PRIVATE
        PINEFORGE_INT26_REENTRY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_stop_reentry_bracket")
endif()
# R5 lane K-SESSION-WINDOWS reads TradingView's session-flag tapes.
foreach(_pf_ksw_target IN ITEMS test_session_prepost_windows_tape
        test_native_session_day_bar_open test_session_extended_day_tape
        test_session_break_open_bar_tape)
    if(TARGET ${_pf_ksw_target})
        target_compile_definitions(${_pf_ksw_target} PRIVATE
            PINEFORGE_KSW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_windows")
    endif()
endforeach()

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

# Lane TV-DEFAULTS reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_tv_strategy_defaults)
    target_compile_definitions(test_tv_strategy_defaults PRIVATE
        PINEFORGE_TVD_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tv_strategy_defaults")
endif()

# Lane TVDEF-DROPS reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_tvdef_drops_tapes)
    target_compile_definitions(test_tvdef_drops_tapes PRIVATE
        PINEFORGE_TVDEF_DROPS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tvdef_drops")
endif()

# Lane W3-ENG-EXIT-ALLOC reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_exit_queue_tapes)
    target_compile_definitions(test_exit_queue_tapes PRIVATE
        PINEFORGE_EXIT_QUEUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_queue")
endif()

# Lane W3B-ENG-GRID reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_grid_close_tapes)
    target_compile_definitions(test_grid_close_tapes PRIVATE
        PINEFORGE_GRID_CLOSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/grid_close")
endif()

# Lane W5-ENG-MARGIN-V6 reads its TradingView tapes from the fixture tree;
# source-bound.
if(TARGET test_margin_v6_tapes)
    target_compile_definitions(test_margin_v6_tapes PRIVATE
        PINEFORGE_MARGIN_V6_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_v6")
endif()

# Lane W6-ENG-FILL-ORDER reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_same_point_entries_tapes)
    target_compile_definitions(test_same_point_entries_tapes PRIVATE
        PINEFORGE_SAME_POINT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/same_point_entries")
endif()

# Lane W8C-SECURITY reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_ltf_lookahead_calling_open_tapes)
    target_compile_definitions(test_ltf_lookahead_calling_open_tapes PRIVATE
        PINEFORGE_LTF_CALLING_OPEN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ltf_lookahead_calling_open")
endif()

# Lane TAIL-A reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_security_multi_day_buckets_tapes)
    target_compile_definitions(test_security_multi_day_buckets_tapes PRIVATE
        PINEFORGE_MULTI_DAY_BUCKETS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/multi_day_buckets")
endif()

# Lane W8D-NOTRADES reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_coof_refill_waypoint_tapes)
    target_compile_definitions(test_coof_refill_waypoint_tapes PRIVATE
        PINEFORGE_COOF_REFILL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_refill_waypoint")
endif()

# Lane TAIL-C reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_coof_w2_refill_tapes)
    target_compile_definitions(test_coof_w2_refill_tapes PRIVATE
        PINEFORGE_COOF_W2_REFILL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_w2_refill")
endif()
if(TARGET test_coof_open_newborn_exit_tape)
    target_compile_definitions(test_coof_open_newborn_exit_tape PRIVATE
        PINEFORGE_COOF_OPEN_NEWBORN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_open_newborn_exit")
endif()
if(TARGET test_magnified_coof_refill_tape)
    target_compile_definitions(test_magnified_coof_refill_tape PRIVATE
        PINEFORGE_MAGNIFIED_REFILL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnified_coof_refill")
endif()

if(TARGET test_pooc_add_close_tape)
    target_compile_definitions(test_pooc_add_close_tape PRIVATE
        PINEFORGE_POOC_ADD_CLOSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_add_close")
endif()

# Lane W8E-EXITS reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_ki62_same_id_cover)
    target_compile_definitions(test_ki62_same_id_cover PRIVATE
        PINEFORGE_KI62_SAME_ID_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ki62_same_id_cover")
endif()

# Lane W4-ENG-POOC-SAMEPASS reads its TradingView tapes from the fixture tree;
# source-bound.
if(TARGET test_pooc_samepass_tapes)
    target_compile_definitions(test_pooc_samepass_tapes PRIVATE
        PINEFORGE_W4_POOC_SAMEPASS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_samepass")
endif()
if(TARGET test_global_exit_fill_size_tapes)
    target_compile_definitions(test_global_exit_fill_size_tapes PRIVATE
        PINEFORGE_W4_GLOBAL_EXIT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/global_exit_fill_size")
endif()
if(TARGET test_coof_immediate_close_tapes)
    target_compile_definitions(test_coof_immediate_close_tapes PRIVATE
        PINEFORGE_TAIL_D_IMMEDIATE_CLOSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_immediate_close")
endif()
if(TARGET test_coof_same_point_exits_tapes)
    target_compile_definitions(test_coof_same_point_exits_tapes PRIVATE
        PINEFORGE_TAIL_D_SAME_POINT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_same_point_exits")
endif()
if(TARGET test_coof_inflight_cancel_tapes)
    target_compile_definitions(test_coof_inflight_cancel_tapes PRIVATE
        PINEFORGE_TAIL_D_INFLIGHT_CANCEL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_inflight_cancel")
endif()
if(TARGET test_coof_resting_bracket_tapes)
    target_compile_definitions(test_coof_resting_bracket_tapes PRIVATE
        PINEFORGE_TAIL_D_RESTING_BRACKET_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_resting_bracket")
endif()
if(TARGET test_coof_timeframe_change_tapes)
    target_compile_definitions(test_coof_timeframe_change_tapes PRIVATE
        PINEFORGE_TAIL_D_TF_CHANGE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_timeframe_change")
endif()
if(TARGET test_exit_fifo_cross_tapes)
    target_compile_definitions(test_exit_fifo_cross_tapes PRIVATE
        PINEFORGE_TAIL_D_FIFO_CROSS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_fifo_cross")
endif()
if(TARGET test_global_exit_children_tapes)
    target_compile_definitions(test_global_exit_children_tapes PRIVATE
        PINEFORGE_GLOBAL_EXIT_CHILDREN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/global_exit_children")
endif()
if(TARGET test_exit_binding_tapes)
    target_compile_definitions(test_exit_binding_tapes PRIVATE
        PINEFORGE_EXIT_BINDING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_binding")
endif()
if(TARGET test_session_transition_clock_tapes)
    target_compile_definitions(test_session_transition_clock_tapes PRIVATE
        PINEFORGE_SESSION_TRANSITION_CLOCK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/session_transition_clock")
endif()
# test_coof_forced_waypoint_tapes #includes its per-case .inc, so the target
# exists only while that include does; the define loop below must still see
# the target when the fixture is present (the guard lives here, before it).
if(NOT test_coof_forced_waypoint_tapes IN_LIST _pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_forced_waypoint/cases.inc")
    pineforge_add_unit_test(test_coof_forced_waypoint_tapes)
elseif(NOT test_coof_forced_waypoint_tapes IN_LIST _pf_kernel_absent_rows)
    add_test(NAME test_coof_forced_waypoint_tapes COMMAND ${CMAKE_COMMAND} -E echo "SKIP private fixtures not present: tests/fixtures/coof_forced_waypoint/cases.inc")
    set_tests_properties(test_coof_forced_waypoint_tapes PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP private fixtures not present: " LABELS private_fixtures)
endif()
foreach(tape_target IN ITEMS test_cross_mechanism_tapes test_carried_trail_open_tapes test_pooc_pending_stop_tapes
                            test_coof_marketable_stop_tapes test_coof_close_slippage_tapes
                            test_coof_forced_waypoint_tapes
                            test_explicit_qty_floor_tapes test_pooc_close_bar_fills_tapes)
    if(TARGET ${tape_target})
        target_compile_definitions(${tape_target} PRIVATE
            PINEFORGE_ORDER_PRINT_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    endif()
endforeach()

if(TARGET test_coof_open_limit_tapes)
    target_compile_definitions(test_coof_open_limit_tapes PRIVATE
        PINEFORGE_TAIL_D_OPEN_LIMIT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_open_limit")
endif()
if(TARGET test_exit_reissue_na_tapes)
    target_compile_definitions(test_exit_reissue_na_tapes PRIVATE
        PINEFORGE_TAIL_D_REISSUE_NA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_reissue_na")
endif()
if(TARGET test_pooc_pair_gross_tapes)
    target_compile_definitions(test_pooc_pair_gross_tapes PRIVATE
        PINEFORGE_TAIL_D_PAIR_GROSS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_pair_gross")
endif()
if(TARGET test_whole_lot_tie_tapes)
    target_compile_definitions(test_whole_lot_tie_tapes PRIVATE
        PINEFORGE_TAIL_D_WHOLE_LOT_TIE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/whole_lot_tie")
endif()
if(TARGET test_source_buying_power_tapes)
    target_compile_definitions(test_source_buying_power_tapes PRIVATE
        PINEFORGE_BUYING_POWER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/source_buying_power")
endif()
if(TARGET test_coof_reissued_exit_tapes)
    target_compile_definitions(test_coof_reissued_exit_tapes PRIVATE
        PINEFORGE_W4_REISSUED_EXIT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_reissued_exit")
endif()
if(TARGET test_coof_fill_bar_exit_tapes)
    target_compile_definitions(test_coof_fill_bar_exit_tapes PRIVATE
        PINEFORGE_W4_COOF_FILL_BAR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_fill_bar_exit")
endif()
if(TARGET test_pooc_margin_call_sizing_tapes)
    target_compile_definitions(test_pooc_margin_call_sizing_tapes PRIVATE
        PINEFORGE_W4_MC_SIZING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_margin_call_sizing")
endif()
if(TARGET test_pooc_short_slip_admission_tapes)
    target_compile_definitions(test_pooc_short_slip_admission_tapes PRIVATE
        PINEFORGE_W4_SHORT_SLIP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_short_slip_admission")
endif()

# Lane W8A-SIGSTATE-1 R4 reads its TradingView tape and daily bars from the
# fixture tree; source-bound.
if(TARGET test_intraday_loss_open_fill_tape)
    target_compile_definitions(test_intraday_loss_open_fill_tape PRIVATE
        PINEFORGE_INTRADAY_LOSS_OPEN_FILL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/intraday_loss_open_fill")
endif()

# Lane W8A-SIGSTATE-1 R-B and R-A read their TradingView tape and 15m bars
# from the fixture tree; source-bound.
if(TARGET test_pyramiding_open_order_tape)
    target_compile_definitions(test_pyramiding_open_order_tape PRIVATE
        PINEFORGE_PYRAMIDING_OPEN_ORDER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pyramiding_open_order")
endif()

# Lane W8A-SIGSTATE-1 R2 reads its TradingView tape and 15m bars from the
# fixture tree; source-bound.
if(TARGET test_pooc_same_pass_close_tape)
    target_compile_definitions(test_pooc_same_pass_close_tape PRIVATE
        PINEFORGE_POOC_SAME_PASS_CLOSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_same_pass_close")
endif()

# Lane W8A-SIGSTATE-1 R3 reads its TradingView tape and 15m bars from the
# fixture tree; source-bound.
if(TARGET test_limit_entry_affordability_tape)
    target_compile_definitions(test_limit_entry_affordability_tape PRIVATE
        PINEFORGE_LIMIT_ENTRY_AFFORDABILITY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/limit_entry_affordability")
endif()

# Lane R1-CONSOLIDATE reads its TradingView tapes from the fixture tree (and the
# bars of tests/fixtures/tvdef_drops); source-bound.
if(TARGET test_same_bar_reissue_tapes)
    target_compile_definitions(test_same_bar_reissue_tapes PRIVATE
        PINEFORGE_SAME_BAR_REISSUE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/same_bar_reissue")
endif()

# Lane R1-CONSOLIDATE's R-A tapes and their lane feeds; source-bound.
if(TARGET test_open_fill_order_tapes)
    target_compile_definitions(test_open_fill_order_tapes PRIVATE
        PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/open_fill_order")
endif()

# Lane RUN-HARNESS reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_run_harness_window_tapes)
    target_compile_definitions(test_run_harness_window_tapes PRIVATE
        PINEFORGE_RUN_HARNESS_WINDOW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/run_harness_window")
endif()

# INT28 reads its TradingView tapes from the fixture tree (and lane
# R1-CONSOLIDATE's w8a-dca-open); source-bound.
if(TARGET test_int28_rule_compositions)
    target_compile_definitions(test_int28_rule_compositions PRIVATE
        PINEFORGE_INT28_COMPOSITIONS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/int28_compositions"
        PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/open_fill_order")
endif()

# INT29 reads its TradingView tape from the fixture tree; source-bound.
if(TARGET test_int29_rule_compositions)
    target_compile_definitions(test_int29_rule_compositions PRIVATE
        PINEFORGE_INT29_COMPOSITIONS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/int29_compositions")
endif()

# Lane INT28-FIX (CA) reads its TradingView tapes from the fixture tree and
# the 15m bars of tests/fixtures/margin_v6; source-bound.
if(TARGET test_reversal_close_all_tapes)
    target_compile_definitions(test_reversal_close_all_tapes PRIVATE
        PINEFORGE_REVERSAL_CLOSE_ALL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reversal_close_all")
endif()

# Lane W5B-ENG-MARGIN-RESIDUAL reads its TradingView tapes from the fixture
# tree; source-bound.
if(TARGET test_margin_residual_tapes)
    target_compile_definitions(test_margin_residual_tapes PRIVATE
        PINEFORGE_MARGIN_RESIDUAL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_residual")
endif()
# The margin-call rules tapes are read from the fixture tree too; source-bound.
if(TARGET test_margin_call_rules_tapes)
    target_compile_definitions(test_margin_call_rules_tapes PRIVATE
        PINEFORGE_MARGIN_CALL_RULES_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_call_rules")
endif()
# The margin-call schedule rules tapes are read from the fixture tree too.
if(TARGET test_margin_schedule_rules_tapes)
    target_compile_definitions(test_margin_schedule_rules_tapes PRIVATE
        PINEFORGE_MARGIN_SCHEDULE_RULES_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_schedule_rules")
endif()
# The opening-call rules tapes are read from the fixture tree too.
if(TARGET test_margin_open_rules_tapes)
    target_compile_definitions(test_margin_open_rules_tapes PRIVATE
        PINEFORGE_MARGIN_OPEN_RULES_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_open_rules")
endif()
# The forward replay reads its sampled tapes from the fixture tree; source-bound.
if(TARGET test_margin_rules_forward_replay)
    target_compile_definitions(test_margin_rules_forward_replay PRIVATE
        PINEFORGE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
endif()
# The event-ledger margin rules tapes are read from the fixture tree too;
# source-bound.
if(TARGET test_margin_ledger_rules_tapes)
    target_compile_definitions(test_margin_ledger_rules_tapes PRIVATE
        PINEFORGE_MARGIN_LEDGER_RULES_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_ledger_rules")
endif()
# The admission rules tapes are read from the fixture tree too; source-bound.
if(TARGET test_admission_rules_tapes)
    target_compile_definitions(test_admission_rules_tapes PRIVATE
        PINEFORGE_ADMISSION_RULES_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/admission_rules")
endif()
if(TARGET test_kernel_trail_stop_replica)
    target_compile_definitions(test_kernel_trail_stop_replica PRIVATE
        PINEFORGE_CALLBACK_LIFECYCLE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callback_lifecycle")
endif()
if(TARGET test_point_order_open_call)
    target_compile_definitions(test_point_order_open_call PRIVATE
        PINEFORGE_ADMISSION_RULES_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/admission_rules")
endif()

# Lane INT28-FIX reads its TradingView tapes from the fixture tree;
# source-bound.
if(TARGET test_slipped_short_tapes)
    target_compile_definitions(test_slipped_short_tapes PRIVATE
        PINEFORGE_SLIPPED_SHORT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/slipped_short")
endif()

# Lane W13-ENG-MARGIN-OPP reads its TradingView tapes from the fixture tree;
# source-bound.
if(TARGET test_margin_opposite_tapes)
    target_compile_definitions(test_margin_opposite_tapes PRIVATE
        PINEFORGE_MARGIN_OPPOSITE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_opposite"
        PINEFORGE_SLIPPED_SHORT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/slipped_short")
endif()
if(TARGET test_explicit_market_admission_tapes)
    target_compile_definitions(test_explicit_market_admission_tapes PRIVATE
        PINEFORGE_EXPLICIT_ADMISSION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/explicit_market_admission")
endif()
if(TARGET test_margin_open_print_tapes)
    target_compile_definitions(test_margin_open_print_tapes PRIVATE
        PINEFORGE_MARGIN_OPEN_PRINT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/margin_open_print")
endif()
if(TARGET test_nonfinite_entry_qty_tape)
    target_compile_definitions(test_nonfinite_entry_qty_tape PRIVATE
        PINEFORGE_NONFINITE_QTY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nonfinite_entry_qty")
endif()

# Lane TAIL-B reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_add_sibling_margin_tapes)
    target_compile_definitions(test_add_sibling_margin_tapes PRIVATE
        PINEFORGE_ADD_SIBLING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/add_sibling_margin")
endif()

# INT30 reads its TradingView tapes from the fixture tree and the 15m bars of
# tests/fixtures/margin_v6; source-bound.
if(TARGET test_int30_rule_compositions)
    target_compile_definitions(test_int30_rule_compositions PRIVATE
        PINEFORGE_INT30_COMPOSITIONS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/int30_compositions")
endif()

# Lane INT28-FIX (RC) reads its TradingView tape from the fixture tree;
# source-bound.
if(TARGET test_coof_recalc_security_tapes)
    target_compile_definitions(test_coof_recalc_security_tapes PRIVATE
        PINEFORGE_COOF_RECALC_SECURITY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_recalc_security")
endif()

# Lane W10-DIAG-UNKNOWN (COQ) reads its TradingView tapes from the fixture tree;
# source-bound.
if(TARGET test_coqueued_open_margin_tapes)
    target_compile_definitions(test_coqueued_open_margin_tapes PRIVATE
        PINEFORGE_COQ_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coqueued_open_margin")
endif()

# Lane W10-DIAG-UNKNOWN (CAP) reads its TradingView tapes from the fixture tree;
# source-bound.
if(TARGET test_intraday_cap_tv_tapes)
    target_compile_definitions(test_intraday_cap_tv_tapes PRIVATE
        PINEFORGE_CAP_TV_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/intraday_cap_tv")
endif()

# Lane W10-DIAG-UNKNOWN (GAPSTOP) reads its TradingView tapes from the fixture
# tree; source-bound.
if(TARGET test_pooc_gap_stop_tapes)
    target_compile_definitions(test_pooc_gap_stop_tapes PRIVATE
        PINEFORGE_GAPSTOP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pooc_gap_stop")
endif()

# Lane W10-DIAG-UNKNOWN (SAMEOPEN-REV) reads its TradingView tapes from the
# fixture tree; source-bound.
if(TARGET test_same_open_reversal_tapes)
    target_compile_definitions(test_same_open_reversal_tapes PRIVATE
        PINEFORGE_SAMEOPEN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/same_open_reversal")
endif()

# Lane W8E-EXITS reads its TradingView tapes from the fixture tree; source-bound.
if(TARGET test_open_entry_fee_charged)
    target_compile_definitions(test_open_entry_fee_charged PRIVATE
        PINEFORGE_OPEN_ENTRY_FEE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/open_entry_fee_charged")
endif()

# K-DRAWSNAP replays TradingView's calc_on_order_fills drawing-rollback tape
# through the Pine host; source-bound.
if(TARGET test_coof_drawing_rollback_tape)
    target_compile_definitions(test_coof_drawing_rollback_tape PRIVATE
        PINEFORGE_K_DRAWSNAP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_drawing_rollback")
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

# Lane W11-ENG-TIME-COLOR: the rows that read TradingView tapes from the
# fixture tree.
foreach(_pf_w11_row IN ITEMS
        "test_session_predicates|PINEFORGE_SESSION_CLOCK_FIXTURE_DIR|session_clock"
        "test_color_tapes|PINEFORGE_COLOR_TV_FIXTURE_DIR|color_tv"
        "test_ta_voltrend_edge|PINEFORGE_SUPERTREND_WARMUP_FIXTURE_DIR|supertrend_warmup"
        "test_engine_trade_accessors|PINEFORGE_DAILY_BREAK_CLOSE_FIXTURE_DIR|daily_break_close")
    string(REPLACE "|" ";" _pf_w11_fields "${_pf_w11_row}")
    list(GET _pf_w11_fields 0 _pf_w11_target)
    list(GET _pf_w11_fields 1 _pf_w11_macro)
    list(GET _pf_w11_fields 2 _pf_w11_dir)
    if(TARGET ${_pf_w11_target})
        target_compile_definitions(${_pf_w11_target} PRIVATE
            ${_pf_w11_macro}="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_pf_w11_dir}")
    endif()
endforeach()

# Lane TAIL-E: the time() bars_back tapes.
if(TARGET test_time_bars_back_tapes)
    target_compile_definitions(test_time_bars_back_tapes PRIVATE
        PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/time_bars_back")
endif()
if(TARGET test_coof_immediate_close_final_tapes)
    target_compile_definitions(test_coof_immediate_close_final_tapes PRIVATE
        PINEFORGE_COOF_IMMEDIATE_CLOSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/coof_immediate_close")
endif()
# Lane XAU-CAL: TradingView's symbol calendar tapes, and lane TAIL-E's
# bars_back tapes the same test replays with the calendar.
if(TARGET test_symbol_calendar_tapes)
    target_compile_definitions(test_symbol_calendar_tapes PRIVATE
        PINEFORGE_SYMBOL_CALENDAR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/symbol_calendar"
        PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/time_bars_back")
endif()

# Lane W12-ENG-TIME: the rows that read its TradingView tapes.
foreach(_pf_w12_row IN ITEMS
        "test_session_predicates|PINEFORGE_SESSION_PERIOD_FIXTURE_DIR|session_period"
        "test_session_predicates|PINEFORGE_TIME_CLOSE_FUNCTION_FIXTURE_DIR|time_close_function"
        "test_session_predicates|PINEFORGE_DST_DAY_CLOSE_FIXTURE_DIR|dst_day_close"
        "test_engine_trade_accessors|PINEFORGE_DST_DAY_CLOSE_FIXTURE_DIR|dst_day_close"
        "test_engine_trade_accessors|PINEFORGE_SESSION_PERIOD_FIXTURE_DIR|session_period")
    string(REPLACE "|" ";" _pf_w12_fields "${_pf_w12_row}")
    list(GET _pf_w12_fields 0 _pf_w12_target)
    list(GET _pf_w12_fields 1 _pf_w12_macro)
    list(GET _pf_w12_fields 2 _pf_w12_dir)
    if(TARGET ${_pf_w12_target})
        target_compile_definitions(${_pf_w12_target} PRIVATE
            ${_pf_w12_macro}="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_pf_w12_dir}")
    endif()
endforeach()

# Lane TAIL-G: the row that reads the daily-break request tapes.
if(TARGET test_foreign_break_stamp_tapes)
    target_compile_definitions(test_foreign_break_stamp_tapes PRIVATE
        PINEFORGE_DAILY_BREAK_CLOSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/daily_break_close")
endif()

# Lane TAIL-H: the rows that read its TradingView tapes.
if(TARGET test_ta_indicators_extras)
    target_compile_definitions(test_ta_indicators_extras PRIVATE
        PINEFORGE_PERCENTILE_WINDOW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/percentile_window")
endif()
if(TARGET test_exit_reservation_tapes)
    target_compile_definitions(test_exit_reservation_tapes PRIVATE
        PINEFORGE_EXIT_RESERVATION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit_reservation")
endif()
if(TARGET test_percent_entry_reservation_tapes)
    target_compile_definitions(test_percent_entry_reservation_tapes PRIVATE
        PINEFORGE_PERCENT_ENTRY_RESERVATION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/percent_entry_reservation")
endif()

# Every row in PINEFORGE_PR_SLOW_TESTS (top of this file) must be a registered
# ctest row on every profile: real, or the private-fixture SKIP placeholder its
# own guarded block above registered.  The check and the `slow` label therefore
# run here, after those blocks: test_local_time_fields is registered by the block
# above, not by the TEST_SOURCES loop, and the guard would fire on it if it ran
# before that block.
foreach(_pf_slow_test IN LISTS PINEFORGE_PR_SLOW_TESTS)
    if(PINEFORGE_BUILD_SOURCE_LAYER AND NOT TEST ${_pf_slow_test})
        message(FATAL_ERROR "PR slow row is no longer registered: ${_pf_slow_test}")
    endif()
    if(TEST ${_pf_slow_test})
        set_property(TEST ${_pf_slow_test} APPEND PROPERTY LABELS slow)
    endif()
endforeach()

# Ctest-registered Python rows (Python tape drivers and scripts/test_*.py
# unittest files). The script prints the same SKIP literal on stdout and exits
# 77 (or raises unittest.SkipTest) when its private fixture is absent, so the
# row reports SKIP rather than pass.
foreach(_pf_python_row IN ITEMS
    test_callback_lifecycle_tapes
    test_coof_competing_tick_tapes
    test_cross_side_exit_tapes
    test_per_entry_exit_tapes
    test_same_side_exit_tapes
    test_run_strategy_emit_window
    test_run_strategy_magnifier
    test_run_strategy_mincontract
    test_symbol_calendar)
    if(TEST ${_pf_python_row})
        set_tests_properties(${_pf_python_row} PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endforeach()

# The gate suites (test_ci_verify slices/cover and the preflight slices/cover, above)
# drive the real ci_verify Driver over the floor, exclusion, overlay
# and live-profile paths, and build their tamper trees through
# tempfile.TemporaryDirectory, which honours TMPDIR: pin TMPDIR to a
# configure-time directory under the build dir, the shape the private-fixture
# controls rows use, instead of letting the rows write into the host's /tmp.
# The unsplit test_ci_verify ran 957.74 s (release A), 979.32 s (kernel A) and 781.00 s
# (release B) in the campaign's lab jobs; with no TIMEOUT property and no
# verifier stage bound it is never cut off for slowness (owner rule,
# 2026-10-12), and its 3600 s collision with the native-live stage deadline is
# gone with both. The preflight suite uses eight slices of sorted unittest
# IDs plus one collection-only cover, outside PINEFORGE_PR_SLOW_TESTS; the
# test_ci_verify slices inherit that timing-derived PR exclusion population and
# full runs (the lab job set among them, which is where its controls run) still
# execute every slice. Both covers only collect and stay outside the slow
# population. All are PUBLIC with no private-fixture label or SKIP_RETURN_CODE,
# and register in both fixture modes.
set_tests_properties(${PINEFORGE_CI_VERIFY_SLICES} test_ci_verify_slices_cover PROPERTIES
    ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp")
set_tests_properties(
    test_ci_preflight_slice_1
    test_ci_preflight_slice_2
    test_ci_preflight_slice_3
    test_ci_preflight_slice_4
    test_ci_preflight_slice_5
    test_ci_preflight_slice_6
    test_ci_preflight_slice_7
    test_ci_preflight_slice_8
    test_ci_preflight_slices_cover
    PROPERTIES
    ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp")

# CR-233: a public, receipt-independent Release smoke. Build these executables
# before CTest; a library-only build leaves statically linked tests stale.
# Full profiles still build all targets and run every registered row.
set(PINEFORGE_PR_FAST_TESTS
    test_ringbuffer
    test_timeframe
    test_path_at
    test_na_runtime
    test_math_sum
    test_str_utils
    test_ta
    test_matrix
    test_native_run_spec
    test_native_bare_host_contracts
    test_native_kernel_edge
)

# CR-150 P00: append after the pre-existing line-pinned registrations.
# Keep the unit-test inventory in sync; its main registration loop is above.
list(APPEND TEST_SOURCES test_cr150_contract_headers)
pineforge_add_unit_test(test_cr150_contract_headers)
target_sources(test_cr150_contract_headers PRIVATE test_cr150_contract_headers_c.c)
set_target_properties(test_cr150_contract_headers PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

# CR-150 L7B: point-source equity metrics.  This registration intentionally
# follows the P00 line-pinned block above (scope ruling SCOPE-RULING-1818.md).
list(APPEND TEST_SOURCES test_metrics_point_source)
pineforge_add_unit_test(test_metrics_point_source)

# CR-150 L15a: append after every existing line-pinned registration.
list(APPEND TEST_SOURCES test_native_bar_views_l15a)
pineforge_add_unit_test(test_native_bar_views_l15a)

add_test(NAME test_native_bar_views_guard_l15a
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_native_cpp_versions.py
            --self-test-bar-views)

# CR-150 L16: source-layer differential, appended after all line-pinned rows.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_magnifier_path_source_l16)
    pineforge_add_unit_test(test_magnifier_path_source_l16)
endif()

# CR-150 L8a: keep the existing CMake line pins stable.
list(APPEND TEST_SOURCES test_trade_ledger)
pineforge_add_unit_test(test_trade_ledger)

list(APPEND TEST_SOURCES test_trade_percent_prefix)
pineforge_add_unit_test(test_trade_percent_prefix)

# CR-150 L7A: isolated trade-fold proof, after all line-pinned registrations.
list(APPEND TEST_SOURCES test_trade_stats_accumulator)
pineforge_add_unit_test(test_trade_stats_accumulator)

# CR-150 L0B: append only; private_fixture_reads.evidence pins earlier lines.
add_test(NAME test_heap_census
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_heap_census.py)
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_heap_census_unique_ids)
    pineforge_add_unit_test(test_heap_census_unique_ids)
    target_link_libraries(test_heap_census_unique_ids PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(test_heap_census_unique_ids PROPERTIES ENABLE_EXPORTS ON)
endif()

# O(1) seam 3 journal-codec red clauses: eleven executables over one test file, one clause each.
# They link the source layer (pine_adapter.hpp, pine_command_scratch_production.hpp), so a
# kernel-only configure must not create them.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    set(_o1s3_row 0)
    foreach(_o1s3_case IN ITEMS
        s3_separate_journal_owners_and_reset_red
        s3_native_codec_all_values_cross_page_red
        s3_admission_codec_allocations_and_order_red
        s3_journal_islands_fixed_under_pins_red
        s3_native_window_dependency_frontier_red
        s3_admission_window_requires_closed_prefix_red
        s3_native_digest_survives_nonzero_base_red
        s3_admission_digest_refuses_zero_refold_red
        s34_readback_never_collects_population_red
        s34_backing_physical_charge_and_lifetime_red
        s3_admission_unknown_birth_cause_refused_red)
        math(EXPR _o1s3_row "${_o1s3_row} + 1")
        add_executable(${_o1s3_case} test_o1s3_journal_red.cpp)
        target_link_libraries(${_o1s3_case} PRIVATE pineforge)
        target_compile_features(${_o1s3_case} PRIVATE cxx_std_17)
        target_compile_definitions(${_o1s3_case} PRIVATE PINEFORGE_O1S3_CLAUSE=${_o1s3_row})
        target_compile_options(${_o1s3_case} PRIVATE -ffp-contract=off -UNDEBUG)
        add_test(NAME ${_o1s3_case} COMMAND ${_o1s3_case} ${_o1s3_case})
    endforeach()
    unset(_o1s3_case)
    unset(_o1s3_row)

    add_executable(test_o1s3_p14_fold_cost test_o1s3_p14_fold_cost.cpp)
    target_link_libraries(test_o1s3_p14_fold_cost PRIVATE pineforge)
    target_compile_features(test_o1s3_p14_fold_cost PRIVATE cxx_std_17)
    target_compile_options(test_o1s3_p14_fold_cost PRIVATE -ffp-contract=off -UNDEBUG)
    add_test(NAME test_o1s3_p14_fold_cost COMMAND test_o1s3_p14_fold_cost)
endif()

add_test(NAME test_cr150_cmake_d COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/check_cr150_cmake_d.py)

# CR-352: regression rows retain the fixture API available on the red base.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_scratch_watch_attribution)
    pineforge_add_unit_test(test_scratch_watch_attribution)
    target_link_libraries(test_scratch_watch_attribution PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    foreach(_pf_scratch_watch_case IN ITEMS
            cotenant_default cotenant_jailed cotenant_positive
            own_tmp own_transient_tmp own_other_tmp own_shm private_non_mkstemp header_scratch)
        add_test(NAME test_scratch_watch_attribution_${_pf_scratch_watch_case}
            COMMAND test_scratch_watch_attribution ${_pf_scratch_watch_case})
    endforeach()
endif()

add_test(NAME test_run_json_sink_fd_skip_accounting COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink_fd_skip_accounting.py --artifacts ${CMAKE_CURRENT_BINARY_DIR}/pytest-no-skips/test_run_json_sink_fd_skip_accounting)

# Property reads and the diagnostic belong before the lane loop;
# the fence grammar permits target definitions, not get_target_property/message.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    get_target_property(_pf_source_key_type pineforge TYPE)
    get_target_property(_pf_source_key_options pineforge COMPILE_OPTIONS)
    get_target_property(_pf_source_key_definitions pineforge COMPILE_DEFINITIONS)
    if(NOT _pf_source_key_type STREQUAL "STATIC_LIBRARY")
        message(STATUS "test_live_source_key_alias and test_live_source_key_spellings_strict not registered: pineforge TYPE is ${_pf_source_key_type}; the adapter overrides require STATIC_LIBRARY")
    endif()
endif()

# CR-150: one file per lane in tests/cr150.d/<lane>.cmake (see tests/cr150.d/README.md).
file(GLOB CR150_LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/*.cmake")
list(SORT CR150_LANE_CMAKE)
foreach(cr150_lane_file IN LISTS CR150_LANE_CMAKE)
    include("${cr150_lane_file}")
endforeach()

# --- CR-150 s4e-fix-tmp begin ---
# CR-352: only these opted-in existing targets need the interposer's linkage.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_default_scratch_tmpfs_tmpdir PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    target_link_libraries(test_jailed_scratch_tmpfs_tmpdir PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()
# --- CR-150 s4e-fix-tmp end ---

# --- CR-150 core-cr150-l6b1 begin ---
# CR-150 L6B1: appended after every pre-existing line pin. A real source
# strategy module, with the same whole-archive link as release modules.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    add_library(report_sink_l6b1_strategy MODULE test_report_sink_l6b1.cpp)
    target_compile_definitions(report_sink_l6b1_strategy PRIVATE PINEFORGE_L6B1_MODULE=1)
    target_link_libraries(report_sink_l6b1_strategy PRIVATE pineforge)
    if(APPLE)
        target_link_options(report_sink_l6b1_strategy PRIVATE
            "-Wl,-force_load,$<TARGET_FILE:pineforge>")
    else()
        target_link_options(report_sink_l6b1_strategy PRIVATE
            "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    endif()
    target_compile_options(report_sink_l6b1_strategy PRIVATE -ffp-contract=off -UNDEBUG)
    set_target_properties(report_sink_l6b1_strategy PROPERTIES
        PREFIX "" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/report_sink_l6b1")
    list(APPEND TEST_SOURCES test_report_sink_l6b1)
    pineforge_add_unit_test(test_report_sink_l6b1)
    target_link_libraries(test_report_sink_l6b1 PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_report_sink_l6b1 PRIVATE
        PINEFORGE_L6B1_MODULE_PATH="$<TARGET_FILE:report_sink_l6b1_strategy>"
        PINEFORGE_L6B1_PYTHON="${Python3_EXECUTABLE}"
        PINEFORGE_L6B1_DOCKER="${PROJECT_SOURCE_DIR}/docker")
    add_dependencies(test_report_sink_l6b1 report_sink_l6b1_strategy)
endif()
# --- CR-150 core-cr150-l6b1 end ---

# --- CR-150 core-cr150-l17a begin ---
# CR-150 L17A: a kernel-only native host and a real version-1 path sink.
# The UNIX-only plain registration is in cr150.d/core-cr150-l17a.cmake.
# No additional target or test settings are required here.
# --- CR-150 core-cr150-l17a end ---

# --- CR-150 core-cr150-l9a begin ---
# CR-150 L9A: full JSON through the temporary sink; after every line pin.
# Preflight also discovers this file through docker/run_json_*_test.py.
# H2-6: optional native and corpus checks must not skip inside a green row.
# The native row is in p2cx-cmake.cmake. The corpus byte proof needs a
# supervisor-supplied PINEFORGE_L9A_CORPUS_CASES manifest, as L9B does, so it
# stays outside this CI registration.
add_test(NAME test_run_json_sink_l9a
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/run_pytest_no_skips.py
        --artifacts ${CMAKE_CURRENT_BINARY_DIR}/pytest-no-skips/test_run_json_sink_l9a
        -- -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink.py -k "not native and not corpus")
# --- CR-150 core-cr150-l9a end ---

# --- CR-150 core-cr150-l19b begin ---
# CR-150 L19B: match the library Debug-mode precedent for
# test_native_runtime_ambient_lifo's PINEFORGE_LIBRARY_DEBUG_CHECKS definition (H2-12).
target_compile_definitions(test_group_receipt_retirement PRIVATE PINEFORGE_LIBRARY_DEBUG_CHECKS=$<CONFIG:Debug>)
# --- CR-150 core-cr150-l19b end ---

# --- CR-150 core-cr150-l6b3 begin ---
if(UNIX)
    target_compile_definitions(test_report_sink_l6b3 PRIVATE
        PINEFORGE_L6B3_PYTHON="${Python3_EXECUTABLE}"
        PINEFORGE_L6B3_DOCKER="${PROJECT_SOURCE_DIR}/docker")
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        target_compile_definitions(test_report_sink_l6b3 PRIVATE PINEFORGE_L6B3_SOURCE=1)
    endif()
endif()
# --- CR-150 core-cr150-l6b3 end ---

# --- CR-150 core-cr150-l6b2 begin ---
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    add_library(report_sink_equity_strategy MODULE test_report_sink_equity.cpp)
    target_compile_definitions(report_sink_equity_strategy PRIVATE PINEFORGE_L6B2_MODULE=1)
    target_link_libraries(report_sink_equity_strategy PRIVATE pineforge)
    if(APPLE)
        target_link_options(report_sink_equity_strategy PRIVATE
            "-Wl,-force_load,$<TARGET_FILE:pineforge>")
    else()
        target_link_options(report_sink_equity_strategy PRIVATE
            "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    endif()
    target_compile_options(report_sink_equity_strategy PRIVATE -ffp-contract=off -UNDEBUG)
    set_target_properties(report_sink_equity_strategy PROPERTIES
        PREFIX "" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/report_sink_equity")
    target_link_libraries(test_report_sink_equity PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_report_sink_equity PRIVATE
        PINEFORGE_L6B2_MODULE_PATH="$<TARGET_FILE:report_sink_equity_strategy>")
    add_dependencies(test_report_sink_equity report_sink_equity_strategy)
endif()
# --- CR-150 core-cr150-l6b2 end ---

# --- CR-150 core-cr150-l17b begin ---
if(TARGET test_selected_metrics_l17b)
    target_compile_definitions(test_selected_metrics_l17b PRIVATE PINEFORGE_LIBRARY_DEBUG_CHECKS=$<CONFIG:Debug>)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        target_compile_definitions(test_selected_metrics_l17b PRIVATE PINEFORGE_L17B_SOURCE=1)
    endif()
endif()
# --- CR-150 core-cr150-l17b end ---
# --- CR-150 core-cr150-l19f begin ---
# CR-150 L19F: plain registrations are in cr150.d/core-cr150-l19f.cmake.
# --- CR-150 core-cr150-l19f end ---

# --- CR-150 core-cr150-l2 begin ---
if(TARGET outputs_fixture_strategy)
    set_tests_properties(test_run_json_bin PROPERTIES
        ENVIRONMENT "PINEFORGE_BIN_STRATEGY_SO=$<TARGET_FILE:outputs_fixture_strategy>")
endif()
# --- CR-150 core-cr150-l2 end ---

# --- CR-150 core-cr150-l9b begin ---
# CR-150 L9B: native named-sink summary rows, after every line pin. The Python
# row is registered in cr150.d/core-cr150-l9b.cmake. These rows read the real
# L6B1 and outputs-fixture modules from their ENVIRONMENT, which a lane file may
# not set, so they are registered here once both modules are declared. The
# row exists only where both modules do: it never runs without them.
# The opt-in corpus row (PINEFORGE_L9B_CORPUS_CASES) needs real lab probes that
# only the supervisor can supply, so it is left out of this registration.
if(TARGET report_sink_l6b1_strategy AND TARGET outputs_fixture_strategy
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_sink_summary_l9b_native
        COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink_summary.py
            -k "native and not corpus")
    set_property(TEST test_run_json_sink_summary_l9b_native APPEND PROPERTY ENVIRONMENT
        "PINEFORGE_L9B_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>"
        "PINEFORGE_L9B_OUTPUTS_SO=$<TARGET_FILE:outputs_fixture_strategy>")
endif()
# --- CR-150 core-cr150-l9b end ---

# --- CR-150 core-cr150-l9s begin ---
# CR-150 L9S: native selected-sink rows, after both module declarations.
# The Python control row stays in cr150.d/core-cr150-l9s.cmake.
# Corpus rows need a supervisor-supplied manifest; selecting them here would
# otherwise always SKIP inside a green row.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS
   AND NOT PINEFORGE_ENABLE_COVERAGE
   AND TARGET report_sink_l6b1_strategy AND TARGET pineforge_window_plan)
    add_test(NAME test_run_json_selected_sink_native_l9s
        COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_sink.py
            -k "native and not corpus")
    set_property(TEST test_run_json_selected_sink_native_l9s APPEND PROPERTY ENVIRONMENT
        "PINEFORGE_L9S_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>"
        "PINEFORGE_L9S_PLANNER_SO=$<TARGET_FILE:pineforge_window_plan>")
    add_test(NAME test_run_json_selected_sink_memory_l9s
        COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_sink_memory.py)
    set_property(TEST test_run_json_selected_sink_memory_l9s APPEND PROPERTY ENVIRONMENT
        "PINEFORGE_L9S_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>"
        "PINEFORGE_L9S_PLANNER_SO=$<TARGET_FILE:pineforge_window_plan>")
    set_tests_properties(test_run_json_selected_sink_memory_l9s PROPERTIES LABELS "slow;mem")
endif()
# --- CR-150 core-cr150-l9s end ---

# --- CR-150 core-cr150-l22a begin ---
if(UNIX)
    target_link_libraries(test_report_sink_fd PRIVATE Threads::Threads)
    target_compile_definitions(test_report_sink_fd PRIVATE
        PINEFORGE_L22A_PYTHON="${Python3_EXECUTABLE}"
        PINEFORGE_L22A_READER="${CMAKE_CURRENT_SOURCE_DIR}/report_sink_fd_reader.py")
endif()
# --- CR-150 core-cr150-l22a end ---

# --- CR-150 core-cr150-l22q begin ---
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    target_link_libraries(test_report_sink_summary PRIVATE Threads::Threads)
endif()
# --- CR-150 core-cr150-l22q end ---

# --- CR-150 core-cr150-l22s begin ---
if(UNIX)
    target_link_libraries(test_scratch_arena_threads PRIVATE Threads::Threads)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(test_scratch_arena PRIVATE "-Wl,--wrap=pread" "-Wl,--wrap=fstat" "-Wl,--wrap=mmap")
endif()
# --- CR-150 core-cr150-l22s end ---

# --- CR-150 core-cr150-l22p begin ---
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_report_scratch_arena)
    add_executable(test_report_scratch_arena test_report_scratch_arena.cpp)
    target_link_libraries(test_report_scratch_arena PRIVATE pineforge)
    target_compile_options(test_report_scratch_arena PRIVATE -ffp-contract=off -UNDEBUG)
    if(PINEFORGE_ENABLE_COVERAGE)
        target_compile_options(test_report_scratch_arena PRIVATE ${_pf_cov_test_flags})
    endif()
    target_link_options(test_report_scratch_arena PRIVATE
        "-Wl,--wrap=mkstemp" "-Wl,--wrap=pwrite" "-Wl,--wrap=write"
        "-Wl,--wrap=ftruncate" "-Wl,--wrap=mmap" "-Wl,--wrap=munmap")
    add_test(NAME test_report_scratch_arena_curve_bytes
        COMMAND test_report_scratch_arena curve bytes)
    add_test(NAME test_report_scratch_arena_curve_fsize
        COMMAND test_report_scratch_arena curve fsize)
    add_test(NAME test_report_scratch_arena_curve_exhaustion
        COMMAND test_report_scratch_arena curve exhaustion)
    add_test(NAME test_report_scratch_arena_curve_address_space
        COMMAND test_report_scratch_arena curve address_space)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        target_compile_definitions(test_report_scratch_arena PRIVATE PINEFORGE_L22P_SOURCE=1)
        add_test(NAME test_report_scratch_arena_position_tail_bytes
            COMMAND test_report_scratch_arena position_tail bytes)
        add_test(NAME test_report_scratch_arena_position_tail_fsize
            COMMAND test_report_scratch_arena position_tail fsize)
        add_test(NAME test_report_scratch_arena_position_tail_exhaustion
            COMMAND test_report_scratch_arena position_tail exhaustion)
        add_test(NAME test_report_scratch_arena_position_tail_address_space
            COMMAND test_report_scratch_arena position_tail address_space)
    endif()
    add_test(NAME test_report_scratch_arena_startup COMMAND test_report_scratch_arena startup)
endif()
# --- CR-150 core-cr150-l22p end ---

# --- CR-150 core-cr150-q7 begin ---
if(UNIX)
    target_link_libraries(test_run_sigterm_abort PRIVATE Threads::Threads)
endif()
if(UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_library(run_sigterm_abort_strategy MODULE run_sigterm_abort_strategy.cpp)
    target_link_libraries(run_sigterm_abort_strategy PRIVATE pineforge)
    if(APPLE)
        target_link_options(run_sigterm_abort_strategy PRIVATE
            "-Wl,-force_load,$<TARGET_FILE:pineforge>")
    else()
        target_link_options(run_sigterm_abort_strategy PRIVATE
            "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    endif()
    target_compile_options(run_sigterm_abort_strategy PRIVATE -ffp-contract=off -UNDEBUG)
    set_target_properties(run_sigterm_abort_strategy PROPERTIES
        PREFIX "" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/run_sigterm_abort")
    add_test(NAME test_run_sigterm_abort_python
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_run_sigterm_abort.py
            --module $<TARGET_FILE:run_sigterm_abort_strategy>)
endif()
# --- CR-150 core-cr150-q7 end ---

# --- CR-150 core-cr150-o1w-contract begin ---
# Real legacy provider, with an explicit old export surface. The Linux lab
# exercises negotiation even after new runtime report exports are introduced.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT APPLE
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_library(o1_report_legacy_provider MODULE test_o1_report_contract.cpp)
    target_compile_definitions(o1_report_legacy_provider PRIVATE PINEFORGE_O1_LEGACY_PROVIDER=1)
    target_link_libraries(o1_report_legacy_provider PRIVATE pineforge)
    target_link_options(o1_report_legacy_provider PRIVATE
        "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive"
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/o1_report_contract/legacy.exports")
    target_compile_options(o1_report_legacy_provider PRIVATE -ffp-contract=off)
    add_test(NAME test_o1_report_mode_unsupported
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_o1_report_mode_unsupported.py
            --module $<TARGET_FILE:o1_report_legacy_provider>
            --runner ${PROJECT_SOURCE_DIR}/docker/run_json.py
            --fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/o1_report_contract/summary_only.json)
endif()
# --- CR-150 core-cr150-o1w-contract end ---

# --- CR-150 core-cr150-o1c begin ---
# LAB-o1bk: production command scratch, without a fixture factory definition.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_o1bk_production_backing)
    pineforge_add_unit_test(test_o1bk_production_backing)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_o1bk_production_backing PRIVATE PINEFORGE_O1BK_WRAP_PWRITE=1)
        target_link_options(test_o1bk_production_backing PRIVATE "-Wl,--wrap=pwrite")
    endif()
endif()
# --- CR-150 core-cr150-o1c end ---

# --- CR-150 core-cr150-o1-revscratch begin ---
# A corpus-enabled source build must execute the real reversing probe. Missing
# fixtures are configure/test failures; this row has no skip or fake module.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND PINEFORGE_BUILD_CORPUS_STRATEGIES
   AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_library(reversal_scratch_strategy MODULE)
    target_sources(reversal_scratch_strategy PRIVATE
        ${PROJECT_SOURCE_DIR}/corpus/validation/bracket-atr-trailing-stop-state-01/generated.cpp)
    target_link_libraries(reversal_scratch_strategy PRIVATE pineforge)
    if(APPLE)
        target_link_options(reversal_scratch_strategy PRIVATE
            "-Wl,-force_load,$<TARGET_FILE:pineforge>")
    else()
        target_link_options(reversal_scratch_strategy PRIVATE
            "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    endif()
    target_compile_options(reversal_scratch_strategy PRIVATE -ffp-contract=off)
    set_target_properties(reversal_scratch_strategy PROPERTIES
        PREFIX "" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reversal_scratch")
    add_test(NAME test_run_json_reversal_scratch
        COMMAND ${Python3_EXECUTABLE} -B
            ${PROJECT_SOURCE_DIR}/tests/test_run_json_reversal_scratch.py
            --so $<TARGET_FILE:reversal_scratch_strategy>
            --artifacts ${CMAKE_CURRENT_BINARY_DIR}/reversal_scratch)
endif()
# --- CR-150 core-cr150-o1-revscratch end ---

# --- CR-150 core-cr150-arena begin ---
# Set B stays in this fence so the set A lane file can be committed by name.
if(UNIX)
    list(APPEND TEST_SOURCES test_command_scratch_arena_bounded)
    pineforge_add_unit_test(test_command_scratch_arena_bounded)
endif()
# --- CR-150 core-cr150-arena end ---

# --- CR-150 core-cr150-privmoves begin ---
# All rows share test_privmoves.cpp. The lane-file checker requires a source
# named after each compiled add_test row, so aliases stay with the target.
# TEST_SOURCES inventory lives in cr150.d/core-cr150-privmoves.cmake.
if(UNIX)
    add_executable(test_privmoves test_privmoves.cpp)
    target_link_libraries(test_privmoves PRIVATE pineforge)
    target_compile_options(test_privmoves PRIVATE -ffp-contract=off -UNDEBUG)
    if(PINEFORGE_ENABLE_COVERAGE)
        target_compile_options(test_privmoves PRIVATE ${_pf_cov_test_flags})
    endif()
    add_test(NAME test_privmoves_compare_tables COMMAND test_privmoves compare tables)
    add_test(NAME test_privmoves_compare_fold COMMAND test_privmoves compare fold)
    add_test(NAME test_privmoves_compare_roots COMMAND test_privmoves compare roots)
    add_test(NAME test_privmoves_compare_openings COMMAND test_privmoves compare openings)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(test_privmoves PRIVATE
            "-Wl,--wrap=mkstemp" "-Wl,--wrap=open" "-Wl,--wrap=openat" "-Wl,--wrap=pread")
        add_test(NAME test_privmoves_directory COMMAND test_privmoves directory)
        add_test(NAME test_privmoves_jail_tables COMMAND test_privmoves jail tables)
        add_test(NAME test_privmoves_jail_fold COMMAND test_privmoves jail fold)
        add_test(NAME test_privmoves_jail_roots COMMAND test_privmoves jail roots)
        add_test(NAME test_privmoves_jail_openings COMMAND test_privmoves jail openings)
    endif()
endif()
# --- CR-150 core-cr150-privmoves end ---

# --- CR-150 core-cr150-cr257 begin ---
# Lane files permit registrations only; the linker flags and extra translation
# units belong in this lane-owned fence, per check_cr150_cmake_d.py.
if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(test_cr257_source_exit_scratch_io PRIVATE
        test_cr257_source_exit_scratch_io_100k.cpp
        test_cr257_exit_scope_io.cpp
        test_cr257_scratch_file_io.cpp)
    target_link_options(test_cr257_source_exit_scratch_io PRIVATE
        "-Wl,--wrap=pread" "-Wl,--wrap=pwrite"
        "-Wl,--wrap=read" "-Wl,--wrap=readv"
        "-Wl,--wrap=preadv" "-Wl,--wrap=preadv2" "-Wl,--wrap=mmap"
        "-Wl,--wrap=posix_fadvise" "-Wl,--wrap=fdatasync")
    # preadv2 requires glibc 2.26+; an unavailable symbol must fail linking.
endif()
# --- CR-150 core-cr150-cr257 end ---

# --- CR-150 core-cr150-cr289 begin ---
# CR-289: ctypes needs a test-only bridge to the hidden C++ writer; the module
# path reaches the pytest row through $<TARGET_FILE:...> in the lane file.
if(UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_library(report_nonfinite_equity_fd MODULE test_report_nonfinite_equity_fd.cpp)
    target_link_libraries(report_nonfinite_equity_fd PRIVATE pineforge)
    target_compile_options(report_nonfinite_equity_fd PRIVATE -ffp-contract=off -UNDEBUG)
endif()
# --- CR-150 core-cr150-cr289 end ---

# --- CR-150 core-cr150-cr224 begin ---
# CR-281 ports CR-224 to Linux's sealed-memfd jailed arena. The module's only
# source is a file of the corpus submodule, so the module and its row exist
# exactly where that file does: the lab profiles check the submodule out
# (ci_verify ensure_corpus_submodule) although they turn corpus strategy
# targets OFF, so the option cannot be the guard; the pull-request build has no
# submodule and configures without it. The same guard is in
# cr150.d/core-cr150-cr224.cmake.
if(PINEFORGE_BUILD_SOURCE_LAYER AND EXISTS "${PROJECT_SOURCE_DIR}/corpus/validation/bracket-same-id-exit-replace-01/generated.cpp" AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_library(cr224_nofile_strategy MODULE ../corpus/validation/bracket-same-id-exit-replace-01/generated.cpp)
    target_link_libraries(cr224_nofile_strategy PRIVATE pineforge)
    target_link_options(cr224_nofile_strategy PRIVATE "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    target_compile_options(cr224_nofile_strategy PRIVATE -ffp-contract=off -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare)
    set_target_properties(cr224_nofile_strategy PROPERTIES PREFIX "" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cr224_nofile")
endif()
# --- CR-150 core-cr150-cr224 end ---

# --- CR-150 live-source-key begin ---
# The wrappers supply adapter symbols before extraction from the static archive.
# Plain normal-library rows and the custom inventory are in the lane file.
if(PINEFORGE_BUILD_SOURCE_LAYER AND _pf_source_key_type STREQUAL "STATIC_LIBRARY")
    add_executable(test_live_source_key_alias
        test_live_source_key_alias.cpp test_live_source_key_alias_adapter.cpp)
    target_link_libraries(test_live_source_key_alias PRIVATE pineforge)
    target_include_directories(test_live_source_key_alias PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(test_live_source_key_alias PRIVATE -UNDEBUG)
    if(_pf_source_key_options)
        target_compile_options(test_live_source_key_alias PRIVATE ${_pf_source_key_options})
    endif()
    if(_pf_source_key_definitions)
        target_compile_definitions(test_live_source_key_alias PRIVATE ${_pf_source_key_definitions})
    endif()
    target_compile_definitions(test_live_source_key_alias PRIVATE PINEFORGE_TEST_NARROW_SOURCE_KEY)
    add_test(NAME test_live_source_key_alias COMMAND test_live_source_key_alias)

    add_executable(test_live_source_key_spellings_strict
        test_live_source_key_spellings_strict.cpp test_live_source_key_spellings_strict_adapter.cpp)
    target_link_libraries(test_live_source_key_spellings_strict PRIVATE pineforge)
    target_include_directories(test_live_source_key_spellings_strict PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(test_live_source_key_spellings_strict PRIVATE -UNDEBUG -ffp-contract=off)
    if(_pf_source_key_options)
        target_compile_options(test_live_source_key_spellings_strict PRIVATE ${_pf_source_key_options})
    endif()
    if(_pf_source_key_definitions)
        target_compile_definitions(test_live_source_key_spellings_strict PRIVATE ${_pf_source_key_definitions})
    endif()
    add_test(NAME test_live_source_key_spellings_strict COMMAND test_live_source_key_spellings_strict)
endif()
# --- CR-150 live-source-key end ---

foreach(_pf_fast_test IN LISTS PINEFORGE_PR_FAST_TESTS)
    if(PINEFORGE_BUILD_SOURCE_LAYER AND (NOT TEST ${_pf_fast_test} OR NOT TARGET ${_pf_fast_test}))
        message(FATAL_ERROR "PR fast row or executable is missing: ${_pf_fast_test}")
    endif()
    if(TEST ${_pf_fast_test})
        get_test_property(${_pf_fast_test} DISABLED _pf_fast_disabled)
        get_test_property(${_pf_fast_test} SKIP_RETURN_CODE _pf_fast_skip_code)
        get_test_property(${_pf_fast_test} SKIP_REGULAR_EXPRESSION _pf_fast_skip_regex)
        get_test_property(${_pf_fast_test} LABELS _pf_fast_labels)
        if(_pf_fast_disabled OR NOT _pf_fast_skip_code STREQUAL "NOTFOUND"
                OR NOT _pf_fast_skip_regex STREQUAL "NOTFOUND"
                OR "slow" IN_LIST _pf_fast_labels OR "private_fixtures" IN_LIST _pf_fast_labels)
            message(FATAL_ERROR "PR fast row must execute without skips: ${_pf_fast_test}")
        endif()
        set_property(TEST ${_pf_fast_test} APPEND PROPERTY LABELS fast)
    endif()
endforeach()
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_custom_target(pineforge_pr_smoke DEPENDS ${PINEFORGE_PR_FAST_TESTS})
endif()
