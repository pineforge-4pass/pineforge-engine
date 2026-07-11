/*
 * engine_report.cpp — fill_report / free_report / trace recording
 */

#include "engine_internal.hpp"

#include <pineforge/metrics.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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
    fill_order_events_section(out);
}

void BacktestEngine::fill_order_events_section(ReportC* out) const {
    const int64_t n = static_cast<int64_t>(order_events_.size());
    out->order_events_len = n;
    out->order_event_count = order_event_count_;
    out->order_event_hash = order_event_hash_;
    out->order_event_dropped = order_event_dropped_;
    if (n == 0) {
        out->order_events = nullptr;
        return;
    }
    out->order_events = new pf_order_event_t[n]{};
    auto copy_string = [](const std::string& source) {
        char* result = new char[source.size() + 1];
        std::memcpy(result, source.c_str(), source.size() + 1);
        return result;
    };
    for (int64_t i = 0; i < n; ++i) {
        out->order_events[i] = order_events_[static_cast<std::size_t>(i)].value;
        out->order_events[i].id = copy_string(
            order_events_[static_cast<std::size_t>(i)].id);
        out->order_events[i].from_entry = copy_string(
            order_events_[static_cast<std::size_t>(i)].from_entry);
        out->order_events[i].oca_name = copy_string(
            order_events_[static_cast<std::size_t>(i)].oca_name);
    }
}


// Copy ``trades_`` into a freshly heap-allocated TradeC[] on ``out`` and
// accumulate ``net_profit`` for the report. Owns the allocation; freed by
// ``free_report``.
void BacktestEngine::fill_trades_section(ReportC* out) const {
    int n = (int)trades_.size();
    out->total_trades = n;
    out->trades_len = n;

    if (n > 0) {
        out->trades = new TradeC[n];
        double net_profit = 0.0;

        for (int i = 0; i < n; i++) {
            const Trade& t = trades_[i];
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
    using metrics::TradeFilter;
    out->metrics.all = metrics::compute_trade_stats(
        out->trades, out->trades_len, TradeFilter::ALL, initial_capital_);
    out->metrics.longs = metrics::compute_trade_stats(
        out->trades, out->trades_len, TradeFilter::LONG, initial_capital_);
    out->metrics.shorts = metrics::compute_trade_stats(
        out->trades, out->trades_len, TradeFilter::SHORT, initial_capital_);
    out->metrics.equity = metrics::compute_equity_stats(
        out->equity_curve, n, initial_capital_, chart_timezone_,
        first_bar_open_, current_bar_.close, bars_in_market_, net_profit_sum_);
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
    if (report && report->order_events) {
        for (int64_t i = 0; i < report->order_events_len; ++i) {
            delete[] report->order_events[i].id;
            delete[] report->order_events[i].from_entry;
            delete[] report->order_events[i].oca_name;
        }
        delete[] report->order_events;
        report->order_events = nullptr;
        report->order_events_len = 0;
        report->order_event_count = 0;
        report->order_event_hash = 0;
        report->order_event_dropped = 0;
    }
}

}  // namespace pineforge
