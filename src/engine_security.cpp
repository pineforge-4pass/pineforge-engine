/*
 * engine_security.cpp — request.security registration + per-eval state feeding
 */

#include "engine_internal.hpp"

#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {

using namespace internal;


// --- register_security_eval ---
void BacktestEngine::register_security_eval(int sec_id, const std::string& requested_tf,
                                             const std::string& input_tf) {
    SecurityEvalState state;
    state.sec_id = sec_id;
    state.tf = requested_tf;

    const std::string& evaluator_input_tf =
        security_input_tf_.empty() ? input_tf : security_input_tf_;
    if (!evaluator_input_tf.empty()) {
        int ratio = tf_ratio(evaluator_input_tf, requested_tf);
        if (ratio > 1) {
            state.aggregator = TimeframeAggregator(requested_tf, evaluator_input_tf,
                syminfo_.timezone, syminfo_.session);
        } else if (ratio == -1) {
            state.aggregator = TimeframeAggregator(requested_tf, evaluator_input_tf,
                syminfo_.timezone, syminfo_.session);
        }
        // ratio <= 0: passthrough (same or finer timeframe)
        state.aggregator.set_early_close_completes(
            session_template_knows_early_close());
    }
    security_eval_states_.push_back(std::move(state));
}


bool BacktestEngine::session_template_knows_early_close() const {
    std::string kind = syminfo_.type;
    for (char& c : kind) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return kind != "forex" && kind != "cfd" && kind != "crypto";
}


int BacktestEngine::security_lower_tf_sub_bar_index(int sec_id) const {
    for (const auto& state : security_eval_states_) {
        if (state.sec_id == sec_id) {
            return state.lower_tf_sub_bar_index;
        }
    }
    return 0;
}


// Safe wrapper around tf_to_seconds: returns <=0 on any parse failure
// (including std::invalid_argument from stoi on garbage like "abc").
// We use this instead of letting stoi escape so we can attach the
// offending literal to the diagnostic.
static int safe_tf_to_seconds(const std::string& tf) {
    try {
        return tf_to_seconds(tf);
    } catch (...) {
        return 0;
    }
}

void BacktestEngine::dispatch_security_eval(SecurityEvalState& state,
                                            const Bar& bar, bool publish,
                                            int64_t bar_index) {
    state.ta_bar_index = bar_index;
    // The requested context starts at its own bar 0 (origin 0): its TA members
    // warm up over the first `length` evaluated bars, exactly as before.
    ta::BarContextScope bar_scope(static_cast<long long>(bar_index), 0);
    evaluate_security(state.sec_id, bar, publish);
}


bool BacktestEngine::security_input_precedes_range_start(
        const SecurityEvalState& state, int64_t input_ts) const {
    if (security_range_start_na_warmup_) {
        // TradingView's deep-backtest request.security series are built from the
        // HTF bars whose OPEN lies inside the loaded chart range: a bucket that
        // opened before the range start is not a partial first bar, it is absent.
        // Keying the cut on the bucket open (session_period_open_ms for D/W/M,
        // the session-anchored intraday grid otherwise) reproduces that; keying
        // on the input timestamp would let the pre-range remainder of that bucket
        // pose as HTF bar 1 and shift every SMA-seeded EMA/RSI/ATR/Stoch by one
        // bucket (OANDA:EURUSD 1700-1700: the week opening Sun 17:00 ET before
        // the range start, the month opening Feb 28 17:00 ET before it). With a
        // range start on the bucket grid — 24x7 UTC midnight for every intraday
        // TF and D, Monday for W, the 1st for M — this is the timestamp cut.
        // Lower-TF (passthrough) evaluators keep the timestamp cut exactly.
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        // A strictly-coarser-than-chart calendar evaluator whose partition is
        // the chart's own dailies (a "W" / "M" on a 1D chart: the implicit
        // native feed of prepare_native_security_feeds, which begins at the
        // range start) labels the bucket in progress at the epoch by its
        // first IN-RANGE stamp -- Tue 2025-04-01 for the week that opened Mon
        // 03-31 -- so the bucket-open cut above keeps it as bar 0 and every
        // SMA-seeded weekly EMA runs one bar early (round 8, family P:
        // hungpixi-macd-enhanced-mtf on BINANCE:BTCUSDT 1D under the ladder's
        // range-start-na-warmup candidate, lab tv famp-sense-hp-btc1d
        // 2026-09-05: TradingView's weekly EMA26 first reads on the 09-29
        // week and its signal EMA9 on the 11-24 week -- weekly bar 0 is the
        // 04-07 week -- while the engine's read one week earlier; on the
        // 11-24 week TradingView's hist[1] is still na (score -6) where the
        // engine's is numeric (score -10), and the 11-25 reversal waited for
        // 11-28). TradingView's rule is the default path's: the bucket is
        // absent iff its NOMINAL open precedes the epoch and the symbol
        // traded between the two (the auxiliary 1m feed is the evidence, so
        // the NSE week whose holiday Monday never traded stays kept). The
        // chart's own timeframe keeps the chart series' first bar, and a
        // native partition with pre-range history (the 15m lanes' explicit
        // daily feed) already opened that bucket before the epoch, so the
        // rule below agrees with the cut above wherever both apply.
        if (aux_security_feed_enabled() && !state.lower_tf_requested
            && !state.lower_tf_array_requested) {
            const CalendarPeriod period = calendar_period_for(state.tf);
            const CalendarPeriod chart_period = calendar_period_for(script_tf_);
            const bool strictly_coarser = period != CalendarPeriod::NONE
                && static_cast<int>(period) > static_cast<int>(chart_period);
            if (strictly_coarser) {
                const int64_t nominal_open = session_period_open_ms(
                    input_ts, syminfo_.timezone, syminfo_.session, period);
                if (nominal_open < security_range_start_ms_
                    && aux_security_traded_between(nominal_open,
                                                   security_range_start_ms_)) {
                    return true;
                }
            }
        }
#endif
        return state.aggregator.bucket_open_ms(input_ts) < security_range_start_ms_;
    }
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    // R19 OTC daily pins: a historical intraday chart beginning mid-session
    // has no partial first D bar in request.security, even when this run has
    // only the chart feed. XAUUSD from Apr1 00:00Z reads D time/close as na
    // that day and ATR14[1] first becomes numeric Apr23; counting the partial
    // Mar31 session seeds ATR a day early. Restrict the no-aux inference to
    // the pinned daily OTC clock. W/M need trading-history evidence to retain
    // a holiday-open period, and native/aux feeds keep their existing rules.
    if (!aux_security_feed_enabled() && native_security_feeds_.empty()
        && security_first_chart_bar_ms_ > 0
        && script_tf_seconds_ > 0 && script_tf_seconds_ < 86400
        && (state.tf == "D" || state.tf == "1D")
        && !state.lower_tf_requested && !state.lower_tf_array_requested
        && (syminfo_.type == "forex" || syminfo_.type == "cfd")
        && !stream_warmup_mode_ && stream_phase_ == StreamPhase::IDLE) {
        const int64_t stamp = session_period_open_ms(
            input_ts, syminfo_.timezone, syminfo_.session, CalendarPeriod::DAY);
        // Metals stamp the D bar at 17:00 ET but first trade at 18:00.
        // Starting at that actual open keeps the complete first session.
        const int64_t trading_open = session_covered_instant_ms(
            stamp, syminfo_.timezone, syminfo_.session);
        return trading_open < security_first_chart_bar_ms_;
    }
    // Default cut (round 8, family P), split-feed runs only: a coarser-than-
    // chart or chart-timeframe series starts at the first bucket that OPENS at
    // or after the run's first chart bar -- the deep-backtest range start --
    // and the bucket in progress at that instant is absent, exactly as
    // TradingView's is (security_first_chart_bar_ms_). "In progress" is a
    // fact about trading, not the calendar: TradingView dates a bar by its
    // first traded bar, so the NSE week whose Monday 2025-03-31 was a holiday
    // opens on Tuesday 04-01 -- the range start -- and is kept (famp-sense-
    // nifty15full: "W" first reads 08-22), while the NYSE week that traded
    // Monday 03-31 (F: 08-29), the CME trade date that opened Mon 22:00Z
    // (ES/NQ "D": 05-01) and the OANDA week that opened Sun 21:00Z (XAUUSD
    // 1D "W": 08-28) are absent. The auxiliary 1m feed holds the pre-range
    // bars (the lanes' finer feeds begin in 2021), so the bucket whose
    // nominal open precedes the first chart bar is dropped iff the feed
    // traded between that open and the first chart bar. A finer-than-chart
    // evaluator is never cut (its slice begins at the first chart bar), and
    // a single-feed run has no evidence and keeps its feed-start series.
    if (security_first_chart_bar_ms_ <= 0 || !aux_security_feed_enabled()
        || state.lower_tf_requested || state.lower_tf_array_requested
        || script_tf_seconds_ <= 0) {
        return false;
    }
    const int requested_seconds = safe_tf_to_seconds(state.tf);
    const bool coarser_or_chart = requested_seconds == -1
        || requested_seconds >= script_tf_seconds_;
    if (!coarser_or_chart) {
        return false;
    }
    const CalendarPeriod period = calendar_period_for(state.tf);
    const int64_t nominal_open = period != CalendarPeriod::NONE
        ? session_period_open_ms(input_ts, syminfo_.timezone, syminfo_.session,
                                 period)
        : session_intraday_bucket_open_ms(input_ts, requested_seconds,
                                          syminfo_.timezone, syminfo_.session);
    if (nominal_open >= security_first_chart_bar_ms_) {
        return false;
    }
    return aux_security_traded_between(nominal_open, security_first_chart_bar_ms_);
#else
    return false;
#endif
}


#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
bool BacktestEngine::aux_security_traded_between(int64_t from_ms,
                                                 int64_t to_ms) const {
    const Bar* bars = nullptr;
    int n = 0;
    source_aux_security_input_view(bars, n);
    if (bars == nullptr || n <= 0) return false;
    auto it = std::lower_bound(
        bars, bars + n, from_ms,
        [](const Bar& bar, int64_t ts) { return bar.timestamp < ts; });
    return it != bars + n && it->timestamp < to_ms;
}
#endif


void BacktestEngine::feed_security_eval_state(
        SecurityEvalState& state, const Bar& input_bar) {
    // Opt-in KI-55 HTF warmup parity (security_range_start_na_warmup run flag):
    //   (a) start every request.security aggregation at range_start_ms, not the
    //       feed start — drop every input bar whose HTF bucket opened before
    //       the range start (security_input_precedes_range_start) so the
    //       aggregator, its TA members, and the exposed history all begin at
    //       the first WHOLE bucket opening at/after the range start;
    //   (b) its embedded lookback ta.ema na-warms per TV built-in semantics —
    //       scoped by raising ta::ema_na_warmup_flag() for the duration of this
    //       call, which covers every evaluate_security() dispatch below (each of
    //       which is the only place the security's EMA members compute());
    //   (c) plain security expressions (e.g. `close`) read na until the first
    //       COMPLETED HTF bar from the range start — a consequence of (a): under
    //       lookahead_off no evaluate_security() fires until that first whole
    //       bucket completes; a bucket straddling the range start never counts.
    // All three collapse to no-ops when the flag is unset (byte-identical).
    if (security_input_precedes_range_start(state, input_bar.timestamp)) {
        return;
    }
    struct SecurityNaWarmupScope {
        bool prev_;
        explicit SecurityNaWarmupScope(bool on)
            : prev_(ta::ema_na_warmup_flag()) { ta::ema_na_warmup_flag() = on; }
        ~SecurityNaWarmupScope() { ta::ema_na_warmup_flag() = prev_; }
    } _na_warmup_scope(security_range_start_na_warmup_);

    if (state.lower_tf_use_input) {
        // Buffer raw input bars until we accumulate one full script-TF
        // chunk, then aggregate (if req > input) and dispatch each
        // resulting LTF bar to the codegen via evaluate_security. The
        // codegen detects ``lower_tf_sub_bar_index == 0`` to clear its
        // accumulator vector and pushes once per dispatch.
        //
        // Bucket-aware dispatch (mirrors feed_ratio_mode in
        // src/timeframe.cpp:270): when the incoming bar belongs to a
        // different wall-clock script-TF bucket than the buffered
        // window, we MUST flush the buffer (even if partial) BEFORE
        // pushing — otherwise a feed gap, warmup misalignment, or
        // sparse-data run leaks bars across the chart-bar boundary
        // (e.g. a 16-bar window dispatched per 15m chart bar instead
        // of 15). Pure count-based dispatch is preserved as a
        // secondary trigger so dense gap-free feeds still flush
        // promptly when the chunk fills.
        int input_seconds = tf_to_seconds(security_input_tf_);
        int script_seconds = script_tf_seconds_;
        if (input_seconds <= 0 || script_seconds <= 0) {
            // Cannot compute bucket math — fall back to original
            // count-only behaviour.
            state.lower_tf_input_buffer.push_back(input_bar);
            return;
        }
        int chunk_size = script_seconds / input_seconds;
        if (chunk_size <= 0) {
            state.lower_tf_input_buffer.push_back(input_bar);
            return;
        }
        // The buffer fills to at most chunk_size input bars before it is
        // dispatched and cleared. Reserve once (no-op when capacity already
        // suffices) so the repeated fill/clear cycle reuses one allocation.
        state.lower_tf_input_buffer.reserve(static_cast<std::size_t>(chunk_size));
        int64_t bucket_ms = static_cast<int64_t>(script_seconds) * 1000;
        int64_t this_bucket = input_bar.timestamp / bucket_ms;
        bool boundary_crossed = false;
        if (!state.lower_tf_input_buffer.empty()) {
            int64_t buffer_bucket =
                state.lower_tf_input_buffer.front().timestamp / bucket_ms;
            if (this_bucket != buffer_bucket) {
                boundary_crossed = true;
            }
        }

        // Lambda: aggregate + dispatch the current buffer (length may
        // be < chunk_size on a boundary-triggered partial flush) and
        // clear it. Uses the same agg_ratio rollup as before but is
        // length-driven by the actual buffer size rather than
        // chunk_size, so partial windows don't index out of bounds.
        auto dispatch_and_clear = [&]() {
            int agg_ratio = state.lower_tf_input_aggregation_ratio;
            if (agg_ratio < 1) agg_ratio = 1;
            int buf_len = static_cast<int>(state.lower_tf_input_buffer.size());
            std::vector<Bar> ltf_bars;
            ltf_bars.reserve(static_cast<std::size_t>(buf_len / agg_ratio + 1));
            if (agg_ratio == 1) {
                for (const Bar& b : state.lower_tf_input_buffer) {
                    ltf_bars.push_back(b);
                }
            } else {
                for (int i = 0; i + agg_ratio <= buf_len; i += agg_ratio) {
                    Bar acc = state.lower_tf_input_buffer[
                        static_cast<std::size_t>(i)];
                    double vol = acc.volume;
                    for (int j = 1; j < agg_ratio; ++j) {
                        const Bar& nxt = state.lower_tf_input_buffer[
                            static_cast<std::size_t>(i + j)];
                        if (nxt.high > acc.high) acc.high = nxt.high;
                        if (nxt.low < acc.low) acc.low = nxt.low;
                        acc.close = nxt.close;
                        vol += nxt.volume;
                    }
                    acc.volume = vol;
                    ltf_bars.push_back(acc);
                }
            }
            state.lower_tf_sub_bar_index = 0;
            for (const Bar& b : ltf_bars) {
                state.feed_count++;
                state.current_bar = b;
                state.current_sub_bar_count = 1;
                state.eval_complete_count++;
                dispatch_security_eval(state, b, true,
                                       state.eval_complete_count - 1);
                state.lower_tf_sub_bar_index++;
            }
            state.lower_tf_input_buffer.clear();
        };

        if (boundary_crossed) {
            // Flush stale bucket BEFORE pushing the new bar so the
            // new bar starts a fresh window aligned to its own bucket.
            dispatch_and_clear();
        }

        state.lower_tf_input_buffer.push_back(input_bar);

        // Secondary trigger: if the buffer happens to fill to
        // chunk_size mid-bucket (the dense gap-free case), flush
        // immediately. This preserves the original count-based
        // behaviour for the common path.
        if (static_cast<int>(state.lower_tf_input_buffer.size()) >= chunk_size) {
            dispatch_and_clear();
        }
        return;
    }
    if (state.lower_tf_emulation) {
        std::vector<Bar> synthetic_bars =
            synthesize_lower_tf_bars(input_bar, state.lower_tf_ratio, state.lower_tf_seconds);
        if (synthetic_bars.empty()) {
            throw std::runtime_error(
                "request.security lower TF emulation could not synthesize bars for requested "
                + state.tf + " from input timeframe " + security_input_tf_
            );
        }
        // Reset the sub-bar counter at the start of every chart bar's
        // synthesis so a ``request.security_lower_tf`` codegen path can
        // detect index 0 and clear its accumulator vector before pushing
        // each per-sub-bar value. The counter is incremented after every
        // dispatch so callers see 0, 1, ..., ratio-1 in sequence.
        state.lower_tf_sub_bar_index = 0;
        for (const auto& synthetic_bar : synthetic_bars) {
            state.feed_count++;
            state.current_bar = synthetic_bar;
            state.current_sub_bar_count = 1;
            state.eval_complete_count++;
            dispatch_security_eval(state, synthetic_bar, true,
                                   state.eval_complete_count - 1);
            state.lower_tf_sub_bar_index++;
        }
        return;
    }

    if (historical_security_lookahead_projection_active_
            && !state.historical_projections.empty()) {
        // Keyed by the input's instant, not by a feed-call index: on the
        // split-feed path this evaluator is fed the finer auxiliary slice
        // (hundreds of inputs per chart bar), and the projection of a bucket
        // is dispatched on the first input at or after its first retained
        // child (that child's own first auxiliary bar) -- exactly the chart
        // bar TradingView's lookahead_on leaks the bucket's FINAL values
        // from. On the single-feed path the first input at or after the
        // child's timestamp is that child itself, as the index cut was.
        const int64_t input_ms = input_bar.timestamp;
        while (state.historical_projection_cursor + 1
                    < state.historical_projections.size()
                && state.historical_projections[
                       state.historical_projection_cursor + 1]
                       .first_child_ms <= input_ms) {
            ++state.historical_projection_cursor;
            state.historical_projection_dispatched = false;
        }
        const auto& projection = state.historical_projections[
            state.historical_projection_cursor];
        state.feed_count++;
        if (state.historical_projection_dispatched
                || input_ms < projection.first_child_ms) {
            // gaps_off holds the first-child projection unchanged until the
            // next HTF bucket. No evaluator call means TA/security histories
            // also advance exactly once per projected bucket.
            return;
        }
        state.historical_projection_dispatched = true;

        Bar projected_bar = projection.bar;
        // A projected bucket is the exchange's bar wherever a native feed
        // serves this timeframe -- complete or not. TradingView's
        // lookahead_on leaks the period's FINAL values from its first chart
        // bar, so the trailing period still in progress at the range end
        // carries the whole native period whenever the feed holds it (lab tv
        // wm-m-f15-jul, 2026-09-05: August's final o/h/l/c 10.92/11.99/
        // 10.68/11.77 from 08-01 09:30 on a chart ending 08-08). A partial
        // with no native bar keeps the available aggregate, uncounted as a
        // miss.
        substitute_native_security_bar(state, projected_bar,
                                       /*count_miss=*/projection.is_complete);
        state.current_bar = projected_bar;
        // The projected HTF bucket is introduced on its first chart child, so
        // generated security series must allocate a fresh history/TA slot even
        // though projected_bar itself already contains every available child.
        state.current_sub_bar_count = 1;
        if (projection.is_complete) {
            state.eval_complete_count++;
        } else {
            state.eval_partial_count++;
        }
        dispatch_security_eval(state, projected_bar, projection.is_complete,
                               projection.is_complete
                                   ? state.eval_complete_count - 1
                                   : state.eval_complete_count);
        return;
    }

    // The next input bar's timestamp (0 when unknown) lets a calendar
    // bucket complete on the period's actual last chart bar -- see
    // security_next_input_ms_ -- and the calling chart bar's nominal close
    // (split-feed path, else 0) lets an OTC bucket do so exactly when that
    // close reaches the period's -- see security_calling_close_ms_.
    AggregatedBar ab = state.aggregator.feed(input_bar, security_next_input_ms_,
                                             security_calling_close_ms_);
    state.feed_count++;
    state.current_sub_bar_count = ab.sub_bar_count;
    if (ab.is_complete) {
        // The aggregator decided WHEN the bucket completes; a native feed for
        // this timeframe decides WHAT it closed at (the settlement / official
        // print).
        substitute_native_security_bar(state, ab.bar);
        state.current_bar = ab.bar;
        state.eval_complete_count++;
        state.last_published_label = ab.bar.timestamp;
        dispatch_security_eval(state, ab.bar, true,
                               state.eval_complete_count - 1);
    } else {
        state.current_bar = ab.bar;
    }
}

}  // namespace pineforge
