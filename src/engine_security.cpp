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


void BacktestEngine::dispatch_security_eval(SecurityEvalState& state,
                                            const Bar& bar, bool publish,
                                            int64_t bar_index) {
    state.ta_bar_index = bar_index;
    // The requested context starts at its own bar 0 (origin 0): its TA members
    // warm up over the first `length` evaluated bars, exactly as before.
    ta::BarContextScope bar_scope(static_cast<long long>(bar_index), 0);
    evaluate_security(state.sec_id, bar, publish);
}


void BacktestEngine::feed_security_eval_state(
        SecurityEvalState& state, const Bar& input_bar) {
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
