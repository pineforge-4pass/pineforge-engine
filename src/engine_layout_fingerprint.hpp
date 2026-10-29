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
// The source guard checks the digest and the preceding measured revision in
// Git history. Updating numbers and both version literals alone cannot pass.
// Set the digest only after measuring the final table on both architectures.
inline constexpr std::uint32_t previous_measured_version = 1u;
inline constexpr char table_sha256[] = "unmeasured";

// name, Linux x86_64 measurement, Linux aarch64 measurement
#define PINEFORGE_MEASURED_ENGINE_LAYOUT(V) \
    V(engine_size, 5056, 5056) \
    V(engine_alignment, 8, 8) \
    V(consumer_size, 10952, 10952) \
    V(consumer_alignment, 8, 8) \
    V(trade_ledger_size, 16, 16) \
    V(engine_execution_consumer_slot, 4064, 4064) \
    V(engine_execution_consumer_ptr, 4072, 4072) \
    V(engine_last_run_status, 3876, 3876) \
    V(engine_report_sink_state, 4416, 4416) \
    V(engine_sink_enabled, 4416, 4416) \
    V(engine_sink_complete, 4720, 4720) \
    V(engine_sink_selected_equity_stream, 4832, 4832) \
    V(engine_sink_active_calls, 4548, 4548) \
    V(engine_sink_stream_calls, 4552, 4552) \
    V(engine_sink_writer, 4560, 4560) \
    V(engine_report_mode, 4872, 4872) \
    V(engine_active_equity_report_mode, 1304, 1304) \
    V(engine_stream_phase, 3416, 3416) \
    V(engine_trades, 848, 848) \
    V(engine_range_end_trades, 864, 864) \
    V(engine_trace_buffer, 3888, 3888) \
    V(engine_initial_capital, 152, 152) \
    V(engine_equity_curve, 1200, 1200) \
    V(engine_broker_state_hashes, 1248, 1248) \
    V(engine_chart_timezone, 624, 624) \
    V(engine_diag_input_bars, 3992, 3992) \
    V(engine_diag_script_bars, 4000, 4000) \
    V(engine_diag_magnifier_sub_bars, 4008, 4008) \
    V(engine_diag_magnifier_ticks, 4016, 4016) \
    V(engine_input_tf, 3032, 3032) \
    V(engine_script_tf_seconds, 3096, 3096) \
    V(engine_diag_script_tf_ratio, 4024, 4024) \
    V(engine_diag_needs_aggregation, 4028, 4028) \
    V(engine_bar_magnifier_enabled, 3008, 3008) \
    V(engine_bars_in_market, 1224, 1224) \
    V(engine_first_bar_open, 1232, 1232) \
    V(engine_current_bar, 768, 768) \
    V(engine_current_bar_close, 792, 792) \
    V(engine_security_eval_states, 3584, 3584) \
    V(security_eval_state_size, 496, 496) \
    V(security_sec_id, 0, 0) \
    V(security_feed_count, 400, 400) \
    V(security_complete_count, 408, 408) \
    V(security_partial_count, 416, 416) \
    V(engine_optional_report_mode, 4928, 4928) \
    V(engine_trace_names, 3912, 3912) \
    V(engine_last_error, 4032, 4032) \
    V(engine_sequence_witness, 4936, 4936) \
    V(engine_sequence_hash_folded, 5000, 5000) \
    V(engine_sequence_hash_folded_leaf, 5008, 5008) \
    V(engine_sequence_row_folded, 5016, 5016) \
    V(engine_output_open_ms, 4128, 4128) \
    V(engine_output_close_ms, 4152, 4152) \
    V(engine_output_series, 4104, 4104) \
    V(engine_outputs_slots, 4084, 4084) \
    V(consumer_rows_current, 1520, 1520) \
    V(consumer_batch_current, 1521, 1521) \
    V(consumer_selected_epoch, 10424, 10424) \
    V(consumer_admitted_selected, 10512, 10512) \
    V(consumer_phase_observed, 10513, 10513) \
    V(consumer_generation_counter, 10528, 10528) \
    V(consumer_dispatching, 10514, 10514) \
    V(consumer_dispatch_reentry_refused, 10515, 10515) \
    V(consumer_state, 280, 280) \
    V(consumer_running_spec, 1496, 1496) \
    V(consumer_consumed_high_water, 1512, 1512) \
    V(consumer_has_floor, 1904, 1904) \
    V(consumer_decision_floor_ms, 1896, 1896) \
    V(consumer_run_failure, 8, 8) \
    V(consumer_failure_code, 8, 8) \
    V(consumer_failure_args, 16, 16) \
    V(consumer_failure_text, 32, 32) \
    V(open_report_source_size, 184, 184) \
    V(open_report_source_alignment, 8, 8) \
    V(engine_pyramid_entries, 128, 128) \
    V(engine_fold_exit_trail_peak, 840, 840) \
    V(engine_commission_type, 160, 160) \
    V(engine_commission_value, 168, 168) \
    V(engine_syminfo, 288, 288) \
    V(engine_syminfo_pointvalue, 584, 584) \
    V(engine_lot_excursion_hook, 2920, 2920) \
    V(engine_account_currency_fx, 200, 200) \
    V(engine_account_currency_fx_timestamps, 208, 208) \
    V(engine_account_currency_fx_rates, 232, 232) \
    V(engine_current_bar_timestamp, 808, 808) \
    V(pyramid_entry_size, 136, 136) \
    V(pyramid_entry_alignment, 8, 8) \
    V(pyramid_entry_price, 0, 0) \
    V(pyramid_entry_time, 8, 8) \
    V(pyramid_entry_qty, 16, 16) \
    V(pyramid_entry_id, 24, 24) \
    V(pyramid_entry_bar_index, 56, 56) \
    V(pyramid_entry_comment, 64, 64) \
    V(pyramid_entry_max_runup, 96, 96) \
    V(pyramid_entry_max_drawdown, 104, 104) \
    V(pyramid_entry_skip_high, 112, 112) \
    V(pyramid_entry_skip_low, 113, 113) \
    V(pyramid_entry_commission, 120, 120) \
    V(pyramid_entry_incarnation, 128, 128)

// Runtime expectations, printed by the probe beside its size/offset values.
// Never infer slots from declaration order or from sizeof(BacktestEngine).
#define PINEFORGE_MEASURED_ENGINE_VIRTUALS(V) \
    V(slot_present_report, 14, 14) \
    V(slot_report_presenter_supports_non_full, 15, 15)

// Assertions cover Linux x86_64 and aarch64 LP64 with libstdc++ only.
// Non-Linux builds explicitly skip this private compatibility guard and must
// not advertise a measured layout to a runner built for these Linux targets.
#if defined(__linux__)
#if !defined(__LP64__) || (!defined(__x86_64__) && !defined(__aarch64__))
#error "Engine layout fingerprint requires a measured Linux LP64 target"
#elif defined(_LIBCPP_VERSION) || !defined(__GLIBCXX__)
#error "Engine layout fingerprint requires libstdc++; this standard library has no measured table"
#else
#if defined(__x86_64__)
#define PINEFORGE_LAYOUT_EXPECTATION(name, x86_64, aarch64) \
    inline constexpr std::size_t name = x86_64;
#else
#define PINEFORGE_LAYOUT_EXPECTATION(name, x86_64, aarch64) \
    inline constexpr std::size_t name = aarch64;
#endif
PINEFORGE_MEASURED_ENGINE_LAYOUT(PINEFORGE_LAYOUT_EXPECTATION)
PINEFORGE_MEASURED_ENGINE_VIRTUALS(PINEFORGE_LAYOUT_EXPECTATION)
#undef PINEFORGE_LAYOUT_EXPECTATION

// Only the standalone probe requests expectations without build assertions.
// Its runtime slot row still rejects every unmeasured entry. The library's
// c_abi.cpp includes this header normally and keeps all build assertions.
#ifndef PINEFORGE_ENGINE_LAYOUT_EXPECTATIONS_ONLY
static_assert(PF_STREAM_ENGINE_LAYOUT_VERSION == measured_version,
    "BacktestEngine layout changed: raise PF_STREAM_ENGINE_LAYOUT_VERSION and update this table");
#define PINEFORGE_LAYOUT_ASSERT(name, expression) \
    static_assert(EngineLayoutProbe::name == name, \
        "BacktestEngine layout changed: raise PF_STREAM_ENGINE_LAYOUT_VERSION and update this table");
PINEFORGE_ENGINE_LAYOUT_VALUES(PINEFORGE_LAYOUT_ASSERT)
#undef PINEFORGE_LAYOUT_ASSERT
#define PINEFORGE_LAYOUT_SLOT_MEASURED(name, member) \
    static_assert(name != unmeasured, "Measure every engine virtual slot before building the library");
PINEFORGE_ENGINE_LAYOUT_VIRTUALS(PINEFORGE_LAYOUT_SLOT_MEASURED)
#undef PINEFORGE_LAYOUT_SLOT_MEASURED
#endif
#endif
#endif

#undef PINEFORGE_MEASURED_ENGINE_LAYOUT
#undef PINEFORGE_MEASURED_ENGINE_VIRTUALS
} // namespace engine_layout
}
}
