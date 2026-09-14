#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_language_state.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_policy_support.hpp>
#include <pineforge/compat/pine/intraday_cap.hpp>

// Generated constructors can explicitly select Pine cap compatibility before
// any host metadata setter. These capability macros belong to the source host
// surface; the generic engine header remains source-free.
#define PINEFORGE_HAS_EXPLICIT_PINE_CAP_V1 1
#define PINEFORGE_HAS_EXPLICIT_PINE_EXECUTION_ADAPTER_V1 1

namespace pineforge {

void fill_pending_order_mirror(const source::PendingOrder& src,
                               const MarketAdmissionJournal* journal,
                               pf_pending_order_v1_t* out);
void fill_pending_order_mirror(const source::PendingOrder& src,
                               pf_pending_order_v1_t* out);

} // namespace pineforge

namespace pineforge::source {

// Intermediate source-layer host. The remaining source ownership surface is
// filled in by the R4-C L2 transfer (contract sections 3.1 and 7).
class PineStrategyHost : public BacktestEngine, protected PineLanguageState {
public:
    explicit PineStrategyHost(
            compat::pine::CapAttachment cap = compat::pine::CapAttachment::None);

    void on_bar(const Bar& bar) final;
    virtual void on_source_bar(const Bar& bar) = 0;
    void configure_pine_strategy(const PineStrategyConfig& config);
    void set_strategy_override(const StrategyOverrides& overrides);
    void set_pine_risk_direction(int direction);
    void set_pine_risk_max_cons_loss_days(int value);
    void set_pine_risk_max_drawdown(double value, bool percent);
    void set_pine_risk_max_intraday_loss(double value, bool percent);
    void set_pine_risk_max_intraday_filled_orders(int limit);
    void set_pine_risk_max_position_size(double value);

    void strategy_entry(const std::string& id, bool is_long,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& comment = "",
                        const std::string& oca_name = "",
                        int oca_type = 0,
                        int qty_type = -1);
    void strategy_close(const std::string& id, const std::string& comment = "",
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        double qty_percent = std::numeric_limits<double>::quiet_NaN(),
                        bool immediately = false);
    void strategy_close(const std::string& id, const std::string& comment,
                        double qty, double qty_percent, bool immediately,
                        uint64_t callsite_token);
    void strategy_close_all();
    void strategy_exit(const std::string& id, const std::string& from_entry,
                       double limit_price, double stop_price,
                       double trail_points = std::numeric_limits<double>::quiet_NaN(),
                       double trail_offset = std::numeric_limits<double>::quiet_NaN(),
                       double trail_price = std::numeric_limits<double>::quiet_NaN(),
                       double qty_percent = 100.0,
                       const std::string& comment = "",
                       double qty = std::numeric_limits<double>::quiet_NaN(),
                       const std::string& oca_name = "",
                       double profit_ticks = std::numeric_limits<double>::quiet_NaN(),
                       double loss_ticks = std::numeric_limits<double>::quiet_NaN());
    void strategy_cancel(const std::string& id);
    void strategy_cancel_all();
    void strategy_order(const std::string& id, bool is_long, double qty,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& oca_name = "",
                        int oca_type = 0);

    int pine_bar_index() const;
    int pine_last_bar_index() const;
    double prev_chart_close() const;
    int last_bar_dual_entry_path() const;
    double live_position_size() const override;
    int pending_order_count() const;
    MarketAdmissionJournal& market_admission_journal();
    const MarketAdmissionJournal& market_admission_journal() const;
    std::vector<admission::Field> market_admission_fields() const;
    const PendingOrder& pending_order_at(int i) const;
    int probe_fill_qty(int index, double fill_price, double* qty,
                       int* close_only, int* partition) const;
    int pending_order_level_resolved(int index) const;
    int pending_order_effective_levels(int index, double* stop,
                                       double* limit,
                                       double* trail_activation) const;
    void enable_pine_intraday_cap();
    void attach_pine_execution_adapter();
    void set_syminfo_metadata(const std::string& key, double value) override;
    bool set_aux_security_feed(const Bar* bars, int n,
                               const std::string& input_tf) override;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    bool source_aux_security_feed_enabled() const override;
    void source_aux_security_input_view(const Bar*& bars, int& n) const override;
#endif
    int observe_last_bar_dual_entry_path_v1() const override;
    int observe_pending_count_v1() const override;
    int observe_pending_copy_v1(int index, pf_pending_order_v1_t* out) const override;
    int observe_probe_fill_qty(int index, double fill_price, double* qty,
                               int* close_only, int* partition) const override;
    int observe_pending_level_resolved(int index) const override;
    int observe_pending_effective_levels(int index, double* stop, double* limit,
                                         double* trail_activation) const override;
    double observe_trail_best_price_v1() const override;

protected:
    // @source-state begin
    PineExecutionAdapter adapter_;
    using PineLanguageState::pos_view_freeze_bar_;
    using PineLanguageState::pos_view_frozen_side_;
    using PineLanguageState::pos_view_frozen_qty_;
    using PineLanguageState::pos_view_frozen_entry_qty_;
    using PineLanguageState::_src_series_active_;
    using PineLanguageState::_src_open_;
    using PineLanguageState::_src_high_;
    using PineLanguageState::_src_low_;
    using PineLanguageState::_src_close_;
    using PineLanguageState::_src_volume_;
    using PineLanguageState::_src_hl2_;
    using PineLanguageState::_src_hlc3_;
    using PineLanguageState::_src_ohlc4_;
    using PineLanguageState::_src_hlcc4_;
    using PineLanguageState::prev_chart_close_;
    using PineLanguageState::last_chart_close_;
    using PineLanguageState::bar_index_offset_;
    using PineLanguageState::is_first_tick_;
    using PineLanguageState::is_last_tick_;
    using PineLanguageState::history_slot_is_new_;
    using PineLanguageState::coof_checkpoint_contains_current_bar_;
    using PineLanguageState::coof_checkpoint_src_open_;
    using PineLanguageState::coof_checkpoint_src_high_;
    using PineLanguageState::coof_checkpoint_src_low_;
    using PineLanguageState::coof_checkpoint_src_close_;
    using PineLanguageState::coof_checkpoint_src_volume_;
    using PineLanguageState::coof_checkpoint_src_hl2_;
    using PineLanguageState::coof_checkpoint_src_hlc3_;
    using PineLanguageState::coof_checkpoint_src_ohlc4_;
    using PineLanguageState::coof_checkpoint_src_hlcc4_;
    using PineLanguageState::coof_checkpoint_prev_chart_close_;
    using PineLanguageState::coof_checkpoint_last_chart_close_;

    std::set<std::string> cycle_filled_entry_ids_;
    std::unordered_map<std::string, double> id_unclosed_qty_;
    bool sb_close_active_ = false;
    int sb_close_bar_ = -1;
    int sb_close_calls_ = 0;
    std::string sb_close_first_id_;
    double sb_close_first_target_ = 0.0;
    bool sb_close_first_carry_valid_ = false;
    double sb_close_first_carry_qty_ = 0.0;
    std::string sb_close_id_;
    std::string sb_close_comment_;
    std::unordered_map<std::string, double> close_reserved_qty_;
    std::unordered_map<std::string, double> close_two_call_first_qty_;
    int callsite_close_bar_ = -1;
    uint64_t callsite_close_queue_seq_ = 0;
    struct SameBarCloseCallsite {
        uint64_t token = 0;
        bool active = false;
        int calls = 0;
        std::string first_id;
        double first_target = 0.0;
        bool first_ledger_consumed = false;
        bool first_carry_valid = false;
        double first_carry_qty = 0.0;
        std::string id;
        std::string comment;
        double target = 0.0;
        std::vector<std::string> deferred_cleanup_ids;
        uint64_t queue_seq = 0;
        bool retire_ledger_whole = true;
    };
    std::unordered_map<uint64_t, SameBarCloseCallsite> callsite_close_callsites_;
    double callsite_close_admitted_total_ = 0.0;
    std::unordered_map<uint64_t, std::unordered_map<std::string, double>>
        callsite_close_reserved_qty_;
    std::unordered_map<uint64_t, std::unordered_map<std::string, double>>
        callsite_close_two_call_first_qty_;
    QtyType default_qty_type_ = QtyType::FIXED;
    double default_qty_value_ = 1.0;
    int pyramiding_ = 1;
    bool margin_zero_cover_full_liquidation_ = false;
    bool close_entries_rule_any_ = false;
    int64_t next_order_seq_ = 1;
    uint64_t exit_leg_event_seq_ = 0;
    int priced_entry_activity_bar_ = -1;
    bool priced_entry_filled_this_bar_ = false;
    struct NamedEntryCancelContext {
        uint64_t entry_incarnation = 0;
        uint64_t surviving_exit_incarnation = 0;
    };
    std::vector<PendingOrder> pending_orders_;
    std::unordered_map<std::string, NamedEntryCancelContext>
        named_entry_cancelled_incarnation_in_current_eval_;
    std::unordered_set<std::string> consumed_partial_exit_ids_;
    std::unordered_set<std::string> scratch_skip_ids_;
    std::vector<uint64_t> scratch_filled_incarnations_;
    internal::DualEntryStopPathWinner dual_entry_path_{};
    internal::DualEntryStopPathWinner last_bar_dual_entry_decision_{};
    double trail_best_before_bar_ = std::numeric_limits<double>::quiet_NaN();
    int trail_best_before_bar_index_ = -1;
    int64_t trail_best_before_bar_position_cycle_ = 0;
    uint64_t trail_best_before_bar_fill_seq_ = 0;
    bool last_exit_fill_was_trail_ = false;
    enum class RiskDirection { BOTH, LONG_ONLY, SHORT_ONLY };
    RiskDirection risk_direction_ = RiskDirection::BOTH;
    int risk_max_cons_loss_days_ = 0;
    double risk_max_drawdown_ = 0.0;
    bool risk_max_drawdown_is_pct_ = false;
    double risk_max_intraday_loss_ = 0.0;
    bool risk_max_intraday_loss_is_pct_ = false;
    double risk_max_position_size_ = 0.0;
    int cons_loss_day_count_ = 0;
    int last_loss_day_ = -1;
    bool risk_halted_ = false;
    double intraday_pnl_ = 0.0;
    int intraday_pnl_day_ = -1;
    double intraday_loss_day_start_equity_ = std::numeric_limits<double>::quiet_NaN();
    int intraday_loss_day_ = -1;
    int intraday_loss_block_day_ = -1;
    bool intraday_loss_evaluating_ = false;
    bool intraday_loss_cancel_pending_ = false;
    bool coof_scheduler_active_ = false;
    bool coof_fill_recalc_active_ = false;
    bool coof_cursor_is_bar_close_ = false;
    bool coof_cursor_is_bar_point_ = false;
    bool coof_evaluating_path_segment_ = false;
    bool coof_recalc_at_bar_open_ = false;
    bool coof_recalc_after_first_open_fill_ = false;
    uint64_t coof_market_entry_recalc_incarnation_ = 0;
    uint64_t coof_market_entry_recalc_fill_seq_ = 0;
    bool coof_at_extreme_waypoint_ = false;
    bool coof_hist_is_segment_ = false;
    int coof_hist_path_index_ = -1;
    int coof_cascade_recalc_leg_ = -1;
    bool coof_cascade_force_wp_gap_ = false;
    double coof_cursor_price_ = std::numeric_limits<double>::quiet_NaN();
    uint64_t coof_direct_fill_events_remaining_ = 0;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    std::vector<Bar> aux_security_bars_;
    std::string aux_security_input_tf_;
    std::vector<std::size_t> aux_security_chart_begin_;
    std::vector<std::size_t> aux_security_chart_end_;
#endif
    // @source-state end

    bool history_advances_new_bar() const;
    void hash_source_extension(BrokerStateHashSink&) const override;
    void _push_source_series();
    Bar broker_trigger_bar(const Bar& bar) const;
    double compute_liquidation_price() const;
    double margin_liquidation_price() const;
    double apply_slippage(double price, bool is_buy) const;
    double apply_limit_fill(double price, bool is_buy) const;
    double apply_fill_slippage(double price, bool is_buy) const;
    double signed_position_size() const;
    void freeze_script_position_view();
    void clear_script_position_view();
    void reset_source_pending_book();
    void reset_source_order_and_close_state();
    void reset_source_risk_and_cap();
    void reset_source_margin_and_coof();
    void reset_source_bar_projections();
    void reset_source_language_series();
    std::optional<execution::Status> validate_source_lifecycle(
        const execution::LifecycleEffects& lifecycle) const;
    std::optional<execution::Status> preflight_source_lifecycle(
        const execution::LifecycleEffects& lifecycle,
        bool will_reset_to_flat, bool will_open_quoted);
    void apply_source_pre_close_lifecycle(const execution::LifecycleBatch& batch);
    void apply_source_pending_removals(
        const std::vector<execution::PendingRemoval>& removals);
    void reset_source_exit_activations_before_flatten();
    void reset_source_position_ledgers_after_book_clear();
    void on_source_append_quoted_lot_after_book(const PyramidEntry& lot);
    void reset_source_open_position_ledgers_before_book(const PyramidEntry& lot);
    void on_source_open_position_booked(const PyramidEntry& lot);
    enum class ExitLegTransitionResult {
        Applied, Replay, StaleIdentity, BindRefused, ActionRefused, Exhausted,
        RevisionExhausted
    };

    // L2 mechanically generated declarations for relocated members appear
    // between these markers while the source layer is assembled.
    // BEGIN L2 SOURCE DECLARATIONS
    OrderBirth capture_order_birth() const;
    void invoke_chart_on_bar(const Bar& bar);
    void dispatch_bar();
    void snapshot_coof_script_state();
    void restore_coof_script_state();
    void commit_coof_script_state();
    uint64_t execute_coof_script_body(
            const Bar& script_bar,
            double broker_cursor_price,
            bool cursor_is_bar_point,
            const OrderBirth& evaluation_origin,
            uint64_t direct_fill_event_budget,
            bool opening_money_prefix = false);
    uint64_t run_coof_recalc_chain(
            const Bar& script_bar, double broker_cursor_price,
            bool cursor_is_bar_point, BirthCursor cursor,
            uint64_t& evaluation_ordinal, uint64_t triggering_events,
            uint64_t max_events, uint64_t events_already,
            bool grouped_stop_recalc = false, uint64_t market_entry_incarnation = 0,
            bool opening_money_prefix = false);
    void dispatch_bar_calc_on_order_fills();
    void legacy_run_simple(const Bar* bars, int n);
    void run_magnified_bar(
            const std::vector<Bar>& sub_bars, int64_t script_bar_ts,
            bool caller_completed_on_boundary);
    void run_magnified_bar_calc_on_order_fills(
            const std::vector<Bar>& sub_bars,
            int64_t script_bar_ts,
            bool caller_completed_on_boundary);
    void legacy_run_tf(const Bar* input_bars, int n_input,
                              const std::string& input_tf,
                              const std::string& script_tf,
                              bool bar_magnifier,
                              int magnifier_samples,
                              MagnifierDistribution magnifier_dist);
    void run_tf_impl(const Bar* input_bars, int n_input,
                              const std::string& input_tf,
                              const std::string& script_tf,
                              bool bar_magnifier,
                              int magnifier_samples,
                              MagnifierDistribution magnifier_dist);
    int count_expected_script_bars(const Bar* input_bars, int n_input,
                                                    bool needs_aggregation) const;
    void init_security_eval_states_for_run(
        const std::string& effective_input_tf);
    void prepare_historical_security_lookahead_projections(
            const Bar* input_bars, int n_input,
            const std::string& effective_input_tf);
    void clear_historical_security_lookahead_projections();
    void set_session_bar_state(bool in_session,
                                               bool intraday_islastbar);
    bool pine_session_ismarket(const std::string& session,
                               const std::string& tz, int64_t bar_ms) const {
        return pineforge::pine_session_ismarket(session, tz, bar_ms, script_tf_);
    }
    bool pine_session_ispremarket(const std::string& session,
                                  const std::string& tz, int64_t bar_ms) const {
        return pineforge::pine_session_ispremarket(session, tz, bar_ms, script_tf_);
    }
    bool pine_session_ispostmarket(const std::string& session,
                                   const std::string& tz, int64_t bar_ms) const {
        return pineforge::pine_session_ispostmarket(session, tz, bar_ms, script_tf_);
    }
    int64_t time_close() const {
        return pine_time_close(current_bar_.timestamp, script_tf_, syminfo_.session,
                               syminfo_.timezone, script_tf_);
    }
    void run_simple_bar_loop(const Bar* input_bars, int n_input);
    void run_aggregation_bar_loop(const Bar* input_bars, int n_input,
                                                    bool bar_magnifier,
                                                    int expected_script_bars);
    const Series<double>& get_input_source(
            const std::string& key, const Series<double>& default_series) const;
    void legacy_run_rich(const Bar* input_bars, int n_input,
                              const std::string& input_tf,
                              const std::string& script_tf,
                              const std::unordered_map<std::string, std::string>& inputs,
                              const SymInfo& syminfo,
                              const StrategyOverrides* overrides,
                              bool bar_magnifier,
                              int magnifier_samples,
                              MagnifierDistribution magnifier_dist);
    bool legacy_stream_begin(const Bar* warmup_bars, int n_warmup,
                                      const std::string& input_tf,
                                      const std::string& script_tf);
    bool legacy_stream_push_bar(const Bar& bar);
    bool legacy_stream_push_tick(const TradeTick& tick);
    bool legacy_stream_push_ticks(const TradeTick* ticks, int n);
    bool legacy_stream_advance_time(int64_t timestamp_ms);
    bool legacy_stream_end(bool finalize_partial_input_bar);
    void dispatch_source_stream_script_bar(const Bar& bar, bool had_tick);
    void source_stream_entry_comment(const PyramidEntry&, std::string&) const override;
    void clear_aux_security_chart_ranges();
    void prepare_aux_security_chart_ranges(
            const Bar* chart_bars, int n_chart, const std::string& chart_tf);
    int64_t aux_security_calling_close_ms() const;
    void feed_aux_security_for_chart_bar(int chart_index);
    void feed_deferred_aux_security_for_chart_bar(int chart_index);
    void finalize_same_bar_market_tx_book();
    void process_carried_long_money_before_priced_orders(
            const Bar& bar);
    void process_pending_orders(const Bar& bar, bool before_pooc_script = false);
    struct CoofFillResult {
        bool filled = false;
        double fill_price = std::numeric_limits<double>::quiet_NaN();
        uint64_t fill_events = 0;
        double chart_waypoint_price = std::numeric_limits<double>::quiet_NaN();
        bool grouped_stop_recalc = false;
        uint64_t market_entry_incarnation = 0;
    };
    CoofFillResult process_next_pending_order(
            const Bar& bar,
            bool allow_market_orders,
            int& exit_closed_from_bar,
            uint64_t& exit_closed_from_incarnation,
            bool& exit_closed_was_long,
            const Bar* chart_bar = nullptr);
    bool process_carried_position_fx_rollover(const Bar& bar);
    bool entry_bar_margin_path_scope() const;
    bool entry_bar_post_fill_adverse(const Bar& bar,
                                                     double* out_mark,
                                                     double* out_pos) const;
    void process_short_margin_before_script(const Bar& bar);
    void process_carried_pooc_short_margin_before_script(const Bar& bar);
    void process_margin_call(const Bar& bar);
    bool pooc_opening_money_scope(const Bar& bar) const;
    bool pooc_trail_money_pre_exit_scope(
            const Bar& bar, const PendingOrder& order, double exit_path_position) const;
    bool tv_money_long_margin_call(
        const Bar& bar, bool carried_pooc_pre_close = false,
        bool opening_only = false,
        double before_exit_path_position =
            std::numeric_limits<double>::quiet_NaN());
    void revive_position_brackets_after_margin_call_partial(
            double margin_call_event_price);
    void settle_dormant_bracket_reissues(exit_legs::Domain domain);
    bool margin_call_slice_before_priced_exit(
            const Bar& bar, double exit_fill_price, double exit_path_position);
    bool margin_call_1x_long_opening_slice_before_priced_exit(
            const Bar& bar);
    bool whole_position_market_close_rests_for_open() const;
    bool margin_call_slice_at_bar_open(const Bar& bar);
    void update_trail_best_for_bar_open(const Bar& bar);
    void sort_exit_siblings_by_path_fill(const Bar& bar);
    bool pending_flat_market_pair_scope_is_live() const;
    bool default_flat_market_gross_scope_is_live()
            const;
    void finalize_default_flat_market_gross_admission();
    void apply_pooc_coof_explicit_flat_market_gross_admission();
    void finalize_pending_flat_market_pairs(const Bar& bar);
    void sort_orders_by_fill_phase(const Bar& bar);
    bool short_seed_collision_materialization_is_live(
            const PendingOrder& order) const;
    bool short_seed_collision_final_short_is_live(
            const PendingOrder& order) const;
    bool same_bar_market_tx_scope_is_live() const;
    bool same_bar_market_close_artifact_is_live(
            const PendingOrder& order) const;
    void apply_same_bar_market_tx_reversal(
            PendingOrder& order, double fill_price, const Bar& bar,
            double& trail_best_path_state);
    bool prearmed_market_parent_bracket_gaps_at_open(
            const PendingOrder& order, const Bar& bar,
            bool* limit_leg = nullptr) const;
    bool pending_flat_market_pair_is_live(
            const PendingOrder& order) const;
    void invalidate_pending_flat_market_pair(int64_t created_seq);
    void compact_filled_pending_orders(
            std::vector<uint64_t>& retired_incarnations,
            int exit_closed_from_bar,
            uint64_t exit_closed_from_incarnation,
            bool exit_closed_was_long);
    bool flat_dual_stop_opposite_is_live(
            const PendingOrder& order, bool flat_dual_stop_pair) const;
    bool use_default_stop_placement_qty(
            const PendingOrder& order, double fill_price,
            bool flat_dual_stop_pair = false) const;
    bool stop_entry_margin_admission_declines(
            const PendingOrder& order, double fill_price, const Bar& /*bar*/,
            bool flat_dual_stop_pair) const;
    void apply_filled_order_to_state(
            size_t order_index,
            double fill_price,
            bool fill_is_limit,
            const Bar& bar,
            double& trail_best_path_state,
            int& exit_closed_from_bar,
            uint64_t& exit_closed_from_incarnation,
            bool& exit_closed_was_long,
            std::vector<uint64_t>& retired_incarnations,
            bool flat_dual_stop_pair = false);
    bool replaced_percent_short_market_is_live(
            const PendingOrder& order) const;
    void apply_market_order_fill(PendingOrder& order, double fill_price,
                                                 const Bar& bar,
                                                 double& trail_best_path_state,
                                                 bool later_same_tick_entry);
    void apply_entry_order_fill(PendingOrder& order, double fill_price,
                                                const Bar& bar,
                                                double& trail_best_path_state,
                                                bool flat_dual_stop_pair);
    void apply_exit_order_fill(PendingOrder& order, double fill_price,
                                               int& exit_closed_from_bar,
                                               uint64_t& exit_closed_from_incarnation,
                                               bool& exit_closed_was_long);
    void reconcile_deferred_layered_exits(
            const std::string& entry_id,
            std::vector<uint64_t>& zero_reservation_incarnations);
    void apply_raw_order_fill(PendingOrder& order, double fill_price,
                                              double& trail_best_path_state,
                                              int& exit_closed_from_bar,
                                              uint64_t& exit_closed_from_incarnation,
                                              bool& exit_closed_was_long);
    void materialize_relative_exit_prices_for_live_position();
    void suppress_declined_reversal_close_legs(
            const PendingOrder& declined_entry);
    bool dormant_bracket_trail_leg_live(const PendingOrder& o) const;
    std::optional<execution::LifecycleBatch>
    select_declined_reversal_pre_close(const Bar& bar) const;
    void mark_position_brackets_dormant_on_declined_reversal(const Bar& bar);
    double pooc_short_exit_trigger_close(
            const PendingOrder& order, const Bar& bar) const;
    enum class OrderEligibility { Proceed, Skip, Remove };
    struct FillEvaluation {
        enum class Kind { Fill, NoFill, DeferredToOpposingPass };
        Kind kind;
        double fill_price;
        bool is_limit_fill = false;
        bool exit_path_fill = false;
        double exit_path_position = std::numeric_limits<double>::quiet_NaN();
    };
    OrderEligibility classify_order_eligibility(
            PendingOrder& order, int opposing_pass,
            internal::DualEntryStopPathWinner dual_entry_path,
            const std::unordered_set<std::string>& pass0_opposing_skip_ids,
            int exit_closed_from_bar, uint64_t exit_closed_from_incarnation,
            bool exit_closed_was_long, const Bar& bar,
            bool flat_dual_stop_pair = false);
    FillEvaluation evaluate_fill_price(
            PendingOrder& order, size_t order_index, const Bar& bar,
            int opposing_pass, double trail_best_path_state,
            std::unordered_set<std::string>& pass0_opposing_skip_ids);
    double calc_qty_for_type(double fill_price, double qty_value, int qty_type) const;
    double calc_default_qty_from_equity(double fill_price, double equity) const;
    double calc_qty_for_type_from_equity(
            double fill_price, double qty_value, int qty_type, double equity) const;
    double source_reversal_qty(
            double fill_price, double explicit_qty, int explicit_qty_type,
            bool prequantized) const;
    void execute_market_entry(const std::string& id, bool is_long, double fill_price,
                                              double explicit_qty, int explicit_qty_type,
                                              PositionSide created_position_side,
                                              bool close_only_opposite,
                                              bool is_priced_entry,
                                              double tv_carry_qty,
                                              int created_bar,
                                              bool later_same_tick_entry,
                                              bool paired_flat_market_transaction,
                                              bool explicit_qty_prequantized,
                                              uint64_t entry_incarnation);
    void execute_market_exit(double fill_price);
    void record_range_end_close_trades();
    void execute_partial_exit_qty(
        double fill_price, double qty_to_close,
        PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit(double fill_price, double qty_percent,
                              PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit_by_entry(double fill_price,
                                                       const std::string& from_entry,
                                                       PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit_by_entry_qty(
            double fill_price, const std::string& from_entry, double qty_to_close,
            PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit_by_entry_percent(double fill_price,
                                                               const std::string& from_entry,
                                                               double qty_percent,
                                                               PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    double cover_samebar_market_adds_on_exit(const PendingOrder& order,
                                                             double fill_price,
                                                             PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void cancel_oca_group(std::string oca_name, std::string exclude_id);
    void reduce_oca_group(std::string oca_name,
                                          std::string exclude_id,
                                          double filled_qty);
    void purge_exit_orders(bool retain_for_pending_entries = false);
    Trade build_close_trade(const PyramidEntry& pe, double close_qty,
                                            double fill_price, bool was_long) const;
    void emit_close_trade(const PyramidEntry& pe, double close_qty,
                                          double fill_price, bool was_long);
    void restore_source_partial_exit_slots(
            int pre_count, PositionReductionCause cause);
    exit_legs::Frame next_leg_event(
        exit_legs::Phase phase = exit_legs::Phase::Observation);
    void apply_leg_action(PendingOrder& order, exit_legs::Operation operation,
                          std::optional<exit_legs::Frame> supplied = std::nullopt);
    void bind_exit_activation(PendingOrder& order);
    void bind_retained_exit_activations();
    void unbind_exit_activations();
    void open_fresh_position(PositionSide requested, double fill_price,
                                             double qty, const std::string& id,
                                             uint64_t entry_incarnation);
    execution::Result settle_source_opening(
            PositionSide requested, double fill_price, double qty,
            const std::string& id, const std::string& comment, uint64_t incarnation);
    void consume_tv_carry_from_siblings(const std::string& id,
                                                        PositionSide created_position_side,
                                                        int created_bar);
    void enter_market_from_flat(const std::string& id, bool is_long,
                                                double fill_price, double explicit_qty,
                                                int explicit_qty_type,
                                                PositionSide created_position_side,
                                                bool is_priced_entry, double tv_carry_qty,
                                                int created_bar,
                                                bool explicit_qty_prequantized,
                                                uint64_t entry_incarnation);
    void add_to_pyramid_market(const std::string& id, bool is_long,
                                               double fill_price, double explicit_qty,
                                               int explicit_qty_type,
                                               PositionSide created_position_side,
                                               bool is_priced_entry,
                                               uint64_t entry_incarnation);
    void add_to_pyramid_market_with_qty_provenance(
            const std::string& id, bool is_long, double fill_price, double explicit_qty,
            int explicit_qty_type, PositionSide created_position_side,
            bool is_priced_entry, bool explicit_qty_prequantized,
            uint64_t entry_incarnation);
    void close_opposite_then_enter(const std::string& id, bool is_long,
                                                   double fill_price, double explicit_qty,
                                                   int explicit_qty_type,
                                                   bool purge_pending_exits,
                                                   bool explicit_qty_prequantized,
                                                   uint64_t entry_incarnation);
    std::vector<execution::PendingRemoval>
    snapshot_exit_pending_removals() const;
    void apply_resolved_close_opposite_then_enter(
            const std::string& id, bool is_long, double fill_price,
            double explicit_qty, int explicit_qty_type,
            bool explicit_qty_prequantized, uint64_t entry_incarnation,
            execution::LifecycleEffects lifecycle);
    void flip_market_position_to(const std::string& id, bool is_long,
                                                 double fill_price, double explicit_qty,
                                                 int explicit_qty_type,
                                                 bool explicit_qty_prequantized,
                                                 bool close_only,
                                                 uint64_t entry_incarnation);
    void sequential_same_tick_reversal_fill(const std::string& id,
                                                            bool is_long,
                                                            double fill_price,
                                                            double explicit_qty,
                                                            int explicit_qty_type,
                                                            uint64_t entry_incarnation);
    void sequential_same_tick_reversal_fill_with_qty_provenance(
            const std::string& id, bool is_long, double fill_price, double explicit_qty,
            int explicit_qty_type, bool explicit_qty_prequantized, uint64_t entry_incarnation);
    exit_legs::Domain current_exit_leg_domain() const;
    exit_legs::Frame preview_next_leg_event(exit_legs::Phase phase) const;
    const PendingOrder* find_unique_pending(
            uint64_t incarnation, int64_t created_seq) const;
    PendingOrder* find_unique_pending(
            uint64_t incarnation, int64_t created_seq);
    ExitLegTransitionResult transition_exit_leg(
            exit_legs::Lifecycle& legs, uint64_t order_incarnation,
            exit_legs::Operation operation, std::optional<exit_legs::Frame> supplied,
            uint64_t& event_seq, int64_t position_cycle) const;
    double pending_same_bar_close_target() const;
    double close_reserved_other_qty(const std::string& id) const;
    double callsite_close_reserved_other_qty(
            uint64_t /*callsite_token*/, const std::string& id) const;
    double callsite_close_physical_reserved_other_qty(
            uint64_t callsite_token, const std::string& id) const;
    void enqueue_same_bar_close(const std::string& id,
                                                const std::string& comment,
                                                uint64_t callsite_token);
    void flush_same_bar_close();
    void flush_active_same_bar_close(
        double admitted_target = std::numeric_limits<double>::quiet_NaN(),
        double pending_later_qty = 0.0,
        bool defer_first_ledger_consume = false, uint64_t callsite_token = 0,
        bool retire_ledger_whole = true);
    void close_reservation_capture_populations(uint64_t admitted_incarnation);
    bool compute_close_target_qty(const std::string& id,
                                                  double qty,
                                                  double qty_percent,
                                                  bool use_script_position_view,
                                                  double& matching_qty_out,
                                                  double& qty_to_close_out,
                                                  bool& all_entries_match_out,
                                                  double& retired_ledger_qty_out);
    bool reversal_pair_close_keeps_brackets(
            const std::string& id) const;
    void hold_brackets_dormant_for_reversal_pair_close(
            const std::string& id);
    void cancel_orders_for_full_close(const std::string& id, bool /*closing_long*/);
    void cancel_same_bar_market_reentries_after_full_close(
            bool closed_long, bool preserve_undercap_entries);
    void execute_immediate_close(const std::string& id,
                                                 const std::string& comment,
                                                 double qty_to_close,
                                                 double matching_qty,
                                                 bool closes_full_position,
                                                 bool closes_fifo_qty,
                                                 bool closes_any_qty,
                                                 bool use_script_position_view,
                                                 bool preserve_undercap_entries);
    uint64_t queue_deferred_close_order(
            const std::string& id,
            const std::string& comment,
            double qty_to_close,
            double matching_qty,
            bool closes_full_position,
            bool closes_any_qty,
            double consumed_ledger_qty = std::numeric_limits<double>::quiet_NaN(),
            double retired_ledger_qty = 0.0);
    bool from_entry_holds_live_lot(const std::string& from_entry) const;
    void clear_existing_exit_order(const std::string& id,
                                                   const std::string& from_entry,
                                                   bool has_trail_request,
                                                   double trail_points,
                                                   double trail_offset,
                                                   double trail_price,
                                                   int64_t& preserved_seq_out,
                                                   uint64_t& replaced_incarnation_out,
                                                   double& preserved_reserved_qty_out,
                                                   int& cleared_leg_count_out,
                                                   std::optional<exit_legs::Definition>* replaced_definition_out = nullptr);
    bool compute_exit_reserved_qty(const std::string& from_entry,
                                                   double preserved_reserved_qty,
                                                   double live_pos_qty,
                                                   double& qp_io,
                                                   bool& is_partial_io,
                                                   double& reserved_qty_out);
    BacktestEngine::BarTime _decompose_bar_time_chart_tz() const;
    execution::Status on_source_close_preflight(
            const Trade* rows, size_t count, std::optional<int>& loss_day) const;
    void on_source_close_observed(
            const Trade* rows, size_t count, std::optional<int> loss_day);
    bool check_risk_allow_entry(bool is_long) const;
    void update_risk_state();
    int intraday_loss_day_key() const;
    void intraday_loss_begin_bar(const Bar& bar);
    bool intraday_loss_orders_blocked() const;
    bool evaluate_max_intraday_loss(double mark_price,
                                                    double excluded_realized);
    void finish_intraday_loss_cancel();
    void evaluate_max_intraday_loss_over_path(const Bar& bar);
    void update_per_trade_extremes();
    admission::Configuration admission_configuration() const;
    admission::CurrentPrices admission_current_prices(const PendingOrder& order) const;
    bool opening_admission_eligible(const MarketAdmissionDraft& draft) const;
    admission::BookObservation admission_book_observation(const PendingOrder& order) const;
    admission::CommandCapture begin_market_command(admission::CommandKind kind,
            const std::string& id,bool buy,double qty,int qty_type,double limit,double stop,const std::string& oca,int oca_type);
    void bind_market_command(PendingOrder& order,admission::CommandCapture& command);
    admission::ReviewCapture begin_market_review(admission::Checkpoint checkpoint);
    void reclaim_market_admission();
    void record_market_sizing_revision(PendingOrder& order,admission::SizingObservation before,double affordability_before);
    // BEGIN L2 POLICY MEMBERS
        compat::pine::CapClock pine_cap_clock() const;
        compat::pine::Calculation pine_cap_calculation() const;
        static compat::pine::Side pine_cap_side(PositionSide side);
        static compat::pine::OrderKind pine_cap_kind(OrderType type);
        compat::pine::MatchedAttempt pine_cap_attempt(const PendingOrder& order) const;
        bool _intraday_cap_currently_latched();
        bool tv_money_scope(double price) const;
        bool rounded_pooc_flat_signal_cost_scope(const PendingOrder& order) const;
        bool pooc_flat_money_admission_scope(const PendingOrder& order,
                                             double fill_price) const;
        bool ordinary_fractional_market_admission_scope(const PendingOrder& order) const;
        bool rounded_signal_cost_scope(const PendingOrder& order) const;
        bool rounded_price_admission_scope(const PendingOrder& order) const;
        bool tv_money_lot_sizing() const;
        double tv_money_required_margin(double required, double mark) const;
        double calc_qty(double fill_price) const;
        double frozen_sizing_price(bool is_buy) const;

        double frozen_default_market_qty(bool is_buy) const;
        bool coof_default_market_sizes_at_fill() const;
        void refresh_frozen_default_sizing_after_margin_call();
    // END L2 POLICY MEMBERS
    // END L2 SOURCE DECLARATIONS
};

} // namespace pineforge::source
