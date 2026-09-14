#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_scheduler.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace pineforge::source {

// L2-only fixture host. It is deliberately a distinct native host rather than
// a base of, alias for, or edit to the live PineStrategyHost. Consequently no
// generated route can reach this lowering before L3a switches inheritance.
class PineNativeHost : public NativeStrategyHost {
public:
    explicit PineNativeHost(
        compat::pine::CapAttachment cap = compat::pine::CapAttachment::None);
    ~PineNativeHost() override;

    PineNativeHost(const PineNativeHost&) = delete;
    PineNativeHost& operator=(const PineNativeHost&) = delete;

    void prepare_native_begin(const NativeBeginArgs&) final;
    void on_native_run_begin() final;
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) final;
    void on_native_bar(const Bar&, const NativeDecisionContext&) final;
    void on_native_applied(const native_order::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) final;
    native_order::ExecutionTerms resolve_execution_terms(
        const NativeExecutionTermsFacts&) const final;
    NativePrecommitVerdict validate_execution_precommit(
        const NativePrecommitView&) const final;

    virtual void on_source_bar(const Bar&) = 0;

    // Generated source surface, forwarded into the adapter. These signatures
    // intentionally match the live source host until L3a joins the routes.
    void configure_pine_strategy(const PineStrategyConfig&);
    void set_strategy_override(const StrategyOverrides&);
    void set_pine_risk_direction(int direction);
    void set_pine_risk_max_cons_loss_days(int value);
    void set_pine_risk_max_drawdown(double value, bool percent);
    void set_pine_risk_max_intraday_loss(double value, bool percent);
    void set_pine_risk_max_intraday_filled_orders(int limit);
    void set_pine_risk_max_position_size(double value);
    void enable_pine_intraday_cap();
    void attach_pine_execution_adapter();
    void set_syminfo_metadata(const std::string& key, double value) override;

    void strategy_entry(const std::string& id, bool is_long,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& comment = {}, const std::string& oca_name = {},
                        int oca_type = 0, int qty_type = -1);
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

    bool is_first_tick() const noexcept { return scheduler_.is_first_tick(); }
    bool is_last_tick() const noexcept { return scheduler_.is_last_tick(); }
    bool history_advances_new_bar() const noexcept {
        return scheduler_.history_advances_new_bar();
    }
    bool security_series_slot_is_new(int slot) const noexcept {
        return scheduler_.security_series_slot_is_new(slot);
    }
    int pine_bar_index() const noexcept { return source_bar_index_; }
    int pine_last_bar_index() const noexcept { return source_last_bar_index_; }
    double prev_chart_close() const noexcept { return scheduler_.previous_chart_close(); }
    int last_bar_dual_entry_path() const noexcept {
        return adapter_.pending_intent_view().last_bar_dual_entry_path();
    }
    const PendingIntentView& pending_intent_view() const noexcept {
        return adapter_.pending_intent_view();
    }
    int short_seed_collision_role_v1(native_order::RequestHandle handle) const noexcept {
        return adapter_.short_seed_collision_role_v1(std::move(handle));
    }
    double live_position_size() const override { return physical_position().signed_units; }
    int pending_order_count() const noexcept { return pending_intent_view().size(); }
    int probe_fill_qty(int index, double fill_price, double* qty, int* close_only,
                       int* partition) const noexcept {
        return pending_intent_view().probe_fill_qty(index, fill_price, qty, close_only, partition);
    }
    int pending_order_level_resolved(int index) const noexcept {
        return pending_intent_view().level_resolved(index);
    }
    int pending_order_effective_levels(int index, double* stop, double* limit,
                                       double* trail_activation) const noexcept {
        return pending_intent_view().effective_levels(index, stop, limit, trail_activation);
    }

protected:
    void hash_source_extension(BrokerStateHashSink&) const override;

private:
    friend class PineScheduler;

    StagedConfiguration staged_configuration() const;
    void scheduler_prepare_script_run(const std::vector<Bar>& bars, bool static_eligible,
                                      int expected_script_bars);
    void scheduler_configure_security_evaluators();
    void scheduler_publish_source_bar(const Bar&, bool first_tick);
    bool scheduler_coof_enabled() const noexcept { return config_.calc_on_order_fills; }
    static PineStrategyConfig apply_overrides(PineStrategyConfig, const StrategyOverrides&);

protected:
    // @source-state begin
    PineStrategyConfig config_{};
    StrategyOverrides override_{};
    QtyType default_qty_type_ = QtyType::FIXED;
    double default_qty_value_ = 1.0;
    int pyramiding_ = 1;
    bool close_entries_rule_any_ = false;
    PineExecutionAdapter adapter_;
    PineScheduler scheduler_;
    int source_bar_index_ = -1;
    int source_last_bar_index_ = -1;
    std::uint64_t source_callback_count_ = 0;
    // @source-state end
};

} // namespace pineforge::source
