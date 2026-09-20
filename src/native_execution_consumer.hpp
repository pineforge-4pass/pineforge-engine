#pragma once

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v18 {

class NativeExecutionConsumer final : public IExecutionConsumer {
public:
    bool is_native() const noexcept override { return true; }
    void refuse_source_mutation(const char* operation) override;
    bool stage_account_currency_fx_series(const std::vector<std::int64_t>& timestamps,
                                          const std::vector<double>& rates) override;
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
                  const void* overrides,
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
    NativeFxCurveSetupResult configure_fx_curve(const NativeFxCurve& curve);
    NativeStateView view() const;
    native_order::SubmitResult submit(BacktestEngine& engine, const native_order::Request& request);
    native_order::ReplaceResult replace(BacktestEngine& engine,
                                        const native_order::RequestHandle& target,
                                        const native_order::Request& request,
                                        native_order::ReplaceOptions options = {});
    native_order::SubmitResult submit_market(BacktestEngine& engine,
                                             const native_order::Request& request);
    native_order::ReplaceResult replace_market(BacktestEngine& engine,
                                               const native_order::RequestHandle& target,
                                               const native_order::Request& request);
    native_order::CancelResult cancel(BacktestEngine& engine,
                                      const native_order::RequestHandle& target);
    std::vector<NativeWorkingRequest> working_requests() const;
    std::size_t cancel_all(BacktestEngine& engine);
    std::size_t cancel_where(BacktestEngine& engine, std::string_view comment);
    native_order::CohortHandle cohort_open(BacktestEngine& engine);
    void cohort_add(BacktestEngine& engine, native_order::CohortHandle cohort,
                    native_order::RequestHandle origin);
    void cohort_remove(BacktestEngine& engine, native_order::CohortHandle cohort,
                       native_order::RequestHandle origin);
    std::optional<NativeCurrentPointView> current_execution_point() const;
    std::optional<NativeTrailState> trail_state(
        const BacktestEngine& engine, const native_order::RequestHandle& target) const;
    NativeCurrentExecutionPreview inspect_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const;
    NativeCurrentExecutionResult execute_current(
        BacktestEngine& engine, const NativeCurrentExecution& command);
    NativePhysicalPosition position(const BacktestEngine& engine) const;
    double marked(const BacktestEngine& engine, double price) const;
    std::optional<double> host_liquidation_price(const BacktestEngine& engine) const;
    NativeRiskState risk_state() const;
    std::vector<NativeMarketEvent> events_after(uint64_t after_ordinal) const;
    uint64_t event_high_water() const noexcept;
    uint64_t terminal_receipt_high_water() const noexcept {
        return terminal_receipt_high_water_;
    }
    void reserve_driver_log(std::size_t expected_points);
    int64_t decision_floor() const noexcept {
        return has_floor_ ? decision_floor_ms_ : std::numeric_limits<int64_t>::min();
    }
    uint64_t high_water() const noexcept { return consumed_high_water_; }
    void reject_inherited_on_bar(BacktestEngine& engine);
    // Report truth at the host's own cadence
    // (NativeReportPolicy::KernelRecordedAtHostMarks): the host marks the
    // script bar it has just published and the kernel records the point.
    // Reporting only, and inert under every other policy.
    void mark_script_report_point(BacktestEngine& engine, int64_t script_bar_ts) const;
    std::optional<Bar> series_bar(std::size_t subscription) const;
    // L5 calculation timing readbacks. The partial bar is the lookahead-free
    // bar so far at the current cursor; the two counters are observation of
    // the recalculation cadence, never matching state.
    std::optional<Bar> partial_bar() const;
    uint64_t recalculation_count() const noexcept { return recalculations_; }
    uint64_t recalculations_skipped() const noexcept { return recalculations_skipped_; }

private:
    struct CurrentExecutionFrame {
        NativeCurrentPointView point;
        uint64_t acceptance_cutoff = 0;
    };
    // Stable append-only history index, never a borrowed element reference.
    struct AppliedNotification {
        std::size_t history_index = 0;
        uint64_t ordinal = 0;
        NativeCurrentPointView point;
        // History index of the MarginCallEvent this fill recorded, when the
        // filled request was the kernel's own liquidation. Absent for every
        // host request, which is the whole population of a run without a
        // margin model.
        std::optional<std::size_t> margin_call_index;
    };
    // The live kernel-issued liquidation of the current position, if any.
    // At most one rests at a time: a moved level or moved units withdraw the
    // previous one under CancelReason::Superseded before the new one is born.
    struct MarginLiquidation {
        native_order::RequestHandle handle{};
        double level = 0.0;
        double units = 0.0;
    };
    // L9 generic risk ledger. Durable decision state, but only for a run that
    // declares NativeRunSpec::risk: it is folded into the continuation digest
    // exactly there, so a spec without the block keeps its pre-risk identity.
    // `run_block` latches to the end of the run (drawdown, consecutive loss
    // days); `day_block` lasts only while `day_ordinal` is still
    // `day_block_day` (intraday loss, fills per day).
    struct RiskLedger {
        std::int64_t day_ordinal = 0;
        bool has_day = false;
        std::uint64_t fills_today = 0;
        std::uint32_t consecutive_loss_days = 0;
        double peak_equity = 0.0;
        bool has_peak = false;
        double day_open_equity = 0.0;
        double day_open_realized = 0.0;
        std::optional<native_order::RiskLimitKind> run_block;
        std::optional<native_order::RiskLimitKind> day_block;
        std::int64_t day_block_day = 0;
    };
    struct ResolvedCandidate {
        native_order::ExecutionPlan physical = execution::Flatten{};
        native_order::ExecutionScope scope = execution::Book{};
        execution::CloseScope financial_scope = execution::Book{};
        std::optional<execution::SelectedOpeningSet> selected;
        native_order::TargetObservation target;
        execution::Fill fill;
        execution::SettlementInspection inspect;
    };
    std::optional<NativeCurrentRefusal> validate_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const;
    NativeCoordinate current_execution_coordinate(uint64_t ordinal) const;
    double current_price(const BacktestEngine& engine, const native_order::LiveRequest& live,
                         NativeCurrentPriceRule rule) const;
    static execution::Action narrow_action(const native_order::ExecutionPlan& plan);
    ResolvedCandidate inspect_candidate(const BacktestEngine& engine,
        const native_order::LiveRequest& live, const native_order::MatchCursor& cursor,
        double resolved, const native_order::ExecutionPlan* plan_override = nullptr) const;
    NativeExecutionTermsFacts build_terms_facts(
        const BacktestEngine& engine, const native_order::LiveRequest& live,
        const native_order::EvaluationContext& evaluation,
        native_order::NativeCandidatePriceKind price_kind,
        NativeCurrentPriceRule price_rule, double raw_price,
        double default_resolved_price, bool shared_cursor_collision = false) const;
    static native_order::ExecutionPlan plan_from_terms(
        native_order::HostSizedKind kind, std::optional<native_order::Side> side,
        native_order::OpeningShape shape, double after, double allowance_left,
        double opposite_book_units);
    // L3 kernel sizing bases. resolve_sized_units answers the units a Sized
    // opening or a ScopeFraction reduce claims at this candidate; nullopt is
    // the nonrepresentable basis the matching path reports as TermsUnresolved.
    std::optional<double> resolve_sized_units(
        const BacktestEngine& engine, const native_order::LiveRequest& live,
        const NativeExecutionTermsFacts& facts) const;
    double sibling_claimed_units(const native_order::LiveRequest& live) const noexcept;
    // L3b placement-time sizing. sizing_point_price answers the price a Sized
    // request's basis converts at when it is accepted; placement_scope_units
    // measures the exposure a ScopeBasis::AtAcceptance fraction freezes; and
    // admit_placement_units runs the run's opening admission against an
    // acceptance-resolved quantity before the request exists.
    double sizing_point_price(const NativeRunSpec& spec, const native_order::Sized& sized,
                              double decision_price) const noexcept;
    std::optional<double> placement_scope_units(
        const BacktestEngine& engine, const native_order::Request& request) const;
    bool admit_placement_units(const BacktestEngine& engine, const native_order::Sized& sized,
                               double units, double price) const;
    std::optional<NativeCurrentExecutionResult> consume_matched_request(
        BacktestEngine& engine, const native_order::RequestHandle& handle,
        const native_order::EvaluationContext& evaluation, double raw_price,
        double default_resolved_price, const NativeCurrentPointView& notification_point,
        native_order::NativeCandidatePriceKind price_kind,
        NativeCurrentPriceRule price_rule, bool shared_cursor_collision = false);
    std::optional<NativeCurrentExecutionResult> terminal_from_history(
        BacktestEngine& engine, const native_order::RequestHandle& cause_handle,
        NativeFailureOperation operation);
    NativeCurrentPointView execution_anchor(const native_order::MatchCursor& cursor,
                                            double resolved) const;
    void enqueue_applied_notification(AppliedNotification notification);
    void drain_applied_notifications(BacktestEngine& engine);
    void invoke_applied_callback(BacktestEngine& engine, const AppliedNotification& notification);
    void finish_callback(BacktestEngine& engine, uint64_t ordinal);
    std::vector<native_order::OpeningObservation> read_openings(
        const BacktestEngine& engine, const std::vector<native_order::RequestHandle>& handles,
        int64_t cycle) const;
    void refresh_openings(const BacktestEngine& engine,
        std::vector<native_order::OpeningObservation>& openings) const noexcept;
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

    // One declared higher-timeframe series, live for the length of a run.
    // The evaluator itself lives in BacktestEngine::security_eval_states_
    // under `sec_id`; this is the consumer's own delivery bookkeeping.
    struct TimeframeSubscription {
        std::size_t index = 0;   // NativeRunSpec::subscriptions index
        int sec_id = 0;
        native_calendar::Timeframe tf{};
        bool lookahead = false;
        // barmerge.gaps_on: clear `latest` on every accepted input this
        // series delivers nothing on. Off by default, and off is the whole
        // established surface.
        bool gaps = false;
        // The latest delivered bucket, what native_series_bar() answers.
        std::optional<Bar> latest;
        // lookahead_off: the bucket being accumulated. -1 until an input
        // opens one.
        int bucket_first_index = -1;
        std::int64_t bucket_first_ms = 0;
        // lookahead_on: the whole series, resolved at begin over the
        // historical input (a stream's is its warmup), each bucket keyed to
        // the input index it is delivered on.
        std::vector<Bar> projected_bars;
        std::vector<int> projected_first_index;
        std::vector<std::int64_t> projected_first_ms;
        std::vector<NativeCompletionKind> projected_completion;
        std::size_t projected_cursor = 0;
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

    enum class CallbackPhase : std::uint8_t {
        None = 0,
        PreOpen = 1,
        Bar = 2,
        Applied = 3,
        Tick = 4,
    };

    struct AppendDigest {
        uint64_t h = 1469598103934665603ULL;
        uint64_t count = 0;
        void reset() noexcept {
            h = 1469598103934665603ULL;
            count = 0;
        }
    };

    // Derived read-only observations for ordinary OHLC points. They are not
    // matching state: every request/position mutation clears this cache.
    struct CohortTargetCacheEntry {
        native_order::RequestHandle handle{};
        native_order::TargetObservation target{};
    };

    // R4-D L10z review fix 4: the interval lookup cache is per-consumer state,
    // not per-thread state. Two engines sharing a thread have independent
    // calendars/timeframes, so a timestamp-keyed cache must not be shared.
    struct IntervalCache {
        std::int64_t input_ts = std::numeric_limits<std::int64_t>::min();
        std::optional<native_calendar::NativeInterval> input_interval;
        std::int64_t script_ts = std::numeric_limits<std::int64_t>::min();
        std::optional<native_calendar::NativeInterval> script_interval;
        void clear() noexcept {
            input_ts = std::numeric_limits<std::int64_t>::min();
            input_interval.reset();
            script_ts = std::numeric_limits<std::int64_t>::min();
            script_interval.reset();
        }
    };

    bool failed() const noexcept;
    bool recoverable_abort() const noexcept;
    void latch_failure(NativeFailure failure) noexcept;
    void fail(BacktestEngine& engine, NativeFailure failure) noexcept;
    void render(BacktestEngine& engine, const char* text) const;
    const NativeRunSpec* spec_ptr() const;
    bool commands_allowed() const;
    bool timeframe_args_ok(const std::string& input_tf, const std::string& script_tf) const;
    bool has_undetected_timeframe() const noexcept;
    bool legacy_tolerant_slot_labels() const noexcept;
    bool uses_raw_label_partition() const noexcept;
    static native_calendar::NativeInterval timestamp_partition(std::int64_t timestamp) noexcept;
    std::optional<native_calendar::NativeInterval> input_interval_at(std::int64_t timestamp) const;
    std::optional<native_calendar::NativeInterval> script_interval_at(std::int64_t timestamp) const;
    bool validate_undetected_begin(BacktestEngine& engine, const NativeBeginArgs& args);
    bool apply_spec(BacktestEngine& engine, const NativeRunSpec& spec);
    bool projection_ok(const BacktestEngine& engine) const;
    bool begin_ready(BacktestEngine& engine, NativeRunPhase phase, int64_t initial_floor_ms);
    bool prepare_public_begin(BacktestEngine& engine, const NativeBeginArgs& args);
    bool apply_staged_ingress(BacktestEngine& engine);
    bool refuse_mixed_input_mode(BacktestEngine& engine, InputMode requested);
    void select_input_mode(InputMode requested);
    bool admit_public_begin(BacktestEngine& engine, const char* not_ready_text);
    bool admit_public_stream_input(BacktestEngine& engine, NativeFailureOperation operation);
    bool preflight_bars(BacktestEngine& engine, const Bar* bars, int n, bool stream,
                        bool preserve_status = false);
    bool preflight_intrabar_path(BacktestEngine& engine);
    void pump_batch(BacktestEngine& engine, const Bar* bars, int n);
    bool consume_confirmed_input(BacktestEngine& engine, const Bar& bar, int index, bool last);
    bool contribute_input(BacktestEngine& engine, const Bar& bar,
                          const native_calendar::NativeInterval& interval,
                          int index, InputContribution kind);
    // Declared higher-timeframe series. Every one of these is a no-op for a
    // spec whose `subscriptions` are empty, which is every source-projected
    // spec, so the adapter pays nothing and cannot observe them.
    void clear_timeframe_subscriptions(BacktestEngine& engine);
    bool begin_timeframe_subscriptions(BacktestEngine& engine, const NativeRunSpec& spec,
                                       const Bar* input_bars, int n_input, bool is_stream);
    bool project_timeframe_subscription(BacktestEngine& engine,
                                        TimeframeSubscription& subscription,
                                        const Bar* input_bars, int n_input);
    bool pump_timeframe_subscriptions(BacktestEngine& engine, const Bar& bar, int index);
    // A declared series is fed by accepted CONFIRMED input only, because that
    // is the only input a batch of the same bars also has. True (refused)
    // exactly when a stream that declares one is asked for tick-driven input.
    bool refuse_subscription_tick_input(BacktestEngine& engine);
    bool deliver_timeframe_bar(BacktestEngine& engine, TimeframeSubscription& subscription,
                               const Bar& bucket, std::int64_t first_contributing_ms,
                               std::int64_t delivered_at_ms, NativeCompletionKind completion);
    void seal_script(BacktestEngine& engine, NativeCompletionKind kind);
    void deliver_confirmed_script(BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base);
    void deliver_intrabar_script(BacktestEngine& engine, const Bar& bar,
                                 const NativeCoordinate& base);
    void deliver_aggregate_calculation(BacktestEngine& engine, const Bar& bar,
                                       const NativeCoordinate& base);
    int64_t calculation_time(const NativeCoordinate& base) const noexcept;
    // Report truth for bare hosts (NativeReportPolicy::KernelRecorded). Both
    // are reporting-only: they mark equity and synthesize report rows, and
    // never book cash, place an order or move the broker book.
    void record_script_report_point(BacktestEngine& engine, int64_t script_open_ms) const;
    void record_report_point(BacktestEngine& engine, int64_t report_ts) const;
    void record_open_position_report_rows(BacktestEngine& engine) const;
    void match_point(BacktestEngine& engine, const NativeDriverPoint& point);
    void match_discrete(BacktestEngine& engine, const NativeDriverPoint& point);
    void match_segment(BacktestEngine& engine, const NativeDriverPoint& dest, double from_price);
    void match_path(BacktestEngine& engine, const NativeDriverPoint& point,
                    bool continuous, double from_price, double to_price);
    void apply_excursion(BacktestEngine& engine, double price);
    void invoke_bar_open_callback(BacktestEngine& engine, const Bar& bar,
                                  const NativeDriverPoint& point);
    // L5 calculation timing. Every calculation of the run is routed through
    // invoke_recalculation, whose BarClose reason is the script bar's own
    // calculation; the default host forwarding keeps on_native_bar exactly
    // what it was. A recalculation never records a report point: only the
    // script calculation does (record_script_report_point).
    void invoke_recalculation(BacktestEngine& engine, const Bar& bar,
                              NativeCalculationReason reason,
                              const native_order::ExecutionAppliedEvent* cause);
    // One Tick recalculation at a modeled path point / observed print, for a
    // spec that asked for EveryModeledPoint. Inert for every other spec.
    void recalculate_at_point(BacktestEngine& engine, const Bar& bar,
                              const NativeDriverPoint& point);
    void invoke_sub_bar_callback(BacktestEngine& engine, const Bar& sub,
                                 const NativeDriverPoint& point);
    // Frame bookkeeping shared by the mid-path callbacks above and by
    // invoke_applied_callback: one owning current point, one decision
    // context, and the engine's callback timestamp.
    void enter_point_frame(BacktestEngine& engine, const NativeCurrentPointView& point,
                           CallbackPhase phase);
    NativeCurrentPointView point_frame_view(const NativeDriverPoint& point) const;
    const Bar& calculating_bar(const BacktestEngine& engine) const noexcept;
    NativeCalculationTrigger calculation_trigger() const noexcept;
    bool recalculates_on_fills() const noexcept;
    // The lookahead-free bar so far. Folded from the modeled points as they
    // are presented, keyed by the script bar's own open so a new script bar
    // always restarts it; `volume` accrues only activity actually consumed
    // (completed lower sub-bars, observed prints).
    void note_partial_point(int64_t script_open_ms, double price, double volume_delta);
    void clear_partial() noexcept;
    // One matching point's recalculation budget. Points are identified by a
    // monotone epoch raised whenever the consumer establishes a new cursor,
    // so executions a callback drives through execute_current at that same
    // cursor spend the same budget as the matched fill that started them.
    void open_point_epoch() noexcept;
    bool claim_recalculation() noexcept;
    bool invoke_input_callback(BacktestEngine& engine, const Bar& bar,
                               const NativeInputContext& context);
    bool invoke_tick_callback(BacktestEngine& engine, const Bar& bar,
                              const NativeTickContext& context);
    void invoke_callback(BacktestEngine& engine, const Bar& bar, const NativeCoordinate& coordinate);
    uint64_t take_ordinal(BacktestEngine& engine);
    void raise_floor(int64_t t);
    native_order::DriverEligibilityClass classify_driver(
            const NativeDriverPoint& point, bool continuous) const noexcept;
    native_order::MatchCursor make_cursor(const NativeDriverPoint& point, double t) const noexcept;
    native_order::PositionIdentity read_position(const BacktestEngine& engine) const;
    native_order::OpeningObservation read_opening(
            const BacktestEngine& engine, const native_order::RequestHandle& opening,
            int64_t cycle) const;
    native_order::TargetObservation read_target(
            const BacktestEngine& engine, const native_order::LiveRequest* live) const;
    const native_order::TargetObservation* cached_cohort_target(
            const BacktestEngine& engine, const native_order::LiveRequest& live);
    void clear_cohort_target_cache() noexcept;
    void retarget_cohort_target_cache(const native_order::RequestHandle& predecessor,
                                      const native_order::RequestHandle& successor) noexcept;
    native_order::CommandContext make_command_context(
            const BacktestEngine& engine, const native_order::Request& request,
            native_order::CommandSurface surface) const;
    void refresh_target_scalars(const BacktestEngine& engine,
                                native_order::TargetObservation& target) const noexcept;
    std::optional<native_order::Side> cohort_side(
        const BacktestEngine& engine, const native_order::LiveRequest& live) const;
    bool request_is_buy(const BacktestEngine& engine,
                        const native_order::LiveRequest& live) const;
    bool admit_opening_inspect(const BacktestEngine& engine, double resolved_price,
                               const execution::SettlementInspection& inspect,
                               bool skip_initial_margin,
                               native_order::MatchRejectReason* reason) const;
    // L4 generic margin model. Every one of these is inert for a spec that
    // leaves `margin` unset, which is every source-projected spec.
    const NativeMarginModel* margin_model() const noexcept;
    std::optional<double> maintenance_fraction(bool short_side) const noexcept;
    // Solve equity(P) == maintenance requirement(P) for the live book.
    std::optional<double> liquidation_level(const BacktestEngine& engine) const;
    // The equity one maintenance test is made against, on the model's basis.
    double margin_equity(const BacktestEngine& engine, double mark) const;
    // The host's gate over one kernel check point. True keeps the check.
    bool margin_check_admitted(const BacktestEngine& engine, NativeMarginCheckKind kind,
                               const native_order::MatchCursor& cursor, double mark) const;
    // The price the breach is measured at: the most adverse price the modeled
    // script path still reaches after `phase`, or `fallback` when the point
    // has no remaining modeled path of its own.
    double margin_sizing_price(bool short_side, NativePathPhase phase,
                               double fallback) const noexcept;
    // Kernel numbers, the host's requirement decision, the breach test, the
    // kernel sizing, then the host's units override. nullopt means no
    // liquidation.
    std::optional<double> margin_call_units(
        const BacktestEngine& engine, double mark, const native_order::MatchCursor& cursor,
        NativeMarginCheckKind kind, double* out_equity, double* out_required) const;
    void withdraw_margin_liquidation(BacktestEngine& engine);
    void maintain_margin_liquidation(BacktestEngine& engine,
                                     const native_order::MatchCursor& cursor,
                                     NativePathPhase phase, double fallback_price,
                                     NativeMarginCheckKind kind);
    void calculation_margin_check(BacktestEngine& engine, const NativeCoordinate& calc,
                                  double mark);
    // One kernel-originated reduction: the margin model's liquidation, and
    // (L9) the risk block's own flatten. `origin`, `label` and `comment` name
    // which, and only a resting liquidation is retained as margin state.
    bool kernel_submit_liquidation(BacktestEngine& engine, double level, double units,
                                   std::int64_t decision_time_ms,
                                   native_order::RequestHandle* out_handle = nullptr,
                                   native_order::RequestOrigin origin =
                                       native_order::RequestOrigin::KernelLiquidation,
                                   const char* label = nullptr,
                                   const char* comment = nullptr);
    std::optional<std::size_t> record_margin_call(
        BacktestEngine& engine, const native_order::ExecutionAppliedEvent& applied,
        const native_order::DefinitionRef& definition, double position_before,
        double position_after);
    // L9 generic risk limits. Every one of these is inert for a spec that
    // leaves `risk` unset, which is every source-projected spec.
    const NativeRiskLimits* risk_limits() const noexcept;
    bool risk_blocked() const noexcept;
    // The risk day of an instant on the spec's own basis: the session day of
    // the run's calendar, or the civil day of the all-day calendar built for
    // the spec's timezone. nullopt where the calendar has no answer, which
    // leaves the ledger on the day it is already on. Every point of a script
    // bar is keyed on that bar's OWN open, so a bar never straddles two risk
    // days: its close point belongs to the day it opened in.
    std::optional<std::int64_t> risk_day(std::int64_t timestamp_ms) const;
    // Roll the ledger onto `day`, closing the previous one: its realized
    // result decides the consecutive-loss streak, the fill count restarts and
    // the opening equity is marked at `mark`.
    void risk_roll_day(const BacktestEngine& engine, std::int64_t day, double mark);
    // One applied fill of the risk day at `coordinate`, counted before any
    // evaluation. It never fires a breach: settlement is not a decision point.
    void risk_note_fill(const BacktestEngine& engine, const NativeCoordinate& coordinate,
                        double price);
    // The evaluation itself, at a script-bar open, at that bar's own close
    // calculation, and after an applied drain. `phase` is the frame a
    // FlattenAndBlock breach executes its own flatten in: the bar-open point's
    // PreOpen, the drained fill's Applied, or the calculation's Bar.
    // `owns_frame` is false exactly where the caller already holds that frame
    // (the close calculation), so the flatten borrows it instead of opening
    // and closing a second one.
    void risk_evaluate(BacktestEngine& engine, const NativeCurrentPointView& point,
                       CallbackPhase phase, bool owns_frame = true);
    void risk_fire(BacktestEngine& engine, native_order::RiskLimitKind kind, double limit,
                   double observed, const NativeCurrentPointView& point, CallbackPhase phase,
                   bool owns_frame);
    void fail_preparation(BacktestEngine& engine, const native_order::PreparationError& error,
                          NativeFailureOperation operation);
    void catch_up_timeline() noexcept;
    bool install_mutation(BacktestEngine& engine, native_order::PreparedMutation&& prepared,
                          NativeFailureOperation operation, uint64_t ordinal);
    bool install_execution(BacktestEngine& engine, native_order::PreparedExecution&& prepared,
                           const native_order::CommittedExecutionFacts& facts,
                           uint64_t ordinal);
    native_order::SubmitResult submit_with_surface(
            BacktestEngine& engine, const native_order::Request& request,
            native_order::CommandSurface surface);
    native_order::ReplaceResult replace_with_surface(
            BacktestEngine& engine, const native_order::RequestHandle& target,
            const native_order::Request& request, native_order::CommandSurface surface,
            native_order::ReplaceOptions options = {});
    void drain_after_applied(BacktestEngine& engine, const native_order::EventId& applied,
                             const native_order::RequestHandle& filler);
    void drain_parent_terminal(BacktestEngine& engine, const native_order::EventId& cause,
                               const native_order::RequestHandle& parent,
                               NativeFailureOperation operation);
    void drain_dependency_queue(
            BacktestEngine& engine,
            std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> seeds,
            NativeFailureOperation operation);
    void observe_trails(BacktestEngine& engine, const NativeDriverPoint& point,
                        const native_order::MatchCursor& cursor, bool continuous,
                        double price);
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
    bool pre_open_birth_eligible(const native_order::RequestHandle&,
                                 const NativeDriverPoint&) const noexcept;
    void record_pre_open_birth(const native_order::Request&, const native_order::RequestHandle&);
    void note_terminal_events(const native_order::EventRange& events) noexcept;

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
    std::optional<native_calendar::Timeframe> intrabar_tf_;
    native_calendar::TimeframeCompatibility pairing_{};
    NativeRunSpec applied_{};
    std::optional<NativeFxCurve> staged_fx_curve_;
    bool staged_ingress_fx_ = false;
    bool in_callback_ = false;
    CallbackPhase callback_phase_ = CallbackPhase::None;
    bool preparing_begin_ = false;
    mutable bool consuming_request_ = false;
    bool draining_notifications_ = false;
    std::optional<CurrentExecutionFrame> current_frame_;
    uint64_t pre_open_birth_point_ordinal_ = 0;
    int64_t pre_open_birth_time_ms_ = 0;
    std::vector<native_order::RequestHandle> pre_open_births_;
    std::vector<AppliedNotification> applied_notifications_;
    std::size_t notification_head_ = 0;
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
    // Declared higher-timeframe series: empty for every spec that declares
    // none, which is the whole source-projected population.
    std::vector<TimeframeSubscription> subscriptions_{};
    // Owned copy of the historical input's forward look, input_next_ms_[i]
    // being input bar i+1's timestamp (0 for the last, and for every live
    // stream input, whose successor nobody has yet). The calendar aggregators
    // need it to complete a D/W/M bucket on the period's actual last bar;
    // allocated only for a run that declares a subscription.
    std::vector<std::int64_t> input_next_ms_{};
    // How many of the inputs the declared series were resolved over at begin
    // are warmup, i.e. where a stream's historical phase ends and its live
    // phase starts. -1 for a batch run, whose every input is historical.
    int subscription_warmup_inputs_ = -1;
    // Borrowed begin arguments, valid only inside one public begin call.
    const Bar* begin_bars_ = nullptr;
    int begin_n_ = 0;
    bool begin_is_stream_ = false;
    Bar forming_{};
    bool has_forming_ = false;
    double last_price_ = 0.0;
    bool has_last_price_ = false;
    int64_t last_print_time_ms_ = 0;
    std::vector<NativeDriverPoint> driver_log_;
    std::vector<NativeAccountObservation> account_log_;
    NativeDecisionContext callback_context_{};
    std::optional<NativeInputContext> input_callback_context_;
    std::optional<Bar> input_callback_bar_;
    std::optional<NativeTickContext> tick_callback_context_;
    std::optional<Bar> tick_callback_bar_;
    NativeDriverStatistics driver_statistics_{};
    std::optional<native_calendar::TimezoneIdentityDescriptor> tz_identity_{};
    // Derived receipt cursor: it can be reconstructed from the immutable
    // command history and only lets source projections skip empty polls.
    uint64_t terminal_receipt_high_water_ = 0;
    // L4: the resting kernel liquidation and the modeled script path it is
    // sized against. Both are folded into the continuation digest only when
    // the run spec declares a margin model.
    std::optional<MarginLiquidation> margin_liquidation_;
    // Kernel liquidations already booked at one driver point. A kernel-sized
    // slice always restores or flattens, so it re-arms at most once per fill;
    // this bounds a host override that keeps answering with a smaller slice.
    uint64_t margin_point_ordinal_ = 0;
    std::uint32_t margin_point_calls_ = 0;
    Bar margin_path_bar_{};
    bool has_margin_path_ = false;
    bool margin_path_high_first_ = false;
    // L9: the risk ledger and the all-day calendar a CalendarDayInTimezone
    // basis keys on. The ledger folds into the continuation digest only under
    // a declared `risk` block; the calendar is derived from the already
    // hashed spec and folds nothing, exactly as `calendar_` does.
    RiskLedger risk_{};
    std::optional<native_calendar::SessionCalendar> risk_day_calendar_;
    std::array<CohortTargetCacheEntry, 16> cohort_target_cache_{};
    std::size_t cohort_target_cache_size_ = 0;
    // Derived calendar lookup cache, cleared at staged ingress (L10c).
    mutable IntervalCache interval_cache_{};
    mutable AppendDigest history_digest_{};
    mutable AppendDigest driver_digest_{};
    mutable AppendDigest account_digest_{};
    // A host-owned margin verdict is part of the continuation only when the
    // generic spec actually exposes an initial-margin gate.  Source specs do
    // not set that gate, preserving their established fingerprint while the
    // new generic authority remains hash-visible for native hosts.
    mutable AppendDigest precommit_digest_{};
    // L5 calculation timing. All of this is derived observation over the
    // already hashed driver/notification state: the partial bar is the fold
    // of points the driver log already holds, the epoch is a cursor counter,
    // and the two totals are readbacks. They fold into the continuation
    // digest only for a spec that actually opted into a non-default cadence,
    // so a default spec keeps the continuation identity it had before this
    // lane (the precommit_digest_ precedent).
    Bar partial_{};
    bool partial_has_ = false;
    // The bar a mid-path callback is calculating: the script bar under
    // delivery in batch, the print's value bar in a stream. Borrowed nowhere:
    // it is an owning copy taken when delivery begins.
    Bar calculating_bar_{};
    bool calculating_bar_has_ = false;
    int64_t partial_script_open_ms_ = 0;
    uint64_t point_epoch_ = 0;
    uint64_t recalc_epoch_ = 0;
    uint32_t recalc_epoch_count_ = 0;
    uint64_t recalculations_ = 0;
    uint64_t recalculations_skipped_ = 0;
};

inline NativeExecutionConsumer& as_native_consumer(IExecutionConsumer& consumer) {
    return static_cast<NativeExecutionConsumer&>(consumer);
}

}  // inline namespace engine_script_run_v18
}  // namespace pineforge
