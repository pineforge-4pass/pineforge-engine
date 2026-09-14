#include <pineforge/source/pine_strategy_host.hpp>
/*
 * engine_stream.cpp — continuous historical warmup -> realtime trade stream
 */

#include "../engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace pineforge {
using namespace source;

namespace {

Bar price_point(double price, double volume, int64_t timestamp) {
    return Bar{price, price, price, price, volume, timestamp};
}

}  // namespace

void source::PineStrategyHost::source_stream_entry_comment(
        const PyramidEntry& pe, std::string& comment) const {
    // Most kernels attach entry_comment after opening the lot. Preserve the
    // pending order's own text even if the new lot closes in this same input.
    for (const auto& order : pending_orders_) {
        if (order.incarnation == pe.entry_incarnation && order.id == pe.entry_id) {
            comment = order.comment;
            break;
        }
    }
}

bool source::PineStrategyHost::legacy_stream_begin(const Bar* warmup_bars, int n_warmup,
                                  const std::string& input_tf,
                                  const std::string& script_tf) {
    const StreamPhase phase_before_begin = stream_phase_;
    last_error_.clear();
    try {
        if (calc_on_order_fills_) {
            throw std::runtime_error("native stream requires close-only calculation; calc_on_order_fills is unsupported");
        }
        if (realtime_tail_ || probe_suppress_tail_logic_) {
            throw std::runtime_error("native stream cannot use historical probe/tail overrides");
        }
        if (!account_currency_fx_timestamps_.empty()) {
            throw std::runtime_error(
                "timestamped account-currency FX is not supported by streaming");
        }
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        if (aux_security_feed_enabled()) {
            throw std::runtime_error(
                "auxiliary request.security feed supports historical native-chart runs only");
        }
#endif
        if (native_security_feed_enabled()) {
            throw std::runtime_error(
                "native request.security feed supports historical runs only");
        }
        if (stream_phase_ == StreamPhase::REALTIME) {
            throw std::runtime_error("stream is already realtime");
        }
        if (warmup_bars == nullptr || n_warmup <= 0) {
            throw std::runtime_error(
                "stream warmup requires at least one confirmed OHLCV bar");
        }
        const int input_seconds = tf_to_seconds(input_tf);
        if (input_seconds <= 0) {
            throw std::runtime_error(
                "stream input timeframe must have a fixed positive duration: "
                + input_tf);
        }
        for (int i = 0; i < n_warmup; ++i) {
            const Bar& bar = warmup_bars[i];
            if (bar.timestamp < 0 || !std::isfinite(bar.open) || bar.open < 0
                || !std::isfinite(bar.high) || !std::isfinite(bar.low)
                || bar.low < 0 || !std::isfinite(bar.close) || bar.close < 0
                || !std::isfinite(bar.volume) || bar.volume < 0
                || bar.low > std::min(bar.open, bar.close)
                || bar.high < std::max(bar.open, bar.close)) {
                throw std::runtime_error("stream warmup has invalid OHLCV");
            }
        }
        for (int i = 1; i < n_warmup; ++i) {
            if (warmup_bars[i].timestamp <= warmup_bars[i - 1].timestamp) {
                throw std::runtime_error(
                    "stream warmup timestamps must be strictly increasing");
            }
        }
        if (!std::isfinite(warmup_bars[n_warmup - 1].close)
            || warmup_bars[n_warmup - 1].close <= 0.0) {
            throw std::runtime_error(
                "stream warmup final close must be finite and positive");
        }

        // A stream's warmup is historical context, not the rightmost realtime
        // bar. This keeps barstate.islast false until normalized trades take
        // over.
        stream_warmup_mode_ = true;
        run(warmup_bars, n_warmup, input_tf, script_tf,
            /*bar_magnifier=*/false, 4, MagnifierDistribution::ENDPOINTS);
        stream_warmup_mode_ = false;
        if (!last_error_.empty()) {
            return false;
        }

        stream_input_tf_ms_ = static_cast<int64_t>(input_seconds) * 1000;
        const int64_t last_open = warmup_bars[n_warmup - 1].timestamp;
        if (last_open > std::numeric_limits<int64_t>::max() - stream_input_tf_ms_) {
            throw std::runtime_error("stream warmup timestamp overflows next bar open");
        }
        stream_next_input_open_ms_ = last_open + stream_input_tf_ms_;
        stream_clock_ms_ = stream_next_input_open_ms_;
        stream_last_tick_ms_ = 0;
        stream_last_sequence_ = 0;
        stream_seen_sequence_ = false;
        stream_has_input_bar_ = false;
        stream_input_bar_ = Bar{};
        stream_last_price_ = warmup_bars[n_warmup - 1].close;
        stream_has_last_price_ = true;
        stream_next_script_bar_index_ =
            static_cast<int>(diag_script_bars_processed_);
        stream_script_bar_had_tick_ = false;
        stream_script_tick_seen_ = false;
        stream_phase_ = StreamPhase::REALTIME;
        stream_input_mode_ = StreamInputMode::UNSET;
        stream_action_sequence_ = 0;
        stream_order_actions_.clear();
        stream_observe_actions_ = true;

        // Exact normalized trades now drive the broker instead of inferred
        // OHLC paths. Strategy callbacks remain close-only; resting orders
        // are nevertheless fillable on
        // each normalized trade, as on TradingView's realtime broker emulator.
        bar_magnifier_enabled_ = true;
        bar_index_ = stream_next_script_bar_index_;
        last_bar_index_ = bar_index_;
        last_bar_time_ = stream_next_input_open_ms_;
        barstate_islast_ = true;
        return true;
    } catch (const std::exception& e) {
        // An already-running stream always rejects before warmup or setup.
        // Reporting that rejection must not terminate its existing lifecycle.
        // Failures after starting a new setup retain the discard/replay rule.
        if (phase_before_begin != StreamPhase::REALTIME) {
            stream_warmup_mode_ = false;
            stream_phase_ = StreamPhase::IDLE;
            stream_observe_actions_ = false;
        }
        last_error_ = e.what();
        return false;
    } catch (...) {
        if (phase_before_begin != StreamPhase::REALTIME) {
            stream_warmup_mode_ = false;
            stream_phase_ = StreamPhase::IDLE;
            stream_observe_actions_ = false;
        }
        last_error_ = "unknown error during BacktestEngine::stream_begin";
        return false;
    }
}

bool source::PineStrategyHost::legacy_stream_push_bar(const Bar& bar) {
    last_error_.clear();
    try {
        if (stream_phase_ != StreamPhase::REALTIME) {
            throw std::runtime_error("stream_push_bar requires a realtime stream");
        }
        if (stream_input_mode_ == StreamInputMode::TICKS || stream_has_input_bar_) {
            throw std::runtime_error("stream cannot mix confirmed bars and ticks");
        }
        if (bar.timestamp < stream_next_input_open_ms_
            || bar.timestamp > std::numeric_limits<int64_t>::max() - stream_input_tf_ms_
            || (bar.timestamp - stream_next_input_open_ms_) % stream_input_tf_ms_ != 0) {
            throw std::runtime_error("confirmed bar timestamp is out of order, off grid, or overflows");
        }
        if (!std::isfinite(bar.open) || bar.open <= 0.0
            || !std::isfinite(bar.high) || !std::isfinite(bar.low) || bar.low <= 0.0
            || !std::isfinite(bar.close) || bar.close <= 0.0
            || !std::isfinite(bar.volume) || bar.volume < 0.0
            || bar.low > std::min(bar.open, bar.close)
            || bar.high < std::max(bar.open, bar.close)) {
            throw std::runtime_error("confirmed bar has invalid OHLCV");
        }
        // Missing observed bars are never invented. Only independently known
        // closed-session intervals can be skipped, without changing aggregation.
        for (int64_t ts = stream_next_input_open_ms_; ts < bar.timestamp;
             ts += stream_input_tf_ms_) {
            if (pineforge::pine_session_ismarket(syminfo_.session, syminfo_.timezone, ts)) {
                throw std::runtime_error("confirmed bar stream has an in-session gap");
            }
        }
        const size_t first_action = stream_order_actions_.size();
        const size_t first_trade = trades_.size();
        stream_input_mode_ = StreamInputMode::BARS;
        bar_magnifier_enabled_ = false;
        stream_feed_input_bar(bar, false);
        stream_next_input_open_ms_ = bar.timestamp + stream_input_tf_ms_;
        stream_clock_ms_ = stream_next_input_open_ms_;
        stream_last_price_ = bar.close;
        stream_has_last_price_ = true;
        stream_refresh_action_metadata(first_action, first_trade);
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::stream_push_bar";
        return false;
    }
}

bool source::PineStrategyHost::legacy_stream_push_tick(const TradeTick& tick) {
    last_error_.clear();
    try {
        if (stream_phase_ != StreamPhase::REALTIME) {
            throw std::runtime_error("stream_push_tick requires a realtime stream");
        }
        if (stream_input_mode_ == StreamInputMode::BARS) {
            throw std::runtime_error("stream cannot mix confirmed bars and ticks");
        }
        if (tick.timestamp > std::numeric_limits<int64_t>::max() - stream_input_tf_ms_) {
            throw std::runtime_error("stream tick timestamp overflows input close");
        }
        if (!std::isfinite(tick.price) || tick.price <= 0.0) {
            throw std::runtime_error("stream tick price must be finite and positive");
        }
        if (!std::isfinite(tick.quantity) || tick.quantity < 0.0) {
            throw std::runtime_error("stream tick quantity must be finite and non-negative");
        }
        if (tick.timestamp < stream_clock_ms_) {
            throw std::runtime_error("stream tick timestamp moved backwards");
        }
        if (tick.sequence != 0 && stream_seen_sequence_
            && tick.sequence <= stream_last_sequence_) {
            throw std::runtime_error("stream sequence must be strictly increasing");
        }

        const size_t first_action = stream_order_actions_.size();
        const size_t first_trade = trades_.size();
        stream_input_mode_ = StreamInputMode::TICKS;
        if (!stream_finalize_until(tick.timestamp)) {
            return false;
        }

        if (!stream_has_input_bar_) {
            stream_input_bar_ = price_point(
                tick.price, tick.quantity, stream_next_input_open_ms_);
            stream_has_input_bar_ = true;
        } else {
            // Validate accumulation before touching OHLC as well as volume.
            // A rejected update must not poison this interval or consume its
            // timestamp/sequence. A new interval takes the fresh-bar branch.
            const double accumulated_volume = stream_input_bar_.volume + tick.quantity;
            if (!std::isfinite(accumulated_volume)) {
                throw std::runtime_error("stream tick volume overflow");
            }
            stream_input_bar_.high = std::max(stream_input_bar_.high, tick.price);
            stream_input_bar_.low = std::min(stream_input_bar_.low, tick.price);
            stream_input_bar_.close = tick.price;
            stream_input_bar_.volume = accumulated_volume;
        }

        stream_last_price_ = tick.price;
        stream_has_last_price_ = true;
        stream_last_tick_ms_ = tick.timestamp;
        stream_clock_ms_ = tick.timestamp;
        if (tick.sequence != 0) {
            stream_last_sequence_ = tick.sequence;
            stream_seen_sequence_ = true;
        }

        // Broker-only tick pass. Pine strategy code stays on its default
        // close-only cadence, but orders created on the preceding close fill
        // at the first actual source record and priced orders see the exact
        // trade path rather than an inferred OHLC traversal.
        current_bar_ = price_point(tick.price, tick.quantity, tick.timestamp);
        bar_index_ = stream_next_script_bar_index_;
        last_bar_index_ = bar_index_;
        last_bar_time_ = tick.timestamp;
        barstate_islast_ = true;
        is_first_tick_ = !stream_script_tick_seen_;
        is_last_tick_ = false;
        // The overwhelming majority of source records arrive while many
        // strategies are flat and have no order in the broker. Such a print
        // still contributes to the forming OHLCV bar above, but there is no
        // broker, excursion, or margin state it can possibly mutate. Avoiding
        // the full order-sort/risk pass here is exact, not an approximation,
        // and makes long shared-feed corpus replays tractable.
        if (!pending_orders_.empty() || position_side_ != PositionSide::FLAT) {
            if (!pending_orders_.empty()) {
                process_pending_orders(current_bar_);
            }
            update_per_trade_extremes();
            const std::size_t trades_before_mc = trades_.size();
            process_margin_call(current_bar_);
            if (trades_.size() != trades_before_mc) {
                refresh_frozen_default_sizing_after_margin_call();
            }
        }
        stream_script_tick_seen_ = true;
        stream_refresh_action_metadata(first_action, first_trade);
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::stream_push_tick";
        return false;
    }
}

bool source::PineStrategyHost::legacy_stream_push_ticks(const TradeTick* ticks, int n) {
    last_error_.clear();
    if (n < 0 || (n > 0 && ticks == nullptr)) {
        last_error_ = "stream_push_ticks received an invalid tick array";
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (!stream_push_tick(ticks[i])) return false;
    }
    return true;
}

bool source::PineStrategyHost::legacy_stream_advance_time(int64_t timestamp_ms) {
    last_error_.clear();
    try {
        if (stream_phase_ != StreamPhase::REALTIME) {
            throw std::runtime_error(
                "stream_advance_time requires a realtime stream");
        }
        if (stream_input_mode_ == StreamInputMode::BARS) {
            throw std::runtime_error("confirmed-bar mode requires a bar, not clock advancement");
        }
        if (timestamp_ms > std::numeric_limits<int64_t>::max() - stream_input_tf_ms_) {
            throw std::runtime_error("stream clock overflows input close");
        }
        if (timestamp_ms < stream_clock_ms_) {
            throw std::runtime_error("stream clock moved backwards");
        }
        const size_t first_action = stream_order_actions_.size();
        const size_t first_trade = trades_.size();
        stream_input_mode_ = StreamInputMode::TICKS;
        if (!stream_finalize_until(timestamp_ms)) return false;
        stream_clock_ms_ = timestamp_ms;
        stream_refresh_action_metadata(first_action, first_trade);
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::stream_advance_time";
        return false;
    }
}

bool source::PineStrategyHost::legacy_stream_end(bool finalize_partial_input_bar) {
    last_error_.clear();
    try {
        if (stream_phase_ != StreamPhase::REALTIME) {
            throw std::runtime_error("stream_end requires a realtime stream");
        }
        const size_t first_action = stream_order_actions_.size();
        const size_t first_trade = trades_.size();
        if (finalize_partial_input_bar && stream_has_input_bar_) {
            stream_feed_input_bar(stream_input_bar_, /*had_tick=*/true);
            stream_has_input_bar_ = false;
            stream_next_input_open_ms_ += stream_input_tf_ms_;
        }
        stream_refresh_action_metadata(first_action, first_trade);
        stream_phase_ = StreamPhase::ENDED;
        stream_observe_actions_ = false;
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::stream_end";
        return false;
    }
}

void source::PineStrategyHost::dispatch_source_stream_script_bar(const Bar& bar, bool had_tick) {
    if (script_tf_seconds_ > 0
        && bar.timestamp > std::numeric_limits<int64_t>::max()
            - static_cast<int64_t>(script_tf_seconds_) * 1000) {
        throw std::runtime_error("stream script bar timestamp overflows its close");
    }
    if (stream_next_script_bar_index_ == std::numeric_limits<int>::max()) {
        throw std::runtime_error("stream script bar index overflow");
    }
    // ABI v4 task 4 fix (final review F6): stream mode calls
    // process_pending_orders() directly and never goes through
    // dispatch_bar() (engine_run.cpp), so dispatch_bar()'s own per-bar
    // reset of last_bar_dual_entry_decision_ never runs here. Without this,
    // a stream bar that arbitrates no dual-entry-stop pass would leave the
    // PREVIOUS bar's decision readable -- and hashed, since
    // engine_state_hash.cpp includes it in the per-bar broker-state hash.
    last_bar_dual_entry_decision_ = internal::DualEntryStopPathWinner::None;
    const int this_bar_index = stream_next_script_bar_index_++;
    bar_index_ = this_bar_index;
    last_bar_index_ = this_bar_index;
    last_bar_time_ = bar.timestamp;
    barstate_islast_ = true;
    is_first_tick_ = true;
    is_last_tick_ = true;
    ++diag_script_bars_processed_;
    pending_close_qty_in_bar_ = 0.0;

    if (stream_input_mode_ == StreamInputMode::BARS) {
        current_bar_ = bar;
        is_tail_bar_ = false;
        const bool in_session = chart_bar_ismarket(bar.timestamp);
        const bool last_session_bar = in_session && script_tf_seconds_ > 0
            && !chart_bar_ismarket(bar.timestamp + static_cast<int64_t>(script_tf_seconds_) * 1000);
        set_session_bar_state(in_session, last_session_bar);
        // A confirmed OHLCV bar is executed by the existing historical OHLC
        // kernel. Its historical-only fill predicates remain enabled, while
        // the independent observation flag records only this live continuation.
        stream_phase_ = StreamPhase::IDLE;
        bar_magnifier_enabled_ = false;
        try {
            dispatch_bar();
        } catch (...) {
            stream_phase_ = StreamPhase::REALTIME;
            throw;
        }
        stream_phase_ = StreamPhase::REALTIME;
        prev_in_session_ = session_ismarket_;
        update_equity_extremes();
        record_equity_point(bar.timestamp);
        if (broker_state_hash_recording_) broker_state_hashes_.push_back(broker_state_hash());
        prev_bar_timestamp_ = bar.timestamp;
        bar_index_ = stream_next_script_bar_index_;
        last_bar_index_ = bar_index_;
        stream_script_tick_seen_ = false;
        return;
    }

    // A synthesized zero-volume interval has no raw broker pass. Give resting
    // market orders one carried-price point at its open so time advancement is
    // deterministic even through quiet in-session intervals.
    if (!had_tick) {
        current_bar_ = price_point(bar.open, 0.0, bar.timestamp);
        process_pending_orders(current_bar_);
        update_per_trade_extremes();
        const std::size_t trades_before_mc = trades_.size();
        process_margin_call(current_bar_);
        if (trades_.size() != trades_before_mc) {
            refresh_frozen_default_sizing_after_margin_call();
        }
    }

    current_bar_ = bar;
    {
        const bool in_session = chart_bar_ismarket(current_bar_.timestamp);
        // Intraday islastbar: the next script bar opens one bar width ahead
        // on the stream clock; fire when that open is out of session.
        bool intraday_islastbar = false;
        if (in_session && script_tf_seconds_ > 0) {
            const int64_t next_ts = current_bar_.timestamp
                + static_cast<int64_t>(script_tf_seconds_) * 1000;
            intraday_islastbar = !chart_bar_ismarket(next_ts);
        }
        set_session_bar_state(in_session, intraday_islastbar);
    }

    _push_source_series();
    invoke_chart_on_bar(current_bar_);
    if (process_orders_on_close_) {
        flush_same_bar_close();
        // New close-time orders only get the closing price point. Re-walking
        // the full OHLC range would let a just-created order see prices that
        // occurred before it existed.
        const Bar completed_bar = current_bar_;
        current_bar_ = price_point(
            completed_bar.close, 0.0, completed_bar.timestamp);
        process_pending_orders(current_bar_);
        current_bar_ = completed_bar;
    }

    finalize_bar();
    prev_in_session_ = session_ismarket_;
    update_equity_extremes();
    record_equity_point(bar.timestamp);
    if (broker_state_hash_recording_) broker_state_hashes_.push_back(broker_state_hash());
    prev_bar_timestamp_ = bar.timestamp;

    // Ticks belonging to the next script bar must compare pending-order
    // created_bar values against the next index before that bar closes.
    bar_index_ = stream_next_script_bar_index_;
    last_bar_index_ = bar_index_;
    stream_script_tick_seen_ = false;
}

} // namespace pineforge
