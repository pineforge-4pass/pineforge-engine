#pragma once

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include "native_calendar_memo.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

// Lookup state a host derives from its own bookkeeping and cannot keep in its
// own object -- a generated script's ABI fixes that object's layout -- parked
// with the consumer that serves the host (R5 lane PERF-P7). The consumer owns
// it for the host, drops it at every run begin, so it never outlives the run
// it was built over, and never reads it: nothing in it is hashed, recorded or
// otherwise observable, and the host verifies whatever it reads back.
class NativeHostCache {
public:
    NativeHostCache() = default;
    NativeHostCache(const NativeHostCache&) = delete;
    NativeHostCache& operator=(const NativeHostCache&) = delete;
    virtual ~NativeHostCache() = default;
    // How many lookups the cache answered in place of the host's own scan.
    virtual std::uint64_t answered() const noexcept = 0;
};
// The journal window's stress switch (NativeExecutionConsumer::
// set_retire_every_point): a build compiled with
// -DPINEFORGE_STRESS_RETIRE_EVERY_POINT starts every consumer with it on, so a
// harness that cannot reach the consumer -- the corpus, the kernel battery --
// runs every Window run retiring at every driver point. Off in every shipped
// build.
#ifdef PINEFORGE_STRESS_RETIRE_EVERY_POINT
inline constexpr bool kRetireEveryPointDefault = true;
#else
inline constexpr bool kRetireEveryPointDefault = false;
#endif

class NativeExecutionConsumer final : public IExecutionConsumer {
public:
    // The digests the spec fold takes of the run spec's three bar arrays
    // (intrabar path, declared series, auxiliary feed). See spec_bar_digests_.
    struct SpecBarDigests {
        std::uint64_t intrabar = 0;
        std::uint64_t subscriptions = 0;
        std::uint64_t auxiliary = 0;
    };

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
    // One named result, so the view is built in the caller's slot.
    NativeStateView view() const {
        NativeStateView v;
        v.consumed_high_water = consumed_high_water_;
        v.decision_floor_ms = decision_floor();
        // running_spec_ is non-null exactly while state_ holds NativeRunning
        // (see cache_running_policy), whose spec it points at: the view the
        // lifecycle probe builds for that alternative, without walking the
        // variant first.
        if (running_spec_) {
            v.kind = NativeLifecycleKind::Running;
            v.spec = running_spec_;
            v.phase = std::get_if<NativeRunning>(&state_)->phase;
        } else {
            probe_lifecycle(v);
        }
        return v;
    }
    void probe_lifecycle(NativeStateView& v) const;
    // The leg order this run walks over `bar`: the declared
    // NativeRunSpec::path_order, and the open-proximity rule under Auto or
    // before a spec is configured. It is the order the confirmed-bar driver
    // walks, and the one a question asked from a host callback is answered
    // in (BacktestEngine::declare_opened_lot_entry_bar_mask).
    bool path_high_first(const Bar& bar) const;
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
    std::size_t cancel_where(BacktestEngine& engine, std::string_view text,
                             NativeRequestField field = NativeRequestField::Comment);
    native_order::CohortHandle cohort_open(BacktestEngine& engine);
    void cohort_add(BacktestEngine& engine, native_order::CohortHandle cohort,
                    native_order::RequestHandle origin);
    void cohort_remove(BacktestEngine& engine, native_order::CohortHandle cohort,
                       native_order::RequestHandle origin);
    std::optional<NativeCurrentPointView> current_execution_point() const {
        if (!in_callback_ || !current_frame_ || !std::holds_alternative<NativeRunning>(state_))
            return std::nullopt;
        return current_frame_->point;
    }
    std::optional<NativeTrailState> trail_state(
        const BacktestEngine& engine, const native_order::RequestHandle& target) const;
    NativeCurrentExecutionPreview inspect_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const;
    NativeCurrentExecutionResult execute_current(
        BacktestEngine& engine, const NativeCurrentExecution& command);
    // The book as one aggregate, off the engine's lots. Defined here so every
    // host read of it (NativeStrategyHost::physical_position) and every margin
    // check inlines it.
    NativePhysicalPosition position(const BacktestEngine& engine) const {
        NativePhysicalPosition out;
        const auto n = engine.pyramid_entries_.size();
        out.lot_count = n;
        if (n == 0) return out;
        if (n == 1) {
            const auto& lot = engine.pyramid_entries_[0];
            out.signed_units = engine.position_side_ == PositionSide::SHORT ? -lot.qty : lot.qty;
            // R4-D L10z review fix 5: the single-lot fast path keeps the
            // weighted path's zero-quantity guard, so an empty lot reports no
            // average.
            out.average_price = lot.qty > 0.0 ? lot.price : 0.0;
            return out;
        }
        double qty = 0.0;
        double weighted = 0.0;
        for (const auto& lot : engine.pyramid_entries_) {
            qty += lot.qty;
            weighted += lot.qty * lot.price;
        }
        out.signed_units = engine.position_side_ == PositionSide::SHORT ? -qty : qty;
        out.average_price = qty > 0.0 ? weighted / qty : 0.0;
        return out;
    }
    std::vector<NativeOpenLot> open_lots(const BacktestEngine& engine, double mark) const;
    double marked(const BacktestEngine& engine, double price) const;
    std::optional<double> host_liquidation_price(const BacktestEngine& engine) const;
    // NativeStrategyHost::native_sized_units: the L3 basis arithmetic under the
    // applied spec, as a pure query (R5 N11).
    std::optional<double> sized_units_preview(const native_order::Sized& sized, double price,
                                              double equity, double fx) const;
    NativeRiskState risk_state() const;
    // The engine's NativeStrategyHost view, dynamic_cast's own answer: taken
    // once per public begin (prepare_public_begin) for the engine that owns
    // this consumer, whose dynamic type is fixed for the consumer's whole
    // life, and read back by every callback site. Any other engine is
    // answered by dynamic_cast directly.
    NativeStrategyHost* native_host(BacktestEngine& engine) const noexcept {
        return &engine == downcast_engine_ ? downcast_host_
                                           : dynamic_cast<NativeStrategyHost*>(&engine);
    }
    const NativeStrategyHost* native_host(const BacktestEngine& engine) const noexcept {
        return &engine == downcast_engine_ ? downcast_host_
                                           : dynamic_cast<const NativeStrategyHost*>(&engine);
    }
    // The consumer `engine` is bound to, read straight off its slot: what
    // BacktestEngine::execution_consumer() answers, without that out-of-line
    // (and, on hardened toolchains, stack-protected) call behind every
    // NativeStrategyHost read -- the Pine adapter makes some forty a bar (R5
    // lane PERF-L1). A slot left empty still goes through
    // execution_consumer(), which binds a consumer exactly as it always has.
    static NativeExecutionConsumer& bound(const BacktestEngine& engine) {
        IExecutionConsumer* consumer = engine.execution_consumer_slot_.ptr.get();
        if (consumer == nullptr) {
            consumer = &const_cast<IExecutionConsumer&>(engine.execution_consumer());
        }
        return static_cast<NativeExecutionConsumer&>(*consumer);
    }
    std::vector<NativeMarketEvent> events_after(uint64_t after_ordinal) const;
    // The command rows of events_after(after_ordinal), read in place (R5 lane
    // PERF-P4). The history is in ordinal order, so those rows are exactly
    // the events from first_command_after(after_ordinal) on (the history's
    // size when there are none); events_after takes its own slice here too.
    // visit_commands_after visits them as the history stood when the call
    // began, in history order, with one COPY per event -- the copy each
    // materialised row was -- so a visitor that appends to the history (a
    // cancel it issues) neither sees what it appended nor holds an event the
    // append reallocated. It stops at, and answers, the first true a visitor
    // returns. Driver points and account rows are never visited.
    std::size_t first_command_after(uint64_t after_ordinal) const noexcept;
    // Whether a command event above `after_ordinal` is in the journal: the
    // first test first_command_after makes, which answers the history's size
    // exactly when it fails. A reader that only asks whether anything lies
    // past its cursor asks this, in O(1), rather than comparing that search
    // with a second one from the top (R5 lane D2-A).
    bool has_command_after(uint64_t after_ordinal) const noexcept {
        const auto& history = requests_.history();
        return !history.empty()
            && std::visit([](const auto& payload) { return payload.ordinal; }, history.back())
                > after_ordinal;
    }
    template <class Visit>
    bool visit_commands_after(uint64_t after_ordinal, Visit&& visit) const {
        const auto& history = requests_.history();
        const std::size_t end = history.size();
        // The history only grows inside a run; the size is re-read anyway so
        // a visit can never index past it.
        for (std::size_t index = first_command_after(after_ordinal);
             index < end && index < history.size(); ++index) {
            const native_order::CommandEvent event = history[index];
            if (visit(event)) return true;
        }
        return false;
    }
    uint64_t event_high_water() const noexcept;
    // V19-B, the reader side of the journal window
    // (NativeStrategyHost::native_acknowledge_events /
    // native_event_window_start). acknowledge_events records that the host
    // has read every event through `through_ordinal`: clamped to the event
    // high water, never lowered, and nothing at all outside a running run.
    // event_window_start is the oldest ordinal a read can still return.
    void acknowledge_events(uint64_t through_ordinal) noexcept;
    uint64_t event_window_start() const noexcept { return requests_.retired_through() + 1; }
    // The v19 broker-state hash's closed-row half (pineforge-broker-state/v19,
    // BacktestEngine::broker_state_hash_from_execution_hash): a running digest
    // of `rows`' six folded fields, each final row folded once. See the
    // definition for the finality rule. closed_rows_digest_holds re-folds the
    // rows the digest has taken and answers whether they still match: the
    // Debug check a run's end makes, and a witness's probe.
    uint64_t closed_rows_digest(const std::vector<Trade>& rows) const;
    bool closed_rows_digest_holds(const std::vector<Trade>& rows) const noexcept;
    // NativeStrategyHost::native_closed_rows_amended: the rows from `first` on
    // changed after they were final; the next read folds them again.
    void closed_rows_amended(std::size_t first) noexcept { rewind_closed_rows(first); }
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
    // NativeStrategyHost::declare_timeframe_subscriptions_result: replace the
    // staged list from inside on_native_run_begin, before the kernel
    // registers. The bool spelling on the host is this answer's status.
    NativeSetupResult declare_timeframe_subscriptions(
        std::vector<NativeTimeframeSubscription> declared);
    // NativeStrategyHost::declare_auxiliary_feed_result: replace the staged
    // feed from inside on_native_run_begin, before the kernel registers.
    NativeSetupResult declare_auxiliary_feed(std::optional<NativeAuxiliaryFeed> declared);
    // NativeStrategyHost::append_auxiliary_bars_result: a realtime stream's
    // later bars of the declared feed, queued for the next accepted input.
    // The bool spelling on the host is this answer's status.
    NativeAuxiliaryAppendResult append_auxiliary_bars(BacktestEngine& engine, const Bar* bars,
                                                      std::size_t n);
    // L5 calculation timing readbacks. The partial bar is the lookahead-free
    // bar so far at the current cursor; the two counters are observation of
    // the recalculation cadence, never matching state.
    std::optional<Bar> partial_bar() const;
    uint64_t recalculation_count() const noexcept { return recalculations_; }
    uint64_t recalculations_skipped() const noexcept { return recalculations_skipped_; }

    // The generic mark-to-market report producer: one row per open physical
    // lot at `mark_price`, dated `mark_time_ms` on `interval_index`, appended
    // to the engine's range-end row space through the same non-mutating row
    // builder every full close uses. Returns the summed NET row P&L.
    //
    // Reporting only: the live book, the realized sums, the equity curve and
    // every hash are left exactly as the run left them, so calling this can
    // never move a fill or a continuation. The caller owns the policy around
    // it — when to mark, what to clear first, and whether anything downstream
    // of the rows (an equity point, an extreme, a row order) is re-derived
    // from them. That is what lets the kernel's own run-end producer below
    // and a host with a different report shape share one loop instead of
    // each carrying its own (R5 audit lane Q6, first-round duplicate D5).
    double append_open_position_report_rows(
        BacktestEngine& engine, double mark_price, int64_t mark_time_ms,
        int interval_index) const;

    // match_path reuses the candidate rows it scanned instead of rescanning
    // the whole working book after an allowance refresh (R5 lane PERF-K3).
    // The reuse is exact, so it is not a run-spec choice: it is on unless
    // this turns it off, which restores the full rescan after every winner
    // and the core's handle lookup for the winner. The switch exists so
    // tests/test_native_match_row_reuse.cpp can hold the two computations
    // equal bit for bit; no host reaches it.
    void set_match_row_reuse(bool enabled) noexcept { match_row_reuse_ = enabled; }

    // match_path tests a priced trigger against the point's path before the
    // rest of its row (R5 lane PERF-L5): a request whose trigger the path
    // cannot reach is passed over without the eligibility read it would
    // otherwise take first. The pass-over is exact, so it is on unless this
    // turns it off, which restores the full evaluation of every row. The
    // switch exists so tests/test_native_match_band_precheck.cpp can hold the
    // two computations equal bit for bit; no host reaches it.
    void set_match_band_precheck(bool enabled) noexcept { match_band_precheck_ = enabled; }

    // The request core's mutations are applied in place by its direct forms
    // (WorkingRequestCore::apply_*, R5 lane L3) wherever the consumer used to
    // install what it had just prepared. The direct forms are exact, so this
    // is not a run-spec choice: it is on unless this turns it off, which
    // restores every prepare/install pair. The switch exists so
    // tests/test_native_direct_mutation.cpp can hold the two paths equal bit
    // for bit; no host reaches it.
    void set_direct_mutation(bool enabled) noexcept { direct_mutation_ = enabled; }

    // The per-point guards in front of drain_queued_notifications,
    // recalculate_at_modeled_point and the three margin check points skip
    // each body exactly where it would return on its first test (R5 lane
    // PERF-L1). Off, every guard calls its body, which makes that test itself:
    // the pre-lane computation, kept so tests/test_native_lean_path.cpp can
    // hold the two equal bit for bit. No host reaches it.
    void set_point_guards(bool enabled) noexcept { point_guards_ = enabled; }

    // A driver point with no live request is matched in front of match_path
    // (match_quiet_point, R5 lane D2-A), which would open its epoch, apply its
    // excursion and return. The short-cut is exact, so it is on unless this
    // turns it off, which hands every point to match_path; quiet_points
    // counts the points it matched this run. Both exist so
    // tests/test_native_quiet_point.cpp can hold the two computations equal
    // bit for bit and see the short-cut run; no host reaches either.
    void set_quiet_point_match(bool enabled) noexcept { quiet_point_match_ = enabled; }
    uint64_t quiet_points() const noexcept { return quiet_points_; }

    // The request core, read-only: tests/test_native_definition_index.cpp
    // holds its cohort receipts, rosters and membership answers -- the chain
    // index V19-B keeps in the core -- against an oracle folded over the whole
    // journal. No host reaches it.
    const native_order::WorkingRequestCore& request_core() const noexcept { return requests_; }

    // The host's parked lookup cache (NativeHostCache), or null. A host adopts
    // one per run and it lives until the next run begin or the next adoption.
    // Off, the consumer holds none and refuses adoption, so a host that falls
    // back to its own scans without a cache scans everything: the switch
    // exists so tests/test_adapter_purge_index.cpp and
    // tests/test_adapter_exit_leg_index.cpp can hold the Pine adapter's
    // cached lookups equal to its scans bit for bit; no host reaches it.
    NativeHostCache* host_cache() const noexcept { return host_cache_.get(); }
    NativeHostCache* adopt_host_cache(std::unique_ptr<NativeHostCache> cache) noexcept {
        if (!host_cache_enabled_) return nullptr;
        host_cache_ = std::move(cache);
        return host_cache_.get();
    }
    void set_host_cache(bool enabled) noexcept {
        host_cache_enabled_ = enabled;
        if (!enabled) host_cache_.reset();
    }
    // V19-B: under NativeEventRetention::Window the journal retires what its
    // host acknowledged at every script-bar boundary. This switch retires at
    // every driver point instead -- the most any reader can lose -- so a run
    // whose trades match its Full twin's proves that no kernel read touches a
    // retired event (tests/test_native_event_retention.cpp, and the corpus
    // and battery under -DPINEFORGE_STRESS_RETIRE_EVERY_POINT). A choice of
    // how early the window closes, never run state; no host reaches it.
    void set_retire_every_point(bool enabled) noexcept { retire_every_point_ = enabled; }
    // V19-B: keep `retention` of the event record instead of what the spec
    // declares, from the next begin on (nullopt restores the spec's). For a
    // test that reads a whole run's events from a host whose spec it does not
    // write -- the Pine host declares Window -- a readback choice alone: the
    // spec, its digest and every decision are the spec's. No host reaches it.
    void set_retention_override(std::optional<NativeEventRetention> retention) noexcept {
        retention_override_ = retention;
    }
    // The ExecutionAppliedEvents this run committed, retained or retired: what
    // a host that counts its fills reads once the window has retired them.
    // Derived, folded into nothing.
    std::uint64_t applied_event_count() const noexcept { return applied_events_; }

private:
    struct CurrentExecutionFrame {
        NativeCurrentPointView point;
        uint64_t acceptance_cutoff = 0;
        CurrentExecutionFrame() = default;
        CurrentExecutionFrame(const NativeCurrentPointView& at, uint64_t cutoff)
            : point(at), acceptance_cutoff(cutoff) {}
        // A callback's frame from its parts, so the optional that holds it
        // constructs it in place (R5 lane PERF-L1).
        CurrentExecutionFrame(const NativeDecisionContext& decision, double price,
                              NativeCurrentQuoteKind quote_kind, uint64_t quote_origin_ordinal,
                              uint64_t cutoff)
            : point{decision, price, quote_kind, quote_origin_ordinal},
              acceptance_cutoff(cutoff) {}
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
                               double units, double price, double fx) const;
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
    // With nothing queued a drain delivers, counts and re-arms nothing: the
    // queue is empty only after its own clear(), which also rewinds the head
    // (R5 lane PERF-L1), so every caller's empty drain is a branch.
    void drain_applied_notifications(BacktestEngine& engine) {
        if (!applied_notifications_.empty() || !point_guards_)
            drain_queued_notifications(engine);
    }
    void drain_queued_notifications(BacktestEngine& engine);
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
        // The declared literal, kept so a later begin can recognize the
        // evaluator states this consumer registered before erasing them.
        std::string tf_literal;
        bool lookahead = false;
        // barmerge.gaps_on: clear `latest` on every accepted input this
        // series delivers nothing on. Off by default, and off is the whole
        // established surface.
        bool gaps = false;
        // NativeSeriesSource::AuxiliaryFeed: the series is built from the
        // run's auxiliary feed, and `auxiliary_cursor` is the first feed bar
        // it has not consumed. False, and the cursor inert, for every series
        // built from the input, which is the whole established surface.
        bool auxiliary = false;
        std::size_t auxiliary_cursor = 0;
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

    // One candidate of match_path's winner selection at its current cursor.
    // `live_index` is the request's position in the working book when the row
    // was scanned: nothing mutates the book between a scan and the reuse of
    // its rows, so it names the request without a handle search.
    enum class MatchKind : std::uint8_t {
        Evaluate = 0,
        ActivateStop = 1,
        ActivateStopLimit = 2,
        BeginTrail = 3,
        ActivateTrail = 4,
        Fill = 5,
    };
    struct MatchCandidate {
        native_order::RequestHandle handle;
        uint64_t incarnation = 0;
        double t = 0.0;
        double price = 0.0;
        MatchKind kind = MatchKind::Fill;
        std::optional<double> trigger_level;
        bool at_level = false;
        bool shared_cursor_collision = false;
        std::size_t live_index = 0;
    };
    // Where a non-Evaluate row of match_path came from at its point: the row's
    // request, trigger state, side and level when it was scanned, and whether
    // its hit landed on the level. A later rescan of the same request at the
    // same key inherits `at_level` and `trigger_level` from it.
    struct MatchProvenance {
        native_order::RequestHandle handle;
        std::uint64_t point_ordinal = 0;
        MatchKind kind = MatchKind::Fill;
        std::size_t trigger_state_index = 0;
        bool is_buy = false;
        std::optional<double> trigger_level;
        double t = 0.0;
        double raw_price = 0.0;
        bool at_level = false;
    };
    // The candidate rows match_path keeps at its cursor while allowance
    // refreshes follow one another (R5 lane PERF-K3). The buffer keeps its
    // capacity from call to call; once the rows are reused they form a heap
    // whose top is the row the scan's own comparison, `precedes`, picks.
    //
    // A scan usually keeps its rows already in that order: the book is in
    // incarnation order, and a book of requests awaiting their allowance
    // gives every row the cursor's own t. While the kept rows stay an
    // ascending run they are taken from its front instead (R5 lane PERF-L5):
    // `precedes` orders rows totally, so the front of an ascending run is the
    // row a heap would put on top, and the first row out of order turns the
    // rest into that heap.
    class MatchRows {
    public:
        // The order match_path picks its winner in: the earliest cursor, then
        // the oldest request, then the kind.
        static bool precedes(const MatchCandidate& row, const MatchCandidate& other) noexcept {
            return row.t < other.t
                || (row.t == other.t && row.incarnation < other.incarnation)
                || (row.t == other.t && row.incarnation == other.incarnation
                    && static_cast<std::uint8_t>(row.kind)
                           < static_cast<std::uint8_t>(other.kind));
        }
        void clear() noexcept {
            rows_.clear();
            head_ = 0;
            ascending_ = true;
            heaped_ = false;
        }
        void keep(const MatchCandidate& row);
        // Takes the first row off -- the run's front, or the heap's top,
        // arranging the heap on first use -- and answers the working-book
        // index that row was scanned at.
        std::size_t take_first();
        const MatchCandidate* first() const noexcept {
            if (ascending_) return head_ < rows_.size() ? &rows_[head_] : nullptr;
            return rows_.empty() ? nullptr : &rows_.front();
        }

    private:
        void sift_up();
        std::vector<MatchCandidate> rows_;
        // While ascending_, the rows still to take are rows_[head_..] in
        // `precedes` order; the rows before head_ were taken.
        std::size_t head_ = 0;
        bool ascending_ = true;
        bool heaped_ = false;
    };

    // R4-D L10z review fix 4: the interval lookup cache is per-consumer state,
    // not per-thread state. Two engines sharing a thread have independent
    // calendars/timeframes, so a timestamp-keyed cache must not be shared.
    //
    // R5 lane PERF-L1: each kind also keeps the answer its slot held before
    // the slot's last replacement (`prior`). A bar asks for its own interval
    // around a lookahead to the next bar's (present_session_day), which used
    // to evict it, so canonical labels resolved the calendar six times a bar
    // for three instants. A lookup that misses the slot takes the prior's
    // answer when it is for the same instant -- the value a fresh resolution
    // gives, since nothing a resolution reads changes between two begins,
    // which clear the cache -- and the slot itself is written exactly as it
    // always was. `held` marks a slot that holds an answer rather than the
    // empty mark, so only answers are ever demoted.
    struct IntervalCache {
        struct Prior {
            bool held = false;
            std::int64_t ts = 0;
            std::optional<native_calendar::NativeInterval> interval;
        };
        std::int64_t input_ts = std::numeric_limits<std::int64_t>::min();
        std::optional<native_calendar::NativeInterval> input_interval;
        std::int64_t script_ts = std::numeric_limits<std::int64_t>::min();
        std::optional<native_calendar::NativeInterval> script_interval;
        bool input_held = false;
        bool script_held = false;
        Prior input_prior;
        Prior script_prior;
        void clear() noexcept {
            input_ts = std::numeric_limits<std::int64_t>::min();
            input_interval.reset();
            script_ts = std::numeric_limits<std::int64_t>::min();
            script_interval.reset();
            input_held = false;
            script_held = false;
            forget_priors();
        }
        void forget_priors() noexcept {
            input_prior.held = false;
            script_prior.held = false;
        }
    };

    bool failed() const noexcept;
    bool recoverable_abort() const noexcept;
    void latch_failure(NativeFailure failure) noexcept;
    void fail(BacktestEngine& engine, NativeFailure failure) noexcept;
    void render(BacktestEngine& engine, const char* text) const;
    // The running spec while the run runs (cache_running_policy), else the
    // lifecycle probe: inline, so the per-point reads of the spec below cost
    // a load, not a call.
    const NativeRunSpec* spec_ptr() const {
        return running_spec_ ? running_spec_ : lifecycle_spec();
    }
    const NativeRunSpec* lifecycle_spec() const;
    // The run's declared price tick: the ladder a trailing stop spelled a
    // whole number of ticks away names (R5 lane E16). Zero with no spec.
    double ladder_tick() const;
    bool commands_allowed() const;
    bool timeframe_args_ok(const std::string& input_tf, const std::string& script_tf) const;
    // The label policy: the running run's cached answers inline, the spec's
    // otherwise (cache_running_policy).
    bool has_undetected_timeframe() const noexcept {
        return running_spec_ ? running_undetected_ : spec_undetected_timeframe();
    }
    bool legacy_tolerant_slot_labels() const noexcept {
        return running_spec_ ? running_tolerant_labels_ : spec_tolerant_slot_labels();
    }
    bool uses_raw_label_partition() const noexcept {
        return running_spec_ ? running_raw_labels_ : spec_raw_label_partition();
    }
    bool spec_undetected_timeframe() const noexcept;
    bool spec_tolerant_slot_labels() const noexcept;
    bool spec_raw_label_partition() const noexcept;
    static native_calendar::NativeInterval timestamp_partition(std::int64_t timestamp) noexcept {
        // No duration is available in this state. Each boundary is the
        // current bar timestamp, so no inferred aggregation or clock grid is
        // introduced.
        return {timestamp, timestamp, timestamp, timestamp, timestamp};
    }
    // The interval of an instant: the slot's answer when it is for this
    // instant, a raw partition, else a resolution -- inline up to the last.
    std::optional<native_calendar::NativeInterval> input_interval_at(std::int64_t timestamp) const {
        if (interval_cache_.input_ts == timestamp) return interval_cache_.input_interval;
        if (uses_raw_label_partition()) return timestamp_partition(timestamp);
        return input_interval_resolved(timestamp);
    }
    std::optional<native_calendar::NativeInterval> script_interval_at(std::int64_t timestamp) const {
        if (interval_cache_.script_ts == timestamp) return interval_cache_.script_interval;
        if (uses_raw_label_partition()) return timestamp_partition(timestamp);
        return script_interval_resolved(timestamp);
    }
    std::optional<native_calendar::NativeInterval> input_interval_resolved(
        std::int64_t timestamp) const;
    std::optional<native_calendar::NativeInterval> script_interval_resolved(
        std::int64_t timestamp) const;
    bool validate_undetected_begin(BacktestEngine& engine, const NativeBeginArgs& args);
    bool apply_spec(BacktestEngine& engine, const NativeRunSpec& spec);
    bool projection_ok(const BacktestEngine& engine) const;
    // The string equality projection_ok compares with (the .cpp's same_text),
    // for tests/test_native_projection_compare.cpp to hold against
    // std::string's own.
    static bool same_bytes(const std::string& a, const std::string& b) noexcept;
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
    bool pump_timeframe_subscriptions(BacktestEngine& engine, const Bar& bar, int index,
                                      std::int64_t input_period_end_ms);
    // The declared auxiliary feed followed by a stream's appended bars, read
    // as one sequence. Zero / never called for a run that declares no feed.
    std::size_t auxiliary_bar_count() const noexcept;
    const Bar& auxiliary_bar(std::size_t at) const noexcept;
    // Fold every feed bar that opened before `input_period_end_ms` into one
    // AuxiliaryFeed series and hand each completed bucket to `completed`.
    template <typename Completed>
    bool feed_auxiliary_slice(BacktestEngine& engine, TimeframeSubscription& subscription,
                              int index, std::int64_t input_period_end_ms,
                              Completed&& completed);
    // A declared series is fed by accepted CONFIRMED input only, because that
    // is the only input a batch of the same bars also has. True (refused)
    // exactly when a stream that declares one is asked for tick-driven input.
    bool refuse_subscription_tick_input(BacktestEngine& engine);
    // FP6: a declared FX curve converts on the clock confirmed input moves.
    // True (refused) exactly when a stream that declares one is asked for
    // tick-driven input, whose hooks and partial slots read a stale clock.
    bool refuse_fx_curve_tick_input(BacktestEngine& engine);
    bool deliver_timeframe_bar(BacktestEngine& engine, TimeframeSubscription& subscription,
                               const Bar& bucket, std::int64_t first_contributing_ms,
                               std::int64_t delivered_at_ms, NativeCompletionKind completion);
    // The lazy seal: a still-open script bucket keyed to an interval other
    // than `script_key` is sealed LazyComplete and reset. True when nothing
    // was to seal or the sealed calculation succeeded.
    bool seal_stale_script(BacktestEngine& engine, std::int64_t script_key);
    void seal_script(BacktestEngine& engine, NativeCompletionKind kind);
    void deliver_confirmed_script(BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base);
    void deliver_intrabar_script(BacktestEngine& engine, const Bar& bar,
                                 const NativeCoordinate& base);
    void deliver_aggregate_calculation(BacktestEngine& engine, const Bar& bar,
                                       const NativeCoordinate& base);
    // Session-day facts (NativeDecisionContext, R5 lane F5) of the script bar
    // opening at `label`. When the script_ bucket is that bar, its input
    // indices place it in the run, and inside pump_batch its held neighbours
    // are the pumped inputs around it; everything else reads its neighbours
    // off the calendar, one script width away.
    void present_session_day(NativeDecisionContext& context, int64_t label) const;
    struct SessionPoint {
        bool in_session = false;
        std::optional<int64_t> ordinal;
    };
    // Through the memo of the last two instants answered (session_points_):
    // a bar's label is asked again as the next bar's "before", and the label
    // after it is the next bar's own (R5 lane PERF-L1).
    SessionPoint session_point(int64_t ms) const {
        for (const auto& entry : session_points_) {
            if (entry.held && entry.ms == ms) return entry.point;
        }
        return session_point_resolved(ms);
    }
    SessionPoint session_point_resolved(int64_t ms) const;
    struct SessionPointMemo {
        bool held = false;
        int64_t ms = 0;
        SessionPoint point;
    };
    std::optional<int64_t> pumped_script_label(int index) const;
    int64_t script_width_ms() const noexcept;
    int64_t calculation_time(const NativeCoordinate& base) const noexcept;
    // Report truth at the kernel's own cadence, one per script calculation.
    // Reporting-only throughout: these mark equity and synthesize report
    // rows, and never book cash, place an order or move the broker book. The
    // split inside record_script_report_point is deliberate — the equity and
    // position extremes are folded under every report policy that has not
    // handed the mark cadence to the host, because they are a property of the
    // run; appending the curve point and the open-position rows is
    // NativeReportPolicy::KernelRecorded's alone.
    void record_script_report_point(BacktestEngine& engine, int64_t script_open_ms) const;
    void record_report_point(BacktestEngine& engine, int64_t report_ts) const;
    void record_open_position_report_rows(BacktestEngine& engine) const;
    void match_point(BacktestEngine& engine, const NativeDriverPoint& point);
    void match_discrete(BacktestEngine& engine, const NativeDriverPoint& point);
    void match_segment(BacktestEngine& engine, const NativeDriverPoint& dest, double from_price);
    // match_path's no-live-request branch, taken in front of the call (R5 lane
    // D2-A): a point with nothing live has no trigger, allowance, trail,
    // receipt or callback work, so matching it is opening its epoch and, on a
    // segment, the one excursion the path applies to an open position. Every
    // test match_path makes before that branch is made here first -- a failed
    // run, a CurrentExecution point, no spec, an FX-roll check that could act
    // -- and any one of them hands the point to match_path whole. Answers
    // whether it matched the point.
    bool match_quiet_point(BacktestEngine& engine, const NativeDriverPoint& point,
                           bool continuous, double to_price) {
        if (!quiet_point_match_ || !requests_.live().empty()
            || point.coordinate.provenance == NativePriceProvenance::CurrentExecution
            || (staged_fx_curve_ && margin_model() != nullptr) || failed()
            || spec_ptr() == nullptr) {
            return false;
        }
        ++quiet_points_;
        open_point_epoch();
        if (continuous) apply_excursion(engine, to_price);
        return true;
    }
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
                              const NativeDriverPoint& point) {
        if (calculation_trigger() == NativeCalculationTrigger::EveryModeledPoint
            || !point_guards_)
            recalculate_at_modeled_point(engine, bar, point);
    }
    void recalculate_at_modeled_point(BacktestEngine& engine, const Bar& bar,
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
    NativeCalculationTrigger calculation_trigger() const noexcept {
        const auto* spec = spec_ptr();
        return spec ? spec->calculation : NativeCalculationTrigger::BarClose;
    }
    bool recalculates_on_fills() const noexcept {
        const auto trigger = calculation_trigger();
        return trigger == NativeCalculationTrigger::BarCloseAndFills
            || trigger == NativeCalculationTrigger::EveryModeledPoint;
    }
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
    uint64_t take_ordinal(BacktestEngine&) {
        const uint64_t ordinal = next_timeline_ordinal_;
        if (ordinal == 0 || ordinal == std::numeric_limits<uint64_t>::max()) exhaust_ordinals();
        ++next_timeline_ordinal_;
        return ordinal;
    }
    [[noreturn]] static void exhaust_ordinals();
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
    void read_target_into(const BacktestEngine& engine, const native_order::LiveRequest* live,
                          native_order::TargetObservation& out,
                          std::vector<native_order::RequestHandle>& handles) const;
    void read_openings_into(const BacktestEngine& engine,
                            const std::vector<native_order::RequestHandle>& handles,
                            int64_t cycle,
                            std::vector<native_order::OpeningObservation>& out) const;
    std::optional<native_order::Side> scratch_cohort_side(
        const BacktestEngine& engine, const native_order::LiveRequest& live);
    bool scratch_request_is_buy(const BacktestEngine& engine,
                                const native_order::LiveRequest& live);
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
    bool admit_opening_inspect(const BacktestEngine& engine, double resolved_price, double fx,
                               const execution::SettlementInspection& inspect,
                               bool skip_initial_margin,
                               native_order::MatchRejectReason* reason) const;
    // L4 generic margin model. Every one of these is inert for a spec that
    // leaves `margin` unset, which is every source-projected spec.
    const NativeMarginModel* margin_model() const noexcept {
        const auto* spec = spec_ptr();
        return spec && spec->margin ? &*spec->margin : nullptr;
    }
    std::optional<double> maintenance_fraction(bool short_side) const noexcept;
    // E3: the account-currency rate ONE check point converts at -- the one the
    // declared curve has in force at that point's own cursor. The engine's own
    // accessor converts at the presented bar clock, which is a later instant
    // whenever the walk has moved past the point being checked.
    double margin_check_fx(const BacktestEngine& engine,
                           const native_order::MatchCursor& cursor) const noexcept;
    // Solve equity(P) == maintenance requirement(P) for the live book, at the
    // check point's own rate.
    std::optional<double> liquidation_level(const BacktestEngine& engine, double fx) const;
    // The equity one maintenance test is made against, on the model's basis
    // and at the check point's own rate.
    double margin_equity(const BacktestEngine& engine, double mark, double fx) const;
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
    // The three check points below return on their first test for a run
    // with no margin model (and the FX roll, with no staged curve), so each
    // one's guard makes that test before the call (R5 lane PERF-L1).
    void maintain_margin_liquidation(BacktestEngine& engine,
                                     const native_order::MatchCursor& cursor,
                                     NativePathPhase phase, double fallback_price,
                                     NativeMarginCheckKind kind) {
        if (margin_model() != nullptr || !point_guards_)
            maintain_margin_at(engine, cursor, phase, fallback_price, kind);
    }
    void maintain_margin_at(BacktestEngine& engine, const native_order::MatchCursor& cursor,
                            NativePathPhase phase, double fallback_price,
                            NativeMarginCheckKind kind);
    void calculation_margin_check(BacktestEngine& engine, const NativeCoordinate& calc,
                                  double mark) {
        const auto* margin = margin_model();
        if ((margin != nullptr && margin->check == NativeLiquidationCheck::CalculationOnly)
            || !point_guards_)
            calculation_margin_check_at(engine, calc, mark);
    }
    void calculation_margin_check_at(BacktestEngine& engine, const NativeCoordinate& calc,
                                     double mark);
    // MG9: a step of the declared FX curve is a check point of its own. Run
    // at the head of every matched driver point; inert without a staged curve
    // and a margin model, so no other run reaches past its first test.
    void fx_roll_margin_check(BacktestEngine& engine, const NativeDriverPoint& point,
                              bool continuous, double from_price) {
        if ((staged_fx_curve_ && margin_model() != nullptr) || !point_guards_)
            fx_roll_margin_check_at(engine, point, continuous, from_price);
    }
    void fx_roll_margin_check_at(BacktestEngine& engine, const NativeDriverPoint& point,
                                 bool continuous, double from_price);
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
    const NativeRiskLimits* risk_limits() const noexcept {
        const auto* spec = spec_ptr();
        return spec && spec->risk ? &*spec->risk : nullptr;
    }
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
    // One request-core mutation as a call site received it (R5 lane L3):
    // applied by a direct form (Installed), or prepared to be installed here
    // (a token, with set_direct_mutation(false)), or neither. core_step
    // takes either answer; settle_step finishes it the way install_mutation
    // does -- a token is installed, an applied range is noted -- and answers
    // false when the run failed.
    using CoreStep = std::variant<native_order::Installed, native_order::PreparedMutation,
                                  native_order::NoChange, native_order::PreparationError>;
    static CoreStep core_step(native_order::Preparation<native_order::PreparedMutation>&& prepared);
    static CoreStep core_step(native_order::Preparation<native_order::Installed>&& applied);
    bool settle_step(BacktestEngine& engine, CoreStep&& step, NativeFailureOperation operation,
                     uint64_t ordinal);
    // What every install does once the core has committed: the committed
    // events noted, the cohort target cache cleared, the timeline caught up.
    void note_install(const native_order::EventRange& events) noexcept;
    bool install_execution(BacktestEngine& engine, native_order::PreparedExecution&& prepared,
                           const native_order::CommittedExecutionFacts& facts,
                           uint64_t ordinal);
    // install_execution for the core's direct form (R5 lane L3): the second
    // half of check_execution, after the settlement, failing the run exactly
    // as an install that fails does.
    bool apply_execution(BacktestEngine& engine, const native_order::RequestHandle& target,
                         const native_order::ExecutionProposal& proposal,
                         const native_order::CommittedExecutionFacts& facts, uint64_t ordinal);
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
            const std::vector<std::pair<native_order::EventId, native_order::RequestHandle>>& seeds,
            NativeFailureOperation operation);
    void observe_trails(BacktestEngine& engine, const NativeDriverPoint& point,
                        const native_order::MatchCursor& cursor, bool continuous,
                        double price);
    void record_driver(const NativeDriverPoint& point);
    // V19-B: the run's event retention (Window without a spec), and the
    // retirement itself: the acknowledged -- or, for a host that never
    // acknowledged, every -- journal event at or below the ordinal of the
    // oldest applied notification still queued, the kernel's own reader.
    NativeEventRetention retention() const noexcept;
    void retire_journal() noexcept;
    NativeCoordinate coordinate_from(const native_calendar::NativeInterval& interval,
                                     int index, int64_t effective,
                                     NativePriceProvenance provenance,
                                     NativePathPhase phase) const;
    bool preflight_ticks(BacktestEngine& engine, const TradeTick* ticks, int n);
    bool deliver_tick(BacktestEngine& engine, const TradeTick& tick);
    // The cooperative abort alone: check_abort_or_projection's first half, for
    // a boundary no host code has run since the last full check. Both answer
    // a boundary that passes inline and leave the failure to latch_abort and
    // latch_projection_mismatch (R5 lane PERF-L1).
    bool check_abort(BacktestEngine& engine, NativeFailureOperation operation,
                     uint64_t ordinal = 0) {
        if (failed()) return false;
        if (!engine.abort_requested_.load(std::memory_order_relaxed)) return true;
        latch_abort(engine, operation, ordinal);
        return false;
    }
    // The abort, then the projected engine fields against the applied spec
    // (projection_ok). Inside a pump (PumpScope) the second half is deferred to
    // the pump's own boundaries, so a callback or policy-hook boundary checks the
    // abort alone (v19, lane V19-C).
    bool check_abort_or_projection(BacktestEngine& engine, NativeFailureOperation operation,
                                   uint64_t ordinal = 0) {
        if (!check_abort(engine, operation, ordinal)) return false;
        return projection_deferred_ || check_projection(engine, operation, ordinal);
    }
    // The projection half alone, never deferred: an in-callback execution's own
    // precondition (execute_current).
    bool check_projection(BacktestEngine& engine, NativeFailureOperation operation,
                          uint64_t ordinal = 0) {
        if (projection_ok(engine)) return true;
        latch_projection_mismatch(engine, operation, ordinal);
        return false;
    }
    void latch_abort(BacktestEngine& engine, NativeFailureOperation operation, uint64_t ordinal);
    void latch_projection_mismatch(BacktestEngine& engine, NativeFailureOperation operation,
                                   uint64_t ordinal);
    // One pump: a batch's whole input loop, or one public stream input. While
    // one is open, check_abort_or_projection defers its projection compare to
    // the pump's boundaries -- before its first input and after its last -- and
    // a host that writes a projected field inside the pump fails there, with
    // the pump's operation and ordinal 0. Derived call-stack state, never folded.
    class PumpScope {
    public:
        explicit PumpScope(bool& deferred) noexcept : deferred_(deferred), prior_(deferred) {
            deferred_ = true;
        }
        ~PumpScope() { deferred_ = prior_; }
        PumpScope(const PumpScope&) = delete;
        PumpScope& operator=(const PumpScope&) = delete;

    private:
        bool& deferred_;
        bool prior_;
    };
    bool projection_deferred_ = false;
    void present_refusal(BacktestEngine& engine, const char* text);
    bool finalize_elapsed_slots(BacktestEngine& engine, int64_t exclusive_end_ms);
    bool emit_quiet_carried_open(BacktestEngine& engine,
                                 const native_calendar::NativeInterval& interval);
    bool finalize_observed_tick_slot(BacktestEngine& engine,
                                     const native_calendar::NativeInterval& interval,
                                     NativeCompletionKind kind);
    bool pre_open_birth_eligible(const native_order::RequestHandle&,
                                 const NativeDriverPoint&) const noexcept;
    void record_pre_open_birth(const native_order::Request&, const native_order::RequestHandle&);
    void note_committed_events(const native_order::EventRange& events) noexcept;
    void note_cohort_receipts() noexcept;
    void verify_closed_rows(const BacktestEngine& engine) const noexcept;
    void rewind_closed_rows(std::size_t first) const noexcept;
    uint64_t spec_digest(const NativeRunSpec& spec, uint64_t run_base) const noexcept;
    void forget_spec_digests() const noexcept;

    NativeLifecycle state_{NativeUnconfigured{}};
    // native_host()'s answer for the engine the last public begin named.
    // Derived, never folded.
    const BacktestEngine* downcast_engine_ = nullptr;
    NativeStrategyHost* downcast_host_ = nullptr;
    // The running spec and the label policy the per-bar interval lookups
    // read, taken once per begin_ready (cache_running_policy). Non-null
    // exactly while state_ holds NativeRunning: that alternative's spec lives
    // in place in state_, no in-run path writes the fields the policy is
    // derived from (the begin-time declarations rewrite `subscriptions` and
    // `auxiliary_feed` alone, and pairing_ is only written before Running),
    // and every other write of state_ is preceded by leave_running(). While
    // it is null, spec_ptr() and the three predicates probe as they always
    // did. Derived, never folded.
    const NativeRunSpec* running_spec_ = nullptr;
    bool running_undetected_ = false;
    bool running_tolerant_labels_ = false;
    bool running_raw_labels_ = false;
    void cache_running_policy() noexcept;
    void leave_running() noexcept { running_spec_ = nullptr; }
    // The caches beside the lookups they replace, compared bit for bit by
    // tests/test_native_callback_caches.cpp.
    friend struct NativeExecutionConsumerProbe;
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
    // Where this consumer's own evaluator states start in
    // BacktestEngine::security_eval_states_. Zero for every host that
    // registers none of its own, which is the whole established population.
    std::size_t subscription_states_base_ = 0;
    // The timeframe literals this consumer installed authoritative bars for.
    // Only these are removed at the next begin: a feed installed through the
    // engine's own public setter (the C ABI writes that same store) is host
    // ingress and survives.
    std::vector<std::string> subscription_feed_tfs_{};
    // The declared auxiliary feed's parsed timeframe, and the bars a realtime
    // stream appended to it with their running content digest. All three are
    // empty for every run that declares no feed.
    std::optional<native_calendar::Timeframe> auxiliary_tf_{};
    std::vector<Bar> auxiliary_appended_{};
    std::uint64_t auxiliary_appended_digest_ = 0;
    // True only inside the kernel's own registration of the declared series,
    // which runs after the run is Running and calls no host callback.
    bool wiring_subscriptions_ = false;
    // True only inside on_native_run_begin, where
    // declare_timeframe_subscriptions() is legal.
    bool in_run_begin_ = false;
    // Borrowed begin arguments, valid only inside one public begin call.
    const Bar* begin_bars_ = nullptr;
    int begin_n_ = 0;
    bool begin_is_stream_ = false;
    // The input array pump_batch is consuming, borrowed for that call alone:
    // the bars a script bar's session-day facts read before and after it
    // (present_session_day). Input, not state -- like input_next_ms_ below.
    const Bar* pump_bars_ = nullptr;
    int pump_n_ = 0;
    // The last script bar present_session_day read off the pumped array: its
    // final input's index and its label, which is the next bar's "before".
    mutable int pumped_last_index_ = -1;
    mutable int64_t pumped_last_label_ = 0;
    // The session day the facts were last read on: a memo over the run's
    // calendar, rebuilt whenever calendar_ is, so one session day is resolved
    // once rather than once per bar. Derived, never folded.
    mutable std::optional<native_calendar::NativeSessionDay> session_day_memo_;
    // Every session day read off calendar_, for every interval and in-session
    // lookup the consumer makes (native_calendar_memo.hpp): the few days a
    // run's bars fall in are resolved once, not once per lookup. A memo over
    // calendar_ alone, rebuilt whenever calendar_ is. Derived, never folded.
    mutable native_calendar::SessionDayMemo calendar_memo_;
    // The last two session points answered (session_point). Derived, never
    // folded; a point whose resolution threw is never held.
    mutable std::array<SessionPointMemo, 2> session_points_{};
    mutable std::size_t session_point_next_ = 0;
    // Forget every memo over the calendar and the lookups it keys: called
    // wherever calendar_ is rebuilt.
    void reset_calendar_memos() const noexcept {
        session_day_memo_.reset();
        calendar_memo_.reset();
        session_points_ = {};
        session_point_next_ = 0;
        interval_cache_.forget_priors();
    }
    Bar forming_{};
    bool has_forming_ = false;
    double last_price_ = 0.0;
    bool has_last_price_ = false;
    int64_t last_print_time_ms_ = 0;
    std::vector<NativeDriverPoint> driver_log_;
    std::vector<NativeAccountObservation> account_log_;
    // V19-B: the newest driver point and account observation the run
    // produced, retained or not -- the stream's high water, which
    // event_high_water() answers under every retention -- and the instants
    // of the last two driver points, the only driver facts a decision reads
    // (fx_roll_margin_check's previous point). The high waters are readback
    // state and fold nowhere; the two instants fold with the margin state
    // they serve.
    struct DriverMark {
        uint64_t ordinal = 0;
        int64_t effective_time_ms = 0;
    };
    uint64_t last_driver_ordinal_ = 0;
    uint64_t last_account_ordinal_ = 0;
    std::array<DriverMark, 2> driver_marks_{};
    std::size_t driver_mark_count_ = 0;
    std::uint64_t applied_events_ = 0;
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
    // V19-B: whether the host acknowledged at least once this run, and the
    // newest ordinal it acknowledged. Reader bookkeeping for the journal
    // window -- it decides which retained events a later boundary retires and
    // nothing else -- so, like the receipt watermark above, it is folded into
    // nothing. Reset at begin.
    bool events_acknowledged_ = false;
    uint64_t acknowledged_through_ = 0;
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
    // R5 lane F3: the spec's bar-array digests, taken once per staged spec.
    // A pure cache of values the continuation already folds -- configure and
    // the two run-begin declarations that rewrite an array clear it -- so a
    // continuation read at every report point is not linear in the feed.
    mutable std::optional<SpecBarDigests> spec_bar_digests_;
    const SpecBarDigests& spec_bar_digests(const NativeRunSpec& spec) const noexcept;
    // v19: the spec as the continuation folds it, one word taken once per
    // applied spec and run generation (spec_digest). The same pure cache
    // discipline as spec_bar_digests_, cleared with it (forget_spec_digests).
    mutable std::optional<uint64_t> spec_digest_;
    mutable uint64_t spec_digest_run_base_ = 0;
    // v19 running digests of the order core's append-only rows, folded once
    // each at the commit that adds the row (note_committed_events,
    // note_cohort_receipts) and folded into the continuation as (count, h):
    // every committed event's compact record, the group-effect receipts and
    // the cohort receipts. Reset with the request core in begin_ready.
    AppendDigest event_records_{};
    AppendDigest group_receipts_{};
    AppendDigest cohort_receipts_{};
    // v19 closed rows (closed_rows_digest): the running digest over
    // trades_[0, closed_rows_digested_), and the mark below which rows are
    // final -- every row an execution booked whose applied notification has
    // returned. Derived from the engine's rows; reset with them in begin_ready.
    mutable std::uint64_t closed_rows_digest_ = 1469598103934665603ULL;
    mutable std::size_t closed_rows_digested_ = 0;
    std::size_t closed_rows_final_ = 0;
    // The digest after each digested row, so an amendment (closed_rows_amended)
    // rewinds to the row before it in O(1): one word per closed row.
    mutable std::vector<std::uint64_t> closed_row_prefix_;
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
    // match_path's candidate rows at its current cursor. Every call starts
    // from an empty buffer, so between calls this is capacity only: the scan
    // stays allocation-free once it has seen its widest book. Scratch, never
    // run state, and folded into nothing. Declared last, with the switch
    // below, so no member the consumer reads at every point changes offset.
    MatchRows match_rows_;
    // Whether match_path reuses those rows after a refresh
    // (set_match_row_reuse). A choice between two computations of the same
    // values, so it is not run state either.
    bool match_row_reuse_ = true;
    // match_path's other per-call scratch (R5 lane PERF-L5): the pointers a
    // scan of more than eight live requests reads the book through, the
    // provenance of the rows it built, and the (incarnation, kind) keys it
    // skips at the current cursor, kept sorted. Each is emptied where the
    // local it replaces was born, so between calls it is capacity only and a
    // warm matcher allocates nothing per point. match_path never re-enters
    // itself: it runs only inside the driver's delivery of an input, and a
    // host callback cannot deliver one. Scratch, never run state, and folded
    // into nothing.
    std::vector<const native_order::LiveRequest*> match_snapshot_;
    std::vector<MatchProvenance> match_provenance_;
    std::vector<std::pair<std::uint64_t, std::uint8_t>> match_skipped_;
    // observe_trails' copy of the book's handles, the same kind of scratch:
    // match_path is its only caller.
    std::vector<native_order::RequestHandle> match_trail_handles_;
    // The target a matcher read builds when the cohort cache does not answer
    // it (read_target_into), and the cohort's lot handles that read collects:
    // one read at a time, each consumed before the next.
    native_order::TargetObservation match_target_;
    std::vector<native_order::RequestHandle> match_target_handles_;
    // Whether match_path tests a priced trigger against the path before the
    // rest of its row (set_match_band_precheck). A choice between two
    // computations of the same values, so it is not run state either.
    bool match_band_precheck_ = true;
    // deliver_intrabar_script's sub-bars and samples (R5 lane PERF-L1).
    // Scratch like match_rows_: cleared at every script bar, capacity only
    // between bars, never read across one, folded into nothing.
    std::vector<const Bar*> intrabar_sub_bars_;
    std::vector<double> intrabar_samples_;
    // set_point_guards. Like match_row_reuse_, a choice between two
    // computations of the same values, not run state.
    bool point_guards_ = true;
    // The host's parked lookup cache (adopt_host_cache) and its switch.
    // Never read by the consumer, dropped at every run begin.
    std::unique_ptr<NativeHostCache> host_cache_;
    bool host_cache_enabled_ = true;
    // set_retire_every_point's switch; kRetireEveryPointDefault above.
    bool retire_every_point_ = kRetireEveryPointDefault;
    // set_retention_override's choice, when a test made one.
    std::optional<NativeEventRetention> retention_override_;
    // V19-B: the chain root of every replace successor, as the core's chain
    // index records it -- durable cohort state the continuation folds once
    // per replace (note_committed_events). Reset with the other digests.
    AppendDigest chain_roots_{};
    // set_direct_mutation's switch. Like match_row_reuse_, a choice between
    // two computations of the same values, not run state; declared last so no
    // member the consumer reads at every point changes offset.
    bool direct_mutation_ = true;
    // drain_after_applied's lists (the group recipients, the waiting
    // children, the bound closes), read one after another into this scratch:
    // capacity only between fills, never run state, folded into nothing. A
    // drain runs inside the settlement of one fill, where no host command can
    // start another.
    std::vector<native_order::RequestHandle> drain_handles_;
    // drain_after_applied's seeds for the dependency queue, the same kind of
    // scratch: cleared where the drain starts, read by the queue it hands
    // them to, capacity only between fills.
    std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> drain_seeds_;
    // consume_matched_request's precommit view, written whole for each fill
    // and shown to the host only inside that fill's precommit call (which
    // cannot start another fill), so its closed-row vector keeps capacity
    // instead of being allocated at every closing fill.
    NativePrecommitView precommit_view_;
    // set_quiet_point_match's switch, a choice between two computations of
    // the same values, and the points the short-cut matched this run (reset
    // at every begin): a witness's probe, never read by the run, folded into
    // nothing.
    bool quiet_point_match_ = true;
    uint64_t quiet_points_ = 0;
};

inline NativeExecutionConsumer& as_native_consumer(IExecutionConsumer& consumer) {
    return static_cast<NativeExecutionConsumer&>(consumer);
}

inline const NativeExecutionConsumer& as_native_consumer(const IExecutionConsumer& consumer) {
    return static_cast<const NativeExecutionConsumer&>(consumer);
}

}  // inline namespace engine_script_run_v19
}  // namespace pineforge
