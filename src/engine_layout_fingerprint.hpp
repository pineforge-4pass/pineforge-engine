#pragma once

#include "engine_layout_probe.hpp"
#include <limits>

namespace pineforge {
inline namespace engine_script_run_v19 {
namespace engine_layout {

// Fill both columns from test_engine_layout_probe on the corresponding
// Linux LP64 toolchain, after all engine layout changes have been integrated.
// An unmeasured entry intentionally fails the library build; it is never an
// accepted size/offset and must not be replaced by an expression of the layout.
inline constexpr std::size_t unmeasured = std::numeric_limits<std::size_t>::max();
inline constexpr std::uint32_t measured_version = 2u;

// name, Linux x86_64 measurement, Linux aarch64 measurement
#define PINEFORGE_MEASURED_ENGINE_LAYOUT(V) \
    V(engine_size, unmeasured, unmeasured) \
    V(engine_alignment, unmeasured, unmeasured) \
    V(consumer_size, unmeasured, unmeasured) \
    V(consumer_alignment, unmeasured, unmeasured) \
    V(trade_ledger_size, unmeasured, unmeasured) \
    V(engine_execution_consumer_slot, unmeasured, unmeasured) \
    V(engine_execution_consumer_ptr, unmeasured, unmeasured) \
    V(engine_last_run_status, unmeasured, unmeasured) \
    V(engine_report_sink_state, unmeasured, unmeasured) \
    V(engine_sink_enabled, unmeasured, unmeasured) \
    V(engine_sink_complete, unmeasured, unmeasured) \
    V(engine_sink_selected_equity_stream, unmeasured, unmeasured) \
    V(engine_sink_active_calls, unmeasured, unmeasured) \
    V(engine_sink_stream_calls, unmeasured, unmeasured) \
    V(engine_sink_writer, unmeasured, unmeasured) \
    V(engine_report_mode, unmeasured, unmeasured) \
    V(engine_active_equity_report_mode, unmeasured, unmeasured) \
    V(engine_stream_phase, unmeasured, unmeasured) \
    V(engine_trades, unmeasured, unmeasured) \
    V(engine_range_end_trades, unmeasured, unmeasured) \
    V(engine_trace_buffer, unmeasured, unmeasured) \
    V(engine_initial_capital, unmeasured, unmeasured) \
    V(engine_equity_curve, unmeasured, unmeasured) \
    V(engine_broker_state_hashes, unmeasured, unmeasured) \
    V(engine_chart_timezone, unmeasured, unmeasured) \
    V(engine_diag_input_bars, unmeasured, unmeasured) \
    V(engine_diag_script_bars, unmeasured, unmeasured) \
    V(engine_diag_magnifier_sub_bars, unmeasured, unmeasured) \
    V(engine_diag_magnifier_ticks, unmeasured, unmeasured) \
    V(engine_input_tf, unmeasured, unmeasured) \
    V(engine_script_tf_seconds, unmeasured, unmeasured) \
    V(engine_diag_script_tf_ratio, unmeasured, unmeasured) \
    V(engine_diag_needs_aggregation, unmeasured, unmeasured) \
    V(engine_bar_magnifier_enabled, unmeasured, unmeasured) \
    V(engine_bars_in_market, unmeasured, unmeasured) \
    V(engine_first_bar_open, unmeasured, unmeasured) \
    V(engine_current_bar, unmeasured, unmeasured) \
    V(engine_current_bar_close, unmeasured, unmeasured) \
    V(engine_security_eval_states, unmeasured, unmeasured) \
    V(security_eval_state_size, unmeasured, unmeasured) \
    V(security_sec_id, unmeasured, unmeasured) \
    V(security_feed_count, unmeasured, unmeasured) \
    V(security_complete_count, unmeasured, unmeasured) \
    V(security_partial_count, unmeasured, unmeasured) \
    V(engine_optional_report_mode, unmeasured, unmeasured) \
    V(engine_trace_names, unmeasured, unmeasured) \
    V(engine_last_error, unmeasured, unmeasured) \
    V(engine_sequence_witness, unmeasured, unmeasured) \
    V(engine_sequence_hash_folded, unmeasured, unmeasured) \
    V(engine_sequence_hash_folded_leaf, unmeasured, unmeasured) \
    V(engine_sequence_row_folded, unmeasured, unmeasured) \
    V(engine_output_open_ms, unmeasured, unmeasured) \
    V(engine_output_close_ms, unmeasured, unmeasured) \
    V(engine_output_series, unmeasured, unmeasured) \
    V(engine_outputs_slots, unmeasured, unmeasured) \
    V(consumer_rows_current, unmeasured, unmeasured) \
    V(consumer_batch_current, unmeasured, unmeasured) \
    V(consumer_selected_epoch, unmeasured, unmeasured) \
    V(consumer_admitted_selected, unmeasured, unmeasured) \
    V(consumer_phase_observed, unmeasured, unmeasured) \
    V(consumer_generation_counter, unmeasured, unmeasured) \
    V(consumer_dispatching, unmeasured, unmeasured) \
    V(consumer_dispatch_reentry_refused, unmeasured, unmeasured) \
    V(consumer_state, unmeasured, unmeasured) \
    V(consumer_running_spec, unmeasured, unmeasured) \
    V(consumer_consumed_high_water, unmeasured, unmeasured) \
    V(consumer_has_floor, unmeasured, unmeasured) \
    V(consumer_decision_floor_ms, unmeasured, unmeasured) \
    V(consumer_run_failure, unmeasured, unmeasured) \
    V(consumer_failure_code, unmeasured, unmeasured) \
    V(consumer_failure_args, unmeasured, unmeasured) \
    V(consumer_failure_text, unmeasured, unmeasured)

#if defined(__linux__)
#if !defined(__LP64__) || (!defined(__x86_64__) && !defined(__aarch64__))
#error "Engine layout fingerprint requires a measured Linux LP64 target"
#endif
#if defined(__x86_64__)
#define PINEFORGE_LAYOUT_EXPECTATION(name, x86_64, aarch64) \
    inline constexpr std::size_t name = x86_64;
#else
#define PINEFORGE_LAYOUT_EXPECTATION(name, x86_64, aarch64) \
    inline constexpr std::size_t name = aarch64;
#endif
PINEFORGE_MEASURED_ENGINE_LAYOUT(PINEFORGE_LAYOUT_EXPECTATION)
#undef PINEFORGE_LAYOUT_EXPECTATION

static_assert(PF_STREAM_ENGINE_LAYOUT_VERSION == measured_version,
    "BacktestEngine layout changed: raise PF_STREAM_ENGINE_LAYOUT_VERSION and update this table");
#define PINEFORGE_LAYOUT_ASSERT(name, expression) \
    static_assert(EngineLayoutProbe::name == name, \
        "BacktestEngine layout changed: raise PF_STREAM_ENGINE_LAYOUT_VERSION and update this table");
PINEFORGE_ENGINE_LAYOUT_VALUES(PINEFORGE_LAYOUT_ASSERT)
#undef PINEFORGE_LAYOUT_ASSERT
#endif

#undef PINEFORGE_MEASURED_ENGINE_LAYOUT
} // namespace engine_layout
}
}
