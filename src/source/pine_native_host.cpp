#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace pineforge::source {

PineNativeHost::PineNativeHost(compat::pine::CapAttachment cap)
    : NativeStrategyHost(), adapter_(*this, cap) {}

PineNativeHost::~PineNativeHost() = default;

PineStrategyConfig PineNativeHost::apply_overrides(PineStrategyConfig config,
                                                   const StrategyOverrides& overrides) {
    if (!std::isnan(overrides.initial_capital)) config.initial_capital = overrides.initial_capital;
    if (!std::isnan(overrides.commission_value)) config.commission_value = overrides.commission_value;
    if (!std::isnan(overrides.default_qty_value)) config.default_qty_value = overrides.default_qty_value;
    if (overrides.pyramiding >= 0) config.pyramiding = overrides.pyramiding;
    if (overrides.slippage >= 0) config.slippage = overrides.slippage;
    if (overrides.commission_type >= 0) config.commission_type = overrides.commission_type;
    if (overrides.default_qty_type >= 0) config.default_qty_type = overrides.default_qty_type;
    if (overrides.process_orders_on_close >= 0)
        config.process_orders_on_close = overrides.process_orders_on_close != 0;
    if (overrides.calc_on_order_fills >= 0)
        config.calc_on_order_fills = overrides.calc_on_order_fills != 0;
    if (overrides.close_entries_rule >= 0)
        config.close_entries_rule_any = overrides.close_entries_rule != 0;
    return config;
}

StagedConfiguration PineNativeHost::staged_configuration() const {
    StagedConfiguration staged;
    staged.syminfo = syminfo_;
    // Existing source fixtures and generated setters may write the legacy
    // scalar mintick slot directly before begin.  It is the authoritative
    // source value at this boundary; rich SymInfo ingress keeps both slots in
    // lockstep above.
    staged.syminfo.mintick = syminfo_mintick_;
    staged.inputs = inputs_;
    staged.chart_timezone = chart_timezone_;
    staged.account_fx = account_currency_fx_;
    if (std::isfinite(qty_step_) && qty_step_ > 0.0) staged.quantity_grid = qty_step_;
    return staged;
}

void PineNativeHost::prepare_native_begin(const NativeBeginArgs& args) {
    // A12: NativeBeginArgs::syminfo is a borrowed rich-only value. Copy it
    // before returning; no pointer or caller-owned object survives this hook.
    if (args.syminfo) {
        syminfo_ = *args.syminfo;
        syminfo_mintick_ = syminfo_.mintick;
        if (std::isfinite(syminfo_.qty_step) && syminfo_.qty_step > 0.0)
            qty_step_ = syminfo_.qty_step;
    }
    if (args.inputs) inputs_ = *args.inputs;
    // Generated constructors use configure_pine_strategy, while many source
    // fixtures configure the established protected slots directly. Keep both
    // ingress styles faithful at the source boundary; generic native state is
    // still configured only from the projected NativeRunSpec below.
    PineStrategyConfig effective = config_;
    if (!source_configuration_captured_) {
        // Existing source fixtures configure the legacy protected fields in
        // their constructors.  Capture that one pre-begin state; afterwards
        // the generic consumer owns the engine's compatibility fields and
        // intentionally clears the retired POOC/COOF booleans (P6).
        effective.process_orders_on_close = process_orders_on_close_;
        effective.calc_on_order_fills = calc_on_order_fills_;
        effective.initial_capital = initial_capital_;
        effective.default_qty_type = static_cast<int>(default_qty_type_);
        effective.default_qty_value = default_qty_value_;
        effective.pyramiding = pyramiding_;
        effective.commission_value = commission_value_;
        effective.commission_type = static_cast<int>(commission_type_);
        effective.slippage = slippage_;
        effective.margin_long = margin_long_;
        effective.margin_short = margin_short_;
        effective.close_entries_rule_any = close_entries_rule_any_;
    }
    if (args.overrides_opaque) {
        const auto* overrides = static_cast<const StrategyOverrides*>(args.overrides_opaque);
        effective = apply_overrides(effective, *overrides);
    }
    const StagedConfiguration staged = staged_configuration();
    adapter_.reset_for_run();
    adapter_.set_configuration(effective);
    adapter_.set_staged_configuration(staged);
    scheduler_.capture_begin(args);
    bar_magnifier_enabled_ = args.bar_magnifier;
    diag_magnifier_sub_bars_processed_ = 0;
    diag_magnifier_sample_ticks_processed_ = 0;
    const NativeRunSpec spec = adapter_.project(effective, staged, args);
    const auto setup = configure_native(spec);
    if (setup.status != NativeSetupStatus::Applied)
        throw std::logic_error("Pine native adapter failed to configure projected run spec");
    config_ = effective;
    source_configuration_captured_ = true;
}

void PineNativeHost::on_native_run_begin() {
    source_bar_index_ = -1; source_last_bar_index_ = -1; source_callback_count_ = 0;
    scheduler_.run_begin(*this);
}
void PineNativeHost::on_native_bar_open(const Bar& bar, const NativeDecisionContext& context) {
    bar_magnifier_enabled_ = context.driver_statistics.intrabar_path_enabled;
    diag_magnifier_sub_bars_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sub_bars_processed);
    diag_magnifier_sample_ticks_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sample_ticks_processed);
    adapter_.on_bar_open(bar, context);
    scheduler_.bar_open(bar, context, *this);
}
void PineNativeHost::on_native_bar(const Bar& bar, const NativeDecisionContext& context) {
    bar_magnifier_enabled_ = context.driver_statistics.intrabar_path_enabled;
    diag_magnifier_sub_bars_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sub_bars_processed);
    diag_magnifier_sample_ticks_processed_ = static_cast<std::int64_t>(
        context.driver_statistics.sample_ticks_processed);
    adapter_.observe_terminal_receipts();
    scheduler_.bar(bar, context, *this);
}
void PineNativeHost::on_native_applied(const native_order::ExecutionAppliedEvent& event,
                                       const NativeDecisionContext& context) {
    adapter_.on_applied(event, context);
    scheduler_.applied(event, context, *this);
}
native_order::ExecutionTerms PineNativeHost::resolve_execution_terms(const NativeExecutionTermsFacts& facts) const {
    return adapter_.resolve_terms(facts);
}
NativePrecommitVerdict PineNativeHost::validate_execution_precommit(const NativePrecommitView& view) const {
    return adapter_.validate_precommit(view);
}

void PineNativeHost::configure_pine_strategy(const PineStrategyConfig& config) {
    guard_native_mutation("configure_pine_strategy");
    config_ = config;
    process_orders_on_close_ = config.process_orders_on_close;
    calc_on_order_fills_ = config.calc_on_order_fills;
    initial_capital_ = config.initial_capital;
    default_qty_type_ = static_cast<QtyType>(config.default_qty_type);
    default_qty_value_ = config.default_qty_value;
    pyramiding_ = config.pyramiding;
    commission_value_ = config.commission_value;
    commission_type_ = static_cast<CommissionType>(config.commission_type);
    slippage_ = config.slippage;
    margin_long_ = config.margin_long;
    margin_short_ = config.margin_short;
    close_entries_rule_any_ = config.close_entries_rule_any;
    adapter_.set_configuration(config_);
    source_configuration_captured_ = true;
}
void PineNativeHost::set_strategy_override(const StrategyOverrides& overrides) {
    guard_native_mutation("set_strategy_override");
    override_ = overrides;
    config_ = apply_overrides(config_, override_);
    process_orders_on_close_ = config_.process_orders_on_close;
    calc_on_order_fills_ = config_.calc_on_order_fills;
    initial_capital_ = config_.initial_capital;
    default_qty_type_ = static_cast<QtyType>(config_.default_qty_type);
    default_qty_value_ = config_.default_qty_value;
    pyramiding_ = config_.pyramiding;
    commission_value_ = config_.commission_value;
    commission_type_ = static_cast<CommissionType>(config_.commission_type);
    slippage_ = config_.slippage;
    close_entries_rule_any_ = config_.close_entries_rule_any;
    adapter_.set_configuration(config_);
    source_configuration_captured_ = true;
}
void PineNativeHost::set_pine_risk_direction(int value) { adapter_.set_risk_direction(value); }
void PineNativeHost::set_pine_risk_max_cons_loss_days(int value) { adapter_.set_risk_max_cons_loss_days(value); }
void PineNativeHost::set_pine_risk_max_drawdown(double value, bool percent) { adapter_.set_risk_max_drawdown(value, percent); }
void PineNativeHost::set_pine_risk_max_intraday_loss(double value, bool percent) { adapter_.set_risk_max_intraday_loss(value, percent); }
void PineNativeHost::set_pine_risk_max_intraday_filled_orders(int value) { adapter_.cap = value; }
void PineNativeHost::set_pine_risk_max_position_size(double value) { adapter_.set_risk_max_position_size(value); }
void PineNativeHost::enable_pine_intraday_cap() { adapter_.enable_intraday_cap(); }
void PineNativeHost::attach_pine_execution_adapter() { adapter_.attach_execution_adapter(); }
void PineNativeHost::set_syminfo_metadata(const std::string& key, double value) {
    BacktestEngine::set_syminfo_metadata(key, value);
    adapter_.cap.metadata(key, value);
    adapter_.priority.metadata(key, value);
}

void PineNativeHost::strategy_entry(const std::string& id, bool is_long, double limit_price,
                                    double stop_price, double qty, const std::string& comment,
                                    const std::string& oca_name, int oca_type, int qty_type) {
    adapter_.entry(id, is_long, limit_price, stop_price, qty, comment, oca_name, oca_type, qty_type);
}
void PineNativeHost::strategy_close(const std::string& id, const std::string& comment,
                                    double qty, double qty_percent, bool immediately) {
    adapter_.close(id, comment, qty, qty_percent, immediately);
}
void PineNativeHost::strategy_close(const std::string& id, const std::string& comment,
                                    double qty, double qty_percent, bool immediately, std::uint64_t token) {
    adapter_.close(id, comment, qty, qty_percent, immediately, token);
}
void PineNativeHost::strategy_close_all() { adapter_.close_all(); }
void PineNativeHost::strategy_exit(const std::string& id, const std::string& from_entry,
                                   double limit_price, double stop_price, double trail_points,
                                   double trail_offset, double trail_price, double qty_percent,
                                   const std::string& comment, double qty, const std::string& oca_name,
                                   double profit_ticks, double loss_ticks) {
    adapter_.exit(id, from_entry, limit_price, stop_price, trail_points, trail_offset,
                  trail_price, qty_percent, comment, qty, oca_name, profit_ticks, loss_ticks);
}
void PineNativeHost::strategy_exit_cancel_bracket(const std::string& id, const std::string& from_entry,
                                                  const std::string& comment) {
    adapter_.exit_cancel_bracket(id, from_entry, comment);
}
void PineNativeHost::strategy_cancel(const std::string& id) { adapter_.cancel(id); }
void PineNativeHost::strategy_cancel_all() { adapter_.cancel_all(); }
void PineNativeHost::strategy_order(const std::string& id, bool is_long, double qty,
                                    double limit_price, double stop_price,
                                    const std::string& oca_name, int oca_type) {
    adapter_.order(id, is_long, qty, limit_price, stop_price, oca_name, oca_type);
}

void PineNativeHost::scheduler_prepare_script_run(const std::vector<Bar>& bars, bool static_eligible,
                                                  int expected_script_bars) {
    prepare_script_run(bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()), static_eligible);
    source_last_bar_index_ = expected_script_bars - 1;
}
void PineNativeHost::scheduler_configure_security_evaluators() { configure_security_evaluators(); }
void PineNativeHost::scheduler_publish_source_bar(const Bar& bar, bool, bool advance_source_index) {
    current_bar_ = bar;
    if (advance_source_index) ++source_bar_index_;
    ++source_callback_count_;
    bar_index_ = source_bar_index_;
    barstate_islast_ = source_bar_index_ == source_last_bar_index_;
    on_source_bar(bar);
    // Complete one source evaluation before appending bracket legs for newly
    // pending same-id openings.  Existing legs are re-priced in-call first,
    // preserving the source roster order at the next native candidate.
    adapter_.flush_pending_entries();
    adapter_.flush_pending_bracket_legs();
}

} // namespace pineforge::source
