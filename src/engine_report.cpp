/*
 * engine_report.cpp — fill_report / free_report / trace recording
 */

#include "engine_internal.hpp"
#include "native_execution_consumer.hpp"
#include "selected_report_view.hpp"
#include <pineforge/metrics_stream.hpp>
#include "report_scratch.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;


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
    e.name_id = intern_trace_name(name);
    e.value = value;
    if (!stream_report_sink_trace(e)) trace_buffer_.push_back(e);
}


// The report of the latest run or stream_begin call this engine was given, or
// the empty report. The engine keeps the rows of the last run it BEGAN, and a
// call that never began (a begin outside Ready, a refused bar array or run
// option, a calendar or timezone refusal) leaves them in place on purpose, so
// the rows alone cannot say which call they answer. Every batch reader shares
// this one gate: strategy_native_run_v1, the native module wrappers and the
// generated run_backtest / run_backtest_full, which call run() and then this.
// A call that began owns its rows whatever became of it (a Completed report and
// a started-then-failed run's partial rows publish unchanged); a call that did
// not, or one refused over a live attempt (a batch call while a stream is live),
// owns none: `*out` is overwritten with the empty report (every array NULL,
// every count zero) and never freed, since what it held is not this call's to
// release. strategy_stream_fill_report, the explicit snapshot, runs this same
// function asking for the rows' own owner (NativeExecutionConsumer::
// fill_snapshot_report), so a live stream's rows survive such a refusal.
void BacktestEngine::fill_report(ReportC* out) const {
    const ReportSinkCallScope sink_call(*this);
    // A recursive capture/observer read is refused before any row gate;
    // the terminal helper consumes its reentry latch.
    if (NativeExecutionConsumer::refuse_report_in_dispatch(*this, out)) return;
    if (!NativeExecutionConsumer::rows_are_current(*this)) {
        *out = ReportC{};
        return;
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
    fill_trace_section(out);

    // ABI v4 task 6: per-script-bar broker-state hash, empty unless
    // recording was enabled via set_broker_state_hash_recording. Owns the
    // allocation; freed by free_report.
    const int64_t hn = (int64_t)broker_state_hashes_.size();
    out->broker_state_hash_len = hn;
    out->broker_state_hash = hn > 0 ? new uint64_t[hn] : nullptr;
    if (hn > 0) std::copy(broker_state_hashes_.begin(), broker_state_hashes_.end(), out->broker_state_hash);
    finish_report_sink(out);
}

// Copy ``trades_`` — followed by ``range_end_trades_``, the report-only rows
// of a position still open after the final bar
// (NativeExecutionConsumer::append_open_position_report_rows) — into a
// freshly heap-allocated TradeC[] on ``out`` and accumulate ``net_profit``.
// A sink allocates only the range-end view. free_report owns either array.
void BacktestEngine::fill_trades_section(ReportC* out) const {
    const int n_closed = (int)trades_.size();
    const int n = n_closed + (int)range_end_trades_.size();
    out->total_trades = n;
    if (report_sink_state_.enabled) {
        // The hook can re-mark open lots, but closed history is immutable.
        // Never materialize that history merely to discard it at finalization.
        const int open_count = (int)range_end_trades_.size();
        out->net_profit = report_closed_net_profit();
        out->trades_len = open_count;
        out->trades = open_count > 0 ? new TradeC[open_count] : nullptr;
        for (int i = 0; i < open_count; ++i) {
            out->trades[i] = report_trade_row(range_end_trades_[i]);
            out->net_profit += out->trades[i].pnl;
        }
        return;
    }
    out->trades_len = n;

    if (n > 0) {
        out->trades = new TradeC[n];
        double net_profit = 0.0;

        for (int i = 0; i < n; i++) {
            const Trade& t = (i < n_closed) ? trades_[i]
                                            : range_end_trades_[i - n_closed];
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
            net_profit += t.pnl;
        }

        out->net_profit = net_profit;
    } else {
        out->trades = nullptr;
        out->net_profit = 0.0;
    }
}

double BacktestEngine::report_closed_net_profit() const {
    // A resident-only fold starts at global zero. A sealing owner must supply
    // its exact report prefix before retiring rows; a suffix is not a report.
    // The engine's differently seeded decision counters are not substitutes.
    if (trades_.base() != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report trade fold is missing its sealed prefix");
    double sum = 0.0;
    for (const auto& trade : trades_) sum += trade.pnl;
    return sum;
}

void BacktestEngine::compute_report_sink_trade_metrics(
        ReportC* out, const TradeC* open_rows, int count) const {
    // Keep each filter's original left-fold order: closed rows, then the
    // presented range-end rows. Interleaving independent filters changes none
    // of their additions. No TradeC history or separately summed tail exists.
    if (trades_.base() != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report trade metrics are missing their sealed prefix");
    metrics::TradeStatsAccumulator all(metrics::TradeFilter::ALL, initial_capital_);
    metrics::TradeStatsAccumulator longs(metrics::TradeFilter::LONG, initial_capital_);
    metrics::TradeStatsAccumulator shorts(metrics::TradeFilter::SHORT, initial_capital_);
    const auto fold = [&](const TradeC& row) {
        all.fold(row);
        longs.fold(row);
        shorts.fold(row);
    };
    for (const auto& trade : trades_) fold(report_trade_row(trade));
    for (int i = 0; i < count; ++i) fold(open_rows[i]);
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


// Compute metrics after fill_trades_section. Sink-off owns its full arrays;
// a sink folds closed rows from the ledger plus its presented open-lot view
// and reads equity history from scratch.
// Metric history derives from recorded points, NOT script_bars_processed:
// an exception mid-run can truncate the curve (metrics then describe the
// truncated prefix; consumers must check strategy_get_last_error).
// No ScopedTimezone may be held here: compute_equity_stats takes the
// non-recursive global tz lock when chart_timezone_ is non-UTC.
void BacktestEngine::fill_metrics_section(ReportC* out) const {
    if (report_sink_state_.equity_scratch && !report_sink_state_.selected_equity_stream) {
        fill_report_sink_metrics_section(out);
        return;
    }
    const int64_t n = (int64_t)equity_curve_.size();
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
    if (report_sink_state_.enabled)
        compute_report_sink_trade_metrics(out, out->trades, out->trades_len);
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
        retire_report_sink_output_row();
        output_series_.resize(output_series_.size() + slots, kUnwrittenOutput);
        output_open_ms_.push_back(open_ms);
        output_close_ms_.push_back(close_ms);
        std::fill(output_ordinals_.begin(), output_ordinals_.end(), 0u);
        output_row_seq_base_ = output_sequence_;
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
    if (output_events_.size() >= static_cast<size_t>(INT_MAX))
        refuse_output_limit("too_many_events", "outputs: the event queue is full");
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
    if (output_events_.empty()) return;
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

// CR-150 L6B1: path sink runtime. Kept after the existing report/output code
// so the line-pinned documentation above retains its source coordinates.
#include "report_sink_writer.hpp"

namespace pineforge {

BacktestEngine::ReportSinkState::ReportSinkState() = default;
BacktestEngine::ReportSinkState::~ReportSinkState() = default;
BacktestEngine::ReportSinkState::ReportSinkState(const ReportSinkState& other)
    : enabled(other.enabled), trade_history_streamable(other.trade_history_streamable),
      path(other.path), run_id(other.run_id), scratch_dir(other.scratch_dir) {}
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
    }
    if (report_sink_state_.equity_scratch) {
        if (next.enabled) {
            // Replacing next-run configuration must not discard the current
            // generation's record-time curve and validation latches.
            next.equity_scratch = std::move(report_sink_state_.equity_scratch);
            next.equity_tail_stored = report_sink_state_.equity_tail_stored;
            next.selected_equity_stream = std::move(report_sink_state_.selected_equity_stream);
        } else {
            // Removing the sink restores the existing array-report contract.
            NativeExecutionConsumer::restore_report_sink_equity_curve(*this);
        }
    }
    report_sink_state_ = std::move(next);
    return 0;
}

int BacktestEngine::report_sink_receipt_v1(pf_report_sink_receipt_v1* out) const noexcept {
    if (!out || out->struct_size != sizeof(*out) || out->version != 1) return -1;
    pf_report_sink_receipt_v1 local{};
    if (report_sink_state_.enabled) local = report_sink_state_.receipt;
    local.struct_size = sizeof local;
    local.version = 1;
    *out = local;
    return 0;
}

void BacktestEngine::set_trade_history_streamable(bool streamable) {
    if (report_sink_setter_status() != 0)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "trade history policy cannot change during a run");
    report_sink_state_.trade_history_streamable = streamable;
}

void BacktestEngine::begin_report_sink() {
    auto& sink = report_sink_state_;
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
    sink.output_base = 0;
    if (!sink.enabled) return;
    // The new run owns these histories. Drop capacity left by a previous
    // sink-off run before callbacks, even if outputs are disabled this time.
    std::vector<uint64_t>().swap(broker_state_hashes_);
    std::vector<TraceEntryC>().swap(trace_buffer_);
    std::vector<double>().swap(output_series_);
    std::vector<int64_t>().swap(output_open_ms_);
    std::vector<int64_t>().swap(output_close_ms_);
    std::vector<OutputEvent>().swap(output_events_);
    sink.receipt.trades_retained = sink.trade_history_streamable ? 0u : 1u;
    const pf_report_sink_v1 descriptor{sizeof(pf_report_sink_v1), 1,
        sink.path.c_str(), sink.run_id.c_str(), sink.scratch_dir.c_str()};
    try {
        sink.writer = std::make_unique<report_sink::LineWriter>(descriptor);
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
        const auto code = error.reason() == report_sink::WriterFailure::io
            ? RunFailureCode::report_sink_failed : RunFailureCode::engine_invariant;
        throw coded<std::runtime_error>(code, {}, error.what());
    }
}

void BacktestEngine::finish_report_sink(ReportC* out) const {
    auto& sink = report_sink_state_;
    if (!sink.enabled) return;
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
            write_report_sink_trades(open_rows, open_count);
            if (outputs_enabled_) {
                write_report_sink_output_row();
                sink.writer->write_k(output_constants_.data(), outputs_constants_);
                ++sink.receipt.count_k;
            }
            write_report_sink_hashes();
            const auto trailer = sink.writer->write_trailer();
            sink.receipt.count_e = trailer.count_e;
            sink.receipt.count_t = trailer.count_t;
            sink.receipt.count_o = trailer.count_o;
            sink.receipt.count_v = trailer.count_v;
            sink.receipt.count_k = trailer.count_k;
            sink.receipt.count_x = trailer.count_x;
            sink.receipt.count_h = trailer.count_h;
            std::memcpy(sink.receipt.sha256_hex, trailer.sha256_hex, sizeof trailer.sha256_hex);
            sink.receipt.complete = 1;
        }
    } catch (const report_sink::WriterError& error) {
        sink.writer.reset();
        free_report(out);
        *out = {};
        auto& engine = const_cast<BacktestEngine&>(*this);
        engine.last_run_status_ = 1;
        const auto code = error.reason() == report_sink::WriterFailure::io
            ? RunFailureCode::report_sink_failed : RunFailureCode::engine_invariant;
        const auto failure = coded<std::runtime_error>(code, {}, error.what());
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
    const int64_t total = closed_count + count;
    if (sink.receipt.count_t < static_cast<int64_t>(trades_.base())
        || sink.receipt.count_t > total)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report trade sink cursor is outside the resident window");
    for (int64_t i = sink.receipt.count_t; i < total; ++i) {
        const TradeC row = i < closed_count
            ? report_trade_row(trades_[static_cast<std::size_t>(i)])
            : open_rows[static_cast<std::size_t>(i - closed_count)];
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
        const Trade& identity = get_report_trade(static_cast<int>(i));
        sink.writer->write_t(i + INT64_C(1), wire, identity.entry_incarnation,
            closed_trade_close_cause(static_cast<int>(i)), identity.entry_id,
            identity.exit_id, identity.exit_comment);
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
        const auto code = error.reason() == report_sink::WriterFailure::io
            ? RunFailureCode::report_sink_failed : RunFailureCode::engine_invariant;
        const auto failure = coded<std::runtime_error>(code, {}, error.what());
        auto& engine = const_cast<BacktestEngine&>(*this);
        engine.last_run_status_ = 1;
        note_run_failure(engine, failure.what(), classify_run_failure(failure));
        throw failure;
    }
}

bool BacktestEngine::stream_report_sink_trace(const TraceEntryC& entry) {
    auto& sink = report_sink_state_;
    if (!sink.writer || sink.receipt.complete) return false;
    const pf_trace_entry_t wire{entry.timestamp, entry.bar_index, entry.name_id, entry.value};
    report_sink_record([&] {
        sink.writer->write_x(wire, trace_names_[static_cast<std::size_t>(entry.name_id)]);
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
    report_sink_record([&] {
        for (const auto hash : broker_state_hashes_) {
            sink.writer->write_h(hash);
            ++sink.receipt.count_h;
        }
    });
}

void BacktestEngine::retire_report_sink_hashes() {
    auto& sink = report_sink_state_;
    if (!sink.writer || sink.receipt.complete) return;
    // The previous point is now final, including any Pine terminal rewrite.
    // Its successor is appended by the existing native or marking-host path.
    write_report_sink_hashes();
    broker_state_hashes_.clear();
}

} // namespace pineforge

// CR-150 L17B: selected report reads share these folds for the array and
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
