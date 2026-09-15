#include <pineforge/source/pine_scheduler.hpp>

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include <stdexcept>
#include <utility>

namespace pineforge::source {

void PineScheduler::capture_begin(const NativeBeginArgs& args) {
    RetainedBegin next;
    if (args.bars && args.n > 0) next.bars.assign(args.bars, args.bars + args.n);
    next.input_tf = args.input_tf; next.script_tf = args.script_tf;
    next.bar_magnifier = args.bar_magnifier; next.magnifier_samples = args.magnifier_samples;
    next.distribution = args.magnifier_distribution;
    next.volume_weighted = args.magnifier_volume_weighted;
    next.volume_weighted_min_samples = args.magnifier_volume_weighted_min_samples;
    next.volume_weighted_max_samples = args.magnifier_volume_weighted_max_samples;
    next.is_stream = args.is_stream; next.warmup_n = args.warmup_n;
    retained_ = std::move(next);
}

void PineScheduler::reset_language() {
    language_.reset_for_run();
    language_.pos_view_freeze_bar_ = -1;
    language_.pos_view_frozen_side_ = PositionSide::FLAT;
    language_.pos_view_frozen_qty_ = 0.0;
    language_.pos_view_frozen_entry_qty_.clear();
    language_.is_first_tick_ = true; language_.is_last_tick_ = true;
    language_.history_slot_is_new_ = true; language_.coof_checkpoint_contains_current_bar_ = false;
    language_.coof_checkpoint_src_open_.clear(); language_.coof_checkpoint_src_high_.clear();
    language_.coof_checkpoint_src_low_.clear(); language_.coof_checkpoint_src_close_.clear();
    language_.coof_checkpoint_src_volume_.clear(); language_.coof_checkpoint_src_hl2_.clear();
    language_.coof_checkpoint_src_hlc3_.clear(); language_.coof_checkpoint_src_ohlc4_.clear();
    language_.coof_checkpoint_src_hlcc4_.clear();
    coof_.clear(); current_script_open_ms_ = 0; saw_open_fill_ = false;
    current_script_bar_ = {}; current_script_bar_valid_ = false;
    source_bar_count_ = 0; expected_source_bars_ = 0; applied_cursor_ = 0;
    coof_callback_script_open_ = std::numeric_limits<std::int64_t>::min();
    prior_input_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    awaiting_legacy_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    input_script_completes_.clear();
    input_script_boundary_completes_.clear();
    uses_aux_security_feed_ = false;
    deferred_boundary_input_ = {};
}

void PineScheduler::run_begin(PineStrategyHost& host) {
    reset_language();
    const bool static_eligible = !retained_.is_stream && !retained_.bar_magnifier
        && retained_.input_tf.empty() && retained_.script_tf.empty();
    const auto state = host.native_state();
    const bool undetected = state.spec && state.spec->timeframe_undetected;
    const int ratio = undetected || !state.spec ? 1
        : tf_ratio(state.spec->input_tf, state.spec->script_tf);
    const bool needs_aggregation = ratio > 1 || ratio == -1;
    input_script_completes_.assign(retained_.bars.size(), 1U);
    input_script_boundary_completes_.assign(retained_.bars.size(), 0U);
    if (needs_aggregation && state.spec) {
        TimeframeAggregator preview(state.spec->script_tf, state.spec->input_tf,
                                    state.spec->timezone, state.spec->session);
        for (std::size_t i = 0; i < retained_.bars.size(); ++i) {
            const AggregatedBar aggregate = preview.feed(retained_.bars[i]);
            input_script_completes_[i] = aggregate.is_complete ? 1U : 0U;
            input_script_boundary_completes_[i] = aggregate.is_complete
                && tf_change(aggregate.bar.timestamp, retained_.bars[i].timestamp,
                             state.spec->script_tf, state.spec->timezone,
                             state.spec->session) ? 1U : 0U;
        }
    }
    expected_source_bars_ = 0;
    for (const auto complete : input_script_completes_) {
        expected_source_bars_ += complete != 0U ? 1 : 0;
    }
    host.scheduler_prepare_script_run(retained_.bars, static_eligible, expected_source_bars_);
    host.scheduler_configure_security_evaluators();
    uses_aux_security_feed_ = host.scheduler_uses_aux_security_feed();
    host.scheduler_prepare_security_sequence(retained_.bars);
}

void PineScheduler::publish_series(const Bar& bar, PineStrategyHost& host) {
    if (language_.history_slot_is_new_) language_.prev_chart_close_ = language_.last_chart_close_;
    language_.last_chart_close_ = bar.close;
    host.scheduler_push_source_series(bar);
}

void PineScheduler::input(
        const Bar& bar, const NativeInputContext& context, PineStrategyHost& host) {
    if (uses_aux_security_feed_) {
        prior_input_script_open_ms_ = context.script_interval.open_ms;
        return;
    }
    std::int64_t next_input_ms = 0;
    if (context.input_index >= 0
        && context.input_index + 1 < static_cast<int>(retained_.bars.size())) {
        next_input_ms = retained_.bars[static_cast<std::size_t>(context.input_index + 1)].timestamp;
    }
    // The generic calendar may wait for a later tradable opening before it
    // seals a script interval.  The source chart aggregator can have already
    // completed that interval on the prior raw bar.  Keep the new raw input
    // out of request.security until the pending script callback observes the
    // same legacy point; then feed it immediately after that callback.
    if (awaiting_legacy_script_open_ms_
        != std::numeric_limits<std::int64_t>::min()) {
        deferred_boundary_input_.bar = bar;
        deferred_boundary_input_.next_input_ms = next_input_ms;
        deferred_boundary_input_.prior_script_open_ms = awaiting_legacy_script_open_ms_;
        deferred_boundary_input_.calling_bar_complete = false;
        deferred_boundary_input_.all_security_states = true;
        deferred_boundary_input_.active = true;
        prior_input_script_open_ms_ = context.script_interval.open_ms;
        return;
    }

    bool calling_bar_complete = context.completes_script_interval;
    bool boundary = prior_input_script_open_ms_
        != std::numeric_limits<std::int64_t>::min()
        && prior_input_script_open_ms_ != context.script_interval.open_ms;
    if (context.input_index >= 0
        && context.input_index < static_cast<int>(input_script_completes_.size())) {
        calling_bar_complete = input_script_completes_[
            static_cast<std::size_t>(context.input_index)] != 0U;
        boundary = input_script_boundary_completes_[
            static_cast<std::size_t>(context.input_index)] != 0U;
    }
    const bool deferred_gate = host.scheduler_feed_security_input(
        bar, next_input_ms, calling_bar_complete, boundary);
    if (deferred_gate) {
        deferred_boundary_input_.bar = bar;
        deferred_boundary_input_.next_input_ms = next_input_ms;
        deferred_boundary_input_.prior_script_open_ms = prior_input_script_open_ms_;
        deferred_boundary_input_.calling_bar_complete = false;
        deferred_boundary_input_.all_security_states = false;
        deferred_boundary_input_.active = true;
    }
    if (calling_bar_complete) {
        awaiting_legacy_script_open_ms_ = boundary
            ? prior_input_script_open_ms_ : context.script_interval.open_ms;
    }
    prior_input_script_open_ms_ = context.script_interval.open_ms;
}

void PineScheduler::bar_open(const Bar&, const NativeDecisionContext& context, PineStrategyHost&) {
    if (context.script_bar_open_ms != current_script_open_ms_) {
        current_script_open_ms_ = context.script_bar_open_ms;
        saw_open_fill_ = false;
        coof_callback_script_open_ = std::numeric_limits<std::int64_t>::min();
    }
}

void PineScheduler::bar(const Bar& value, const NativeDecisionContext& context, PineStrategyHost& host) {
    // P7c: matching advances over every sub-bar; the language callback occurs
    // only at the terminal sub-bar with the script bar timestamp restored.
    language_.is_first_tick_ = context.is_terminal_sub_bar;
    language_.is_last_tick_ = context.is_terminal_sub_bar;
    language_.history_slot_is_new_ = context.is_terminal_sub_bar;
    host.is_first_tick_ = language_.is_first_tick_;
    host.is_last_tick_ = language_.is_last_tick_;
    host.history_slot_is_new_ = language_.history_slot_is_new_;
    if (!context.is_terminal_sub_bar) return;
    // A COOF recalc at this script bar is the source evaluation for that bar;
    // do not issue a second terminal callback with a new source-bar index.
    if (host.scheduler_coof_enabled() && coof_callback_script_open_ == context.script_bar_open_ms) {
        ++source_bar_count_;
        return;
    }
    Bar script_bar = value;
    script_bar.timestamp = context.script_bar_open_ms;
    current_script_bar_ = script_bar;
    current_script_bar_valid_ = true;
    const bool completes_awaiting_legacy_script = awaiting_legacy_script_open_ms_
        == context.script_bar_open_ms;
    if (deferred_boundary_input_.active
        && !deferred_boundary_input_.all_security_states
        && deferred_boundary_input_.prior_script_open_ms == context.script_bar_open_ms) {
        host.scheduler_publish_security_boundary();
    }
    if (uses_aux_security_feed_) {
        host.scheduler_feed_aux_security(source_bar_count_);
    }
    publish_series(script_bar, host);
    host.scheduler_publish_source_bar(script_bar, true);
    if (uses_aux_security_feed_) {
        host.scheduler_feed_deferred_aux_security(source_bar_count_);
    }
    if (deferred_boundary_input_.active
        && deferred_boundary_input_.prior_script_open_ms == context.script_bar_open_ms) {
        if (deferred_boundary_input_.all_security_states) {
            (void)host.scheduler_feed_security_input(
                deferred_boundary_input_.bar, deferred_boundary_input_.next_input_ms,
                deferred_boundary_input_.calling_bar_complete, false);
        } else {
            host.scheduler_feed_deferred_security_input(
                deferred_boundary_input_.bar, deferred_boundary_input_.next_input_ms);
        }
        deferred_boundary_input_ = {};
    }
    if (completes_awaiting_legacy_script) {
        awaiting_legacy_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    }
    ++source_bar_count_;
    if (terminal_source_bar()) {
        host.scheduler_record_range_end(current_script_bar_);
        if (!retained_.is_stream) host.scheduler_finish_security_sequence();
    }
}

void PineScheduler::applied(const native_order::ExecutionAppliedEvent& event,
                            const NativeDecisionContext& context, PineStrategyHost& host) {
    if (event.ordinal <= applied_cursor_) return;
    applied_cursor_ = event.ordinal;
    if (!host.scheduler_coof_enabled()) return;
    const bool at_open = context.coordinate.path_phase == NativePathPhase::Open;
    const bool first_open = at_open && !saw_open_fill_;
    if (at_open) saw_open_fill_ = true;
    coof_.push_back({event.ordinal, context.script_bar_open_ms, first_open});
    const Bar point{event.resolved_price, event.resolved_price, event.resolved_price,
                    event.resolved_price, 0.0, context.script_bar_open_ms};
    language_.is_first_tick_ = true; language_.is_last_tick_ = false;
    language_.history_slot_is_new_ = false;
    host.is_first_tick_ = language_.is_first_tick_;
    host.is_last_tick_ = language_.is_last_tick_;
    host.history_slot_is_new_ = language_.history_slot_is_new_;
    host.adapter_.begin_coof_recalc(context, first_open);
    try {
        host.scheduler_publish_source_bar(point, true, first_open);
    } catch (...) {
        host.adapter_.end_coof_recalc();
        throw;
    }
    host.adapter_.end_coof_recalc();
    coof_callback_script_open_ = context.script_bar_open_ms;
    if (first_open) ++source_bar_count_;
    if (!first_open) return;
    auto newborns = host.adapter_.take_first_open_newborns();
    constexpr std::size_t kCoofLoopGuard = 1U << 20;
    if (newborns.size() > kCoofLoopGuard)
        throw std::overflow_error("Pine COOF first-open loop guard exhausted");
    for (const auto& handle : newborns)
        (void)host.execute_current({handle, NativeCurrentPriceRule::NearestTick});
}

} // namespace pineforge::source
