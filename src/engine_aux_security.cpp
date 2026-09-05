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
    for (int i = 0; i < n_chart; ++i) {
        if (aux_security_chart_begin_[static_cast<std::size_t>(i)] == missing) {
            throw std::runtime_error(
                "native chart bar has no matching auxiliary request.security bars");
        }
    }
}


// The calling chart bar's nominal close -- TradingView's time_close of the
// native bar this slice belongs to: a calendar chart bar closes at its
// period's nominal (last traded session-day) close whatever the slice holds
// -- Fri 17:00 ET for OANDA:XAUUSD's 07-03-stamped daily bar although its
// data ends 12:45 -- and an intraday chart bar at its grid end. An OTC
// calendar bucket the next auxiliary bar leaves completes on the slice's
// last bar exactly when this close reaches the period's
// (TimeframeAggregator::feed(bar, next_input_ms, calling_close_ms); lab tv
// oanda1d pin, 2026-09-05). current_bar_ is the native chart bar the run
// loop set before calling here.
int64_t BacktestEngine::aux_security_calling_close_ms() const {
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


void BacktestEngine::feed_aux_security_for_chart_bar(int chart_index) {
    const std::size_t idx = static_cast<std::size_t>(chart_index);
    if (idx >= aux_security_chart_begin_.size()
        || idx >= aux_security_chart_end_.size()) {
        throw std::runtime_error(
            "auxiliary request.security chart routing is not initialized");
    }
    const std::size_t begin = aux_security_chart_begin_[idx];
    const std::size_t end = aux_security_chart_end_[idx];

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
                feed_security_at_calling_bar_boundary(
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


void BacktestEngine::feed_deferred_aux_security_for_chart_bar(int chart_index) {
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
            feed_security_at_calling_bar_boundary(state, d.bar,
                                                  d.calling_bar_complete);
        }
    }
    security_calling_close_ms_ = 0;
}

#endif  // PINEFORGE_HAS_AUX_SECURITY_FEED_V1


// ---- native higher-timeframe request.security feeds -------------------------
//
// TradingView's request.security(syminfo.tickerid, "D", close) on an intraday
// chart of CME_MINI:ES1! returns the 15:00 CT settlement, on NASDAQ:AAPL the
// official closing print, on NSE:NIFTY the exchange's official OHLC -- values
// no aggregation of the intraday feed produces (pinned 2026-09-04 by lab tv
// limit-fill probes: 9/9 ES1!, 5/5 AAPL and 2/2 NIFTY fills equal the 1D
// feed's close and none the last 15m close). The campaign holds those daily
// bars; this path lets a completed daily bucket carry them while the
// aggregator keeps deciding WHEN the bucket completes.
//
// Its "W" and "M" requests read TradingView's weekly / monthly bars, which
// are BUILT FROM THOSE SAME NATIVE DAILY BARS -- o = the first session's
// daily open, h / l = the daily extremes, c = the last session's daily close,
// v = the sum -- never from the chart's intraday prints (pinned 2026-09-05 by
// lab tv, wm-security-buckets: 1473/1473 qty-encoded reads on NYSE:F 15m and
// CME_MINI:ES1! 15m equal the native-1D-built period, 0 the 15m-built one;
// NYSE:F week 2025-07-28 c 10.82 vs 10.81, week 2025-11-17 o = h = 13.1751
// vs 13.14 / 13.155, ES1! week 2025-08-11 c 6471.5 = Friday's settlement vs
// the 15m print 6467.25). So a "W" / "M" evaluator with no feed of its own
// derives its buckets from the installed daily feed, keyed exactly as its
// aggregator labels the same period.
//
// The native feed also decides WHERE a period begins and ends. TradingView's
// daily bar on CME_MINI:ES1! is the span from one native stamp to the bar
// before the next: a holiday session that pauses at 12:00 CT and reopens at
// 17:00 the same day (Labor Day, Thanksgiving, Independence Day) has no
// stamp of its own and is folded into the NEXT trade date's daily bar, which
// advances on that session's last bar and carries TradingView's own o/h/l/c/v
// (pinned 2026-09-05 by lab tv, es-daily-timing, ledger
// log-20260905t031053z-f283208c: the Sun 08-31 17:00 stamp runs to Tue 09-02
// 15:45, o 6478.75 h 6491.5 l 6371.75 c 6425.5 v 1802584; the Wed 11-26 17:00
// stamp to Fri 11-28 12:00 across the Thanksgiving pause, reopen and the
// registry's Thu 20:45 -> Fri 07:15 hole; the Thu 07-03 17:00 stamp to Mon
// 07-07 15:45 with o 6307.75 = the Sunday open and h 6315 below the holiday
// session's 6322.75 -- values no chart aggregate produces). Every evaluator
// that reads the feed therefore takes its stamps as its aggregator's period
// partition (TimeframeAggregator::set_native_periods), each period's trade
// date being the session-day of its last chart bar, so the derived W / M
// buckets group the merged day with its next trade date's week / month.

bool BacktestEngine::set_native_security_feed(const std::string& timeframe,
                                              const Bar* bars, int n) {
    int seconds = 0;
    try {
        seconds = tf_to_seconds(timeframe);
    } catch (...) {
        seconds = 0;
    }
    if (timeframe.empty() || seconds == 0) {
        last_error_ =
            "native request.security feed requires a parseable timeframe";
        return false;
    }
    auto existing = native_security_feeds_.begin();
    while (existing != native_security_feeds_.end()
           && existing->seconds != seconds) {
        ++existing;
    }
    if (n == 0) {
        if (existing != native_security_feeds_.end()) {
            native_security_feeds_.erase(existing);
        }
        last_error_.clear();
        return true;
    }
    if (n < 0 || bars == nullptr) {
        last_error_ =
            "native request.security feed requires bars and a positive count";
        return false;
    }
    for (int i = 1; i < n; ++i) {
        if (bars[i].timestamp <= bars[i - 1].timestamp) {
            last_error_ =
                "native request.security feed timestamps must be strictly increasing";
            return false;
        }
    }
    NativeSecurityFeed feed;
    feed.tf = timeframe;
    feed.seconds = seconds;
    feed.bars.assign(bars, bars + n);
    if (existing != native_security_feeds_.end()) {
        *existing = std::move(feed);
    } else {
        native_security_feeds_.push_back(std::move(feed));
    }
    last_error_.clear();
    return true;
}


void BacktestEngine::prepare_native_security_feeds(const Bar* input_bars,
                                                   int n_input) {
    diag_native_security_substitutions_ = 0;
    diag_native_security_misses_ = 0;
    if (native_security_feeds_.empty()) {
        for (auto& state : security_eval_states_) {
            state.native_feed_index = -1;
            state.native_bars_by_label.clear();
        }
        return;
    }
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    // The evaluators are fed the auxiliary slice on the split-feed path: the
    // period's last chart bar is the last auxiliary bar.
    if (aux_security_feed_enabled()) {
        input_bars = aux_security_bars_.data();
        n_input = static_cast<int>(aux_security_bars_.size());
    }
#endif
    if (input_bars == nullptr || n_input < 0) n_input = 0;
    // Per feed: its stamps and, per stamp, the period's trade instant -- the
    // last input bar before the next stamp (the stamp itself when no input
    // bar lies in the period, e.g. beyond the chart), whose session-day is
    // the trade date TradingView files the bar under (the merged Labor-Day
    // bar stamped Sun 17:00 CT is Tuesday's). Both sequences are sorted, so
    // one merge pass over the input bars serves every stamp.
    struct NativePeriods {
        std::vector<int64_t> stamps;
        std::vector<int64_t> trade_instants;
    };
    std::vector<NativePeriods> periods(native_security_feeds_.size());
    for (std::size_t f = 0; f < native_security_feeds_.size(); ++f) {
        const auto& bars = native_security_feeds_[f].bars;
        NativePeriods& np = periods[f];
        np.stamps.reserve(bars.size());
        np.trade_instants.reserve(bars.size());
        int j = 0;
        for (std::size_t k = 0; k < bars.size(); ++k) {
            const int64_t stamp = bars[k].timestamp;
            const int64_t next = (k + 1 < bars.size())
                ? bars[k + 1].timestamp
                : std::numeric_limits<int64_t>::max();
            int64_t last_in_period = stamp;
            while (j < n_input && input_bars[j].timestamp < next) {
                if (input_bars[j].timestamp >= stamp) {
                    last_in_period = input_bars[j].timestamp;
                }
                ++j;
            }
            np.stamps.push_back(stamp);
            np.trade_instants.push_back(last_in_period);
        }
    }
    for (auto& state : security_eval_states_) {
        state.native_feed_index = -1;
        state.native_bars_by_label.clear();
        // Only an aggregating (coarser-than-input) request has buckets to
        // substitute; passthrough, lower-TF emulation and input passthrough
        // read the feed itself.
        if (state.lower_tf_emulation || state.lower_tf_use_input
            || !state.aggregator.is_active()) {
            continue;
        }
        int requested_seconds = 0;
        try {
            requested_seconds = tf_to_seconds(state.tf);
        } catch (...) {
            requested_seconds = 0;
        }
        if (requested_seconds == 0) continue;
        // Key by the label the aggregate carries: the covered session instant
        // (OANDA's 17:00 stamp covers the 18:00 session, see
        // session_covered_instant_ms) labelled exactly as this state's
        // aggregator labels its own buckets.
        auto label_of = [&](int64_t covered_ms) {
            return state.aggregator.bar_label_ms(covered_ms);
        };
        // (a) A feed of the requested timeframe itself: one native bar per
        // bucket, its stamps the evaluator's period partition (a calendar
        // aggregator; an intraday RATIO grid keeps its own buckets, so the
        // stamps are inert there). A later native bar under one label (a
        // session the run's calendar coalesces) is the period's final print:
        // keep it.
        for (std::size_t i = 0; i < native_security_feeds_.size(); ++i) {
            if (native_security_feeds_[i].seconds != requested_seconds) continue;
            state.native_feed_index = static_cast<int>(i);
            if (state.aggregator.calendar_period() != CalendarPeriod::NONE) {
                state.aggregator.set_native_periods(
                    periods[i].stamps, periods[i].trade_instants,
                    calendar_period_for(native_security_feeds_[i].tf));
            }
            const auto& bars = native_security_feeds_[i].bars;
            state.native_bars_by_label.reserve(bars.size());
            for (const Bar& bar : bars) {
                state.native_bars_by_label[label_of(session_covered_instant_ms(
                    bar.timestamp, syminfo_.timezone, syminfo_.session))] = bar;
            }
            break;
        }
        if (state.native_feed_index >= 0) continue;
        // (b) A calendar week / month with no feed of its own: TradingView's
        // W/M bar is the native daily bars of the period aggregated (see the
        // header above), so build it from the installed daily feed, whose
        // stamps become this evaluator's period partition as well. A daily
        // bar belongs to the W/M period of its TRADE DATE -- the session-day
        // its last chart bar falls in, the aggregator's own group under the
        // native partition (a 13:30Z-stamped NYSE:F bar is its session
        // date's week; a 22:00Z-stamped ES1! bar opens the 17:00 CT session
        // of the NEXT trading date, so Sunday's bar is Monday's and starts
        // the week; the Thu 07-03 17:00 stamp that runs through the
        // Independence-Day session to Mon 07-07 is Monday's week) -- and the
        // bucket is labelled by the group's first stamp, which is where the
        // aggregator labels the bucket the chart's first bar of that period
        // opens (a holiday Monday leaves both on Tuesday). Periods the daily
        // feed only partly covers yield partial buckets, exactly as a partly
        // covered chart would.
        const CalendarPeriod period = state.aggregator.calendar_period();
        if (period != CalendarPeriod::WEEK && period != CalendarPeriod::MONTH) {
            continue;
        }
        for (std::size_t i = 0; i < native_security_feeds_.size(); ++i) {
            if (native_security_feeds_[i].seconds != kSecPerDay) continue;
            state.aggregator.set_native_periods(periods[i].stamps,
                                                periods[i].trade_instants,
                                                CalendarPeriod::DAY);
            const auto& days = native_security_feeds_[i].bars;
            bool open = false;
            int64_t period_open = 0;
            int64_t label = 0;
            Bar bucket{};
            for (const Bar& day : days) {
                const int64_t covered = session_covered_instant_ms(
                    day.timestamp, syminfo_.timezone, syminfo_.session);
                const int64_t key = state.aggregator.bucket_open_ms(covered);
                if (!open || key != period_open) {
                    if (open) state.native_bars_by_label[label] = bucket;
                    open = true;
                    period_open = key;
                    label = label_of(covered);
                    bucket = day;
                    bucket.timestamp = label;
                    continue;
                }
                bucket.high = std::max(bucket.high, day.high);
                bucket.low = std::min(bucket.low, day.low);
                bucket.close = day.close;
                bucket.volume += day.volume;
            }
            if (open) state.native_bars_by_label[label] = bucket;
            state.native_feed_index = static_cast<int>(i);
            break;
        }
    }
}


// The chart symbol's own D period from the native daily feed. TradingView's
// chart-level D consumers on an exchange-calendar intraday chart -- time("D"),
// ta.change(time("D")), timeframe.change("1D"), ta.vwap's default anchor --
// follow the exchange's trade-date daily bars, the very bars the "D" feed
// holds, not the nominal 17:00 CT session opens: a CME holiday session's
// 17:00 reopen stays inside the D bar that opened before the holiday (pinned
// 2026-09-05, round 7 family O, ledger log-20260905t123531z-7fe6b95a, lab tv
// o-cme-dayanchor-full: 255 D periods 2025-04-01 .. 2026-05-01, every start a
// registry daily-feed row; the Mon 05-26 17:00 CT reopen reads time("D") =
// Sun 05-25 17:00, timeframe.change("1D") false, ta.vwap cumulating on). The
// partition is the feed's stamps with each period's trade day as the
// session-day of its last chart bar (timeframe.hpp NativeDayPartition), built
// only when a "D" feed is installed on an intraday chart; run() installs it
// for the bar loop. No feed, or a calendar chart: empty, every rule nominal.
void BacktestEngine::prepare_chart_day_partition(const Bar* input_bars,
                                                 int n_input) {
    chart_day_partition_ = NativeDayPartition{};
    if (native_security_feeds_.empty()) return;
    if (calendar_period_for(input_tf_) != CalendarPeriod::NONE) return;
    for (const NativeSecurityFeed& feed : native_security_feeds_) {
        if (feed.seconds != kSecPerDay || feed.bars.empty()) continue;
        std::vector<int64_t> stamps;
        stamps.reserve(feed.bars.size());
        for (const Bar& bar : feed.bars) stamps.push_back(bar.timestamp);
        build_native_day_partition(chart_day_partition_, syminfo_.timezone,
                                   syminfo_.session, stamps, input_bars,
                                   n_input);
        return;
    }
}


bool BacktestEngine::substitute_native_security_bar(SecurityEvalState& state,
                                                    Bar& bar,
                                                    bool count_miss) {
    if (state.native_feed_index < 0) return false;
    // The completion path hands a bucket already stamped with its label; the
    // historical lookahead projection hands the raw first-child timestamp
    // (prepare_historical_security_lookahead_projections), the label only
    // when that child traded at the period's day stamp. Both read one key.
    const int64_t label = state.aggregator.bar_label_ms(bar.timestamp);
    const auto found = state.native_bars_by_label.find(label);
    if (found == state.native_bars_by_label.end()) {
        if (count_miss) ++diag_native_security_misses_;
        return false;
    }
    const Bar& native = found->second;
    bar.open = native.open;
    bar.high = native.high;
    bar.low = native.low;
    bar.close = native.close;
    bar.volume = native.volume;
    ++diag_native_security_substitutions_;
    return true;
}

}  // namespace pineforge
