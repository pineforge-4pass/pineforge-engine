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
        PineSecurityEvalState& pine = pine_security_state(state.sec_id);
        pine.lower_tf_sub_bar_index = 0;
        pine.lower_tf_input_buffer.clear();
        pine.first_bucket_published = false;
        pine.deferred_aux.clear();
        pine.slice_open_label = 0;
        pine.last_published_label = 0;
        pine.historical_projections.clear();
        pine.historical_projection_cursor = 0;
        pine.historical_projection_dispatched = false;
        state.native_feed_index = -1;
        state.native_bars_by_label.clear();
        state.aggregator = TimeframeAggregator();
        if (pine.lower_tf_emulation || pine.lower_tf_use_input) continue;
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
        PineSecurityEvalState& pine = pine_security_state(state.sec_id);
        const bool eligible = !pine.lower_tf_requested && !pine.lower_tf_emulation
            && !pine.lower_tf_use_input && pine.lookahead_on && !pine.gaps_on
            && !pine.heikinashi
            && (calendar_month || requested_seconds > script_seconds);
        if (!eligible) continue;

        int projection_begin = 0;
        while (projection_begin < n_input
               && security_input_precedes_range_start(
                   state, historical_security_child_ms(input_bars, projection_begin))) {
            ++projection_begin;
        }
        if (projection_begin >= n_input) continue;
        pine.historical_projection_cursor = 0;
        pine.historical_projection_dispatched = false;

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
        std::size_t count = 1;
        std::int64_t group_open = input_bars[projection_begin].timestamp;
        for (int i = projection_begin + 1; i < n_input; ++i) {
            if (crosses_requested_boundary(group_open, input_bars[i].timestamp)) {
                ++count;
                group_open = input_bars[i].timestamp;
            }
        }
        auto& cursor = pine.historical_projections;
        cursor.input = input_bars;
        cursor.input_count = n_input;
        cursor.sec_id = state.sec_id;
        cursor.requested_seconds = requested_seconds;
        cursor.input_ms = input_ms;
        cursor.partition = active_native_day_partition();
        cursor.bind(this,
            [](const void* owner, const HistoricalSecurityProjectionCursor& view, int begin) {
                return static_cast<const PineStrategyHost*>(owner)
                    ->read_historical_security_projection(view, begin);
            },
            [](const void* owner, const Bar* input, int child) {
                return static_cast<const PineStrategyHost*>(owner)
                    ->historical_security_child_ms(input, child);
            },
            projection_begin, count);
    }
}

std::int64_t PineStrategyHost::historical_security_child_ms(const Bar* input, int child) const {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) {
        const std::size_t index = static_cast<std::size_t>(child);
        if (index < aux_security_chart_begin_.size()
            && aux_security_chart_begin_[index] < aux_security_bars_.size()) {
            return aux_security_bars_[aux_security_chart_begin_[index]].timestamp;
        }
    }
#endif
    return input[child].timestamp;
}

HistoricalSecurityProjectionCursor::Step PineStrategyHost::read_historical_security_projection(
        const HistoricalSecurityProjectionCursor& cursor, int begin) const {
    NativeDayPartitionScope partition_scope(cursor.partition);
    const auto found = std::find_if(security_eval_states_.begin(), security_eval_states_.end(),
        [&](const SecurityEvalState& state) { return state.sec_id == cursor.sec_id; });
    const auto& state = *found;
    const auto crosses = [&](std::int64_t from, std::int64_t to) {
        if (from != 0 && to != 0) {
            if (state.aggregator.has_native_periods())
                return state.aggregator.period_changes(from, to);
            return tf_change(from, to, state.tf, syminfo_.timezone, syminfo_.session);
        }
        const auto width = static_cast<std::int64_t>(cursor.requested_seconds) * 1000;
        return width > 0 && from / width != to / width;
    };
    Bar aggregate = cursor.input[begin];
    int next = begin + 1;
    while (next < cursor.input_count
           && !crosses(aggregate.timestamp, cursor.input[next].timestamp)) {
        const auto& child = cursor.input[next++];
        aggregate.high = std::max(aggregate.high, child.high);
        aggregate.low = std::min(aggregate.low, child.low);
        aggregate.close = child.close;
        aggregate.volume += child.volume;
    }
    bool complete = next < cursor.input_count;
    if (!complete) {
        const auto last = cursor.input[cursor.input_count - 1].timestamp;
        if (last <= std::numeric_limits<std::int64_t>::max() - cursor.input_ms)
            complete = crosses(last, last + cursor.input_ms);
    }
    return {{aggregate, historical_security_child_ms(cursor.input, begin), complete}, next};
}

void PineStrategyHost::clear_historical_security_lookahead_projections() {
    historical_security_lookahead_projection_active_ = false;
    for (auto& entry : pine_security_states_) {
        entry.second.historical_projections.clear();
        entry.second.historical_projection_cursor = 0;
        entry.second.historical_projection_dispatched = false;
    }
}

} // namespace pineforge::source
