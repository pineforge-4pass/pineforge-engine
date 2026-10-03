#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_language_state.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace pineforge::source {

class PineStrategyHost;

// Source cadence over generic native callbacks. The driver remains the sole
// owner of matching; this class owns only language publication, retained begin
// data and the Pine calc-on-order-fills callback cadence.
class PineScheduler {
public:
    void capture_begin(const NativeBeginArgs&);
    void run_begin(PineStrategyHost&);
    void input(const Bar&, const NativeInputContext&, PineStrategyHost&);
    void tick(const Bar&, const NativeTickContext&, PineStrategyHost&);
    void bar_open(const Bar&, const NativeDecisionContext&, PineStrategyHost&);
    void bar(const Bar&, const NativeDecisionContext&, PineStrategyHost&);
    // Applied-notification bookkeeping only. The calc_on_order_fills
    // recalculation itself is the kernel's cadence now
    // (NativeCalculationTrigger::BarCloseAndFills): the consumer drives it
    // from the same drain iteration, right after this callback returns, and
    // delivers it as recalculate() below.
    void applied(const native_order::ExecutionAppliedEvent&);
    // Pine's own admission of a fill recalculation, re-checked at the
    // kernel's OrderFill cursor. Pure: it reads the freshly advanced applied
    // cursor, the source config and adapter facts, and mutates nothing.
    bool coof_recalculation_due(const native_order::ExecutionAppliedEvent&,
                                const NativeDecisionContext&, PineStrategyHost&) const;
    void recalculate(const native_order::ExecutionAppliedEvent&, const NativeDecisionContext&,
                     PineStrategyHost&);

    PineLanguageState& language() noexcept { return language_; }
    bool is_first_tick() const noexcept { return language_.is_first_tick_; }
    bool is_last_tick() const noexcept { return language_.is_last_tick_; }
    bool bar_magnifier_enabled() const noexcept { return retained_.bar_magnifier; }
    bool history_advances_new_bar() const noexcept {
        return language_.is_first_tick_ && language_.history_slot_is_new_;
    }
    bool security_series_slot_is_new(int) const noexcept {
        return language_.history_slot_is_new_;
    }
    double previous_chart_close() const noexcept { return language_.prev_chart_close_; }
    int bar_index_offset() const noexcept { return language_.bar_index_offset_; }
    void set_bar_index_offset(int value) noexcept { language_.bar_index_offset_ = value; }
    void set_source_series_active(bool value) noexcept { language_._src_series_active_ = value; }
    double script_position_view(int bar_index, PositionSide side, double quantity) const noexcept;
    void freeze_script_position_view(int bar_index, PositionSide side, double quantity,
                                     const std::vector<PyramidEntry>& lots);
    void clear_script_position_view() noexcept;
    const Series<double>& source_series(const std::string&) const;
    void fixture_publish_source_series(const Bar&, bool new_history_slot);
    int source_bar_count() const noexcept { return source_bar_count_; }
    std::optional<Bar> next_source_bar(int interval_index) const {
        if (retained_.is_stream || retained_.bar_magnifier
            || (!retained_.input_tf.empty() && !retained_.script_tf.empty()
                && retained_.input_tf != retained_.script_tf)
            || interval_index < 0
            || interval_index + 1 >= static_cast<int>(retained_.bars.size())) {
            return std::nullopt;
        }
        return retained_.bars[static_cast<std::size_t>(interval_index + 1)];
    }
    // The first input the run retains -- a batch's, or a stream's warmup --
    // after input `input_index` that opens at or after `at_or_after`, or
    // nullopt when the retained input ends first.
    std::optional<Bar> retained_input_from(int input_index, std::int64_t at_or_after) const {
        if (input_index < 0) return std::nullopt;
        for (std::size_t i = static_cast<std::size_t>(input_index) + 1; i < retained_.bars.size(); ++i) {
            if (retained_.bars[i].timestamp >= at_or_after) return retained_.bars[i];
        }
        return std::nullopt;
    }
    bool retains_stream() const noexcept { return retained_.is_stream; }
    bool input_is_observed_ticks() const noexcept { return input_is_observed_ticks_; }
    void retain_confirmed_input(const Bar& bar) noexcept {
        if (!retained_.is_stream) return;
        for (std::size_t index = 0; index < confirmed_input_count_; ++index) {
            auto& existing = confirmed_input_bars_[index];
            if (existing.timestamp == bar.timestamp) {
                existing = bar;
                return;
            }
        }
        confirmed_input_bars_[confirmed_input_next_] = bar;
        confirmed_input_next_ = (confirmed_input_next_ + 1) % confirmed_input_bars_.size();
        confirmed_input_count_ = std::min(confirmed_input_count_ + 1, confirmed_input_bars_.size());
    }
    std::size_t confirmed_input_count() const noexcept { return confirmed_input_count_; }
    std::optional<Bar> confirmed_input_bar_at(std::int64_t timestamp) const noexcept {
        for (std::size_t index = 0; index < confirmed_input_count_; ++index) {
            if (confirmed_input_bars_[index].timestamp == timestamp)
                return confirmed_input_bars_[index];
        }
        return std::nullopt;
    }
    static bool confirmed_script_interval_complete(const NativeInputContext& context) noexcept {
        return context.script_interval.last_traded_close_ms > context.script_interval.open_ms
            && context.input_interval.last_traded_close_ms
                >= context.script_interval.last_traded_close_ms;
    }
    // The input the run retains -- a batch's, or a stream's warmup -- in
    // time order, and whether each of its bars is a chart bar (no
    // aggregation to a coarser script timeframe).
    const std::vector<Bar>& retained_input() const noexcept { return retained_.bars; }
    bool retained_input_is_chart() const noexcept {
        return retained_.input_tf.empty() || retained_.script_tf.empty()
            || retained_.input_tf == retained_.script_tf;
    }
    // The open of the script bar published last, before the run's first none.
    std::optional<std::int64_t> last_published_script_open() const noexcept {
        if (last_published_script_open_ms_ == std::numeric_limits<std::int64_t>::min())
            return std::nullopt;
        return last_published_script_open_ms_;
    }
    bool terminal_source_bar() const noexcept {
        return expected_source_bars_ > 0 && source_bar_count_ >= expected_source_bars_;
    }
    int source_bar_index_for(const NativeDecisionContext& context) const noexcept;
    // The next fill point, for a fill recalculation's market order, of the
    // lower-path bar being walked: a price that bar still reaches, or
    // (next_open) the next lower-path bar's opening -- NaN when there is
    // none -- which a plain market order reaches by itself, since the kernel
    // fills a newborn market request at the path's next discrete point. The
    // leg order is the kernel's answer for that bar
    // (PineExecutionAdapter::source_path_uses_high_first), not a copy of it.
    struct InputWaypoint {
        double price;
        bool next_open;
    };
    std::optional<InputWaypoint> next_input_waypoint(
        const PineStrategyHost&, const NativeDecisionContext&) const noexcept;
    const Bar* current_script_bar() const noexcept {
        return current_script_bar_valid_ ? &current_script_bar_ : nullptr;
    }
    // The bar of the run's lower intrabar path the context is walking (its
    // sub_bar_open_ms), or nullptr when the run declares no lower path or no
    // bar of it carries that stamp. A magnified run's sub-bars are
    // TradingView's intrabars built from the input (R5 lane MAG-INTRABAR,
    // source::tradingview_magnifier_bars), not input bars.
    const Bar* lower_path_bar_at(const PineStrategyHost& host,
                                 const NativeDecisionContext& context) const noexcept;
    // One intrabar of a magnified run's path is done. TradingView's magnified
    // broker admits the open fill and one refill at EVERY intrabar's open,
    // not only at the chart bar's first one (R5 lane MAG-INTRABAR,
    // tests/fixtures/magnifier_intrabars/mi-coof-refill*), so the open-point
    // count starts over with the next intrabar.
    void sub_bar_complete() noexcept;
    std::optional<Bar> broker_bar(const PineStrategyHost& host,
                                  const NativeDecisionContext& context) const {
        if (const Bar* sub = lower_path_bar_at(host, context)) return *sub;
        if (const auto observed = confirmed_input_bar_at(context.sub_bar_open_ms)) return observed;
        const auto& bars = retained_.bars;
        const std::size_t n = bars.size();
        if (n > 0) {
            if (broker_bar_cursor < n && bars[broker_bar_cursor].timestamp == context.sub_bar_open_ms) {
                return bars[broker_bar_cursor];
            }
            if (broker_bar_cursor >= n || bars[broker_bar_cursor].timestamp > context.sub_bar_open_ms) {
                broker_bar_cursor = 0;
            }
            while (broker_bar_cursor < n && bars[broker_bar_cursor].timestamp < context.sub_bar_open_ms) {
                ++broker_bar_cursor;
            }
            if (broker_bar_cursor < n && bars[broker_bar_cursor].timestamp == context.sub_bar_open_ms) {
                return bars[broker_bar_cursor];
            }
            const auto found = std::find_if(bars.begin(), bars.end(),
                [&](const Bar& bar) { return bar.timestamp == context.sub_bar_open_ms; });
            if (found != bars.end()) {
                broker_bar_cursor = static_cast<std::size_t>(std::distance(bars.begin(), found));
                return *found;
            }
        }
        if (retained_.is_stream && !input_is_observed_ticks_) return std::nullopt;
        return current_script_bar_valid_ ? std::optional<Bar>{current_script_bar_}
                                         : std::nullopt;
    }

    void hash_state(BrokerStateHashSink&) const;

private:
    struct RetainedBegin {
        std::vector<Bar> bars;
        std::string input_tf;
        std::string script_tf;
        bool bar_magnifier = false;
        int magnifier_samples = 4;
        MagnifierDistribution distribution = MagnifierDistribution::ENDPOINTS;
        bool volume_weighted = false;
        int volume_weighted_min_samples = 2;
        int volume_weighted_max_samples = 64;
        bool is_stream = false;
        int warmup_n = 0;
        bool simple_run = false;
    };

    void publish_series(const Bar&, PineStrategyHost&);
    void update_source_series(const Bar&);
    void reset_language();
    // Restarts the consumed-prefix digests below (the retained input changed).
    void reset_consumed_digests() noexcept;
    void snapshot_coof_script_state(PineStrategyHost&);
    void restore_coof_script_state(PineStrategyHost&);
    void commit_coof_script_state(PineStrategyHost&);

    struct DeferredBoundaryInput {
        Bar bar{};
        std::int64_t next_input_ms = 0;
        std::int64_t prior_script_open_ms = 0;
        bool calling_bar_complete = false;
        bool all_security_states = false;
        bool active = false;
    };

    // Derived lookup cursor over retained_.bars (reset-and-rescan in
    // broker_bar()); waived from the state hash by
    // scripts/check_broker_state_hash_coverage.py::SCHEDULER_CACHE_WAIVERS.
    mutable std::size_t broker_bar_cursor = 0;

    // @source-state begin
    PineLanguageState language_;
    RetainedBegin retained_;
    std::int64_t current_script_open_ms_ = 0;
    Bar current_script_bar_{};
    bool current_script_bar_valid_ = false;
    bool saw_open_fill_ = false;
    int open_point_fills_ = 0;
    int source_bar_count_ = 0;
    int expected_source_bars_ = 0;
    std::uint64_t applied_cursor_ = 0;
    std::int64_t coof_callback_script_open_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t last_published_script_open_ms_ =
        std::numeric_limits<std::int64_t>::min();
    std::int64_t prior_input_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t awaiting_legacy_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t last_stream_input_open_ms_ = std::numeric_limits<std::int64_t>::min();
    std::vector<unsigned char> input_script_completes_;
    std::vector<unsigned char> input_script_boundary_completes_;
    bool uses_aux_security_feed_ = false;
    bool input_is_observed_ticks_ = false;
    std::array<Bar, 8> confirmed_input_bars_{};
    std::size_t confirmed_input_next_ = 0;
    std::size_t confirmed_input_count_ = 0;
    DeferredBoundaryInput deferred_boundary_input_{};
    // R5 lane V19-E: running digests of the consumed input prefix -- the bars,
    // their script completions and their boundary completions -- each element
    // folded once, the first time the state hash reads past it (the v2
    // scheduler fold re-read the whole prefix at every read).
    mutable std::size_t consumed_bars_folded_ = 0;
    mutable std::uint64_t consumed_bars_digest_ = 1469598103934665603ULL;
    mutable std::size_t consumed_completes_folded_ = 0;
    mutable std::uint64_t consumed_completes_digest_ = 1469598103934665603ULL;
    mutable std::size_t consumed_boundaries_folded_ = 0;
    mutable std::uint64_t consumed_boundaries_digest_ = 1469598103934665603ULL;
    // @source-state end
};

} // namespace pineforge::source
