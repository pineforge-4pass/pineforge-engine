/*
 * engine_report.cpp — fill_report / free_report / trace recording
 */

#include "engine_internal.hpp"

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

    fill_security_diag_section(out);
    fill_trace_section(out);
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
            net_profit += t.pnl;
        }

        out->net_profit = net_profit;
    } else {
        out->trades = nullptr;
        out->net_profit = 0.0;
    }
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
}

}  // namespace pineforge
