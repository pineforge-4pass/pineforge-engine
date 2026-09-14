#include <pineforge/source/pine_scheduler.hpp>

#include <pineforge/source/pine_native_host.hpp>

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
    source_bar_count_ = 0; applied_cursor_ = 0;
}

void PineScheduler::run_begin(PineNativeHost& host) {
    reset_language();
    const bool static_eligible = !retained_.is_stream && !retained_.bar_magnifier
        && retained_.input_tf.empty() && retained_.script_tf.empty();
    host.scheduler_prepare_script_run(retained_.bars, static_eligible);
    host.scheduler_configure_security_evaluators();
}

void PineScheduler::publish_series(const Bar& bar) {
    if (language_.history_slot_is_new_) language_.prev_chart_close_ = language_.last_chart_close_;
    language_.last_chart_close_ = bar.close;
    if (!language_._src_series_active_) return;
    language_._src_open_.push(bar.open); language_._src_high_.push(bar.high);
    language_._src_low_.push(bar.low); language_._src_close_.push(bar.close);
    language_._src_volume_.push(bar.volume); language_._src_hl2_.push((bar.high + bar.low) / 2.0);
    language_._src_hlc3_.push((bar.high + bar.low + bar.close) / 3.0);
    language_._src_ohlc4_.push((bar.open + bar.high + bar.low + bar.close) / 4.0);
    language_._src_hlcc4_.push((bar.high + bar.low + bar.close + bar.close) / 4.0);
}

void PineScheduler::bar_open(const Bar&, const NativeDecisionContext& context, PineNativeHost&) {
    if (context.script_bar_open_ms != current_script_open_ms_) {
        current_script_open_ms_ = context.script_bar_open_ms;
        saw_open_fill_ = false;
    }
}

void PineScheduler::bar(const Bar& value, const NativeDecisionContext& context, PineNativeHost& host) {
    // P7c: matching advances over every sub-bar; the language callback occurs
    // only at the terminal sub-bar with the script bar timestamp restored.
    language_.is_first_tick_ = context.is_terminal_sub_bar;
    language_.is_last_tick_ = context.is_terminal_sub_bar;
    language_.history_slot_is_new_ = context.is_terminal_sub_bar;
    if (!context.is_terminal_sub_bar) return;
    Bar script_bar = value;
    script_bar.timestamp = context.script_bar_open_ms;
    publish_series(script_bar);
    host.scheduler_publish_source_bar(script_bar, true);
    ++source_bar_count_;
}

void PineScheduler::applied(const native_order::ExecutionAppliedEvent& event,
                            const NativeDecisionContext& context, PineNativeHost& host) {
    if (event.ordinal <= applied_cursor_) return;
    applied_cursor_ = event.ordinal;
    if (!host.scheduler_coof_enabled()) return;
    const bool at_open = context.coordinate.path_phase == NativePathPhase::Open;
    const bool first_open = at_open && !saw_open_fill_;
    if (at_open) saw_open_fill_ = true;
    coof_.push_back({event.ordinal, context.script_bar_open_ms, first_open});
    if (!first_open) return;
    const Bar point{event.resolved_price, event.resolved_price, event.resolved_price,
                    event.resolved_price, 0.0, context.script_bar_open_ms};
    language_.is_first_tick_ = true; language_.is_last_tick_ = false;
    language_.history_slot_is_new_ = false;
    host.scheduler_publish_source_bar(point, true);
    ++source_bar_count_;
    auto newborns = host.adapter_.take_first_open_newborns();
    constexpr std::size_t kCoofLoopGuard = 1U << 20;
    if (newborns.size() > kCoofLoopGuard)
        throw std::overflow_error("Pine COOF first-open loop guard exhausted");
    for (const auto& handle : newborns)
        (void)host.execute_current({handle, NativeCurrentPriceRule::NearestTick});
}

} // namespace pineforge::source
