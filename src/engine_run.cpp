/*
 * engine_run.cpp — the account-currency FX series, the per-run state reset,
 * the chart-bar session predicate and the input accessors.
 *
 * The bar pumps, the magnifier sampling loop and the run entrypoints are not
 * here: driving a run is the market driver's and the execution consumer's
 * work, and the open_trade_* accessors are engine_trade_accessors.cpp's.
 * Two rules this file no longer states are the spec's: the chart body's EMA
 * warmup mode is the ambient ta::EmaSeeding default the source layer raises
 * around one evaluation context, and whether a bar's own close executes the
 * market orders that bar's script just placed is NativeCloseExecution
 * (AfterCalculation vs NextEligiblePoint). ADR-0001 rule 5.
 */

#include "engine_internal.hpp"

#include <pineforge/ta.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;

namespace {
// A callback owns its origin, not an engine clone. Nesting and exceptions restore
// the previous owner; copying an engine cannot inherit a live callback token.
thread_local const BacktestEngine* birth_context_owner = nullptr;
thread_local std::optional<OrderBirth> birth_context;
class ScopedBirthContext {
public:
    ScopedBirthContext(const BacktestEngine* owner, const OrderBirth& birth)
        : previous_owner_(birth_context_owner), previous_(birth_context) {
        birth_context_owner = owner; birth_context = birth;
    }
    ~ScopedBirthContext() {
        birth_context_owner = previous_owner_; birth_context = previous_;
    }
private:
    const BacktestEngine* previous_owner_;
    std::optional<OrderBirth> previous_;
};
[[noreturn]] void reject_chart_bar(int index, const char* rule) {
    throw std::invalid_argument("chart bar[" + std::to_string(index) + "]." + rule);
}

// Structural admission only: no price-domain, grid, calendar or financial
// arithmetic policy. Scan the entire supplied array before any run mutation.
// Each public run() calls this once; run_tf_impl receives validated input.
void validate_chart_bars(const Bar* bars, int n) {
    if (n < 0) throw std::invalid_argument("chart bar count must be non-negative");
    if (n > 0 && bars == nullptr)
        throw std::invalid_argument("chart bars must be non-null for a nonempty array");
    for (int i = 0; i < n; ++i) {
        const Bar& bar = bars[i];
        if (!std::isfinite(bar.open)) reject_chart_bar(i, "open must be finite");
        if (!std::isfinite(bar.high)) reject_chart_bar(i, "high must be finite");
        if (!std::isfinite(bar.low)) reject_chart_bar(i, "low must be finite");
        if (!std::isfinite(bar.close)) reject_chart_bar(i, "close must be finite");
        if (bar.low > std::min(bar.open, bar.close))
            reject_chart_bar(i, "low must not exceed open or close");
        if (bar.high < std::max(bar.open, bar.close))
            reject_chart_bar(i, "high must not be below open or close");
        // NaN is unavailable activity, distinct from a known zero total.
        if (!std::isnan(bar.volume) && (!std::isfinite(bar.volume) || bar.volume < 0))
            reject_chart_bar(i, "volume must be non-negative finite or NaN (unavailable)");
        if (i > 0) {
            const int64_t previous = bars[i - 1].timestamp;
            if (bar.timestamp <= previous)
                reject_chart_bar(i, "timestamp must be strictly increasing");
            // With increasing signed values, a difference can overflow only
            // when the previous timestamp is negative. This addition is safe;
            // do not subtract the timestamps before checking representability.
            if (previous < 0 && bar.timestamp > std::numeric_limits<int64_t>::max() + previous)
                reject_chart_bar(i, "timestamp delta exceeds int64 range");
        }
    }
}
}  // namespace

bool BacktestEngine::set_account_currency_fx_series(
        const int64_t* timestamps_ms, const double* rates, int n) {
    guard_native_mutation("set_account_currency_fx_series");
    // Timestamped FX is not route-complete for the realtime scheduler. Reject
    // late installation as well as stream_begin-with-series so callers cannot
    // bypass fail-closed behavior by changing configuration after warmup.
    if (stream_phase_ == StreamPhase::REALTIME || stream_warmup_mode_) {
        return false;
    }
    if (n < 0 || (n > 0 && (!timestamps_ms || !rates))) return false;
    std::vector<int64_t> next_timestamps;
    std::vector<double> next_rates;
    if (n > 0) {
        next_timestamps.reserve(static_cast<std::size_t>(n));
        next_rates.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            if ((i > 0 && timestamps_ms[i] <= timestamps_ms[i - 1])
                || !std::isfinite(rates[i]) || rates[i] <= 0.0) {
                return false;
            }
            next_timestamps.push_back(timestamps_ms[i]);
            next_rates.push_back(rates[i]);
        }
    }
    if (!execution_consumer().stage_account_currency_fx_series(next_timestamps, next_rates))
        return false;
    account_currency_fx_timestamps_ = std::move(next_timestamps);
    account_currency_fx_rates_ = std::move(next_rates);
    return true;
}

double BacktestEngine::account_currency_fx_at(int64_t timestamp_ms) const {
    if (account_currency_fx_timestamps_.empty()) {
        return account_currency_fx_;
    }
    const auto it = std::upper_bound(
        account_currency_fx_timestamps_.begin(),
        account_currency_fx_timestamps_.end(), timestamp_ms);
    if (it == account_currency_fx_timestamps_.begin()) {
        return account_currency_fx_;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(account_currency_fx_timestamps_.begin(), it) - 1);
    return account_currency_fx_rates_[index];
}

double BacktestEngine::active_account_currency_fx() const {
    return account_currency_fx_at(current_bar_.timestamp);
}

// Reset all per-run STATE (not configuration) so a reused handle's run N is
// bit-identical to a fresh handle's run 1. See header doc + tests/
// test_handle_reuse_reset.cpp. Configuration fields (initial_capital_,
// pyramiding_, slippage_, commission_*, default_qty_*, syminfo_, inputs_, risk
// thresholds) are intentionally NOT touched — they are set before run().
void BacktestEngine::reset_run_state() {
    // Closed-trade list + cached P&L / count accumulators.
    trades_.clear();
    trades_.reserve(256);
    range_end_trades_.clear();
    net_profit_sum_ = 0.0;
    net_profit_roundoff_bound_ = 0.0;
    net_profit_roundoff_value_ = 0.0;
    gross_profit_sum_ = 0.0;
    gross_loss_sum_ = 0.0;
    win_trades_count_ = 0;
    loss_trades_count_ = 0;
    eventrades_count_ = 0;

    // Open position and request identities.
    reset_position_state_to_flat();   // position_side_/qty/price/time/count,
                                      // pyramid_entries_, trail, partial ids
    // Cycle ownership is scoped to this run, like order incarnations below.
    // A flat transition within a run must keep advancing it; only a new run
    // returns the allocator to its constructor value.
    next_position_cycle_seq_ = 1;
    // Request incarnations are report provenance scoped to one run.
    next_order_incarnation_ = 1;
    lot_excursion_hook_ = nullptr;
    fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();

    // Equity + position-size extremes.
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;
    max_drawdown_ = 0.0;
    max_runup_ = 0.0;
    max_contracts_held_all_ = 0.0;
    max_contracts_held_long_ = 0.0;
    max_contracts_held_short_ = 0.0;
    equity_curve_.clear();           // retain capacity (handle-reuse sweep pattern)
    bars_in_market_ = 0;
    first_bar_open_ = std::numeric_limits<double>::quiet_NaN();
    broker_state_hashes_.clear();    // ABI v4 task 6: retain capacity like equity_curve_
    last_script_continuation_hash_ = 0;
    last_script_continuation_valid_ = false;

    // Generic risk-adjacent lifecycle state.
    position_close_obligation_ = {};
    broker_fill_event_seq_ = 0;

    // Per-bar cursor + session-predicate state.
    bar_index_ = 0;
    prev_bar_timestamp_ = 0;
    // The chart's native daily partition is rebuilt per run by the
    // multi-timeframe run() (prepare_chart_day_partition); a run that never
    // builds one must not report a stale one.
    chart_day_partition_ = NativeDayPartition{};
    account_currency_fx_broker_epoch_initialized_ = false;
    account_currency_fx_broker_epoch_ = 0;
    account_currency_fx_broker_rate_ = account_currency_fx_;
    prev_in_session_ = false;
    session_ismarket_ = false;
    session_isfirstbar_ = false;
    session_islastbar_ = false;

    // A normal run starts a new lifecycle. stream_warmup_mode_ is deliberately
    // preserved: stream_begin sets it before delegating to run() so historical
    // bars are not mislabeled as the rightmost realtime bar.
    stream_phase_ = StreamPhase::IDLE;
    stream_input_tf_ms_ = 0;
    stream_next_input_open_ms_ = 0;
    stream_clock_ms_ = 0;
    stream_last_tick_ms_ = 0;
    stream_last_sequence_ = 0;
    stream_seen_sequence_ = false;
    stream_has_input_bar_ = false;
    stream_input_bar_ = Bar{};
    stream_last_price_ = 0.0;
    stream_has_last_price_ = false;
    stream_next_script_bar_index_ = 0;
    stream_script_bar_had_tick_ = false;
    stream_script_tick_seen_ = false;
    stream_input_mode_ = StreamInputMode::UNSET;
    stream_observe_actions_ = false;
    stream_action_sequence_ = 0;
    stream_order_actions_.clear();

    // Per-bar trace/diagnostic buffers (trace_enabled_ is config — preserved).
    if (trace_enabled_) {
        trace_buffer_.clear();                       // keep capacity for the next traced run
    } else {
        std::vector<TraceEntryC>().swap(trace_buffer_);  // release retained capacity
    }
    trace_names_.clear();
    trace_name_index_.clear();
}

bool BacktestEngine::chart_bar_ismarket(int64_t bar_ms) const {
    return pineforge::session_in_market(syminfo_.session, syminfo_.timezone,
                                            bar_ms, script_tf_);
}

// --- Input injection helpers ---
double BacktestEngine::get_input_double(const std::string& key, double default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        try { return std::stod(it->second); } catch (...) {}
    }
    return default_val;
}


int BacktestEngine::get_input_int(const std::string& key, int default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        try { return std::stoi(it->second); } catch (...) {}
    }
    return default_val;
}


int64_t BacktestEngine::get_input_int64(const std::string& key, int64_t default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        try { return std::stoll(it->second); } catch (...) {}
    }
    return default_val;
}


bool BacktestEngine::get_input_bool(const std::string& key, bool default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) {
        const std::string& v = it->second;
        if (v == "true" || v == "1") return true;
        if (v == "false" || v == "0") return false;
    }
    return default_val;
}


std::string BacktestEngine::get_input_string(const std::string& key, const std::string& default_val) const {
    auto it = inputs_.find(key);
    if (it != inputs_.end()) return it->second;
    return default_val;
}





// --- Full run() overload with SymInfo, StrategyOverrides, and input injection ---


}  // namespace pineforge
