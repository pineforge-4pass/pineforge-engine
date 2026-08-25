/*
 * engine_aux_security.cpp — native-chart / request.security feed separation
 */

#include "engine_internal.hpp"

#include <pineforge/ta.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pineforge {

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1

bool BacktestEngine::set_aux_security_feed(const Bar* bars, int n,
                                           const std::string& input_tf) {
    if (n == 0) {
        aux_security_bars_.clear();
        aux_security_input_tf_.clear();
        clear_aux_security_chart_ranges();
        return true;
    }
    if (n < 0 || bars == nullptr || input_tf.empty()) {
        last_error_ =
            "auxiliary request.security feed requires bars, a positive count, and input_tf";
        return false;
    }
    int seconds = 0;
    try {
        seconds = tf_to_seconds(input_tf);
    } catch (...) {
        seconds = 0;
    }
    if (seconds <= 0) {
        last_error_ =
            "auxiliary request.security feed requires a fixed positive input_tf";
        return false;
    }
    for (int i = 1; i < n; ++i) {
        if (bars[i].timestamp <= bars[i - 1].timestamp) {
            last_error_ =
                "auxiliary request.security feed timestamps must be strictly increasing";
            return false;
        }
    }
    aux_security_bars_.assign(bars, bars + n);
    aux_security_input_tf_ = input_tf;
    clear_aux_security_chart_ranges();
    last_error_.clear();
    return true;
}


void BacktestEngine::clear_aux_security_chart_ranges() {
    aux_security_chart_begin_.clear();
    aux_security_chart_end_.clear();
}


void BacktestEngine::prepare_aux_security_chart_ranges(
        const Bar* chart_bars, int n_chart, const std::string& chart_tf) {
    clear_aux_security_chart_ranges();
    if (!aux_security_feed_enabled()) return;
    if (chart_bars == nullptr || n_chart <= 0) {
        throw std::runtime_error(
            "auxiliary request.security feed requires at least one native chart bar");
    }

    int chart_seconds = 0;
    int aux_seconds = 0;
    try {
        chart_seconds = tf_to_seconds(chart_tf);
        aux_seconds = tf_to_seconds(aux_security_input_tf_);
    } catch (...) {
        chart_seconds = 0;
        aux_seconds = 0;
    }
    if (chart_seconds <= 0 || aux_seconds <= 0
        || aux_seconds >= chart_seconds) {
        throw std::runtime_error(
            "auxiliary request.security feed input_tf must be fixed and strictly finer than the native chart timeframe");
    }
    if (chart_seconds % aux_seconds != 0) {
        throw std::runtime_error(
            "auxiliary request.security feed input_tf must evenly divide the native chart timeframe");
    }
    for (int i = 1; i < n_chart; ++i) {
        if (chart_bars[i].timestamp <= chart_bars[i - 1].timestamp) {
            throw std::runtime_error(
                "native chart feed timestamps must be strictly increasing with an auxiliary security feed");
        }
    }

    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    aux_security_chart_begin_.assign(static_cast<std::size_t>(n_chart), missing);
    aux_security_chart_end_.assign(static_cast<std::size_t>(n_chart), missing);
    TimeframeAggregator chart_router(chart_tf, aux_security_input_tf_,
                                     syminfo_.timezone, syminfo_.session);
    const int64_t first_chart_label = chart_bars[0].timestamp;
    const int64_t last_chart_label = chart_bars[n_chart - 1].timestamp;
    std::size_t chart_index = 0;
    for (std::size_t aux_index = 0; aux_index < aux_security_bars_.size();
         ++aux_index) {
        const int64_t label = chart_router.bar_label_ms(
            aux_security_bars_[aux_index].timestamp);
        // Evidence feeds may intentionally cover a wider history than the
        // native chart tape. Those leading/trailing buckets are inert. Once a
        // label enters the native span, however, it must match an actual chart
        // bar exactly; silently skipping an interior hole would shift security
        // state across the chart matrix.
        if (label < first_chart_label || label > last_chart_label) {
            continue;
        }
        while (chart_index + 1 < static_cast<std::size_t>(n_chart)
               && chart_bars[chart_index].timestamp < label) {
            ++chart_index;
        }
        if (chart_bars[chart_index].timestamp != label) {
            throw std::runtime_error(
                "auxiliary request.security bar does not map to a native chart bar");
        }
        if (aux_security_chart_begin_[chart_index] == missing) {
            aux_security_chart_begin_[chart_index] = aux_index;
        }
        aux_security_chart_end_[chart_index] = aux_index + 1;
    }
    for (int i = 0; i < n_chart; ++i) {
        if (aux_security_chart_begin_[static_cast<std::size_t>(i)] == missing) {
            throw std::runtime_error(
                "native chart bar has no matching auxiliary request.security bars");
        }
    }
}


void BacktestEngine::feed_aux_security_for_chart_bar(int chart_index) {
    const std::size_t idx = static_cast<std::size_t>(chart_index);
    if (idx >= aux_security_chart_begin_.size()
        || idx >= aux_security_chart_end_.size()) {
        throw std::runtime_error(
            "auxiliary request.security chart routing is not initialized");
    }
    const std::size_t begin = aux_security_chart_begin_[idx];
    const std::size_t end = aux_security_chart_end_[idx];

    // Lower-TF arrays belong to the native chart slice, not a raw UTC bucket.
    // Accumulate the entire symbol-clock-aligned slice here and publish it only
    // after its final auxiliary bar. This keeps an overnight 17:00-17:00 daily
    // bar as one array even though its raw timestamps cross UTC midnight.
    for (std::size_t i = begin; i < end; ++i) {
        const Bar& aux_bar = aux_security_bars_[i];
        const bool calling_bar_complete = (i + 1 == end);
        for (auto& state : security_eval_states_) {
            if (!state.lower_tf_array_requested) {
                feed_security_at_calling_bar_boundary(
                    state, aux_bar, calling_bar_complete);
                continue;
            }
            if (security_input_precedes_range_start(state, aux_bar.timestamp)) {
                continue;
            }
            if (state.lower_tf_use_input) {
                state.lower_tf_input_buffer.push_back(aux_bar);
            } else if (state.lower_tf_emulation) {
                std::vector<Bar> synthetic = internal::synthesize_lower_tf_bars(
                    aux_bar, state.lower_tf_ratio, state.lower_tf_seconds);
                if (synthetic.empty()) {
                    throw std::runtime_error(
                        "request.security_lower_tf could not synthesize auxiliary sub-bars");
                }
                state.lower_tf_input_buffer.insert(
                    state.lower_tf_input_buffer.end(),
                    synthetic.begin(), synthetic.end());
            } else {
                throw std::runtime_error(
                    "request.security_lower_tf auxiliary routing is not initialized");
            }
        }
    }

    struct SecurityNaWarmupScope {
        bool previous;
        explicit SecurityNaWarmupScope(bool enabled)
            : previous(ta::ema_na_warmup_flag()) {
            ta::ema_na_warmup_flag() = enabled;
        }
        ~SecurityNaWarmupScope() { ta::ema_na_warmup_flag() = previous; }
    } warmup_scope(security_range_start_na_warmup_);

    for (auto& state : security_eval_states_) {
        if (!state.lower_tf_array_requested) continue;
        int aggregate_ratio = state.lower_tf_emulation
            ? 1 : state.lower_tf_input_aggregation_ratio;
        if (aggregate_ratio < 1) aggregate_ratio = 1;
        const int count = static_cast<int>(state.lower_tf_input_buffer.size());
        std::vector<Bar> requested_bars;
        requested_bars.reserve(
            static_cast<std::size_t>(count / aggregate_ratio + 1));
        if (aggregate_ratio == 1) {
            requested_bars.assign(state.lower_tf_input_buffer.begin(),
                                  state.lower_tf_input_buffer.end());
        } else {
            for (int i = 0; i + aggregate_ratio <= count;
                 i += aggregate_ratio) {
                Bar aggregate = state.lower_tf_input_buffer[
                    static_cast<std::size_t>(i)];
                double volume = aggregate.volume;
                for (int j = 1; j < aggregate_ratio; ++j) {
                    const Bar& next = state.lower_tf_input_buffer[
                        static_cast<std::size_t>(i + j)];
                    aggregate.high = std::max(aggregate.high, next.high);
                    aggregate.low = std::min(aggregate.low, next.low);
                    aggregate.close = next.close;
                    volume += next.volume;
                }
                aggregate.volume = volume;
                requested_bars.push_back(aggregate);
            }
        }

        state.lower_tf_sub_bar_index = 0;
        for (const Bar& bar : requested_bars) {
            state.feed_count++;
            state.current_bar = bar;
            state.current_sub_bar_count = 1;
            state.eval_complete_count++;
            evaluate_security(state.sec_id, bar, true);
            state.lower_tf_sub_bar_index++;
        }
        state.lower_tf_input_buffer.clear();
    }
}

#endif  // PINEFORGE_HAS_AUX_SECURITY_FEED_V1

}  // namespace pineforge
