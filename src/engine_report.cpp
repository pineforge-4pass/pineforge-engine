/*
 * engine_report.cpp — fill_report / free_report / trace recording
 */

#include "engine_internal.hpp"

#include <pineforge/metrics.hpp>

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
    trace_buffer_.push_back(e);
}


void BacktestEngine::fill_report(ReportC* out) const {
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
}


// Copy ``trades_`` — followed by ``range_end_trades_``, the report-only rows
// of a position still open after the final bar
// (NativeExecutionConsumer::append_open_position_report_rows) — into a
// freshly heap-allocated TradeC[] on ``out`` and accumulate ``net_profit``
// for the report. Owns the allocation; freed by ``free_report``.
void BacktestEngine::fill_trades_section(ReportC* out) const {
    const int n_closed = (int)trades_.size();
    const int n = n_closed + (int)range_end_trades_.size();
    out->total_trades = n;
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


// Copy the equity curve out and compute all metric blocks. Must run AFTER
// fill_trades_section (reads out->trades). Owns the curve allocation;
// freed by free_report (which expects new pf_equity_point_t[n]).
// equity_curve_len derives from the vector size, NOT script_bars_processed:
// an exception mid-run can truncate the curve (metrics then describe the
// truncated prefix; consumers must check strategy_get_last_error).
// No ScopedTimezone may be held here: compute_equity_stats takes the
// non-recursive global tz lock when chart_timezone_ is non-UTC.
void BacktestEngine::fill_metrics_section(ReportC* out) const {
    const int64_t n = (int64_t)equity_curve_.size();
    out->equity_curve_len = n;
    if (n > 0) {
        out->equity_curve = new pf_equity_point_t[n];
        std::copy(equity_curve_.begin(), equity_curve_.end(), out->equity_curve);
    } else {
        out->equity_curve = nullptr;
    }
    present_report(out);
    using metrics::TradeFilter;
    out->metrics.all = metrics::compute_trade_stats(
        out->trades, out->trades_len, TradeFilter::ALL, initial_capital_);
    out->metrics.longs = metrics::compute_trade_stats(
        out->trades, out->trades_len, TradeFilter::LONG, initial_capital_);
    out->metrics.shorts = metrics::compute_trade_stats(
        out->trades, out->trades_len, TradeFilter::SHORT, initial_capital_);
    out->metrics.equity = metrics::compute_equity_stats(
        out->equity_curve, n, initial_capital_, chart_timezone_,
        first_bar_open_, current_bar_.close, bars_in_market_,
        out->net_profit);   // includes the range-end rows, like the trades
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
// defect, engine_invariant; a cap the run reached is pine_runtime_limit; the
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

[[noreturn]] void refuse_output_limit(const char* limit, const char* text) {
    throw coded<std::runtime_error>(RunFailureCode::pine_runtime_limit,
                                    {{"limit", limit}, {"max", INT_MAX}}, text);
}

// FNV-1a 64 of the bytes, as stream_state_hash folds them.
uint64_t output_message_hash(const std::string& text) {
    uint64_t hash = 1469598103934665603ULL;
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
        note_run_failure(*this, "outputs: this module declares no outputs",
                         RunFailureCode::outputs_rejected, {{"reason", "not_declared"}});
        return false;
    }
    if (on != outputs_enabled_) {
        if (output_host_running()) {
            note_run_failure(*this, "outputs: recording cannot change during a run",
                             RunFailureCode::outputs_rejected, {{"reason", "run_in_progress"}});
            return false;
        }
        outputs_enabled_ = on;
        clear_output_record();
    }
    // A setter that succeeds leaves no failure behind (strategy_get_last_error_code).
    clear_run_failure(*this);
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
        // bar_index is an int32_t, like the C record's.
        if (output_open_ms_.size() >= static_cast<size_t>(INT_MAX))
            refuse_output_limit("output_rows", "outputs: the row count is at its limit");
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
            "outputs: bar " + std::to_string(output_open_ms_.size() - 1)
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
        refuse_output_limit("output_events", "outputs: the event queue is full");
    if (output_sequence_ == std::numeric_limits<uint64_t>::max())
        refuse_output("event sequence overflow");
    OutputEvent event;
    event.sequence = output_sequence_ + 1;
    event.output_index = output;
    event.bar_index = static_cast<int32_t>(output_open_ms_.size() - 1);
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
    output_cleared_sequence_ = std::max(output_cleared_sequence_, output_events_.back().sequence);
    output_events_.clear();
}

double BacktestEngine::output_constant_value(int index) const {
    if (index < 0 || index >= output_constants_count()) return kUnwrittenOutput;
    const size_t at = static_cast<size_t>(index);
    return output_constant_written_[at] ? output_constants_[at] : kUnwrittenOutput;
}

}  // namespace pineforge
