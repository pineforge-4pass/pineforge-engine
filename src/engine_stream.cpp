/*
 * engine_stream.cpp — continuous historical warmup -> realtime trade stream
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace pineforge {

namespace {

Bar price_point(double price, double volume, int64_t timestamp) {
    return Bar{price, price, price, price, volume, timestamp};
}

}  // namespace













bool BacktestEngine::stream_finalize_until(int64_t timestamp_ms) {
    while (timestamp_ms >= stream_next_input_open_ms_ + stream_input_tf_ms_) {
        const bool had_tick = stream_has_input_bar_;
        // The raw time-of-day session test (namespace-scope form), not the
        // chart-bar rule BacktestEngine::pine_session_ismarket applies: this
        // decides whether a tick-less INPUT interval is a closed market that
        // must not become a synthetic bar, and stays byte-identical on
        // daily-or-higher feeds too.
        const bool in_session = pineforge::pine_session_ismarket(
            syminfo_.session, syminfo_.timezone,
            stream_next_input_open_ms_);

        // A normalized provider may jump from one market session to the next.
        // Do not turn the closed interval into synthetic tradable bars. A real
        // source record is still honored even if the configured metadata is
        // imperfect, so provider data remains authoritative.
        if (!had_tick && !in_session) {
            stream_input_bar_ = Bar{};
            stream_next_input_open_ms_ += stream_input_tf_ms_;
            continue;
        }

        Bar completed;
        if (had_tick) {
            completed = stream_input_bar_;
        } else {
            if (!stream_has_last_price_) {
                last_error_ = "stream cannot synthesize a gap before any price";
                return false;
            }
            completed = price_point(
                stream_last_price_, 0.0, stream_next_input_open_ms_);
        }

        stream_feed_input_bar(completed, had_tick);
        stream_has_input_bar_ = false;
        stream_input_bar_ = Bar{};
        stream_next_input_open_ms_ += stream_input_tf_ms_;
    }
    return true;
}

void BacktestEngine::stream_feed_input_bar(const Bar& bar, bool had_tick) {
    ++diag_input_bars_processed_;
    last_bar_time_ = bar.timestamp;

    if (!diag_needs_aggregation_) {
        for (auto& state : security_eval_states_) {
            feed_security_eval_state(state, bar);
        }
        dispatch_source_stream_script_bar(bar, had_tick);
        return;
    }

    // Feed the chart aggregator first so finer lookahead_on history receives
    // the same real completion event that drives stream_dispatch_script_bar.
    AggregatedBar ab = script_tf_agg_.feed(bar);
    const bool completed_on_boundary = ab.is_complete
        && tf_change(ab.bar.timestamp, bar.timestamp, script_tf_,
                     syminfo_.timezone, syminfo_.session);
    if (completed_on_boundary) {
        for (auto& state : security_eval_states_) {
            if (state.publish_gate_tf_seconds > 0) {
                publish_security_eval_state_at_calling_boundary(state);
            } else {
                feed_security_eval_state(state, bar, ab.is_complete);
            }
        }

        // The current input opened the next caller. Dispatch the completed
        // caller while its actual final requested child is still visible,
        // then evaluate the retained input for the next caller.
        dispatch_source_stream_script_bar(ab.bar, stream_script_bar_had_tick_);
        stream_script_bar_had_tick_ = had_tick;
        for (auto& state : security_eval_states_) {
            if (state.publish_gate_tf_seconds > 0) {
                feed_security_eval_state(
                    state, bar, /*calling_bar_complete=*/false);
            }
        }
        return;
    }
    for (auto& state : security_eval_states_) {
        feed_security_eval_state(state, bar, ab.is_complete);
    }
    if (completed_on_boundary) {
        // The current input bar opened the next bucket; the aggregator emitted
        // the preceding partial bucket before retaining this bar as its new
        // current state.
        dispatch_source_stream_script_bar(ab.bar, stream_script_bar_had_tick_);
        stream_script_bar_had_tick_ = had_tick;
    } else {
        stream_script_bar_had_tick_ = stream_script_bar_had_tick_ || had_tick;
        if (ab.is_complete) {
            dispatch_source_stream_script_bar(ab.bar, stream_script_bar_had_tick_);
            stream_script_bar_had_tick_ = false;
        }
    }
}



void BacktestEngine::stream_observe_entry(const PyramidEntry& pe) {
    if (stream_action_sequence_ == std::numeric_limits<uint64_t>::max()) {
        throw std::runtime_error("stream action sequence overflow");
    }
    StreamOrderAction action;
    action.sequence = ++stream_action_sequence_;
    action.timestamp_ms = pe.time;
    action.bar_index = pe.entry_bar_index;
    action.is_entry = true;
    action.is_long = position_side_ == PositionSide::LONG;
    action.quantity = pe.qty;
    action.price = pe.price;
    action.order_id = pe.entry_id;
    action.comment = pe.entry_comment;
    action.entry_incarnation = pe.entry_incarnation;
    source_stream_entry_comment(pe, action.comment);
    stream_order_actions_.push_back(std::move(action));
}

void BacktestEngine::stream_observe_exit(size_t trade_index) {
    if (stream_action_sequence_ == std::numeric_limits<uint64_t>::max()) {
        throw std::runtime_error("stream action sequence overflow");
    }
    const Trade& trade = trades_.at(trade_index);
    StreamOrderAction action;
    action.sequence = ++stream_action_sequence_;
    action.timestamp_ms = trade.exit_time;
    action.bar_index = trade.exit_bar_index;
    action.is_entry = false;
    action.is_long = trade.is_long;
    action.quantity = trade.qty;
    action.price = trade.exit_price;
    action.entry_incarnation = trade.entry_incarnation;
    action.closed_trade_index = trade_index;
    stream_order_actions_.push_back(std::move(action));
}

void BacktestEngine::stream_refresh_action_metadata(size_t first_action, size_t first_trade) {
    // Fill kernels finish assigning close IDs/comments after emit_close_trade.
    // Refresh only events produced by this input, retaining physical hook order.
    for (size_t i = first_action; i < stream_order_actions_.size(); ++i) {
        auto& action = stream_order_actions_[i];
        if (!action.is_entry) {
            const auto& trade = trades_.at(action.closed_trade_index);
            action.order_id = trade.exit_id;
            action.comment = trade.exit_comment;
            continue;
        }
        bool found = false;
        for (const auto& pe : pyramid_entries_) {
            if (pe.entry_incarnation == action.entry_incarnation && pe.entry_id == action.order_id) {
                action.comment = pe.entry_comment;
                found = true;
                break;
            }
        }
        if (!found) {
            for (size_t j = first_trade; j < trades_.size(); ++j) {
                const auto& trade = trades_[j];
                if (trade.entry_incarnation == action.entry_incarnation && trade.entry_id == action.order_id) {
                    action.comment = trade.entry_comment;
                    break;
                }
            }
        }
    }
}

uint64_t BacktestEngine::stream_state_hash() const {
    // Versioned observable-state fingerprint. Not a native-object snapshot and
    // deliberately excludes the consumable action queue. Recovery replays the
    // same externally supplied input sequence into the same strategy/config/
    // version and checks every step. This does not predict live executions.
    uint64_t hash = 1469598103934665603ULL;
    auto bytes = [&hash](const void* data, size_t count) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < count; ++i) { hash ^= p[i]; hash *= 1099511628211ULL; }
    };
    auto integer = [&bytes](uint64_t value) { bytes(&value, sizeof(value)); };
    auto real = [&bytes](double value) {
        if (value == 0.0) value = 0.0;
        if (std::isnan(value)) value = std::numeric_limits<double>::quiet_NaN();
        bytes(&value, sizeof(value));
    };
    auto bar_hash = [&integer, &real](const Bar& bar) {
        integer(static_cast<uint64_t>(bar.timestamp));
        real(bar.open); real(bar.high); real(bar.low); real(bar.close); real(bar.volume);
    };
    integer(16); integer(broker_state_hash());
    integer(static_cast<uint64_t>(stream_phase_));
    integer(static_cast<uint64_t>(stream_input_mode_));
    integer(static_cast<uint64_t>(stream_input_tf_ms_));
    integer(static_cast<uint64_t>(stream_next_input_open_ms_));
    integer(static_cast<uint64_t>(stream_clock_ms_));
    integer(static_cast<uint64_t>(stream_last_tick_ms_));
    integer(stream_last_sequence_); integer(stream_seen_sequence_);
    integer(stream_has_input_bar_); bar_hash(stream_input_bar_);
    real(stream_last_price_); integer(stream_has_last_price_);
    integer(static_cast<uint64_t>(stream_next_script_bar_index_));
    integer(stream_script_bar_had_tick_); integer(stream_script_tick_seen_);
    integer(stream_action_sequence_);
    integer(static_cast<uint64_t>(diag_input_bars_processed_));
    integer(static_cast<uint64_t>(diag_script_bars_processed_));
    integer(static_cast<uint64_t>(prev_bar_timestamp_));
    bar_hash(current_bar_); bar_hash(script_tf_agg_.current());
    integer(script_tf_agg_.has_pending_partial());
    integer(security_eval_states_.size());
    for (const auto& state : security_eval_states_) {
        integer(state.feed_count); integer(state.eval_complete_count);
        integer(state.eval_partial_count); bar_hash(state.current_bar);
        bar_hash(state.aggregator.current());
        integer(state.aggregator.has_pending_partial());
    }
    return hash;
}

}  // namespace pineforge
