#include <pineforge/source/pine_adapter.hpp>

#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
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
    pending_bracket_legs_.clear();
    pending_entries_.clear();
    pending_relative_exits_.clear();
    live_handles_.clear();
    first_open_newborns_.clear();
    pending_view_handles_.clear();
    current_debited_applied_ordinals_.clear();
    receipt_cursor_ = 0;
    materializing_relative_ = false;
    current_position_cycle_ = 0;
    current_position_sign_ = 0;
    next_sequential_group_ = 0;
    pooc_close_basis_by_script_bar_.clear();
    pooc_open_basis_ = 0.0;
    pooc_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
    close_all_pending_script_bar_ = std::numeric_limits<std::int64_t>::min();
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
    // A13: source hosts opt into the generic legacy-compatible batch ingress.
    // Native-only hosts retain the strict Canonical/None defaults.
    spec.slot_label_policy = NativeSlotLabelPolicy::LegacyTolerant;
    spec.legacy_tolerance = NativeLegacyTolerance::BatchStructuralBars;
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

double PineExecutionAdapter::cohort_exposure_for(const SourceId& id) const noexcept {
    const auto found = cohorts_by_id_.find(id);
    if (found == cohorts_by_id_.end()) return 0.0;
    double total = 0.0;
    for (const auto& row : found->second.live_units_by_origin) {
        if (std::isfinite(row.second) && row.second > 0.0) total += row.second;
    }
    return std::isfinite(total) && total > 0.0 ? total : 0.0;
}

double PineExecutionAdapter::quantize_close_units(double basis, double percent) const noexcept {
    if (!std::isfinite(basis) || basis <= 0.0 || !std::isfinite(percent) || percent <= 0.0)
        return 0.0;
    double units = basis * percent / 100.0;
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (staged_.quantity_grid && std::isfinite(*staged_.quantity_grid)
        && *staged_.quantity_grid > 0.0) {
        const double step = *staged_.quantity_grid;
        // Pine's percentage close quantity is a source command fact: it
        // floors to the instrument step and retains one tradable step for a
        // positive fractional result (the L0 integer-lot witness).
        units = std::floor(units / step + 1e-12) * step;
        if (units == 0.0) units = step;
    }
    return units;
}

void PineExecutionAdapter::consume_cohort_units(
        const SourceId& id, const native_order::ExecutionAppliedEvent& event) {
    if (!(event.closed_units > 0.0) || !std::isfinite(event.closed_units)) return;
    const auto found = cohorts_by_id_.find(id);
    if (found == cohorts_by_id_.end()) return;
    auto& facts = found->second;
    std::vector<std::uint64_t> selected;
    if (const auto* opening = std::get_if<execution::OpeningExposure>(&event.scope)) {
        selected.push_back(opening->incarnation);
    } else if (const auto* openings = std::get_if<native_order::SelectedExposure>(&event.scope)) {
        selected = openings->incarnations;
    } else {
        for (const auto& handle : facts.opened) selected.push_back(handle.incarnation);
    }
    double remaining = event.closed_units;
    for (const auto incarnation : selected) {
        auto unit = facts.live_units_by_origin.find(incarnation);
        if (unit == facts.live_units_by_origin.end() || !(unit->second > 0.0)) continue;
        const double deduction = std::min(unit->second, remaining);
        unit->second -= deduction;
        remaining -= deduction;
        if (unit->second == 0.0) facts.live_units_by_origin.erase(unit);
        if (!(remaining > 0.0)) break;
    }
}

bool PineExecutionAdapter::origin_is_pending(
        const native_order::RequestHandle& origin) const noexcept {
    if (origin.incarnation == 0) return false;
    const auto placement = placement_.find(origin.incarnation);
    if (placement == placement_.end() || !placement->second.opening) return false;
    return std::find(live_handles_.begin(), live_handles_.end(), origin) != live_handles_.end();
}

void PineExecutionAdapter::cancel_bracket_origin(const native_order::RequestHandle& origin) {
    pending_bracket_legs_.erase(std::remove_if(pending_bracket_legs_.begin(), pending_bracket_legs_.end(),
        [&](const PendingBracketLeg& leg) { return leg.snapshot.bracket_origin == origin; }),
        pending_bracket_legs_.end());
    std::vector<native_order::RequestHandle> matches;
    for (const auto& handle : live_handles_) {
        const auto placement = placement_.find(handle.incarnation);
        if (placement != placement_.end() && placement->second.bracket_origin == origin)
            matches.push_back(handle);
    }
    for (const auto& handle : matches) {
        const auto result = require_host().cancel(handle);
        if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
    }
}

void PineExecutionAdapter::cancel_bracket_siblings(const native_order::RequestHandle& handle) {
    const auto source = placement_.find(handle.incarnation);
    if (source == placement_.end()) return;
    const auto& snapshot = source->second;
    if (snapshot.family != PineOrderFamily::ExitLimit && snapshot.family != PineOrderFamily::ExitStop
        && snapshot.family != PineOrderFamily::ExitTrail) return;
    std::vector<native_order::RequestHandle> matches;
    for (const auto& candidate : live_handles_) {
        if (candidate == handle) continue;
        const auto placement = placement_.find(candidate.incarnation);
        if (placement == placement_.end()) continue;
        const auto& sibling = placement->second;
        if (sibling.source_id == snapshot.source_id && sibling.from_entry == snapshot.from_entry
            && sibling.bracket_origin == snapshot.bracket_origin
            && (sibling.family == PineOrderFamily::ExitLimit || sibling.family == PineOrderFamily::ExitStop
                || sibling.family == PineOrderFamily::ExitTrail)) {
            matches.push_back(candidate);
        }
    }
    for (const auto& sibling : matches) {
        const auto result = require_host().cancel(sibling);
        if (result.status == native_order::CancelStatus::Cancelled) retire(sibling);
    }
}

void PineExecutionAdapter::observe_terminal_receipts() {
    const auto rows = require_host().native_events(receipt_cursor_);
    for (const auto& row : rows) {
        receipt_cursor_ = std::max(receipt_cursor_, row.ordinal);
        if (!row.command) continue;
        std::visit([&](const auto& event) {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, native_order::MatchRejectedEvent>
                          || std::is_same_v<Event, native_order::CancelledEvent>) {
                const auto placement = placement_.find(event.handle().incarnation);
                if (placement != placement_.end()) {
                    const auto handle = event.handle();
                    const bool opening = placement->second.opening;
                    retire(handle);
                    if (opening) cancel_bracket_origin(handle);
                }
            } else if constexpr (std::is_same_v<Event, native_order::ExecutionAppliedEvent>) {
                if (event.terminal) cancel_bracket_siblings(event.handle());
            }
        }, *row.command);
    }
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
    if (config_.pyramiding > 0 && current != 0.0
        && ((current > 0.0) == is_long)) {
        std::size_t accepted_in_cycle = 0;
        for (const auto& cohort : cohorts_by_id_) {
            for (const auto& origin : cohort.second.opened) {
                const auto placement = placement_.find(origin.incarnation);
                if (placement != placement_.end() && placement->second.is_long == is_long)
                    ++accepted_in_cycle;
            }
        }
        for (const auto& handle : live_handles_) {
            const auto placement = placement_.find(handle.incarnation);
            if (placement != placement_.end() && placement->second.opening
                && placement->second.is_long == is_long) ++accepted_in_cycle;
        }
        // Pine's cap is a monotone entry-incarnation count for the current
        // position cycle; a partial close does not free a pyramiding slot.
        if (accepted_in_cycle >= static_cast<std::size_t>(config_.pyramiding)) return;
    }
    const auto current_point = require_host().current_execution_point();
    const bool close_all_precedes = current_point
        && close_all_pending_script_bar_ == current_point->decision.script_bar_open_ms;
    const bool reverses = current != 0.0 && ((current > 0.0) != is_long) && !close_all_precedes;
    const bool priced = !std::isnan(limit_price) || !std::isnan(stop_price);
    const bool cash_sized = qty_type == static_cast<int>(QtyType::CASH);
    const bool fixed_priced_reverse = reverses && !default_sized && priced && !cash_sized;
    const bool cash_priced_reverse = reverses && !default_sized && priced && cash_sized;
    if (default_sized) {
        request.intent = native_order::HostSized{native_order::HostSizedKind::Open,
            is_long ? native_order::Side::Long : native_order::Side::Short};
    } else if (fixed_priced_reverse || cash_priced_reverse) {
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
    // Reuse the durable level tuple for the parent trigger facts.  A deferred
    // relative exit may safely arm from a non-gap LIMIT parent's known entry
    // level before that parent is applied.
    snapshot.exit_levels.limit = limit_price;
    snapshot.exit_levels.stop = stop_price;
    snapshot.reverse_to = reverses; snapshot.sizing = sizing_snapshot();
    snapshot.terms_priced_reverse = fixed_priced_reverse || cash_priced_reverse;
    snapshot.placement_cycle = current_position_cycle_;
    if (fixed_priced_reverse) snapshot.frozen_reversal_transaction = std::abs(current) + qty;
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
    if (const auto point = require_host().current_execution_point()) {
        snapshot.placement_script_open_ms = point->decision.script_bar_open_ms;
        snapshot.placement_sub_open_ms = point->decision.sub_bar_open_ms;
        if (default_sized && reverses) {
            for (auto it = live_handles_.rbegin(); it != live_handles_.rend(); ++it) {
                const auto prior = placement_.find(it->incarnation);
                if (prior == placement_.end()) continue;
                auto& prior_snapshot = prior->second;
                if (!prior_snapshot.opening || prior_snapshot.family != PineOrderFamily::Entry
                    || prior_snapshot.is_long != is_long
                    || prior_snapshot.placement_script_open_ms != point->decision.script_bar_open_ms) {
                    continue;
                }
                const std::uint64_t group = prior_snapshot.sequential_group != 0
                    ? prior_snapshot.sequential_group : ++next_sequential_group_;
                prior_snapshot.sequential_group = group;
                if (prior_snapshot.sequential_rank == 0) prior_snapshot.sequential_rank = 1;
                snapshot.sequential_group = group;
                snapshot.sequential_rank = 2;
                break;
            }
        }
    }
    const bool pooc_same_side_add = config_.process_orders_on_close
        && !config_.calc_on_order_fills && current != 0.0
        && ((current > 0.0) == is_long)
        && std::holds_alternative<native_order::Market>(request.trigger);
    if (pooc_same_side_add) {
        auto queued = std::find_if(pending_entries_.begin(), pending_entries_.end(),
            [&](const PendingEntry& value) { return value.replacement_key == id; });
        PendingEntry pending{std::move(request), std::move(snapshot), id};
        if (queued == pending_entries_.end()) pending_entries_.push_back(std::move(pending));
        else *queued = std::move(pending);
        return;
    }
    submit_or_replace(std::move(request), std::move(snapshot), true, id);
}

void PineExecutionAdapter::close(const SourceId& id, const std::string& comment, double qty,
                                 double qty_percent, bool immediately, std::uint64_t callsite_token) {
    const auto openings = openings_for(id);
    // P-DA3: strategy.close against an empty cohort is dropped at the command.
    if (openings.empty()) return;
    const double requested_percent = std::isnan(qty_percent) ? 100.0 : qty_percent;
    if (immediately) {
        const double current = require_host().physical_position().signed_units;
        if (current != 0.0) {
            pending_entries_.erase(std::remove_if(pending_entries_.begin(), pending_entries_.end(),
                [&](const PendingEntry& entry) {
                    return (current > 0.0) == entry.snapshot.is_long;
                }), pending_entries_.end());
        }
    }
    // A queued percentage close snapshots its source-id exposure at the
    // command.  This covers both POOC (same calculation pass) and ordinary
    // next-open scheduling; the generic cohort remains authoritative for
    // selecting the actual live lots at its later candidate.  Immediate
    // closes deliberately re-read after their preceding execution.
    double frozen_qty = qty;
    if (std::isnan(qty)) {
        const auto point = require_host().current_execution_point();
        const std::int64_t bar_key = point ? point->decision.script_bar_open_ms
                                           : require_host().native_decision_floor();
        const double source_basis = cohort_exposure_for(id);
        const double fallback_basis = std::abs(require_host().physical_position().signed_units);
        const double script_basis = source_basis > 0.0 ? source_basis : fallback_basis;
        pooc_close_basis_by_script_bar_.emplace(bar_key, script_basis);
        frozen_qty = quantize_close_units(script_basis, requested_percent);
    }
    // P-DA4: an immediate close has a live cohort at the command boundary;
    // materialize its percentage quantity and bind that fixed roster before
    // invoking execute_current.  Deferred exits retain HostSized/BindCohort.
    const bool host_sized = std::isnan(qty) && !immediately;
    native_order::Request request;
    request.intent = host_sized
        ? native_order::OrderIntent{native_order::HostSized{native_order::HostSizedKind::Close, std::nullopt}}
        : native_order::OrderIntent{native_order::Reduce{native_order::ExplicitUnits{frozen_qty}}};
    // The generic request label is the legacy close transaction signal while
    // PlacementSnapshot keeps the public source id for cohorts/readback.
    request.label = "__close__" + id;
    request.comment = comment; request.owner = owner_for_close(id, host_sized);
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Close; snapshot.source_id = id; snapshot.from_entry = id;
    snapshot.comment = comment; snapshot.requested_qty = frozen_qty; snapshot.qty_percent = qty_percent;
    snapshot.immediately = immediately; snapshot.deferred_cohort = host_sized; snapshot.sizing = sizing_snapshot();
    // Only the generated callsite-token form represents source replacement.
    // Independent close statements in one evaluation must coexist (P1/P2).
    const SourceId replacement_key = callsite_token == 0
        ? SourceId{} : id + "#close#" + std::to_string(callsite_token);
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false, replacement_key);
    if (immediately && accepted) {
        const auto outcome = require_host().execute_current(
            {*accepted, NativeCurrentPriceRule::NearestTick});
        if (const auto* applied = std::get_if<native_order::ExecutionAppliedEvent>(&outcome)) {
            consume_cohort_units(id, *applied);
            current_debited_applied_ordinals_.insert(applied->ordinal);
        }
    }
}

void PineExecutionAdapter::close_all() {
    if (const auto point = require_host().current_execution_point())
        close_all_pending_script_bar_ = point->decision.script_bar_open_ms;
    if (config_.calc_on_order_fills) {
        const auto point = require_host().current_execution_point();
        const std::int64_t script_open = point ? point->decision.script_bar_open_ms
                                               : require_host().native_decision_floor();
        std::vector<native_order::RequestHandle> newborns;
        for (const auto& handle : live_handles_) {
            const auto placement = placement_.find(handle.incarnation);
            if (placement != placement_.end() && placement->second.opening
                && placement->second.placement_script_open_ms == script_open) {
                newborns.push_back(handle);
            }
        }
        for (const auto& handle : newborns) {
            const auto result = require_host().cancel(handle);
            if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
        }
    }
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
    double entry_price = require_host().position_avg_price();
    const double tick = staged_.syminfo.mintick;
    bool parent_long = physical.signed_units > 0.0;
    bool known_parent_level = physical.signed_units != 0.0;
    if (physical.signed_units == 0.0) {
        const auto cohort = cohorts_by_id_.find(from_entry);
        if (cohort != cohorts_by_id_.end()) {
            for (const auto& origin : cohort->second.origins) {
                const auto parent = placement_.find(origin.incarnation);
                if (parent != placement_.end() && parent->second.opening
                    && finite_positive(parent->second.exit_levels.limit)) {
                    entry_price = parent->second.exit_levels.limit;
                    parent_long = parent->second.is_long;
                    known_parent_level = true;
                    break;
                }
            }
        }
    }
    if (known_parent_level && finite_positive(entry_price) && finite_positive(tick)) {
        const bool long_side = physical.signed_units != 0.0 ? physical.signed_units > 0.0 : parent_long;
        if (!finite_positive(limit_price) && finite_positive(profit_ticks))
            limit_price = entry_price + (long_side ? 1.0 : -1.0) * profit_ticks * tick;
        if (!finite_positive(stop_price) && finite_positive(loss_ticks))
            stop_price = entry_price - (long_side ? 1.0 : -1.0) * loss_ticks * tick;
        if (!finite_positive(trail_offset) && finite_positive(trail_points))
            trail_offset = trail_points * tick;
    }
    const bool unresolved_relative = !finite_positive(limit_price) && !finite_positive(stop_price)
        && !finite_positive(trail_offset)
        && (finite_positive(profit_ticks) || finite_positive(loss_ticks)
            || finite_positive(trail_points));
    if (unresolved_relative) {
        PendingRelativeExit pending;
        pending.exit_id = exit_id; pending.from_entry = from_entry;
        pending.trail_points = trail_points; pending.trail_offset = trail_offset;
        pending.trail_price = trail_price; pending.qty_percent = qty_percent;
        pending.comment = comment; pending.qty = qty; pending.oca_name = oca_name;
        pending.profit_ticks = profit_ticks; pending.loss_ticks = loss_ticks;
        auto existing = std::find_if(pending_relative_exits_.begin(), pending_relative_exits_.end(),
            [&](const PendingRelativeExit& value) {
                return value.exit_id == exit_id && value.from_entry == from_entry;
            });
        if (existing == pending_relative_exits_.end()) pending_relative_exits_.push_back(std::move(pending));
        else *existing = std::move(pending);
        return;
    }
    if (std::isnan(qty) && qty_percent == 100.0) {
        const auto cohort = cohorts_by_id_.find(from_entry);
        if (cohort != cohorts_by_id_.end() && !cohort->second.origins.empty()) {
            const auto origin = cohort->second.origins.back();
            const auto parent = placement_.find(origin.incarnation);
            if (parent != placement_.end() && parent->second.family == PineOrderFamily::Entry)
                parent->second.has_full_entry_bracket = true;
        }
    }
    const bool dynamic = std::isnan(qty);
    const auto family_key = key_for(exit_id, from_entry);
    auto submit_leg = [&](PineOrderFamily family, native_order::Trigger trigger) {
        auto submit_one = [&](native_order::Owner owner, bool host_sized,
                              const SourceId& replacement_key, const std::string& group_name,
                              bool defer_new_instance,
                              native_order::RequestHandle bracket_origin = {}) {
            native_order::Request request;
            request.intent = host_sized
                ? native_order::OrderIntent{native_order::HostSized{
                    native_order::HostSizedKind::Close, std::nullopt}}
                : native_order::OrderIntent{native_order::Reduce{
                    native_order::ExplicitUnits{qty}}};
            request.label = exit_id; request.comment = comment; request.trigger = trigger;
            request.owner = std::move(owner);
            request.group = group_for(group_name, oca_name.empty() ? 1 : 0);
            PlacementSnapshot snapshot;
            snapshot.family = family; snapshot.source_id = exit_id; snapshot.from_entry = from_entry;
            snapshot.comment = comment; snapshot.oca_name = oca_name; snapshot.requested_qty = qty;
            snapshot.qty_percent = qty_percent; snapshot.deferred_cohort = host_sized;
            snapshot.bracket_origin = std::move(bracket_origin);
            snapshot.exit_levels = {limit_price, stop_price, trail_points, trail_offset,
                                    trail_price, profit_ticks, loss_ticks};
            snapshot.sizing = sizing_snapshot();
            if (defer_new_instance && live_by_source_key_.find(key_for(replacement_key))
                == live_by_source_key_.end()) {
                auto queued = std::find_if(pending_bracket_legs_.begin(), pending_bracket_legs_.end(),
                    [&](const PendingBracketLeg& row) { return row.replacement_key == replacement_key; });
                PendingBracketLeg staged{std::move(request), std::move(snapshot), replacement_key, family_key};
                if (queued == pending_bracket_legs_.end()) pending_bracket_legs_.push_back(std::move(staged));
                else *queued = std::move(staged);
                return;
            }
            const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false,
                                                    replacement_key);
            if (accepted) bracket_families_[family_key].push_back(*accepted);
        };

        if (dynamic) {
            const auto group_name = oca_name.empty() ? exit_id + "\x1f" + from_entry : oca_name;
            submit_one(owner_for_close(from_entry, !materializing_relative_), true,
                exit_id + "\x1f" + from_entry + std::to_string(static_cast<int>(family)), group_name, false);
            return;
        }

        // An explicit bracket quantity is one independently persistent leg
        // for every source entry provenance, including an origin that is
        // still pending.  BindCohort keeps that pending-origin leg deferred
        // without inventing a source id in the generic core; the source key
        // gives re-issues replacement semantics per (exit, from_entry, leg,
        // origin) rather than accidentally replacing a carried instance.
        const auto cohort = cohort_for(from_entry);
        const auto found = cohorts_by_id_.find(from_entry);
        std::vector<native_order::RequestHandle> origins;
        if (found != cohorts_by_id_.end()) origins = found->second.origins;
        if (origins.empty()) origins.push_back({});
        for (const auto& origin : origins) {
            const std::string origin_key = std::to_string(origin.incarnation);
            const auto replacement_key = exit_id + "\x1f" + from_entry + "\x1f"
                + std::to_string(static_cast<int>(family)) + "\x1f" + origin_key;
            const auto group_name = oca_name.empty()
                ? exit_id + "\x1f" + from_entry + "\x1f" + origin_key : oca_name;
            const bool has_live_leg = live_by_source_key_.find(key_for(replacement_key))
                != live_by_source_key_.end();
            if (origin.incarnation != 0 && !has_live_leg && !origin_is_pending(origin)) continue;
            submit_one(native_order::BindCohort{cohort}, true, replacement_key, group_name,
                       !has_live_leg, origin);
        }
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

void PineExecutionAdapter::flush_pending_bracket_legs() {
    auto queued = std::move(pending_bracket_legs_);
    pending_bracket_legs_.clear();
    for (auto& leg : queued) {
        const auto accepted = submit_or_replace(std::move(leg.request), std::move(leg.snapshot), false,
                                                leg.replacement_key);
        if (accepted) bracket_families_[leg.family_key].push_back(*accepted);
    }
}

void PineExecutionAdapter::flush_pending_entries() {
    auto queued = std::move(pending_entries_);
    pending_entries_.clear();
    for (auto& entry : queued) {
        (void)submit_or_replace(std::move(entry.request), std::move(entry.snapshot), true,
                                entry.replacement_key);
    }
}

void PineExecutionAdapter::materialize_relative_exits(
        const PlacementSnapshot& opening, const native_order::ExecutionAppliedEvent& event) {
    if (pending_relative_exits_.empty() || !finite_positive(staged_.syminfo.mintick)) return;
    std::vector<PendingRelativeExit> pending;
    for (auto it = pending_relative_exits_.begin(); it != pending_relative_exits_.end();) {
        if (it->from_entry == opening.source_id) {
            pending.push_back(std::move(*it));
            it = pending_relative_exits_.erase(it);
        } else {
            ++it;
        }
    }
    const double tick = staged_.syminfo.mintick;
    for (const auto& value : pending) {
        double limit = kNaN;
        double stop = kNaN;
        double offset = value.trail_offset;
        if (finite_positive(value.profit_ticks)) {
            limit = event.resolved_price + (opening.is_long ? 1.0 : -1.0)
                * value.profit_ticks * tick;
        }
        if (finite_positive(value.loss_ticks)) {
            stop = event.resolved_price - (opening.is_long ? 1.0 : -1.0)
                * value.loss_ticks * tick;
        }
        if (!finite_positive(offset) && finite_positive(value.trail_points))
            offset = value.trail_points * tick;
        materializing_relative_ = true;
        try {
            exit(value.exit_id, value.from_entry, limit, stop, value.trail_points, offset,
                 value.trail_price, value.qty_percent, value.comment, value.qty, value.oca_name,
                 kNaN, kNaN);
        } catch (...) {
            materializing_relative_ = false;
            throw;
        }
        materializing_relative_ = false;
    }
}

void PineExecutionAdapter::exit_cancel_bracket(const SourceId& exit_id,
                                               const SourceId& from_entry,
                                               const std::string&) {
    const auto key = key_for(exit_id, from_entry);
    pending_relative_exits_.erase(std::remove_if(pending_relative_exits_.begin(), pending_relative_exits_.end(),
        [&](const PendingRelativeExit& value) {
            return value.exit_id == exit_id && value.from_entry == from_entry;
        }), pending_relative_exits_.end());
    pending_bracket_legs_.erase(std::remove_if(pending_bracket_legs_.begin(), pending_bracket_legs_.end(),
        [&](const PendingBracketLeg& leg) { return leg.family_key == key; }), pending_bracket_legs_.end());
    const auto found = bracket_families_.find(key);
    if (found == bracket_families_.end()) return;
    for (const auto& handle : found->second) {
        const auto result = require_host().cancel(handle);
        if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
    }
    bracket_families_.erase(found);
}

void PineExecutionAdapter::cancel(const SourceId& id) {
    pending_relative_exits_.erase(std::remove_if(pending_relative_exits_.begin(), pending_relative_exits_.end(),
        [&](const PendingRelativeExit& value) { return value.exit_id == id || value.from_entry == id; }),
        pending_relative_exits_.end());
    pending_entries_.erase(std::remove_if(pending_entries_.begin(), pending_entries_.end(),
        [&](const PendingEntry& entry) { return entry.snapshot.source_id == id; }), pending_entries_.end());
    pending_bracket_legs_.erase(std::remove_if(pending_bracket_legs_.begin(), pending_bracket_legs_.end(),
        [&](const PendingBracketLeg& leg) { return leg.snapshot.source_id == id; }),
        pending_bracket_legs_.end());
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
    pending_bracket_legs_.clear();
    pending_entries_.clear();
    pending_relative_exits_.clear();
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
    // Explicit native intents already carry their canonical trigger/fill
    // price. Re-rounding a binary64 limit here can move it one representable
    // value beyond its immutable level and turn an otherwise valid limit fill
    // into InvalidTerms (the 65-resting-order oracle exposes exactly that).
    if (!std::holds_alternative<native_order::HostSized>(facts.definition->request.intent))
        return result;
    double resolved = facts.default_resolved_price;
    const bool market_like = std::holds_alternative<native_order::Market>(facts.definition->request.trigger);
    if (market_like && config_.slippage != 0 && finite_positive(staged_.syminfo.mintick)) {
        resolved += facts.is_buy ? config_.slippage * staged_.syminfo.mintick
                                 : -config_.slippage * staged_.syminfo.mintick;
    }
    if (market_like) result.resolved_price = nearest_tick(resolved, staged_.syminfo.mintick);
    // Source stop/trail exits crossed inside a modeled path settle at their
    // armed level, whereas an open gap retains the presented open quote.  The
    // generic driver deliberately exposes both facts; selecting this source
    // policy here preserves the non-gap relative-parent lifecycle.
    if ((source.family == PineOrderFamily::ExitStop || source.family == PineOrderFamily::ExitTrail)
        && facts.trigger_level && facts.cursor.point.path_phase != NativePathPhase::Open) {
        result.resolved_price = nearest_tick(*facts.trigger_level, staged_.syminfo.mintick);
    }
    if (source.family == PineOrderFamily::Close || source.family == PineOrderFamily::ExitLimit
        || source.family == PineOrderFamily::ExitStop || source.family == PineOrderFamily::ExitTrail) {
        if (finite_positive(source.requested_qty)) {
            result.units = source.requested_qty;
            return result;
        }
        double percent = source.qty_percent;
        if (std::isnan(percent)) percent = 100.0;
        result.units = std::max(0.0, facts.scope_exposure_units * percent / 100.0);
        return result;
    }
    if (source.family == PineOrderFamily::Entry && source.terms_priced_reverse) {
        double own_units = source.requested_qty;
        if (source.qty_type == static_cast<int>(QtyType::CASH)) {
            own_units = finite_positive(result.resolved_price)
                ? source.requested_qty / result.resolved_price : 0.0;
        }
        result.units = own_units;
        if (finite_positive(source.frozen_reversal_transaction)
            && source.placement_cycle == current_position_cycle_
            && std::abs(facts.position.signed_units - (source.is_long
                ? -source.frozen_reversal_transaction : source.frozen_reversal_transaction))
                < 1e-12) {
            result.units = source.frozen_reversal_transaction;
            result.shape = native_order::OpeningShape::CloseOpposite;
        } else {
            result.shape = native_order::OpeningShape::ReverseTo;
        }
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
    if (source.family == PineOrderFamily::Entry && source.sequential_group != 0
        && source.sequential_rank != 0 && source.has_full_entry_bracket) {
        bool paired = false;
        for (const auto& row : placement_) {
            const auto& peer = row.second;
            if (peer.family == PineOrderFamily::Entry
                && peer.sequential_group == source.sequential_group
                && peer.sequential_rank != 0 && peer.sequential_rank != source.sequential_rank
                && peer.has_full_entry_bracket) {
                paired = true;
                break;
            }
        }
        if (paired) {
            const bool opposite = facts.position.signed_units != 0.0
                && ((facts.position.signed_units > 0.0) != source.is_long);
            if (source.sequential_rank == 1 && opposite)
                result.units = std::max(std::abs(facts.position.signed_units), *result.units);
            result.shape = native_order::OpeningShape::Transact;
            return result;
        }
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
    // Terminal entry refusals have no Applied notification.  Consume their
    // generic receipt before the next matching point so their deferred
    // per-origin bracket legs cannot close a different cohort member.
    observe_terminal_receipts();
    if (close_all_pending_script_bar_ != context.script_bar_open_ms)
        close_all_pending_script_bar_ = std::numeric_limits<std::int64_t>::min();
    pooc_open_script_bar_ = context.script_bar_open_ms;
    pooc_open_basis_ = std::abs(require_host().physical_position().signed_units);
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
    const double live_position = require_host().physical_position().signed_units;
    const int next_sign = live_position > 0.0 ? 1 : (live_position < 0.0 ? -1 : 0);
    if (next_sign != 0 && (current_position_sign_ == 0 || current_position_sign_ != next_sign))
        ++current_position_cycle_;
    current_position_sign_ = next_sign;
    if (placement != placement_.end() && placement->second.opening
        && std::abs(event.opened_units) > 0.0) {
        auto& facts = cohorts_by_id_[placement->second.source_id];
        facts.cycle = event.cycle_after;
        if (std::find(facts.opened.begin(), facts.opened.end(), event.handle()) == facts.opened.end())
            facts.opened.push_back(event.handle());
        facts.live_units_by_origin[event.handle().incarnation] += std::abs(event.opened_units);
        materialize_relative_exits(placement->second, event);
    }
    const bool current_debit_observed =
        current_debited_applied_ordinals_.erase(event.ordinal) != 0;
    if (!current_debit_observed && placement != placement_.end()
        && !placement->second.from_entry.empty()) {
        consume_cohort_units(placement->second.from_entry, event);
    }
    if (placement != placement_.end() && placement->second.family == PineOrderFamily::CloseAll
        && event.closed_units > 0.0) {
        for (auto& cohort : cohorts_by_id_) cohort.second.live_units_by_origin.clear();
    }
    if (require_host().physical_position().signed_units == 0.0) {
        for (auto& cohort : cohorts_by_id_) {
            cohort.second.opened.clear();
            cohort.second.live_units_by_origin.clear();
        }
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
