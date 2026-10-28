/*
 * engine_report.cpp — fill_report / free_report / trace recording
 */

#include "engine_internal.hpp"
#include "native_execution_consumer.hpp"
#include "selected_report_view.hpp"
#include <pineforge/metrics_stream.hpp>
#include "report_scratch.hpp"
#include "strategy_history_policy.hpp"
#include <pineforge/metrics.hpp>
#include <pineforge/detail/scratch_sequence.hpp>
#include "report_value_invalid.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unordered_set>

namespace pineforge {
using namespace internal;

metrics::MetricsStreamingStatusV1 BacktestEngine::set_metrics_streaming_v1(
        metrics::StreamingSharpeV1 mode, metrics::MetricsNeedMaskV1 needed) {
    using namespace metrics;
    if ((mode != StreamingSharpeV1::On && mode != StreamingSharpeV1::Off)
        || (needed & ~kMetricsNeedAllV1))
        return MetricsStreamingStatusV1::InvalidArgument;
    if (report_sink_setter_status() != 0) return MetricsStreamingStatusV1::Busy;
    auto& s = metrics_streaming_;
    s.configuration.streaming_sharpe = mode;
    s.configuration.metrics_needed = needed;
    // Changing preferences cannot renew another invocation's proof.
    s.pending_plain = s.prepared = false;
    s.pending_plan = {};
    s.pending_failed_plain_inputs = 0;
    if (!s.active) {
        s.receipt = {};
        s.receipt.streaming_sharpe = mode;
        s.receipt.metrics_needed = needed;
        s.receipt.route = MetricsRatioRouteV1::LegacyActualCurve;
        s.receipt.route_reason = MetricsRouteReasonV1::PlainNo;
    }
    return MetricsStreamingStatusV1::Ok;
}

metrics::MetricsStreamingStatusV1 BacktestEngine::metrics_extent_receipt_v1(
        metrics::MetricsExtentReceiptV1* out) const {
    using namespace metrics;
    if (!out || out->struct_size != sizeof(*out) || out->version != 1)
        return MetricsStreamingStatusV1::InvalidArgument;
    auto& s = metrics_streaming_;
    if (s.no_curve && s.fold) {
        finish_metrics_streaming_v1({});
    } else if (s.active && !s.receipt_final) {
        // Obtain the existing presentation exactly once through its ordinary
        // report/capture gate. Recorded endpoints alone cannot witness a
        // custom presenter's final metric sequence.
        ReportC report{};
        try {
            fill_report(&report);
        } catch (...) {
            free_report(&report);
            throw;
        }
        free_report(&report);
    } else if (!s.active) {
        s.receipt.streaming_sharpe = s.configuration.streaming_sharpe;
        s.receipt.metrics_needed = s.configuration.metrics_needed;
        s.receipt.route = MetricsRatioRouteV1::LegacyActualCurve;
    }
    *out = s.receipt;
    return MetricsStreamingStatusV1::Ok;
}

metrics::MetricsStreamingStatusV1 BacktestEngine::prepare_metrics_streaming_v1(
        const metrics::MetricsStreamingConfigV1& config,
        const metrics::MetricsExtentPlanV1& plan) {
    using namespace metrics;
    if ((config.streaming_sharpe != StreamingSharpeV1::On
         && config.streaming_sharpe != StreamingSharpeV1::Off)
        || (config.metrics_needed & ~kMetricsNeedAllV1))
        return MetricsStreamingStatusV1::InvalidArgument;
    if (report_sink_setter_status() != 0) return MetricsStreamingStatusV1::Busy;
    auto& s = metrics_streaming_;
    const auto& admitted = s.pending_plan;
    const bool same_plan = plan.present == admitted.present
        && plan.extent.empty == admitted.extent.empty
        && plan.extent.count == admitted.extent.count
        && (plan.extent.empty
            || (plan.extent.first_time_ms == admitted.extent.first_time_ms
                && plan.extent.last_time_ms == admitted.extent.last_time_ms))
        && plan.attempt == admitted.attempt
        && plan.input_generation == admitted.input_generation
        && plan.admitted_view == admitted.admitted_view
        && plan.spec_identity == admitted.spec_identity;
    s.prepared_configuration = config;
    s.configuration.streaming_sharpe = config.streaming_sharpe;
    s.configuration.metrics_needed = config.metrics_needed;
    s.prepared = true;
    if (!same_plan) s.pending_plain = false;
    // Absence of a recovery certificate keeps the actual curve from bar 1;
    // it never introduces a run refusal or a guessed-prefix rf.
    if (!s.pending_plain || config.deterministic_replay != 1)
        s.prepared_configuration.curve_available = 1;
    return MetricsStreamingStatusV1::Ok;
}

void BacktestEngine::begin_metrics_streaming_v1(bool admitted_shape) {
    using namespace metrics;
    auto& s = metrics_streaming_;
    auto config = s.prepared ? s.prepared_configuration : s.configuration;
    const auto plan = s.pending_plan;
    const bool plain = s.pending_plain && admitted_shape;
    const auto failed = s.pending_failed_plain_inputs
        | (s.pending_plain && !admitted_shape ? 1ULL << 24 : 0);
    s.pending_plain = s.prepared = false; // proof is consumed by THIS begin
    s.pending_plan = {};
    s.pending_failed_plain_inputs = 0;
    s.fold.reset();
    s.active = true;
    s.plain = plain;
    s.no_curve = false;
    s.points = 0;
    s.receipt_final = false;
    s.result = {};
    s.receipt = {};
    s.receipt.extent_plan = plan;
    s.receipt.plain_subset = plain ? 1u : 0u;
    s.receipt.failed_plain_inputs = failed;
    s.receipt.streaming_sharpe = config.streaming_sharpe;
    s.receipt.metrics_needed = config.metrics_needed;
    s.receipt.route = MetricsRatioRouteV1::LegacyActualCurve;
    s.receipt.route_reason = MetricsRouteReasonV1::PlainNo;
    s.run_configuration = config;
    if (!plain) return;
    const auto needs_curve = kMetricsNeedReturnsV1 | kMetricsNeedCurveV1;
    const bool bound = plan.present == 1 && plan.attempt == config.attempt
        && plan.input_generation == config.input_generation
        && plan.admitted_view == config.admitted_view
        && plan.spec_identity == config.spec_identity
        && ((plan.extent.empty == 1 && plan.extent.count == 0)
            || (plan.extent.empty == 0 && plan.extent.count > 0));
    const bool off_needs_sharpe = config.streaming_sharpe == StreamingSharpeV1::Off
        && (config.metrics_needed & kMetricsNeedSharpeBarV1);
    const bool month_domain = plan.extent.empty
        || (monthly_replay_time_supported_v1(plan.extent.first_time_ms)
            && monthly_replay_time_supported_v1(plan.extent.last_time_ms));
    // A sink/observer or a legacy SUMMARY_ONLY provider keeps its own
    // existing capture contract. No speculative E bytes or private curve
    // are smuggled into a prepared no-curve attempt.
    s.no_curve = config.curve_available == 0 && config.deterministic_replay == 1 && bound
        && month_domain
        && !(config.metrics_needed & needs_curve) && !off_needs_sharpe
        && !report_sink_state_.enabled
        && !NativeExecutionConsumer::observer_registered(*this)
        && std::fegetround() == FE_TONEAREST
        && pending_equity_report_mode_ == ReportMode::FULL;
    if (!s.no_curve) {
        if (!month_domain) s.receipt.route_reason = MetricsRouteReasonV1::NumericGuard;
        // The exact ordered replay is only needed for the no-curve route.
        // Existing curve routes keep their established provider and do not
        // allocate a second scratch history.
        config.curve_available = 1;
        s.run_configuration = config;
        return;
    }
    s.run_configuration = config;
    try {
        if (make_streaming_equity_ratios_v1(config, plan, {}, s.fold)
                == MetricsStreamingStatusV1::Ok
            && enable_streaming_monthly_replay_v1(*s.fold) == MetricsStreamingStatusV1::Ok)
            return;
    } catch (const std::bad_alloc&) {
        // An allocation used ONLY by this optimization cannot replace an
        // otherwise readable run with a new resource failure.
    } catch (const detail::ReportScratchIoError&) {
        // An unjailed report scratch create failure can use the established
        // resident curve route. An attached arena failure is a typed run
        // failure: falling back to RAM would hide the jailed resource limit.
        if (detail::ScratchArena::attached()) throw;
        s.receipt.route_reason = MetricsRouteReasonV1::ScratchUnavailable;
    }
    s.fold.reset();
    s.no_curve = false;
    s.run_configuration.curve_available = 1;
    if (s.receipt.route_reason == MetricsRouteReasonV1::PlainNo
        || s.receipt.route_reason == MetricsRouteReasonV1::NoRecoveryProof)
        s.receipt.route_reason = MetricsRouteReasonV1::ScratchUnavailable;
}

void BacktestEngine::sync_metrics_streaming_held_v1() const {
    auto& s = metrics_streaming_;
    if (s.fold && s.points > 0) {
        if (const auto* held = last_equity_point())
            s.fold->amend_held(s.points - 1, *held,
                report_equity_month_key(held->time_ms));
    }
}

void BacktestEngine::finish_metrics_streaming_v1(
        metrics::MetricsRatioCurveV1 curve, pf_equity_stats_t* equity) const {
    using namespace metrics;
    auto& s = metrics_streaming_;
    const auto boundary = s.receipt.stop_boundary;
    const auto failed = s.receipt.failed_plain_inputs;
    const bool execution_failed = last_run_status_ != 0 || !last_error_.empty();
    if (s.fold && s.plain && (s.no_curve || (!execution_failed && curve.count == s.points))) {
        sync_metrics_streaming_held_v1();
        if (curve.read && curve.count > 0) {
            const auto tail = curve.read(curve.context, curve.count - 1);
            s.fold->amend_held(s.points - 1, tail,
                report_equity_month_key(tail.time_ms));
        }
        const auto status = s.fold->finish(s.result, curve);
        if (status == MetricsStreamingStatusV1::Ok) {
            s.receipt = s.result.receipt;
            s.receipt.plain_subset = 1;
            s.receipt.failed_plain_inputs = failed;
            s.receipt.stop_boundary = boundary;
            MetricsMonthlyResultV1 monthly;
            const bool monthly_complete = streaming_monthly_result_v1(*s.fold, monthly)
                == MetricsStreamingStatusV1::Ok && monthly.complete;
            // Monthly replay replaces the curve only when complete.
            // Interrupted attempts still need whole-report recovery because
            // their presentation/completion is unproved.
            if (s.no_curve && (execution_failed
                || (s.receipt.route == MetricsRatioRouteV1::Streaming && !monthly_complete))) {
                s.receipt.route = MetricsRatioRouteV1::LegacyRerun;
                s.receipt.route_reason = execution_failed
                    ? MetricsRouteReasonV1::ExistingFailure : MetricsRouteReasonV1::CurveNeeded;
                s.receipt.replay_attempts = 1; // host must resolve before publication
                s.receipt.authoritative_attempt = 0;
            }
            s.receipt_final = true;
            if (equity && s.receipt.route == MetricsRatioRouteV1::Streaming
                && !s.result.failure_code) {
                if (s.receipt.metrics_needed & kMetricsNeedSharpeBarV1)
                    equity->sharpe_bar = s.result.sharpe_bar;
                if (s.receipt.metrics_needed & kMetricsNeedSortinoBarV1)
                    equity->sortino_bar = s.result.sortino_bar;
            }
            return;
        }
    }
    // No trusted stream identity: observe the final authoritative curve
    // without changing ANY legacy computation or failure stage.
    auto& receipt = s.receipt;
    receipt.route = MetricsRatioRouteV1::LegacyActualCurve;
    if (execution_failed) receipt.route_reason = MetricsRouteReasonV1::ExistingFailure;
    if (receipt.route_reason == MetricsRouteReasonV1::None)
        receipt.route_reason = MetricsRouteReasonV1::PlainNo;
    receipt.actual_extent = {};
    receipt.eligible_returns = 0;
    if (curve.count > 0 && curve.read) {
        receipt.actual_extent.empty = 0;
        receipt.actual_extent.count = curve.count;
        auto point = curve.read(curve.context, 0);
        receipt.actual_extent.first_time_ms = point.time_ms;
        for (std::int64_t i = 1; i < curve.count; ++i) {
            if (point.equity > 0.0) ++receipt.eligible_returns;
            point = curve.read(curve.context, i);
        }
        receipt.actual_extent.last_time_ms = point.time_ms;
    }
    const auto& config = s.run_configuration;
    receipt.actual_attempt = config.attempt;
    receipt.actual_input_generation = config.input_generation;
    receipt.actual_admitted_view = config.admitted_view;
    receipt.actual_spec_identity = config.spec_identity;
    receipt.authoritative_attempt = config.attempt;
    receipt.mismatch_fields = fold_detail::extent_mismatch_v1(
        receipt.extent_plan, config, receipt.actual_extent);
    receipt.witness = !receipt.extent_plan.present ? MetricsExtentWitnessV1::NotEvaluated
        : receipt.mismatch_fields ? MetricsExtentWitnessV1::Mismatch : MetricsExtentWitnessV1::Match;
    s.receipt_final = true;
}

int32_t BacktestEngine::intern_trace_name(const std::string& name) {
    auto it = trace_name_index_.find(name);
    if (it != trace_name_index_.end()) return it->second;
    int32_t id = static_cast<int32_t>(trace_names_.size());
    trace_names_.push_back(name);
    trace_name_index_.emplace(name, id);
    return id;
}


void BacktestEngine::trace(const std::string& name, double value) {
    if (!trace_enabled_) return;
    TraceEntryC e;
    e.timestamp = current_bar_.timestamp;
    e.bar_index = bar_index_;
    e.name_id = 0;
    e.value = value;
    // A sink carries the name text in each X record, so the resident interned
    // name table is not consulted (R16: streaming is O(1) in distinct names).
    if (stream_report_sink_trace(name, e)) {
        fold_trace_leaf(sequence_witness_.traces, e.timestamp, e.bar_index, name, value);
        return;
    }
    if (optional_report_mode_ == report_sequence::ReportMode::SummaryOnly) {
        fold_trace_leaf(sequence_witness_.traces, e.timestamp, e.bar_index, name, value);
        return;
    }
    e.name_id = intern_trace_name(name);
    trace_buffer_.push_back(e);
}


// The report of the latest run or stream_begin call this engine was given, or
// the empty report. The engine keeps the rows of the last run it BEGAN, and a
// call that never began (a begin outside Ready, a refused bar array or run
// option, a calendar or timezone refusal) leaves them in place on purpose, so
// the rows alone cannot say which call they answer. Every batch reader shares
// this one gate: strategy_native_run_v1, the native module wrappers and the
// generated run_backtest / run_backtest_full, which call run() and then this.
// Begun runs publish completed or partial rows unless report_sink_failed is latched,
// even at finish after run success: every later read gives empty ReportC (all arrays NULL,
// all counts zero), avoiding retries outside run()'s catch; status, code, arguments and
// message stay in the run-failure latch. Refused calls overwrite unowned storage without freeing.
// Recursive capture/observer reads are refused first; their terminal helper
// consumes the reentry latch. strategy_stream_fill_report uses this same
// gate via fill_snapshot_report but asks for rows_current_ instead of
// batch_current_, preserving a live stream's rows after a refused batch.
void BacktestEngine::fill_report(ReportC* out) const try {
    const ReportSinkCallScope sink_call(*this);
    if (NativeExecutionConsumer::refuse_report_in_dispatch(*this, out)) return;
    if (!NativeExecutionConsumer::rows_are_current(*this)
        || (last_run_status_ != 0
            && run_failure_value_of(*this).code == RunFailureCode::report_sink_failed)) {
        *out = ReportC{};
        return;
    }
    if (metrics_streaming_.no_curve) {
        finish_metrics_streaming_v1({});
        if (metrics_streaming_.receipt.route == metrics::MetricsRatioRouteV1::LegacyRerun) {
            // A prepared adapter owns this speculative attempt and must
            // resolve its receipt with the authoritative whole-run replay.
            // No partial ratio/report or sink publication escapes first.
            *out = ReportC{};
            return;
        }
    }
    // Sealed selected/observed generations use their capture (or the empty
    // report). A live selected sink cannot supply the ordinary full prefix.
    if (NativeExecutionConsumer::fill_selected_report(*this, out)) {
        finish_report_sink(out);
        return;
    }
    if (report_sink_state_.selected_equity_stream) {
        // Refuse before allocating rows or staging a presented execution point.
        // A host may catch this read refusal and continue the same generation.
        *out = ReportC{};
        throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
            {{"shape", "live_selected_sink"}},
            "native selected report: a live sink report is unavailable before capture");
    }
    // Finalize the optional sequences the run accumulated. Both calls are
    // idempotent: the summary witness counts each row/hash once, and the sink
    // path has already streamed and retired them.
    fold_pending_hashes_into_witness();
    fold_current_output_row_witness();
    fill_trades_section(out);

    out->input_bars_processed = diag_input_bars_processed_;
    out->script_bars_processed = diag_script_bars_processed_;
    out->magnifier_sub_bars_total = diag_magnifier_sub_bars_processed_;
    out->magnifier_sample_ticks_total = diag_magnifier_sample_ticks_processed_;
    out->input_tf_seconds = tf_to_seconds(input_tf_);
    out->script_tf_seconds = script_tf_seconds_;
    out->script_tf_ratio = diag_script_tf_ratio_;
    out->needs_aggregation = diag_needs_aggregation_ ? 1 : 0;
    out->bar_magnifier_enabled = bar_magnifier_enabled_ ? 1 : 0;

    fill_metrics_section(out);  // reads out->trades — keep after fill_trades_section

    fill_security_diag_section(out);
    if (report_mode_ == ReportMode::FULL) {
        fill_trace_section(out);

        // ABI v4 task 6: per-script-bar broker-state hash, empty unless
        // recording was enabled via set_broker_state_hash_recording. Owns the
        // allocation; freed by free_report. SummaryOnly keeps the fixed ordered
        // witness and publishes no owning array (R14).
        if (optional_report_mode_ == report_sequence::ReportMode::SummaryOnly) {
            out->broker_state_hash_len = 0;
            out->broker_state_hash = nullptr;
        } else {
            const int64_t hn = (int64_t)broker_state_hashes_.size();
            out->broker_state_hash_len = hn;
            out->broker_state_hash = hn > 0 ? new uint64_t[hn] : nullptr;
            if (hn > 0) std::copy(broker_state_hashes_.begin(), broker_state_hashes_.end(), out->broker_state_hash);
        }
    } else {
        // SUMMARY_ONLY captures no optional history. STREAMED already emitted
        // trace entries as they were recorded and writes the hash sequence to
        // the sink at finish; neither keeps a resident report array here.
        out->trace = nullptr;
        out->trace_len = 0;
        out->trace_names = nullptr;
        out->trace_names_len = 0;
        out->broker_state_hash = nullptr;
        out->broker_state_hash_len = 0;
    }
    finish_report_sink(out);
} catch (const std::exception& error) {
    // A C wrapper can ask for partial rows after execution already failed.
    // A later invalid metric must not replace that latched typed failure.
    if (!last_error().empty()
        && classify_run_failure(error).code == RunFailureCode::report_result_invalid) {
        throw coded<std::runtime_error>(run_failure_value_of(*this), last_error());
    }
    throw;
}

// Copy ``trades_`` — followed by ``range_end_trades_``, the report-only rows
// of a position still open after the final bar
// (NativeExecutionConsumer::append_open_position_report_rows) — into a
// freshly heap-allocated TradeC[] on ``out`` and accumulate ``net_profit``.
// A FULL sink allocates only the range-end view. The SUMMARY_ONLY and STREAMED
// routes allocate no TradeC array at all: they keep the logical total and fold
// the same rows one at a time. free_report owns the FULL arrays.
void BacktestEngine::fill_trades_section(ReportC* out) const {
    const std::size_t closed = trades_.size();
    const std::size_t open = range_end_trades_.size();
    if (closed > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || open > static_cast<std::size_t>(std::numeric_limits<int>::max()) - closed)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report trade count exceeds the report row limit");
    const bool sink_generation = report_sink_state_.enabled || report_sink_state_.receipt.complete;
    if (!sink_generation && trades_.base() != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "array report requires the sealed trade rows in the report sink");
    const int n_closed = (int)trades_.size();
    const int n = n_closed + (int)range_end_trades_.size();
    out->total_trades = 0;
    out->trades_len = 0;
    out->net_profit = 0.0;
    if (report_mode_ != ReportMode::FULL) {
        // O1 R13: summary and streamed capture keep the logical total and the
        // exact left fold and materialize no TradeC row array. Every row
        // enters once, in report order: the closed rows first, then one
        // rebuilt range-end row per open lot. A streamed run's closed rows
        // are already at the sink cursor, so only their fold prefix is read.
        out->trades = nullptr;
        double net = 0.0;
        if (sink_generation) {
            net = report_closed_net_profit();
            out->net_profit = net;
            out->total_trades = n_closed;
        } else {
            for (std::size_t index = trades_.base(); index < trades_.size(); ++index) {
                const Trade trade = trades_.numeric_at(index);
                report::validate_trade(trade);
                ++out->total_trades;
                net = report::finite_value(net + trade.pnl, "summary.net_pnl");
                out->net_profit = net;
            }
        }
        for (const auto& row : range_end_trades_) {
            report::validate_trade(row);
            ++out->total_trades;
            net = report::finite_value(net + row.pnl, "summary.net_pnl");
            out->net_profit = net;
        }
        return;
    }
    if (sink_generation) {
        // The hook can re-mark open lots, but closed history is immutable.
        // Never materialize that history merely to discard it at finalization.
        const int open_count = (int)range_end_trades_.size();
        out->net_profit = report_closed_net_profit();
        out->total_trades = n_closed;
        out->trades = open_count > 0 ? new TradeC[open_count] : nullptr;
        for (int i = 0; i < open_count; ++i) {
            const Trade row = range_end_trades_.row((std::size_t)i);
            report::validate_trade(row);
            out->trades[i] = report_trade_row(row);
            out->trades_len = i + 1;
            out->total_trades = n_closed + i + 1;
            out->net_profit = report::finite_value(
                out->net_profit + out->trades[i].pnl, "summary.net_pnl");
        }
        return;
    }

    if (n > 0) {
        out->trades = new TradeC[n];
        double net_profit = 0.0;

        for (int i = 0; i < n; i++) {
            // Closed and terminal rows are detached values. Only the FULL
            // route's owning output array retains per-row report storage.
            const Trade t = (i < n_closed)
                ? trades_.numeric_at(static_cast<std::size_t>(i))
                : range_end_trades_.row(static_cast<std::size_t>(i - n_closed));
            report::validate_trade(t);
            out->trades[i].entry_time = t.entry_time;
            out->trades[i].exit_time = t.exit_time;
            out->trades[i].entry_price = t.entry_price;
            out->trades[i].exit_price = t.exit_price;
            out->trades[i].pnl = t.pnl;
            out->trades[i].pnl_pct = t.pnl_pct;
            out->trades[i].is_long = t.is_long ? 1 : 0;
            out->trades[i].max_runup = t.max_runup;
            out->trades[i].max_drawdown = t.max_drawdown;
            out->trades[i].qty = t.qty;
            out->trades[i].commission = t.commission;
            out->trades[i].entry_bar_index = t.entry_bar_index;
            out->trades[i].exit_bar_index = t.exit_bar_index;
            out->trades[i].open_at_end = t.open_at_end ? 1 : 0;
            // A partial report keeps each fully initialized row, including
            // this one if only the following aggregate overflows.
            out->trades_len = out->total_trades = i + 1;
            net_profit = report::finite_value(net_profit + t.pnl, "summary.net_pnl");
            out->net_profit = net_profit;
        }
    } else {
        out->trades = nullptr;
    }
}

double BacktestEngine::report_closed_net_profit() const {
    double sum = 0.0;
    if (trades_.base() != 0) {
        const auto* consumer = static_cast<const NativeExecutionConsumer*>(
            execution_consumer_slot_.ptr.get());
        if (!consumer || consumer->closed_trade_prefix().base != trades_.base())
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "report trade fold is missing its sealed prefix");
        sum = report::finite_value(consumer->closed_trade_prefix().pnl, "summary.net_pnl");
    }
    // Continue the original left fold; the decision counters use other seeds.
    for (std::size_t index = trades_.base(); index < trades_.size(); ++index) {
        const Trade trade = trades_.numeric_at(index);
        report::validate_trade(trade);
        sum = report::finite_value(sum + trade.pnl, "summary.net_pnl");
    }
    return sum;
}

void BacktestEngine::compute_report_sink_trade_metrics(
        ReportC* out, const TradeC* open_rows, int count) const {
    // Keep each filter's original left-fold order: closed rows, then the
    // presented range-end rows. Interleaving independent filters changes none
    // of their additions. No TradeC history or separately summed tail exists.
    if (trades_.base() != 0) {
        const auto* consumer = static_cast<const NativeExecutionConsumer*>(
            execution_consumer_slot_.ptr.get());
        if (!consumer)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "report trade metrics are missing their sealed prefix");
        consumer->finish_streamed_trade_metrics(*this, out, open_rows, count);
        return;
    }
    metrics::TradeStatsAccumulator all(metrics::TradeFilter::ALL, initial_capital_);
    metrics::TradeStatsAccumulator longs(metrics::TradeFilter::LONG, initial_capital_);
    metrics::TradeStatsAccumulator shorts(metrics::TradeFilter::SHORT, initial_capital_);
    const auto fold = [&](const TradeC& row) {
        all.fold(row);
        longs.fold(row);
        shorts.fold(row);
    };
    for (std::size_t index = trades_.base(); index < trades_.size(); ++index)
        fold(report_trade_row(trades_.numeric_at(index)));
    if (open_rows != nullptr || report_mode_ == ReportMode::FULL) {
        // FULL still folds the caller's presented rows (a selected capture
        // may deliberately publish none of them).
        for (int i = 0; i < count; ++i) fold(open_rows[i]);
    } else {
        // O1 R12/R13: no resident open-row array in summary/streamed capture;
        // rebuild each terminal row from its lot in the same order and fold it
        // once.
        if (count != 0)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "report trade metrics lost their open rows");
        for (const auto& row : range_end_trades_) fold(report_trade_row(row));
    }
    out->metrics.all = all.finish();
    out->metrics.longs = longs.finish();
    out->metrics.shorts = shorts.finish();
}


namespace internal {

// The metric blocks of a report from explicit values; see engine_internal.hpp.
// The statements are the ones fill_metrics_section ran inline after
// present_report, in the same order.
void compute_report_metrics(pf_metrics_t& result,
                            const TradeC* trades, int trades_len,
                            const pf_equity_point_t* equity_curve, int64_t equity_len,
                            double net_profit, double initial_capital,
                            const std::string& chart_timezone,
                            double first_bar_open, double last_close,
                            int64_t bars_in_market, int64_t observation_count) {
    using metrics::TradeFilter;
    result.all = metrics::compute_trade_stats(
        trades, trades_len, TradeFilter::ALL, initial_capital);
    result.longs = metrics::compute_trade_stats(
        trades, trades_len, TradeFilter::LONG, initial_capital);
    result.shorts = metrics::compute_trade_stats(
        trades, trades_len, TradeFilter::SHORT, initial_capital);
    result.equity = metrics::compute_equity_stats(
        equity_curve, equity_len, initial_capital, chart_timezone,
        first_bar_open, last_close, bars_in_market,
        net_profit,    // includes the range-end rows, like the trades
        observation_count);
}

// The ordinary entry: every point of the curve is an observation, which is the
// expression compute_equity_stats' own ordinary overload evaluates.
void compute_report_metrics(pf_metrics_t& result,
                            const TradeC* trades, int trades_len,
                            const pf_equity_point_t* equity_curve, int64_t equity_len,
                            double net_profit, double initial_capital,
                            const std::string& chart_timezone,
                            double first_bar_open, double last_close,
                            int64_t bars_in_market) {
    compute_report_metrics(result, trades, trades_len, equity_curve, equity_len,
                           net_profit, initial_capital, chart_timezone,
                           first_bar_open, last_close, bars_in_market, equity_len);
}

}  // namespace internal


bool NativeExecutionConsumer::observer_registered(const BacktestEngine& engine) noexcept {
    const auto* consumer = static_cast<const NativeExecutionConsumer*>(
        engine.execution_consumer_slot_.ptr.get());
    return consumer && consumer->selected_epoch_.observer_registered;
}

int BacktestEngine::set_equity_report_mode(ReportMode mode) {
    if (mode != ReportMode::FULL && mode != ReportMode::SUMMARY_ONLY
        && mode != ReportMode::STREAMED) return -1;
    if (report_sink_setter_status() != 0) return -3;
    if (mode == ReportMode::SUMMARY_ONLY && report_sink_state_.enabled) return -4;
    if (mode == ReportMode::SUMMARY_ONLY
        && NativeExecutionConsumer::observer_registered(*this))
        return PF_REPORT_MODE_REPLAY_UNAVAILABLE;
    pending_equity_report_mode_ = mode;
    return 0;
}

// Compute metrics after fill_trades_section. Sink-off owns its full arrays;
// a sink folds closed rows from the ledger plus its presented open-lot view
// and reads equity history from scratch. Summary uses only fixed folds.
// Metric history derives from recorded points, NOT script_bars_processed:
// an exception mid-run can truncate the curve (metrics then describe the
// truncated prefix; consumers must check strategy_get_last_error).
// No ScopedTimezone may be held here: compute_equity_stats takes the
// non-recursive global tz lock when chart_timezone_ is non-UTC.
void BacktestEngine::fill_metrics_section(ReportC* out) const {
    if (active_equity_report_mode_ == ReportMode::SUMMARY_ONLY || metrics_streaming_.no_curve) {
        // Only the independently held report point is presented. A whole-array
        // presenter requires an adapter at the versioned mode admission gate.
        auto presented_fold = summary_equity_fold_;
        pf_equity_point_t presented{};
        const auto* held = last_equity_point();
        if (held) presented = *held;
        out->equity_curve = held ? &presented : nullptr;
        out->equity_curve_len = held ? 1 : 0;
        try {
            present_report(out);
            if (out->equity_curve != (held ? &presented : nullptr)
                || out->equity_curve_len != (held ? 1 : 0))
                throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
                    {{"shape", "summary_equity_tail"}},
                    "summary equity presentation must preserve its held-point view");
            if (held)
                presented_fold.replace_last(presented,
                    report_equity_month_key(presented.time_ms));
            summary_equity_snapshot_ = presented_fold.first_pass();
            summary_equity_snapshot_valid_ = true;
            const auto* ratios = summary_equity_ratios_
                && summary_equity_ratios_->matches(summary_equity_snapshot_)
                ? &*summary_equity_ratios_ : nullptr;
            metrics::EquityRatioResult streaming_ratios;
            if (metrics_streaming_.no_curve) {
                finish_metrics_streaming_v1({});
                if (metrics_streaming_.receipt.route == metrics::MetricsRatioRouteV1::Streaming) {
                    metrics::MetricsMonthlyResultV1 monthly;
                    metrics::streaming_monthly_result_v1(*metrics_streaming_.fold, monthly);
                    streaming_ratios.expected = streaming_ratios.replayed = summary_equity_snapshot_;
                    streaming_ratios.complete = true;
                    streaming_ratios.sharpe_monthly = monthly.sharpe_monthly;
                    streaming_ratios.sortino_monthly = monthly.sortino_monthly;
                    streaming_ratios.sharpe_bar = metrics_streaming_.result.sharpe_bar;
                    streaming_ratios.sortino_bar = metrics_streaming_.result.sortino_bar;
                    streaming_ratios.nonfinite_field = metrics_streaming_.result.failure_field;
                    ratios = &streaming_ratios;
                }
            } else {
                // The old SUMMARY_ONLY second-pass contract remains intact.
                auto& receipt = metrics_streaming_.receipt;
                receipt.actual_extent = {};
                receipt.actual_extent.count = summary_equity_snapshot_.point_count;
                receipt.actual_extent.empty = summary_equity_snapshot_.point_count == 0;
                if (!receipt.actual_extent.empty) {
                    receipt.actual_extent.first_time_ms = summary_equity_snapshot_.first.time_ms;
                    receipt.actual_extent.last_time_ms = summary_equity_snapshot_.last.time_ms;
                }
                receipt.eligible_returns = summary_equity_snapshot_.bar_returns.count;
                receipt.route = metrics::MetricsRatioRouteV1::LegacyActualCurve;
                metrics_streaming_.receipt_final = true;
            }
            summary_equity_ratios_ready_ =
                !summary_equity_snapshot_.needs_second_pass() || ratios;
            // While the legacy trade producer is present, use its presented
            // rows exactly once. R13's omitted-row producer supplies these
            // three fixed blocks before equity assembly instead.
            if (out->trades || out->total_trades == 0) {
                out->metrics.all = metrics::compute_trade_stats(
                    out->trades, out->trades_len, metrics::TradeFilter::ALL, initial_capital_);
                out->metrics.longs = metrics::compute_trade_stats(
                    out->trades, out->trades_len, metrics::TradeFilter::LONG, initial_capital_);
                out->metrics.shorts = metrics::compute_trade_stats(
                    out->trades, out->trades_len, metrics::TradeFilter::SHORT, initial_capital_);
            } else {
                compute_report_sink_trade_metrics(out, nullptr, 0);
            }
            if (out->metrics.all.num_trades != out->total_trades)
                throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                    "summary trade metrics differ from the logical trade count");
            const metrics::EquityMetricInputs inputs{
                initial_capital_, first_bar_open_, current_bar_.close, out->net_profit};
            out->metrics = metrics::finalize_metrics(
                summary_equity_snapshot_, inputs, out->metrics.all,
                out->metrics.longs, out->metrics.shorts, ratios);
        } catch (...) {
            // Never let free_report delete the borrowed stack point.
            out->equity_curve = nullptr;
            out->equity_curve_len = 0;
            summary_equity_snapshot_valid_ = summary_equity_ratios_ready_ = false;
            throw;
        }
        // Omitted output is explicit in the envelope's capture flags and
        // logical count; this ABI-4 length describes only a resident array.
        out->equity_curve = nullptr;
        out->equity_curve_len = 0;
        return;
    }
    if (report_sink_state_.equity_scratch && !report_sink_state_.selected_equity_stream) {
        fill_report_sink_metrics_section(out);
        if (report_mode_ != ReportMode::FULL) {
            // The scratch curve owns the point sequence. The held one-point
            // array only carried the presented tail into the metric fold, so
            // summary/streamed capture returns no resident curve.
            delete[] out->equity_curve;
            out->equity_curve = nullptr;
            out->equity_curve_len = 0;
        }
        return;
    }
    const int64_t n = (int64_t)equity_curve_.size();
    if (report_mode_ != ReportMode::FULL) {
        // O1 R13: no owning copy of the recorded points. The metric folds walk
        // the recorded sequence in place, in the same order and grouping the
        // FULL route folds its copy. Presentation of an owning array belongs
        // to the FULL route; an exact summary replay is a separate provider
        // contract (docs/design/o1-report-contract.md), not an array copy.
        out->equity_curve = nullptr;
        out->equity_curve_len = 0;
        compute_report_metrics(out->metrics, nullptr, 0, equity_curve_.data(), n,
                               out->net_profit, initial_capital_, chart_timezone_,
                               first_bar_open_, current_bar_.close, bars_in_market_);
        compute_report_sink_trade_metrics(out, nullptr, 0);
        observe_metrics_curve_v1(equity_curve_, n, &out->metrics.equity);
        return;
    }
    out->equity_curve_len = n;
    if (n > 0) {
        out->equity_curve = new pf_equity_point_t[n];
        std::copy(equity_curve_.begin(), equity_curve_.end(), out->equity_curve);
    } else {
        out->equity_curve = nullptr;
    }
    present_report_equity_tail(out);
    compute_report_metrics(out->metrics, out->trades, out->trades_len,
                           out->equity_curve, n, out->net_profit,
                           initial_capital_, chart_timezone_,
                           first_bar_open_, current_bar_.close, bars_in_market_);
    if (report_sink_state_.enabled || report_sink_state_.receipt.complete)
        compute_report_sink_trade_metrics(out, out->trades, out->trades_len);
    observe_metrics_curve_v1(out->equity_curve, n, &out->metrics.equity);
}


// Heap-allocate and populate the per-evaluator security diagnostics array
// and the corresponding feed / complete-eval / partial-eval totals.
// Owns the allocation; freed by ``free_report``.
void BacktestEngine::fill_security_diag_section(ReportC* out) const {
    int sec_n = (int)security_eval_states_.size();
    out->security_diag_len = sec_n;
    out->security_feeds_total = 0;
    out->security_eval_complete_total = 0;
    out->security_eval_partial_total = 0;
    if (sec_n > 0) {
        out->security_diag = new SecurityDiagC[sec_n];
        for (int i = 0; i < sec_n; ++i) {
            const auto& s = security_eval_states_[(size_t)i];
            out->security_diag[i].sec_id = s.sec_id;
            out->security_diag[i].feed_count = s.feed_count;
            out->security_diag[i].eval_complete_count = s.eval_complete_count;
            out->security_diag[i].eval_partial_count = s.eval_partial_count;
            out->security_feeds_total += s.feed_count;
            out->security_eval_complete_total += s.eval_complete_count;
            out->security_eval_partial_total += s.eval_partial_count;
        }
    } else {
        out->security_diag = nullptr;
    }
}


// Heap-allocate flat copies of ``trace_buffer_`` and ``trace_names_`` so
// the C-side arrays are independent of the engine's internal vector
// capacity (Python reads them after run() returns but before
// strategy_free; the c_str() pointers in trace_names_ are stable for that
// window because no trace() calls run after fill_report). Owns both
// allocations; freed by ``free_report``.
void BacktestEngine::fill_trace_section(ReportC* out) const {
    // SummaryOnly keeps the ordered witness, not the sequence (R15). A sink
    // has already streamed every X record, so no resident array is owed.
    if (optional_report_mode_ == report_sequence::ReportMode::SummaryOnly) {
        out->trace = nullptr;
        out->trace_len = 0;
        out->trace_names = nullptr;
        out->trace_names_len = 0;
        return;
    }
    int trace_n = (int)trace_buffer_.size();
    out->trace_len = trace_n;
    if (trace_n > 0) {
        out->trace = new TraceEntryC[trace_n];
        for (int i = 0; i < trace_n; ++i) {
            out->trace[i] = trace_buffer_[(size_t)i];
        }
    } else {
        out->trace = nullptr;
    }

    int names_n = (int)trace_names_.size();
    out->trace_names_len = names_n;
    if (names_n > 0) {
        out->trace_names = new const char*[names_n];
        for (int i = 0; i < names_n; ++i) {
            out->trace_names[i] = trace_names_[(size_t)i].c_str();
        }
    } else {
        out->trace_names = nullptr;
    }
}


void BacktestEngine::free_report(ReportC* report) {
    if (report && report->trades) {
        delete[] report->trades;
        report->trades = nullptr;
        report->trades_len = 0;
    }
    if (report && report->security_diag) {
        delete[] report->security_diag;
        report->security_diag = nullptr;
        report->security_diag_len = 0;
    }
    if (report && report->trace) {
        delete[] report->trace;
        report->trace = nullptr;
        report->trace_len = 0;
    }
    if (report && report->trace_names) {
        // Only the pointer table is heap-allocated here; the C-strings
        // it points into are owned by BacktestEngine::trace_names_ and
        // freed when the strategy is destroyed.
        delete[] report->trace_names;
        report->trace_names = nullptr;
        report->trace_names_len = 0;
    }
    if (report && report->equity_curve) {
        // Allocation site (fill_metrics_section) must use
        // `new pf_equity_point_t[n]` to match this delete[].
        delete[] report->equity_curve;
        report->equity_curve = nullptr;
        report->equity_curve_len = 0;
    }
    if (report && report->broker_state_hash) {
        delete[] report->broker_state_hash;
        report->broker_state_hash = nullptr;
        report->broker_state_hash_len = 0;
    }
}

}  // namespace pineforge

extern "C" PF_API std::int32_t strategy_set_metrics_streaming_v1(
        void* strategy, std::uint32_t mode, std::uint64_t needed) noexcept {
    using namespace pineforge::metrics;
    if (!strategy) return static_cast<std::int32_t>(MetricsStreamingStatusV1::InvalidArgument);
    try {
        return static_cast<std::int32_t>(
            static_cast<pineforge::BacktestEngine*>(strategy)->set_metrics_streaming_v1(
                static_cast<StreamingSharpeV1>(mode), needed));
    } catch (...) {
        return static_cast<std::int32_t>(MetricsStreamingStatusV1::InvalidArgument);
    }
}

extern "C" PF_API std::int32_t strategy_metrics_extent_receipt_v1(
        const void* strategy, pineforge::metrics::MetricsExtentReceiptV1* out) noexcept {
    using namespace pineforge::metrics;
    if (!strategy) return static_cast<std::int32_t>(MetricsStreamingStatusV1::InvalidArgument);
    try {
        return static_cast<std::int32_t>(
            static_cast<const pineforge::BacktestEngine*>(strategy)->metrics_extent_receipt_v1(out));
    } catch (...) {
        return static_cast<std::int32_t>(MetricsStreamingStatusV1::InvalidArgument);
    }
}


// ---------------------------------------------------------------------------
// Recorded outputs (engine.hpp, docs/outputs.md). Kept after everything the
// file held before, so no line above moves. The consumer's private header is
// read for three facts only: whether the host is Running, the run's phase and
// the current point's completion. Every refusal carries its run-failure code
// (run_failure.hpp): a broken recorder contract is a host or generated-code
// defect, engine_invariant; a cap the run reached is outputs_limit; the
// two a caller causes (a switch it may not flip, a bar recalculated after it
// took that bar's events) are outputs_rejected.
#include "native_execution_consumer.hpp"

#include <pineforge/run_failure.hpp>

#include <climits>
#include <cstring>
#include <limits>
#include <string>

namespace pineforge {

namespace {

constexpr double kUnwrittenOutput = std::numeric_limits<double>::quiet_NaN();

[[noreturn]] void refuse_output(const std::string& text) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, "outputs: " + text);
}

[[noreturn]] void refuse_output_limit(const char* reason, const char* text) {
    throw coded<std::runtime_error>(RunFailureCode::outputs_limit,
                                    {{"reason", reason}, {"max", INT_MAX}}, text);
}

// A refused switch, recorded as the C boundary records a refused call: a
// handle whose last run failed keeps that run's own text and code.
void note_switch_refusal(BacktestEngine& engine, const char* text, const char* reason) {
    if (engine.last_run_status() != 0
        && run_failure_value_of(engine).code != RunFailureCode::none) {
        return;
    }
    note_run_failure(engine, text, RunFailureCode::outputs_rejected, {{"reason", reason}});
}

// FNV-1a 64 of the bytes, with the standard offset basis and prime.
uint64_t output_message_hash(const std::string& text) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Equal bit for bit, or both NaN.
bool same_output_constant(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    uint64_t x = 0;
    uint64_t y = 0;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
}

}  // namespace

bool BacktestEngine::output_host_running() const {
    return execution_consumer_slot_.ptr
        && as_native_consumer(*execution_consumer_slot_.ptr).state_kind()
               == NativeLifecycleKind::Running;
}

void BacktestEngine::clear_output_record() {
    reset_report_sink_outputs();
    output_series_.clear();
    output_open_ms_.clear();
    output_close_ms_.clear();
    output_events_.clear();
    output_sequence_ = 0;
    output_row_seq_base_ = 0;
    output_cleared_sequence_ = 0;
    sequence_witness_.rows = report_sequence::SequenceWitness{};
    sequence_witness_.events = report_sequence::SequenceWitness{};
    sequence_row_folded_ = false;
    output_row_witness_base_ = report_sequence::SequenceWitness{};
    output_event_row_witness_ = report_sequence::SequenceWitness{};
    std::fill(output_ordinals_.begin(), output_ordinals_.end(), 0u);
    std::fill(output_constants_.begin(), output_constants_.end(), kUnwrittenOutput);
    std::fill(output_constant_written_.begin(), output_constant_written_.end(), uint8_t{0});
}
bool BacktestEngine::set_outputs_enabled(bool on) {
    if (on && !outputs_declared_) {
        note_switch_refusal(*this, "outputs: this module declares no outputs", "not_declared");
        return false;
    }
    if (on != outputs_enabled_) {
        if (output_host_running()) {
            note_switch_refusal(*this, "outputs: recording cannot change during a run",
                                "run_in_progress");
            return false;
        }
        outputs_enabled_ = on;
        clear_output_record();
    }
    // Success leaves the failure record as it is, as strategy_set_trace_enabled
    // and the checked settings setters do: a failed run's text and code survive.
    return true;
}

void BacktestEngine::declare_outputs(int series_slots, int outputs, int run_constants) {
    if (series_slots < 0 || outputs < 0 || run_constants < 0)
        refuse_output("declared counts must not be negative");
    if (output_host_running()) refuse_output("declare_outputs while a run is in progress");
    if (outputs_declared_) {
        if (series_slots != outputs_slots_ || outputs != outputs_count_
            || run_constants != outputs_constants_) {
            refuse_output("declare_outputs repeated with other counts");
        }
        return;
    }
    outputs_slots_ = series_slots;
    outputs_count_ = outputs;
    outputs_constants_ = run_constants;
    output_ordinals_.assign(static_cast<size_t>(outputs), 0u);
    output_constants_.assign(static_cast<size_t>(run_constants), kUnwrittenOutput);
    output_constant_written_.assign(static_cast<size_t>(run_constants), uint8_t{0});
    outputs_declared_ = true;
}

void BacktestEngine::output_run_begin() {
    clear_output_record();
    output_run_begun_ = true;
}

void BacktestEngine::output_bar(int64_t open_ms, int64_t close_ms) {
    if (!outputs_enabled_) return;
    if (!output_run_begun_) refuse_output("output_bar before any output_run_begin");
    const size_t slots = static_cast<size_t>(outputs_slots_);
    if (output_open_ms_.empty() || open_ms > output_open_ms_.back()) {
        if (report_sink_output_row() + 1 >= INT_MAX)
            refuse_output_limit("too_many_rows", "outputs: the row count is at its limit");
        // Closing the previous row: fold it into the ordered witness and drop
        // it (summary), or stream and drop it (sink). R17 keeps one row.
        finalize_summary_output_row();
        retire_report_sink_output_row();
        output_series_.resize(output_series_.size() + slots, kUnwrittenOutput);
        output_open_ms_.push_back(open_ms);
        output_close_ms_.push_back(close_ms);
        std::fill(output_ordinals_.begin(), output_ordinals_.end(), 0u);
        output_row_seq_base_ = output_sequence_;
        output_row_witness_base_ = sequence_witness_.rows;
        output_event_row_witness_ = sequence_witness_.events;
        return;
    }
    if (open_ms < output_open_ms_.back()) {
        refuse_output("bar opened at " + std::to_string(open_ms)
                      + " is before the last recorded bar");
    }
    // A recalculation of the last row. Its queued events are retracted and
    // their sequences issued again, unless a clear already handed one out.
    if (output_cleared_sequence_ > output_row_seq_base_) {
        throw coded<std::runtime_error>(
            RunFailureCode::outputs_rejected, {{"reason", "recalculated_after_clear"}},
            "outputs: bar " + std::to_string(report_sink_output_row())
                + " was recalculated after its events were cleared");
    }
    // A recalculation retracts this row's provisional events: the summary
    // witness returns to its value when the row opened (O(1) rollback).
    if (optional_report_mode_ == report_sequence::ReportMode::SummaryOnly
        && !report_sink_state_.writer) {
        sequence_witness_.events = output_event_row_witness_;
        // A report read may have folded this still-open row (a live stream
        // snapshot). Its values are about to change, so retire that fold and
        // let the next finalization fold the recalculated row.
        if (sequence_row_folded_) {
            sequence_witness_.rows = output_row_witness_base_;
            sequence_row_folded_ = false;
        }
    }
    std::fill(output_series_.end() - static_cast<std::ptrdiff_t>(slots), output_series_.end(),
              kUnwrittenOutput);
    output_close_ms_.back() = close_ms;
    std::fill(output_ordinals_.begin(), output_ordinals_.end(), 0u);
    while (!output_events_.empty() && output_events_.back().sequence > output_row_seq_base_)
        output_events_.pop_back();
    output_sequence_ = output_row_seq_base_;
}

void BacktestEngine::output_value(int slot, double value) {
    if (!outputs_enabled_) return;
    if (output_open_ms_.empty()) refuse_output("output_value with no open bar");
    if (slot < 0 || slot >= outputs_slots_)
        refuse_output("series slot " + std::to_string(slot) + " is out of range");
    output_series_[(output_open_ms_.size() - 1) * static_cast<size_t>(outputs_slots_)
                   + static_cast<size_t>(slot)] = value;
}

void BacktestEngine::output_event(int output, double value) {
    record_output_event(output, value, nullptr);
}

void BacktestEngine::output_event(int output, double value, const std::string& message) {
    record_output_event(output, value, &message);
}

void BacktestEngine::record_output_event(int output, double value, const std::string* message) {
    if (!outputs_enabled_) return;
    if (output_open_ms_.empty()) refuse_output("output_event with no open bar");
    if (output < 0 || output >= outputs_count_)
        refuse_output("output " + std::to_string(output) + " is out of range");
    if (output_sequence_ == std::numeric_limits<uint64_t>::max())
        refuse_output("event sequence overflow");
    OutputEvent event;
    event.sequence = output_sequence_ + 1;
    event.output_index = output;
    event.bar_index = static_cast<int32_t>(report_sink_output_row());
    event.bar_open_ms = output_open_ms_.back();
    event.bar_close_ms = output_close_ms_.back();
    event.ordinal_in_bar = output_ordinals_[static_cast<size_t>(output)];
    const NativeExecutionConsumer& consumer = as_native_consumer(execution_consumer());
    event.phase = static_cast<uint32_t>(consumer.state_phase());
    const NativeCurrentPointView* point = consumer.current_point();
    event.confirmed = point != nullptr
            && point->decision.coordinate.completion == NativeCompletionKind::PartialFinalized
        ? 0u : 1u;
    event.value = value;
    if (message != nullptr) {
        event.has_message = true;
        event.message = *message;
        event.message_hash64 = output_message_hash(*message);
    }
    // SummaryOnly publishes no event array (R18): the record is folded into
    // the ordered witness at once, and a same-row recalculation restores the
    // witness snapshot, so the fixed state does not grow with events.
    if (optional_report_mode_ == report_sequence::ReportMode::SummaryOnly
        && !report_sink_state_.writer) {
        fold_event_leaf(sequence_witness_.events, event);
        output_sequence_ = event.sequence;
        ++output_ordinals_[static_cast<size_t>(output)];
        return;
    }
    if (output_events_.size() >= static_cast<size_t>(INT_MAX))
        refuse_output_limit("too_many_events", "outputs: the event queue is full");
    output_events_.push_back(std::move(event));
    output_sequence_ = output_events_.back().sequence;
    ++output_ordinals_[static_cast<size_t>(output)];
}

void BacktestEngine::output_constant(int index, double value) {
    if (!outputs_enabled_) return;
    if (!output_run_begun_) refuse_output("output_constant before any output_run_begin");
    if (index < 0 || index >= outputs_constants_)
        refuse_output("run constant " + std::to_string(index) + " is out of range");
    const size_t at = static_cast<size_t>(index);
    if (!output_constant_written_[at]) {
        output_constants_[at] = value;
        output_constant_written_[at] = 1;
        return;
    }
    if (!same_output_constant(output_constants_[at], value))
        refuse_output("run constant " + std::to_string(index) + " changed within a run");
}

bool BacktestEngine::output_bar_times_copy(int64_t from_bar, int64_t* open_ms,
                                           int64_t* close_ms, int64_t capacity,
                                           int64_t* written) const {
    const int64_t rows = output_bars_len();
    if (written == nullptr || capacity < 0 || from_bar < 0 || from_bar > rows) return false;
    const int64_t n = std::min(capacity, rows - from_bar);
    const auto first = static_cast<std::ptrdiff_t>(from_bar);
    if (open_ms != nullptr)
        std::copy(output_open_ms_.begin() + first, output_open_ms_.begin() + first + n, open_ms);
    if (close_ms != nullptr)
        std::copy(output_close_ms_.begin() + first, output_close_ms_.begin() + first + n, close_ms);
    *written = n;
    return true;
}

bool BacktestEngine::output_series_copy(int slot, int64_t from_bar, double* out,
                                        int64_t capacity, int64_t* written) const {
    const int64_t rows = output_bars_len();
    if (written == nullptr || slot < 0 || slot >= output_series_count() || capacity < 0
        || (capacity > 0 && out == nullptr) || from_bar < 0 || from_bar > rows) {
        return false;
    }
    const int64_t n = std::min(capacity, rows - from_bar);
    const size_t stride = static_cast<size_t>(outputs_slots_);
    for (int64_t i = 0; i < n; ++i)
        out[i] = output_series_[static_cast<size_t>(from_bar + i) * stride + static_cast<size_t>(slot)];
    *written = n;
    return true;
}

const BacktestEngine::OutputEvent* BacktestEngine::output_event_at(int index) const {
    if (index < 0 || index >= output_events_len()) return nullptr;
    return &output_events_[static_cast<size_t>(index)];
}

void BacktestEngine::output_events_clear() {
    if (output_events_.empty()) {
        // SummaryOnly has no queue: the folded witness already holds the
        // cleared events, exactly as the sink published them before clearing.
        if (optional_report_mode_ == report_sequence::ReportMode::SummaryOnly
            && !report_sink_state_.writer) {
            output_cleared_sequence_ = std::max(output_cleared_sequence_, output_sequence_);
        }
        return;
    }
    write_report_sink_events();
    output_cleared_sequence_ = std::max(output_cleared_sequence_, output_events_.back().sequence);
    output_events_.clear();
}
double BacktestEngine::output_constant_value(int index) const {
    if (index < 0 || index >= output_constants_count()) return kUnwrittenOutput;
    const size_t at = static_cast<size_t>(index);
    return output_constant_written_[at] ? output_constants_[at] : kUnwrittenOutput;
}

}  // namespace pineforge

// Sink runtime. Kept after the existing report/output code
// so the line-pinned documentation above retains its source coordinates.
#include <pineforge/detail/scratch_arena.hpp>
#include "report_sink_writer.hpp"

namespace pineforge {

namespace {

struct BorrowedFdIdentity {
    bool valid = false;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
};

BorrowedFdIdentity capture_fd_identity(int fd) noexcept {
    struct stat info{};
    int result;
    do {
        result = ::fstat(fd, &info);
    } while (result < 0 && errno == EINTR);
    if (result != 0) return {};
    return {true, static_cast<std::uint64_t>(info.st_dev),
                  static_cast<std::uint64_t>(info.st_ino)};
}

bool fd_identity_matches(int fd, const BorrowedFdIdentity& expected,
                         int& error_number) noexcept {
    if (!expected.valid) {
        error_number = EBADF;
        return false;
    }
    const auto actual = capture_fd_identity(fd);
    if (!actual.valid) {
        error_number = errno == 0 ? EBADF : errno;
        return false;
    }
    if (actual.device != expected.device || actual.inode != expected.inode) {
        error_number = EINVAL;
        return false;
    }
    return true;
}

}  // namespace

BacktestEngine::ReportSinkState::ReportSinkState() = default;
BacktestEngine::ReportSinkState::~ReportSinkState() = default;
BacktestEngine::ReportSinkState::ReportSinkState(const ReportSinkState& other)
    : enabled(other.enabled), trade_history_streamable(other.trade_history_streamable),
      path(other.path), run_id(other.run_id), scratch_dir(other.scratch_dir),
      fd(other.fd), fd_device(other.fd_device), fd_inode(other.fd_inode),
      fd_identity_valid(other.fd_identity_valid) {}
BacktestEngine::ReportSinkState&
BacktestEngine::ReportSinkState::operator=(const ReportSinkState& other) {
    if (this != &other) *this = ReportSinkState(other);
    return *this;
}
BacktestEngine::ReportSinkState::ReportSinkState(ReportSinkState&&) noexcept = default;
BacktestEngine::ReportSinkState&
BacktestEngine::ReportSinkState::operator=(ReportSinkState&&) noexcept = default;

int BacktestEngine::report_sink_setter_status() const {
    const auto& sink = report_sink_state_;
    const auto* consumer = execution_consumer_slot_.ptr
        ? &as_native_consumer(*execution_consumer_slot_.ptr) : nullptr;
    if (sink.stream_calls != 0 || stream_phase_ == StreamPhase::REALTIME
        || (consumer && consumer->running() && consumer->state_phase() != NativeRunPhase::Batch))
        return -2;
    if (sink.active_calls != 0 || (consumer && consumer->running())) return -3;
    return 0;
}

int BacktestEngine::set_report_sink_v1(const pf_report_sink_v1* descriptor) {
    if (descriptor && (descriptor->struct_size != sizeof(*descriptor)
        || descriptor->version != 1 || !descriptor->path || !*descriptor->path)) return -1;
    const int status = report_sink_setter_status();
    if (status != 0) return status;
    if (!descriptor && report_sink_state_.fd >= 0 && report_sink_state_.receipt.complete
        && last_run_status_ == 0 && last_error_.empty()) {
        // A completed fd generation remains readable until the next begin.
        // Detach only its next-run configuration; keep the writer, receipt,
        // summary and scratch owner without closing or rehydrating anything.
        report_sink_state_.enabled = false;
        return 0;
    }
    // A scratch-backed generation cannot become an array report on removal.
    // Keep its sink and metric source intact, including an observer capture.
    // A nonzero ledger base likewise requires the streamed report: its sealed
    // rows are gone from RAM. A new begin clears size/base to zero, lifting
    // this guard; the scratch-owner guard still applies to ordinary runs.
    if (!descriptor && (report_sink_state_.equity_scratch || trades_.base() > 0)) return -3;
    // Construct the full replacement first. Allocation failure cannot erase
    // an installed sink, its receipt or its copied strings.
    ReportSinkState next;
    next.trade_history_streamable = report_sink_state_.trade_history_streamable;
    if (descriptor) {
        next.enabled = true;
        next.path = descriptor->path;
        next.run_id = descriptor->run_id ? descriptor->run_id : "";
        if (descriptor->scratch_dir) next.scratch_dir = descriptor->scratch_dir;
        else {
            const auto slash = next.path.find_last_of('/');
            next.scratch_dir = slash == std::string::npos ? "."
                : slash == 0 ? "/" : next.path.substr(0, slash);
        }
        detail::ScratchArena::attach_from_environment();
    }
    if (report_sink_state_.equity_scratch) {
        if (next.enabled) {
            // Replacing next-run configuration must not discard the current
            // generation's record-time curve and validation latches.
            next.equity_scratch = std::move(report_sink_state_.equity_scratch);
            next.equity_tail_stored = report_sink_state_.equity_tail_stored;
            next.selected_equity_stream = std::move(report_sink_state_.selected_equity_stream);
        }
    }
    report_sink_state_ = std::move(next);
    return 0;
}

int BacktestEngine::set_report_sink_fd_v1(const pf_report_sink_fd_v1* descriptor) {
    if (descriptor && (descriptor->struct_size != sizeof(*descriptor)
        || descriptor->version != 1 || descriptor->fd < 0)) return -1;
    const int status = report_sink_setter_status();
    if (status != 0) return status;
    if (!descriptor && report_sink_state_.fd >= 0 && report_sink_state_.receipt.complete
        && last_run_status_ == 0 && last_error_.empty())
        return set_report_sink_v1(nullptr);
    // NULL has the same generation-preserving guard through either setter.
    if (!descriptor && (report_sink_state_.equity_scratch || trades_.base() > 0)) return -3;
    ReportSinkState next;
    next.trade_history_streamable = report_sink_state_.trade_history_streamable;
    if (descriptor) {
        next.enabled = true;
        next.fd = descriptor->fd;
        next.run_id = descriptor->run_id ? descriptor->run_id : "";
        const auto identity = capture_fd_identity(next.fd);
        next.fd_device = identity.device;
        next.fd_inode = identity.inode;
        next.fd_identity_valid = identity.valid;
        // No path parent or scratch directory exists for a borrowed stream.
        detail::ScratchArena::attach_from_environment();
    }
    if (report_sink_state_.equity_scratch) {
        // Replacing next-run configuration must not discard the current
        // generation's record-time curve and selected validation latches.
        next.equity_scratch = std::move(report_sink_state_.equity_scratch);
        next.equity_tail_stored = report_sink_state_.equity_tail_stored;
        next.selected_equity_stream = std::move(report_sink_state_.selected_equity_stream);
    }
    report_sink_state_ = std::move(next);
    return 0;
}

int BacktestEngine::report_sink_receipt_v1(pf_report_sink_receipt_v1* out) const noexcept {
    if (!out || out->struct_size != sizeof(*out) || out->version != 1) return -1;
    pf_report_sink_receipt_v1 local{};
    // Share fill_report's attempt gate: a refusal before begin leaves the
    // preceding generation's storage intact, but cannot publish its receipt.
    if (NativeExecutionConsumer::rows_are_current(*this)
        && (report_sink_state_.enabled || report_sink_state_.receipt.complete))
        local = report_sink_state_.receipt;
    local.struct_size = sizeof local;
    local.version = 1;
    *out = local;
    return 0;
}

int BacktestEngine::report_sink_summary_v1(pf_report_sink_summary_v1* out) const noexcept {
    if (!out || out->struct_size != sizeof(*out) || out->version != 1) return -1;
    pf_report_sink_summary_v1 local{};
    if (NativeExecutionConsumer::rows_are_current(*this)
        && (report_sink_state_.enabled || report_sink_state_.receipt.complete)) {
        local = report_sink_state_.summary;
        local.complete = report_sink_state_.receipt.complete;
    }
    local.struct_size = sizeof local;
    local.version = 1;
    *out = local;
    return 0;
}

int BacktestEngine::report_sink_diagnostics_v1(
        pf_report_sink_diagnostics_v1* out) const noexcept {
    if (!out || out->struct_size != sizeof(*out) || out->version != 1) return -1;
    pf_report_sink_diagnostics_v1 local{};
    if (NativeExecutionConsumer::rows_are_current(*this)
        && report_sink_state_.receipt.complete && report_sink_state_.writer)
        local = report_sink_state_.writer->diagnostics();
    local.struct_size = sizeof local;
    local.version = 1;
    *out = local;
    return 0;
}
void BacktestEngine::set_trade_history_streamable(bool streamable) {
    if (report_sink_setter_status() != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "trade history policy cannot change during a run");
    // Refuse before changing policy: ordinary reports need the sealed rows
    // that are gone from RAM. A new begin clears size/base to zero and allows
    // the policy to change again, without reading those rows back.
    if (!streamable && trades_.base() > 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "trade history policy cannot retain sealed trade rows");
    report_sink_state_.trade_history_streamable = streamable;
}
void BacktestEngine::begin_report_sink() {
    auto& sink = report_sink_state_;
    // A begin owns a fresh run: the optional-sequence witnesses start empty
    // beside the arrays the run resets.
    reset_optional_sequence_witness();
    sink.writer.reset();
    sink.equity_scratch.reset();
    sink.equity_tail_stored = false;
    sink.selected_equity_stream.reset();
#ifndef NDEBUG
    sink.selected_released_extremes[0] = initial_capital_;
    sink.selected_released_extremes[1] = initial_capital_;
    sink.selected_released_extremes[2] = 0.0;
    sink.selected_released_extremes[3] = 0.0;
#endif
    sink.receipt = {};
    sink.summary = {};
    sink.output_base = 0;
    // Run preparation, before callbacks/bar zero, also when no sink is set.
    active_equity_report_mode_ = pending_equity_report_mode_;
    summary_equity_fold_ = {};
    summary_equity_snapshot_ = {};
    summary_equity_snapshot_valid_ = summary_equity_ratios_ready_ = false;
    summary_equity_ratios_.reset();
    summary_broker_equity_prefix_[0] = summary_broker_equity_prefix_[1] = initial_capital_;
    summary_broker_equity_prefix_[2] = summary_broker_equity_prefix_[3] = 0.0;
    if (active_equity_report_mode_ == ReportMode::SUMMARY_ONLY || metrics_streaming_.no_curve) {
        if (sink.enabled)
            throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
                {{"shape", "summary_with_sequence_sink"}},
                "summary equity mode conflicts with a sequence sink");
        // SUMMARY_ONLY and a proved streaming attempt share this one held point.
        // Drop a prior FULL run's capacity before any callback can mark.
        std::vector<pf_equity_point_t> held;
        held.reserve(1);
        equity_curve_.swap(held);
        return; // No curve writer, region, file, or sink publication.
    }
    if (!sink.enabled) return;
    trades_.set_history_retirement(sink.trade_history_streamable
        && !detail::strategy_reads_history_by_index(*this));
    // The new run owns these histories. Drop capacity left by a previous
    // sink-off run before callbacks, even if outputs are disabled this time.
    std::vector<uint64_t>().swap(broker_state_hashes_);
    std::vector<TraceEntryC>().swap(trace_buffer_);
    std::vector<double>().swap(output_series_);
    std::vector<int64_t>().swap(output_open_ms_);
    std::vector<int64_t>().swap(output_close_ms_);
    std::vector<OutputEvent>().swap(output_events_);
    sink.receipt.trades_retained = sink.trade_history_streamable ? 0u : 1u;
    try {
        if (sink.fd >= 0) {
            const BorrowedFdIdentity expected{sink.fd_identity_valid,
                sink.fd_device, sink.fd_inode};
            int error_number = EBADF;
            if (!fd_identity_matches(sink.fd, expected, error_number))
                throw report_sink::WriterError(
                    report_sink::WriterFailure::io,
                    "borrowed report sink descriptor identity changed", error_number);
            const pf_report_sink_fd_v1 descriptor{sizeof(pf_report_sink_fd_v1), 1,
                sink.fd, sink.run_id.c_str()};
            sink.writer = std::make_unique<report_sink::LineWriter>(descriptor);
        } else {
            const pf_report_sink_v1 descriptor{sizeof(pf_report_sink_v1), 1,
                sink.path.c_str(), sink.run_id.c_str(), sink.scratch_dir.c_str()};
            sink.writer = std::make_unique<report_sink::LineWriter>(descriptor);
        }
        sink.writer->write_header({outputs_enabled_, trace_enabled_, broker_state_hash_recording_,
                                   outputs_slots_, outputs_constants_});
        pf_selected_window_config_v1 window{};
        const bool selected = NativeExecutionConsumer::selected_window_admitted(*this, &window);
        const auto* consumer = execution_consumer_slot_.ptr
            ? &as_native_consumer(*execution_consumer_slot_.ptr) : nullptr;
        const auto* spec = consumer ? consumer->view().spec : nullptr;
        if (!selected || (spec && spec->report_policy != NativeReportPolicy::HostRecorded)) {
            // The kernel recorder owns only its editable tail in RAM, including
            // when this handle previously retained a full sink-off curve.
            std::vector<pf_equity_point_t>().swap(equity_curve_);
            if (selected) equity_curve_.reserve(kReportSinkHeldEquityPoints);
            sink.equity_scratch =
                std::make_unique<report_sink::ScratchCurveWriter>(sink.scratch_dir);
            if (selected) {
                sink.selected_equity_stream = std::make_unique<report::SelectedReportStream>(
                    window.start_ms, window.end_ms, initial_capital_);
                sink.equity_scratch->append({window.start_ms, initial_capital_, 0.0});
            }
        }
    } catch (const report_sink::WriterError& error) {
        sink.writer.reset();
        throw coded<std::runtime_error>(error.run_failure(), error.what());
    }
}

pf_report_sink_summary_v1 NativeExecutionConsumer::finish_report_sink_summary(
        const BacktestEngine& engine) const {
    if (engine.trades_.base() != closed_trade_prefix_.base
        || (engine.trades_.base() != 0 && !closed_trade_report_))
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report summary has no matching sealed prefix");
    // Resume a copy of the original left fold: sealed closed rows, resident
    // closed rows, then one rebuilt terminal row per lot. Never retain rows
    // or change the sealed prefix when a report is read again.
    auto summary = closed_trade_report_ ? closed_trade_report_->summary : ReportTradeSummary{};
    if (summary.value.count_t != static_cast<std::int64_t>(engine.trades_.base()))
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report summary count differs from the sealed trade base");
    for (std::size_t index = engine.trades_.base();
         index < engine.trades_.size(); ++index)
        summary.fold(report_trade_row(engine.trades_.numeric_at(index)));
    for (const auto& row : engine.range_end_trades_) summary.fold(report_trade_row(row));
    return summary.value;
}

void BacktestEngine::finish_report_sink(ReportC* out) const {
    auto& sink = report_sink_state_;
    if (!sink.enabled && !sink.receipt.complete) return;
    try {
        // Partial reports still answer as summaries, but never finish a sink.
        // The row/capture gates in fill_report already reject another attempt's
        // rows and a failed selected/observed generation's presentation.
        const auto* consumer = execution_consumer_slot_.ptr
            ? &as_native_consumer(*execution_consumer_slot_.ptr) : nullptr;
        if (sink.writer && !sink.receipt.complete && last_run_status_ == 0
            && last_error_.empty() && consumer
            && consumer->state_kind() == NativeLifecycleKind::Completed) {
            if (sink.equity_scratch) {
                // Metrics/capture have already staged the presented copy.
                // region() flushes scratch before a trailer can claim success.
                const auto region = sink.equity_scratch->region();
                for (int64_t i = sink.receipt.count_e; i < region.point_count; ++i) {
                    pf_equity_point_t point{};
                    report_sink::read_scratch_points(region, i, &point, 1);
                    sink.writer->write_e(point);
                    ++sink.receipt.count_e;
                }
            } else {
                for (int64_t i = sink.receipt.count_e; i < out->equity_curve_len; ++i) {
                    sink.writer->write_e(out->equity_curve[i]);
                    ++sink.receipt.count_e;
                }
            }
            const TradeC* open_rows = out->trades;
            int open_count = out->trades_len;
            NativeExecutionConsumer::report_sink_open_trades(*this, open_rows, open_count);
            auto summary = report_mode_ != ReportMode::FULL && open_rows == nullptr && open_count == 0
                ? consumer->finish_report_sink_summary(*this)
                : consumer->finish_report_sink_summary(*this, open_rows, open_count);
            if (summary.count_t != out->total_trades)
                throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                    "report summary count differs from the report trade count");
            write_report_sink_trades(open_rows, open_count);
            if (outputs_enabled_) {
                write_report_sink_output_row();
                sink.writer->write_k(output_constants_.data(), outputs_constants_);
                ++sink.receipt.count_k;
            }
            write_report_sink_hashes();
            if (summary.count_t != sink.receipt.count_t)
                throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                    "report summary count differs from the sink trade count");
            // The last input bar is already complete. A SIGTERM received
            // during report assembly still abandons a whole-record prefix,
            // before any success trailer or complete receipt is published.
            // Keep security diagnostics and scalars on abort; the common
            // cleanup below releases only the sink's sequence arrays.
            if (!NativeExecutionConsumer::abort_before_sink_trailer(
                    const_cast<BacktestEngine&>(*this))) {
                const auto trailer = sink.writer->write_trailer();
                sink.receipt.count_e = trailer.count_e;
                sink.receipt.count_t = trailer.count_t;
                sink.receipt.count_o = trailer.count_o;
                sink.receipt.count_v = trailer.count_v;
                sink.receipt.count_k = trailer.count_k;
                sink.receipt.count_x = trailer.count_x;
                sink.receipt.count_h = trailer.count_h;
                std::memcpy(sink.receipt.sha256_hex, trailer.sha256_hex, sizeof trailer.sha256_hex);
                summary.struct_size = sizeof summary;
                summary.version = 1;
                summary.complete = 1;
                sink.summary = summary;
                sink.receipt.complete = 1;
            }
        }
    } catch (const report_sink::WriterError& error) {
        sink.writer.reset();
        free_report(out);
        *out = {};
        auto& engine = const_cast<BacktestEngine&>(*this);
        engine.last_run_status_ = 1;
        const auto failure = coded<std::runtime_error>(error.run_failure(), error.what());
        note_run_failure(engine, failure.what(), classify_run_failure(failure));
        throw failure;
    } catch (const std::exception& error) {
        sink.writer.reset();
        free_report(out);
        *out = {};
        auto& engine = const_cast<BacktestEngine&>(*this);
        engine.last_run_status_ = 1;
        note_run_failure(engine, error.what(), classify_run_failure(error));
        throw;
    } catch (...) {
        sink.writer.reset();
        free_report(out);
        *out = {};
        const_cast<BacktestEngine&>(*this).last_run_status_ = 1;
        throw;
    }
    delete[] out->trades;
    delete[] out->equity_curve;
    delete[] out->broker_state_hash;
    delete[] out->trace;
    delete[] out->trace_names;
    out->trades = nullptr; out->trades_len = 0;
    out->equity_curve = nullptr; out->equity_curve_len = 0;
    out->broker_state_hash = nullptr; out->broker_state_hash_len = 0;
    out->trace = nullptr; out->trace_len = 0;
    out->trace_names = nullptr; out->trace_names_len = 0;
}

void BacktestEngine::write_report_sink_trades(const TradeC* open_rows, int count) const {
    auto& sink = report_sink_state_;
    const auto closed_count = static_cast<int64_t>(trades_.size());
    // A captured presentation supplies its own open rows; the summary and
    // streamed routes rebuild them from the terminal mark, one at a time.
    // Nothing here keeps a TradeC array for the range-end rows.
    const bool lazy_open_rows = report_mode_ != ReportMode::FULL && open_rows == nullptr;
    const int64_t open_count = lazy_open_rows
        ? static_cast<int64_t>(range_end_trades_.size())
        : static_cast<int64_t>(count);
    const int64_t total = closed_count + open_count;
    if (sink.receipt.count_t < static_cast<int64_t>(trades_.base())
        || sink.receipt.count_t > total)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report trade sink cursor is outside the resident window");
    for (int64_t i = sink.receipt.count_t; i < total; ++i) {
        const int64_t open_index = i - closed_count;
        // Value copies, never a pointer into the ledger: the row source owns
        // whatever representation its lane chooses, and a by-value accessor
        // would leave a pointer to a temporary here.
        Trade closed;
        Trade rebuilt;
        const Trade* identity = nullptr;
        TradeC row{};
        if (i < closed_count) {
            closed = trades_[static_cast<std::size_t>(i)];
            identity = &closed;
            row = report_trade_row(closed);
        } else if (lazy_open_rows) {
            rebuilt = range_end_trades_.row(static_cast<std::size_t>(open_index));
            identity = &rebuilt;
            row = report_trade_row(rebuilt);
        } else {
            // FULL: the presented row's values, with a detached identity from
            // the same terminal mark. Sink emission retains no legacy row.
            rebuilt = range_end_trades_.row(static_cast<std::size_t>(open_index));
            identity = &rebuilt;
            row = open_rows[static_cast<std::size_t>(open_index)];
        }
        pf_trade_t wire{};
        wire.entry_time = row.entry_time;
        wire.exit_time = row.exit_time;
        wire.entry_price = row.entry_price;
        wire.exit_price = row.exit_price;
        wire.pnl = row.pnl;
        wire.pnl_pct = row.pnl_pct;
        wire.is_long = row.is_long;
        wire.max_runup = row.max_runup;
        wire.max_drawdown = row.max_drawdown;
        wire.qty = row.qty;
        wire.commission = row.commission;
        wire.entry_bar_index = row.entry_bar_index;
        wire.exit_bar_index = row.exit_bar_index;
        wire.open_at_end = row.open_at_end;
        sink.writer->write_t(i + INT64_C(1), wire, identity->entry_incarnation,
            classify_report_close_cause(*identity), identity->entry_id,
            identity->exit_id, identity->exit_comment);
        ++sink.receipt.count_t;
    }
}

void BacktestEngine::stage_report_sink_equity_point(const pf_equity_point_t& point) const {
    auto& sink = report_sink_state_;
    if (sink.equity_tail_stored) {
        sink.equity_scratch->replace_last(point);
    } else {
        sink.equity_scratch->append(point);
        sink.equity_tail_stored = true;
    }
}

void BacktestEngine::stage_report_sink_equity_tail(const ReportC* out) const {
    auto& sink = report_sink_state_;
    if (!sink.equity_scratch || !last_equity_point()) return;
    try {
        if (!out->equity_curve || out->equity_curve_len != 1)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "ordinary sink presentation lost its held equity point");
        stage_report_sink_equity_point(out->equity_curve[0]);
        sink.equity_scratch->flush();
    } catch (const std::exception& error) {
        sink.writer.reset();
        auto& engine = const_cast<BacktestEngine&>(*this);
        engine.last_run_status_ = 1;
        note_run_failure(engine, error.what(), classify_run_failure(error));
        throw;
    }
}

void BacktestEngine::compute_report_sink_metrics(ReportC* out,
                                                std::int64_t equity_count) const {
    try {
        const auto region = report_sink_state_.equity_scratch->region();
        if (region.point_count != equity_count)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "ordinary sink metric curve differs from its captured point count");
        compute_report_sink_trade_metrics(out, out->trades, out->trades_len);
        const report_sink::ScratchCurveWindow curve(region);
        out->metrics.equity = metrics::compute_equity_stats(
            curve, equity_count, initial_capital_, chart_timezone_, first_bar_open_,
            current_bar_.close, bars_in_market_, out->net_profit);
        observe_metrics_curve_v1(curve, equity_count, &out->metrics.equity);
    } catch (const std::exception& error) {
        report_sink_state_.writer.reset();
        auto& engine = const_cast<BacktestEngine&>(*this);
        engine.last_run_status_ = 1;
        note_run_failure(engine, error.what(), classify_run_failure(error));
        throw;
    }
}

void BacktestEngine::fill_report_sink_metrics_section(ReportC* out) const {
    try {
        const auto* held = last_equity_point();
        out->equity_curve_len = held ? 1 : 0;
        out->equity_curve = held ? new pf_equity_point_t[1]{*held} : nullptr;
        present_report_equity_tail(out);
        stage_report_sink_equity_tail(out);
        compute_report_sink_metrics(out, report_sink_state_.equity_scratch->size());
    } catch (...) {
        free_report(out);
        *out = {};
        throw;
    }
}

// Callback-time writes use the same failure codes as begin/fill. The writer
// poisons itself on any failed record, so no later read can add a trailer.
template<class Write>
void BacktestEngine::report_sink_record(Write write) const {
    try {
        write();
    } catch (const report_sink::WriterError& error) {
        const auto failure = coded<std::runtime_error>(error.run_failure(), error.what());
        auto& engine = const_cast<BacktestEngine&>(*this);
        engine.last_run_status_ = 1;
        note_run_failure(engine, failure.what(), classify_run_failure(failure));
        throw failure;
    }
}

bool BacktestEngine::stream_report_sink_trace(const std::string& name,
                                              const TraceEntryC& entry) {
    auto& sink = report_sink_state_;
    if (!sink.writer || sink.receipt.complete) return false;
    // The X record carries the name text itself; name_id is report-array
    // provenance and this streaming path keeps no resident name table (R16).
    // The caller's name view outlives the write.
    const pf_trace_entry_t wire{entry.timestamp, entry.bar_index, 0, entry.value};
    report_sink_record([&] {
        sink.writer->write_x(wire, name);
        ++sink.receipt.count_x;
    });
    return true;
}

void BacktestEngine::reset_report_sink_outputs() {
    auto& sink = report_sink_state_;
    if (sink.writer && !sink.receipt.complete && output_host_running()
        && (sink.receipt.count_o != 0 || sink.receipt.count_v != 0))
        refuse_output("cannot restart output recording after rows were written to the sink");
    sink.output_base = 0;
}

std::int64_t BacktestEngine::report_sink_output_row() const {
    return report_sink_state_.output_base + static_cast<std::int64_t>(output_open_ms_.size()) - 1;
}

void BacktestEngine::write_report_sink_events() const {
    auto& sink = report_sink_state_;
    if (!sink.writer || sink.receipt.complete) return;
    report_sink_record([&] {
        for (const auto& event : output_events_) {
            pf_output_event_v1_t wire{};
            wire.struct_version = 1;
            wire.size = sizeof wire;
            wire.sequence = event.sequence;
            wire.output_index = event.output_index;
            wire.bar_index = event.bar_index;
            wire.bar_open_ms = event.bar_open_ms;
            wire.bar_close_ms = event.bar_close_ms;
            wire.ordinal_in_bar = event.ordinal_in_bar;
            wire.phase = event.phase;
            wire.confirmed = event.confirmed;
            wire.value = event.value;
            wire.message_hash64 = event.message_hash64;
            wire.message = event.has_message ? event.message.c_str() : nullptr;
            sink.writer->write_v(wire, event.message);
            ++sink.receipt.count_v;
            fold_event_leaf(sequence_witness_.events, event);
        }
    });
}

void BacktestEngine::write_report_sink_output_row() const {
    auto& sink = report_sink_state_;
    if (!sink.writer || sink.receipt.complete || output_open_ms_.empty()) return;
    report_sink_record([&] {
        const double* values = output_series_.empty() ? nullptr : output_series_.data();
        sink.writer->write_o(sink.output_base, output_open_ms_.back(), output_close_ms_.back(),
                             values, outputs_slots_);
        ++sink.receipt.count_o;
        fold_row_leaf(sequence_witness_.rows, sink.output_base, output_open_ms_.back(),
                      output_close_ms_.back(), values, outputs_slots_);
        write_report_sink_events();
    });
}

void BacktestEngine::retire_report_sink_output_row() {
    auto& sink = report_sink_state_;
    if (!sink.writer || sink.receipt.complete || output_open_ms_.empty()) return;
    // Only a newer open retires the row. Equal opens still run the existing
    // value/event rollback and reissue that row's original event sequences.
    write_report_sink_output_row();
    ++sink.output_base;
    output_series_.clear();
    output_open_ms_.clear();
    output_close_ms_.clear();
    output_events_.clear();
}

void BacktestEngine::write_report_sink_hashes() const {
    auto& sink = report_sink_state_;
    if (!sink.writer || sink.receipt.complete) return;
    // The witness covers exactly the leaves this call writes, in order, and a
    // repeated call after fill_report's fold adds nothing twice.
    fold_pending_hashes_into_witness();
    report_sink_record([&] {
        for (const auto hash : broker_state_hashes_) {
            sink.writer->write_h(hash);
            ++sink.receipt.count_h;
        }
    });
}

void BacktestEngine::retire_report_sink_hashes() {
    auto& sink = report_sink_state_;
    if (sink.writer && !sink.receipt.complete) {
        // The previous point is now final, including any Pine terminal
        // rewrite. Its successor is appended by the existing native or
        // marking-host path.
        write_report_sink_hashes();
        broker_state_hashes_.clear();
        sequence_hash_folded_ = 0;
        sequence_hash_folded_leaf_ = 0;
        return;
    }
    if (!sink.writer && optional_report_mode_ == report_sequence::ReportMode::SummaryOnly) {
        // No stream requested: fold the finalized leaf into the ordered
        // witness and retire it, so the recorded vector stays one leaf deep.
        fold_pending_hashes_into_witness();
        broker_state_hashes_.clear();
        sequence_hash_folded_ = 0;
        sequence_hash_folded_leaf_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Fixed ordered witnesses.
//
// Every fold below is over the record's semantic fields, in record order, on
// an explicit little-endian byte spelling. Nothing here reads the report mode
// or an array, so the same run folds the same digest whether the sequence was
// streamed to a sink or consumed as a summary. The witnesses are the only
// state the summary mode adds; each is O(1).
namespace {

// FNV-1a's prime over an explicit little-endian spelling. The digest starts at
// zero because a witness is an opaque ordered equality check, not a hash of
// independent bytes.
constexpr std::uint64_t kSequenceDigestPrime = 1099511628211ULL;

void sequence_fold_byte(std::uint64_t& digest, std::uint8_t byte) {
    digest ^= byte;
    digest *= kSequenceDigestPrime;
}

void sequence_fold_u64(std::uint64_t& digest, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        sequence_fold_byte(digest, static_cast<std::uint8_t>(value >> shift));
}

std::uint64_t sequence_unfold_u64(std::uint64_t digest, std::uint64_t value) {
    // The FNV prime is odd, so multiplication by it is invertible modulo
    // 2^64. Six Newton steps produce its exact modular inverse.
    std::uint64_t inverse = 1;
    for (int step = 0; step < 6; ++step)
        inverse *= 2 - kSequenceDigestPrime * inverse;
    for (int shift = 56; shift >= 0; shift -= 8) {
        digest *= inverse;
        digest ^= static_cast<std::uint8_t>(value >> shift);
    }
    return digest;
}

void sequence_fold_i64(std::uint64_t& digest, std::int64_t value) {
    sequence_fold_u64(digest, static_cast<std::uint64_t>(value));
}

void sequence_fold_u32(std::uint64_t& digest, std::uint32_t value) {
    sequence_fold_u64(digest, value);
}

void sequence_fold_i32(std::uint64_t& digest, std::int32_t value) {
    sequence_fold_u64(digest, static_cast<std::uint32_t>(value));
}

void sequence_fold_f64(std::uint64_t& digest, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    sequence_fold_u64(digest, bits);
}

void sequence_fold_text(std::uint64_t& digest, const std::string& text) {
    sequence_fold_u64(digest, static_cast<std::uint64_t>(text.size()));
    for (const unsigned char byte : text) sequence_fold_byte(digest, byte);
}

}  // namespace

void BacktestEngine::reset_optional_sequence_witness() {
    sequence_witness_ = report_sequence::OptionalSequenceSummary{};
    sequence_hash_folded_ = 0;
    sequence_hash_folded_leaf_ = 0;
    sequence_row_folded_ = false;
    output_row_witness_base_ = report_sequence::SequenceWitness{};
    output_event_row_witness_ = report_sequence::SequenceWitness{};
}

void BacktestEngine::fold_hash_leaf(report_sequence::SequenceWitness& witness,
                                    std::uint64_t hash) {
    ++witness.count;
    sequence_fold_u64(witness.digest, hash);
}

void BacktestEngine::fold_trace_leaf(report_sequence::SequenceWitness& witness,
                                     std::int64_t timestamp, std::int32_t bar_index,
                                     const std::string& name, double value) {
    ++witness.count;
    sequence_fold_i64(witness.digest, timestamp);
    sequence_fold_i32(witness.digest, bar_index);
    sequence_fold_text(witness.digest, name);
    sequence_fold_f64(witness.digest, value);
}

void BacktestEngine::fold_row_leaf(report_sequence::SequenceWitness& witness,
                                   std::int64_t row, std::int64_t open_ms,
                                   std::int64_t close_ms, const double* values,
                                   std::int64_t count) {
    ++witness.count;
    sequence_fold_i64(witness.digest, row);
    sequence_fold_i64(witness.digest, open_ms);
    sequence_fold_i64(witness.digest, close_ms);
    sequence_fold_i64(witness.digest, count);
    for (std::int64_t i = 0; i < count; ++i)
        sequence_fold_f64(witness.digest, values[i]);
}

void BacktestEngine::fold_event_leaf(report_sequence::SequenceWitness& witness,
                                     const OutputEvent& event) {
    ++witness.count;
    sequence_fold_u64(witness.digest, event.sequence);
    sequence_fold_i32(witness.digest, event.output_index);
    sequence_fold_i32(witness.digest, event.bar_index);
    sequence_fold_i64(witness.digest, event.bar_open_ms);
    sequence_fold_i64(witness.digest, event.bar_close_ms);
    sequence_fold_u32(witness.digest, event.ordinal_in_bar);
    sequence_fold_u32(witness.digest, event.phase);
    sequence_fold_u32(witness.digest, event.confirmed);
    sequence_fold_f64(witness.digest, event.value);
    sequence_fold_u64(witness.digest, event.message_hash64);
    sequence_fold_u32(witness.digest, event.has_message ? 1u : 0u);
    if (event.has_message) sequence_fold_text(witness.digest, event.message);
}

void BacktestEngine::fold_pending_hashes_into_witness() const {
    if (sequence_hash_folded_ != 0
        && sequence_hash_folded_ <= broker_state_hashes_.size()) {
        const auto current = broker_state_hashes_[sequence_hash_folded_ - 1];
        if (sequence_hash_folded_leaf_ != current) {
            sequence_witness_.hashes.digest =
                sequence_unfold_u64(sequence_witness_.hashes.digest, sequence_hash_folded_leaf_);
            --sequence_witness_.hashes.count;
            fold_hash_leaf(sequence_witness_.hashes, current);
            sequence_hash_folded_leaf_ = current;
        }
    }
    for (std::size_t i = sequence_hash_folded_; i < broker_state_hashes_.size(); ++i)
        fold_hash_leaf(sequence_witness_.hashes, broker_state_hashes_[i]);
    sequence_hash_folded_ = broker_state_hashes_.size();
    sequence_hash_folded_leaf_ = broker_state_hashes_.empty() ? 0 : broker_state_hashes_.back();
}

void BacktestEngine::fold_current_output_row_witness() const {
    if (optional_report_mode_ != report_sequence::ReportMode::SummaryOnly
        || report_sink_state_.writer || output_open_ms_.empty() || sequence_row_folded_) {
        return;
    }
    fold_row_leaf(sequence_witness_.rows,
                  static_cast<std::int64_t>(sequence_witness_.rows.count),
                  output_open_ms_.back(), output_close_ms_.back(),
                  output_series_.empty() ? nullptr : output_series_.data(),
                  static_cast<std::int64_t>(outputs_slots_));
    sequence_row_folded_ = true;
}

void BacktestEngine::finalize_summary_output_row() {
    if (optional_report_mode_ != report_sequence::ReportMode::SummaryOnly
        || report_sink_state_.writer) {
        return;
    }
    fold_current_output_row_witness();
    // Discarding the resident row must preserve the next event's global row.
    report_sink_state_.output_base += static_cast<std::int64_t>(output_open_ms_.size());
    output_series_.clear();
    output_open_ms_.clear();
    output_close_ms_.clear();
    output_events_.clear();
    sequence_row_folded_ = false;
}

int BacktestEngine::set_optional_report_mode(report_sequence::ReportMode mode) {
    // A mode is run configuration: it cannot change under a live run, stream
    // or sink generation, and it starts a fresh witness.
    if (report_sink_setter_status() != 0) return -1;
    optional_report_mode_ = mode;
    reset_optional_sequence_witness();
    return 0;
}

report_sequence::OptionalSequenceSummary BacktestEngine::optional_sequence_summary() const {
    fold_pending_hashes_into_witness();
    fold_current_output_row_witness();
    return sequence_witness_;
}

} // namespace pineforge

// Selected report reads share these folds for the array and
// scratch sources. Formulae, return generation and floating-point order live
// in metrics::compute_equity_stats; this boundary supplies the selected facts.
namespace pineforge::internal {
namespace {

template<class PointSource>
void selected_report_metrics(
        pf_metrics_t& result, const TradeC* trades, int trades_len,
        const PointSource& curve, std::int64_t equity_len,
        double net_profit, double initial_capital, const std::string& chart_timezone,
        double first_window_open, double last_window_close,
        std::int64_t bars_in_market, std::int64_t observation_count) {
    // The anchor always exists, including M=0. Presentation may shorten the
    // published curve, but cannot change either of these captured counts.
    if (equity_len <= 0 || curve.size() < equity_len
        || observation_count != equity_len - 1)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "native selected report: invalid metric curve counts");
    pf_metrics_t computed{};
    computed.all = metrics::compute_trade_stats(
        trades, trades_len, metrics::TradeFilter::ALL, initial_capital);
    computed.longs = metrics::compute_trade_stats(
        trades, trades_len, metrics::TradeFilter::LONG, initial_capital);
    computed.shorts = metrics::compute_trade_stats(
        trades, trades_len, metrics::TradeFilter::SHORT, initial_capital);
    computed.equity = metrics::compute_equity_stats<PointSource>(
        curve, equity_len, initial_capital, chart_timezone,
        first_window_open, last_window_close, bars_in_market, net_profit,
        observation_count);
    result = computed;
}

}  // namespace

void compute_selected_report_metrics(
        pf_metrics_t& result, const TradeC* trades, int trades_len,
        const metrics::ArrayCurve& curve, std::int64_t equity_len,
        double net_profit, double initial_capital, const std::string& chart_timezone,
        double first_window_open, double last_window_close,
        std::int64_t bars_in_market, std::int64_t observation_count) {
    selected_report_metrics(result, trades, trades_len, curve, equity_len,
                            net_profit, initial_capital, chart_timezone,
                            first_window_open, last_window_close,
                            bars_in_market, observation_count);
}

void compute_selected_report_metrics(
        pf_metrics_t& result, const TradeC* trades, int trades_len,
        const metrics::SpillCurve& curve, std::int64_t equity_len,
        double net_profit, double initial_capital, const std::string& chart_timezone,
        double first_window_open, double last_window_close,
        std::int64_t bars_in_market, std::int64_t observation_count) {
    selected_report_metrics(result, trades, trades_len, curve, equity_len,
                            net_profit, initial_capital, chart_timezone,
                            first_window_open, last_window_close,
                            bars_in_market, observation_count);
}

}  // namespace pineforge::internal


// ---------------------------------------------------------------------------
// The terminal range-end row source and the versioned
// report envelope. Appended at the end of the file, after every function the
// file held before.
namespace pineforge {

struct OpenReportRowSource::OwnedExcursions {
    detail::ScratchSequence<ClosedLotExcursion> values;
};

// A terminal mark replaces the previous generation wholesale, exactly as the
// row vector's clear + append did. Retained C-accessor rows belong to the
// replaced generation and are dropped with it.
void OpenReportRowSource::clear() noexcept {
    present_ = false;
    was_long_ = false;
    mark_price_ = 0.0;
    mark_time_ms_ = 0;
    interval_index_ = 0;
    account_fx_ = 1.0;
    count_ = 0;
    pnl_ = 0.0;
    excursions_.reset();
    materialized_.clear();
    synthetic_.clear();
}

void OpenReportRowSource::begin(
        double mark_price, std::int64_t mark_time_ms, int interval_index,
        double account_fx, bool was_long, std::size_t count) {
    clear();
    was_long_ = was_long;
    mark_price_ = mark_price;
    mark_time_ms_ = mark_time_ms;
    interval_index_ = interval_index;
    account_fx_ = account_fx;
    std::shared_ptr<OwnedExcursions> excursions;
    if (count != 0 && owner_ != nullptr && owner_->lot_excursion_hook_) {
        excursions = std::make_shared<OwnedExcursions>();
        excursions->values.set_directory(owner_->trades_.scratch_directory());
    }
    double marked = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const Trade built = build_row(index, true);
        if (!internal::closed_row_all_finite(built))
            throw std::logic_error("non-finite open-position report row");
        if (excursions)
            excursions->values.push_back({built.max_runup, built.max_drawdown});
        marked += built.pnl;
    }
    // Publish only the complete, validated generation. A callback, row guard
    // or scratch failure leaves no terminal row addressable and no row PnL.
    excursions_ = std::move(excursions);
    pnl_ = marked;
    count_ = count;
    present_ = count != 0;
}

// The exact row the range-end append used to push: the same non-mutating
// builder, the same context fields and the same open_at_end mark. Only the
// storage changed; every field, including the four identity strings, is
// rebuilt from the lot, the frozen terminal mark and any retained host answer.
Trade OpenReportRowSource::row(std::size_t index) const {
    if (!present_) {
        if (index >= synthetic_.size())
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "range-end report row index is outside the seeded rows");
        return synthetic_[index];
    }
    if (owner_ == nullptr || index >= count_)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "range-end report row index is outside the terminal mark");
    return build_row(index, false);
}

Trade OpenReportRowSource::build_row(
        std::size_t index, bool consult_excursion_hook) const {
    if (owner_ == nullptr)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "range-end report row has no owner");
    const BacktestEngine& engine = *owner_;
    // The mark names the lots it was taken over. A lot retired after the mark
    // (before its report row was read) would otherwise rebuild a different
    // row; the producers only mark a terminal, frozen book, so this is an
    // invariant, not a runtime refusal of a valid run.
    if (index >= engine.pyramid_entries_.size())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "range-end report lot was retired after its terminal mark");
    const PyramidEntry& lot = engine.pyramid_entries_[index];
    execution::PhysicalExecutionContext context;
    context.effective_time_ms = mark_time_ms_;
    context.interval_index = interval_index_;
    context.account_fx = account_fx_;
    if (!std::isnan(engine.fold_exit_trail_peak_))
        context.preceding_exit_trail_peak = engine.fold_exit_trail_peak_;
    ClosedLotExcursion retained;
    if (excursions_) retained = excursions_->values[index];
    Trade built = engine.build_close_trade_with_costs(
        lot, lot.qty, mark_price_, was_long_,
        engine.allocated_entry_commission(lot, lot.qty),
        engine.calc_commission_at(mark_price_, lot.qty, account_fx_), context,
        excursions_ ? &retained : nullptr, consult_excursion_hook);
    built.open_at_end = true;
    return built;
}

// The legacy borrowed-pointer boundary (the C entry/exit string accessors).
// Rows are retained from the front as they are first asked for; this is the
// only place a range-end row outlives its reader, and no report, fold or sink
// path calls it.
const Trade& OpenReportRowSource::materialize(std::size_t index) const {
    if (!present_) {
        if (index >= synthetic_.size())
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "range-end report row index is outside the seeded rows");
        return synthetic_[index];
    }
    if (owner_ == nullptr || index >= count_)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "range-end report row index is outside the terminal mark");
    while (materialized_.size() <= index) materialized_.push_back(row(materialized_.size()));
    return materialized_[index];
}

bool NativeExecutionConsumer::report_capture_needs_sink(
        const BacktestEngine& engine, bool include_pending) noexcept {
    const auto* consumer = static_cast<const NativeExecutionConsumer*>(
        engine.execution_consumer_slot_.ptr.get());
    if (!consumer) return false;
    const auto& epoch = consumer->selected_epoch_;
    return epoch.admitted_selected || epoch.phase_observed
        || (include_pending && (epoch.window_configured || epoch.observer_registered));
}

int BacktestEngine::set_report_mode_v1(ReportMode mode) {
    switch (mode) {
        case ReportMode::FULL:
        case ReportMode::SUMMARY_ONLY:
        case ReportMode::STREAMED:
            break;
        default:
            // Unknown words are invalid, never aliases for FULL.
            return PF_REPORT_MODE_INVALID;
    }
    // A live execution, dispatch or stream owns the report configuration.
    if (report_sink_setter_status() != 0) return PF_REPORT_MODE_BUSY;
    const bool sink = report_sink_state_.enabled;
    if (mode == ReportMode::FULL && sink) return PF_REPORT_MODE_CONFLICT;
    if (mode == ReportMode::SUMMARY_ONLY && sink) return PF_REPORT_MODE_CONFLICT;
    if (mode == ReportMode::STREAMED && !sink) return PF_REPORT_MODE_CONFLICT;
    if (mode == ReportMode::SUMMARY_ONLY
        && NativeExecutionConsumer::report_capture_needs_sink(*this))
        return PF_REPORT_MODE_REPLAY_UNAVAILABLE;
    // A whole-array presenter cannot certify omitted or external rows without
    // an exact bounded-state adapter. Refuse before changing configuration.
    if (mode != ReportMode::FULL && !report_presenter_supports_non_full())
        return mode == ReportMode::SUMMARY_ONLY
            ? PF_REPORT_MODE_REPLAY_UNAVAILABLE : PF_REPORT_MODE_CONFLICT;
    report_mode_ = mode;
    return PF_REPORT_MODE_ACCEPTED;
}

namespace {

// One sequence's logical count and capture policy. Disabled optional sources
// carry zero in all three fields; a captured empty sequence keeps captured=1.
pf_report_sequence_v1 report_sequence_v1(std::int64_t logical_count,
                                         bool enabled, bool omitted, bool streamed) {
    pf_report_sequence_v1 sequence{};
    if (!enabled) return sequence;
    sequence.logical_count = logical_count;
    // Omitted resident history is uncaptured unless the sink still emits it.
    // A captured empty sequence still reports captured=1.
    sequence.captured = omitted && !streamed ? 0u : 1u;
    sequence.external = streamed ? 1u : 0u;
    return sequence;
}

}  // namespace

int BacktestEngine::fill_report_envelope_v1(ReportEnvelopeV1* out) const {
    if (out == nullptr || out->metadata.struct_size != sizeof(pf_report_metadata_v1)
        || out->metadata.version != PF_REPORT_ENVELOPE_VERSION)
        return PF_REPORT_MODE_INVALID;
    // Filling over an unfreed payload would lose its owner.
    const ReportC& existing = out->report;
    if (existing.trades != nullptr || existing.trades_len != 0
        || existing.security_diag != nullptr || existing.security_diag_len != 0
        || existing.trace != nullptr || existing.trace_len != 0
        || existing.trace_names != nullptr || existing.trace_names_len != 0
        || existing.equity_curve != nullptr || existing.equity_curve_len != 0
        || existing.broker_state_hash != nullptr || existing.broker_state_hash_len != 0)
        return PF_REPORT_MODE_INVALID;
    // A successful read reports a frozen generation; a live attempt refuses.
    if (report_sink_setter_status() != 0) return PF_REPORT_MODE_BUSY;
    // The capture policy and the configured sink must still agree; a sink
    // removed or installed after the setter makes the shape ambiguous.
    const bool sink_enabled = report_sink_state_.enabled || report_sink_state_.receipt.complete;
    if ((report_mode_ == ReportMode::FULL && sink_enabled)
        || (report_mode_ == ReportMode::SUMMARY_ONLY && sink_enabled)
        || (report_mode_ == ReportMode::STREAMED && !sink_enabled))
        return PF_REPORT_MODE_CONFLICT;
    // Pending windows/observers configure the next run; this read judges
    // only the admitted capture of the generation whose report it returns.
    if (report_mode_ == ReportMode::SUMMARY_ONLY
        && NativeExecutionConsumer::report_capture_needs_sink(*this, false))
        return PF_REPORT_MODE_REPLAY_UNAVAILABLE;

    ReportEnvelopeV1 local{};
    local.metadata.struct_size = sizeof(pf_report_metadata_v1);
    local.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    local.metadata.mode = static_cast<pf_report_mode_v1_t>(report_mode_);
    local.report = ReportC{};
    try {
        fill_report(&local.report);
    } catch (...) {
        free_report(&local.report);
        throw;
    }

    const bool streamed = report_mode_ == ReportMode::STREAMED;
    const bool summary = report_mode_ == ReportMode::SUMMARY_ONLY;
    const bool current = NativeExecutionConsumer::rows_are_current(*this);
    const auto sequence = [current, summary, streamed](std::int64_t count, bool enabled,
                                                      bool omitted = false) {
        return report_sequence_v1(current ? count : 0, enabled, summary || omitted, streamed);
    };
    const auto& sink = report_sink_state_;
    // Before completion a retired row is counted at the cursor and a pending
    // one where it lives. The trailer includes the held row/event/hash too;
    // their still-resident buffers must not be counted again after finish.
    // Selected capture transfers scratch ownership before this read. A
    // completed receipt remains authoritative; without a sink the equity
    // recorder exposes its logical count even when only its tail is resident.
    std::int64_t equity_count = sink.receipt.complete ? sink.receipt.count_e
        : sink.equity_scratch ? sink.equity_scratch->size() : equity_logical_point_count();
    if (local.report.equity_curve_len > equity_count)
        equity_count = local.report.equity_curve_len;
    const bool optional_summary =
        optional_report_mode_ == report_sequence::ReportMode::SummaryOnly;
    const auto witness = current && optional_summary ? optional_sequence_summary()
        : report_sequence::OptionalSequenceSummary{};
    const std::int64_t trace_count = optional_summary
        ? static_cast<std::int64_t>(witness.traces.count)
        : sink.receipt.count_x + static_cast<std::int64_t>(trace_buffer_.size());
    const std::int64_t output_rows = optional_summary
        ? static_cast<std::int64_t>(witness.rows.count)
        : sink.receipt.count_o + (sink.receipt.complete ? 0
            : static_cast<std::int64_t>(output_open_ms_.size()));
    const std::int64_t output_events = optional_summary
        ? static_cast<std::int64_t>(witness.events.count)
        : sink.receipt.count_v + (sink.receipt.complete ? 0
            : static_cast<std::int64_t>(output_events_.size()));
    const std::int64_t hash_count = broker_state_hash_recording_
        ? (optional_summary ? static_cast<std::int64_t>(witness.hashes.count)
            : sink.receipt.count_h + (sink.receipt.complete ? 0
                : static_cast<std::int64_t>(broker_state_hashes_.size())))
        : 0;

    local.metadata.trades = sequence(local.report.total_trades, true);
    local.metadata.equity_curve = sequence(
        equity_count, equity_count > 0 || sink.equity_scratch != nullptr,
        active_equity_report_mode_ == ReportMode::SUMMARY_ONLY || metrics_streaming_.no_curve);
    local.metadata.broker_state_hash = sequence(
        hash_count, broker_state_hash_recording_, optional_summary);
    local.metadata.trace = sequence(trace_count, trace_enabled_, optional_summary);
    local.metadata.trace_names = sequence(
        static_cast<std::int64_t>(trace_names_.size()), trace_enabled_, optional_summary);
    local.metadata.output_rows = sequence(output_rows, outputs_enabled_, optional_summary);
    local.metadata.output_events = sequence(output_events, outputs_enabled_, optional_summary);

    const auto* consumer = execution_consumer_slot_.ptr && execution_consumer_slot_.native
        ? static_cast<const NativeExecutionConsumer*>(execution_consumer_slot_.ptr.get())
        : nullptr;
    const bool completed = current && last_run_status_ == 0 && last_error_.empty() && consumer
        && consumer->state_kind() == NativeLifecycleKind::Completed
        && ((active_equity_report_mode_ != ReportMode::SUMMARY_ONLY
             && !metrics_streaming_.no_curve) || equity_summary_ratios_ready())
        && (!metrics_streaming_.no_curve
            || metrics_streaming_.receipt.route == metrics::MetricsRatioRouteV1::Streaming);
    local.metadata.complete = completed && (!streamed || sink.receipt.complete) ? 1u : 0u;
    *out = local;
    return PF_REPORT_MODE_ACCEPTED;
}

}  // namespace pineforge
