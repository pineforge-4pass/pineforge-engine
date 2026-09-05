set(TEST_SOURCES
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
    test_ta_indicators_extras
    test_kc
    test_ta_rma_warmup
    test_ta_sma_exact_order
    test_ta_sma_window_residual
    test_ta_sma_stochrsi_ties
    test_ta_na_source_rules
    test_matrix
    test_matrix_identity
    test_matrix_na
    test_matrix_snapshot_compat
    test_session_time
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
    test_zero_lot_entry_decline
    test_pooc_coof_reversal_gross_admission
    test_limit_fill_slippage
    test_tv_fill_rounding
    test_strategy_commands_extra
    test_multi_tier_exit_precedence
    test_same_tick_multi_entry_race
    test_full_close_while_pyramiding
    test_integer_lot_percent_exit_min_step
    test_deferred_flip_carry_close_only
    test_close_all_coqueued_entry
    test_same_bar_add_exit_coverage
    test_declined_reversal_close_leg
    test_bracket_lifecycle_declined_reversal
    test_exit_bracket_pending_entry_leg
    test_dual_entry_placement_sizing
    test_default_flat_market_gross_admission
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
    test_margin_call_intrabar_chronology
    test_margin_call_trail_exit_chronology
    test_margin_call_1x_long_entry_fill
    test_margin_call_gap_open
    test_entry_bar_margin_path
    test_m_admission_36
    test_aapl15_margin_brackets
    test_percent_equity_open_entry_fee
    test_streaming
    test_calc_on_order_fills
    test_coof_cascade_eligibility
    test_cascade_exit_gapjump
    test_pooc_position_visibility
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
    test_trail_activation_tick_bar
    test_session_predicates_daily_chart
)

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
