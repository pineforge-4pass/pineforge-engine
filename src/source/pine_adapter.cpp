#include <pineforge/source/pine_adapter.hpp>

#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace pineforge::source {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

std::uint64_t fnv_append(std::uint64_t value, const void* bytes, std::size_t size) noexcept {
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < size; ++i) {
        value ^= p[i];
        value *= 1099511628211ULL;
    }
    return value;
}

std::uint64_t fnv_string(std::string_view value) noexcept {
    return fnv_append(1469598103934665603ULL, value.data(), value.size());
}

std::uint64_t source_key(const SourceId& left, const SourceId& right) noexcept {
    std::uint64_t value = fnv_string(left);
    const char separator = '\0';
    value = fnv_append(value, &separator, sizeof(separator));
    return fnv_append(value, right.data(), right.size());
}

double nearest_tick(double value, double tick) noexcept {
    if (!std::isfinite(value) || !finite_positive(tick)) return value;
    return std::round(value / tick) * tick;
}

NativeFeeKind fee_kind_for(int commission_type) noexcept {
    switch (static_cast<CommissionType>(commission_type)) {
    case CommissionType::CASH_PER_CONTRACT: return NativeFeeKind::CashPerUnit;
    case CommissionType::CASH_PER_ORDER: return NativeFeeKind::CashPerExecution;
    case CommissionType::PERCENT:
    default: return NativeFeeKind::Percent;
    }
}

NativeOpenDirections directions_for(int direction) noexcept {
    if (direction > 0) return NativeOpenDirections::Long;
    if (direction < 0) return NativeOpenDirections::Short;
    return NativeOpenDirections::Both;
}

} // namespace

PineExecutionAdapter::PineExecutionAdapter(compat::pine::CapAttachment attachment)
    : cap(attachment) {
    pending_view_.owner_ = this;
}

PineExecutionAdapter::PineExecutionAdapter(NativeStrategyHost& host,
                                           compat::pine::CapAttachment attachment)
    : PineExecutionAdapter(attachment) {
    bind(host);
}

void PineExecutionAdapter::bind(NativeStrategyHost& host) noexcept { host_ = &host; }

NativeStrategyHost& PineExecutionAdapter::require_host() const {
    if (!host_) throw std::logic_error("Pine execution adapter is not bound to a native host");
    return *host_;
}

void PineExecutionAdapter::reset_for_run() {
    cohorts_by_id_.clear();
    placement_.clear();
    live_by_source_key_.clear();
    bracket_families_.clear();
    live_handles_.clear();
    first_open_newborns_.clear();
    pending_view_handles_.clear();
    day_ledger_ = {};
    short_seed_ = {};
    last_bar_dual_entry_path_ = 0;
    source_sequence_ = 0;
    cap.reset_run();
    refresh_pending_view();
}

void PineExecutionAdapter::set_configuration(const PineStrategyConfig& config) noexcept { config_ = config; }
void PineExecutionAdapter::set_staged_configuration(const StagedConfiguration& staged) { staged_ = staged; }

NativeRunSpec PineExecutionAdapter::project(const PineStrategyConfig& config,
                                             const StagedConfiguration& staged,
                                             const NativeBeginArgs& args) const {
    NativeRunSpec spec;
    if (run_counter_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Pine native run counter exhausted");
    }
    const std::string timezone = staged.syminfo.timezone.empty() ? "UTC" : staged.syminfo.timezone;
    const std::string session = staged.syminfo.session.empty() ? "24x7" : staged.syminfo.session;
    // Bind a stable identity only to staged session/timezone facts; labels and
    // source ids never cross this generic identity boundary.
    spec.identity = {session + "@" + timezone, ++run_counter_};

    // A11: sub-two-bar public starts retain the explicit undetected state and
    // intentionally leave both string fields empty.
    spec.timeframe_undetected = args.n < 2;
    if (!spec.timeframe_undetected) {
        std::string effective_input = args.input_tf;
        if (effective_input.empty() && args.n >= 2 && args.bars != nullptr) {
            effective_input = detect_timeframe(args.bars, args.n);
        }
        spec.input_tf = std::move(effective_input);
        spec.script_tf = args.script_tf.empty() ? spec.input_tf : args.script_tf;
    }
    spec.ticker = staged.syminfo.ticker;
    spec.tickerid = staged.syminfo.tickerid;
    spec.type = staged.syminfo.type;
    spec.currency = staged.syminfo.currency;
    spec.basecurrency = staged.syminfo.basecurrency;
    spec.description = staged.syminfo.description;
    spec.volumetype = staged.syminfo.volumetype;
    spec.timezone = timezone;
    spec.session = session;
    spec.chart_timezone = staged.chart_timezone;
    spec.initial_capital = config.initial_capital;
    spec.point_value = staged.syminfo.pointvalue;
    spec.account_fx = staged.account_fx;
    spec.price_tick = staged.syminfo.mintick;
    spec.slippage_ticks = config.slippage < 0 ? 0U : static_cast<std::uint32_t>(config.slippage);
    spec.fee_kind = fee_kind_for(config.commission_type);
    spec.fee_value = config.commission_value;
    spec.quantity_grid = staged.quantity_grid;
    spec.close_execution = config.process_orders_on_close
        ? NativeCloseExecution::AfterCalculation : NativeCloseExecution::NextEligiblePoint;
    if (config.pyramiding > 0) spec.max_open_lots = static_cast<std::uint64_t>(config.pyramiding);
    spec.allowed_open_directions = directions_for(risk_.direction);
    const double margin = std::min(config.margin_long, config.margin_short);
    if (finite_positive(margin)) spec.initial_margin_fraction = margin / 100.0;
    if (args.bar_magnifier) {
        if (spec.timeframe_undetected) {
            throw std::logic_error("undetected timeframe cannot form an intrabar path");
        }
        IntrabarPath::lower_tf path;
        if (args.bars && args.n > 0) path.bars.assign(args.bars, args.bars + args.n);
        path.tf = spec.input_tf;
        path.samples = args.magnifier_samples;
        path.distribution = args.magnifier_distribution;
        path.volume_weighted = args.magnifier_volume_weighted;
        path.volume_weighted_min_samples = args.magnifier_volume_weighted_min_samples;
        path.volume_weighted_max_samples = args.magnifier_volume_weighted_max_samples;
        spec.intrabar.value = std::move(path);
    }
    const auto validation = validate_native_run_spec(spec);
    if (!validation) {
        throw std::logic_error("Pine adapter produced invalid native run spec field "
                               + std::to_string(static_cast<unsigned>(validation.field)));
    }
    return spec;
}

std::uint64_t PineExecutionAdapter::key_for(const SourceId& left, const SourceId& right) const noexcept {
    return source_key(left, right);
}

native_order::CohortHandle PineExecutionAdapter::cohort_for(const SourceId& id) {
    auto found = cohorts_by_id_.find(id);
    if (found != cohorts_by_id_.end()) return found->second.handle;
    CohortFacts facts;
    facts.handle = require_host().cohort_open();
    if (facts.handle.value == 0) throw std::logic_error("native cohort allocation refused");
    const auto result = facts.handle;
    cohorts_by_id_.emplace(id, std::move(facts));
    return result;
}

PineSizingSnapshot PineExecutionAdapter::sizing_snapshot() const {
    PineSizingSnapshot snapshot;
    const auto& host = require_host();
    if (const auto point = host.current_execution_point()) {
        snapshot.price = point->price;
        snapshot.mark = point->price;
        snapshot.equity = host.native_marked_equity(point->price);
    }
    snapshot.fx = staged_.account_fx;
    return snapshot;
}

native_order::Trigger PineExecutionAdapter::trigger_for(double limit_price, double stop_price,
                                                         double trail_offset, double trail_price) const {
    if (finite_positive(trail_offset)) {
        return native_order::Trail{trail_offset,
            finite_positive(trail_price) ? std::optional<double>{trail_price} : std::nullopt};
    }
    if (finite_positive(limit_price) && finite_positive(stop_price))
        return native_order::StopLimit{stop_price, limit_price};
    if (finite_positive(limit_price)) return native_order::Limit{limit_price};
    if (finite_positive(stop_price)) return native_order::Stop{stop_price};
    return native_order::Market{};
}

native_order::Group PineExecutionAdapter::group_for(const std::string& name, int type) const {
    if (name.empty()) return native_order::NoGroup{};
    const auto group = fnv_string(name);
    return native_order::Member{group == 0 ? 1 : group, 0,
        type == 1 ? native_order::GroupEffect::Cancel : native_order::GroupEffect::Reduce};
}

void PineExecutionAdapter::remember(const native_order::RequestHandle& handle,
                                    PlacementSnapshot snapshot) {
    placement_[handle.incarnation] = std::move(snapshot);
    if (std::find(live_handles_.begin(), live_handles_.end(), handle) == live_handles_.end())
        live_handles_.push_back(handle);
    refresh_pending_view();
}

void PineExecutionAdapter::retire(const native_order::RequestHandle& handle) noexcept {
    live_handles_.erase(std::remove(live_handles_.begin(), live_handles_.end(), handle),
                        live_handles_.end());
    first_open_newborns_.erase(std::remove(first_open_newborns_.begin(),
        first_open_newborns_.end(), handle), first_open_newborns_.end());
    for (auto it = live_by_source_key_.begin(); it != live_by_source_key_.end();) {
        if (it->second == handle) it = live_by_source_key_.erase(it); else ++it;
    }
    refresh_pending_view();
}

std::optional<native_order::RequestHandle> PineExecutionAdapter::submit_or_replace(
        native_order::Request request, PlacementSnapshot snapshot, bool opening,
        const SourceId& replacement_key) {
    auto& host = require_host();
    const auto key = replacement_key.empty() ? 0 : key_for(replacement_key);
    std::optional<native_order::RequestHandle> accepted;
    if (key != 0) {
        const auto existing = live_by_source_key_.find(key);
        if (existing != live_by_source_key_.end()) {
            const auto result = host.replace(existing->second, request);
            if (result.status == native_order::ReplaceStatus::Replaced && result.successor) {
                retire(existing->second);
                accepted = *result.successor;
            }
        }
    }
    if (!accepted) {
        const auto result = host.submit(request);
        if (result.status != native_order::SubmitStatus::Accepted || !result.handle) return std::nullopt;
        accepted = *result.handle;
    }
    snapshot.opening = opening;
    snapshot.source_sequence = ++source_sequence_;
    remember(*accepted, std::move(snapshot));
    if (key != 0) live_by_source_key_[key] = *accepted;
    if (opening) {
        const auto source = placement_.at(accepted->incarnation).source_id;
        const auto cohort = cohort_for(source);
        host.cohort_add(cohort, *accepted);
        cohorts_by_id_.at(source).origins.push_back(*accepted);
    }
    if (config_.calc_on_order_fills && std::holds_alternative<native_order::Market>(request.trigger)
        && std::holds_alternative<native_order::ImmediateRemaining>(request.capacity)
        && host.current_execution_point()) {
        first_open_newborns_.push_back(*accepted);
    }
    return accepted;
}

std::vector<native_order::RequestHandle> PineExecutionAdapter::openings_for(const SourceId& id) const {
    const auto found = cohorts_by_id_.find(id);
    return found == cohorts_by_id_.end() ? std::vector<native_order::RequestHandle>{}
                                         : found->second.opened;
}

native_order::Owner PineExecutionAdapter::owner_for_close(const SourceId& id, bool dynamic) const {
    const auto found = cohorts_by_id_.find(id);
    if (dynamic || found == cohorts_by_id_.end()) {
        if (found == cohorts_by_id_.end())
            return native_order::BindCohort{const_cast<PineExecutionAdapter*>(this)->cohort_for(id)};
        return native_order::BindCohort{found->second.handle};
    }
    return native_order::BindOpenings{found->second.opened, found->second.cycle};
}

void PineExecutionAdapter::entry(const SourceId& id, bool is_long, double limit_price,
                                 double stop_price, double qty, const std::string& comment,
                                 const std::string& oca_name, int oca_type, int qty_type) {
    native_order::Request request;
    const bool default_sized = std::isnan(qty);
    const double signed_target = is_long ? qty : -qty;
    const double current = require_host().physical_position().signed_units;
    const bool reverses = !default_sized && current != 0.0
        && ((current > 0.0) != (signed_target > 0.0));
    if (default_sized) {
        request.intent = native_order::HostSized{native_order::HostSizedKind::Open,
            is_long ? native_order::Side::Long : native_order::Side::Short};
    } else if (reverses) {
        request.intent = native_order::ReverseTo{signed_target};
    } else {
        request.intent = native_order::Transact{signed_target};
    }
    request.label = id; request.comment = comment;
    request.trigger = trigger_for(limit_price, stop_price, kNaN, kNaN);
    request.group = group_for(oca_name, oca_type);
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Entry; snapshot.source_id = id; snapshot.comment = comment;
    snapshot.oca_name = oca_name; snapshot.oca_type = oca_type; snapshot.qty_type = qty_type;
    snapshot.requested_qty = qty; snapshot.is_long = is_long; snapshot.deferred_cohort = default_sized;
    snapshot.reverse_to = reverses; snapshot.sizing = sizing_snapshot();
    if (default_sized && finite_positive(snapshot.sizing.price)) {
        if (config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
            && finite_positive(snapshot.sizing.equity)) {
            snapshot.sizing.frozen_units = config_.default_qty_value / 100.0
                * snapshot.sizing.equity / snapshot.sizing.price;
        } else if (config_.default_qty_type == static_cast<int>(QtyType::CASH)) {
            snapshot.sizing.frozen_units = config_.default_qty_value / snapshot.sizing.price;
        }
        snapshot.sizing.at_fill = config_.calc_on_order_fills;
    }
    submit_or_replace(std::move(request), std::move(snapshot), true, id);
}

void PineExecutionAdapter::close(const SourceId& id, const std::string& comment, double qty,
                                 double qty_percent, bool immediately, std::uint64_t) {
    const auto openings = openings_for(id);
    // P-DA3: strategy.close against an empty cohort is dropped at the command.
    if (openings.empty()) return;
    const bool dynamic = std::isnan(qty);
    native_order::Request request;
    request.intent = dynamic
        ? native_order::OrderIntent{native_order::HostSized{native_order::HostSizedKind::Close, std::nullopt}}
        : native_order::OrderIntent{native_order::Reduce{native_order::ExplicitUnits{qty}}};
    request.label = id; request.comment = comment; request.owner = owner_for_close(id, dynamic);
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Close; snapshot.source_id = id; snapshot.from_entry = id;
    snapshot.comment = comment; snapshot.requested_qty = qty; snapshot.qty_percent = qty_percent;
    snapshot.immediately = immediately; snapshot.deferred_cohort = dynamic; snapshot.sizing = sizing_snapshot();
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false, id + "#close");
    if (immediately && accepted)
        (void)require_host().execute_current({*accepted, NativeCurrentPriceRule::NearestTick});
}

void PineExecutionAdapter::close_all() {
    native_order::Request request;
    request.intent = native_order::Flatten{}; request.label = "__pine_close_all";
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::CloseAll; snapshot.source_id = request.label;
    snapshot.sizing = sizing_snapshot();
    submit_or_replace(std::move(request), std::move(snapshot), false, "__pine_close_all");
}

void PineExecutionAdapter::exit(const SourceId& exit_id, const SourceId& from_entry,
                                double limit_price, double stop_price, double trail_points,
                                double trail_offset, double trail_price, double qty_percent,
                                const std::string& comment, double qty,
                                const std::string& oca_name, double profit_ticks,
                                double loss_ticks) {
    // Relative levels resolve against a live source cohort. The original tick
    // facts remain in the snapshot for deferred/observer projections.
    const auto physical = require_host().physical_position();
    const double entry_price = require_host().position_avg_price();
    const double tick = staged_.syminfo.mintick;
    if (physical.signed_units != 0.0 && finite_positive(entry_price) && finite_positive(tick)) {
        const bool long_side = physical.signed_units > 0.0;
        if (!finite_positive(limit_price) && finite_positive(profit_ticks))
            limit_price = entry_price + (long_side ? 1.0 : -1.0) * profit_ticks * tick;
        if (!finite_positive(stop_price) && finite_positive(loss_ticks))
            stop_price = entry_price - (long_side ? 1.0 : -1.0) * loss_ticks * tick;
        if (!finite_positive(trail_offset) && finite_positive(trail_points))
            trail_offset = trail_points * tick;
    }
    const bool dynamic = std::isnan(qty);
    const auto group_name = oca_name.empty() ? exit_id + "\x1f" + from_entry : oca_name;
    const auto family_key = key_for(exit_id, from_entry);
    auto submit_leg = [&](PineOrderFamily family, native_order::Trigger trigger) {
        native_order::Request request;
        request.intent = dynamic
            ? native_order::OrderIntent{native_order::HostSized{native_order::HostSizedKind::Close, std::nullopt}}
            : native_order::OrderIntent{native_order::Reduce{native_order::ExplicitUnits{qty}}};
        request.label = exit_id; request.comment = comment; request.trigger = std::move(trigger);
        request.owner = owner_for_close(from_entry, dynamic);
        request.group = group_for(group_name, oca_name.empty() ? 1 : 0);
        PlacementSnapshot snapshot;
        snapshot.family = family; snapshot.source_id = exit_id; snapshot.from_entry = from_entry;
        snapshot.comment = comment; snapshot.oca_name = oca_name; snapshot.requested_qty = qty;
        snapshot.qty_percent = qty_percent; snapshot.deferred_cohort = dynamic;
        snapshot.exit_levels = {limit_price, stop_price, trail_points, trail_offset,
                                trail_price, profit_ticks, loss_ticks};
        snapshot.sizing = sizing_snapshot();
        const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false,
            exit_id + "\x1f" + from_entry + std::to_string(static_cast<int>(family)));
        if (accepted) bracket_families_[family_key].push_back(*accepted);
    };
    if (finite_positive(limit_price)) submit_leg(PineOrderFamily::ExitLimit, native_order::Limit{limit_price});
    if (finite_positive(stop_price)) submit_leg(PineOrderFamily::ExitStop, native_order::Stop{stop_price});
    if (finite_positive(trail_offset)) {
        submit_leg(PineOrderFamily::ExitTrail, native_order::Trail{trail_offset,
            finite_positive(trail_price) ? std::optional<double>{trail_price} : std::nullopt});
    }
    if (!finite_positive(limit_price) && !finite_positive(stop_price) && !finite_positive(trail_offset))
        exit_cancel_bracket(exit_id, from_entry, comment);
}

void PineExecutionAdapter::exit_cancel_bracket(const SourceId& exit_id,
                                               const SourceId& from_entry,
                                               const std::string&) {
    const auto key = key_for(exit_id, from_entry);
    const auto found = bracket_families_.find(key);
    if (found == bracket_families_.end()) return;
    for (const auto& handle : found->second) {
        const auto result = require_host().cancel(handle);
        if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
    }
    bracket_families_.erase(found);
}

void PineExecutionAdapter::cancel(const SourceId& id) {
    std::vector<native_order::RequestHandle> matches;
    for (const auto& handle : live_handles_) {
        const auto snapshot = placement_.find(handle.incarnation);
        if (snapshot != placement_.end() && snapshot->second.source_id == id) matches.push_back(handle);
    }
    for (const auto& handle : matches) {
        const auto result = require_host().cancel(handle);
        if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
    }
}

void PineExecutionAdapter::cancel_all() {
    const auto handles = live_handles_;
    for (const auto& handle : handles) {
        const auto result = require_host().cancel(handle);
        if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
    }
    bracket_families_.clear();
}

void PineExecutionAdapter::order(const SourceId& id, bool is_long, double qty,
                                 double limit_price, double stop_price,
                                 const std::string& oca_name, int oca_type) {
    native_order::Request request;
    request.intent = native_order::Transact{is_long ? qty : -qty};
    request.label = id; request.trigger = trigger_for(limit_price, stop_price, kNaN, kNaN);
    request.group = group_for(oca_name, oca_type);
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Order; snapshot.source_id = id; snapshot.oca_name = oca_name;
    snapshot.oca_type = oca_type; snapshot.requested_qty = qty; snapshot.is_long = is_long;
    snapshot.sizing = sizing_snapshot();
    submit_or_replace(std::move(request), std::move(snapshot), true, id);
}

native_order::ExecutionTerms PineExecutionAdapter::resolve_terms(
        const NativeExecutionTermsFacts& facts) const {
    native_order::ExecutionTerms result{facts.default_resolved_price, std::nullopt,
                                        native_order::OpeningShape::Transact};
    const auto snapshot = placement_.find(facts.target.incarnation);
    if (snapshot == placement_.end()) return result;
    const auto& source = snapshot->second;
    double resolved = facts.default_resolved_price;
    const bool market_like = std::holds_alternative<native_order::Market>(facts.definition->request.trigger);
    if (market_like && config_.slippage != 0 && finite_positive(staged_.syminfo.mintick)) {
        resolved += facts.is_buy ? config_.slippage * staged_.syminfo.mintick
                                 : -config_.slippage * staged_.syminfo.mintick;
    }
    result.resolved_price = nearest_tick(resolved, staged_.syminfo.mintick);
    if (!std::holds_alternative<native_order::HostSized>(facts.definition->request.intent)) return result;
    if (source.family == PineOrderFamily::Close || source.family == PineOrderFamily::ExitLimit
        || source.family == PineOrderFamily::ExitStop || source.family == PineOrderFamily::ExitTrail) {
        double percent = source.qty_percent;
        if (std::isnan(percent)) percent = 100.0;
        result.units = std::max(0.0, facts.scope_exposure_units * percent / 100.0);
        return result;
    }
    if (finite_positive(source.sizing.frozen_units) && !source.sizing.at_fill) {
        result.units = source.sizing.frozen_units;
    } else if (config_.default_qty_type == static_cast<int>(QtyType::FIXED)) {
        result.units = config_.default_qty_value;
    } else if (config_.default_qty_type == static_cast<int>(QtyType::CASH)) {
        result.units = finite_positive(result.resolved_price) ? config_.default_qty_value / result.resolved_price : 0.0;
    } else {
        const double equity = source.sizing.at_fill
            ? require_host().native_marked_equity(result.resolved_price) : source.sizing.equity;
        const double price = source.sizing.at_fill ? result.resolved_price : source.sizing.price;
        result.units = finite_positive(equity) && finite_positive(price)
            ? equity * config_.default_qty_value / 100.0 / price : 0.0;
    }
    if (source.reverse_to) result.shape = native_order::OpeningShape::ReverseTo;
    return result;
}

NativePrecommitVerdict PineExecutionAdapter::validate_precommit(const NativePrecommitView& view) const {
    if (risk_.halted) return NativePrecommitVerdict::Refuse;
    if (risk_.max_position_size > 0.0 && std::abs(view.inspected_opened_units) > risk_.max_position_size)
        return NativePrecommitVerdict::Refuse;
    if (risk_.max_cons_loss_days > 0 && day_ledger_.consecutive_loss_days >= risk_.max_cons_loss_days)
        return NativePrecommitVerdict::Refuse;
    return NativePrecommitVerdict::Proceed;
}

std::int64_t PineExecutionAdapter::day_key(std::int64_t timestamp_ms) noexcept {
    constexpr std::int64_t kDayMs = 86400000;
    return timestamp_ms >= 0 ? timestamp_ms / kDayMs : -(((-timestamp_ms) + kDayMs - 1) / kDayMs);
}

void PineExecutionAdapter::on_bar_open(const Bar& bar, const NativeDecisionContext& context) {
    day_ledger_.current_day = day_key(context.sub_bar_open_ms);
    if (day_ledger_.intraday_loss_day != day_ledger_.current_day) {
        day_ledger_.intraday_loss_day = day_ledger_.current_day;
        day_ledger_.intraday_start_equity = require_host().native_marked_equity(bar.open);
        day_ledger_.intraday_realized = 0.0;
    }
    cap.ordinary_open(0);
}

void PineExecutionAdapter::on_applied(const native_order::ExecutionAppliedEvent& event,
                                      const NativeDecisionContext& context) {
    const auto placement = placement_.find(event.handle().incarnation);
    if (placement != placement_.end() && placement->second.opening && event.opened_units > 0.0) {
        auto& facts = cohorts_by_id_[placement->second.source_id];
        facts.cycle = event.cycle_after;
        if (std::find(facts.opened.begin(), facts.opened.end(), event.handle()) == facts.opened.end())
            facts.opened.push_back(event.handle());
    }
    if (event.terminal) retire(event.handle());
    if (event.ordinal != day_ledger_.observed_applied_ordinal) {
        day_ledger_.observed_applied_ordinal = event.ordinal;
        const auto day = day_key(context.sub_bar_open_ms);
        if (event.closed_trade_count > 0) {
            double total = 0.0;
            const auto& host = require_host();
            for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
                const auto index = event.first_trade_index + i;
                if (index < static_cast<std::size_t>(host.trade_count())) total += host.get_trade(static_cast<int>(index)).pnl;
            }
            day_ledger_.intraday_realized += total;
            if (day != day_ledger_.last_loss_day) {
                day_ledger_.consecutive_loss_days = total < 0.0 ? day_ledger_.consecutive_loss_days + 1 : 0;
                day_ledger_.last_loss_day = day;
            }
        }
    }
    refresh_pending_view();
}

int PineExecutionAdapter::short_seed_collision_role_v1(native_order::RequestHandle handle) const noexcept {
    if (!short_seed_.active) return 0;
    if (handle == short_seed_.long_entry) return 1;
    if (handle == short_seed_.materialize_long) return 2;
    if (handle == short_seed_.final_short) return 3;
    return 0;
}

void PineExecutionAdapter::set_risk_direction(int direction) noexcept { risk_.direction = direction; }
void PineExecutionAdapter::set_risk_max_cons_loss_days(int value) noexcept { risk_.max_cons_loss_days = value; }
void PineExecutionAdapter::set_risk_max_drawdown(double value, bool percent) noexcept { risk_.max_drawdown = value; risk_.max_drawdown_percent = percent; }
void PineExecutionAdapter::set_risk_max_intraday_loss(double value, bool percent) noexcept { risk_.max_intraday_loss = value; risk_.max_intraday_loss_percent = percent; }
void PineExecutionAdapter::set_risk_max_position_size(double value) noexcept { risk_.max_position_size = value; }
void PineExecutionAdapter::enable_intraday_cap() noexcept { cap.attach(); }
void PineExecutionAdapter::attach_execution_adapter() noexcept { priority.attach(); }

std::vector<native_order::RequestHandle> PineExecutionAdapter::take_first_open_newborns() {
    auto result = std::move(first_open_newborns_);
    first_open_newborns_.clear();
    return result;
}

void PineExecutionAdapter::refresh_pending_view() noexcept { pending_view_handles_ = live_handles_; }

int PendingIntentView::size() const noexcept { return owner_ ? static_cast<int>(owner_->pending_view_handles_.size()) : 0; }

int PendingIntentView::probe_fill_qty(int index, double fill_price, double* qty,
                                      int* close_only, int* partition) const noexcept {
    if (!owner_ || index < 0 || index >= static_cast<int>(owner_->pending_view_handles_.size())
        || !qty || !close_only || !partition) return -1;
    const auto handle = owner_->pending_view_handles_[static_cast<std::size_t>(index)];
    const auto it = owner_->placement_.find(handle.incarnation);
    if (it == owner_->placement_.end()) return -1;
    const auto& snapshot = it->second;
    *close_only = snapshot.opening ? 0 : 1;
    if (!snapshot.opening) { *qty = kNaN; *partition = -1; return 1; }
    if (finite_positive(snapshot.requested_qty)) { *qty = snapshot.requested_qty; *partition = 0; return 0; }
    if (finite_positive(snapshot.sizing.frozen_units)) { *qty = snapshot.sizing.frozen_units; *partition = 1; return 0; }
    *qty = finite_positive(fill_price) && owner_->config_.default_qty_type == static_cast<int>(QtyType::CASH)
        ? owner_->config_.default_qty_value / fill_price : owner_->config_.default_qty_value;
    *partition = 2;
    return 0;
}

int PendingIntentView::level_resolved(int index) const noexcept {
    if (!owner_ || index < 0 || index >= static_cast<int>(owner_->pending_view_handles_.size())) return -1;
    const auto handle = owner_->pending_view_handles_[static_cast<std::size_t>(index)];
    const auto it = owner_->placement_.find(handle.incarnation);
    if (it == owner_->placement_.end()) return -1;
    if (it->second.from_entry.empty()) return 1;
    const auto cohort = owner_->cohorts_by_id_.find(it->second.from_entry);
    return cohort != owner_->cohorts_by_id_.end() && !cohort->second.opened.empty() ? 1 : 0;
}

int PendingIntentView::effective_levels(int index, double* stop, double* limit,
                                        double* trail_activation) const noexcept {
    if (!owner_ || index < 0 || index >= static_cast<int>(owner_->pending_view_handles_.size())
        || !stop || !limit || !trail_activation) return -1;
    const auto handle = owner_->pending_view_handles_[static_cast<std::size_t>(index)];
    const auto it = owner_->placement_.find(handle.incarnation);
    if (it == owner_->placement_.end()) return -1;
    *stop = it->second.exit_levels.stop;
    *limit = it->second.exit_levels.limit;
    *trail_activation = it->second.exit_levels.trail_price;
    return level_resolved(index);
}

int PendingIntentView::short_seed_collision_role(int index) const noexcept {
    if (!owner_ || index < 0 || index >= static_cast<int>(owner_->pending_view_handles_.size())) return -1;
    return owner_->short_seed_collision_role_v1(owner_->pending_view_handles_[static_cast<std::size_t>(index)]);
}
int PendingIntentView::last_bar_dual_entry_path() const noexcept { return owner_ ? owner_->last_bar_dual_entry_path_ : 0; }
double PendingIntentView::trail_best_price() const noexcept {
    return owner_ && owner_->host_ ? owner_->host_->trail_best_price() : kNaN;
}

} // namespace pineforge::source
