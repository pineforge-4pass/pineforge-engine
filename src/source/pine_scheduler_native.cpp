#include <pineforge/source/pine_scheduler.hpp>

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
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
    next.simple_run = args.simple_run;
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
    current_script_open_ms_ = 0; saw_open_fill_ = false; open_point_fills_ = 0;
    current_script_bar_ = {}; current_script_bar_valid_ = false;
    source_bar_count_ = 0; expected_source_bars_ = 0; applied_cursor_ = 0;
    coof_callback_script_open_ = std::numeric_limits<std::int64_t>::min();
    last_published_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    prior_input_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    awaiting_legacy_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    last_stream_input_open_ms_ = std::numeric_limits<std::int64_t>::min();
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
    host.stream_warmup_mode_ = retained_.is_stream;
    host.scheduler_prepare_script_run(retained_.bars, static_eligible,
                                      expected_source_bars_, !needs_aggregation);
    // ab9714be pine_scheduler.cpp:717-804 (legacy_run_simple) never configures
    // the request.security evaluator surface; :1249-1323 (run_tf_impl, reached
    // by every timeframe-aware, magnified or stream overload, including the
    // generated wrapper's run_backtest / run_backtest_full with EMPTY
    // timeframes) configures it unconditionally after prepare_script_run()
    // received the static-eligibility flag.  Empty timeframes only request
    // auto-detection; only the bare run(bars, n) overload skips the surface,
    // and the kernel reports that overload as NativeBeginArgs::simple_run.
    // (R4-D L10o: gating on static eligibility left every request.security
    // series na whenever a caller auto-detected the timeframe.)
    if (!retained_.simple_run) host.scheduler_configure_security_evaluators();
    uses_aux_security_feed_ = host.scheduler_uses_aux_security_feed();
    host.scheduler_prepare_security_sequence(retained_.bars);
}

int PineScheduler::source_bar_index_for(const NativeDecisionContext& context) const noexcept {
    // Matching at a new script-bar open precedes the terminal source callback;
    // all fills after that callback (including COOF/POOC notifications) belong
    // to the already-published source index.  This is the same cadence the
    // legacy aggregation loop used for Trade.entry_bar_index/exit_bar_index.
    const bool published = last_published_script_open_ms_
        == context.script_bar_open_ms;
    const bool coof_published = coof_callback_script_open_ == context.script_bar_open_ms;
    if (published || coof_published)
        return std::max(0, source_bar_count_ - 1);
    return source_bar_count_;
}

std::optional<double> PineScheduler::next_input_waypoint(
        const NativeDecisionContext& context, double current_price,
        NativePathOrder order) const noexcept {
    const auto found = std::find_if(retained_.bars.begin(), retained_.bars.end(),
        [&](const Bar& bar) { return bar.timestamp == context.sub_bar_open_ms; });
    if (found == retained_.bars.end()) return std::nullopt;
    bool high_first = std::abs(found->high - found->open)
        < std::abs(found->open - found->low);
    if (order == NativePathOrder::HighFirst) high_first = true;
    else if (order == NativePathOrder::LowFirst) high_first = false;
    const NativePathPhase phase[] = {
        NativePathPhase::Open,
        high_first ? NativePathPhase::High : NativePathPhase::Low,
        high_first ? NativePathPhase::Low : NativePathPhase::High,
        NativePathPhase::Close,
    };
    const double price[] = {
        found->open,
        high_first ? found->high : found->low,
        high_first ? found->low : found->high,
        found->close,
    };
    for (int index = 0; index < 4; ++index) {
        if (phase[index] != context.coordinate.path_phase) continue;
        if (index > 0 && current_price != price[index]) return price[index];
        if (index < 3) return price[index + 1];
        return std::nullopt;
    }
    return std::nullopt;
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

void PineScheduler::snapshot_coof_state(PineStrategyHost& host) {
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
    host.snapshot_script_state();
    language_.coof_checkpoint_contains_current_bar_ = false;
}

void PineScheduler::restore_coof_state(PineStrategyHost& host) {
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

void PineScheduler::commit_coof_state(PineStrategyHost& host) {
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
    host.commit_script_state();
    language_.coof_checkpoint_contains_current_bar_ = true;
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
    struct InputBarIndexScope {
        PineStrategyHost& host;
        int previous;
        explicit InputBarIndexScope(PineStrategyHost& value, int index)
            : host(value), previous(value.bar_index_) { host.bar_index_ = index; }
        ~InputBarIndexScope() { host.bar_index_ = previous; }
    } input_bar_index(host, context.input_index);
    if (uses_aux_security_feed_) {
        prior_input_script_open_ms_ = context.script_interval.open_ms;
        return;
    }
    std::int64_t next_input_ms = 0;
    if (context.input_index >= 0
        && context.input_index + 1 < static_cast<int>(retained_.bars.size())) {
        next_input_ms = retained_.bars[static_cast<std::size_t>(context.input_index + 1)].timestamp;
    }
    if (retained_.bar_magnifier && deferred_boundary_input_.active
        && deferred_boundary_input_.prior_script_open_ms != context.script_interval.open_ms) {
        // A sparse lower feed reveals the completed caller only when the next
        // child arrives. Feed the retained final child first, then let the
        // new caller's child proceed in timestamp order.
        const std::int64_t completed_script_open =
            deferred_boundary_input_.prior_script_open_ms;
        (void)host.scheduler_feed_security_input(
            deferred_boundary_input_.bar, deferred_boundary_input_.next_input_ms,
            true, false);
        // Keep the new caller's first child out of request.security until the
        // completed caller's chart callback has observed the published value.
        deferred_boundary_input_.bar = bar;
        deferred_boundary_input_.next_input_ms = next_input_ms;
        deferred_boundary_input_.prior_script_open_ms = completed_script_open;
        deferred_boundary_input_.calling_bar_complete = false;
        deferred_boundary_input_.all_security_states = true;
        deferred_boundary_input_.active = true;
        prior_input_script_open_ms_ = context.script_interval.open_ms;
        return;
    }
    // The generic calendar may wait for a later tradable opening before it
    // seals a script interval.  The source chart aggregator can have already
    // completed that interval on the prior raw bar.  Keep the new raw input
    // out of request.security until the pending script callback observes the
    // same legacy point; then feed it immediately after that callback.
    if (!retained_.bar_magnifier && awaiting_legacy_script_open_ms_
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
    // The final sparse magnifier child is a partial requested bucket.  The
    // legacy lower-TF pump does not promote that tail to a completed
    // request.security value merely because the input array ended.
    if (retained_.bar_magnifier && next_input_ms == 0)
        calling_bar_complete = false;
    bool security_boundary_ahead = false;
    if (retained_.bar_magnifier && next_input_ms != 0) {
        const bool caller_boundary = tf_change(
            bar.timestamp, next_input_ms, retained_.script_tf,
            host.syminfo_.timezone, host.syminfo_.session);
        if (caller_boundary) {
            for (const auto& state : host.security_eval_states_) {
                if (state.publish_gate_tf_seconds > 0) {
                    security_boundary_ahead = true;
                    break;
                }
            }
        }
    }
    if (security_boundary_ahead) {
        deferred_boundary_input_.bar = bar;
        deferred_boundary_input_.next_input_ms = next_input_ms;
        deferred_boundary_input_.prior_script_open_ms = context.script_interval.open_ms;
        deferred_boundary_input_.calling_bar_complete = true;
        deferred_boundary_input_.all_security_states = false;
        deferred_boundary_input_.active = true;
        prior_input_script_open_ms_ = context.script_interval.open_ms;
        return;
    }
    const bool deferred_gate = host.scheduler_feed_security_input(
        bar, next_input_ms, calling_bar_complete,
        boundary && !retained_.bar_magnifier);
    if (deferred_gate) {
        deferred_boundary_input_.bar = bar;
        deferred_boundary_input_.next_input_ms = next_input_ms;
        deferred_boundary_input_.prior_script_open_ms = prior_input_script_open_ms_;
        deferred_boundary_input_.calling_bar_complete = false;
        deferred_boundary_input_.all_security_states = false;
        deferred_boundary_input_.active = true;
    }
    if (calling_bar_complete && !retained_.bar_magnifier) {
        awaiting_legacy_script_open_ms_ = boundary
            ? prior_input_script_open_ms_ : context.script_interval.open_ms;
    }
    prior_input_script_open_ms_ = context.script_interval.open_ms;
}

void PineScheduler::tick(const Bar& bar, const NativeTickContext& context,
                         PineStrategyHost& host) {
    if (!retained_.is_stream) return;
    const auto& interval = context.decision.input_interval;
    if (last_stream_input_open_ms_ == interval.open_ms) return;
    if (prior_input_script_open_ms_ != std::numeric_limits<std::int64_t>::min()
        && prior_input_script_open_ms_ != context.decision.script_interval.open_ms) {
        // A realtime tick can be the first child of the next caller after a
        // sparse warmup.  The legacy stream replays the retained final child
        // at this boundary (without advancing the requested-context slot).
        host.scheduler_publish_security_boundary();
    }
    last_stream_input_open_ms_ = interval.open_ms;
    const auto& script = context.decision.script_interval;
    const bool complete = interval.next_period_open_ms >= script.next_period_open_ms;
    (void)host.scheduler_feed_security_input(
        bar, interval.next_input_open_ms, complete, false);
    prior_input_script_open_ms_ = script.open_ms;
}

void PineScheduler::bar_open(const Bar& value, const NativeDecisionContext& context,
                             PineStrategyHost& host) {
    if (context.script_bar_open_ms != current_script_open_ms_) {
        current_script_open_ms_ = context.script_bar_open_ms;
        saw_open_fill_ = false;
        open_point_fills_ = 0;
        coof_callback_script_open_ = std::numeric_limits<std::int64_t>::min();
        if (host.scheduler_coof_enabled()) snapshot_coof_script_state(host);
    }
    if (!retained_.is_stream) {
        current_script_bar_ = value;
        current_script_bar_.timestamp = context.script_bar_open_ms;
        current_script_bar_valid_ = true;
    }
}

void PineScheduler::bar(const Bar& value, const NativeDecisionContext& context, PineStrategyHost& host) {
    // P7c: matching advances over every sub-bar; the language callback occurs
    // only at the terminal sub-bar with the script bar timestamp restored.
    language_.is_first_tick_ = context.is_terminal_sub_bar;
    language_.is_last_tick_ = context.is_terminal_sub_bar;
    language_.history_slot_is_new_ = context.is_terminal_sub_bar;
    if (!context.is_terminal_sub_bar) return;
    // A FeedTolerant native stream can emit a quiet carried callback for
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
    if (retained_.is_stream && !retained_.bars.empty()
        && source_bar_count_ >= static_cast<int>(retained_.bars.size())) {
        const int input_seconds = tf_to_seconds(retained_.input_tf);
        const std::int64_t expected_open = retained_.bars.back().timestamp
            + static_cast<std::int64_t>(std::max(input_seconds, 0)) * 1000;
        if (input_seconds > 0 && script_bar.timestamp < expected_open) {
            // ab9714be pine_stream.cpp:112-125 labels the first realtime
            // source bar at last_warmup + input_duration.  A tolerant native
            // calendar can report its aligned interval label instead; retain
            // the source-visible raw stream label without changing matching.
            script_bar.timestamp = expected_open;
        }
    }
    current_script_bar_ = script_bar;
    current_script_bar_valid_ = true;
    const bool completes_awaiting_legacy_script = awaiting_legacy_script_open_ms_
        == context.script_bar_open_ms;
    if (deferred_boundary_input_.active
        && !deferred_boundary_input_.all_security_states
        && deferred_boundary_input_.prior_script_open_ms == context.script_bar_open_ms) {
        // The legacy magnifier feeds the boundary-triggering lower bar after
        // the completed script callback; it does not replay the caller at the
        // callback boundary.  The ordinary batch loop does replay it.
        if (!retained_.bar_magnifier)
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
        && !state.spec->timeframe_undetected) {
        if (tf_ratio(state.spec->input_tf, state.spec->script_tf) == 1
            && source_bar_count_ + 1 < static_cast<int>(retained_.bars.size())) {
            next_script_open_ms = retained_.bars[
                static_cast<std::size_t>(source_bar_count_ + 1)].timestamp;
        }
        host.scheduler_update_session_state(script_bar, next_script_open_ms);
    }
    const bool suppress_probe_tail = host.probe_suppress_tail_logic()
        && expected_source_bars_ > 0
        && source_bar_count_ + 1 >= expected_source_bars_;
    if (suppress_probe_tail) host.scheduler_publish_suppressed_tail(script_bar);
    else {
        host.scheduler_publish_source_bar(script_bar, true, !had_coof_recalc);
        last_published_script_open_ms_ = context.script_bar_open_ms;
    }
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
        if (!suppress_probe_tail) host.scheduler_record_range_end(current_script_bar_);
        if (!retained_.is_stream) host.scheduler_finish_security_sequence();
    }
    host.scheduler_record_broker_hash();
}

void PineScheduler::applied(const native_order::ExecutionAppliedEvent& event,
                            const NativeDecisionContext& context, PineStrategyHost& host) {
    if (event.ordinal <= applied_cursor_) return;
    applied_cursor_ = event.ordinal;
    if (!host.scheduler_coof_enabled()) return;
    if (host.config_.process_orders_on_close
        && (context.coordinate.provenance == NativePriceProvenance::Calculation
            || context.coordinate.provenance == NativePriceProvenance::AfterCalculationClose
            || context.coordinate.path_phase == NativePathPhase::None)) {
        // ab9714be pine_scheduler.cpp terminal POOC dispatch: a fill at the
        // already-consumed close is final for that script bar and does not
        // schedule a calc_on_order_fills source callback.
        return;
    }
    if (host.adapter_.suppress_grouped_stop_recalc(event, context)) return;
    // ab9714be pine_scheduler.cpp:531-537: the historical O point admits the
    // carried order's open fill and one refill; every later fill advances
    // along the path.  The native matcher may book such a fill at the start
    // (t == 0) of the O->W1 segment instead of at the Open phase; both are
    // the same O point, so its recalculation carries Open provenance.  Once
    // the O point is spent, a further fill at that price lies on the O->W1
    // leg (pine_scheduler.cpp:571-606) and its recalculation says so.
    const bool bar_known = current_script_bar_valid_
        && current_script_bar_.timestamp == context.script_bar_open_ms;
    const NativePathPhase first_extreme = bar_known
        && host.adapter_.source_path_uses_high_first(current_script_bar_)
        ? NativePathPhase::High : NativePathPhase::Low;
    const bool open_point = context.coordinate.path_phase == NativePathPhase::Open
        || (bar_known && context.coordinate.path_phase == first_extreme
            && event.cursor.t == 0.0);
    const bool at_open = open_point && open_point_fills_ < 2;
    open_point_fills_ = at_open ? open_point_fills_ + 1 : 2;
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
    NativeDecisionContext coof_context = context;
    if (at_open) coof_context.coordinate.path_phase = NativePathPhase::Open;
    else if (open_point && bar_known) coof_context.coordinate.path_phase = first_extreme;
    host.adapter_.begin_coof_recalc(
        event, coof_context, first_open, host.broker_fill_event_seq_);
    // ab9714be pine_scheduler.cpp:357-366: under COOF every fill recalculation
    // invokes update_per_trade_extremes() against the full script bar, except
    // the carried positive-slip POOC opening-money chain at O
    // (pine_scheduler.cpp:494-511), whose exits see only the open.
    Bar extremes_bar = callback_bar;
    if (host.config_.process_orders_on_close && host.config_.slippage > 0 && open_point) {
        extremes_bar.high = extremes_bar.low = extremes_bar.close = extremes_bar.open;
    }
    sample_open_trade_extremes(
        host.pyramid_entries_, host.position_side_, host.bar_index_, extremes_bar);
    try {
        host.scheduler_publish_source_bar(
            callback_bar, true, callback_advances_source_bar);
        // Low/high/close recalculations stage source requests until the
        // callback has finished so their statement order is complete.  Drain
        // that source queue while the fill coordinate is still current; the
        // accepted MARKET newborns below then execute at this same broker
        // point, matching calc_on_order_fills chronology.
        host.adapter_.flush_coof_tail(/*openings_only=*/true);
        host.adapter_.flush_coof_tail();
    } catch (...) {
        host.adapter_.end_coof_recalc();
        throw;
    }
    host.adapter_.end_coof_recalc();
    restore_coof_script_state(host);
    coof_callback_script_open_ = context.script_bar_open_ms;
    last_published_script_open_ms_ = context.script_bar_open_ms;
    if (callback_advances_source_bar) ++source_bar_count_;
    if (!first_open) {
        auto newborns = host.adapter_.take_first_open_newborns();
        for (const auto& handle : newborns) {
            (void)host.execute_current({handle, NativeCurrentPriceRule::NearestTick});
        }
        return;
    }
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
