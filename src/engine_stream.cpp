/*
 * engine_stream.cpp — continuous historical warmup -> realtime trade stream
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace pineforge {

namespace {

Bar price_point(double price, double volume, int64_t timestamp) {
    return Bar{price, price, price, price, volume, timestamp};
}

}  // namespace

bool BacktestEngine::stream_begin(const Bar* warmup_bars, int n_warmup,
                                  const std::string& input_tf,
                                  const std::string& script_tf) {
    last_error_.clear();
    try {
        if (!account_currency_fx_timestamps_.empty()) {
            throw std::runtime_error(
                "timestamped account-currency FX is not supported by streaming");
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

        // Exact normalized trades now drive the broker instead of inferred
        // OHLC paths. Strategy code remains close-only unless codegen opts in
        // to calc_on_every_tick; resting orders are nevertheless fillable on
        // each normalized trade, as on TradingView's realtime broker emulator.
        bar_magnifier_enabled_ = true;
        bar_index_ = stream_next_script_bar_index_;
        last_bar_index_ = bar_index_;
        last_bar_time_ = stream_next_input_open_ms_;
        barstate_islast_ = true;
        return true;
    } catch (const std::exception& e) {
        stream_warmup_mode_ = false;
        stream_phase_ = StreamPhase::IDLE;
        last_error_ = e.what();
        return false;
    } catch (...) {
        stream_warmup_mode_ = false;
        stream_phase_ = StreamPhase::IDLE;
        last_error_ = "unknown error during BacktestEngine::stream_begin";
        return false;
    }
}

bool BacktestEngine::stream_push_tick(const TradeTick& tick) {
    last_error_.clear();
    try {
        if (stream_phase_ != StreamPhase::REALTIME) {
            throw std::runtime_error("stream_push_tick requires a realtime stream");
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

        if (!stream_finalize_until(tick.timestamp)) {
            return false;
        }

        if (!stream_has_input_bar_) {
            stream_input_bar_ = price_point(
                tick.price, tick.quantity, stream_next_input_open_ms_);
            stream_has_input_bar_ = true;
        } else {
            stream_input_bar_.high = std::max(stream_input_bar_.high, tick.price);
            stream_input_bar_.low = std::min(stream_input_bar_.low, tick.price);
            stream_input_bar_.close = tick.price;
            stream_input_bar_.volume += tick.quantity;
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
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::stream_push_tick";
        return false;
    }
}

bool BacktestEngine::stream_push_ticks(const TradeTick* ticks, int n) {
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

bool BacktestEngine::stream_advance_time(int64_t timestamp_ms) {
    last_error_.clear();
    try {
        if (stream_phase_ != StreamPhase::REALTIME) {
            throw std::runtime_error(
                "stream_advance_time requires a realtime stream");
        }
        if (timestamp_ms < stream_clock_ms_) {
            throw std::runtime_error("stream clock moved backwards");
        }
        if (!stream_finalize_until(timestamp_ms)) return false;
        stream_clock_ms_ = timestamp_ms;
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::stream_advance_time";
        return false;
    }
}

bool BacktestEngine::stream_end(bool finalize_partial_input_bar) {
    last_error_.clear();
    try {
        if (stream_phase_ != StreamPhase::REALTIME) {
            throw std::runtime_error("stream_end requires a realtime stream");
        }
        if (finalize_partial_input_bar && stream_has_input_bar_) {
            stream_feed_input_bar(stream_input_bar_, /*had_tick=*/true);
            stream_has_input_bar_ = false;
            stream_next_input_open_ms_ += stream_input_tf_ms_;
        }
        stream_phase_ = StreamPhase::ENDED;
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    } catch (...) {
        last_error_ = "unknown error during BacktestEngine::stream_end";
        return false;
    }
}

bool BacktestEngine::stream_finalize_until(int64_t timestamp_ms) {
    while (timestamp_ms >= stream_next_input_open_ms_ + stream_input_tf_ms_) {
        const bool had_tick = stream_has_input_bar_;
        const bool in_session = pine_session_ismarket(
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

    for (auto& state : security_eval_states_) {
        feed_security_eval_state(state, bar);
    }

    if (!diag_needs_aggregation_) {
        stream_dispatch_script_bar(bar, had_tick);
        return;
    }

    AggregatedBar ab = script_tf_agg_.feed(bar);
    const bool completed_on_boundary = ab.is_complete
        && tf_change(ab.bar.timestamp, bar.timestamp, script_tf_);
    if (completed_on_boundary) {
        // The current input bar opened the next bucket; the aggregator emitted
        // the preceding partial bucket before retaining this bar as its new
        // current state.
        stream_dispatch_script_bar(ab.bar, stream_script_bar_had_tick_);
        stream_script_bar_had_tick_ = had_tick;
    } else {
        stream_script_bar_had_tick_ = stream_script_bar_had_tick_ || had_tick;
        if (ab.is_complete) {
            stream_dispatch_script_bar(ab.bar, stream_script_bar_had_tick_);
            stream_script_bar_had_tick_ = false;
        }
    }
}

void BacktestEngine::stream_dispatch_script_bar(const Bar& bar, bool had_tick) {
    const int this_bar_index = stream_next_script_bar_index_++;
    bar_index_ = this_bar_index;
    last_bar_index_ = this_bar_index;
    last_bar_time_ = bar.timestamp;
    barstate_islast_ = true;
    is_first_tick_ = true;
    is_last_tick_ = true;
    ++diag_script_bars_processed_;
    pending_close_qty_in_bar_ = 0.0;

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
    session_ismarket_ = pine_session_ismarket(
        syminfo_.session, syminfo_.timezone, current_bar_.timestamp);
    session_isfirstbar_ = session_ismarket_ && !prev_in_session_;
    if (session_ismarket_ && script_tf_seconds_ > 0) {
        const int64_t next_ts = current_bar_.timestamp
            + static_cast<int64_t>(script_tf_seconds_) * 1000;
        session_islastbar_ = !pine_session_ismarket(
            syminfo_.session, syminfo_.timezone, next_ts);
    } else {
        session_islastbar_ = false;
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
    prev_bar_timestamp_ = bar.timestamp;

    // Ticks belonging to the next script bar must compare pending-order
    // created_bar values against the next index before that bar closes.
    bar_index_ = stream_next_script_bar_index_;
    last_bar_index_ = bar_index_;
    stream_script_tick_seen_ = false;
}

}  // namespace pineforge
