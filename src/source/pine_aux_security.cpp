#include <pineforge/source/pine_strategy_host.hpp>
/*
 * engine_aux_security.cpp — native-chart / request.security feed separation
 */

#include "../engine_internal.hpp"

#include <pineforge/ta.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pineforge {
using namespace source;

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1

bool source::PineStrategyHost::set_aux_security_feed(const Bar* bars, int n,
                                           const std::string& input_tf) {
    guard_native_mutation("set_aux_security_feed");
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

bool source::PineStrategyHost::source_aux_security_feed_enabled() const {
    return !aux_security_bars_.empty();
}

void source::PineStrategyHost::source_aux_security_input_view(
        const Bar*& bars, int& n) const {
    bars = aux_security_bars_.data();
    n = static_cast<int>(aux_security_bars_.size());
}

void source::PineStrategyHost::clear_aux_security_chart_ranges() {
    aux_security_chart_begin_.clear();
    aux_security_chart_end_.clear();
}

void source::PineStrategyHost::prepare_aux_security_chart_ranges(
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
    const CalendarPeriod chart_period = calendar_period_for(chart_tf);
    const bool calendar_chart = chart_period != CalendarPeriod::NONE;
    // Calendar chart timestamps are the ACTUAL exchange bar opens and define
    // the authoritative partition.  A native daily bar may begin at a shifted
    // special-session open (NSE Muhurat), or may coalesce more than one
    // nominal session key (the CME Labor-Day Sunday/Monday sessions).  Route
    // each auxiliary bar through [chart_ts[i], chart_ts[i + 1]) while retaining
    // the native label for chart and broker semantics.  Nominal calendar keys
    // remain useful only for ignoring wider leading/trailing feed coverage.
    std::vector<int64_t> chart_route_keys;
    chart_route_keys.reserve(static_cast<std::size_t>(n_chart));
    for (int i = 0; i < n_chart; ++i) {
        // Key each native bar by the session it COVERS: OANDA stamps daily
        // bars at the 17:00 ET break, one hour before the 1800-1700 session
        // the bar actually carries, and the raw stamp would key one session
        // too early -- the tail prefilter below would then drop the last
        // bar's entire slice as beyond last_chart_key.
        const int64_t key = calendar_chart
            ? session_period_open_ms(
                  session_covered_instant_ms(chart_bars[i].timestamp,
                                             syminfo_.timezone,
                                             syminfo_.session),
                  syminfo_.timezone, syminfo_.session, chart_period)
            : chart_bars[i].timestamp;
        if (!chart_route_keys.empty() && key <= chart_route_keys.back()) {
            throw std::runtime_error(
                "native chart feed trading-period identities must be unique and strictly increasing with an auxiliary security feed");
        }
        chart_route_keys.push_back(key);
    }
    const int64_t first_chart_key = chart_route_keys.front();
    const int64_t last_chart_key = chart_route_keys.back();
    std::size_t chart_index = 0;
    auto record_aux = [&](std::size_t aux_index) {
        if (aux_security_chart_begin_[chart_index] == missing) {
            aux_security_chart_begin_[chart_index] = aux_index;
        }
        aux_security_chart_end_[chart_index] = aux_index + 1;
    };
    if (calendar_chart) {
        const int64_t first_chart_timestamp = chart_bars[0].timestamp;
        for (std::size_t aux_index = 0;
             aux_index < aux_security_bars_.size(); ++aux_index) {
            const int64_t aux_timestamp =
                aux_security_bars_[aux_index].timestamp;
            const int64_t period_key = session_period_open_ms(
                aux_timestamp, syminfo_.timezone, syminfo_.session,
                chart_period);
            if (period_key < first_chart_key || period_key > last_chart_key
                || aux_timestamp < first_chart_timestamp) {
                continue;
            }
            while (chart_index + 1 < static_cast<std::size_t>(n_chart)
                   && chart_bars[chart_index + 1].timestamp <= aux_timestamp) {
                ++chart_index;
            }
            record_aux(aux_index);
        }
    } else {
        for (std::size_t aux_index = 0;
             aux_index < aux_security_bars_.size(); ++aux_index) {
            const int64_t label = chart_router.bar_label_ms(
                aux_security_bars_[aux_index].timestamp);
            // Intraday native bars remain exact session-grid labels.  Wider
            // leading/trailing coverage is inert, but an interior grid hole
            // must fail closed rather than attach to a neighbouring bar.
            if (label < first_chart_key || label > last_chart_key) {
                continue;
            }
            while (chart_index + 1 < static_cast<std::size_t>(n_chart)
                   && chart_route_keys[chart_index] < label) {
                ++chart_index;
            }
            if (chart_route_keys[chart_index] != label) {
                throw std::runtime_error(
                    "auxiliary request.security bar does not map to a native chart bar");
            }
            record_aux(aux_index);
        }
    }
    // An exchange chart can retain a short/early-close chart slot for which
    // the finer export has no bar. Retain the sentinel so the native-hook
    // route supplies no auxiliary evaluator input at that slot and holds the
    // existing request.security value instead of rejecting the chart run.
}

int64_t source::PineStrategyHost::aux_security_calling_close_ms() const {
    const CalendarPeriod chart_period = calendar_period_for(input_tf_);
    if (chart_period != CalendarPeriod::NONE) {
        return session_period_last_traded_close_ms(
            session_covered_instant_ms(current_bar_.timestamp,
                                       syminfo_.timezone, syminfo_.session),
            syminfo_.timezone, syminfo_.session, chart_period);
    }
    int chart_seconds = 0;
    try {
        chart_seconds = tf_to_seconds(input_tf_);
    } catch (...) {
        chart_seconds = 0;
    }
    return chart_seconds > 0
        ? current_bar_.timestamp + static_cast<int64_t>(chart_seconds) * 1000
        : 0;
}

void source::PineStrategyHost::feed_aux_security_for_chart_bar(int chart_index) {
    const std::size_t idx = static_cast<std::size_t>(chart_index);
    if (idx >= aux_security_chart_begin_.size()
        || idx >= aux_security_chart_end_.size()) {
        throw std::runtime_error(
            "auxiliary request.security chart routing is not initialized");
    }
    const std::size_t begin = aux_security_chart_begin_[idx];
    const std::size_t end = aux_security_chart_end_[idx];
    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    if (begin == missing || end == missing) return;

    security_calling_close_ms_ = aux_security_calling_close_ms();

    // A first-bucket-latched evaluator (calling_open_latches_first) starts
    // every chart bar's slice live and is deferred once its first bucket of
    // the slice has been published.
    for (auto& state : security_eval_states_) {
        state.first_bucket_published = false;
        state.deferred_aux.clear();
        state.slice_open_label = begin < end
            ? state.aggregator.bucket_open_ms(aux_security_bars_[begin].timestamp)
            : 0;
    }

    // Lower-TF arrays belong to the native chart slice, not a raw UTC bucket.
    // Accumulate the entire symbol-clock-aligned slice here and publish it only
    // after its final auxiliary bar. This keeps an overnight 17:00-17:00 daily
    // bar as one array even though its raw timestamps cross UTC midnight.
    for (std::size_t i = begin; i < end; ++i) {
        const Bar& aux_bar = aux_security_bars_[i];
        const bool calling_bar_complete = (i + 1 == end);
        // The auxiliary bar after this one, across chart bars (0 at the
        // feed's end): a calendar bucket completes on the period's actual
        // last bar (security_next_input_ms_).
        security_next_input_ms_ = (i + 1 < aux_security_bars_.size())
            ? aux_security_bars_[i + 1].timestamp : 0;
        for (auto& state : security_eval_states_) {
            if (!state.lower_tf_array_requested) {
                if (state.calling_open_latches_first
                    && state.first_bucket_published) {
                    // TradingView reads the calling bar's FIRST intrabar:
                    // the chart body runs on the first bucket's
                    // publication, the rest of the slice follows it
                    // (feed_deferred_aux_security_for_chart_bar).
                    state.deferred_aux.push_back(
                        {aux_bar, security_next_input_ms_,
                         calling_bar_complete});
                    continue;
                }
                const int64_t published_before = state.eval_complete_count;
                pine_feed_security_eval_state(
                    state, aux_bar, calling_bar_complete);
                // The slice's first bucket is published: a completion
                // labelled at or after the slice's first bucket open. An
                // older label is the previous slice's pending tail emitted
                // on this boundary (the Thanksgiving 21:57 singleton on
                // Black Friday's first minute): it precedes this bar's
                // first bucket in the requested series and does not latch.
                if (state.calling_open_latches_first
                    && state.eval_complete_count > published_before
                    && state.last_published_label >= state.slice_open_label) {
                    state.first_bucket_published = true;
                }
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

    security_calling_close_ms_ = 0;

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
            dispatch_security_eval(state, bar, true,
                                   state.eval_complete_count - 1);
            state.lower_tf_sub_bar_index++;
        }
        state.lower_tf_input_buffer.clear();
    }
}

void source::PineStrategyHost::feed_deferred_aux_security_for_chart_bar(int chart_index) {
    (void)chart_index;
    bool any = false;
    for (const auto& state : security_eval_states_) {
        if (state.calling_open_latches_first && !state.deferred_aux.empty()) {
            any = true;
            break;
        }
    }
    if (!any) return;
    security_calling_close_ms_ = aux_security_calling_close_ms();
    for (auto& state : security_eval_states_) {
        if (!state.calling_open_latches_first || state.deferred_aux.empty()) {
            continue;
        }
        // Move the slice out first: feeding never re-enters the deferral
        // (first_bucket_published stays set until the next chart bar's
        // slice resets it), but the buffer must not be appended to while
        // it is walked.
        std::vector<SecurityEvalState::DeferredAuxBar> held;
        held.swap(state.deferred_aux);
        for (const auto& d : held) {
            security_next_input_ms_ = d.next_input_ms;
            pine_feed_security_eval_state(state, d.bar,
                                          d.calling_bar_complete);
        }
    }
    security_calling_close_ms_ = 0;
}

#endif  // PINEFORGE_HAS_AUX_SECURITY_FEED_V1

} // namespace pineforge
