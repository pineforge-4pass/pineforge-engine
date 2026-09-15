#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <limits>

namespace pineforge::source {

void PineStrategyHost::init_security_eval_states_for_run(
        const std::string& effective_input_tf) {
    security_next_input_ms_ = 0;
    security_calling_close_ms_ = 0;
    for (auto& state : security_eval_states_) {
        state.feed_count = 0;
        state.eval_complete_count = 0;
        state.eval_partial_count = 0;
        state.current_bar = Bar{};
        state.current_sub_bar_count = 0;
        state.lower_tf_sub_bar_index = 0;
        state.lower_tf_input_buffer.clear();
        state.first_bucket_published = false;
        state.deferred_aux.clear();
        state.slice_open_label = 0;
        state.last_published_label = 0;
        state.historical_projections.clear();
        state.historical_projection_cursor = 0;
        state.historical_projection_dispatched = false;
        state.native_feed_index = -1;
        state.native_bars_by_label.clear();
        state.aggregator = TimeframeAggregator();
        if (state.lower_tf_emulation || state.lower_tf_use_input) continue;
        const int ratio = tf_ratio(effective_input_tf, state.tf);
        if (ratio > 1 || ratio == -1) {
            state.aggregator = TimeframeAggregator(
                state.tf, effective_input_tf, syminfo_.timezone, syminfo_.session);
        }
        state.aggregator.set_early_close_completes(session_template_knows_early_close());
    }
}

void PineStrategyHost::prepare_historical_security_lookahead_projections(
        const Bar* input_bars, int n_input, const std::string& effective_input_tf) {
    clear_historical_security_lookahead_projections();
    const int input_seconds = tf_to_seconds(effective_input_tf);
    const int script_seconds = script_tf_seconds_;
    if (!historical_security_lookahead_projection_ || stream_warmup_mode_
        || effective_input_tf != script_tf_ || input_bars == nullptr || n_input <= 0
        || input_seconds <= 0 || script_seconds <= 0) {
        return;
    }

    historical_security_lookahead_projection_active_ = true;
    const std::int64_t input_ms = static_cast<std::int64_t>(input_seconds) * 1000;
    for (auto& state : security_eval_states_) {
        const int requested_seconds = tf_to_seconds(state.tf);
        const bool calendar_month = requested_seconds == -1
            && calendar_period_for(state.tf) == CalendarPeriod::MONTH;
        const bool eligible = !state.lower_tf_requested && !state.lower_tf_emulation
            && !state.lower_tf_use_input && state.lookahead_on && !state.gaps_on
            && !state.heikinashi
            && (calendar_month || requested_seconds > script_seconds);
        if (!eligible) continue;

        auto child_instant_ms = [&](int child) -> std::int64_t {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
            if (aux_security_feed_enabled()) {
                const std::size_t index = static_cast<std::size_t>(child);
                if (index < aux_security_chart_begin_.size()
                    && aux_security_chart_begin_[index] < aux_security_bars_.size()) {
                    return aux_security_bars_[aux_security_chart_begin_[index]].timestamp;
                }
            }
#endif
            return input_bars[child].timestamp;
        };
        int projection_begin = 0;
        while (projection_begin < n_input
               && security_input_precedes_range_start(
                   state, child_instant_ms(projection_begin))) {
            ++projection_begin;
        }
        if (projection_begin >= n_input) continue;
        const int projection_count = n_input - projection_begin;
        const int expected_children = std::max(
            1, (calendar_month ? 31 * 86400 : requested_seconds) / input_seconds);
        state.historical_projections.reserve(
            static_cast<std::size_t>(projection_count / expected_children + 1));
        state.historical_projection_cursor = 0;
        state.historical_projection_dispatched = false;

        auto crosses_requested_boundary = [&](std::int64_t from_ms, std::int64_t to_ms) {
            if (from_ms != 0 && to_ms != 0) {
                if (state.aggregator.has_native_periods())
                    return state.aggregator.period_changes(from_ms, to_ms);
                return tf_change(from_ms, to_ms, state.tf,
                                 syminfo_.timezone, syminfo_.session);
            }
            const std::int64_t requested_ms =
                static_cast<std::int64_t>(requested_seconds) * 1000;
            return requested_ms > 0 && from_ms / requested_ms != to_ms / requested_ms;
        };
        auto merge = [](Bar& aggregate, const Bar& child) {
            aggregate.high = std::max(aggregate.high, child.high);
            aggregate.low = std::min(aggregate.low, child.low);
            aggregate.close = child.close;
            aggregate.volume += child.volume;
        };
        auto publish_group = [&](int begin, const Bar& aggregate, bool complete) {
            state.historical_projections.push_back(
                HistoricalSecurityProjection{aggregate, child_instant_ms(begin), complete});
        };

        int group_begin = projection_begin;
        Bar aggregate = input_bars[projection_begin];
        for (int i = projection_begin + 1; i < n_input; ++i) {
            if (crosses_requested_boundary(aggregate.timestamp, input_bars[i].timestamp)) {
                publish_group(group_begin, aggregate, true);
                group_begin = i;
                aggregate = input_bars[i];
            } else {
                merge(aggregate, input_bars[i]);
            }
        }
        bool final_complete = false;
        const std::int64_t last_timestamp = input_bars[n_input - 1].timestamp;
        if (last_timestamp <= std::numeric_limits<std::int64_t>::max() - input_ms)
            final_complete = crosses_requested_boundary(last_timestamp, last_timestamp + input_ms);
        publish_group(group_begin, aggregate, final_complete);
    }
}

void PineStrategyHost::clear_historical_security_lookahead_projections() {
    historical_security_lookahead_projection_active_ = false;
    for (auto& state : security_eval_states_) {
        state.historical_projections.clear();
        state.historical_projection_cursor = 0;
        state.historical_projection_dispatched = false;
    }
}

} // namespace pineforge::source
