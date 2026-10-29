#pragma once

#include <pineforge/engine.hpp>
#include "native_execution_consumer.hpp"
#include <cstddef>
#include <cstring>
#include <limits>

// Real expressions shared by the library's fingerprint and the standalone
// measurement program. Expected measurements live only in the fingerprint.
// Include the enclosing offsets as well as nested fields: a nested reorder
// can preserve both the enclosing object's offset and its total size.
#define PINEFORGE_ENGINE_LAYOUT_VALUES(V) \
    V(engine_size, sizeof(BacktestEngine)) \
    V(engine_alignment, alignof(BacktestEngine)) \
    V(consumer_size, sizeof(NativeExecutionConsumer)) \
    V(consumer_alignment, alignof(NativeExecutionConsumer)) \
    V(trade_ledger_size, sizeof(TradeLedger)) \
    V(engine_execution_consumer_slot, offsetof(BacktestEngine, execution_consumer_slot_)) \
    V(engine_execution_consumer_ptr, offsetof(BacktestEngine, execution_consumer_slot_.ptr)) \
    V(engine_last_run_status, offsetof(BacktestEngine, last_run_status_)) \
    V(engine_report_sink_state, offsetof(BacktestEngine, report_sink_state_)) \
    V(engine_sink_enabled, offsetof(BacktestEngine, report_sink_state_.enabled)) \
    V(engine_sink_complete, offsetof(BacktestEngine, report_sink_state_.receipt.complete)) \
    V(engine_sink_selected_equity_stream, offsetof(BacktestEngine, report_sink_state_.selected_equity_stream)) \
    V(engine_sink_active_calls, offsetof(BacktestEngine, report_sink_state_.active_calls)) \
    V(engine_sink_stream_calls, offsetof(BacktestEngine, report_sink_state_.stream_calls)) \
    V(engine_sink_writer, offsetof(BacktestEngine, report_sink_state_.writer)) \
    V(engine_report_mode, offsetof(BacktestEngine, report_mode_)) \
    V(engine_active_equity_report_mode, offsetof(BacktestEngine, active_equity_report_mode_)) \
    V(engine_stream_phase, offsetof(BacktestEngine, stream_phase_)) \
    V(engine_trades, offsetof(BacktestEngine, trades_)) \
    V(engine_range_end_trades, offsetof(BacktestEngine, range_end_trades_)) \
    V(engine_trace_buffer, offsetof(BacktestEngine, trace_buffer_)) \
    V(engine_initial_capital, offsetof(BacktestEngine, initial_capital_)) \
    V(engine_equity_curve, offsetof(BacktestEngine, equity_curve_)) \
    V(engine_broker_state_hashes, offsetof(BacktestEngine, broker_state_hashes_)) \
    V(engine_chart_timezone, offsetof(BacktestEngine, chart_timezone_)) \
    V(engine_diag_input_bars, offsetof(BacktestEngine, diag_input_bars_processed_)) \
    V(engine_diag_script_bars, offsetof(BacktestEngine, diag_script_bars_processed_)) \
    V(engine_diag_magnifier_sub_bars, offsetof(BacktestEngine, diag_magnifier_sub_bars_processed_)) \
    V(engine_diag_magnifier_ticks, offsetof(BacktestEngine, diag_magnifier_sample_ticks_processed_)) \
    V(engine_input_tf, offsetof(BacktestEngine, input_tf_)) \
    V(engine_script_tf_seconds, offsetof(BacktestEngine, script_tf_seconds_)) \
    V(engine_diag_script_tf_ratio, offsetof(BacktestEngine, diag_script_tf_ratio_)) \
    V(engine_diag_needs_aggregation, offsetof(BacktestEngine, diag_needs_aggregation_)) \
    V(engine_bar_magnifier_enabled, offsetof(BacktestEngine, bar_magnifier_enabled_)) \
    V(engine_bars_in_market, offsetof(BacktestEngine, bars_in_market_)) \
    V(engine_first_bar_open, offsetof(BacktestEngine, first_bar_open_)) \
    V(engine_current_bar, offsetof(BacktestEngine, current_bar_)) \
    V(engine_current_bar_close, offsetof(BacktestEngine, current_bar_.close)) \
    V(engine_security_eval_states, offsetof(BacktestEngine, security_eval_states_)) \
    V(security_eval_state_size, sizeof(BacktestEngine::SecurityEvalState)) \
    V(security_sec_id, offsetof(BacktestEngine::SecurityEvalState, sec_id)) \
    V(security_feed_count, offsetof(BacktestEngine::SecurityEvalState, feed_count)) \
    V(security_complete_count, offsetof(BacktestEngine::SecurityEvalState, eval_complete_count)) \
    V(security_partial_count, offsetof(BacktestEngine::SecurityEvalState, eval_partial_count)) \
    V(engine_optional_report_mode, offsetof(BacktestEngine, optional_report_mode_)) \
    V(engine_trace_names, offsetof(BacktestEngine, trace_names_)) \
    V(engine_last_error, offsetof(BacktestEngine, last_error_)) \
    V(engine_sequence_witness, offsetof(BacktestEngine, sequence_witness_)) \
    V(engine_sequence_hash_folded, offsetof(BacktestEngine, sequence_hash_folded_)) \
    V(engine_sequence_hash_folded_leaf, offsetof(BacktestEngine, sequence_hash_folded_leaf_)) \
    V(engine_sequence_row_folded, offsetof(BacktestEngine, sequence_row_folded_)) \
    V(engine_output_open_ms, offsetof(BacktestEngine, output_open_ms_)) \
    V(engine_output_close_ms, offsetof(BacktestEngine, output_close_ms_)) \
    V(engine_output_series, offsetof(BacktestEngine, output_series_)) \
    V(engine_outputs_slots, offsetof(BacktestEngine, outputs_slots_)) \
    V(consumer_rows_current, offsetof(NativeExecutionConsumer, rows_current_)) \
    V(consumer_batch_current, offsetof(NativeExecutionConsumer, batch_current_)) \
    V(consumer_selected_epoch, offsetof(NativeExecutionConsumer, selected_epoch_)) \
    V(consumer_admitted_selected, offsetof(NativeExecutionConsumer, selected_epoch_.admitted_selected)) \
    V(consumer_phase_observed, offsetof(NativeExecutionConsumer, selected_epoch_.phase_observed)) \
    V(consumer_generation_counter, offsetof(NativeExecutionConsumer, selected_epoch_.generation_counter)) \
    V(consumer_dispatching, offsetof(NativeExecutionConsumer, selected_epoch_.dispatching)) \
    V(consumer_dispatch_reentry_refused, offsetof(NativeExecutionConsumer, selected_epoch_.dispatch_reentry_refused)) \
    V(consumer_state, offsetof(NativeExecutionConsumer, state_)) \
    V(consumer_running_spec, offsetof(NativeExecutionConsumer, running_spec_)) \
    V(consumer_consumed_high_water, offsetof(NativeExecutionConsumer, consumed_high_water_)) \
    V(consumer_has_floor, offsetof(NativeExecutionConsumer, has_floor_)) \
    V(consumer_decision_floor_ms, offsetof(NativeExecutionConsumer, decision_floor_ms_)) \
    V(consumer_run_failure, offsetof(NativeExecutionConsumer, run_failure_)) \
    V(consumer_failure_code, offsetof(NativeExecutionConsumer, run_failure_.code)) \
    V(consumer_failure_args, offsetof(NativeExecutionConsumer, run_failure_.args)) \
    V(consumer_failure_text, offsetof(NativeExecutionConsumer, run_failure_.text)) \
    V(open_report_source_size, sizeof(OpenReportRowSource)) \
    V(open_report_source_alignment, alignof(OpenReportRowSource)) \
    V(engine_pyramid_entries, offsetof(BacktestEngine, pyramid_entries_)) \
    V(engine_fold_exit_trail_peak, offsetof(BacktestEngine, fold_exit_trail_peak_)) \
    V(engine_commission_type, offsetof(BacktestEngine, commission_type_)) \
    V(engine_commission_value, offsetof(BacktestEngine, commission_value_)) \
    V(engine_syminfo, offsetof(BacktestEngine, syminfo_)) \
    V(engine_syminfo_pointvalue, offsetof(BacktestEngine, syminfo_.pointvalue)) \
    V(engine_lot_excursion_hook, offsetof(BacktestEngine, lot_excursion_hook_)) \
    V(engine_account_currency_fx, offsetof(BacktestEngine, account_currency_fx_)) \
    V(engine_account_currency_fx_timestamps, offsetof(BacktestEngine, account_currency_fx_timestamps_)) \
    V(engine_account_currency_fx_rates, offsetof(BacktestEngine, account_currency_fx_rates_)) \
    V(engine_current_bar_timestamp, offsetof(BacktestEngine, current_bar_.timestamp)) \
    V(pyramid_entry_size, sizeof(PyramidEntry)) \
    V(pyramid_entry_alignment, alignof(PyramidEntry)) \
    V(pyramid_entry_price, offsetof(PyramidEntry, price)) \
    V(pyramid_entry_time, offsetof(PyramidEntry, time)) \
    V(pyramid_entry_qty, offsetof(PyramidEntry, qty)) \
    V(pyramid_entry_id, offsetof(PyramidEntry, entry_id)) \
    V(pyramid_entry_bar_index, offsetof(PyramidEntry, entry_bar_index)) \
    V(pyramid_entry_comment, offsetof(PyramidEntry, entry_comment)) \
    V(pyramid_entry_max_runup, offsetof(PyramidEntry, max_runup)) \
    V(pyramid_entry_max_drawdown, offsetof(PyramidEntry, max_drawdown)) \
    V(pyramid_entry_skip_high, offsetof(PyramidEntry, skip_entry_bar_high)) \
    V(pyramid_entry_skip_low, offsetof(PyramidEntry, skip_entry_bar_low)) \
    V(pyramid_entry_commission, offsetof(PyramidEntry, entry_commission_account)) \
    V(pyramid_entry_incarnation, offsetof(PyramidEntry, entry_incarnation))

// Calls through BacktestEngine in the live report reader and its snapshot
// fallback. These slots require runtime checks: sizeof/offsetof cannot see a
// virtual inserted before an existing method.
#define PINEFORGE_ENGINE_LAYOUT_VIRTUALS(V) \
    V(slot_present_report, &BacktestEngine::present_report) \
    V(slot_report_presenter_supports_non_full, &BacktestEngine::report_presenter_supports_non_full)

// These polymorphic implementation classes are deliberately not standard
// layout. GCC and Clang's supported offsetof extension measures their actual
// ABI layout without constructing an engine or dereferencing a null pointer.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
namespace pineforge {
inline namespace engine_script_run_v19 {
struct EngineLayoutProbe {
#define PINEFORGE_LAYOUT_VALUE(name, expression) \
    static constexpr std::size_t name = expression;
    PINEFORGE_ENGINE_LAYOUT_VALUES(PINEFORGE_LAYOUT_VALUE)
#undef PINEFORGE_LAYOUT_VALUE

#if defined(__linux__)
    template<class Member>
    static std::size_t virtual_slot(Member member) noexcept {
        // Linux Itanium member-function pointers contain a function/offset
        // word and a this-adjustment word. No engine or vtable is constructed.
        struct Representation {
            std::ptrdiff_t function;
            std::ptrdiff_t adjustment;
        } raw{};
        static_assert(sizeof member == sizeof raw, "Unsupported member-function pointer ABI");
        std::memcpy(&raw, &member, sizeof raw);
        constexpr auto invalid = std::numeric_limits<std::size_t>::max();
        std::ptrdiff_t byte_offset;
#if defined(__aarch64__)
        // AArch64 uses the ARM variant: the virtual flag is in the low bit
        // of the adjustment; the function word is the untagged byte offset.
        if (raw.adjustment != 1) return invalid;
        byte_offset = raw.function;
#else
        // Standard Itanium encoding: function = 1 + vtable byte offset.
        if (raw.adjustment != 0 || raw.function <= 0 || raw.function % 2 != 1)
            return invalid;
        byte_offset = raw.function - 1;
#endif
        if (byte_offset < 0 || byte_offset % sizeof(void*) != 0) return invalid;
        return static_cast<std::size_t>(byte_offset) / sizeof(void*);
    }

#define PINEFORGE_LAYOUT_VIRTUAL(name, member) \
    static std::size_t name() noexcept { return virtual_slot(member); }
    PINEFORGE_ENGINE_LAYOUT_VIRTUALS(PINEFORGE_LAYOUT_VIRTUAL)
#undef PINEFORGE_LAYOUT_VIRTUAL
#endif
};
}
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
