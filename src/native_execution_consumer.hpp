#pragma once

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <limits>
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
    int64_t decision_floor() const noexcept {
        return has_floor_ ? decision_floor_ms_ : std::numeric_limits<int64_t>::min();
    }
    uint64_t high_water() const noexcept { return consumed_high_water_; }
    void reject_inherited_on_bar(BacktestEngine& engine);

private:
    struct ScriptBucket {
        int64_t key = 0;
        native_calendar::NativeInterval interval{};
        Bar agg{};
        bool has_data = false;
        int64_t first_open_ms = 0;
        int64_t first_source_time_ms = 0;
        int64_t latest_close_ms = 0;
        int first_index = 0;
        int last_index = 0;
        bool sealed = false;
        // False once a tick or quiet-carried slot has contributed. Seal then
        // calculates without replaying modeled OHLC matching/excursion.
        bool modeled_ohlc = true;
    };

    enum class InputContribution : std::uint8_t {
        ConfirmedBar = 0,
        ObservedTickSlot = 1,
        QuietCarried = 2,
    };

    enum class InputMode : std::uint8_t {
        Unselected = 0,
        ConfirmedBars = 1,
        ObservedTicks = 2,
    };

    struct AppendDigest {
        uint64_t h = 1469598103934665603ULL;
        uint64_t count = 0;
        void reset() noexcept {
            h = 1469598103934665603ULL;
            count = 0;
        }
    };

    bool failed() const noexcept;
    void latch_failure(NativeFailure failure) noexcept;
    void fail(BacktestEngine& engine, NativeFailure failure) noexcept;
    void render(BacktestEngine& engine, const char* text) const;
    const NativeRunSpec* spec_ptr() const;
    bool commands_allowed() const;
    bool timeframe_args_ok(const std::string& input_tf, const std::string& script_tf) const;
    bool apply_spec(BacktestEngine& engine, const NativeRunSpec& spec);
    bool projection_ok(const BacktestEngine& engine) const;
    bool begin_ready(BacktestEngine& engine, NativeRunPhase phase, int64_t initial_floor_ms);
    bool refuse_mixed_input_mode(BacktestEngine& engine, InputMode requested);
    void select_input_mode(InputMode requested);
    bool admit_public_begin(BacktestEngine& engine, const char* not_ready_text);
    bool admit_public_stream_input(BacktestEngine& engine, NativeFailureOperation operation);
    bool preflight_bars(BacktestEngine& engine, const Bar* bars, int n, bool stream);
    void pump_batch(BacktestEngine& engine, const Bar* bars, int n);
    bool consume_confirmed_input(BacktestEngine& engine, const Bar& bar, int index, bool last);
    bool contribute_input(BacktestEngine& engine, const Bar& bar,
                          const native_calendar::NativeInterval& interval,
                          int index, InputContribution kind);
    void seal_script(BacktestEngine& engine, NativeCompletionKind kind);
    void deliver_confirmed_script(BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base);
    void deliver_aggregate_calculation(BacktestEngine& engine, const Bar& bar,
                                       const NativeCoordinate& base);
    int64_t calculation_time(const NativeCoordinate& base) const noexcept;
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
    bool preflight_ticks(BacktestEngine& engine, const TradeTick* ticks, int n);
    bool deliver_tick(BacktestEngine& engine, const TradeTick& tick);
    bool check_abort_or_projection(BacktestEngine& engine, NativeFailureOperation operation,
                                   uint64_t ordinal = 0);
    void present_refusal(BacktestEngine& engine, const char* text);
    bool finalize_elapsed_slots(BacktestEngine& engine, int64_t exclusive_end_ms);
    bool emit_quiet_carried_open(BacktestEngine& engine,
                                 const native_calendar::NativeInterval& interval);
    bool finalize_observed_tick_slot(BacktestEngine& engine,
                                     const native_calendar::NativeInterval& interval,
                                     NativeCompletionKind kind);
    void sync_history_digest() const noexcept;
    void fold_driver_digest(const NativeDriverPoint& point) const noexcept;
    void fold_account_digest(const NativeAccountObservation& row) const noexcept;

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
    InputMode input_mode_ = InputMode::Unselected;
    int next_interval_index_ = 0;
    std::optional<int64_t> current_input_open_;
    std::optional<int64_t> observed_input_cursor_;
    std::optional<int64_t> next_tradable_synthesis_cursor_;
    std::optional<native_calendar::NativeInterval> last_accepted_input_;
    std::optional<int64_t> last_observed_slot_open_;
    std::optional<native_calendar::NativeInterval> last_finalized_input_;
    uint64_t last_tick_sequence_ = 0;
    bool has_tick_sequence_ = false;
    ScriptBucket script_{};
    Bar forming_{};
    bool has_forming_ = false;
    double last_price_ = 0.0;
    bool has_last_price_ = false;
    int64_t last_print_time_ms_ = 0;
    std::vector<NativeDriverPoint> driver_log_;
    std::vector<NativeAccountObservation> account_log_;
    NativeDecisionContext callback_context_{};
    std::optional<native_calendar::TimezoneIdentityDescriptor> tz_identity_{};
    mutable AppendDigest history_digest_{};
    mutable AppendDigest driver_digest_{};
    mutable AppendDigest account_digest_{};
};

inline NativeExecutionConsumer& as_native_consumer(IExecutionConsumer& consumer) {
    return static_cast<NativeExecutionConsumer&>(consumer);
}

}  // inline namespace engine_script_run_v12
}  // namespace pineforge
