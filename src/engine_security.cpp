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
    // The next input bar's timestamp (0 when unknown) lets a calendar
    // bucket complete on the period's actual last input bar -- see
    // security_next_input_ms_.
    AggregatedBar ab = state.aggregator.feed(input_bar, security_next_input_ms_);
    state.feed_count++;
    state.current_sub_bar_count = ab.sub_bar_count;
    if (ab.is_complete) {
        // The aggregator decided WHEN the bucket completes; a native feed for
        // this timeframe decides WHAT it closed at (the settlement / official
        // print).
        substitute_native_security_bar(state, ab.bar);
        state.current_bar = ab.bar;
        state.eval_complete_count++;
        dispatch_security_eval(state, ab.bar, true,
                               state.eval_complete_count - 1);
    } else {
        state.current_bar = ab.bar;
    }
}

}  // namespace pineforge
