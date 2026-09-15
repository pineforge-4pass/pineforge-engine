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
    current_script_open_ms_ = 0; saw_open_fill_ = false;
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

void PineScheduler::snapshot_coof_script_state(PineStrategyHost& host) {
    if (language_._src_series_active_) {
        language_.coof_checkpoint_src_open_ = language_._src_open_;
        language_.coof_checkpoint_src_high_ = language_._src_high_;
        language_.coof_checkpoint_src_low_ = language_._src_low_;
        language_.coof_checkpoint_src_close_ = language_._src_close_;
        language_.coof_checkpoint_src_volume_ = language_._src_volume_;
        language_.coof_checkpoint_src_hl2_ = language_._src_hl2_;
        language_.coof_checkpoint_src_hlc3_ = language_._src_hlc3_;
        language_.coof_checkpoint_src_ohlc4_ = language_._src_ohlc4_;
        language_.coof_checkpoint_src_hlcc4_ = language_._src_hlcc4_;
    }
    language_.coof_checkpoint_prev_chart_close_ = language_.prev_chart_close_;
    language_.coof_checkpoint_last_chart_close_ = language_.last_chart_close_;
    language_.coof_checkpoint_contains_current_bar_ = false;
    host.snapshot_script_state();
}

void PineScheduler::restore_coof_script_state(PineStrategyHost& host) {
    if (language_._src_series_active_) {
        language_._src_open_ = language_.coof_checkpoint_src_open_;
        language_._src_high_ = language_.coof_checkpoint_src_high_;
        language_._src_low_ = language_.coof_checkpoint_src_low_;
        language_._src_close_ = language_.coof_checkpoint_src_close_;
        language_._src_volume_ = language_.coof_checkpoint_src_volume_;
        language_._src_hl2_ = language_.coof_checkpoint_src_hl2_;
        language_._src_hlc3_ = language_.coof_checkpoint_src_hlc3_;
        language_._src_ohlc4_ = language_.coof_checkpoint_src_ohlc4_;
        language_._src_hlcc4_ = language_.coof_checkpoint_src_hlcc4_;
    }
    language_.prev_chart_close_ = language_.coof_checkpoint_prev_chart_close_;
    language_.last_chart_close_ = language_.coof_checkpoint_last_chart_close_;
    host.restore_script_state();
}

void PineScheduler::commit_coof_script_state(PineStrategyHost& host) {
    if (language_._src_series_active_) {
        language_.coof_checkpoint_src_open_ = language_._src_open_;
        language_.coof_checkpoint_src_high_ = language_._src_high_;
        language_.coof_checkpoint_src_low_ = language_._src_low_;
        language_.coof_checkpoint_src_close_ = language_._src_close_;
        language_.coof_checkpoint_src_volume_ = language_._src_volume_;
        language_.coof_checkpoint_src_hl2_ = language_._src_hl2_;
        language_.coof_checkpoint_src_hlc3_ = language_._src_hlc3_;
        language_.coof_checkpoint_src_ohlc4_ = language_._src_ohlc4_;
        language_.coof_checkpoint_src_hlcc4_ = language_._src_hlcc4_;
    }
    language_.coof_checkpoint_prev_chart_close_ = language_.prev_chart_close_;
    language_.coof_checkpoint_last_chart_close_ = language_.last_chart_close_;
    language_.coof_checkpoint_contains_current_bar_ = true;
    host.commit_script_state();
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
    host.scheduler_prepare_script_run(retained_.bars, static_eligible,
                                      expected_source_bars_, !needs_aggregation);
    if (!static_eligible) host.scheduler_configure_security_evaluators();
    uses_aux_security_feed_ = host.scheduler_uses_aux_security_feed();
    host.scheduler_prepare_security_sequence(retained_.bars);
}

void PineScheduler::publish_series(const Bar& bar, PineStrategyHost& host) {
    (void)host;
    update_source_series(bar);
}

void PineScheduler::update_source_series(const Bar& bar) {
    if (language_.history_slot_is_new_)
        language_.prev_chart_close_ = language_.last_chart_close_;
    language_.last_chart_close_ = bar.close;
    if (!language_._src_series_active_) return;
    const double hl2 = (bar.high + bar.low) / 2.0;
    const double hlc3 = (bar.high + bar.low + bar.close) / 3.0;
    const double ohlc4 = (bar.open + bar.high + bar.low + bar.close) / 4.0;
    const double hlcc4 = (bar.high + bar.low + bar.close + bar.close) / 4.0;
    if (language_.history_slot_is_new_) {
        language_._src_open_.push(bar.open);
        language_._src_high_.push(bar.high);
        language_._src_low_.push(bar.low);
        language_._src_close_.push(bar.close);
        language_._src_volume_.push(bar.volume);
        language_._src_hl2_.push(hl2);
        language_._src_hlc3_.push(hlc3);
        language_._src_ohlc4_.push(ohlc4);
        language_._src_hlcc4_.push(hlcc4);
        return;
    }
    language_._src_open_.update(bar.open);
    language_._src_high_.update(bar.high);
    language_._src_low_.update(bar.low);
    language_._src_close_.update(bar.close);
    language_._src_volume_.update(bar.volume);
    language_._src_hl2_.update(hl2);
    language_._src_hlc3_.update(hlc3);
    language_._src_ohlc4_.update(ohlc4);
    language_._src_hlcc4_.update(hlcc4);
}

double PineScheduler::script_position_view(
        int bar_index, PositionSide side, double quantity) const noexcept {
    if (language_.pos_view_freeze_bar_ == bar_index) {
        if (language_.pos_view_frozen_side_ == PositionSide::LONG)
            return language_.pos_view_frozen_qty_;
        if (language_.pos_view_frozen_side_ == PositionSide::SHORT)
            return -language_.pos_view_frozen_qty_;
        return 0.0;
    }
    if (side == PositionSide::LONG) return quantity;
    if (side == PositionSide::SHORT) return -quantity;
    return 0.0;
}

void PineScheduler::freeze_script_position_view(
        int bar_index, PositionSide side, double quantity,
        const std::vector<PyramidEntry>& lots) {
    if (language_.pos_view_freeze_bar_ == bar_index) return;
    language_.pos_view_freeze_bar_ = bar_index;
    language_.pos_view_frozen_side_ = side;
    language_.pos_view_frozen_qty_ = quantity;
    language_.pos_view_frozen_entry_qty_.clear();
    for (const auto& lot : lots)
        language_.pos_view_frozen_entry_qty_[lot.entry_id] += lot.qty;
}

void PineScheduler::clear_script_position_view() noexcept {
    language_.pos_view_freeze_bar_ = -1;
}

const Series<double>& PineScheduler::source_series(const std::string& key) const {
    if (key == "open") return language_._src_open_;
    if (key == "high") return language_._src_high_;
    if (key == "low") return language_._src_low_;
    if (key == "close") return language_._src_close_;
    if (key == "volume") return language_._src_volume_;
    if (key == "hl2") return language_._src_hl2_;
    if (key == "hlc3") return language_._src_hlc3_;
    if (key == "ohlc4") return language_._src_ohlc4_;
    if (key == "hlcc4") return language_._src_hlcc4_;
    throw std::invalid_argument("unknown source series");
}

void PineScheduler::fixture_publish_source_series(const Bar& bar, bool new_history_slot) {
    language_.history_slot_is_new_ = new_history_slot;
    language_.is_first_tick_ = new_history_slot;
    update_source_series(bar);
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

void PineScheduler::bar_open(const Bar&, const NativeDecisionContext& context,
                             PineStrategyHost& host) {
    if (context.script_bar_open_ms != current_script_open_ms_) {
        current_script_open_ms_ = context.script_bar_open_ms;
        saw_open_fill_ = false;
        coof_callback_script_open_ = std::numeric_limits<std::int64_t>::min();
        if (host.scheduler_coof_enabled()) snapshot_coof_script_state(host);
    }
}

void PineScheduler::bar(const Bar& value, const NativeDecisionContext& context, PineStrategyHost& host) {
    // P7c: matching advances over every sub-bar; the language callback occurs
    // only at the terminal sub-bar with the script bar timestamp restored.
    language_.is_first_tick_ = context.is_terminal_sub_bar;
    language_.is_last_tick_ = context.is_terminal_sub_bar;
    language_.history_slot_is_new_ = context.is_terminal_sub_bar;
    if (!context.is_terminal_sub_bar) return;
    // A LegacyTolerant native stream can emit a quiet carried callback for
    // the calendar-aligned slot immediately preceding the last raw-label
    // warmup bar. The retired source stream starts its realtime cadence from
    // that warmup bar's next source slot, so it never published this stale
    // callback to generated code.
    if (retained_.is_stream && current_script_bar_valid_
        && context.script_bar_open_ms <= current_script_bar_.timestamp) {
        // The generic driver still dispatched a script point. Preserve the
        // public one-hash-per-dispatch accounting without exposing it to the
        // source callback cadence.
        host.scheduler_record_broker_hash();
        return;
    }
    const bool coof = host.scheduler_coof_enabled();
    const bool had_coof_recalc = coof_callback_script_open_
        == context.script_bar_open_ms;
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
    const int chart_index = context.coordinate.interval_index;
    if (uses_aux_security_feed_) host.scheduler_feed_aux_security(chart_index);
    if (coof) {
        restore_coof_script_state(host);
        language_.is_first_tick_ = true;
        language_.is_last_tick_ = true;
        language_.history_slot_is_new_ =
            !language_.coof_checkpoint_contains_current_bar_;
    }
    publish_series(script_bar, host);
    std::optional<std::int64_t> next_script_open_ms;
    if (const auto state = host.native_state(); state.spec
        && !state.spec->timeframe_undetected
        && tf_ratio(state.spec->input_tf, state.spec->script_tf) == 1
        && source_bar_count_ + 1 < static_cast<int>(retained_.bars.size())) {
        next_script_open_ms = retained_.bars[
            static_cast<std::size_t>(source_bar_count_ + 1)].timestamp;
    }
    if (const auto state = host.native_state(); state.spec
        && !state.spec->timeframe_undetected) {
        host.scheduler_update_session_state(script_bar, next_script_open_ms);
    }
    host.scheduler_publish_source_bar(script_bar, true, !had_coof_recalc);
    if (coof) commit_coof_script_state(host);
    if (uses_aux_security_feed_) host.scheduler_feed_deferred_aux_security(chart_index);
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
    if (!had_coof_recalc) ++source_bar_count_;
    if (terminal_source_bar()) {
        host.scheduler_record_range_end(current_script_bar_);
        if (!retained_.is_stream) host.scheduler_finish_security_sequence();
    }
    host.scheduler_record_broker_hash();
}

void PineScheduler::applied(const native_order::ExecutionAppliedEvent& event,
                            const NativeDecisionContext& context, PineStrategyHost& host) {
    if (event.ordinal <= applied_cursor_) return;
    applied_cursor_ = event.ordinal;
    if (!host.scheduler_coof_enabled()) return;
    const bool at_open = context.coordinate.path_phase == NativePathPhase::Open;
    const bool first_open = at_open && !saw_open_fill_;
    if (at_open) saw_open_fill_ = true;
    const bool first_callback = coof_callback_script_open_
        != context.script_bar_open_ms;
    const bool callback_advances_source_bar = first_callback
        && !language_.coof_checkpoint_contains_current_bar_;
    // COOF re-evaluates the source script against the full script bar while
    // the native current-execution coordinate still supplies the fill price
    // for sizing/placement.  A one-price synthetic callback erases high/low,
    // volume and barstate facts that the legacy scheduler retained.
    Bar callback_bar = current_script_bar_valid_
        && current_script_bar_.timestamp == context.script_bar_open_ms
        ? current_script_bar_ : host.current_bar_;
    callback_bar.timestamp = context.script_bar_open_ms;
    restore_coof_script_state(host);
    language_.is_first_tick_ = true;
    language_.is_last_tick_ = true;
    language_.history_slot_is_new_ =
        !language_.coof_checkpoint_contains_current_bar_;
    publish_series(callback_bar, host);
    host.adapter_.begin_coof_recalc(context, first_open);
    try {
        host.scheduler_publish_source_bar(
            callback_bar, true, callback_advances_source_bar);
    } catch (...) {
        host.adapter_.end_coof_recalc();
        throw;
    }
    host.adapter_.end_coof_recalc();
    restore_coof_script_state(host);
    coof_callback_script_open_ = context.script_bar_open_ms;
    if (callback_advances_source_bar) ++source_bar_count_;
    if (!first_open) return;
    constexpr std::uint64_t kNoFillEventBudget = std::numeric_limits<std::uint64_t>::max();
    constexpr std::size_t kCoofLoopGuard = 1U << 20;
    std::uint64_t budget = kNoFillEventBudget;
    std::size_t executed = 0;
    for (;;) {
        auto newborns = host.adapter_.take_first_open_newborns();
        if (newborns.empty()) break;
        for (const auto& handle : newborns) {
            if (budget == 0 || executed == kCoofLoopGuard)
                throw std::overflow_error("Pine COOF first-open loop guard exhausted");
            --budget;
            ++executed;
            (void)host.execute_current({handle, NativeCurrentPriceRule::NearestTick});
        }
    }
}

} // namespace pineforge::source
