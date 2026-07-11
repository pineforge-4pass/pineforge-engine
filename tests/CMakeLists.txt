set(TEST_SOURCES
    test_ringbuffer
    test_timeframe
    test_magnifier
    test_magnifier_real_bars
    test_magnifier_distributions
    test_path_at
    test_na_runtime
    test_recompute
    test_str_utils
    test_ta
    test_ta_indicators_extras
    test_kc
    test_ta_rma_warmup
    test_ta_na_source_rules
    test_matrix
    test_session_time
    test_time_tradingday
    test_chart_timezone
    test_intraday_rollover_chart_tz
    test_intraday_cap_auto_close
    test_dmi_parity
    test_integration
    test_request_security
    test_security_range_start_na_warmup
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
    test_same_id_stop_replace
    test_exit_id_scoped_erase
    test_max_contracts_held
    test_generic_matrix_primitives
    test_generic_matrix_udt
    test_generic_matrix_bool_proxy
    test_generic_matrix_series
    test_generic_matrix_bool_dispatch
    test_session_predicates
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
    test_margin_admission_gate
    test_frozen_flat_gap_reject
    test_explicit_qty_fill_admission
    test_limit_fill_slippage
    test_strategy_commands_extra
    test_multi_tier_exit_precedence
    test_same_tick_multi_entry_race
    test_full_close_while_pyramiding
    test_deferred_flip_carry_close_only
    test_close_all_coqueued_entry
    test_declined_reversal_close_leg
    test_lower_tf_parse_extra
    test_ta_ma_warmup_extra
    test_ta_osc_edge
    test_ta_extremes_edge
    test_ta_voltrend_edge
    test_timezone_concurrency
    test_pointvalue
    test_metrics
    test_drawing
    test_margin_call
    test_streaming
    test_calc_on_order_fills
)

find_package(Threads REQUIRED)

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
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(test_c_abi PRIVATE ${_pf_cov_test_flags})
endif()
add_test(NAME test_c_abi COMMAND test_c_abi)
