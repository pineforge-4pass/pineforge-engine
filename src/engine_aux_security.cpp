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
    guard_native_mutation("set_native_security_feed");
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
    // On the split-feed path the chart's own bars ARE the native feed of the
    // chart timeframe: TradingView's request.security(tickerid, <the chart's
    // timeframe>, expr) is the chart series itself, and its "W" / "M" on a
    // daily chart are those daily bars aggregated -- never the auxiliary 1m
    // slice re-aggregated (round 7 family M mechanism 4: on NYSE:F 1D the
    // 1m aggregate closes 12.195 on 2026-04-08 where the chart bar closes
    // 12.18, so amandaborgeson06's "D" stoch read K 90.84 > D 90.60 and
    // TradingView's chart K 90.13 < D 90.32; sensor tape scratchpad/r7/pins/
    // m1d-amanda-sense-f). An explicit native feed of that timeframe (the
    // 15m lanes' FEED_1D on a 1D chart is the same file) keeps precedence;
    // the implicit feed lives only for this preparation.
    bool pushed_chart_feed = false;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled() && input_bars != nullptr && n_input > 0
        && script_tf_seconds_ > 0) {
        bool have_chart_tf_feed = false;
        for (const auto& feed : native_security_feeds_) {
            if (feed.seconds == script_tf_seconds_) {
                have_chart_tf_feed = true;
                break;
            }
        }
        if (!have_chart_tf_feed) {
            NativeSecurityFeed chart_feed;
            chart_feed.tf = script_tf_;
            chart_feed.seconds = script_tf_seconds_;
            chart_feed.bars.assign(input_bars, input_bars + n_input);
            native_security_feeds_.push_back(std::move(chart_feed));
            pushed_chart_feed = true;
        }
    }
#endif
    struct ImplicitChartFeedGuard {
        std::vector<NativeSecurityFeed>& feeds;
        bool active;
        ~ImplicitChartFeedGuard() {
            if (active && !feeds.empty()) feeds.pop_back();
        }
    } implicit_chart_feed_guard{native_security_feeds_, pushed_chart_feed};
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
    if (source_aux_security_feed_enabled()) {
        source_aux_security_input_view(input_bars, n_input);
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
