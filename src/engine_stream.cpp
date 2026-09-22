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
    // The settlement's commit calls this after recording its rows and its
    // opening lot: re-read each observed action's id and comment from the
    // row or lot it names, so the stream carries the final values.
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
    integer(18); integer(broker_state_hash());
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
