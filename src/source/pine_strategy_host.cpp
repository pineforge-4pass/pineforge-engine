#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include "../engine_internal.hpp"
#include "../timezone.hpp"

#include <cmath>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace pineforge {
using namespace source;

source::PineStrategyHost::PineStrategyHost(compat::pine::CapAttachment cap)
    : NativeStrategyHost(),
      adapter_(*this, cap),
      _src_series_active_(scheduler_.language()._src_series_active_),
      _src_open_(scheduler_.language()._src_open_),
      _src_high_(scheduler_.language()._src_high_),
      _src_low_(scheduler_.language()._src_low_),
      _src_close_(scheduler_.language()._src_close_),
      _src_volume_(scheduler_.language()._src_volume_),
      _src_hl2_(scheduler_.language()._src_hl2_),
      _src_hlc3_(scheduler_.language()._src_hlc3_),
      _src_ohlc4_(scheduler_.language()._src_ohlc4_),
      _src_hlcc4_(scheduler_.language()._src_hlcc4_),
      is_last_tick_(scheduler_.language().is_last_tick_) {}

std::uint64_t source::PineStrategyHost::broker_state_hash_projection() const {
    // Native run generations reject stale native handles, but they were not
    // part of the source broker state before lowering. The adapter hashes its
    // current logical request state below with those generations canonicalized.
    return broker_state_hash_from_execution_hash(0);
}

double source::PineStrategyHost::margin_liquidation_price() const {
    return compute_liquidation_price();
}

double source::PineStrategyHost::compute_liquidation_price() const {
    if (position_side_ == PositionSide::FLAT) return na<double>();
    const double point_value = syminfo_.pointvalue;
    const double quantity = position_qty_;
    if (!(quantity > 0.0) || !(point_value > 0.0)) return na<double>();
    const double direction = position_side_ == PositionSide::LONG ? 1.0 : -1.0;
    const double margin_pct = position_side_ == PositionSide::LONG
        ? config_.margin_long : config_.margin_short;
    const double denominator = (margin_pct / 100.0) - direction;
    if (std::abs(denominator) < 1e-12) return na<double>();
    const double equity_basis =
        (initial_capital_ + net_profit_sum_) / active_account_currency_fx();
    double liquidation =
        (equity_basis / (quantity * point_value) - direction * position_entry_price_)
        / denominator;
    if (syminfo_mintick_ > 0.0) {
        liquidation = position_side_ == PositionSide::SHORT
            ? std::ceil(liquidation / syminfo_mintick_) * syminfo_mintick_
            : std::floor(liquidation / syminfo_mintick_) * syminfo_mintick_;
    }
    return liquidation;
}

PineStrategyConfig source::PineStrategyHost::apply_overrides(
        PineStrategyConfig config, const StrategyOverrides& overrides) {
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

StagedConfiguration source::PineStrategyHost::staged_configuration() const {
    StagedConfiguration staged;
    staged.syminfo = syminfo_;
    staged.syminfo.mintick = syminfo_mintick_;
    staged.inputs = inputs_;
    staged.chart_timezone = chart_timezone_;
    staged.account_fx = account_currency_fx_;
    staged.account_fx_effective_from_ms = account_currency_fx_timestamps_;
    staged.account_fx_per_quote = account_currency_fx_rates_;
    if (std::isfinite(qty_step_) && qty_step_ > 0.0) staged.quantity_grid = qty_step_;
    return staged;
}

void source::PineStrategyHost::prepare_native_begin(const NativeBeginArgs& args) {
    if (args.syminfo) {
        syminfo_ = *args.syminfo;
        syminfo_mintick_ = syminfo_.mintick;
        if (std::isfinite(syminfo_.qty_step) && syminfo_.qty_step > 0.0)
            qty_step_ = syminfo_.qty_step;
    }
    if (args.inputs) inputs_ = *args.inputs;

    if (!(args.n < 2 && !args.is_stream)) {
        std::string effective_input = args.input_tf;
        if (effective_input.empty() && args.n >= 2 && args.bars != nullptr)
            effective_input = detect_timeframe(args.bars, args.n);
        const std::string effective_script = args.script_tf.empty()
            ? effective_input : args.script_tf;
        try {
            if (!effective_input.empty() && !effective_script.empty()
                && tf_ratio(effective_input, effective_script) == -2) {
                throw std::runtime_error(
                    "script timeframe must be coarser than or equal to input timeframe: requested script_tf "
                    + effective_script + " from input timeframe " + effective_input);
            }
        } catch (const std::runtime_error&) {
            throw;
        } catch (...) {
            // The native specification validator owns malformed literals.
        }
    }

    PineStrategyConfig effective = config_;
    if (args.overrides_opaque) {
        const auto* overrides = static_cast<const StrategyOverrides*>(args.overrides_opaque);
        effective = apply_overrides(effective, *overrides);
    }
    const StagedConfiguration staged = staged_configuration();
    if (!staged.account_fx_effective_from_ms.empty() && effective.calc_on_order_fills)
        throw std::logic_error(
            "timestamped account-currency FX is not supported with calc_on_order_fills");
    if (!staged.account_fx_effective_from_ms.empty() && args.bar_magnifier)
        throw std::logic_error(
            "timestamped account-currency FX is not supported with bar magnifier");

    adapter_.reset_for_run();
    adapter_.set_configuration(effective);
    adapter_.set_staged_configuration(staged);
    adapter_.set_begin_mode(args.is_stream);
    adapter_.set_margin_call_enabled(margin_call_enabled_);
    scheduler_.capture_begin(args);
    scheduler_.set_source_series_active(effective.src_series_active);
    const NativeRunSpec spec = adapter_.project(effective, staged, args);
    const auto setup = configure_native(spec);
    if (setup.status != NativeSetupStatus::Applied)
        throw std::logic_error("Pine native adapter failed to configure projected run spec");
    config_ = effective;
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::on_native_run_begin() {
    source_bar_index_ = -1;
    source_last_bar_index_ = -1;
    source_callback_count_ = 0;
    scheduler_.run_begin(*this);
}

void source::PineStrategyHost::on_native_input(
        const Bar& bar, const NativeInputContext& context) {
    scheduler_.input(bar, context, *this);
}

void source::PineStrategyHost::on_native_tick(
        const Bar& tick, const NativeTickContext& context) {
    adapter_.on_tick(tick, context);
}

void source::PineStrategyHost::on_native_bar_open(
        const Bar& bar, const NativeDecisionContext& context) {
    bar_magnifier_enabled_ = scheduler_.bar_magnifier_enabled();
    diag_magnifier_sub_bars_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sub_bars_processed) : 0;
    diag_magnifier_sample_ticks_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sample_ticks_processed) : 0;
    adapter_.on_bar_open(bar, context);
    scheduler_.bar_open(bar, context, *this);
}

void source::PineStrategyHost::on_native_bar(
        const Bar& bar, const NativeDecisionContext& context) {
    bar_magnifier_enabled_ = scheduler_.bar_magnifier_enabled();
    diag_magnifier_sub_bars_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sub_bars_processed) : 0;
    diag_magnifier_sample_ticks_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sample_ticks_processed) : 0;
    adapter_.observe_terminal_receipts();
    scheduler_.bar(bar, context, *this);
    adapter_.on_bar_close(bar, context);
    if (context.is_terminal_sub_bar
        && context.coordinate.interval_index == source_last_bar_index_) {
        scheduler_record_range_end(bar);
    }
}

void source::PineStrategyHost::on_native_applied(
        const native_order::ExecutionAppliedEvent& event,
        const NativeDecisionContext& context) {
    // The legacy source observer counted one broker fill for every committed
    // execution event.  The native consumer owns those events now; mirror the
    // count at its notification boundary so restored source tests and public
    // source-side policy reads see the same monotone value.
    ++broker_fill_event_seq_;
    adapter_.on_applied(event, context);
    if (adapter_.take_intraday_loss_relabel(event.ordinal)) {
        for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
            const std::size_t index = event.first_trade_index + i;
            if (index >= trades_.size()) continue;
            trades_[index].exit_id.clear();
            trades_[index].exit_comment = "Close Position (Max intraday Loss)";
        }
    }
    project_short_seed_report_rows(event);
    scheduler_.applied(event, context, *this);
    if (scheduler_.terminal_source_bar() || barstate_islast_) {
        const Bar terminal = scheduler_.current_script_bar()
            ? *scheduler_.current_script_bar() : current_bar_;
        scheduler_record_range_end(terminal);
    }
}

native_order::ExecutionTerms source::PineStrategyHost::resolve_execution_terms(
        const NativeExecutionTermsFacts& facts) const {
    return adapter_.resolve_terms(facts);
}

NativePrecommitVerdict source::PineStrategyHost::validate_execution_precommit(
        const NativePrecommitView& view) const {
    return adapter_.validate_precommit(view);
}

void source::PineStrategyHost::configure_pine_strategy(const PineStrategyConfig& config) {
    guard_native_mutation("configure_pine_strategy");
    config_ = config;
    adapter_.set_configuration(config_);
    scheduler_.set_source_series_active(config_.src_series_active);
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::set_strategy_override(const StrategyOverrides& overrides) {
    guard_native_mutation("set_strategy_override");
    override_ = overrides;
    config_ = apply_overrides(config_, override_);
    adapter_.set_configuration(config_);
    scheduler_.set_source_series_active(config_.src_series_active);
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::set_pine_risk_direction(int direction) {
    adapter_.set_risk_direction(direction);
}

void source::PineStrategyHost::set_pine_risk_max_cons_loss_days(int value) {
    adapter_.set_risk_max_cons_loss_days(value);
}

void source::PineStrategyHost::set_pine_risk_max_drawdown(double value, bool percent) {
    adapter_.set_risk_max_drawdown(value, percent);
}

void source::PineStrategyHost::set_pine_risk_max_intraday_loss(double value, bool percent) {
    adapter_.set_risk_max_intraday_loss(value, percent);
}

void source::PineStrategyHost::set_pine_risk_max_intraday_filled_orders(int limit) {
    adapter_.cap = limit;
}

void source::PineStrategyHost::set_pine_risk_max_position_size(double value) {
    adapter_.set_risk_max_position_size(value);
}

int source::PineStrategyHost::pine_bar_index() const {
    return source_bar_index_ + scheduler_.bar_index_offset();
}

int source::PineStrategyHost::pine_last_bar_index() const {
    return source_last_bar_index_ + scheduler_.bar_index_offset();
}

bool source::PineStrategyHost::is_first_tick() const noexcept {
    return scheduler_.is_first_tick();
}

bool source::PineStrategyHost::is_last_tick() const noexcept {
    return scheduler_.is_last_tick();
}

compat::pine::CapClock source::PineStrategyHost::fixture_cap_clock() const {
    NativeDecisionContext context;
    if (const auto point = current_execution_point()) {
        context = point->decision;
    } else {
        context.coordinate.interval_index = bar_index_;
        context.sub_bar_open_ms = current_bar_.timestamp;
        context.script_bar_open_ms = current_bar_.timestamp;
    }
    const BarTime time = fixture_chart_time(context.sub_bar_open_ms);
    return {context.sub_bar_open_ms,
            syminfo_.session.empty() ? "24x7" : syminfo_.session,
            syminfo_.timezone.empty() ? "UTC" : syminfo_.timezone,
            time.dayofmonth, time.month};
}

compat::pine::Calculation source::PineStrategyHost::fixture_cap_calculation() const {
    NativeDecisionContext context;
    if (const auto point = current_execution_point()) {
        context = point->decision;
    } else {
        context.coordinate.interval_index = bar_index_;
        context.sub_bar_open_ms = current_bar_.timestamp;
        context.script_bar_open_ms = current_bar_.timestamp;
    }
    return adapter_.cap_calculation(context);
}

bool source::PineStrategyHost::fixture_intraday_cap_latched() {
    return adapter_.cap.placement(fixture_cap_clock())
        == compat::pine::Placement::Deny;
}

source::PineStrategyHost::BarTime source::PineStrategyHost::fixture_chart_time(
        std::int64_t timestamp_ms) const {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm tm{};
    const auto utc = [&]() {
        return ::gmtime_r(&seconds, &tm) != nullptr;
    };
    if (chart_timezone_.empty() || chart_timezone_ == "UTC"
        || chart_timezone_ == "Etc/UTC") {
        (void)utc();
    } else {
        try {
            pine_tz::ScopedTimezone guard(chart_timezone_);
            if (::localtime_r(&seconds, &tm) == nullptr) (void)utc();
        } catch (...) {
            (void)utc();
        }
    }
    BarTime result;
    result.year = tm.tm_year + 1900;
    result.month = tm.tm_mon + 1;
    result.dayofmonth = tm.tm_mday;
    result.hour = tm.tm_hour;
    result.minute = tm.tm_min;
    result.second = tm.tm_sec;
    result.dayofweek = tm.tm_wday + 1;
    result.weekofyear = (tm.tm_yday + 7 - ((tm.tm_wday + 6) % 7)) / 7;
    return result;
}

std::uint64_t source::PineStrategyHost::fixture_applied_receipt_count() const {
    std::uint64_t count = 0;
    for (const auto& event : native_events(0)) {
        if (!event.command
            || !std::holds_alternative<native_order::ExecutionAppliedEvent>(*event.command)) {
            continue;
        }
        ++count;
    }
    return count;
}

bool source::PineStrategyHost::history_advances_new_bar() const noexcept {
    return scheduler_.history_advances_new_bar();
}

bool source::PineStrategyHost::security_series_slot_is_new(int slot) const noexcept {
    return BacktestEngine::security_series_slot_is_new(slot);
}

double source::PineStrategyHost::prev_chart_close() const {
    return scheduler_.previous_chart_close();
}

int source::PineStrategyHost::last_bar_dual_entry_path() const {
    return adapter_.pending_intent_view().last_bar_dual_entry_path();
}

double source::PineStrategyHost::signed_position_size() const {
    return scheduler_.script_position_view(bar_index_, position_side_, position_qty_);
}

void source::PineStrategyHost::freeze_script_position_view() {
    scheduler_.freeze_script_position_view(
        bar_index_, position_side_, position_qty_, pyramid_entries_);
}

void source::PineStrategyHost::clear_script_position_view() {
    scheduler_.clear_script_position_view();
}

const Series<double>& source::PineStrategyHost::source_series(const std::string& key) const {
    return scheduler_.source_series(key);
}

const Series<double>& source::PineStrategyHost::source_input_series(
        const std::string& key, const Series<double>& fallback) const {
    const auto found = inputs_.find(key);
    if (found == inputs_.end() || found->second.empty()) return fallback;
    try {
        return scheduler_.source_series(found->second);
    } catch (const std::invalid_argument&) {
        return fallback;
    }
}

double source::PineStrategyHost::live_position_size() const {
    return physical_position().signed_units;
}

int source::PineStrategyHost::pending_order_count() const {
    return pending_intent_view().size();
}

MarketAdmissionJournal& source::PineStrategyHost::market_admission_journal() {
    return adapter_.admission_journal;
}

const MarketAdmissionJournal& source::PineStrategyHost::market_admission_journal() const {
    return adapter_.admission_journal;
}

std::vector<admission::Field> source::PineStrategyHost::market_admission_fields() const {
    std::vector<admission::Field> fields;
    adapter_.admission_journal.reflect("journal", [&](const admission::Field& field) {
        fields.push_back(field);
    });
    return fields;
}

int source::PineStrategyHost::probe_fill_qty(
        int index, double fill_price, double* qty, int* close_only, int* partition) const {
    return pending_intent_view().probe_fill_qty(index, fill_price, qty, close_only, partition);
}

int source::PineStrategyHost::pending_order_level_resolved(int index) const {
    return pending_intent_view().level_resolved(index);
}

int source::PineStrategyHost::pending_order_effective_levels(
        int index, double* stop, double* limit, double* trail_activation) const {
    return pending_intent_view().effective_levels(index, stop, limit, trail_activation);
}

const PendingIntentView& source::PineStrategyHost::pending_intent_view() const noexcept {
    return adapter_.pending_intent_view();
}

int source::PineStrategyHost::short_seed_collision_role_v1(
        native_order::RequestHandle handle) const noexcept {
    return adapter_.short_seed_collision_role_v1(std::move(handle));
}

void source::PineStrategyHost::enable_pine_intraday_cap() {
    adapter_.enable_intraday_cap();
}

void source::PineStrategyHost::attach_pine_execution_adapter() {
    adapter_.attach_execution_adapter();
    // Generated constructors attach the source execution bridge before their
    // risk statements and metadata arrive.  The intraday-cap configuration is
    // part of that same source-policy attachment; leaving it detached makes a
    // later max_intraday_filled_orders declaration silently inert.
    adapter_.enable_intraday_cap();
}

void source::PineStrategyHost::set_syminfo_metadata(
        const std::string& key, double value) {
    BacktestEngine::set_syminfo_metadata(key, value);
    if (key == "bar_index_offset") {
        scheduler_.set_bar_index_offset(std::isfinite(value)
            ? static_cast<int>(std::llround(value)) : 0);
    }
    if (key == "security_range_start_na_warmup") {
        if (std::isfinite(value) && value > 0.0) {
            security_range_start_na_warmup_ = true;
            security_range_start_ms_ = static_cast<int64_t>(std::llround(value));
        } else {
            security_range_start_na_warmup_ = false;
            security_range_start_ms_ = 0;
        }
    }
    if (key == "chart_ema_na_warmup")
        chart_ema_na_warmup_ = std::isfinite(value) && value > 0.0;
    if (key == "historical_security_lookahead_projection")
        historical_security_lookahead_projection_ = std::isfinite(value) && value > 0.0;
    if (key == "margin_long" && config_.margin_long == 100.0)
        config_.margin_long = (std::isfinite(value) && value > 0.0) ? value : 100.0;
    if (key == "margin_short" && config_.margin_short == 100.0)
        config_.margin_short = (std::isfinite(value) && value > 0.0) ? value : 100.0;
    adapter_.set_configuration(config_);
    adapter_.priority.metadata(key, value);
    adapter_.cap.metadata(key, value);
}

int source::PineStrategyHost::observe_last_bar_dual_entry_path_v1() const {
    return pending_intent_view().last_bar_dual_entry_path();
}

int source::PineStrategyHost::observe_pending_count_v1() const {
    return pending_intent_view().size();
}

int source::PineStrategyHost::observe_pending_copy_v1(
        int index, pf_pending_order_v1_t* out) const {
    return pending_intent_view().copy_v1(index, out);
}

int source::PineStrategyHost::observe_probe_fill_qty(
        int index, double fill_price, double* qty, int* close_only, int* partition) const {
    return pending_intent_view().probe_fill_qty(index, fill_price, qty, close_only, partition);
}

int source::PineStrategyHost::observe_pending_level_resolved(int index) const {
    return pending_intent_view().level_resolved(index);
}

int source::PineStrategyHost::observe_pending_effective_levels(
        int index, double* stop, double* limit, double* trail_activation) const {
    return pending_intent_view().effective_levels(index, stop, limit, trail_activation);
}

double source::PineStrategyHost::observe_trail_best_price_v1() const {
    return adapter_.pending_intent_view().trail_best_price();
}

const std::vector<source::PineStrategyHost::FixtureIntentRow>&
source::PineStrategyHost::source_pending_view() const {
    source_pending_view_cache_.clear();
    source_pending_view_cache_.reserve(adapter_.pending_same_bar_commands_.size()
        + adapter_.pending_entries_.size() + adapter_.pending_bracket_legs_.size()
        + adapter_.pending_coof_requests_.size()
        + adapter_.source_shadow_pending_.size() + adapter_.live_handles_.size());
    const auto append = [&](const PlacementSnapshot& snapshot, const std::string& label) {
        FixtureIntentKind type = FixtureIntentKind::MARKET;
        switch (snapshot.family) {
        case PineOrderFamily::Close:
        case PineOrderFamily::CloseAll:
        case PineOrderFamily::ExitLimit:
        case PineOrderFamily::ExitStop:
        case PineOrderFamily::ExitTrail:
        case PineOrderFamily::Margin:
        case PineOrderFamily::Risk:
            type = FixtureIntentKind::EXIT;
            break;
        case PineOrderFamily::Order:
            type = FixtureIntentKind::RAW_ORDER;
            break;
        case PineOrderFamily::Entry:
            break;
        }
        FixtureIntentRow row;
        row.id = snapshot.frozen_market_targeted_close ? label : snapshot.source_id;
        row.type = type;
        const bool default_stop = snapshot.family == PineOrderFamily::Entry
            && !std::isfinite(snapshot.exit_levels.limit)
            && std::isfinite(snapshot.exit_levels.stop)
            && std::isnan(snapshot.requested_qty)
            && config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
            && config_.default_qty_value <= 100.0;
        const double absent = std::numeric_limits<double>::quiet_NaN();
        row.default_stop_placement_qty = default_stop ? snapshot.sizing.frozen_units : absent;
        row.default_stop_sizing_price = default_stop ? snapshot.sizing.price : absent;
        row.frozen_market_own_units = snapshot.frozen_market_own_units;
        row.frozen_market_transaction_units = snapshot.frozen_market_transaction_units;
        row.from_entry = snapshot.from_entry;
        row.is_long = snapshot.is_long;
        row.qty = snapshot.family == PineOrderFamily::Close
            ? snapshot.requested_qty
            : (std::isfinite(snapshot.projection_remaining_qty)
                ? snapshot.projection_remaining_qty : snapshot.requested_qty);
        row.qty_percent = snapshot.qty_percent;
        row.created_bar = snapshot.projection_created_bar;
        row.created_seq = static_cast<std::int64_t>(snapshot.source_sequence);
        row.paired_flat_market_peer_seq = 0;
        row.paired_flat_market_transaction_qty = std::numeric_limits<double>::quiet_NaN();
        row.frozen_default_qty = default_stop ? absent : snapshot.sizing.frozen_units;
        row.default_stop_placement_equity = default_stop
            ? snapshot.projection_default_stop_equity : absent;
        row.default_stop_placement_signal_close = default_stop
            ? snapshot.projection_default_stop_signal_close : absent;
        row.affordability_placement_equity = snapshot.projection_affordability_equity;
        if (snapshot.family == PineOrderFamily::Entry
            && snapshot.frozen_market_instruction
            && std::isfinite(snapshot.requested_qty)) {
            auto observation = std::make_shared<admission::CommandObservation>();
            observation->command = snapshot.command_ordinal;
            observation->kind = admission::CommandKind::Entry;
            observation->birth = snapshot.birth.cause() == OrderBirthCause::Unattributed
                ? OrderBirth::chart_evaluation(source_bar_index_, current_bar_.timestamp)
                : snapshot.birth;
            observation->id = snapshot.source_id;
            observation->requested_quantity = snapshot.requested_qty;
            observation->quantity_type = snapshot.qty_type;
            observation->buy = snapshot.is_long;
            observation->prices = {snapshot.exit_levels.limit, snapshot.exit_levels.stop};
            observation->oca_name = snapshot.oca_name;
            observation->oca_type = snapshot.oca_type;
            auto& configuration = observation->configuration;
            configuration.process_on_close = config_.process_orders_on_close;
            configuration.calc_on_fills = config_.calc_on_order_fills;
            configuration.slippage = config_.slippage;
            configuration.pyramiding = config_.pyramiding;
            configuration.default_quantity_type = config_.default_qty_type;
            configuration.default_quantity_value = config_.default_qty_value;
            configuration.long_margin = config_.margin_long;
            configuration.short_margin = config_.margin_short;
            configuration.commission_value = config_.commission_value;
            configuration.commission_type = config_.commission_type;
            configuration.pointvalue = staged_configuration().syminfo.pointvalue;
            configuration.fx = snapshot.sizing.fx;
            configuration.quantity_step = staged_configuration().quantity_grid
                ? *staged_configuration().quantity_grid : 0.0;
            configuration.mintick = staged_configuration().syminfo.mintick;
            observation->bar = source_bar_index_;
            observation->placement_side = static_cast<int>(PositionSide::FLAT);
            observation->placement_cycle = snapshot.placement_cycle;
            observation->held_quantity = 0.0;
            observation->held_entries = 0;
            observation->realized_equity = snapshot.sizing.equity;
            observation->placement_equity = snapshot.sizing.equity;
            observation->signal_close = snapshot.sizing.price;
            observation->quantized_fixed_quantity =
                snapshot.frozen_market_own_units;
            observation->original_sizing = admission::SizingObservation{
                snapshot.requested_qty, snapshot.sizing.equity,
                snapshot.sizing.price, snapshot.sizing.mark, snapshot.sizing.fx};
            row.market_admission.bind(std::move(observation));
        }
        source_pending_view_cache_.push_back(std::move(row));
    };
    for (const auto& command : adapter_.pending_same_bar_commands_)
        append(command.snapshot, command.request.label);
    for (const auto& entry : adapter_.pending_entries_)
        append(entry.snapshot, entry.request.label);
    for (const auto& leg : adapter_.pending_bracket_legs_)
        append(leg.snapshot, leg.request.label);
    for (const auto& pending : adapter_.pending_coof_requests_)
        append(pending.snapshot, pending.request.label);
    for (const auto& shadow : adapter_.source_shadow_pending_)
        append(shadow.snapshot, shadow.label);
    for (const auto& handle : adapter_.live_handles_) {
        const auto found = adapter_.placement_.find(handle.incarnation);
        if (found != adapter_.placement_.end()) append(found->second, found->second.source_id);
    }
    return source_pending_view_cache_;
}

void source::PineStrategyHost::source_stream_entry_comment(
        const PyramidEntry&, std::string&) const {}

void source::PineStrategyHost::project_short_seed_report_rows(
        const native_order::ExecutionAppliedEvent& event) {
    const ShortSeedPlan plan = adapter_.short_seed_;
    if (!plan.report_swap_pending || event.closed_trade_count == 0
        || event.handle() == plan.final_short) {
        return;
    }
    std::optional<PlacementSnapshot> placement_snapshot;
    if (const auto placement = adapter_.placement_.find(event.handle().incarnation);
        placement != adapter_.placement_.end()) {
        placement_snapshot = placement->second;
    }
    if (!placement_snapshot || placement_snapshot->family != PineOrderFamily::Close
        || placement_snapshot->from_entry != plan.seed_id) {
        return;
    }
    for (auto& trade : trades_) {
        if (trade.entry_incarnation == plan.materialize_long.incarnation
            && trade.entry_id == plan.materialize_label) {
            trade.entry_incarnation = plan.final_short.incarnation;
        }
    }
    const std::size_t begin = event.first_trade_index;
    const std::size_t end = begin + event.closed_trade_count;
    for (std::size_t index = begin; index < end && index < trades_.size(); ++index) {
        if (trades_[index].entry_incarnation == plan.final_short.incarnation
            && trades_[index].entry_id == plan.final_short_id) {
            trades_[index].entry_incarnation = plan.materialize_long.incarnation;
        }
    }
    adapter_.short_seed_.report_swap_pending = false;
}

void source::PineStrategyHost::scheduler_prepare_script_run(
        const std::vector<Bar>& bars, bool static_eligible, int expected_script_bars) {
    if (const auto state = native_state(); state.spec) {
        input_tf_ = state.spec->timeframe_undetected ? "" : state.spec->input_tf;
        script_tf_ = state.spec->timeframe_undetected ? "" : state.spec->script_tf;
        script_tf_seconds_ = tf_to_seconds(script_tf_);
    }
    prepare_script_run(bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()),
                       static_eligible);
    source_last_bar_index_ = expected_script_bars - 1;
}

void source::PineStrategyHost::scheduler_configure_security_evaluators() {
    configure_security_evaluators();
}

bool source::PineStrategyHost::scheduler_uses_aux_security_feed() const noexcept {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    return aux_security_feed_enabled();
#else
    return false;
#endif
}

void source::PineStrategyHost::scheduler_prepare_security_sequence(
        const std::vector<Bar>& bars) {
    security_input_tf_ = input_tf_;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) security_input_tf_ = aux_security_input_tf_;
#endif
    validate_security_timeframes(security_input_tf_);
    security_first_chart_bar_ms_ = bars.empty() ? 0 : bars.front().timestamp;
    init_security_eval_states_for_run(security_input_tf_);
    prepare_native_security_feeds(
        bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()));
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) {
        prepare_aux_security_chart_ranges(
            bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()), script_tf_);
    }
#endif
    prepare_historical_security_lookahead_projections(
        bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()), input_tf_);
    prepare_chart_day_partition(
        bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()));
}

bool source::PineStrategyHost::scheduler_feed_security_input(
        const Bar& bar, std::int64_t next_input_ms, bool calling_bar_complete,
        bool defer_boundary_gate) {
    security_next_input_ms_ = next_input_ms;
    security_calling_close_ms_ = 0;
    bool deferred = false;
    for (auto& state : security_eval_states_) {
        if (defer_boundary_gate && state.publish_gate_tf_seconds > 0) {
            deferred = true;
            continue;
        }
        feed_security_eval_state(state, bar, calling_bar_complete);
    }
    return deferred;
}

void source::PineStrategyHost::scheduler_publish_security_boundary() {
    for (auto& state : security_eval_states_) {
        if (state.publish_gate_tf_seconds > 0)
            publish_security_eval_state_at_calling_boundary(state);
    }
}

void source::PineStrategyHost::scheduler_feed_deferred_security_input(
        const Bar& bar, std::int64_t next_input_ms) {
    security_next_input_ms_ = next_input_ms;
    security_calling_close_ms_ = 0;
    for (auto& state : security_eval_states_) {
        if (state.publish_gate_tf_seconds > 0)
            feed_security_eval_state(state, bar, false);
    }
}

void source::PineStrategyHost::scheduler_feed_aux_security(int chart_index) {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) feed_aux_security_for_chart_bar(chart_index);
#else
    (void)chart_index;
#endif
}

void source::PineStrategyHost::scheduler_feed_deferred_aux_security(int chart_index) {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) feed_deferred_aux_security_for_chart_bar(chart_index);
#else
    (void)chart_index;
#endif
}

void source::PineStrategyHost::scheduler_finish_security_sequence() {
    clear_historical_security_lookahead_projections();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    clear_aux_security_chart_ranges();
#endif
}

void source::PineStrategyHost::scheduler_record_range_end(const Bar& terminal_bar) {
    range_end_trades_.clear();
    if (stream_warmup_mode_ || position_side_ == PositionSide::FLAT || equity_curve_.empty()
        || !std::isfinite(terminal_bar.close)) return;
    const Bar saved = current_bar_;
    current_bar_ = terminal_bar;
    const bool was_long = position_side_ == PositionSide::LONG;
    const double fill_price = bar_fill_price(current_bar_.close);
    const auto saved_timestamp = current_bar_.timestamp;
    current_bar_.timestamp = equity_curve_.back().time_ms;
    double range_end_pnl = 0.0;
    for (const auto& lot : pyramid_entries_) {
        execution::PhysicalExecutionContext context;
        context.effective_time_ms = current_bar_.timestamp;
        context.interval_index = bar_index_;
        context.preceding_exit_path_prefix = fold_exit_path_extremes_;
        if (!std::isnan(fold_exit_trail_peak_))
            context.preceding_exit_trail_peak = fold_exit_trail_peak_;
        Trade row = build_close_trade_with_costs(
            lot, lot.qty, fill_price, was_long,
            allocated_entry_commission(lot, lot.qty), calc_commission(fill_price, lot.qty),
            context);
        row.open_at_end = true;
        range_end_pnl += row.pnl;
        range_end_trades_.push_back(std::move(row));
    }
    current_bar_.timestamp = saved_timestamp;
    auto& last = equity_curve_.back();
    last.open_profit = 0.0;
    last.equity = initial_capital_ + net_profit_sum_ + range_end_pnl;
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;
    max_drawdown_ = 0.0;
    max_runup_ = 0.0;
    for (const auto& point : equity_curve_) fold_equity_extreme(point.equity);
    current_bar_ = saved;
}

void source::PineStrategyHost::scheduler_publish_source_bar(
        const Bar& bar, bool, bool advance_source_index) {
    current_bar_ = bar;
    if (advance_source_index) ++source_bar_index_;
    ++source_callback_count_;
    bar_index_ = source_bar_index_;
    barstate_islast_ = source_bar_index_ == source_last_bar_index_;
    NativeDayPartitionScope chart_day_partition(
        chart_day_partition_.empty() ? nullptr : &chart_day_partition_);
    // A named-entry cancellation token has source-evaluation scope.  Clear a
    // prior callback before publishing receipts and entering this body.
    adapter_.begin_source_evaluation();
    // Publish terminal and group-adjustment receipts before the source body
    // reads its public pending projection at this decision boundary.
    adapter_.observe_terminal_receipts();
    position_entry_count_ = physical_position().signed_units == 0.0
        ? 0 : adapter_.source_entry_slot_count();
    on_source_bar(bar);
    // Handwritten/source-generated callbacks historically read and could
    // update the live Pine configuration fields directly.  Keep the adapter's
    // source policy view synchronized at the callback boundary; the generic
    // NativeRunSpec remains immutable for the run.
    adapter_.set_configuration(config_);
    adapter_.flush_pending_entries();
    adapter_.flush_pending_bracket_legs();
    if (advance_source_index) {
        update_equity_extremes();
        record_equity_point(bar.timestamp);
        prev_bar_timestamp_ = bar.timestamp;
    }
}

void source::PineStrategyHost::scheduler_record_broker_hash() {
    if (broker_state_hash_recording_) broker_state_hashes_.push_back(broker_state_hash());
}

} // namespace pineforge
