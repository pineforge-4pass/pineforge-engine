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
                                             bool lookahead_on, bool gaps_on) {
    SecurityEvalState state;
    state.sec_id = sec_id;
    state.tf = requested_tf;
    state.gaps_on = gaps_on;
    state.lookahead_on = lookahead_on;

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
    for (auto& state : security_eval_states_) {
        state.lower_tf_requested = false;
        state.lower_tf_emulation = false;
        state.lower_tf_ratio = 0;
        state.lower_tf_seconds = 0;
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
        if (requested_seconds <= 0) {
            const char* api = state.lower_tf_array_requested
                ? "request.security_lower_tf" : "request.security";
            throw std::runtime_error(
                std::string(api) + ": invalid timeframe literal '" + state.tf + "'"
            );
        }

        if (requested_seconds < input_seconds) {
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
        // request.security; never valid for request.security_lower_tf.
        if (state.lower_tf_array_requested) {
            throw std::runtime_error(
                "request.security_lower_tf: requested timeframe '" + state.tf
                + "' is not finer than input '" + input_tf
                + "'. Lower-TF API requires a strictly finer timeframe."
            );
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
    state.current_bar = ab.bar;
    state.current_sub_bar_count = ab.sub_bar_count;
    if (ab.is_complete) {
        state.eval_complete_count++;
        evaluate_security(state.sec_id, ab.bar, true);
    } else if (state.lookahead_on) {
        state.eval_partial_count++;
        evaluate_security(state.sec_id, ab.bar, false);
    } else if (state.gaps_on) {
        clear_security(state.sec_id);
    }
}

}  // namespace pineforge
