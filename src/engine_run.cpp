/*
 * engine_run.cpp — public run() entrypoints + run_magnified_bar + get_input_*
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;


// open_trade_* accessors moved to engine_trade_accessors.cpp.
void BacktestEngine::run(const Bar* bars, int n) {
    last_error_.clear();
    try {
    trades_.reserve(256);
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;

    // Runtime diagnostics (single-timeframe path)
    diag_input_bars_processed_ = n;
    diag_script_bars_processed_ = 0;
    diag_magnifier_sub_bars_processed_ = 0;
    diag_magnifier_sample_ticks_processed_ = 0;
    diag_script_tf_ratio_ = 1;
    diag_needs_aggregation_ = false;
    bar_magnifier_enabled_ = false;
    for (auto& state : security_eval_states_) {
        state.feed_count = 0;
        state.eval_complete_count = 0;
        state.eval_partial_count = 0;
        state.current_bar = Bar{};
        state.current_sub_bar_count = 0;
    }

    for (int i = 0; i < n; i++) {
        current_bar_ = bars[i];
        bar_index_ = i;
        is_first_tick_ = true;
        is_last_tick_ = true;
        barstate_islast_ = (i == n - 1);
        diag_script_bars_processed_++;
        // Reset per-bar pending-close accumulator. Each on_bar call
        // captures fresh ``strategy.close*`` qty for the same-bar
        // close-then-entry source-order rule (see engine.hpp).
        pending_close_qty_in_bar_ = 0.0;
        if (process_orders_on_close_) {
            // TradingView process_orders_on_close semantics:
            //   1. Evaluate existing stop/limit orders from previous bars
            //   2. Update per-trade extremes so on_bar reads current values
            //   3. Strategy logic runs at bar close (creates new orders)
            //   4. New market orders fill at bar.close; new stop/limit wait for next bar
            process_pending_orders(current_bar_);   // step 1: old stop/limit
            update_per_trade_extremes();            // step 2: update before strategy reads
            on_bar(current_bar_);                   // step 3: strategy logic
            process_pending_orders(current_bar_);   // step 4: new market orders
        } else {
            process_pending_orders(current_bar_);
            update_per_trade_extremes();
            on_bar(current_bar_);
        }
        update_equity_extremes();
        prev_bar_timestamp_ = current_bar_.timestamp;
    }
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}


// --- run_magnified_bar ---
void BacktestEngine::run_magnified_bar(const std::vector<Bar>& sub_bars) {
    if (sub_bars.empty()) return;

    double bar_open = sub_bars.front().open;
    double running_high = sub_bars.front().open;
    double running_low = sub_bars.front().open;
    double cumulative_vol = 0.0;
    int64_t timestamp = sub_bars.front().timestamp;

    int total_sub = (int)sub_bars.size();
    diag_magnifier_sub_bars_processed_ += total_sub;

    // Precompute per-script-bar mean volume so volume-weighted sampling can
    // scale each sub-bar's tick count relative to the local average.
    double mean_vol = 0.0;
    if (magnifier_volume_weighted_ && total_sub > 0) {
        double sum_vol = 0.0;
        for (const Bar& sb : sub_bars) sum_vol += sb.volume;
        mean_vol = sum_vol / total_sub;
    }

    for (int si = 0; si < total_sub; ++si) {
        const Bar& sb = sub_bars[si];
        cumulative_vol += sb.volume;
        timestamp = sb.timestamp;

        // Feed security evaluators with each sub-bar
        for (auto& state : security_eval_states_) {
            feed_security_eval_state(state, sb);
        }

        std::vector<double> samples =
            magnifier_volume_weighted_
                ? sample_price_path_volume_weighted(sb, magnifier_samples_, mean_vol,
                                                    /*min_samples=*/2,
                                                    /*max_samples=*/std::max(magnifier_samples_ * 4, 8),
                                                    magnifier_dist_)
                : sample_price_path(sb, magnifier_samples_, magnifier_dist_);
        int n_samples = (int)samples.size();
        diag_magnifier_sample_ticks_processed_ += n_samples;

        for (int pi = 0; pi < n_samples; ++pi) {
            double price = samples[pi];
            running_high = std::max(running_high, price);
            running_low = std::min(running_low, price);

            current_bar_.open = bar_open;
            current_bar_.high = running_high;
            current_bar_.low = running_low;
            current_bar_.close = price;
            current_bar_.volume = cumulative_vol;
            current_bar_.timestamp = timestamp;

            is_first_tick_ = (si == 0 && pi == 0);
            is_last_tick_ = (si == total_sub - 1 && pi == n_samples - 1);

            if (process_orders_on_close_) {
                process_pending_orders(current_bar_);
                update_per_trade_extremes();
                if (is_last_tick_) {
                    // Force is_first_tick_ true so that on_bar advances the series history.
                    is_first_tick_ = true;
                    on_bar(current_bar_);
                    process_pending_orders(current_bar_);
                }
            } else {
                process_pending_orders(current_bar_);
                update_per_trade_extremes();
                if (is_last_tick_) {
                    // Force is_first_tick_ true so that on_bar advances the series history.
                    is_first_tick_ = true;
                    on_bar(current_bar_);
                }
            }
        }
    }
    finalize_bar();
}


// --- New run() overload with full parameter set ---
void BacktestEngine::run(const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) {
    last_error_.clear();
    try {
    // Auto-detect input_tf from bar timestamps if not provided
    std::string effective_input_tf = input_tf;
    if (effective_input_tf.empty() && n_input >= 2) {
        effective_input_tf = detect_timeframe(input_bars, n_input);
    }
    // script_tf defaults to input_tf if not provided (strategy runs on the data's timeframe)
    std::string effective_script_tf = script_tf.empty() ? effective_input_tf : script_tf;

    // Store parameters
    input_tf_ = effective_input_tf;
    script_tf_ = effective_script_tf;
    bar_magnifier_enabled_ = bar_magnifier;
    magnifier_samples_ = magnifier_samples;
    magnifier_dist_ = magnifier_dist;

    configure_security_evaluators();

    // Runtime diagnostics baseline
    diag_input_bars_processed_ = n_input;
    diag_script_bars_processed_ = 0;
    diag_magnifier_sub_bars_processed_ = 0;
    diag_magnifier_sample_ticks_processed_ = 0;

    trades_.reserve(256);
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;

    // Determine aggregation ratio for script TF
    int ratio = tf_ratio(effective_input_tf, effective_script_tf);
    if (ratio == -2 && !effective_input_tf.empty() && !effective_script_tf.empty()) {
        throw std::runtime_error(
            "script timeframe must be coarser than or equal to input timeframe: requested script_tf "
            + effective_script_tf + " from input timeframe " + effective_input_tf
        );
    }
    bool needs_aggregation = (ratio > 1 || ratio == -1);
    diag_script_tf_ratio_ = ratio;
    diag_needs_aggregation_ = needs_aggregation;

    // Initialize script TF aggregator
    if (needs_aggregation) {
        // Use a single timeframe-constructor path so script timeframe boundaries
        // follow the same wall-clock/calendar semantics as request.security.
        script_tf_agg_ = TimeframeAggregator(effective_script_tf, effective_input_tf);
    } else {
        script_tf_agg_ = TimeframeAggregator();  // passthrough
    }

    int expected_script_bars =
        count_expected_script_bars(input_bars, n_input, needs_aggregation);

    validate_security_timeframes(effective_input_tf);

    init_security_eval_states_for_run(effective_input_tf);

    if (!needs_aggregation && !bar_magnifier) {
        run_simple_bar_loop(input_bars, n_input);
    } else {
        run_aggregation_bar_loop(input_bars, n_input, bar_magnifier,
                                 expected_script_bars);
    }
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}


// Preview pass: when aggregating to a coarser script TF, count how many
// completed script-TF bars the input feed will produce so the simple-loop /
// aggregation-loop can flag the final bar with barstate.islast at the right
// moment. Returns n_input verbatim when no aggregation is needed.
int BacktestEngine::count_expected_script_bars(const Bar* input_bars, int n_input,
                                                bool needs_aggregation) const {
    if (!needs_aggregation) return n_input;
    TimeframeAggregator preview_agg(script_tf_, input_tf_);
    int count = 0;
    for (int i = 0; i < n_input; ++i) {
        AggregatedBar preview = preview_agg.feed(input_bars[i]);
        if (preview.is_complete) {
            ++count;
        }
    }
    return count;
}


// Lazily reset per-run security evaluator state and (re)construct the
// aggregator each evaluator uses based on its requested TF vs. input_tf.
// Lower-TF emulation evaluators keep their default-constructed (passthrough)
// aggregator since their per-sub-bar synthesis is driven elsewhere.
void BacktestEngine::init_security_eval_states_for_run(
    const std::string& effective_input_tf) {
    for (auto& state : security_eval_states_) {
        state.feed_count = 0;
        state.eval_complete_count = 0;
        state.eval_partial_count = 0;
        state.current_bar = Bar{};
        state.current_sub_bar_count = 0;
        state.lower_tf_sub_bar_index = 0;
        state.lower_tf_input_buffer.clear();
        state.aggregator = TimeframeAggregator();
        if (state.lower_tf_emulation || state.lower_tf_use_input) {
            continue;
        }
        int req_ratio = tf_ratio(effective_input_tf, state.tf);
        if (req_ratio > 1) {
            state.aggregator = TimeframeAggregator(state.tf, effective_input_tf);
        } else if (req_ratio == -1) {
            state.aggregator = TimeframeAggregator(state.tf, effective_input_tf);
        }
    }
}


// Bar pump for the no-aggregation, no-magnifier case: every input bar is a
// script bar, fed straight to on_bar with the standard
// pending-orders / per-trade-extremes / on_bar / equity-update sequence
// (with the process_orders_on_close TV variant when configured).
void BacktestEngine::run_simple_bar_loop(const Bar* input_bars, int n_input) {
    for (int i = 0; i < n_input; ++i) {
        current_bar_ = input_bars[i];
        bar_index_ = i;
        is_first_tick_ = true;
        is_last_tick_ = true;
        barstate_islast_ = (i == n_input - 1);
        diag_script_bars_processed_++;

        // Feed security evaluators
        for (auto& state : security_eval_states_) {
            feed_security_eval_state(state, input_bars[i]);
        }

        if (process_orders_on_close_) {
            process_pending_orders(current_bar_);
            update_per_trade_extremes();
            on_bar(current_bar_);
            process_pending_orders(current_bar_);
        } else {
            process_pending_orders(current_bar_);
            update_per_trade_extremes();
            on_bar(current_bar_);
        }
        update_equity_extremes();
        prev_bar_timestamp_ = current_bar_.timestamp;
    }
}


// Bar pump for the aggregation path (with or without magnifier). Feeds each
// input bar through ``script_tf_agg_`` and either dispatches each completed
// script bar straight to on_bar (no magnifier) or hands the collected
// sub-bars to ``run_magnified_bar`` for price-path sampling. Security
// evaluators are fed per input bar in the non-magnifier case and deferred
// to ``run_magnified_bar`` in the magnifier case so each sub-bar runs
// exactly once before its sampled ticks.
void BacktestEngine::run_aggregation_bar_loop(const Bar* input_bars, int n_input,
                                                bool bar_magnifier,
                                                int expected_script_bars) {
    std::vector<Bar> group_sub_bars;
    int script_bar_index = 0;
    int emitted_script_bars = 0;

    for (int i = 0; i < n_input; ++i) {
        if (!bar_magnifier) {
            for (auto& state : security_eval_states_) {
                feed_security_eval_state(state, input_bars[i]);
            }
        }

        AggregatedBar ab = script_tf_agg_.feed(input_bars[i]);

        if (bar_magnifier) {
            group_sub_bars.push_back(input_bars[i]);
        }

        if (ab.is_complete) {
            bar_index_ = script_bar_index++;
            emitted_script_bars++;
            barstate_islast_ = (emitted_script_bars == expected_script_bars);
            diag_script_bars_processed_++;

            if (bar_magnifier && !group_sub_bars.empty()) {
                // Magnifier mode: process sub-bars with price path sampling
                run_magnified_bar(group_sub_bars);
                group_sub_bars.clear();
            } else {
                // No magnifier: use aggregated bar directly
                current_bar_ = ab.bar;
                if (process_orders_on_close_) {
                    process_pending_orders(current_bar_);
                    update_per_trade_extremes();
                    on_bar(current_bar_);
                    process_pending_orders(current_bar_);
                } else {
                    process_pending_orders(current_bar_);
                    update_per_trade_extremes();
                    on_bar(current_bar_);
                }
            }
            update_equity_extremes();
            prev_bar_timestamp_ = current_bar_.timestamp;
        }
    }
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
void BacktestEngine::run(const Bar* input_bars, int n_input,
                          const std::string& input_tf,
                          const std::string& script_tf,
                          const std::unordered_map<std::string, std::string>& inputs,
                          const SymInfo& syminfo,
                          const StrategyOverrides* overrides,
                          bool bar_magnifier,
                          int magnifier_samples,
                          MagnifierDistribution magnifier_dist) {
    last_error_.clear();
    try {
    // Store syminfo and inputs
    syminfo_ = syminfo;
    syminfo_mintick_ = syminfo.mintick;
    inputs_ = inputs;

    // Apply overrides
    if (overrides) {
        if (!std::isnan(overrides->initial_capital))
            initial_capital_ = overrides->initial_capital;
        if (overrides->pyramiding >= 0)
            pyramiding_ = overrides->pyramiding;
        if (overrides->slippage >= 0)
            slippage_ = overrides->slippage;
        if (!std::isnan(overrides->commission_value))
            commission_value_ = overrides->commission_value;
        if (overrides->commission_type >= 0)
            commission_type_ = static_cast<CommissionType>(overrides->commission_type);
        if (!std::isnan(overrides->default_qty_value))
            default_qty_value_ = overrides->default_qty_value;
        if (overrides->default_qty_type >= 0)
            default_qty_type_ = static_cast<QtyType>(overrides->default_qty_type);
        if (overrides->process_orders_on_close >= 0)
            process_orders_on_close_ = (overrides->process_orders_on_close != 0);
        if (overrides->close_entries_rule >= 0)
            close_entries_rule_any_ = (overrides->close_entries_rule != 0);
    }

    // Delegate to the TF-aware run
    run(input_bars, n_input, input_tf, script_tf, bar_magnifier, magnifier_samples, magnifier_dist);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::run";
    }
}

}  // namespace pineforge
