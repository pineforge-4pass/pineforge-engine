set(TEST_SOURCES
    test_unbounded_margin_admission
    test_resolved_execution
    test_native_scoped_close
    test_native_reversal_contract
    test_order_action
    test_native_order_core
    test_native_order_terminal
    test_native_market_vertical
    test_native_market_vertical_contract
    test_native_calendar_driver_contract
    test_native_host_repairs
    test_order_action_integration
    test_placement_facts
    test_order_cancellation
    test_cancellation_mirror_coverage
    test_market_admission_causality
    test_opposite_intent_facts
    test_pending_order_mirror_no_alloc
    test_market_admission_decisions
    test_market_admission_state
    test_market_admission_matrix
    test_reservation_expansion
    test_frozen_market_instruction
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
    test_pending_quantity_intent
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
    test_pine_cap_boundary
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
    test_pooc_global_full_exit
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
    test_pending_placement_receipts
    test_order_birth_provenance
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
    test_dropped_reversal_mc_first
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
    test_prearmed_exit_path_cursor
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
    test_live_state_hash
    test_pyramid_entry_state_hash
    test_live_state_hash_recording
    test_live_pending_order_mirror
    test_live_order_derived
    test_live_trade_accessors
    test_live_flags_off_identity
    test_live_flags_lane_positive
)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
            ${_pf_script_cpp_abi_flags}
)

add_test(
    NAME test_native_cpp_versions
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_native_cpp_versions.py
)
add_test(
    NAME test_native_cpp_abi
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_native_cpp_abi.py
            --compiler ${CMAKE_CXX_COMPILER}
            --library $<TARGET_FILE:pineforge>
            --include ${PROJECT_SOURCE_DIR}/include
            --generated-include ${PROJECT_BINARY_DIR}/include
            ${_pf_script_cpp_abi_flags}
)
set_tests_properties(test_native_cpp_abi PROPERTIES TIMEOUT 240)

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

# The committed PendingOrder POD mirror (include/pineforge/pending_order_mirror.hpp,
# src/pending_order_mirror.cpp) must be exactly what engine.hpp generates:
# a PendingOrder member nobody mirrored (or waived) fails here and in CI.
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

add_test(
    NAME test_reservation_expansion_mirror_mutations
    COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test_reservation_expansion_mirror.py
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

# Run the same literal lifetime/OCA/callback and financial assertions with
# explicit Pine attachment as well as the unchanged bare-native test above.
add_test(NAME test_pending_order_identity_pine
         COMMAND test_pending_order_identity --pine)

add_test(NAME test_exit_leg_lifecycle_coverage COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/test_exit_leg_lifecycle_coverage.py)
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
