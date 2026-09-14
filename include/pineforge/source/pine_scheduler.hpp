#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_language_state.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace pineforge::source {

class PineNativeHost;

// Source cadence over generic native callbacks. The driver remains the sole
// owner of matching; this class owns only language publication, retained begin
// data and the Pine calc-on-order-fills callback cadence.
class PineScheduler {
public:
    void capture_begin(const NativeBeginArgs&);
    void run_begin(PineNativeHost&);
    void bar_open(const Bar&, const NativeDecisionContext&, PineNativeHost&);
    void bar(const Bar&, const NativeDecisionContext&, PineNativeHost&);
    void applied(const native_order::ExecutionAppliedEvent&, const NativeDecisionContext&,
                 PineNativeHost&);

    bool is_first_tick() const noexcept { return language_.is_first_tick_; }
    bool history_advances_new_bar() const noexcept {
        return language_.is_first_tick_ && language_.history_slot_is_new_;
    }
    bool security_series_slot_is_new(int) const noexcept {
        return language_.history_slot_is_new_;
    }
    double previous_chart_close() const noexcept { return language_.prev_chart_close_; }
    int source_bar_count() const noexcept { return source_bar_count_; }

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

    struct CoofInterval {
        std::uint64_t applied_ordinal = 0;
        std::int64_t script_open_ms = 0;
        bool first_open = false;
    };

    void publish_series(const Bar&);
    void reset_language();

    // @source-state begin
    PineLanguageState language_;
    std::deque<CoofInterval> coof_;
    RetainedBegin retained_;
    std::int64_t current_script_open_ms_ = 0;
    bool saw_open_fill_ = false;
    int source_bar_count_ = 0;
    std::uint64_t applied_cursor_ = 0;
    // @source-state end
};

} // namespace pineforge::source
