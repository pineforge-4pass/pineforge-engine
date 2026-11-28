/*
 * engine_security.cpp — the generic higher-timeframe evaluator: registration,
 * the aggregate / substitute / dispatch step, the dispatch into
 * evaluate_security(). Publication semantics are not here: a native
 * subscription's are the consumer's delivery rules, TradingView's are the
 * source host's (src/source/pine_security_eval.cpp).
 */

#include "engine_internal.hpp"
#include "native_execution_consumer.hpp"

#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {

using namespace internal;

std::optional<RuntimeSubdivision> runtime_subdivision(
        const std::string& input_timeframe, const std::string& requested_timeframe) {
    int ratio = 0, seconds = 0;
    if (!internal::supports_lower_tf_emulation(
            input_timeframe, requested_timeframe, &ratio, &seconds)) return std::nullopt;
    return RuntimeSubdivision{ratio, seconds};
}

std::vector<Bar> runtime_subdivide(const Bar& bar, RuntimeSubdivision subdivision) {
    if (subdivision.ratio <= 0 || subdivision.seconds <= 0)
        throw std::invalid_argument("invalid runtime bar subdivision");
    return internal::synthesize_lower_tf_bars(bar, subdivision.ratio, subdivision.seconds);
}

bool BacktestEngine::runtime_context_early_close_completes() const {
    return session_template_knows_early_close();
}
// Internal to native_calendar.cpp and declared by this one caller: see its definition there.
namespace native_calendar { inline namespace native_calendar_v2 { void forget_interval_days() noexcept; } }
// --- register_security_eval ---
void BacktestEngine::register_security_eval(int sec_id, const std::string& requested_tf,
                                             const std::string& input_tf) {
    native_calendar::forget_interval_days();  // this thread's kept session days end here
    SecurityEvalState state;
    state.sec_id = sec_id;
    state.tf = requested_tf;

    const std::string& evaluator_input_tf =
        security_input_tf_.empty() ? input_tf : security_input_tf_;
    state.evaluator_input_tf = evaluator_input_tf;
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

// Whether an early close (a session-day ending before its template's end)
// completes a calendar period. Keyed by the instrument class in
// SymInfo::type -- the vocabulary the frozen C ABI's strategy_set_syminfo_type
// fixes ("stock" / "futures" / "index" / "fund" vs the continuous-session
// OTC classes "forex" / "cfd" / "crypto"), so the rule is the kernel's own
// market-structure classification, not a source-language one (the source-runtime extraction
// ruling, ADR-0001). OTC classes never close a day early: their session
// template has no holiday knowledge, so the period completes on the next
// session's first bar instead (the OANDA pins in timeframe.hpp).
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
    // warm up over the first `length` evaluated bars, exactly as before. On the
    // pump's runtime block when it has one (the per-pump state cache).
    AmbientBarContextScope bar_scope(NativeExecutionConsumer::bound(*this).pump_ambient(),
                                     static_cast<long long>(bar_index), 0);
    evaluate_security(state.sec_id, bar, publish);
}


bool BacktestEngine::native_fixed_security_clock(const SecurityEvalState& state) const {
    if (state.native_feed_index < 0 || !state.aggregator.supports_fixed_completion())
        return false;
    const auto requested = internal::tf_to_clock_seconds(state.tf);
    const auto input = internal::tf_to_clock_seconds(state.evaluator_input_tf);
    if (input > 0 && requested % input != 0) return true;
    // Fine input can divide the request while its observable chart does not.
    // Ordinary divisible and calendar subscriptions keep their old step.
    const auto view = NativeExecutionConsumer::bound(*this).view();
    const auto chart = native_calendar::parse_timeframe(
        view.spec ? view.spec->script_tf : script_tf_);
    if (!chart || !chart->is_fixed()) return false;
    const auto chart_seconds = static_cast<std::int64_t>(chart->count())
        * (chart->unit() == native_calendar::TimeframeUnit::Minute ? 60 : 1);
    return chart_seconds > 0 && requested > chart_seconds && requested % chart_seconds != 0;
}

std::optional<AggregatedBar> BacktestEngine::next_native_fixed_security_bar(
        SecurityEvalState& state, int64_t horizon_ms,
        int64_t input_ms, int64_t next_input_ms) {
    while (auto completed = state.aggregator.next_fixed_completion(
               state.fixed_clock, horizon_ms, input_ms, next_input_ms)) {
        const bool native = substitute_native_security_bar(
            state, completed->bar, completed->sub_bar_count > 0);
        if (native || completed->sub_bar_count > 0) return completed;
        // An unobserved period with no authoritative row has no payload.
    }
    return std::nullopt;
}

void BacktestEngine::feed_security_eval_state(
        SecurityEvalState& state, const Bar& input_bar) {
    feed_security_eval_state(state, input_bar,
        security_input_tf_.empty() ? input_tf_ : security_input_tf_);
}

void BacktestEngine::feed_security_eval_state(
        SecurityEvalState& state, const Bar& input_bar, const std::string& feed_tf) {
    // The next input bar's timestamp (0 when unknown) lets a calendar
    // bucket complete on the period's actual last input bar -- see
    // security_next_input_ms_.
    AggregatedBar ab = state.aggregator.feed(
        input_bar, security_next_input_ms_, 0, feed_tf, state.tf);
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
