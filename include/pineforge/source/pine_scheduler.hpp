#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_language_state.hpp>

#include <algorithm>
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
    bool terminal_source_bar() const noexcept {
        return expected_source_bars_ > 0 && source_bar_count_ >= expected_source_bars_;
    }
    int source_bar_index_for(const NativeDecisionContext& context) const noexcept;
    std::optional<double> next_input_waypoint(
        const NativeDecisionContext&, double current_price,
        NativePathOrder) const noexcept;
    const Bar* current_script_bar() const noexcept {
        return current_script_bar_valid_ ? &current_script_bar_ : nullptr;
    }
    std::optional<Bar> broker_bar(const NativeDecisionContext& context) const {
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
    DeferredBoundaryInput deferred_boundary_input_{};
    // @source-state end
};

} // namespace pineforge::source
