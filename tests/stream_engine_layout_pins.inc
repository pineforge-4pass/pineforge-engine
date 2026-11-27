// Measured pins of the engine layout that PF_STREAM_ENGINE_LAYOUT_VERSION
// stands for: the size of BacktestEngine and of NativeExecutionConsumer, and
// the offset and size of each member the live runner's reader touches
// (tests/test_stream_engine_layout.cpp names them and compares them).
//
// Row forms, one per line:
//   PF_STREAM_LAYOUT_STAMP(<stamp>u)
//   PF_STREAM_LAYOUT_SIZE(<class>, <bytes>)
//   PF_STREAM_LAYOUT_MEMBER(<class>, <member path>, <offset>, <bytes>)
//
// Measured on Linux x86_64 with GNU libstdc++. No number here is edited by
// hand: when the layout moves, raise the stamp in include/pineforge/pineforge.h
// and replace every row below with the table the test prints for the new
// stamp. The preflight stage abi-pin-guard refuses a change of these rows that
// does not raise the stamp.
PF_STREAM_LAYOUT_STAMP(6u)
PF_STREAM_LAYOUT_SIZE(BacktestEngine, 6408)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, active_equity_report_mode_, 1296, 4)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, bar_magnifier_enabled_, 3032, 1)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, bars_in_market_, 1216, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, broker_state_hashes_, 1240, 24)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, chart_timezone_, 624, 32)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, current_bar_.close, 792, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, diag_input_bars_processed_, 5320, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, diag_magnifier_sample_ticks_processed_, 5344, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, diag_magnifier_sub_bars_processed_, 5336, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, diag_needs_aggregation_, 5356, 1)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, diag_script_bars_processed_, 5328, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, diag_script_tf_ratio_, 5352, 4)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, equity_curve_, 1192, 24)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, execution_consumer_slot_.ptr, 5400, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, first_bar_open_, 1224, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, initial_capital_, 152, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, input_tf_, 3056, 32)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, last_error_, 5360, 32)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, last_run_status_, 5204, 4)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, optional_report_mode_, 6257, 1)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, range_end_trades_, 856, 184)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, report_mode_, 6200, 4)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, report_sink_state_.enabled, 5744, 1)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, report_sink_state_.receipt.complete, 6048, 4)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, report_sink_state_.selected_equity_stream, 6160, 8)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, script_tf_seconds_, 3120, 4)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, security_eval_states_, 3616, 24)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, stream_phase_, 3448, 4)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, trace_buffer_, 5216, 24)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, trace_names_, 5240, 24)
PF_STREAM_LAYOUT_MEMBER(BacktestEngine, trades_, 848, 8)
PF_STREAM_LAYOUT_SIZE(NativeExecutionConsumer, 11856)
PF_STREAM_LAYOUT_MEMBER(NativeExecutionConsumer, consumed_high_water_, 2216, 8)
PF_STREAM_LAYOUT_MEMBER(NativeExecutionConsumer, rows_current_, 2224, 1)
PF_STREAM_LAYOUT_MEMBER(NativeExecutionConsumer, running_spec_, 1664, 8)
PF_STREAM_LAYOUT_MEMBER(NativeExecutionConsumer, selected_epoch_.admitted_selected, 11280, 1)
PF_STREAM_LAYOUT_MEMBER(NativeExecutionConsumer, selected_epoch_.generation_counter, 11296, 8)
PF_STREAM_LAYOUT_MEMBER(NativeExecutionConsumer, selected_epoch_.phase_observed, 11281, 1)
PF_STREAM_LAYOUT_MEMBER(NativeExecutionConsumer, state_, 448, 1200)
