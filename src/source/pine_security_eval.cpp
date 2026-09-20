#include <pineforge/source/pine_strategy_host.hpp>
/*
 * pine_security_eval.cpp — Pine's request.security evaluator semantics
 *
 * The kernel owns the higher-timeframe feed store and its routing: the
 * evaluator registry, the timeframe aggregator, the native feed store, the
 * dispatch into evaluate_security(). What TradingView does with a completed
 * or in-progress bucket -- barmerge.lookahead / barmerge.gaps publication,
 * ticker.heikinashi substitution, the range-start warmup, the historical
 * lookahead projection, request.security_lower_tf emulation, the calling
 * chart bar's publication boundary -- is this source host's, composed here
 * around the kernel's primitives.
 */

#include "../engine_internal.hpp"

#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pineforge {

using namespace internal;


// --- register_security_eval ---
void source::PineStrategyHost::register_security_eval(
        int sec_id, const std::string& requested_tf, const std::string& input_tf,
        bool lookahead_on, bool gaps_on, bool heikinashi) {
    const std::size_t before = security_eval_states_.size();
    BacktestEngine::register_security_eval(sec_id, requested_tf, input_tf);
    if (security_eval_states_.size() <= before) return;
    SecurityEvalState& state = security_eval_states_.back();
    state.gaps_on = gaps_on;
    state.lookahead_on = lookahead_on;
    state.heikinashi = heikinashi;

    const std::string& evaluator_input_tf =
        security_input_tf_.empty() ? input_tf : security_input_tf_;
    if (evaluator_input_tf.empty()) return;
    int lower_ratio = 0;
    int lower_seconds = 0;
    if (supports_lower_tf_emulation(evaluator_input_tf, requested_tf,
                                    &lower_ratio, &lower_seconds)) {
        // Registration precedes the final input-timeframe validation and
        // cannot yet distinguish request.security_lower_tf from a plain
        // request.security evaluator. The latter retains its own
        // lookahead/gaps contract, so the lower-TF-array restriction is
        // applied only after that identity is known below.
        state.lower_tf_requested = true;
        state.lower_tf_emulation = true;
        state.lower_tf_ratio = lower_ratio;
        state.lower_tf_seconds = lower_seconds;
        // A lower-timeframe site synthesizes its sub-bars; it has no
        // aggregator of its own.
        state.aggregator = TimeframeAggregator();
    }
}


// --- register_security_lower_tf_eval ---
// ``request.security_lower_tf`` is a strict subset of ``request.security``:
// the requested TF must be finer than the chart's input TF and lookahead /
// gaps are pinned off (TV does not expose them on this builtin). We reuse
// the existing eval-state plumbing and set ``lower_tf_array_requested``
// so ``validate_security_timeframes`` can produce a precise diagnostic
// when the chart's input TF makes lower-TF emulation impossible.
void source::PineStrategyHost::register_security_lower_tf_eval(
    int sec_id,
    const std::string& requested_tf,
    const std::string& input_tf
) {
    auto before = security_eval_states_.size();
    register_security_eval(sec_id, requested_tf, input_tf, false, false);
    if (security_eval_states_.size() > before) {
        security_eval_states_.back().lower_tf_array_requested = true;
    }
}


void source::PineStrategyHost::pine_feed_security_eval_state(
        SecurityEvalState& state, const Bar& input_bar,
        bool calling_bar_complete) {
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

    // Heikin-Ashi same-symbol read: replace an aggregated bar's OHLC with its
    // HA candle before evaluating the security expression. The completed
    // state advances once per committed HTF bucket; a partial/projection peek
    // derives from the prior state without committing it.
    auto apply_ha = [&state](Bar& b, bool commit) {
        double ha_close = (b.open + b.high + b.low + b.close) / 4.0;
        double ha_open = state.ha_seeded
                             ? (state.ha_prev_open + state.ha_prev_close) / 2.0
                             : (b.open + b.close) / 2.0;
        double ha_high = std::max(b.high, std::max(ha_open, ha_close));
        double ha_low = std::min(b.low, std::min(ha_open, ha_close));
        b.open = ha_open;
        b.high = ha_high;
        b.low = ha_low;
        b.close = ha_close;
        if (commit) {
            state.ha_prev_open = ha_open;
            state.ha_prev_close = ha_close;
            state.ha_seeded = true;
        }
    };

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
        if (state.heikinashi) {
            apply_ha(projected_bar, projection.is_complete);
        }
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
        // print), before any Heikin-Ashi derivation. Partial (lookahead_on)
        // peeks keep the running aggregate.
        substitute_native_security_bar(state, ab.bar);
        if (state.heikinashi) apply_ha(ab.bar, /*commit=*/true);
        state.current_bar = ab.bar;
        state.eval_complete_count++;
        // For a plain request.security whose target TF is strictly finer
        // than script_tf (publish_gate_tf_seconds > 0), the security's own
        // aggregator completes multiple times per calling/script bar.
        // Only the completion whose bucket END lands on a script_tf
        // boundary is "the last completion of THIS calling bar" — that's
        // the one a history-offset read (``expr[1]``) should latch as
        // "confirmed as of the previous calling bar" the NEXT time the
        // calling script reads it. Suppress ``is_complete`` (so codegen's
        // gated hist.push() does not fire) for every other, intermediate
        // completion; the underlying TA state keeps advancing regardless
        // (compute()/recompute() dispatch is driven by
        // current_sub_bar_count, not by this flag) — only the exposed
        // history buffer's advance is gated. eval_complete_count/current_bar
        // bookkeeping above stays driven by the real completion.
        bool publish = true;
        if (state.publish_gate_tf_seconds > 0 && script_tf_seconds_ > 0) {
            // Merge finer-context history on the event that the calling chart
            // aggregator actually completes. Unlike a fixed seconds modulus,
            // this includes session-clipped calling bars.
            // The requested-context evaluator still runs once
            // per input update; only its is_complete publication signal is
            // replaced.
            publish = calling_bar_complete;
        }
        // A boundary emission: the input bar opened a NEW bucket and the
        // aggregator emitted the previous one, still partial (a singleton the
        // count / real-end / session-close rules never reached: OANDA:XAUUSD
        // Thanksgiving's 21:54 bucket holding the 21:56 minute, emitted when
        // 21:59 opens the 21:57 bucket). Under lookahead_on that bucket's
        // history slot was already opened by its first sub-bar's partial
        // peek (compute), so the completion must REWRITE it (recompute) --
        // a fresh compute here committed a phantom copy of the bucket and
        // shifted every later requested value (the RSI diverged from the
        // lookahead_off twin's on the same buckets). And the input bar that
        // opened the new bucket got no peek of its own in this feed, so open
        // its slot now, exactly as the merge branch below does for a first
        // sub-bar. Count, real-end and session-close completions carry the
        // input bar's own bucket (the label matches) and are untouched, as
        // is lookahead_off (no peeks: every completion is a new slot).
        // The emitted bucket is a boundary emission exactly when it is not
        // the aggregator's CURRENT bucket: a boundary completion hands back
        // the previous bucket and re-seats the aggregator on the one the
        // input opened (feed_calendar_mode / feed_ratio_mode), while every
        // eager completion (count, real end, session close, the calendar
        // period's last input) leaves the completed bucket current. The
        // former test compared the label against bar_label_ms(input), which
        // is the intraday grid open for a fixed-TF bucket but the DAY stamp
        // for a calendar W / M bucket -- so every weekly completion on its
        // last daily bar looked like a boundary and committed a phantom copy
        // of the week as one more requested bar (round 7, family I: the
        // hungpixi weekly f_count carry decayed twice per week, hist ties
        // compared the week with its own copy).
        const bool boundary_emission = state.lookahead_on
            && state.aggregator.is_active()
            && ab.bar.timestamp != state.aggregator.current().timestamp;
        if (boundary_emission && state.current_sub_bar_count < 2) {
            state.current_sub_bar_count = 2;
        }
        state.last_published_label = ab.bar.timestamp;
        dispatch_security_eval(state, ab.bar, publish,
                               state.eval_complete_count - 1);
        if (boundary_emission) {
            Bar fresh = state.aggregator.current();
            if (state.heikinashi) apply_ha(fresh, /*commit=*/false);
            state.current_bar = fresh;
            state.current_sub_bar_count = 1;
            state.eval_partial_count++;
            const bool peek_publish = state.publish_gate_tf_seconds > 0
                && calling_bar_complete;
            dispatch_security_eval(state, fresh, peek_publish,
                                   state.eval_complete_count);
        }
    } else if (state.lookahead_on) {
        if (state.heikinashi) apply_ha(ab.bar, /*commit=*/false);
        state.current_bar = ab.bar;
        state.eval_partial_count++;
        // A shortened calling bar can complete while the finer requested
        // bucket is partial. Publish that current requested-context value to
        // merged history without adding a second evaluator/TA dispatch.
        const bool publish = state.publish_gate_tf_seconds > 0
            && calling_bar_complete;
        // Partial (in-progress) bucket: the index the completion will carry.
        dispatch_security_eval(state, ab.bar, publish,
                               state.eval_complete_count);
    } else {
        state.current_bar = ab.bar;
        if (state.gaps_on) {
            clear_security(state.sec_id);
        }
    }

    // The calling chart bar's last auxiliary bar left the finer bucket it
    // opened (or merged into) partial: its count, real end and session
    // close all lie beyond the chart bar's last sub-bar (the Thanksgiving
    // 21:57 3m bucket holding the 21:59 minute alone; the 2-minute 20:57
    // bucket of a day whose 20:59 minute did not trade). TradingView reads
    // that bucket at the chart bar's close (lab tv dca-ltf-last-intrabar,
    // 2026-09-05: 72.64, the 21:59 minute's RSI, where the previous bucket
    // reads 38.87), so finalize it now, exactly as a count / real-end /
    // session-close completion would have, and publish it as one more
    // completed requested-context bar. The aggregator marks it emitted:
    // the next chart bar's first sub-bar starts a fresh bucket without
    // re-emitting this one, and a later sub-bar of the same bucket (not a
    // completed chart bar's, but guarded) merges without completing it
    // again. A dense feed whose final bucket completed on its count has no
    // pending partial and is untouched (calling_close_completes_partial).
    if (calling_bar_complete && state.calling_close_completes_partial
        && state.aggregator.has_pending_partial()) {
        AggregatedBar tail = state.aggregator.complete_pending_partial();
        if (tail.is_complete) {
            state.current_sub_bar_count = tail.sub_bar_count;
            substitute_native_security_bar(state, tail.bar);
            if (state.heikinashi) apply_ha(tail.bar, /*commit=*/true);
            state.current_bar = tail.bar;
            state.eval_complete_count++;
            state.last_published_label = tail.bar.timestamp;
            dispatch_security_eval(state, tail.bar, true,
                                   state.eval_complete_count - 1);
        }
    }
}

}  // namespace pineforge
