#pragma once

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v12 {

class NativeExecutionConsumer final : public IExecutionConsumer {
public:
    bool is_native() const noexcept override { return true; }
    void refuse_source_mutation(const char* operation) override;
    uint64_t continuation_hash() const noexcept override;

    void run_simple(BacktestEngine& engine, const Bar* bars, int n) override;
    void run_tf(BacktestEngine& engine,
                const Bar* input_bars, int n_input,
                const std::string& input_tf,
                const std::string& script_tf,
                bool bar_magnifier,
                int magnifier_samples,
                MagnifierDistribution magnifier_dist) override;
    void run_rich(BacktestEngine& engine,
                  const Bar* input_bars, int n_input,
                  const std::string& input_tf,
                  const std::string& script_tf,
                  const std::unordered_map<std::string, std::string>& inputs,
                  const SymInfo& syminfo,
                  const StrategyOverrides* overrides,
                  bool bar_magnifier,
                  int magnifier_samples,
                  MagnifierDistribution magnifier_dist) override;

    bool stream_begin(BacktestEngine& engine,
                      const Bar* warmup_bars, int n_warmup,
                      const std::string& input_tf,
                      const std::string& script_tf) override;
    bool stream_push_bar(BacktestEngine& engine, const Bar& bar) override;
    bool stream_push_tick(BacktestEngine& engine, const TradeTick& tick) override;
    bool stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) override;
    bool stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) override;
    bool stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) override;

    NativeSetupResult configure(BacktestEngine& engine, const NativeRunSpec& spec);
    NativeStateView view() const;
    native_order::SubmitResult submit(BacktestEngine& engine, const native_order::Request& request);
    native_order::ReplaceResult replace(BacktestEngine& engine,
                                        const native_order::RequestHandle& target,
                                        const native_order::Request& request);
    native_order::CancelResult cancel(BacktestEngine& engine,
                                      const native_order::RequestHandle& target);
    NativePhysicalPosition position(const BacktestEngine& engine) const;
    double marked(const BacktestEngine& engine, double price) const;
    std::vector<NativeMarketEvent> events_after(uint64_t after_ordinal) const;
    int64_t decision_floor() const noexcept { return decision_floor_ms_; }
    uint64_t high_water() const noexcept { return consumed_high_water_; }
    void reject_inherited_on_bar(BacktestEngine& engine);

private:
    struct ScriptBucket {
        int64_t key = 0;
        native_calendar::NativeInterval interval{};
        Bar agg{};
        bool has_data = false;
        int64_t first_open_ms = 0;
        int64_t latest_close_ms = 0;
        int first_index = 0;
        int last_index = 0;
        bool sealed = false;
    };

    bool failed() const noexcept;
    void fail(BacktestEngine& engine, NativeFailure failure) noexcept;
    void render(BacktestEngine& engine, const char* text) const;
    const NativeRunSpec* spec_ptr() const;
    bool commands_allowed() const;
    bool timeframe_args_ok(const std::string& input_tf, const std::string& script_tf) const;
    bool apply_spec(BacktestEngine& engine, const NativeRunSpec& spec);
    bool projection_ok(const BacktestEngine& engine) const;
    bool begin_ready(BacktestEngine& engine, NativeRunPhase phase);
    bool preflight_bars(BacktestEngine& engine, const Bar* bars, int n, bool stream);
    void pump_batch(BacktestEngine& engine, const Bar* bars, int n);
    bool consume_confirmed_input(BacktestEngine& engine, const Bar& bar, int index, bool last);
    void seal_script(BacktestEngine& engine, NativeCompletionKind kind);
    void deliver_confirmed_script(BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base);
    void match_point(BacktestEngine& engine, const NativeDriverPoint& point);
    void apply_excursion(BacktestEngine& engine, double price);
    void invoke_callback(BacktestEngine& engine, const Bar& bar, const NativeCoordinate& coordinate);
    uint64_t take_ordinal(BacktestEngine& engine);
    void raise_floor(int64_t t);
    double resolve_price(const native_order::Request& request, double raw) const;
    bool admit_opening(const BacktestEngine& engine,
                       const execution::SettlementInspection& inspect,
                       native_order::MatchRejectReason* reason) const;
    void terminal_no_effect(BacktestEngine& engine, std::size_t live_index);
    void terminal_reject(BacktestEngine& engine, std::size_t live_index,
                         native_order::MatchRejectReason reason);
    void record_driver(const NativeDriverPoint& point);
    NativeCoordinate coordinate_from(const native_calendar::NativeInterval& interval,
                                     int index, int64_t effective,
                                     NativePriceProvenance provenance,
                                     NativePathPhase phase) const;

    NativeLifecycle state_{NativeUnconfigured{}};
    uint64_t consumed_high_water_ = 0;
    std::string bound_session_key_;
    native_order::WorkingRequestCore requests_{{"unbound", 1}};
    uint64_t next_timeline_ordinal_ = 1;
    int64_t decision_floor_ms_ = 0;
    bool has_floor_ = false;
    native_calendar::SessionCalendar calendar_{};
    native_calendar::Timeframe input_tf_{};
    native_calendar::Timeframe script_tf_{};
    native_calendar::TimeframeCompatibility pairing_{};
    NativeRunSpec applied_{};
    bool in_callback_ = false;
    bool processing_input_ = false;
    bool stream_ticks_ = false;
    int next_interval_index_ = 0;
    std::optional<int64_t> current_input_open_;
    std::optional<int64_t> observed_input_cursor_;
    std::optional<int64_t> next_tradable_synthesis_cursor_;
    std::vector<int64_t> observed_slots_;
    ScriptBucket script_{};
    Bar forming_{};
    bool has_forming_ = false;
    double last_price_ = 0.0;
    bool has_last_price_ = false;
    int64_t last_print_time_ms_ = 0;
    std::vector<NativeDriverPoint> driver_log_;
    std::vector<NativeAccountObservation> account_log_;
    NativeDecisionContext callback_context_{};
};

inline NativeExecutionConsumer& as_native_consumer(IExecutionConsumer& consumer) {
    return static_cast<NativeExecutionConsumer&>(consumer);
}

}  // inline namespace engine_script_run_v12
}  // namespace pineforge
