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
#include <iterator>
#include <stdexcept>

namespace pineforge {

using namespace internal;


// --- register_security_eval ---
void source::PineStrategyHost::register_security_eval(
        int sec_id, const std::string& requested_tf, const std::string& input_tf,
        bool lookahead_on, bool gaps_on, bool heikinashi) {
    const std::size_t before = security_eval_states_.size();
    // Generated configure_security_evaluators() opens with
    // security_eval_states_.clear(): the first site registered into an empty
    // registry starts this host's per-site table over with it.
    if (before == 0) pine_security_states_.clear();
    BacktestEngine::register_security_eval(sec_id, requested_tf, input_tf);
    if (security_eval_states_.size() <= before) return;
    SecurityEvalState& state = security_eval_states_.back();
    PineSecurityEvalState& pine = pine_security_states_[sec_id];
    pine = PineSecurityEvalState{};
    pine.gaps_on = gaps_on;
    pine.lookahead_on = lookahead_on;
    pine.heikinashi = heikinashi;

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


bool source::PineStrategyHost::security_series_slot_is_new(int sec_id) const noexcept {
    if (security_history_publication_replay_) {
        return false;
    }
    for (const auto& state : security_eval_states_) {
        if (state.sec_id != sec_id) {
            continue;
        }
        const auto pine = pine_security_states_.find(sec_id);
        const bool lookahead_on =
            pine != pine_security_states_.end() && pine->second.lookahead_on;
        return !lookahead_on || state.current_sub_bar_count <= 1;
    }
    return true;
}


void source::PineStrategyHost::prune_pine_security_states() {
    for (auto it = pine_security_states_.begin(); it != pine_security_states_.end();) {
        bool registered = false;
        for (const auto& state : security_eval_states_) {
            if (state.sec_id == it->first) {
                registered = true;
                break;
            }
        }
        it = registered ? std::next(it) : pine_security_states_.erase(it);
    }
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

void source::PineStrategyHost::validate_security_timeframes(const std::string& input_tf) {
    if (input_tf.empty()) {
        if (!security_eval_states_.empty()) {
            throw std::runtime_error(
                "request.security cannot infer input timeframe from available input bars; pass input_tf explicitly"
            );
        }
        return;
    }

    // Note: script_tf >= input_tf is enforced separately in
    // BacktestEngine::run() (see engine_run.cpp ~line 199), which throws
    // "script timeframe must be coarser than or equal to input timeframe"
    // when the script_tf/input_tf ratio is invalid. We do not re-check
    // that invariant here.
    int input_seconds = tf_to_seconds(input_tf);
    int script_seconds = script_tf_seconds_;
    for (auto& state : security_eval_states_) {
        PineSecurityEvalState& pine = pine_security_state(state.sec_id);
        state.lower_tf_requested = false;
        state.lower_tf_emulation = false;
        state.lower_tf_ratio = 0;
        state.lower_tf_seconds = 0;
        state.lower_tf_use_input = false;
        state.lower_tf_input_aggregation_ratio = 1;
        state.lower_tf_input_buffer.clear();
        pine.publish_gate_tf_seconds = 0;
        pine.calling_close_completes_partial = false;
        state.calling_open_latches_first = false;
        state.first_bucket_published = false;
        state.deferred_aux.clear();
        if (state.tf.empty()) continue;

        int lower_ratio = 0;
        int lower_seconds = 0;
        bool ltf_supported = supports_lower_tf_emulation(
            input_tf, state.tf, &lower_ratio, &lower_seconds);
        if (ltf_supported && state.lower_tf_array_requested) {
            // Only request.security_lower_tf may opt into LTF emulation.
            // Scalar request.security remains a validate-time refusal even
            // when registration recognized an integer-divisor lower TF.
            state.lower_tf_requested = true;
            ensure_supported_lower_tf_emulation_flags(pine.lookahead_on, pine.gaps_on);
            state.lower_tf_emulation = true;
            state.lower_tf_ratio = lower_ratio;
            state.lower_tf_seconds = lower_seconds;
            continue;
        }

        // Parse the requested TF defensively so a garbage literal like
        // "abc" produces a precise diagnostic instead of an opaque
        // std::invalid_argument from stoi.
        int requested_seconds = safe_tf_to_seconds(state.tf);
        // Calendar month ("M" / "NM") has no fixed second count; tf_to_seconds
        // returns -1 as a calendar marker (a genuine parse failure returns 0).
        // Month is always a COARSER HTF, so admit it for request.security and
        // let the CALENDAR TimeframeAggregator (tf_ratio == -1) aggregate it.
        // request.security_lower_tf("M") stays invalid — month is never an
        // intrabar TF. (Weekly/daily already pass: they return positive seconds.)
        bool is_calendar_month = (requested_seconds == -1 && !state.lower_tf_array_requested);
        if (requested_seconds <= 0 && !is_calendar_month) {
            const char* api = state.lower_tf_array_requested
                ? "request.security_lower_tf" : "request.security";
            throw std::runtime_error(
                std::string(api) + ": invalid timeframe literal '" + state.tf + "'"
            );
        }

        if (!is_calendar_month && requested_seconds < input_seconds) {
            // Finer than input — only valid for security_lower_tf with
            // an integer divisor ratio.
            if (!state.lower_tf_array_requested) {
                throw std::runtime_error(
                    "request.security: requested timeframe '" + state.tf
                    + "' is finer than input '" + input_tf
                    + "'. Use request.security_lower_tf for sub-input timeframes."
                );
            }
            // LTF case: must be an exact integer divisor.
            if (input_seconds % requested_seconds != 0) {
                throw std::runtime_error(
                    "request.security_lower_tf: requested timeframe '" + state.tf
                    + "' is not an integer divisor of input '" + input_tf
                    + "' (ratio " + std::to_string(
                        static_cast<double>(input_seconds) / requested_seconds)
                    + " is non-integer; chart bars cannot be evenly subdivided)"
                );
            }
            // Defensive: integer-ratio finer LTF should already have been
            // accepted by supports_lower_tf_emulation above. Reaching
            // here implies a mismatch between the two checks (e.g. a
            // non-fixed-intraday TF on one side).
            throw std::runtime_error(
                "request.security_lower_tf: internal error - passed integer-ratio check but emulation support returned false (requested '"
                + state.tf + "', input '" + input_tf + "')"
            );
        }

        // requested_seconds >= input_seconds: HTF or same TF. Valid for
        // request.security; for request.security_lower_tf this is the
        // input-passthrough path when the requested TF is also strictly
        // finer than the script TF.
        if (state.lower_tf_array_requested) {
            if (script_seconds <= 0) {
                throw std::runtime_error(
                    "request.security_lower_tf: script timeframe is unknown — cannot validate '"
                    + state.tf + "' against script TF"
                );
            }
            if (requested_seconds >= script_seconds) {
                throw std::runtime_error(
                    "request.security_lower_tf: requested timeframe '" + state.tf
                    + "' must be finer than script timeframe '" + script_tf_
                    + "'. Lower-TF API requires a strictly finer timeframe."
                );
            }
            if (script_seconds % requested_seconds != 0) {
                throw std::runtime_error(
                    "request.security_lower_tf: requested timeframe '" + state.tf
                    + "' must evenly divide script timeframe '"
                    + script_tf_ + "' (script_tf must be an integer multiple of requested TF)"
                );
            }
            if (requested_seconds % input_seconds != 0) {
                throw std::runtime_error(
                    "request.security_lower_tf: requested timeframe '" + state.tf
                    + "' is not an integer multiple of input '" + input_tf
                    + "' (cannot aggregate raw input bars to requested TF)"
                );
            }
            state.lower_tf_requested = true;
            state.lower_tf_use_input = true;
            state.lower_tf_input_aggregation_ratio =
                requested_seconds / input_seconds;
            state.lower_tf_ratio = script_seconds / requested_seconds;
            state.lower_tf_seconds = requested_seconds;
        } else if (!is_calendar_month && pine.lookahead_on
                   && script_seconds > 0
                   && requested_seconds < script_seconds
                   && script_seconds % requested_seconds == 0) {
            // Plain request.security with a target TF finer than the
            // script/chart TF (e.g. so2TF="5" on a 15m chart) and
            // lookahead=barmerge.lookahead_ON: TradingView merges the
            // FIRST intrabar of each calling bar, so a history-offset
            // read (``expr[1]``) must expose the value confirmed as of
            // the PREVIOUS calling bar's boundary, not whatever native
            // sub-period last completed inside the CURRENT calling bar.
            // Gate publication (see feed_security_eval_state) to only
            // the completion whose bucket end aligns with a script_tf
            // boundary.
            //
            // lookahead_OFF is deliberately EXCLUDED: TV's lookahead_off
            // merge takes the LAST intrabar of the calling bar, so the
            // exposed value (and any ``[k]`` offset off it) advances at
            // the security's own finer cadence — one push per completed
            // security period, exactly the ungated behavior. Gating it
            // regressed masayanfx-multi-time-score-strategy
            // (request.security(sym, "5", ta.highest(high, 20)[1],
            // barmerge.gaps_off, barmerge.lookahead_off) on a 15m chart)
            // from 100.0% to 93.7% trade parity vs TradingView, while
            // the gated lookahead_on case (3commas triple-RSI DCA,
            // so2Rsi = request.security(sym, "5", ta.rsi(close,7)[1],
            // lookahead=barmerge.lookahead_on)) needs the latch to hold
            // 100.0%.
            pine.publish_gate_tf_seconds = requested_seconds;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
            // Served by the auxiliary finer feed (the split-feed path every
            // @1D lane runs): TradingView reads the calling bar's FIRST
            // intrabar, so the evaluator publishes every completed bucket
            // and the chart body runs right after the first one of its
            // slice (feed_aux_security_for_chart_bar defers the rest) --
            // see calling_open_latches_first. The gate served the legacy
            // single-feed loop and stays there.
            if (aux_security_feed_enabled()) {
                pine.publish_gate_tf_seconds = 0;
                state.calling_open_latches_first = true;
            }
#endif
        }
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
        else if (!is_calendar_month && !pine.lookahead_on
                 && aux_security_feed_enabled()
                 && script_seconds > 0
                 && requested_seconds < script_seconds) {
            // Plain request.security, lookahead_OFF, target TF strictly
            // finer than the script/chart TF, served by the auxiliary finer
            // feed: TV's merge takes the LAST intrabar of the calling bar,
            // whatever its sub-bar count -- see the field's note. The
            // aggregator's own completions stay ungated (masayanfx above);
            // only a bucket still partial on the calling bar's last
            // auxiliary bar is finalized and published there.
            pine.calling_close_completes_partial = true;
        }
#endif
    }
}


void source::PineStrategyHost::publish_security_eval_state_at_calling_boundary(
        SecurityEvalState& state) {
    if (pine_security_state(state.sec_id).publish_gate_tf_seconds <= 0
            || state.feed_count <= 0) {
        return;
    }

    struct ReplayScope {
        bool& active;
        bool previous;
        explicit ReplayScope(bool& flag)
            : active(flag), previous(flag) { active = true; }
        ~ReplayScope() { active = previous; }
    } replay_scope(security_history_publication_replay_);

    // The boundary-triggering input belongs to the next calling bar. Publish
    // the final requested value that was already evaluated for the completed
    // caller, before the chart body runs and before that retained input is fed.
    // security_series_slot_is_new() returns false in this scope, so generated
    // TA sites recompute the current slot instead of advancing their cadence —
    // under the same requested-context bar index as the evaluation replayed.
    dispatch_security_eval(state, state.current_bar, true,
                           state.ta_bar_index >= 0 ? state.ta_bar_index
                                                   : state.eval_complete_count);
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
    PineSecurityEvalState& pine = pine_security_state(state.sec_id);
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
    auto apply_ha = [&pine](Bar& b, bool commit) {
        double ha_close = (b.open + b.high + b.low + b.close) / 4.0;
        double ha_open = pine.ha_seeded
                             ? (pine.ha_prev_open + pine.ha_prev_close) / 2.0
                             : (b.open + b.close) / 2.0;
        double ha_high = std::max(b.high, std::max(ha_open, ha_close));
        double ha_low = std::min(b.low, std::min(ha_open, ha_close));
        b.open = ha_open;
        b.high = ha_high;
        b.low = ha_low;
        b.close = ha_close;
        if (commit) {
            pine.ha_prev_open = ha_open;
            pine.ha_prev_close = ha_close;
            pine.ha_seeded = true;
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
        if (pine.heikinashi) {
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
        if (pine.heikinashi) apply_ha(ab.bar, /*commit=*/true);
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
        if (pine.publish_gate_tf_seconds > 0 && script_tf_seconds_ > 0) {
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
        const bool boundary_emission = pine.lookahead_on
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
            if (pine.heikinashi) apply_ha(fresh, /*commit=*/false);
            state.current_bar = fresh;
            state.current_sub_bar_count = 1;
            state.eval_partial_count++;
            const bool peek_publish = pine.publish_gate_tf_seconds > 0
                && calling_bar_complete;
            dispatch_security_eval(state, fresh, peek_publish,
                                   state.eval_complete_count);
        }
    } else if (pine.lookahead_on) {
        if (pine.heikinashi) apply_ha(ab.bar, /*commit=*/false);
        state.current_bar = ab.bar;
        state.eval_partial_count++;
        // A shortened calling bar can complete while the finer requested
        // bucket is partial. Publish that current requested-context value to
        // merged history without adding a second evaluator/TA dispatch.
        const bool publish = pine.publish_gate_tf_seconds > 0
            && calling_bar_complete;
        // Partial (in-progress) bucket: the index the completion will carry.
        dispatch_security_eval(state, ab.bar, publish,
                               state.eval_complete_count);
    } else {
        state.current_bar = ab.bar;
        if (pine.gaps_on) {
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
    if (calling_bar_complete && pine.calling_close_completes_partial
        && state.aggregator.has_pending_partial()) {
        AggregatedBar tail = state.aggregator.complete_pending_partial();
        if (tail.is_complete) {
            state.current_sub_bar_count = tail.sub_bar_count;
            substitute_native_security_bar(state, tail.bar);
            if (pine.heikinashi) apply_ha(tail.bar, /*commit=*/true);
            state.current_bar = tail.bar;
            state.eval_complete_count++;
            state.last_published_label = tail.bar.timestamp;
            dispatch_security_eval(state, tail.bar, true,
                                   state.eval_complete_count - 1);
        }
    }
}

}  // namespace pineforge
