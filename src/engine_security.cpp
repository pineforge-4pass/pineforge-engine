/*
 * engine_security.cpp — request.security registration + per-eval state feeding
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;


// --- register_security_eval ---
void BacktestEngine::register_security_eval(int sec_id, const std::string& requested_tf,
                                             const std::string& input_tf,
                                             bool lookahead_on, bool gaps_on,
                                             bool heikinashi) {
    SecurityEvalState state;
    state.sec_id = sec_id;
    state.tf = requested_tf;
    state.gaps_on = gaps_on;
    state.lookahead_on = lookahead_on;
    state.heikinashi = heikinashi;

    if (!input_tf.empty()) {
        int lower_ratio = 0;
        int lower_seconds = 0;
        if (supports_lower_tf_emulation(input_tf, requested_tf, &lower_ratio, &lower_seconds)) {
            ensure_supported_lower_tf_emulation_flags(lookahead_on, gaps_on);
            state.lower_tf_requested = true;
            state.lower_tf_emulation = true;
            state.lower_tf_ratio = lower_ratio;
            state.lower_tf_seconds = lower_seconds;
        } else {
            int ratio = tf_ratio(input_tf, requested_tf);
            if (ratio > 1) {
                state.aggregator = TimeframeAggregator(requested_tf, input_tf);
            } else if (ratio == -1) {
                state.aggregator = TimeframeAggregator(requested_tf, input_tf);
            }
            // ratio <= 0: passthrough (same or unsupported lower TF)
        }
    }
    security_eval_states_.push_back(std::move(state));
}


// --- register_security_lower_tf_eval ---
// ``request.security_lower_tf`` is a strict subset of ``request.security``:
// the requested TF must be finer than the chart's input TF and lookahead /
// gaps are pinned off (TV does not expose them on this builtin). We reuse
// the existing eval-state plumbing and set ``lower_tf_array_requested``
// so ``validate_security_timeframes`` can produce a precise diagnostic
// when the chart's input TF makes lower-TF emulation impossible.
void BacktestEngine::register_security_lower_tf_eval(
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

void BacktestEngine::validate_security_timeframes(const std::string& input_tf) {
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
        state.lower_tf_requested = false;
        state.lower_tf_emulation = false;
        state.lower_tf_ratio = 0;
        state.lower_tf_seconds = 0;
        state.lower_tf_use_input = false;
        state.lower_tf_input_aggregation_ratio = 1;
        state.lower_tf_input_buffer.clear();
        state.publish_gate_tf_seconds = 0;
        if (state.tf.empty()) continue;

        int lower_ratio = 0;
        int lower_seconds = 0;
        bool ltf_supported = supports_lower_tf_emulation(
            input_tf, state.tf, &lower_ratio, &lower_seconds);
        if (ltf_supported && state.lower_tf_array_requested) {
            // Only request.security_lower_tf may opt into LTF emulation.
            // request.security with a finer TF must be rejected even
            // when the ratio happens to be an integer — see the
            // finer-than-input check below.
            state.lower_tf_requested = true;
            ensure_supported_lower_tf_emulation_flags(state.lookahead_on, state.gaps_on);
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
        } else if (!is_calendar_month && script_seconds > 0
                   && requested_seconds < script_seconds
                   && script_seconds % requested_seconds == 0) {
            // Plain request.security with a target TF finer than the
            // script/chart TF (e.g. so2TF="5" on a 15m chart): the
            // security's own aggregator completes multiple times per
            // calling bar. A history-offset read (``expr[1]``) must expose
            // the value confirmed as of the PREVIOUS calling bar, not
            // whatever native sub-period last completed inside the
            // CURRENT calling bar. Gate publication (see
            // feed_security_eval_state) to only the completion whose
            // bucket end aligns with a script_tf boundary.
            state.publish_gate_tf_seconds = requested_seconds;
        }
    }
}


bool BacktestEngine::security_series_slot_is_new(int sec_id) const {
    for (const auto& state : security_eval_states_) {
        if (state.sec_id != sec_id) {
            continue;
        }
        return !state.lookahead_on || state.current_sub_bar_count <= 1;
    }
    return true;
}


void BacktestEngine::feed_security_eval_state(SecurityEvalState& state, const Bar& input_bar) {
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
        int input_seconds = tf_to_seconds(input_tf_);
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
                evaluate_security(state.sec_id, b, true);
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
                + state.tf + " from input timeframe " + input_tf_
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
            evaluate_security(state.sec_id, synthetic_bar, true);
            state.lower_tf_sub_bar_index++;
        }
        return;
    }

    AggregatedBar ab = state.aggregator.feed(input_bar);
    state.feed_count++;
    state.current_sub_bar_count = ab.sub_bar_count;
    // Heikin-Ashi same-symbol read: replace the completed (aggregated) bar's
    // OHLC with its HA candle before evaluating the security expression, so
    // close/open/high/low inside request.security see HA values (TradingView
    // ticker.heikinashi semantics):
    //   haClose = (O+H+L+C)/4
    //   haOpen  = seeded ? (prevHaOpen+prevHaClose)/2 : (O+C)/2
    //   haHigh  = max(H, haOpen, haClose);  haLow = min(L, haOpen, haClose)
    // HA is stateful; the running open/close advance once per COMPLETED bar
    // (committed below). A partial lookahead peek derives from prior state
    // without advancing it. The lambda mutates ab.bar in place.
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
    if (ab.is_complete) {
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
            int64_t bucket_end_sec =
                ab.bar.timestamp / 1000 + state.publish_gate_tf_seconds;
            publish = (bucket_end_sec % script_tf_seconds_) == 0;
        }
        evaluate_security(state.sec_id, ab.bar, publish);
    } else if (state.lookahead_on) {
        if (state.heikinashi) apply_ha(ab.bar, /*commit=*/false);
        state.current_bar = ab.bar;
        state.eval_partial_count++;
        evaluate_security(state.sec_id, ab.bar, false);
    } else {
        state.current_bar = ab.bar;
        if (state.gaps_on) {
            clear_security(state.sec_id);
        }
    }
}

}  // namespace pineforge
