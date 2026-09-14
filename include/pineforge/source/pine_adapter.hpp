#pragma once

// Pine-only lowering facts. This header deliberately sits above the generic
// native-host surface: no native kernel type learns a Pine source identifier,
// sizing convention, or lifecycle vocabulary from it.
#include <pineforge/native_host.hpp>
#include <pineforge/compat/pine/intraday_cap.hpp>
#include <pineforge/compat/pine/order_priority.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pineforge::source {

// A2: Pine entry ids and from_entry values are source strings, never a
// kernel-owned identifier type.
using SourceId = std::string;

inline constexpr char kSourceAdapterDomain[] = "pineforge-source-adapter/v2";

struct PineStrategyConfig {
    bool process_orders_on_close = false;
    bool calc_on_order_fills = false;
    double initial_capital = 1000000.0;
    int default_qty_type = static_cast<int>(QtyType::FIXED);
    double default_qty_value = 1.0;
    int pyramiding = 1;
    double commission_value = 0.0;
    int commission_type = static_cast<int>(CommissionType::PERCENT);
    int slippage = 0;
    double margin_long = 100.0;
    double margin_short = 100.0;
    bool close_entries_rule_any = false;
    bool src_series_active = false;
};

struct StrategyOverrides {
    double initial_capital = std::numeric_limits<double>::quiet_NaN();
    double commission_value = std::numeric_limits<double>::quiet_NaN();
    double default_qty_value = std::numeric_limits<double>::quiet_NaN();
    int pyramiding = -1;
    int slippage = -1;
    int commission_type = -1;
    int default_qty_type = -1;
    int process_orders_on_close = -1;
    int calc_on_order_fills = -1;
    int close_entries_rule = -1;
};

// Value snapshot supplied by PineNativeHost at the begin boundary. It owns
// every source-side value the adapter needs to form a run spec; none of these
// values are retained by generic native code.
struct StagedConfiguration {
    SymInfo syminfo{};
    InputsMap inputs{};
    std::string chart_timezone{};
    double account_fx = 1.0;
    std::vector<std::int64_t> account_fx_effective_from_ms{};
    std::vector<double> account_fx_per_quote{};
    std::optional<double> quantity_grid{};
};

enum class PineOrderFamily : std::uint8_t {
    Entry = 0,
    Close = 1,
    CloseAll = 2,
    ExitLimit = 3,
    ExitStop = 4,
    ExitTrail = 5,
    Order = 6,
    Margin = 7,
};

struct PineExitLevels {
    double limit = std::numeric_limits<double>::quiet_NaN();
    double stop = std::numeric_limits<double>::quiet_NaN();
    double trail_points = std::numeric_limits<double>::quiet_NaN();
    double trail_offset = std::numeric_limits<double>::quiet_NaN();
    double trail_price = std::numeric_limits<double>::quiet_NaN();
    double profit_ticks = std::numeric_limits<double>::quiet_NaN();
    double loss_ticks = std::numeric_limits<double>::quiet_NaN();
};

struct PineSizingSnapshot {
    double equity = std::numeric_limits<double>::quiet_NaN();
    double price = std::numeric_limits<double>::quiet_NaN();
    double fx = std::numeric_limits<double>::quiet_NaN();
    double mark = std::numeric_limits<double>::quiet_NaN();
    double frozen_units = std::numeric_limits<double>::quiet_NaN();
    bool at_fill = false;
};

// Immutable source placement evidence keyed by the native request handle.
// It is written only at submit/replace/applied boundaries and is read by the
// const terms/precommit/projection methods.
struct PlacementSnapshot {
    PineOrderFamily family = PineOrderFamily::Entry;
    SourceId source_id{};
    SourceId from_entry{};
    std::string comment{};
    std::string oca_name{};
    int oca_type = 0;
    int qty_type = -1;
    double requested_qty = std::numeric_limits<double>::quiet_NaN();
    double qty_percent = std::numeric_limits<double>::quiet_NaN();
    bool is_long = true;
    bool immediately = false;
    bool opening = false;
    bool deferred_cohort = false;
    bool frozen_market_instruction = false;
    bool reverse_to = false;
    bool terms_priced_reverse = false;
    double frozen_reversal_transaction = std::numeric_limits<double>::quiet_NaN();
    std::int64_t placement_cycle = 0;
    std::uint64_t sequential_group = 0;
    std::uint8_t sequential_rank = 0;
    bool has_full_entry_bracket = false;
    // Explicit bracket legs retain the source opening provenance that caused
    // their submission.  A pending parent rejected at a later candidate can
    // then retire only its own deferred legs.
    native_order::RequestHandle bracket_origin{};
    std::uint64_t source_sequence = 0;
    std::int64_t placement_script_open_ms = 0;
    std::int64_t placement_sub_open_ms = 0;
    PineSizingSnapshot sizing{};
    PineExitLevels exit_levels{};
};

struct ShortSeedPlan {
    native_order::RequestHandle long_entry{};
    native_order::RequestHandle materialize_long{};
    native_order::RequestHandle final_short{};
    bool active = false;
};

struct SourceDayLedger {
    std::int64_t current_day = std::numeric_limits<std::int64_t>::min();
    std::int64_t last_loss_day = std::numeric_limits<std::int64_t>::min();
    int consecutive_loss_days = 0;
    std::int64_t intraday_loss_day = std::numeric_limits<std::int64_t>::min();
    double intraday_start_equity = std::numeric_limits<double>::quiet_NaN();
    double intraday_realized = 0.0;
    std::uint64_t observed_applied_ordinal = 0;
};

struct PineRiskState {
    int direction = 0; // 0 both, >0 long, <0 short
    int max_cons_loss_days = 0;
    double max_drawdown = 0.0;
    bool max_drawdown_percent = false;
    double max_intraday_loss = 0.0;
    bool max_intraday_loss_percent = false;
    double max_position_size = 0.0;
    bool halted = false;
};

class PineExecutionAdapter;

// Allocation-free view facade for Appendix C's later C projection. L2 does
// not wire the C ABI; this only exposes named truthful sources to native
// fixture tests and keeps observer reads side-effect-free.
class PendingIntentView {
public:
    int size() const noexcept;
    int probe_fill_qty(int index, double fill_price, double* qty,
                       int* close_only, int* partition) const noexcept;
    int level_resolved(int index) const noexcept;
    int effective_levels(int index, double* stop, double* limit,
                         double* trail_activation) const noexcept;
    int short_seed_collision_role(int index) const noexcept;
    int last_bar_dual_entry_path() const noexcept;
    double trail_best_price() const noexcept;

private:
    friend class PineExecutionAdapter;
    const PineExecutionAdapter* owner_ = nullptr;
};

class PineExecutionAdapter {
public:
    // Keep the legacy host's construction surface valid until L3a. A null
    // host means this compatibility carrier has no lowering authority.
    explicit PineExecutionAdapter(
            compat::pine::CapAttachment attachment = compat::pine::CapAttachment::None);
    PineExecutionAdapter(NativeStrategyHost& host,
                         compat::pine::CapAttachment attachment = compat::pine::CapAttachment::None);

    void bind(NativeStrategyHost& host) noexcept;
    void reset_for_run();
    void set_configuration(const PineStrategyConfig& config) noexcept;
    void set_staged_configuration(const StagedConfiguration& staged);

    NativeRunSpec project(const PineStrategyConfig&, const StagedConfiguration&,
                          const NativeBeginArgs&) const;

    void entry(const SourceId& id, bool is_long,
               double limit_price = std::numeric_limits<double>::quiet_NaN(),
               double stop_price = std::numeric_limits<double>::quiet_NaN(),
               double qty = std::numeric_limits<double>::quiet_NaN(),
               const std::string& comment = {}, const std::string& oca_name = {},
               int oca_type = 0, int qty_type = -1);
    void close(const SourceId& id, const std::string& comment = {},
               double qty = std::numeric_limits<double>::quiet_NaN(),
               double qty_percent = std::numeric_limits<double>::quiet_NaN(),
               bool immediately = false, std::uint64_t callsite_token = 0);
    void close_all();
    void exit(const SourceId& exit_id, const SourceId& from_entry,
              double limit_price, double stop_price,
              double trail_points = std::numeric_limits<double>::quiet_NaN(),
              double trail_offset = std::numeric_limits<double>::quiet_NaN(),
              double trail_price = std::numeric_limits<double>::quiet_NaN(),
              double qty_percent = 100.0, const std::string& comment = {},
              double qty = std::numeric_limits<double>::quiet_NaN(),
              const std::string& oca_name = {},
              double profit_ticks = std::numeric_limits<double>::quiet_NaN(),
              double loss_ticks = std::numeric_limits<double>::quiet_NaN());
    void exit_cancel_bracket(const SourceId& exit_id, const SourceId& from_entry,
                             const std::string& comment = {});
    void cancel(const SourceId& id);
    void cancel_all();
    void order(const SourceId& id, bool is_long, double qty,
               double limit_price = std::numeric_limits<double>::quiet_NaN(),
               double stop_price = std::numeric_limits<double>::quiet_NaN(),
               const std::string& oca_name = {}, int oca_type = 0);

    native_order::ExecutionTerms resolve_terms(const NativeExecutionTermsFacts&) const;
    NativePrecommitVerdict validate_precommit(const NativePrecommitView&) const;
    void on_bar_open(const Bar&, const NativeDecisionContext&);
    void on_applied(const native_order::ExecutionAppliedEvent&, const NativeDecisionContext&);

    int short_seed_collision_role_v1(native_order::RequestHandle) const noexcept;
    const PendingIntentView& pending_intent_view() const noexcept { return pending_view_; }

    void set_risk_direction(int direction) noexcept;
    void set_risk_max_cons_loss_days(int value) noexcept;
    void set_risk_max_drawdown(double value, bool percent) noexcept;
    void set_risk_max_intraday_loss(double value, bool percent) noexcept;
    void set_risk_max_position_size(double value) noexcept;
    void set_margin_call_enabled(bool enabled) noexcept;
    void enable_intraday_cap() noexcept;
    void attach_execution_adapter() noexcept;
    bool calc_on_order_fills() const noexcept { return config_.calc_on_order_fills; }
    bool process_orders_on_close() const noexcept { return config_.process_orders_on_close; }
    std::vector<native_order::RequestHandle> take_first_open_newborns();
    // Pull terminal generic receipts before a source callback observes the
    // next command boundary.  This retires group-cancelled bracket siblings
    // and pending origins rejected by native admission.
    void observe_terminal_receipts();
    // Called by the fixture scheduler after one source script evaluation so
    // re-priced carried bracket legs retain their original roster order before
    // newly pending-entry legs are appended.
    void flush_pending_bracket_legs();
    // Ordinary POOC same-direction adds are held until the source evaluation
    // closes, so a later close_all in that same evaluation settles first and
    // the add opens the next source position at the same close point.
    void flush_pending_entries();
    void begin_coof_recalc(const NativeDecisionContext&, bool first_open);
    void end_coof_recalc() noexcept;

    void hash_state(BrokerStateHashSink&) const;

    // Retained for the untouched legacy source host. New fixture state is
    // represented by the private maps below rather than this carrier alone.
    compat::pine::IntradayCap cap;
    compat::pine::OrderPriority priority;
    MarketAdmissionJournal admission_journal;

private:
    friend class PendingIntentView;
    struct CohortFacts {
        native_order::CohortHandle handle{};
        std::vector<native_order::RequestHandle> origins;
        std::vector<native_order::RequestHandle> opened;
        // Live source exposure by opening provenance.  This is source-layer
        // bookkeeping only: the generic core still resolves cohort authority
        // at every candidate.  It lets a command snapshot an existing id's
        // percentage basis before later requests in the same script pass
        // reduce that cohort.
        std::unordered_map<std::uint64_t, double> live_units_by_origin;
        std::int64_t cycle = 0;
    };

    struct PendingBracketLeg {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
        std::uint64_t family_key = 0;
    };

    struct PendingEntry {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
    };

    struct PendingRelativeExit {
        SourceId exit_id;
        SourceId from_entry;
        double trail_points = std::numeric_limits<double>::quiet_NaN();
        double trail_offset = std::numeric_limits<double>::quiet_NaN();
        double trail_price = std::numeric_limits<double>::quiet_NaN();
        double qty_percent = 100.0;
        std::string comment;
        double qty = std::numeric_limits<double>::quiet_NaN();
        std::string oca_name;
        double profit_ticks = std::numeric_limits<double>::quiet_NaN();
        double loss_ticks = std::numeric_limits<double>::quiet_NaN();
    };

    struct PendingCoofRequest {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
        bool opening = false;
        std::uint64_t family_key = 0;
    };

    NativeStrategyHost& require_host() const;
    native_order::CohortHandle cohort_for(const SourceId& id);
    std::optional<native_order::RequestHandle> submit_or_replace(
        native_order::Request request, PlacementSnapshot snapshot, bool opening,
        const SourceId& replacement_key = {});
    void remember(const native_order::RequestHandle&, PlacementSnapshot);
    void retire(const native_order::RequestHandle&) noexcept;
    std::vector<native_order::RequestHandle> openings_for(const SourceId&) const;
    double cohort_exposure_for(const SourceId&) const noexcept;
    double quantize_close_units(double basis, double percent) const noexcept;
    double active_staged_fx(std::int64_t) const noexcept;
    void apply_fx_open_margin_slice(const Bar&, const NativeDecisionContext&);
    void apply_fx_opening_margin_slice(const native_order::ExecutionAppliedEvent&,
                                       const NativeDecisionContext&);
    void submit_fx_margin_slice(const Bar&, const NativeDecisionContext&, double rate);
    void consume_cohort_units(const SourceId&, const native_order::ExecutionAppliedEvent&);
    bool origin_is_pending(const native_order::RequestHandle&) const noexcept;
    void cancel_bracket_origin(const native_order::RequestHandle&);
    void cancel_bracket_siblings(const native_order::RequestHandle&);
    void materialize_relative_exits(const PlacementSnapshot&,
                                   const native_order::ExecutionAppliedEvent&);
    bool defer_coof_tail() const noexcept;
    void flush_coof_tail();
    native_order::Owner owner_for_close(const SourceId&, bool dynamic) const;
    native_order::Trigger trigger_for(double limit_price, double stop_price,
                                      double trail_offset, double trail_price) const;
    native_order::Group group_for(const std::string&, int) const;
    PineSizingSnapshot sizing_snapshot() const;
    std::uint64_t key_for(const SourceId&, const SourceId& = {}) const noexcept;
    static std::int64_t day_key(std::int64_t timestamp_ms) noexcept;
    void refresh_pending_view() noexcept;

    // @source-state begin
    NativeStrategyHost* host_ = nullptr;
    PineStrategyConfig config_{};
    StagedConfiguration staged_{};
    mutable std::uint64_t run_counter_ = 0;
    std::uint64_t source_sequence_ = 0;
    std::unordered_map<SourceId, CohortFacts> cohorts_by_id_;
    std::unordered_map<std::uint64_t, PlacementSnapshot> placement_;
    std::unordered_map<std::uint64_t, native_order::RequestHandle> live_by_source_key_;
    std::unordered_map<std::uint64_t, std::vector<native_order::RequestHandle>> bracket_families_;
    std::vector<PendingBracketLeg> pending_bracket_legs_;
    std::vector<PendingEntry> pending_entries_;
    std::vector<PendingRelativeExit> pending_relative_exits_;
    std::vector<PendingCoofRequest> pending_coof_requests_;
    std::vector<native_order::RequestHandle> live_handles_;
    std::vector<native_order::RequestHandle> first_open_newborns_;
    std::vector<native_order::RequestHandle> pending_view_handles_;
    // Current executions settle synchronously, while their generic Applied
    // notification is delivered after the enclosing callback.  Record the
    // source-cohort debit so a second immediate command sees the new basis,
    // then suppress just that duplicate debit at notification delivery.
    std::unordered_set<std::uint64_t> current_debited_applied_ordinals_;
    std::uint64_t receipt_cursor_ = 0;
    bool materializing_relative_ = false;
    std::int64_t current_position_cycle_ = 0;
    int current_position_sign_ = 0;
    std::uint64_t next_sequential_group_ = 0;
    bool coof_recalc_active_ = false;
    bool coof_first_open_ = false;
    NativeDecisionContext coof_context_{};
    Bar coof_script_bar_{};
    bool coof_script_bar_valid_ = false;
    std::unordered_map<std::int64_t, double> pooc_close_basis_by_script_bar_;
    double pooc_open_basis_ = 0.0;
    std::int64_t pooc_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t close_all_pending_script_bar_ = std::numeric_limits<std::int64_t>::min();
    double last_fx_rate_ = std::numeric_limits<double>::quiet_NaN();
    std::int64_t position_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
    bool source_margin_call_enabled_ = true;
    SourceDayLedger day_ledger_{};
    PineRiskState risk_{};
    ShortSeedPlan short_seed_{};
    native_order::RequestHandle short_seed_candidate_long_{};
    native_order::RequestHandle short_seed_candidate_final_short_{};
    int last_bar_dual_entry_path_ = 0;
    PendingIntentView pending_view_{};
    // @source-state end
};

} // namespace pineforge::source
