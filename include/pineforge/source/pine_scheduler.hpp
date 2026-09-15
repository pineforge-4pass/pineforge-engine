#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_language_state.hpp>

#include <cstdint>
#include <limits>
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
    void bar_open(const Bar&, const NativeDecisionContext&, PineStrategyHost&);
    void bar(const Bar&, const NativeDecisionContext&, PineStrategyHost&);
    void applied(const native_order::ExecutionAppliedEvent&, const NativeDecisionContext&,
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
    bool terminal_source_bar() const noexcept {
        return expected_source_bars_ > 0 && source_bar_count_ >= expected_source_bars_;
    }
    const Bar* current_script_bar() const noexcept {
        return current_script_bar_valid_ ? &current_script_bar_ : nullptr;
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

    // @source-state begin
    PineLanguageState language_;
    RetainedBegin retained_;
    std::int64_t current_script_open_ms_ = 0;
    Bar current_script_bar_{};
    bool current_script_bar_valid_ = false;
    bool saw_open_fill_ = false;
    int source_bar_count_ = 0;
    int expected_source_bars_ = 0;
    std::uint64_t applied_cursor_ = 0;
    std::int64_t coof_callback_script_open_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t prior_input_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t awaiting_legacy_script_open_ms_ = std::numeric_limits<std::int64_t>::min();
    std::vector<unsigned char> input_script_completes_;
    std::vector<unsigned char> input_script_boundary_completes_;
    bool uses_aux_security_feed_ = false;
    DeferredBoundaryInput deferred_boundary_input_{};
    // @source-state end
};

} // namespace pineforge::source
