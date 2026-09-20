#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_scheduler.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

// Generated strategies require this switched source-host surface. There is
// intentionally no compatibility execution path behind the capability gate.
#define PINEFORGE_HAS_EXPLICIT_PINE_CAP_V1 1
#define PINEFORGE_HAS_EXPLICIT_PINE_EXECUTION_ADAPTER_V1 1
#define PINEFORGE_HAS_NATIVE_LOWERING_V1 1

namespace pineforge::source {

// Hash domain of the per-site request.security semantics below. Folded into
// the source extension only when a site is registered, so a run without
// request.security hashes exactly as before; bumped whenever the folded
// field set changes.
inline constexpr char kSourceSecurityDomain[] = "pineforge-source-security/v4";

// One projected higher-timeframe bucket of the opt-in historical
// request.security lookahead projection (PineSecurityEvalState below).
struct HistoricalSecurityProjection {
    Bar bar{};
    // The instant the projection is dispatched on: the first retained
    // chart child's timestamp on the single-feed path, that child's first
    // auxiliary bar on the split-feed path (the requested-context
    // evaluator is fed the finer slice there). Keyed by instant, not by
    // feed-call index, so both paths consume one projection per bucket.
    int64_t first_child_ms = 0;
    bool is_complete = false;
};

// Pine's publication semantics for ONE request.security site, kept beside the
// kernel's generic evaluator state of the same sec_id
// (BacktestEngine::SecurityEvalState: aggregator, current bar, feed counts,
// native feed routing). The kernel evaluator knows none of these rules; this
// host composes them around the kernel's aggregation, substitution and
// dispatch primitives (src/source/pine_security_eval.cpp).
struct PineSecurityEvalState {
    // barmerge.lookahead_on: the site reads the in-progress requested bucket.
    // Every input peeks the running aggregate (a partial evaluation), a
    // completion rewrites the slot its first peek opened instead of opening a
    // second one, and generated TA sites take their recompute path on every
    // sub-bar after a bucket's first (security_series_slot_is_new).
    bool lookahead_on = false;
    // barmerge.gaps_on: the site reads na on every input that completes no
    // requested bucket (clear_security on the generated series).
    bool gaps_on = false;
    // Plain ``request.security`` (not ``_lower_tf``) with a requested TF
    // STRICTLY FINER than script_tf (e.g. so2TF="5" read from a 15m
    // chart) under ``lookahead=barmerge.lookahead_ON``: the security's
    // own aggregator completes multiple times
    // (script_seconds / requested_seconds) per calling/script bar. A
    // history-offset read (``expr[1]`` inside the security call, see
    // the ``*_hist`` push/read machinery in codegen) is meant to expose
    // "the value already confirmed as of the close of the PREVIOUS
    // calling bar" — TV's lookahead_on merge takes the FIRST intrabar
    // of each calling bar, so the publish granularity is the CALLING
    // bar, not the security's own (finer) period. Without this, the
    // read-before-push ``hist[0]`` gets refreshed on every one of the
    // R completions inside the current calling bar, so by the time
    // on_bar() reads it the value has silently drifted to "one
    // security-period behind the LAST completion of THIS SAME calling
    // bar" (e.g. the middle of 3 sub-periods) instead of "the last
    // completion of the PREVIOUS calling bar" — an aliasing bug
    // confirmed against TradingView-exported trades on a triple-RSI
    // DCA strategy using so2Rsi = request.security(sym, "5",
    // ta.rsi(close,7)[1], lookahead=barmerge.lookahead_on) on a 15m
    // chart (finer target under lookahead + offset).
    //
    // ``lookahead_OFF`` is deliberately NOT gated (field stays 0): TV's
    // lookahead_off merge takes the LAST intrabar of the calling bar,
    // so the exposed value — and any ``[k]`` history offset off it —
    // advances at the security's own finer cadence (one hist.push per
    // completed security period), which is exactly the ungated
    // behavior. Gating lookahead_off regressed
    // masayanfx-multi-time-score-strategy
    // (request.security(sym, "5", ta.highest(high, 20)[1],
    // barmerge.gaps_off, barmerge.lookahead_off) on a 15m chart) from
    // 100.0% to 93.7% trade parity vs TradingView.
    //
    // When nonzero, this holds the requested TF's duration in seconds
    // (script_seconds % this == 0 verified at validate time) and gates
    // ``pine_feed_security_eval_state``'s aggregator branch: only the
    // completion whose bucket END aligns to a script_tf boundary is
    // passed through to ``evaluate_security`` as ``is_complete = true``
    // (letting codegen's ``hist.push()`` fire); all other completions
    // within the same calling bar are still evaluated (so the
    // underlying TA state keeps advancing at native/security
    // resolution) but are passed ``is_complete = false`` so they do not
    // advance the exposed history buffer. Zero (the default) means "not
    // applicable" (target TF coarser than or equal to script_tf, or
    // lookahead_off — the already-correct cases) and leaves behavior
    // unchanged.
    int publish_gate_tf_seconds = 0;
    // Plain ``request.security`` with a requested TF strictly finer than
    // script_tf, served by the auxiliary finer feed (the split-feed
    // path), under ``lookahead_off``: TradingView surfaces the LAST
    // intrabar of the calling chart bar at that bar's close whatever
    // the bucket's sub-bar count -- on the OANDA:XAUUSD 1D chart the
    // Thanksgiving 2025-11-26 bar's last 3m bucket (21:57Z, holding
    // the 21:59Z minute alone before the 22:00Z session close) is the
    // value ``request.security(tickerid, "3", ta.rsi(close, 14))``
    // reads at the daily close (72.64, lab tv dca-ltf-last-intrabar,
    // 2026-09-05), where the aggregator's count / real-end /
    // session-close rules leave that bucket partial until the next
    // chart bar's first sub-bar and the close read the 21:54Z bucket
    // (38.87). When set, pine_feed_security_eval_state
    // finalizes and publishes the pending partial bucket on the
    // calling bar's last auxiliary bar (TimeframeAggregator::
    // complete_pending_partial), once: the next chart bar's first
    // sub-bar resets the bucket without re-emitting it. Dense feeds
    // whose final bucket completes on its count are untouched (no
    // partial is pending), and so are lanes without the auxiliary
    // slice, lookahead_on (its gated publication is untouched: on this
    // shape it stays one bucket behind, as before -- the tape pins
    // lookahead_off only), lower-TF arrays and calendar / same-TF
    // requests. False (the default) means "not applicable".
    bool calling_close_completes_partial = false;
    // Heikin-Ashi same-symbol read: request.security(ticker.heikinashi(
    // syminfo.tickerid), ...). When set, the completed (aggregated) bar's
    // OHLC is replaced by its Heikin-Ashi candle before the security
    // expression is evaluated, so close/open/high/low inside the call see
    // HA values. HA is stateful (ha_open depends on the prior HA bar), so
    // the running state lives here per sec_id.
    bool heikinashi = false;
    double ha_prev_open = 0.0;
    double ha_prev_close = 0.0;
    bool ha_seeded = false;
    // One entry per projected HTF bucket, populated only for an explicitly
    // opted-in finite historical batch. Empty for every default/streaming
    // run and for sites outside the narrow HTF lookahead_on+gaps_off
    // contract. The feed index advances once per retained input bar (bars
    // before an opt-in security range start are dropped by both producer
    // and consumer); the projection cursor advances only at the next
    // bucket's first child.
    std::vector<HistoricalSecurityProjection> historical_projections;
    std::size_t historical_projection_cursor = 0;
    // Which projection (cursor) has already been dispatched: every later
    // input of the same bucket is a no-op for the evaluator.
    bool historical_projection_dispatched = false;
};

class PineStrategyHost : public NativeStrategyHost, public BrokerStateHashProvider {
public:
    explicit PineStrategyHost(
        compat::pine::CapAttachment cap = compat::pine::CapAttachment::None);

    std::uint64_t broker_state_hash_projection() const override;

    void prepare_native_begin(const NativeBeginArgs&) final;
    void on_native_run_begin() final;
    void on_native_input(const Bar&, const NativeInputContext&) final;
    void on_native_tick(const Bar&, const NativeTickContext&) final;
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) final;
    void on_native_bar(const Bar&, const NativeDecisionContext&) final;
    // R6: every calculation of the run arrives here. BarClose forwards to
    // on_native_bar exactly as the kernel's default does; OrderFill is
    // calc_on_order_fills, which the consumer now schedules (spec
    // NativeCalculationTrigger::BarCloseAndFills) and the source layer only
    // executes — language-state rollback, publication, first-open chain.
    void on_native_recalculate(const Bar&, const NativeDecisionContext&,
                               NativeCalculationReason,
                               const native_order::ExecutionAppliedEvent*) final;
    void on_native_applied(const native_order::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) final;
    native_order::ExecutionTerms resolve_execution_terms(
        const NativeExecutionTermsFacts&) const final;
    NativePrecommitVerdict validate_execution_precommit(
        const NativePrecommitView&) const final;
    // R5: the kernel owns the margin mechanism; these three answer with the
    // TradingView policy the adapter holds.
    bool margin_check_allowed(const NativeMarginCheckPoint&) const final;
    std::optional<NativeMarginDecision> resolve_margin_requirement(
        const NativeMarginRequirementView&) const final;
    std::optional<double> resolve_margin_call_units(
        const NativeMarginCallView&) const final;
    // RULING A48: the source host owns per-lot excursion accounting (MFE/MAE)
    // on the switched route. It samples every completed source bar's H/L/C
    // with the owner's entry-bar masks, and supplies the closing row's two
    // magnitudes through the kernel's single generic capability.
    bool owns_lot_excursions() const noexcept final { return true; }
    ClosedLotExcursion closed_lot_excursion(
        const ClosedLotExcursionFacts&) const final;
    // The R5 R2 sizing classification of the adapter, for tests and for hosts
    // that need to know whether the core or the source owns a default
    // quantity at this point.
    bool adapter_core_sizes_default_opening(bool is_long) const;

    virtual void on_source_bar(const Bar&) = 0;
    void configure_pine_strategy(const PineStrategyConfig&);
    void set_strategy_override(const StrategyOverrides&);
    // A stream's historical warmup remains a configuration window until the
    // first realtime input.  The legacy stream path permits the feed to
    // finalize its session template in that window (the sparse-boundary
    // request.security probe relies on it); retain that source-host surface
    // while native staged ingress remains refused once realtime starts.
    void set_syminfo_session(const std::string&);
    void set_pine_risk_direction(int);
    void set_pine_risk_max_cons_loss_days(int);
    void set_pine_risk_max_drawdown(double, bool);
    void set_pine_risk_max_intraday_loss(double, bool);
    void set_pine_risk_max_intraday_filled_orders(int);
    void set_pine_risk_max_position_size(double);

    void strategy_entry(const std::string& id, bool is_long,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& comment = {},
                        const std::string& oca_name = {}, int oca_type = 0,
                        int qty_type = -1);
    void strategy_close(const std::string& id, const std::string& comment = {},
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        double qty_percent = std::numeric_limits<double>::quiet_NaN(),
                        bool immediately = false);
    void strategy_close(const std::string& id, const std::string& comment,
                        double qty, double qty_percent, bool immediately,
                        std::uint64_t callsite_token);
    void strategy_close_all();
    void strategy_exit(const std::string& id, const std::string& from_entry,
                       double limit_price, double stop_price,
                       double trail_points = std::numeric_limits<double>::quiet_NaN(),
                       double trail_offset = std::numeric_limits<double>::quiet_NaN(),
                       double trail_price = std::numeric_limits<double>::quiet_NaN(),
                       double qty_percent = 100.0, const std::string& comment = {},
                       double qty = std::numeric_limits<double>::quiet_NaN(),
                       const std::string& oca_name = {},
                       double profit_ticks = std::numeric_limits<double>::quiet_NaN(),
                       double loss_ticks = std::numeric_limits<double>::quiet_NaN());
    void strategy_exit_cancel_bracket(const std::string& exit_id,
                                      const std::string& from_entry,
                                      const std::string& comment = {});
    void strategy_cancel(const std::string& id);
    void strategy_cancel_all();
    void strategy_order(const std::string& id, bool is_long, double qty,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& oca_name = {}, int oca_type = 0);

    int pine_bar_index() const;
    int pine_last_bar_index() const;
    double prev_chart_close() const;
    bool is_first_tick() const noexcept;
    bool is_last_tick() const noexcept;
    bool history_advances_new_bar() const noexcept;
    bool security_series_slot_is_new(int) const noexcept;
    int last_bar_dual_entry_path() const;
    double live_position_size() const override;
    int pending_order_count() const;
    MarketAdmissionJournal& market_admission_journal();
    const MarketAdmissionJournal& market_admission_journal() const;
    std::vector<admission::Field> market_admission_fields() const;
    int probe_fill_qty(int index, double fill_price, double* qty,
                       int* close_only, int* partition) const;
    int pending_order_level_resolved(int index) const;
    int pending_order_effective_levels(int index, double* stop, double* limit,
                                       double* trail_activation) const;
    const PendingIntentView& pending_intent_view() const noexcept;
    int short_seed_collision_role_v1(native_order::RequestHandle) const noexcept;
    void enable_pine_intraday_cap();
    void attach_pine_execution_adapter();
    void set_syminfo_metadata(const std::string&, double) override;
    bool set_aux_security_feed(const Bar* bars, int n,
                               const std::string& input_tf) override;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    bool source_aux_security_feed_enabled() const override;
    void source_aux_security_input_view(const Bar*&, int&) const override;
#endif
    int observe_last_bar_dual_entry_path_v1() const override;
    int observe_pending_count_v1() const override;
    int observe_pending_copy_v1(int, pf_pending_order_v1_t*) const override;
    int observe_probe_fill_qty(int, double, double*, int*, int*) const override;
    int observe_pending_level_resolved(int) const override;
    int observe_pending_effective_levels(int, double*, double*, double*) const override;
    double observe_trail_best_price_v1() const override;

    // Fixture-only adapter projections retained for the L2 literal twins.
    // They are read-only and never participate in execution or matching.
    enum class FixtureIntentKind { MARKET, EXIT, ENTRY, RAW_ORDER };
    struct FixtureIntentRow {
        std::string id;
        FixtureIntentKind type = FixtureIntentKind::MARKET;
        double default_stop_placement_qty = std::numeric_limits<double>::quiet_NaN();
        double default_stop_sizing_price = std::numeric_limits<double>::quiet_NaN();
        double frozen_market_own_units = std::numeric_limits<double>::quiet_NaN();
        double frozen_market_transaction_units = std::numeric_limits<double>::quiet_NaN();
        // These are immutable adapter placement facts, exposed only to test
        // facades which formerly read the retired source PendingOrder owner.
        std::string from_entry;
        bool is_long = true;
        double qty = std::numeric_limits<double>::quiet_NaN();
        double qty_percent = std::numeric_limits<double>::quiet_NaN();
        std::int64_t created_bar = -1;
        std::int64_t created_seq = 0;
        std::uint64_t incarnation = 0;
        bool over_pyramiding_cap_at_placement = false;
        std::int64_t paired_flat_market_peer_seq = 0;
        double paired_flat_market_transaction_qty = std::numeric_limits<double>::quiet_NaN();
        double frozen_default_qty = std::numeric_limits<double>::quiet_NaN();
        double default_stop_placement_equity = std::numeric_limits<double>::quiet_NaN();
        double default_stop_placement_signal_close = std::numeric_limits<double>::quiet_NaN();
        double affordability_placement_equity = std::numeric_limits<double>::quiet_NaN();
        // The adapter currently has no pair-review receipt for this request;
        // an empty Draft truthfully represents that absence to a fixture.
        MarketAdmissionDraft market_admission{};
    };

public:
    // Toggle TradingView's forced-liquidation (margin call) emulation.
    // Defaults ON to match TV; set false for the hold-the-position behaviour.
    // TV runs the broker margin-call emulator by default.  It is a no-op for
    // the validation corpus (long-only positions at the default 100% margin
    // can never be liquidated — the formula denominator ``margin/100 -
    // direction`` is 0 — and no corpus short is sized at full equity).
    //
    // R5 lane L12 (2.ii k): this used to be BacktestEngine state.  The kernel
    // toggle is the presence of `spec.margin` since L4/L4b, so the TV emulator
    // switch belongs to the adapter that implements that emulator.  The value
    // reaches `PineExecutionAdapter::source_margin_call_enabled_` at the
    // native begin boundary, exactly as before.
    void set_margin_call_enabled(bool enabled) {
        guard_native_mutation("set_margin_call_enabled");
        margin_call_enabled_ = enabled;
    }
    bool margin_call_enabled() const { return margin_call_enabled_; }

protected:
    // Narrow test-facade configuration slots keep the frozen L0 oracle bodies
    // unchanged while routing their setup through the source configuration
    // projected at the native begin boundary.
    PineStrategyConfig& fixture_configuration() noexcept { return config_; }
    const PineStrategyConfig& fixture_configuration() const noexcept { return config_; }
    bool fixture_intraday_cap_latched();
    compat::pine::CapClock fixture_cap_clock() const;
    compat::pine::Calculation fixture_cap_calculation() const;
    BarTime fixture_chart_time(std::int64_t timestamp_ms) const;
    std::int64_t fixture_chart_day_key(std::int64_t timestamp_ms) const noexcept {
        return adapter_.chart_day_key(timestamp_ms);
    }
    std::uint64_t fixture_applied_receipt_count() const;
    bool fixture_cap_due_pending() const noexcept {
        return adapter_.cap.due_cause().has_value();
    }
    class FixtureQtyTypeSlot {
    public:
        explicit FixtureQtyTypeSlot(PineStrategyHost& host) noexcept : host_(host) {}
        FixtureQtyTypeSlot& operator=(QtyType value) noexcept {
            host_.config_.default_qty_type = static_cast<int>(value);
            return *this;
        }
        operator QtyType() const noexcept {
            return static_cast<QtyType>(host_.config_.default_qty_type);
        }
    private:
        PineStrategyHost& host_;
    };
    FixtureQtyTypeSlot fixture_default_qty_type_slot() noexcept {
        return FixtureQtyTypeSlot(*this);
    }
    class FixtureCommissionTypeSlot {
    public:
        explicit FixtureCommissionTypeSlot(PineStrategyHost& host) noexcept : host_(host) {}
        FixtureCommissionTypeSlot& operator=(CommissionType value) noexcept {
            host_.config_.commission_type = static_cast<int>(value);
            return *this;
        }
        operator CommissionType() const noexcept {
            return static_cast<CommissionType>(host_.config_.commission_type);
        }
    private:
        PineStrategyHost& host_;
    };
    FixtureCommissionTypeSlot fixture_commission_type_slot() noexcept {
        return FixtureCommissionTypeSlot(*this);
    }
    class FixtureRiskDirectionSlot {
    public:
        explicit FixtureRiskDirectionSlot(PineStrategyHost& host) noexcept : host_(host) {}
        FixtureRiskDirectionSlot& operator=(int value) noexcept {
            host_.adapter_.set_risk_direction(value);
            return *this;
        }
    private:
        PineStrategyHost& host_;
    };
    FixtureRiskDirectionSlot fixture_risk_direction_slot() noexcept {
        return FixtureRiskDirectionSlot(*this);
    }
    class SourceIdLedgerView {
    public:
        struct value_type { double second = 0.0; };
        class const_iterator {
        public:
            const value_type* operator->() const noexcept { return &value_; }
            bool operator==(const const_iterator& other) const noexcept {
                return present_ == other.present_;
            }
            bool operator!=(const const_iterator& other) const noexcept {
                return !(*this == other);
            }
        private:
            friend class SourceIdLedgerView;
            bool present_ = false;
            value_type value_{};
        };
        const_iterator find(const std::string& id) const noexcept {
            const double units = host_ ? host_->adapter_.source_unclosed_qty_for(id) : 0.0;
            const_iterator result;
            result.present_ = units > 0.0;
            result.value_.second = units;
            return result;
        }
        const_iterator end() const noexcept { return {}; }
    private:
        friend class PineStrategyHost;
        explicit SourceIdLedgerView(const PineStrategyHost* host) noexcept : host_(host) {}
        const PineStrategyHost* host_ = nullptr;
    };
    SourceIdLedgerView source_id_ledger_view() const noexcept {
        return SourceIdLedgerView(this);
    }
    double signed_position_size() const;
    void freeze_script_position_view();
    void clear_script_position_view();
    const Series<double>& source_series(const std::string&) const;
    const Series<double>& source_input_series(const std::string& key,
                                              const Series<double>& fallback) const;
    // Generated input.source() calls retain this established surface spelling.
    const Series<double>& get_input_source(const std::string& key,
                                           const Series<double>& fallback) const {
        return source_input_series(key, fallback);
    }
    // Generated strategy.margin_liquidation_price reads this Pine-specific
    // projection over the inherited native position state.
    double margin_liquidation_price() const;
    void fixture_publish_source_series(const Bar& bar, bool new_history_slot) {
        scheduler_.fixture_publish_source_series(bar, new_history_slot);
    }
    int64_t time_close() const {
        return pine_time_close(current_bar_.timestamp, script_tf_, syminfo_.session,
                               syminfo_.timezone, script_tf_);
    }
    // ab9714be pine_strategy_host.hpp:348-358: generated three-argument
    // session predicates are class-scope calls whose chart timeframe changes
    // the D/W/M meaning.  Keep that Pine policy in the source host; the
    // namespace-level overload remains the raw intraday time-of-day query.
    bool pine_session_ismarket(const std::string& session,
                               const std::string& timezone,
                               std::int64_t bar_ms) const {
        return pineforge::pine_session_ismarket(session, timezone, bar_ms, script_tf_);
    }
    bool pine_session_ispremarket(const std::string& session,
                                  const std::string& timezone,
                                  std::int64_t bar_ms) const {
        return pineforge::pine_session_ispremarket(session, timezone, bar_ms, script_tf_);
    }
    bool pine_session_ispostmarket(const std::string& session,
                                   const std::string& timezone,
                                   std::int64_t bar_ms) const {
        return pineforge::pine_session_ispostmarket(session, timezone, bar_ms, script_tf_);
    }
    const std::vector<FixtureIntentRow>& source_pending_view() const;
    // request.security registration, as generated configure_security_evaluators()
    // spells it. The kernel registers the aggregating evaluator; Pine's
    // publication semantics for the site (barmerge.lookahead, barmerge.gaps,
    // ticker.heikinashi, the lower-timeframe emulation a finer request
    // selects) are this host's, recorded for the sec_id it just registered.
    void register_security_eval(int sec_id, const std::string& requested_tf,
                                const std::string& input_tf, bool lookahead_on,
                                bool gaps_on = false, bool heikinashi = false);
    // ``request.security_lower_tf`` registers the same per-sec_id eval
    // state but with the additional contract that the requested TF must
    // resolve to a finer-than-input TF emulation. This wrapper sets the
    // ``lower_tf_array_requested`` flag so ``validate_security_timeframes``
    // can throw a precise error if the chart's input TF turns out to be
    // <= the requested TF (mirroring TradingView's "lower timeframe
    // required" error for ``request.security_lower_tf``).
    void register_security_lower_tf_eval(int sec_id, const std::string& requested_tf,
                                         const std::string& input_tf);
    // TradingView's request.security / request.security_lower_tf timeframe
    // rules for the registered sites against the run's evaluator input
    // timeframe: the diagnostics a script author reads, the lower-timeframe
    // emulation each array site selects, and the publication gates a
    // finer-than-chart site runs under.
    void validate_security_timeframes(const std::string& input_tf);
    void source_stream_entry_comment(const PyramidEntry&, std::string&) const override;
    void hash_source_extension(BrokerStateHashSink&) const override;

private:
    friend class PineScheduler;
    friend class PineExecutionAdapter;

    StagedConfiguration staged_configuration() const;
    static PineStrategyConfig apply_overrides(PineStrategyConfig,
                                              const StrategyOverrides&);
    static std::uint64_t adapter_event_high_water(const NativeStrategyHost&) noexcept;
    static std::uint64_t adapter_terminal_receipt_high_water(const NativeStrategyHost&) noexcept;
    std::uint64_t adapter_broker_fill_event_sequence() const noexcept {
        return broker_fill_event_seq_;
    }
    void scheduler_prepare_script_run(const std::vector<Bar>&,
                                      bool static_eligible, int expected_script_bars,
                                      bool script_bar_geometry);
    void scheduler_configure_security_evaluators();
    bool scheduler_uses_aux_security_feed() const noexcept;
    void scheduler_prepare_security_sequence(const std::vector<Bar>&);
    void init_security_eval_states_for_run(const std::string& effective_input_tf);
    void prepare_historical_security_lookahead_projections(
        const Bar* input_bars, int n_input, const std::string& effective_input_tf);
    void clear_historical_security_lookahead_projections();
    bool scheduler_feed_security_input(const Bar&, std::int64_t next_input_ms,
                                       bool calling_bar_complete,
                                       bool defer_boundary_gate);
    void scheduler_publish_security_boundary();
    void scheduler_feed_deferred_security_input(const Bar&, std::int64_t next_input_ms);
    void scheduler_feed_aux_security(int chart_index);
    void scheduler_feed_deferred_aux_security(int chart_index);
    void scheduler_finish_security_sequence();
    // Pine's evaluator step for one request.security site and one evaluator
    // input bar: the kernel's aggregation, native-bar substitution and
    // dispatch primitives, composed under TradingView's publication rules
    // (src/source/pine_security_eval.cpp).
    void pine_feed_security_eval_state(SecurityEvalState& state, const Bar& input_bar,
                                       bool calling_bar_complete = false);
    // The site's Pine semantics; a sec_id registered outside this host's
    // register_security_eval (none today) reads as a plain site.
    PineSecurityEvalState& pine_security_state(int sec_id) {
        return pine_security_states_[sec_id];
    }
    // Drops the entries of sec_ids the evaluator registry no longer holds.
    void prune_pine_security_states();
    // KI-55 range-start gate for one evaluator: true when the input bar at
    // `input_ts` belongs to an HTF bucket that opened before the cut --
    // security_range_start_ms_ under the flag, else the run's first chart
    // bar for a coarser-than-chart / chart-timeframe evaluator
    // (security_first_chart_bar_ms_, split-feed runs with the auxiliary feed
    // proving prior trading, or single-feed historical intraday forex/cfd D
    // requests keyed to their session's actual open; false for lower TFs). The
    // progressive feed and the historical lookahead projection builder must
    // agree on this predicate so projected child indexes line up with the
    // per-state feed cursor.
    bool security_input_precedes_range_start(const SecurityEvalState& state,
                                             int64_t input_ts) const;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    // True when the auxiliary request.security feed holds a bar in
    // [from_ms, to_ms): the evidence that an HTF bucket whose nominal open
    // precedes the run's first chart bar was in progress at the range start.
    bool aux_security_traded_between(int64_t from_ms, int64_t to_ms) const;
#endif
    // The calling chart bar's publication boundary for a gated site
    // (publish_gate_tf_seconds): re-publish the completed caller's final
    // requested value before the retained boundary input is fed.
    void publish_security_eval_state_at_calling_boundary(SecurityEvalState& state);
    void scheduler_record_range_end(const Bar&);
    // One report point per published source slot. The kernel records it
    // (NativeReportPolicy::KernelRecordedAtHostMarks); this host owns only
    // the Pine cadence that says where the points fall.
    void scheduler_mark_report_point(std::int64_t script_bar_ts);
    void scheduler_record_broker_hash();
    void capture_script_continuation_hash();
    void scheduler_update_session_state(
        const Bar&, std::optional<std::int64_t> next_script_open_ms);
    void scheduler_set_session_bar_state(bool in_session,
                                         bool intraday_is_last_bar);
    execution::AccountEffectProjection adapter_project_flatten(
        double price, const std::string& id, const std::string& comment,
        std::uint64_t incarnation) const;
    void adapter_label_bracket_trades(
        const native_order::ExecutionAppliedEvent&, bool from_bracket);
    bool adapter_has_open_entry_id(const std::string&) const;
    void scheduler_publish_source_bar(const Bar&, bool first_tick,
                                      bool advance_source_index = true);
    void scheduler_publish_suppressed_tail(const Bar&);
    double compute_liquidation_price() const;
    void project_short_seed_report_rows(const native_order::ExecutionAppliedEvent&);
    bool scheduler_coof_enabled() const noexcept { return config_.calc_on_order_fills; }
    // The range-end row an applied execution may complete. It stays ordered
    // after the fill's calc_on_order_fills recalculation, which can advance
    // the source bar counter, so it runs in whichever of the two callbacks is
    // last for that event.
    void record_applied_range_end();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    void clear_aux_security_chart_ranges();
    void prepare_aux_security_chart_ranges(const Bar*, int, const std::string&);
    std::int64_t aux_security_calling_close_ms() const;
    void feed_aux_security_for_chart_bar(int);
    void feed_deferred_aux_security_for_chart_bar(int);
#endif

protected:
    // @source-state begin
    PineExecutionAdapter adapter_;
    class SourceCloseObligationView {
    public:
        explicit SourceCloseObligationView(const PineExecutionAdapter& adapter) noexcept
            : adapter_(&adapter) {}
        bool pending() const noexcept {
            return adapter_ && adapter_->cap.due_cause().has_value();
        }
    private:
        const PineExecutionAdapter* adapter_ = nullptr;
    };
    // Retained protected spelling for source fixtures/generated code.  The
    // authoritative due request lives in IntradayCap; this read-only facade
    // prevents a second mutable close-obligation owner.
    SourceCloseObligationView position_close_obligation_{adapter_};
    PineStrategyConfig config_{};
    StrategyOverrides override_{};
    PineScheduler scheduler_{};
    // Generated strategies still use this source-series spelling directly.
    // The state remains scheduler-owned and is hashed by PineScheduler.
    bool& _src_series_active_;
    Series<double>& _src_open_;
    Series<double>& _src_high_;
    Series<double>& _src_low_;
    Series<double>& _src_close_;
    Series<double>& _src_volume_;
    Series<double>& _src_hl2_;
    Series<double>& _src_hlc3_;
    Series<double>& _src_ohlc4_;
    Series<double>& _src_hlcc4_;
    const bool& is_last_tick_;
    int source_bar_index_ = -1;
    int source_last_bar_index_ = -1;
    std::uint64_t source_callback_count_ = 0;
    bool source_configuration_captured_ = false;
    bool source_prepare_failed_ = false;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    std::vector<Bar> aux_security_bars_;
    std::string aux_security_input_tf_;
    std::vector<std::size_t> aux_security_chart_begin_;
    std::vector<std::size_t> aux_security_chart_end_;
#endif
    // Per-sec_id request.security semantics (PineSecurityEvalState), keyed and
    // folded in sec_id order. An entry lives exactly as long as the kernel
    // evaluator state registered under the same sec_id: generated
    // configure_security_evaluators() opens with security_eval_states_.clear(),
    // so the first registration into an empty registry starts this table over.
    std::map<int, PineSecurityEvalState> pine_security_states_;
    // @source-state end

    // Provider configuration, not book state: it is staged before a run and
    // projected into the adapter at begin, so it is waived from the durable
    // fold like the other configuration slots.
    bool margin_call_enabled_ = true;

    // Independent opt-in KI-55 HTF warmup parity. When enabled,
    // request.security series aggregate from security_range_start_ms_ instead
    // of the feed start and their embedded ta.ema na-warm per TV built-in
    // semantics. The cut is taken per evaluator on HTF-BUCKET opens, not on
    // input timestamps: an input bar is dropped when the D/W/M (or intraday
    // grid) bucket it belongs to opened before the range start, so the first
    // HTF bar every series sees is a whole bucket that opened at/after the
    // range start (security_input_precedes_range_start). Default OFF;
    // consulted only by pine_feed_security_eval_state, the auxiliary
    // lower-timeframe slice and the historical lookahead projection builder.
    bool security_range_start_na_warmup_ = false;
    int64_t security_range_start_ms_ = 0;
    // The run's first chart bar (0 outside a run). Without the flag above
    // this is the default cut for every coarser-than-chart and chart-
    // timeframe request.security aggregation: TradingView's deep-backtest
    // series of a timeframe hold the bars of that timeframe whose OPEN lies
    // at or after the range start, and the bucket in progress at the range
    // start is absent -- on every lane (round 8, family P: masayanfx
    // multi-time-score; lab tv famp-sense-{es15full,nq15full,f15full,
    // nifty15full,nifty1d,xau1d,xau15,eur15,eth15,btc1d}, 2026-09-05: "D"
    // on CME_MINI:ES1! 15m first reads on the 05-01 20:45Z bar (bucket 0 is
    // the 04-02 trade date; the 04-01 date opened 03-31 22:00Z before the
    // 04-01 00:00Z range start), "240" on 04-04 13:45Z (the 22:00Z bucket
    // dropped), "W" on the 08-29 / 08-28 bar on every 15m and 1D lane (the
    // Mon 03-31 week dropped), while "60" on a 00:00Z start and "D" on the
    // NYSE / NSE lanes (the chart's first bar IS the session open) keep
    // their first bucket). Whether the bucket was in progress is read from
    // the auxiliary 1m feed (did it trade between the bucket's nominal open
    // and the first chart bar? the NSE week whose Monday was a holiday opens
    // on Tuesday and is kept). Historical intraday single-feed forex/cfd D
    // requests also omit their partial first session, using its actual trading
    // open rather than its label. Other single-feed series keep their feed-start
    // behavior; native feeds retain their own rules. Lower-TF evaluators are
    // untouched (their slices begin at the first chart bar anyway), and the
    // flag above keeps its explicit epoch plus the EMA na-warmup semantics.
    int64_t security_first_chart_bar_ms_ = 0;

    // Opt-in historical-only request.security lookahead projection. TradingView
    // can merge a completed higher-timeframe bar onto the first chart child
    // when a finite historical batch is already known. The normal path
    // remains progressive, and stream warmup/realtime deliberately ignore this
    // selector so future data can never leak into a live continuation.
    bool historical_security_lookahead_projection_ = false;
    bool historical_security_lookahead_projection_active_ = false;

    // Boundary-fallback publication replays the completed caller's already
    // evaluated final requested value. Force generated TA sites down their
    // recompute path so the replay advances merged history only, never the
    // requested-context TA cadence. True only inside
    // publish_security_eval_state_at_calling_boundary's own scope.
    bool security_history_publication_replay_ = false;

    // Read-only test projection cache; no future execution can observe it.
    mutable std::vector<FixtureIntentRow> source_pending_view_cache_;

    // Transient excursion-sampler caches (RULING A48). Pure caches: each is
    // re-derived from the immutable placement snapshots and the delivered
    // decision context, and cleared at the applied-notification boundary.
    mutable bool excursion_priced_fill_ = false;
    mutable bool excursion_level_fill_ = false;
    mutable bool excursion_margin_call_ = false;
    // Which of the owner's two margin-slice chronologies the pending slice was
    // born in: true when it samples only the traversed waypoint prefix (the
    // POOC pre-script pass, or the 1x-long opening slice taken inside
    // process_pending_orders before a priced exit's fill), false when the
    // non-POOC end-of-bar opening trim inherits the complete script bar.
    mutable bool excursion_margin_prefix_ = false;
    // A slice the owner books with no sample of the current bar at all: the
    // carried position's open slice and the general pre-exit slice own only
    // their carried extremes and the fill itself.
    mutable bool excursion_margin_fill_only_ = false;
    mutable bool excursion_range_end_projection_ = false;
    mutable double excursion_trail_offset_ticks_ =
        std::numeric_limits<double>::quiet_NaN();
    // Held units the applied request's precommit saw (the owner's pre-fill
    // position_qty_); cleared after the applied notification.
    mutable double precommit_held_units_ =
        std::numeric_limits<double>::quiet_NaN();
    // The matcher basis behind a TRAIL fill. ab9714be pine_fills.cpp:5766-5770
    // reads the arming peak off the PRE-slip fill price (apply_fill_slippage
    // runs later, at :7136/:7184), while the booked
    // ClosedLotExcursionFacts::fill_price the sampler sees is already slipped.
    mutable double excursion_trail_raw_price_ =
        std::numeric_limits<double>::quiet_NaN();
};

using PineNativeHost = PineStrategyHost;
using FixtureIntentRow = PineStrategyHost::FixtureIntentRow;
using FixtureIntentKind = PineStrategyHost::FixtureIntentKind;

} // namespace pineforge::source
