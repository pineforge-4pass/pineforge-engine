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

bool PineStrategyHost::uses_historical_security_lookahead_projection(
        const SecurityEvalState& state) const {
    const PineSecurityEvalState& pine = pine_security_state(state.sec_id);
    // Historical-only projection prevents forming-bar future leaks in
    // realtime and suppressed probe tails for both registration paths.
    const bool metadata_historical_path = historical_security_lookahead_projection_
        && !stream_warmup_mode_ && !realtime_tail_ && !probe_suppress_tail_logic_;
    // The per-site selector's live/probe behavior remains progressive.
    const bool per_site_historical_path = pine.historical_lookahead_projection
        && !stream_warmup_mode_ && !realtime_tail_ && !probe_suppress_tail_logic_;
    if (!historical_security_lookahead_projection_active_
        || !(metadata_historical_path || per_site_historical_path)
        || pine.lower_tf_requested || pine.lower_tf_emulation
        || pine.lower_tf_use_input || !pine.lookahead_on || pine.gaps_on
        || pine.heikinashi) {
        return false;
    }
    const int requested_seconds = tf_to_seconds(state.tf);
    return requested_seconds > script_tf_seconds_
        || (requested_seconds == -1
            && calendar_period_for(state.tf) == CalendarPeriod::MONTH);
}

void PineStrategyHost::prepare_historical_security_lookahead_projections(
        const Bar* input_bars, int n_input, const std::string& effective_input_tf) {
    clear_historical_security_lookahead_projections();
    const int input_seconds = tf_to_seconds(effective_input_tf);
    const int script_seconds = script_tf_seconds_;
    const bool metadata_historical_requested = historical_security_lookahead_projection_
        && !stream_warmup_mode_ && !realtime_tail_ && !probe_suppress_tail_logic_;
    const bool per_site_historical_requested = !stream_warmup_mode_
        && !realtime_tail_ && !probe_suppress_tail_logic_
        && std::any_of(pine_security_states_.begin(), pine_security_states_.end(),
            [](const auto& site) { return site.second.historical_lookahead_projection; });
    if (!(metadata_historical_requested || per_site_historical_requested)
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
        if (!uses_historical_security_lookahead_projection(state)) continue;
        PineSecurityEvalState& pine = pine_security_state(state.sec_id);

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
        pine.historical_projections.reserve(
            static_cast<std::size_t>(projection_count / expected_children + 1));
        pine.historical_projection_cursor = 0;
        pine.historical_projection_dispatched = false;

        auto crosses_requested_boundary = [&](std::int64_t from_ms, std::int64_t to_ms) {
            if (from_ms != 0 && to_ms != 0) {
                if (state.aggregator.has_native_periods())
                    return state.aggregator.period_changes(from_ms, to_ms);
                return tf_change(from_ms, to_ms, state.tf,
                                 syminfo_.timezone, syminfo_.session);
            }
            if (pine.historical_lookahead_projection) {
                // Both instants come from actual inputs; zero is a valid
                // Unix timestamp, not tf_change's unknown-time sentinel.
                if (state.aggregator.has_native_periods()
                    || state.aggregator.calendar_period() != CalendarPeriod::NONE)
                    return state.aggregator.period_changes(from_ms, to_ms);
                return state.aggregator.bucket_open_ms(from_ms)
                    != state.aggregator.bucket_open_ms(to_ms);
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
            pine.historical_projections.push_back(
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
    for (auto& entry : pine_security_states_) {
        entry.second.historical_projections.clear();
        entry.second.historical_projection_cursor = 0;
        entry.second.historical_projection_dispatched = false;
    }
}

} // namespace pineforge::source
