#include <pineforge/source/pine_adapter.hpp>

#include <pineforge/pending_order_mirror.hpp>

#include <pineforge/timeframe.hpp>

#include "../timezone.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstring>
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

void copy_pending_string(std::string_view value, char* out, std::uint8_t* truncated,
                         std::uint64_t* hash) noexcept {
    *hash = fnv_string(value);
    const std::size_t size = std::min<std::size_t>(value.size(), 63U);
    if (size != 0) std::memcpy(out, value.data(), size);
    out[size] = '\0';
    *truncated = value.size() > size ? 1U : 0U;
}

int mirror_order_type(PineOrderFamily family) noexcept {
    switch (family) {
    case PineOrderFamily::Entry: return 1;
    case PineOrderFamily::Order: return 3;
    case PineOrderFamily::ExitLimit:
    case PineOrderFamily::ExitStop:
    case PineOrderFamily::ExitTrail: return 2;
    case PineOrderFamily::Close:
    case PineOrderFamily::CloseAll:
    case PineOrderFamily::Margin:
    case PineOrderFamily::Risk: return 0;
    }
    return 0;
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

double directional_tick(double value, double tick, bool upward) noexcept {
    if (!std::isfinite(value) || !finite_positive(tick)) return value;
    const double scaled = value / tick;
    return (upward ? std::ceil(scaled - 1e-12) : std::floor(scaled + 1e-12)) * tick;
}

double floor_quantity_grid(double units, const std::optional<double>& grid) noexcept {
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (!grid || !std::isfinite(*grid) || *grid <= 0.0) return units;
    return std::floor(units / *grid + 1e-12) * *grid;
}

double source_money_round(double value) noexcept {
    if (!std::isfinite(value) || value == 0.0) return value;
    const double magnitude = std::floor(std::log10(std::abs(value)));
    const double scale = std::pow(10.0, 9.0 - magnitude);
    const double rounded = std::floor(std::abs(value) * scale + 0.5) / scale;
    return value < 0.0 ? -rounded : rounded;
}

double source_money_floor_lot(double units, const std::optional<double>& grid) noexcept {
    if (!grid || !std::isfinite(*grid) || *grid <= 0.0) return units;
    if (!std::isfinite(units) || units <= 0.0) return units;
    double floored = std::floor(units / *grid) * *grid;
    if (*grid == 0.01) {
        const double cent_candidate = std::floor(units * 100.0) * *grid;
        if (cent_candidate > floored && cent_candidate <= units) floored = cent_candidate;
    }
    return floored < units ? floored : units;
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
    pending_same_bar_commands_.clear();
    source_shadow_pending_.clear();
    pending_same_bar_close_qty_ = 0.0;
    pending_relative_exits_.clear();
    pending_coof_requests_.clear();
    live_handles_.clear();
    first_open_newborns_.clear();
    pending_view_handles_.clear();
    current_debited_applied_ordinals_.clear();
    intraday_loss_relabel_ordinals_.clear();
    receipt_cursor_ = 0;
    materializing_relative_ = false;
    current_position_cycle_ = 0;
    current_position_sign_ = 0;
    next_sequential_group_ = 0;
    coof_recalc_active_ = false;
    coof_first_open_ = false;
    coof_context_ = {};
    coof_script_bar_ = {};
    coof_script_bar_valid_ = false;
    pooc_close_basis_by_script_bar_.clear();
    pooc_open_basis_ = 0.0;
    pooc_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
    close_all_pending_script_bar_ = std::numeric_limits<std::int64_t>::min();
    last_fx_rate_ = kNaN;
    position_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
    cap_latest_fill_ = 0;
    day_ledger_ = {};
    risk_.observed_peak_equity = kNaN;
    risk_.observed_max_drawdown = 0.0;
    risk_.intraday_block_day = std::numeric_limits<std::int64_t>::min();
    risk_.intraday_cancel_pending = false;
    policy_script_bar_ = {};
    policy_script_bar_valid_ = false;
    stream_mode_ = false;
    short_seed_ = {};
    short_seed_candidate_long_ = {};
    short_seed_candidate_materialize_ = {};
    short_seed_candidate_final_short_ = {};
    last_bar_dual_entry_path_ = 0;
    source_sequence_ = 0;
    cap.reset_run();
    refresh_pending_view();
}

void PineExecutionAdapter::set_configuration(const PineStrategyConfig& config) noexcept { config_ = config; }
void PineExecutionAdapter::set_staged_configuration(const StagedConfiguration& staged) { staged_ = staged; }
void PineExecutionAdapter::set_begin_mode(bool is_stream) noexcept { stream_mode_ = is_stream; }

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
    // A historical one-bar run has no detectable timeframe, but a stream
    // begin carries an explicit provider timeframe even when its warmup has
    // only one bar.  Preserve that public stream contract rather than
    // erasing the caller's labels into the undetected batch shape.
    // The simple begin has no timeframe argument to preserve when fewer than
    // two bars cannot establish one. A TF-aware public begin is explicit even
    // for one historical bar, and follows the legacy run_tf_impl path.
    spec.timeframe_undetected = args.n < 2 && !args.is_stream
        && args.input_tf.empty() && args.script_tf.empty();
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
    // Pine's request_abort surface reports a cooperative cancellation through
    // status, not through last_error(). Native-only hosts retain Error.
    spec.abort_reporting = NativeAbortReporting::Quiet;
    // Pine's pyramiding gate is source-command policy (including its
    // same-bar frozen-market exception), so leave one generic lot of headroom
    // for the source-side transaction batch and enforce ordinary additions in
    // entry() before they reach native matching. The variable-size batch is
    // the frozen partial-equity/cash form; 100%-equity retains its ordinary
    // all-in admission path.
    if (config.pyramiding > 0) {
        const bool batch_headroom = config.default_qty_type == static_cast<int>(QtyType::FIXED)
            || config.default_qty_type == static_cast<int>(QtyType::CASH)
            || (config.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
                && config.default_qty_value < 100.0);
        spec.max_open_lots = static_cast<std::uint64_t>(config.pyramiding)
            + (batch_headroom ? 1U : 0U);
    }
    spec.allowed_open_directions = directions_for(risk_.direction);
    // Pine's frozen default sizing admits against its signal-time tuple.  The
    // generic initial-margin gate only sees the later fill-time FX rate, so
    // source admission is reproduced in validate_precommit instead.
    if (args.bar_magnifier) {
        const bool synthesized = spec.timeframe_undetected || spec.input_tf == spec.script_tf;
        if (synthesized) {
            IntrabarPath::synthesized path;
            path.samples = args.magnifier_samples;
            path.distribution = args.magnifier_distribution;
            path.volume_weighted = args.magnifier_volume_weighted;
            path.volume_weighted_min_samples = args.magnifier_volume_weighted_min_samples;
            path.volume_weighted_max_samples = args.magnifier_volume_weighted_max_samples;
            spec.intrabar.value = std::move(path);
        } else {
            // A18: a genuinely finer supplied feed remains a retained
            // lower-timeframe path. The generic validator owns duration and
            // divisibility rejection for any non-finer malformed pairing.
            IntrabarPath::lower_tf path;
            if (args.bars && args.n > 0) path.bars.assign(args.bars, args.bars + args.n);
            path.tf = spec.input_tf;
            path.samples = args.magnifier_samples;
            path.distribution = args.magnifier_distribution;
            path.volume_weighted = args.magnifier_volume_weighted;
            path.volume_weighted_min_samples = args.magnifier_volume_weighted_min_samples;
            path.volume_weighted_max_samples = args.magnifier_volume_weighted_max_samples;
            path.sample_eligibility = IntrabarPath::SampleEligibility::DistributionSamples;
            spec.intrabar.value = std::move(path);
        }
    } else if (!spec.timeframe_undetected
               && (args.magnifier_samples != 4
                   || args.magnifier_distribution != MagnifierDistribution::ENDPOINTS)) {
        // Legacy TF-aware callers accept inactive sampler arguments. Preserve
        // that source-surface shape without relaxing the strict native public
        // API: an empty lower path is already defined to fall back to the
        // caller's confirmed script-bar path at delivery.
        IntrabarPath::lower_tf inert;
        inert.tf = spec.input_tf;
        inert.samples = 4;
        inert.distribution = MagnifierDistribution::ENDPOINTS;
        inert.volume_weighted = false;
        inert.volume_weighted_min_samples = 2;
        inert.volume_weighted_max_samples = 64;
        inert.sample_eligibility = IntrabarPath::SampleEligibility::ContinuousSegments;
        spec.intrabar.value = std::move(inert);
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
    if (const auto point = host.current_execution_point())
        snapshot.fx = active_staged_fx(point->decision.sub_bar_open_ms);
    return snapshot;
}

double PineExecutionAdapter::default_sizing_units(const PineSizingSnapshot& sizing) const noexcept {
    if (!finite_positive(sizing.price) || !finite_positive(sizing.fx)) return 0.0;
    if (config_.default_qty_type == static_cast<int>(QtyType::CASH)) {
        const double denominator = sizing.price * staged_.syminfo.pointvalue * sizing.fx;
        return finite_positive(denominator)
            ? floor_quantity_grid(config_.default_qty_value / denominator, staged_.quantity_grid)
            : 0.0;
    }
    if (config_.default_qty_type != static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        || !finite_positive(sizing.equity)) {
        return 0.0;
    }
    const double equity = staged_.quantity_grid ? source_money_round(sizing.equity) : sizing.equity;
    double cash = config_.default_qty_value / 100.0 * equity;
    if (config_.commission_type == static_cast<int>(CommissionType::PERCENT)
        && config_.commission_value > 0.0) {
        cash /= 1.0 + config_.commission_value / 100.0;
    }
    const double denominator = sizing.price * staged_.syminfo.pointvalue * sizing.fx;
    if (!finite_positive(denominator)) return 0.0;
    const double units = cash / denominator;
    return staged_.quantity_grid ? source_money_floor_lot(units, staged_.quantity_grid)
                                 : floor_quantity_grid(units, staged_.quantity_grid);
}

bool PineExecutionAdapter::same_bar_market_tx_scope() const {
    const bool all_in_percent = config_.default_qty_type
        == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        && config_.default_qty_value >= 100.0;
    const bool fixed_default = config_.default_qty_type == static_cast<int>(QtyType::FIXED);
    const bool variable_default = config_.default_qty_type == static_cast<int>(QtyType::CASH)
        || (config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
            && config_.default_qty_value < 100.0);
    const bool variable_short_seed = variable_default && host_
        && require_host().physical_position().signed_units < 0.0
        && cohort_exposure_for("Short") > 0.0;
    if (!host_ || config_.process_orders_on_close || config_.calc_on_order_fills
        || coof_recalc_active_ || config_.close_entries_rule_any
        || config_.pyramiding > 1 || all_in_percent
        || (!fixed_default && !variable_short_seed)
        || config_.slippage != 0 || config_.commission_value != 0.0
        || risk_.direction != 0 || risk_.max_cons_loss_days != 0
        || risk_.max_drawdown > 0.0 || risk_.max_intraday_loss > 0.0
        || risk_.max_position_size > 0.0 || risk_.halted || cap.active()) {
        return false;
    }
    const auto state = require_host().native_state();
    const auto* inert = state.spec ? state.spec->intrabar.lower() : nullptr;
    const bool inactive_sampler_path = inert != nullptr && inert->bars.empty()
        && inert->tf == state.spec->input_tf && inert->samples == 4
        && inert->distribution == MagnifierDistribution::ENDPOINTS
        && !inert->volume_weighted && inert->volume_weighted_min_samples == 2
        && inert->volume_weighted_max_samples == 64
        && inert->sample_eligibility == IntrabarPath::SampleEligibility::ContinuousSegments;
    return state.phase == NativeRunPhase::Batch && state.spec != nullptr
        && (state.spec->intrabar.is_none() || inactive_sampler_path);
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

void PineExecutionAdapter::retire(native_order::RequestHandle handle) noexcept {
    live_handles_.erase(std::remove(live_handles_.begin(), live_handles_.end(), handle),
                        live_handles_.end());
    first_open_newborns_.erase(std::remove(first_open_newborns_.begin(),
        first_open_newborns_.end(), handle), first_open_newborns_.end());
    for (auto it = live_by_source_key_.begin(); it != live_by_source_key_.end();) {
        if (it->second == handle) it = live_by_source_key_.erase(it); else ++it;
    }
    if (handle == short_seed_candidate_long_) short_seed_candidate_long_ = {};
    if (handle == short_seed_candidate_materialize_) short_seed_candidate_materialize_ = {};
    if (handle == short_seed_candidate_final_short_) short_seed_candidate_final_short_ = {};
    if (short_seed_.active && (handle == short_seed_.long_entry
        || handle == short_seed_.materialize_long || handle == short_seed_.final_short)) {
        short_seed_.active = false;
    }
    refresh_pending_view();
}

void PineExecutionAdapter::maybe_activate_short_seed_plan() {
    if (short_seed_.active || short_seed_.long_entry.incarnation != 0
        || short_seed_candidate_long_.incarnation == 0
        || short_seed_candidate_materialize_.incarnation == 0
        || short_seed_candidate_final_short_.incarnation == 0) {
        return;
    }
    short_seed_.long_entry = short_seed_candidate_long_;
    short_seed_.materialize_long = short_seed_candidate_materialize_;
    short_seed_.final_short = short_seed_candidate_final_short_;
    short_seed_.active = true;
}

std::optional<native_order::RequestHandle> PineExecutionAdapter::submit_or_replace(
        native_order::Request request, PlacementSnapshot snapshot, bool opening,
        const SourceId& replacement_key) {
    auto& host = require_host();
    const NativePhysicalPosition physical = host.physical_position();
    snapshot.projection_position_side = physical.signed_units > 0.0
        ? static_cast<std::int32_t>(PositionSide::LONG)
        : (physical.signed_units < 0.0 ? static_cast<std::int32_t>(PositionSide::SHORT)
                                       : static_cast<std::int32_t>(PositionSide::FLAT));
    snapshot.projection_after_close = pending_same_bar_close_qty_ > 0.0;
    snapshot.projection_over_pyramiding = opening && config_.pyramiding > 0
        && ((physical.signed_units > 0.0) == snapshot.is_long)
        && physical.signed_units != 0.0
        && physical.lot_count >= static_cast<std::size_t>(config_.pyramiding);
    snapshot.projection_created_during_coof = coof_recalc_active_;
    snapshot.projection_coof_at_terminal = coof_recalc_active_
        && coof_context_.is_terminal_sub_bar;
    snapshot.projection_coof_mid_bar = coof_recalc_active_
        && !coof_context_.is_terminal_sub_bar;
    snapshot.projection_tv_carry_qty = std::abs(physical.signed_units);
    snapshot.projection_default_stop_equity = snapshot.sizing.equity;
    snapshot.projection_default_stop_signal_close = snapshot.sizing.mark;
    snapshot.projection_explicit_equity = std::isfinite(snapshot.requested_qty)
        ? snapshot.sizing.equity : kNaN;
    snapshot.projection_explicit_signal_close = std::isfinite(snapshot.requested_qty)
        ? snapshot.sizing.price : kNaN;
    snapshot.projection_affordability_equity = snapshot.sizing.equity;
    snapshot.projection_affordability_signal_price = snapshot.sizing.price;
    snapshot.projection_affordability_held_qty = std::abs(physical.signed_units);
    if (const auto point = host.current_execution_point()) {
        snapshot.projection_created_bar = point->decision.coordinate.interval_index;
        snapshot.placement_script_open_ms = point->decision.script_bar_open_ms;
        snapshot.placement_sub_open_ms = point->decision.sub_bar_open_ms;
    }
    if (auto* member = std::get_if<native_order::Member>(&request.group)) {
        if (source_sequence_ >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::overflow_error("Pine OCA member sequence exhausted");
        }
        // The generic group member's cohort distinguishes siblings. A source
        // OCA name identifies the group; every accepted source instruction is
        // a distinct member of it, including replacement incarnations.
        member->cohort = static_cast<std::int64_t>(source_sequence_ + 1U);
    }
    const auto key = replacement_key.empty() ? 0 : key_for(replacement_key);
    std::optional<native_order::RequestHandle> accepted;
    std::optional<PlacementSnapshot> predecessor_snapshot;
    if (key != 0) {
        std::optional<native_order::RequestHandle> existing_handle;
        if (const auto existing = live_by_source_key_.find(key);
            existing != live_by_source_key_.end()) {
            // `retire` removes this key from live_by_source_key_. Keep the
            // handle independent of the map node before either operation.
            existing_handle = existing->second;
            if (const auto previous = placement_.find(existing_handle->incarnation);
                previous != placement_.end()) {
                predecessor_snapshot = previous->second;
            }
        }
        if (existing_handle) {
            const auto result = host.replace(*existing_handle, request);
            if (result.status == native_order::ReplaceStatus::Replaced && result.successor) {
                snapshot.projection_predecessor = existing_handle->incarnation;
                if (predecessor_snapshot) {
                    const auto family = predecessor_snapshot->family;
                    snapshot.projection_predecessor_exit = family == PineOrderFamily::ExitLimit
                        || family == PineOrderFamily::ExitStop || family == PineOrderFamily::ExitTrail;
                    snapshot.projection_predecessor_market = family == PineOrderFamily::Entry
                        && !std::isfinite(predecessor_snapshot->exit_levels.limit)
                        && !std::isfinite(predecessor_snapshot->exit_levels.stop)
                        && !std::isfinite(predecessor_snapshot->exit_levels.trail_offset);
                }
                retire(*existing_handle);
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

double PineExecutionAdapter::active_staged_fx(std::int64_t timestamp_ms) const noexcept {
    double rate = staged_.account_fx;
    const std::size_t count = std::min(staged_.account_fx_effective_from_ms.size(),
                                       staged_.account_fx_per_quote.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (staged_.account_fx_effective_from_ms[i] > timestamp_ms) break;
        rate = staged_.account_fx_per_quote[i];
    }
    return std::isfinite(rate) && rate > 0.0 ? rate : 1.0;
}

void PineExecutionAdapter::submit_fx_margin_slice(
        const Bar& bar, const NativeDecisionContext&, double rate) {
    const auto position = require_host().physical_position();
    const double held = std::abs(position.signed_units);
    const double margin = position.signed_units > 0.0 ? config_.margin_long : config_.margin_short;
    if (!source_margin_call_enabled_ || !(held > 0.0) || !finite_positive(margin) || margin != 100.0
        || !finite_positive(bar.open) || !finite_positive(staged_.syminfo.pointvalue)) return;
    const double required = held * bar.open * staged_.syminfo.pointvalue * rate;
    const double equity = require_host().native_marked_equity(bar.open);
    if (!(required > equity) || !std::isfinite(equity)) return;
    const double raw_minimum = (required - equity)
        / (bar.open * staged_.syminfo.pointvalue * rate);
    if (!(raw_minimum > 0.0) || !std::isfinite(raw_minimum)) return;
    double minimum = raw_minimum;
    if (staged_.quantity_grid) minimum = floor_quantity_grid(minimum, staged_.quantity_grid);
    double units = 0.0;
    if (minimum > 0.0) {
        // The source broker floors the restore quantity before applying its
        // fourfold liquidation multiplier, then floors the executable result.
        units = 4.0 * minimum;
        if (staged_.quantity_grid) units = floor_quantity_grid(units, staged_.quantity_grid);
    } else if (staged_.quantity_grid && *staged_.quantity_grid <= 1.0
               && raw_minimum > 1e-12 && raw_minimum < 1.0) {
        // A positive deficit which floors below one lot is discontinuous in
        // the legacy FX rollover path: it closes one whole contract (G2).
        const double candidate = std::min(1.0, held);
        const double gridded = floor_quantity_grid(candidate, staged_.quantity_grid);
        const double guard = std::max(1e-12, std::abs(candidate) * 1e-12);
        if (candidate >= held - 1e-12 || std::abs(gridded - candidate) <= guard)
            units = candidate;
    }
    units = std::min(held, units);
    if (!(units > 0.0) || !std::isfinite(units)) return;
    native_order::Request request;
    request.intent = native_order::Reduce{native_order::ExplicitUnits{units}};
    request.label = "__margin_call__";
    request.comment = "Margin call";
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Margin;
    snapshot.source_id = request.label;
    snapshot.requested_qty = units;
    snapshot.sizing = sizing_snapshot();
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false,
                                            "__margin_call__");
    if (accepted) {
        (void)require_host().execute_current({*accepted, NativeCurrentPriceRule::NearestTick});
    }
}

void PineExecutionAdapter::apply_fx_open_margin_slice(
        const Bar& bar, const NativeDecisionContext& context) {
    const double rate = active_staged_fx(context.sub_bar_open_ms);
    const double prior = last_fx_rate_;
    last_fx_rate_ = rate;
    if (!std::isfinite(prior) || prior == rate) return;
    const auto position = require_host().physical_position();
    if (position.signed_units == 0.0
        || position_open_script_bar_ >= context.script_bar_open_ms) return;
    const double margin = position.signed_units > 0.0 ? config_.margin_long : config_.margin_short;
    if (source_margin_call_enabled_ && finite_positive(margin) && margin != 100.0) {
        throw std::runtime_error(
            "timestamped account-currency FX broker-open rollover supports "
            "only carried 1x full-margin positions");
    }
    submit_fx_margin_slice(bar, context, rate);
}

void PineExecutionAdapter::apply_fx_opening_margin_slice(
        const native_order::ExecutionAppliedEvent& event,
        const NativeDecisionContext& context) {
    std::optional<PlacementSnapshot> opening_snapshot;
    if (const auto found = placement_.find(event.handle().incarnation);
        found != placement_.end()) {
        opening_snapshot = found->second;
    }
    if (!opening_snapshot || !opening_snapshot->opening
        || opening_snapshot->family != PineOrderFamily::Entry
        || !finite_positive(opening_snapshot->sizing.frozen_units)
        || config_.default_qty_type != static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        || config_.commission_type != static_cast<int>(CommissionType::PERCENT)
        || !(config_.commission_value > 0.0)) {
        return;
    }
    const double rate = active_staged_fx(context.sub_bar_open_ms);
    if (!std::isfinite(opening_snapshot->sizing.fx) || opening_snapshot->sizing.fx == rate) return;
    Bar opening;
    opening.open = event.resolved_price;
    opening.high = event.resolved_price;
    opening.low = event.resolved_price;
    opening.close = event.resolved_price;
    opening.timestamp = context.sub_bar_open_ms;
    submit_fx_margin_slice(opening, context, rate);
}

void PineExecutionAdapter::schedule_preopen_margin_slice(
        const Bar& bar, const NativeDecisionContext& context) {
    // The legacy broker checks the adverse excursion of a default-percent
    // stop entry's fill bar after the opening is admitted.  Submit its
    // source-owned close leg at the preceding open: BindCohort resolves only
    // after that opening applies, then the ordinary native path matches the
    // adverse waypoint in the same bar.  This keeps the liquidation policy
    // entirely above the generic driver.
    if (!source_margin_call_enabled_
        || require_host().physical_position().signed_units != 0.0
        || config_.default_qty_type != static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        || !finite_positive(staged_.syminfo.pointvalue)
        || !finite_positive(staged_.syminfo.mintick)
        || !finite_positive(bar.open)) {
        return;
    }

    // A pre-open margin submission can append to live_handles_ and insert
    // into placement_; scan value copies rather than retaining either
    // container's elements across submit_or_replace.
    const auto live_handles = live_handles_;
    for (const auto& handle : live_handles) {
        std::optional<PlacementSnapshot> opening_copy;
        if (const auto found = placement_.find(handle.incarnation);
            found != placement_.end()) {
            opening_copy = found->second;
        }
        if (!opening_copy) continue;
        const PlacementSnapshot& opening = *opening_copy;
        if (!opening.opening || opening.family != PineOrderFamily::Entry
            || !finite_positive(opening.sizing.frozen_units)
            || !finite_positive(opening.exit_levels.stop)) {
            continue;
        }
        const bool marketable = opening.is_long
            ? opening.exit_levels.stop <= bar.open
            : opening.exit_levels.stop >= bar.open;
        if (!marketable) continue;

        const double entry = nearest_tick(bar.open, staged_.syminfo.mintick);
        const double adverse_raw = opening.is_long ? bar.low : bar.high;
        const double adverse = nearest_tick(adverse_raw, staged_.syminfo.mintick);
        if (!finite_positive(entry) || !finite_positive(adverse)
            || (opening.is_long ? !(adverse < entry) : !(adverse > entry))) {
            continue;
        }
        const double fx = active_staged_fx(context.sub_bar_open_ms);
        const double margin = opening.is_long ? config_.margin_long : config_.margin_short;
        if (!finite_positive(fx) || !finite_positive(margin)) continue;
        const double units = opening.sizing.frozen_units;
        const double unrealized = (opening.is_long ? adverse - entry : entry - adverse)
            * units * staged_.syminfo.pointvalue * fx;
        const double marked_equity = opening.sizing.equity + unrealized;
        const double required = units * adverse * staged_.syminfo.pointvalue * fx
            * margin / 100.0;
        const double unit_margin = adverse * staged_.syminfo.pointvalue * fx
            * margin / 100.0;
        if (!std::isfinite(marked_equity) || !std::isfinite(required)
            || !finite_positive(unit_margin) || !(required > marked_equity)) {
            continue;
        }
        double restore = floor_quantity_grid((required - marked_equity) / unit_margin,
                                             staged_.quantity_grid);
        double slice = floor_quantity_grid(4.0 * restore, staged_.quantity_grid);
        slice = std::min(units, slice);
        if (!finite_positive(slice)) continue;

        native_order::Request request;
        request.intent = native_order::HostSized{native_order::HostSizedKind::Close, std::nullopt};
        request.label = "__margin_preopen__" + opening.source_id;
        request.comment = "Margin call";
        // Preserve the exact source adverse waypoint as the generic trigger;
        // terms rounds its resulting fill by the source tick rule.
        request.trigger = native_order::Stop{adverse_raw};
        request.owner = native_order::BindCohort{cohort_for(opening.source_id)};
        PlacementSnapshot snapshot;
        snapshot.family = PineOrderFamily::Margin;
        snapshot.source_id = request.label;
        snapshot.from_entry = opening.source_id;
        snapshot.comment = request.comment;
        snapshot.requested_qty = slice;
        snapshot.sizing = opening.sizing;
        (void)submit_or_replace(std::move(request), std::move(snapshot), false,
                                "__margin_preopen__" + opening.source_id);
        // The source stop-entry row is unique under this pre-open condition;
        // a second candidate belongs to a later source evaluation.
        return;
    }
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

void PineExecutionAdapter::cancel_bracket_origin(native_order::RequestHandle origin) {
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

void PineExecutionAdapter::cancel_bracket_siblings(native_order::RequestHandle handle) {
    std::optional<PlacementSnapshot> snapshot_copy;
    if (const auto source = placement_.find(handle.incarnation);
        source != placement_.end()) {
        snapshot_copy = source->second;
    }
    if (!snapshot_copy) return;
    const PlacementSnapshot& snapshot = *snapshot_copy;
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
    // A bracket born by the first-open COOF callback already has one durable
    // opening receipt. Bind that exact roster at the callback boundary so its
    // next real magnifier tick can consume it; later/deferred source commands
    // retain the growing cohort owner.
    if (dynamic && coof_recalc_active_ && coof_first_open_
        && found != cohorts_by_id_.end() && !found->second.opened.empty()) {
        return native_order::BindOpenings{found->second.opened, found->second.cycle};
    }
    if (dynamic || found == cohorts_by_id_.end()) {
        if (found == cohorts_by_id_.end())
            return native_order::BindCohort{const_cast<PineExecutionAdapter*>(this)->cohort_for(id)};
        return native_order::BindCohort{found->second.handle};
    }
    return native_order::BindOpenings{found->second.opened, found->second.cycle};
}

void PineExecutionAdapter::begin_coof_recalc(const NativeDecisionContext& context, bool first_open) {
    coof_recalc_active_ = true;
    coof_first_open_ = first_open;
    coof_context_ = context;
}

void PineExecutionAdapter::end_coof_recalc() noexcept {
    coof_recalc_active_ = false;
    coof_first_open_ = false;
    coof_context_ = {};
}

bool PineExecutionAdapter::defer_coof_tail() const noexcept {
    if (!coof_recalc_active_ || coof_first_open_) return false;
    const auto state = require_host().native_state();
    if (state.spec && state.spec->intrabar.lower()) return false;
    return coof_context_.coordinate.path_phase == NativePathPhase::Low
        || coof_context_.coordinate.path_phase == NativePathPhase::Close
        || coof_context_.coordinate.path_phase == NativePathPhase::None;
}

void PineExecutionAdapter::flush_coof_tail() {
    auto queued = std::move(pending_coof_requests_);
    pending_coof_requests_.clear();
    for (auto& pending : queued) {
        const bool execute_at_open = pending.opening
            && std::holds_alternative<native_order::Market>(pending.request.trigger)
            && std::holds_alternative<native_order::ImmediateRemaining>(pending.request.capacity)
            && require_host().current_execution_point().has_value();
        const auto accepted = submit_or_replace(std::move(pending.request), std::move(pending.snapshot),
                                                pending.opening, pending.replacement_key);
        if (accepted && pending.family_key != 0)
            bracket_families_[pending.family_key].push_back(*accepted);
        // A cascade market command held over after the final eligible
        // extreme is born at the following broker open. It is an open-point
        // execution, rather than a new C-tick candidate (COOF R2).
        if (accepted && execute_at_open)
            (void)require_host().execute_current({*accepted, NativeCurrentPriceRule::NearestTick});
    }
}

void PineExecutionAdapter::entry(const SourceId& id, bool is_long, double limit_price,
                                 double stop_price, double qty, const std::string& comment,
                                 const std::string& oca_name, int oca_type, int qty_type) {
    native_order::Request request;
    const bool default_sized = std::isnan(qty);
    const bool priced = !std::isnan(limit_price) || !std::isnan(stop_price);
    const bool explicit_fixed = !default_sized
        && (qty_type < 0 || qty_type == static_cast<int>(QtyType::FIXED));
    const double normalized_qty = explicit_fixed
        ? floor_quantity_grid(std::abs(qty), staged_.quantity_grid) : qty;
    const double signed_target = is_long ? normalized_qty : -normalized_qty;
    const double current = require_host().physical_position().signed_units;
    const auto source_point = require_host().current_execution_point();
    if (risk_.halted || intraday_loss_orders_blocked()
        || (source_point && cap_placement_denied(source_point->decision))) {
        return;
    }
    // Explicit entry quantities have a source placement-time admission
    // boundary.  In particular, non-finite units and finite values whose
    // required margin overflows must never become a live generic request that
    // waits until a later matching point to be rejected.
    if (!default_sized) {
        if (!std::isfinite(qty)) return;
        const bool opposite_live = current != 0.0 && ((current > 0.0) != is_long);
        if (!opposite_live) {
            // Admission is a source command fact at the signal mark; a
            // priced entry's later trigger/gap check remains at fill time.
            const double mark = source_point ? source_point->price : kNaN;
            const double margin = is_long ? config_.margin_long : config_.margin_short;
            const double fx = source_point
                ? active_staged_fx(source_point->decision.sub_bar_open_ms) : staged_.account_fx;
            const double equity = source_point
                ? require_host().native_marked_equity(source_point->price) : kNaN;
            const double required = std::abs(normalized_qty) * mark
                * staged_.syminfo.pointvalue * fx * margin / 100.0;
            if (margin <= 100.0 && (!std::isfinite(required) || !std::isfinite(equity)
                                    || required > equity)) {
                return;
            }
        }
    }
    const bool short_seed_long_candidate = current < 0.0 && is_long;
    const bool short_seed_final_candidate = current < 0.0 && !is_long
        && short_seed_candidate_long_.incarnation != 0;
    const bool same_bar_market_candidate = same_bar_market_tx_scope()
        && !priced && oca_name.empty()
        && (qty_type < 0 || qty_type == static_cast<int>(QtyType::FIXED))
        && (default_sized || finite_positive(qty));
    const bool all_in_percent = default_sized && !priced && oca_name.empty()
        && config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        && config_.default_qty_value >= 100.0;
    bool paired_all_in_reentry = false;
    if (all_in_percent && current != 0.0 && ((current > 0.0) == is_long) && source_point) {
        for (const auto& handle : live_handles_) {
            const auto prior = placement_.find(handle.incarnation);
            if (prior == placement_.end() || !prior->second.opening
                || prior->second.family != PineOrderFamily::Entry
                || prior->second.is_long == is_long) {
                continue;
            }
            paired_all_in_reentry = true;
            break;
        }
    }
    if (!same_bar_market_candidate && current != 0.0
        && ((current > 0.0) == is_long) && config_.pyramiding == 0) {
        // A zero pyramiding setting permits the flat opening but makes a
        // same-direction MARKET reissue a source no-op.  It must be dropped
        // before native matching so IntradayCap's factor-A policy observes no
        // fabricated physical fill.
        if (source_point) observe_intraday_cap_noop(is_long, source_point->decision);
        return;
    }
    if (!same_bar_market_candidate && config_.pyramiding > 0 && current != 0.0
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
        if (accepted_in_cycle >= static_cast<std::size_t>(config_.pyramiding)
            && !short_seed_final_candidate && !paired_all_in_reentry) return;
    }
    const auto current_point = source_point;
    const bool close_all_precedes = current_point
        && close_all_pending_script_bar_ == current_point->decision.script_bar_open_ms;
    const bool reverses = current != 0.0 && ((current > 0.0) != is_long) && !close_all_precedes;
    const bool direction_blocked = (risk_.direction > 0 && !is_long)
        || (risk_.direction < 0 && is_long);
    if (direction_blocked) {
        if (reverses) {
            native_order::Request close;
            close.intent = native_order::Flatten{};
            close.label = "__risk_close__" + id;
            PlacementSnapshot snapshot;
            snapshot.family = PineOrderFamily::CloseAll;
            snapshot.source_id = close.label;
            snapshot.sizing = sizing_snapshot();
            const SourceId replacement_key = close.label;
            submit_or_replace(std::move(close), std::move(snapshot), false, replacement_key);
        }
        return;
    }
    if (default_sized && reverses && current_point) {
        for (const auto& handle : live_handles_) {
            const auto pending = placement_.find(handle.incarnation);
            if (pending == placement_.end()) continue;
            const auto& prior = pending->second;
            if (prior.family == PineOrderFamily::Entry && prior.opening
                && !prior.is_long && prior.is_long == is_long && prior.replaced_opening
                && prior.replacement_predecessor_market
                && prior.placement_script_open_ms == current_point->decision.script_bar_open_ms) {
                // The replacement's transaction owns this source pass; a
                // later same-side default market command remains unfilled.
                return;
            }
        }
    }
    if (default_sized && reverses && is_long && current_point) {
        std::vector<native_order::RequestHandle> superseded_buy_replacements;
        for (const auto& handle : live_handles_) {
            const auto pending = placement_.find(handle.incarnation);
            if (pending == placement_.end()) continue;
            const auto& prior = pending->second;
            if (prior.family == PineOrderFamily::Entry && prior.opening && prior.is_long
                && prior.replaced_opening && prior.replacement_predecessor_market
                && prior.placement_script_open_ms == current_point->decision.script_bar_open_ms) {
                superseded_buy_replacements.push_back(handle);
            }
        }
        for (const auto& handle : superseded_buy_replacements) {
            cancel_bracket_origin(handle);
            const auto result = require_host().cancel(handle);
            if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
        }
    }
    const bool cash_sized = qty_type == static_cast<int>(QtyType::CASH);
    const bool fixed_priced_reverse = reverses && !default_sized && priced && !cash_sized;
    const bool cash_priced_reverse = reverses && !default_sized && priced && cash_sized;
    const bool default_stop_scope = default_sized && std::isnan(limit_price)
        && finite_positive(stop_price)
        && config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        && config_.default_qty_value <= 100.0;
    double default_stop_sizing_price = kNaN;
    if (default_stop_scope && finite_positive(staged_.syminfo.mintick)) {
        stop_price = directional_tick(stop_price, staged_.syminfo.mintick, is_long);
        const double signal = source_point
            ? nearest_tick(source_point->price, staged_.syminfo.mintick) : kNaN;
        const bool marketable = finite_positive(signal)
            && (is_long ? stop_price <= signal : stop_price >= signal);
        default_stop_sizing_price = marketable ? signal : stop_price;
    }
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
    if (coof_recalc_active_ && !coof_first_open_ && coof_script_bar_valid_
        && std::holds_alternative<native_order::Market>(request.trigger) && !defer_coof_tail()) {
        const auto phase = coof_context_.coordinate.path_phase;
        const double next_extreme = phase == NativePathPhase::High ? coof_script_bar_.low
            : (phase == NativePathPhase::Low ? coof_script_bar_.high : kNaN);
        const auto point = require_host().current_execution_point();
        const double current_quote = point ? point->price : kNaN;
        if (finite_positive(next_extreme) && finite_positive(current_quote)
            && next_extreme != current_quote) {
            const bool falling = next_extreme < current_quote;
            if (is_long) {
                request.trigger = falling ? native_order::Trigger{native_order::Limit{next_extreme}}
                                          : native_order::Trigger{native_order::Stop{next_extreme}};
            } else {
                request.trigger = falling ? native_order::Trigger{native_order::Stop{next_extreme}}
                                          : native_order::Trigger{native_order::Limit{next_extreme}};
            }
        }
    }
    request.group = group_for(oca_name, oca_type);
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Entry; snapshot.source_id = id; snapshot.comment = comment;
    snapshot.oca_name = oca_name; snapshot.oca_type = oca_type; snapshot.qty_type = qty_type;
    snapshot.requested_qty = normalized_qty; snapshot.is_long = is_long;
    snapshot.deferred_cohort = default_sized;
    // Reuse the durable level tuple for the parent trigger facts.  A deferred
    // relative exit may safely arm from a non-gap LIMIT parent's known entry
    // level before that parent is applied.
    snapshot.exit_levels.limit = limit_price;
    snapshot.exit_levels.stop = stop_price;
    snapshot.reverse_to = reverses || paired_all_in_reentry; snapshot.sizing = sizing_snapshot();
    const auto predecessor = live_by_source_key_.find(key_for(id));
    snapshot.replaced_opening = predecessor != live_by_source_key_.end();
    if (snapshot.replaced_opening) {
        const auto prior = placement_.find(predecessor->second.incarnation);
        snapshot.replacement_predecessor_market = prior != placement_.end()
            && !finite_positive(prior->second.exit_levels.limit)
            && !finite_positive(prior->second.exit_levels.stop);
    }
    const bool special_sell_replacement = default_sized && reverses && !is_long
        && snapshot.replaced_opening && snapshot.replacement_predecessor_market;
    if (special_sell_replacement && current_point) {
        std::vector<native_order::RequestHandle> superseded_siblings;
        for (const auto& handle : live_handles_) {
            const auto pending = placement_.find(handle.incarnation);
            if (pending == placement_.end()) continue;
            const auto& prior = pending->second;
            if (prior.family == PineOrderFamily::Entry && prior.opening
                && prior.source_id != id && prior.is_long == is_long
                && prior.placement_script_open_ms == current_point->decision.script_bar_open_ms) {
                superseded_siblings.push_back(handle);
            }
        }
        for (const auto& handle : superseded_siblings) {
            cancel_bracket_origin(handle);
            const auto result = require_host().cancel(handle);
            if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
        }
        std::vector<native_order::RequestHandle> carried_brackets;
        std::vector<SourceId> carried_ids;
        for (const auto& cohort : cohorts_by_id_) {
            for (const auto& origin : cohort.second.opened) {
                const auto prior = placement_.find(origin.incarnation);
                if (prior != placement_.end() && prior->second.is_long != is_long) {
                    carried_brackets.push_back(origin);
                    carried_ids.push_back(cohort.first);
                }
            }
        }
        for (const auto& origin : carried_brackets) cancel_bracket_origin(origin);
        std::vector<native_order::RequestHandle> dynamic_carried_legs;
        for (const auto& handle : live_handles_) {
            const auto leg = placement_.find(handle.incarnation);
            if (leg == placement_.end()) continue;
            const auto family = leg->second.family;
            if ((family == PineOrderFamily::ExitLimit || family == PineOrderFamily::ExitStop
                 || family == PineOrderFamily::ExitTrail)
                && std::find(carried_ids.begin(), carried_ids.end(), leg->second.from_entry)
                    != carried_ids.end()) {
                dynamic_carried_legs.push_back(handle);
            }
        }
        for (const auto& handle : dynamic_carried_legs) {
            const auto result = require_host().cancel(handle);
            if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
        }
    }
    snapshot.terms_priced_reverse = fixed_priced_reverse || cash_priced_reverse;
    snapshot.placement_cycle = current_position_cycle_;
    if (fixed_priced_reverse) {
        snapshot.frozen_reversal_transaction = std::abs(current) + normalized_qty;
    }
    if (default_stop_scope && finite_positive(default_stop_sizing_price)) {
        // A default-sized stop entry freezes its quantity against the
        // directionally snapped level, except an already-marketable stop
        // which is a next-open market order and therefore freezes at the
        // source close. Neither path re-sizes at its later fill quote.
        snapshot.sizing.price = default_stop_sizing_price;
    }
    if (default_sized && finite_positive(snapshot.sizing.price)) {
        if (config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
            || config_.default_qty_type == static_cast<int>(QtyType::CASH)) {
            snapshot.sizing.frozen_units = default_sizing_units(snapshot.sizing);
        }
        snapshot.sizing.at_fill = config_.calc_on_order_fills;
    }
    if (default_stop_scope && finite_positive(snapshot.sizing.frozen_units)
        && finite_positive(snapshot.sizing.mark)) {
        const double margin = is_long ? config_.margin_long : config_.margin_short;
        const double required = snapshot.sizing.frozen_units * snapshot.sizing.mark
            * staged_.syminfo.pointvalue * snapshot.sizing.fx * margin / 100.0;
        if (margin > 0.0 && (!std::isfinite(required) || !std::isfinite(snapshot.sizing.equity)
            || required > snapshot.sizing.equity)) {
            // Legacy replacement first removes the prior same-id resting
            // stop, then leaves the rejected re-issue absent from the book.
            std::optional<native_order::RequestHandle> prior_handle;
            if (const auto prior = live_by_source_key_.find(key_for(id));
                prior != live_by_source_key_.end()) {
                prior_handle = prior->second;
            }
            if (prior_handle) {
                const auto result = require_host().cancel(*prior_handle);
                if (result.status == native_order::CancelStatus::Cancelled) retire(*prior_handle);
            }
            return;
        }
    }
    if (const auto point = require_host().current_execution_point()) {
        snapshot.placement_script_open_ms = point->decision.script_bar_open_ms;
        snapshot.placement_sub_open_ms = point->decision.sub_bar_open_ms;
        if (default_sized && reverses && !snapshot.replaced_opening) {
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
    if (same_bar_market_candidate) {
        // Default percent/cash commands are already frozen at their source
        // call boundary.  The batch's topology must use that physical own
        // size, never the public percentage/cash scalar, just as the fixed
        // branch uses its explicit unit value.
        const double default_own = finite_positive(snapshot.sizing.frozen_units)
            ? snapshot.sizing.frozen_units : config_.default_qty_value;
        const double own_units = floor_quantity_grid(default_sized
                ? default_own : std::abs(qty), staged_.quantity_grid);
        bool opposite_market_pending = false;
        bool opposite_entry_pending = false;
        double opposite_pending_own = 0.0;
        const auto inspect_pending = [&](const PlacementSnapshot& prior) {
            if (!prior.opening || prior.source_id == id || prior.is_long == is_long) {
                return;
            }
            if (prior.frozen_market_instruction
                && finite_positive(prior.frozen_market_own_units)) {
                opposite_market_pending = true;
                opposite_pending_own += prior.frozen_market_own_units;
            } else {
                opposite_entry_pending = true;
            }
        };
        for (const auto& pending : pending_same_bar_commands_) inspect_pending(pending.snapshot);
        if (const auto point = require_host().current_execution_point()) {
            for (const auto& handle : live_handles_) {
                const auto prior = placement_.find(handle.incarnation);
                if (prior == placement_.end()
                    || prior->second.placement_script_open_ms
                        != point->decision.script_bar_open_ms) {
                    continue;
                }
                inspect_pending(prior->second);
            }
        }
        const bool same_side = current != 0.0 && ((current > 0.0) == is_long);
        const bool over_cap = same_side && config_.pyramiding > 0
            && require_host().physical_position().lot_count
                >= static_cast<std::size_t>(config_.pyramiding);
        if (over_cap && !opposite_market_pending && !opposite_entry_pending) return;
        if (!(over_cap && !opposite_market_pending)
            && finite_positive(own_units)) {
            const double held_opposite = current != 0.0 && ((current > 0.0) != is_long)
                ? std::max(0.0, std::abs(current) - pending_same_bar_close_qty_) : 0.0;
            const double transaction = own_units + held_opposite + opposite_pending_own;
            if (finite_positive(transaction)) {
                if (over_cap && opposite_market_pending
                    && config_.default_qty_type == static_cast<int>(QtyType::FIXED)) {
                    // The kept over-cap member is admitted at its source
                    // call as the whole frozen broker movement: held side,
                    // this member's own leg, and every opposite pending
                    // MARKET leg.  The eventual net position is smaller,
                    // but using it here would incorrectly admit famS's
                    // 3-lot ES/NQ census rows.
                    const double gross_units = std::abs(current) + own_units
                        + opposite_pending_own;
                    const double margin = is_long ? config_.margin_long : config_.margin_short;
                    const double required = gross_units * snapshot.sizing.price
                        * staged_.syminfo.pointvalue * snapshot.sizing.fx * margin / 100.0;
                    if (!std::isfinite(required) || !std::isfinite(snapshot.sizing.equity)
                        || required > snapshot.sizing.equity) {
                        return;
                    }
                }
                snapshot.opening = true;
                snapshot.frozen_market_instruction = true;
                snapshot.frozen_market_own_units = own_units;
                snapshot.frozen_market_transaction_units = transaction;
                auto existing = std::find_if(pending_same_bar_commands_.begin(),
                    pending_same_bar_commands_.end(), [&](const PendingSameBarCommand& row) {
                        return !row.snapshot.frozen_market_targeted_close
                            && row.replacement_key == id;
                    });
                PendingSameBarCommand pending{std::move(request), std::move(snapshot), id, true};
                if (existing == pending_same_bar_commands_.end()) {
                    pending_same_bar_commands_.push_back(std::move(pending));
                } else {
                    *existing = std::move(pending);
                }
                return;
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
    if (defer_coof_tail()) {
        pending_coof_requests_.push_back({std::move(request), std::move(snapshot), id, true, 0});
        return;
    }
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), true, id);
    if (accepted) {
        if (short_seed_long_candidate) short_seed_candidate_long_ = *accepted;
        if (short_seed_final_candidate) short_seed_candidate_final_short_ = *accepted;
    } else if (paired_all_in_reentry) {
        // The source call is still observable in its current script pass,
        // although native max-lot admission has already terminally refused
        // it. Preserve that truthful source observer row until the next
        // broker open, without retaining a second executable order.
        // (The copy is made from the source snapshot before the next call.)
        SourceShadowPending shadow;
        shadow.snapshot.family = PineOrderFamily::Entry;
        shadow.snapshot.source_id = id;
        shadow.snapshot.is_long = is_long;
        shadow.snapshot.opening = true;
        shadow.snapshot.sizing = sizing_snapshot();
        shadow.label = id;
        source_shadow_pending_.push_back(std::move(shadow));
    }
}

void PineExecutionAdapter::close(const SourceId& id, const std::string& comment, double qty,
                                 double qty_percent, bool immediately, std::uint64_t callsite_token) {
    if (intraday_loss_orders_blocked()) return;
    if (const auto point = require_host().current_execution_point();
        point && cap_placement_denied(point->decision)) {
        return;
    }
    // The public empty-id spelling is the source route's full-position
    // strategy.close form.  It is not a cohort lookup (there is no empty
    // entry-id cohort), and it retains its caller-supplied report comment.
    if (id.empty()) {
        if (const auto point = require_host().current_execution_point())
            close_all_pending_script_bar_ = point->decision.script_bar_open_ms;
        bool empty_entry = false;
        bool opposite_entry = false;
        bool empty_is_long = false;
        for (const auto& pending : pending_same_bar_commands_) {
            if (!pending.opening || pending.snapshot.family != PineOrderFamily::Entry) continue;
            if (pending.snapshot.source_id.empty()) {
                empty_entry = true;
                empty_is_long = pending.snapshot.is_long;
            }
        }
        if (empty_entry) {
            for (const auto& pending : pending_same_bar_commands_) {
                if (!pending.opening || pending.snapshot.family != PineOrderFamily::Entry) continue;
                if (pending.snapshot.is_long != empty_is_long) {
                    opposite_entry = true;
                    break;
                }
            }
        }
        if (empty_entry && opposite_entry) {
            SourceShadowPending shadow;
            shadow.snapshot.family = PineOrderFamily::CloseAll;
            shadow.snapshot.source_id = "__pine_close_all";
            shadow.snapshot.sizing = sizing_snapshot();
            shadow.label = shadow.snapshot.source_id;
            source_shadow_pending_.push_back(std::move(shadow));
            return;
        }
        native_order::Request request;
        request.intent = native_order::Flatten{};
        request.label = "__pine_close_all";
        request.comment = comment;
        PlacementSnapshot snapshot;
        snapshot.family = PineOrderFamily::CloseAll;
        snapshot.source_id = request.label;
        snapshot.comment = comment;
        snapshot.sizing = sizing_snapshot();
        (void)qty;
        (void)qty_percent;
        (void)immediately;
        (void)callsite_token;
        submit_or_replace(std::move(request), std::move(snapshot), false, "__pine_close_all");
        return;
    }
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
    const double current = require_host().physical_position().signed_units;
    // A partial source close breaks the exact ShortSeed transaction book.
    // Its legacy effect is to leave the two frozen reversal commands on the
    // ordinary broker pass; the stale close itself owns no surviving broker
    // object.  Detect that complete paired batch by source facts rather than
    // a generic id heuristic.
    if (same_bar_market_tx_scope() && !immediately && current != 0.0
        && (!std::isnan(qty) || !std::isnan(qty_percent))) {
        const bool variable_default = config_.default_qty_type
            != static_cast<int>(QtyType::FIXED);
        if (variable_default) {
            bool held_reentry = false;
            bool opposite_reversal = false;
            const bool held_long = current > 0.0;
            for (const auto& pending : pending_same_bar_commands_) {
                if (!pending.opening || pending.snapshot.family != PineOrderFamily::Entry) continue;
                if (pending.snapshot.source_id == id && pending.snapshot.is_long == held_long)
                    held_reentry = true;
                if (pending.snapshot.is_long != held_long) opposite_reversal = true;
            }
            if (held_reentry && opposite_reversal) {
                // The partial close has no executable artifact in the source
                // transaction pass.  Retain an accepted, dormant source
                // handle so the ShortSeed role projection remains truthful;
                // its unreachable buy limit prevents it from changing the
                // ordinary two-reversal outcome.
                if (id == "Short" && current < 0.0) {
                    native_order::Request placeholder;
                    placeholder.intent = native_order::HostSized{
                        native_order::HostSizedKind::Close, std::nullopt};
                    placeholder.label = "__close__" + id;
                    placeholder.trigger = native_order::Limit{
                        std::numeric_limits<double>::min()};
                    placeholder.owner = owner_for_close(id, true);
                    PlacementSnapshot placeholder_snapshot;
                    placeholder_snapshot.family = PineOrderFamily::Close;
                    placeholder_snapshot.source_id = id;
                    placeholder_snapshot.from_entry = id;
                    placeholder_snapshot.requested_qty = frozen_qty;
                    placeholder_snapshot.qty_percent = qty_percent;
                    placeholder_snapshot.deferred_cohort = true;
                    placeholder_snapshot.sizing = sizing_snapshot();
                    const auto accepted = submit_or_replace(
                        std::move(placeholder), std::move(placeholder_snapshot), false,
                        "__short_seed_partial_hold__" + id);
                    if (accepted) short_seed_candidate_materialize_ = *accepted;
                }
                return;
            }
        }
    }
    if (same_bar_market_tx_scope() && !immediately && id.size() != 0
        && std::isnan(qty) && std::isnan(qty_percent) && current != 0.0
        && finite_positive(frozen_qty)) {
        native_order::Request request;
        request.label = "__close__" + id;
        request.comment = comment;
        PlacementSnapshot snapshot;
        snapshot.family = PineOrderFamily::Close;
        snapshot.source_id = id;
        snapshot.from_entry = id;
        snapshot.comment = comment;
        snapshot.requested_qty = frozen_qty;
        snapshot.qty_percent = qty_percent;
        snapshot.frozen_market_instruction = true;
        snapshot.frozen_market_targeted_close = true;
        snapshot.frozen_market_target_was_long = current > 0.0;
        snapshot.sizing = sizing_snapshot();
        if (const auto point = require_host().current_execution_point()) {
            snapshot.placement_script_open_ms = point->decision.script_bar_open_ms;
            snapshot.placement_sub_open_ms = point->decision.sub_bar_open_ms;
        }
        const SourceId replacement_key = callsite_token == 0
            ? SourceId{} : id + "#close#" + std::to_string(callsite_token);
        pending_same_bar_commands_.push_back(
            {std::move(request), std::move(snapshot), replacement_key, false});
        pending_same_bar_close_qty_ += frozen_qty;
        return;
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
    // The all-in source collision retains a same-side re-entry which may be
    // rejected only at the next opening.  Its close must be a child of that
    // candidate: if the re-entry is refused, the legacy close is suppressed
    // rather than flattening the carried seed on its own.
    const bool all_in_percent = std::isnan(qty)
        && config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
        && config_.default_qty_value >= 100.0;
    bool all_in_dependent_close = false;
    if (all_in_percent) {
        const auto point = require_host().current_execution_point();
        if (point) {
            std::optional<native_order::RequestHandle> reentry;
            for (const auto& handle : live_handles_) {
                const auto found = placement_.find(handle.incarnation);
                if (found == placement_.end() || !found->second.opening
                    || found->second.family != PineOrderFamily::Entry
                    || found->second.source_id != id) {
                    continue;
                }
                reentry = handle;
            }
            if (reentry) {
                request.owner = native_order::WaitForApplied{*reentry};
                all_in_dependent_close = true;
            }
        }
    }
    // Only the generated callsite-token form represents source replacement.
    // Independent close statements in one evaluation must coexist (P1/P2).
    const SourceId replacement_key = callsite_token == 0
        ? SourceId{} : id + "#close#" + std::to_string(callsite_token);
    const PlacementSnapshot shadow_snapshot = snapshot;
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false, replacement_key);
    if (!accepted && all_in_dependent_close) {
        source_shadow_pending_.push_back({shadow_snapshot, "__close__" + id});
    }
    if (accepted && short_seed_candidate_long_.incarnation != 0
        && short_seed_candidate_final_short_.incarnation != 0) {
        short_seed_candidate_materialize_ = *accepted;
        maybe_activate_short_seed_plan();
    }
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
    if (intraday_loss_orders_blocked()) return;
    if (const auto point = require_host().current_execution_point();
        point && cap_placement_denied(point->decision)) {
        return;
    }
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
    if (intraday_loss_orders_blocked()) return;
    if (const auto point = require_host().current_execution_point();
        point && cap_placement_denied(point->decision)) {
        return;
    }
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
            if (defer_coof_tail()) {
                pending_coof_requests_.push_back({std::move(request), std::move(snapshot), replacement_key,
                                                  false, family_key});
                return;
            }
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
    flush_pending_same_bar_commands();
    auto queued = std::move(pending_entries_);
    pending_entries_.clear();
    for (auto& entry : queued) {
        (void)submit_or_replace(std::move(entry.request), std::move(entry.snapshot), true,
                                entry.replacement_key);
    }
}

void PineExecutionAdapter::flush_pending_same_bar_commands() {
    auto queued = std::move(pending_same_bar_commands_);
    pending_same_bar_commands_.clear();
    pending_same_bar_close_qty_ = 0.0;
    if (queued.empty()) return;

    // Legacy `finalize_same_bar_market_tx_book` retains command order within
    // each broker-side pass but moves every BUY member before every SELL
    // member.  The generic request core keeps submission order on an equal
    // point, so materialising the source batch in that order is sufficient
    // and does not add a source branch to generic matching.
    std::stable_sort(queued.begin(), queued.end(), [](const PendingSameBarCommand& left,
                                                       const PendingSameBarCommand& right) {
        const auto buy_rank = [](const PendingSameBarCommand& command) {
            if (command.snapshot.frozen_market_targeted_close)
                return command.snapshot.frozen_market_target_was_long ? 1 : 0;
            return command.snapshot.is_long ? 0 : 1;
        };
        return buy_rank(left) < buy_rank(right);
    });

    const bool single_entry = queued.size() == 1
        && !queued.front().snapshot.frozen_market_targeted_close;
    const double batch_start = require_host().physical_position().signed_units;
    double simulated = batch_start;
    std::optional<native_order::RequestHandle> short_seed_long;
    std::optional<native_order::RequestHandle> short_seed_materialize;
    std::optional<native_order::RequestHandle> short_seed_final;
    for (std::size_t i = 0; i < queued.size(); ++i) {
        auto& command = queued[i];
        auto request = std::move(command.request);
        auto snapshot = std::move(command.snapshot);
        bool opening = command.opening;
        const bool long_candidate = batch_start < 0.0 && opening
            && snapshot.family == PineOrderFamily::Entry && snapshot.is_long;
        const bool final_short_candidate = batch_start < 0.0 && opening
            && snapshot.family == PineOrderFamily::Entry && !snapshot.is_long;
        const bool materialize_candidate = batch_start < 0.0
            && snapshot.frozen_market_targeted_close
            && !snapshot.frozen_market_target_was_long;

        if (!snapshot.frozen_market_targeted_close) {
            const double units = snapshot.frozen_market_transaction_units;
            if (!finite_positive(units)) continue;
            if (single_entry) {
                const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), opening,
                                                        command.replacement_key);
                if (accepted && long_candidate) short_seed_long = *accepted;
                if (accepted && final_short_candidate) short_seed_final = *accepted;
                continue;
            }
            request.intent = native_order::Transact{snapshot.is_long ? units : -units};
            simulated += snapshot.is_long ? units : -units;
        } else {
            const double target = snapshot.requested_qty;
            if (!finite_positive(target) || simulated == 0.0) continue;
            const bool target_long = snapshot.frozen_market_target_was_long;
            const bool still_target_side = (simulated > 0.0) == target_long;
            const double units = std::min(target, std::abs(simulated));
            if (!finite_positive(units)) continue;
            if (still_target_side) {
                request.intent = native_order::Reduce{native_order::ExplicitUnits{units}};
                simulated += simulated > 0.0 ? -units : units;
            } else {
                // A default-FIFO close whose original side was consumed may
                // become the legacy artifact only when its same-id frozen
                // MARKET entry is still later in the sorted broker pass.
                bool artifact = false;
                for (std::size_t later = i + 1; later < queued.size(); ++later) {
                    const auto& sibling = queued[later];
                    if (!sibling.snapshot.frozen_market_targeted_close
                        && sibling.snapshot.frozen_market_instruction
                        && sibling.snapshot.source_id == snapshot.source_id
                        && sibling.snapshot.is_long == target_long) {
                        artifact = true;
                        break;
                    }
                }
                if (!artifact) continue;
                const double signed_units = simulated > 0.0 ? units : -units;
                request.intent = native_order::Transact{signed_units};
                simulated += signed_units;
                // This is a broker-created artifact lot carrying the close
                // label, not a new source-id cohort member.
                opening = false;
            }
        }
        const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), opening,
                                                command.replacement_key);
        if (accepted && long_candidate) short_seed_long = *accepted;
        if (accepted && materialize_candidate) short_seed_materialize = *accepted;
        if (accepted && final_short_candidate) short_seed_final = *accepted;
    }
    if (short_seed_long) short_seed_candidate_long_ = *short_seed_long;
    if (short_seed_materialize) short_seed_candidate_materialize_ = *short_seed_materialize;
    if (short_seed_final) short_seed_candidate_final_short_ = *short_seed_final;
    maybe_activate_short_seed_plan();
}

void PineExecutionAdapter::materialize_relative_exits(
        PlacementSnapshot opening, const native_order::ExecutionAppliedEvent& event) {
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
    // Cancellation can synchronously change source state. Copy the family
    // roster and release the map iterator before issuing any host operation.
    std::vector<native_order::RequestHandle> handles;
    bool found_family = false;
    if (const auto found = bracket_families_.find(key); found != bracket_families_.end()) {
        handles = found->second;
        bracket_families_.erase(found);
        found_family = true;
    }
    if (!found_family) return;
    for (const auto& handle : handles) {
        const auto result = require_host().cancel(handle);
        if (result.status == native_order::CancelStatus::Cancelled) retire(handle);
    }
}

void PineExecutionAdapter::cancel(const SourceId& id) {
    pending_same_bar_commands_.erase(std::remove_if(pending_same_bar_commands_.begin(),
        pending_same_bar_commands_.end(), [&](const PendingSameBarCommand& command) {
            return command.snapshot.source_id == id;
        }), pending_same_bar_commands_.end());
    pending_same_bar_close_qty_ = 0.0;
    for (const auto& command : pending_same_bar_commands_) {
        if (command.snapshot.frozen_market_targeted_close)
            pending_same_bar_close_qty_ += command.snapshot.requested_qty;
    }
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
    pending_same_bar_commands_.clear();
    pending_same_bar_close_qty_ = 0.0;
    pending_relative_exits_.clear();
}

void PineExecutionAdapter::order(const SourceId& id, bool is_long, double qty,
                                 double limit_price, double stop_price,
                                 const std::string& oca_name, int oca_type) {
    if (risk_.halted || intraday_loss_orders_blocked()) return;
    if (const auto point = require_host().current_execution_point();
        point && cap_placement_denied(point->decision)) {
        return;
    }
    native_order::Request request;
    const bool default_sized = std::isnan(qty);
    const double normalized_qty = default_sized ? qty
        : floor_quantity_grid(std::abs(qty), staged_.quantity_grid);
    request.intent = default_sized
        ? native_order::OrderIntent{native_order::HostSized{native_order::HostSizedKind::Open,
            is_long ? native_order::Side::Long : native_order::Side::Short}}
        : native_order::OrderIntent{native_order::Transact{is_long ? normalized_qty : -normalized_qty}};
    request.label = id; request.trigger = trigger_for(limit_price, stop_price, kNaN, kNaN);
    request.group = group_for(oca_name, oca_type);
    if (default_sized && oca_type == 2) {
        if (auto* member = std::get_if<native_order::Member>(&request.group)) {
            // Pine's default-sized RAW sibling is cancelled after an OCA
            // reduce member fills; only an explicit quantity consumes the
            // group reduction as a residual working amount.
            member->effect = native_order::GroupEffect::Cancel;
        }
    }
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Order; snapshot.source_id = id; snapshot.oca_name = oca_name;
    snapshot.oca_type = oca_type; snapshot.requested_qty = normalized_qty; snapshot.is_long = is_long;
    snapshot.sizing = sizing_snapshot();
    if (default_sized && finite_positive(snapshot.sizing.price)
        && (config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
            || config_.default_qty_type == static_cast<int>(QtyType::CASH))) {
        snapshot.sizing.frozen_units = default_sizing_units(snapshot.sizing);
        snapshot.sizing.at_fill = config_.calc_on_order_fills;
    }
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
    if (!std::holds_alternative<native_order::HostSized>(facts.definition->request.intent)) {
        if (std::holds_alternative<native_order::Market>(facts.definition->request.trigger)) {
            result.resolved_price = nearest_tick(result.resolved_price, staged_.syminfo.mintick);
        }
        if (finite_positive(source.forced_execution_price)) {
            result.resolved_price = nearest_tick(source.forced_execution_price,
                                                 staged_.syminfo.mintick);
        }
        return result;
    }
    double resolved = facts.default_resolved_price;
    const bool market_like = std::holds_alternative<native_order::Market>(facts.definition->request.trigger);
    if (market_like && config_.slippage != 0 && finite_positive(staged_.syminfo.mintick)) {
        resolved += facts.is_buy ? config_.slippage * staged_.syminfo.mintick
                                 : -config_.slippage * staged_.syminfo.mintick;
    }
    if (market_like) result.resolved_price = nearest_tick(resolved, staged_.syminfo.mintick);
    if (finite_positive(source.forced_execution_price)) {
        result.resolved_price = nearest_tick(source.forced_execution_price,
                                             staged_.syminfo.mintick);
    }
    // Source stop/trail exits crossed inside a modeled path settle at their
    // armed level, whereas an open gap retains the presented open quote.  The
    // generic driver deliberately exposes both facts; selecting this source
    // policy here preserves the non-gap relative-parent lifecycle.
    if ((source.family == PineOrderFamily::ExitStop || source.family == PineOrderFamily::ExitTrail
         || source.family == PineOrderFamily::Margin)
        && facts.trigger_level && facts.cursor.point.path_phase != NativePathPhase::Open) {
        result.resolved_price = nearest_tick(*facts.trigger_level, staged_.syminfo.mintick);
    }
    if (source.family == PineOrderFamily::Close || source.family == PineOrderFamily::ExitLimit
        || source.family == PineOrderFamily::ExitStop || source.family == PineOrderFamily::ExitTrail
        || source.family == PineOrderFamily::Margin) {
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
        const double fx = source.sizing.at_fill ? facts.active_fx : source.sizing.fx;
        const double denominator = price * staged_.syminfo.pointvalue * fx;
        double cash = equity * config_.default_qty_value / 100.0;
        if (config_.commission_type == static_cast<int>(CommissionType::PERCENT)
            && config_.commission_value > 0.0) {
            cash /= 1.0 + config_.commission_value / 100.0;
        }
        result.units = finite_positive(equity) && finite_positive(denominator)
            ? floor_quantity_grid(cash / denominator, staged_.quantity_grid) : 0.0;
    }
    if (source.family == PineOrderFamily::Order) {
        const bool opposite = facts.position.signed_units != 0.0
            && ((facts.position.signed_units > 0.0) != source.is_long);
        if (opposite) {
            result.units = std::min(*result.units, facts.opposite_book_units);
            result.shape = native_order::OpeningShape::CloseOpposite;
        }
        return result;
    }
    if (source.family == PineOrderFamily::Entry && source.sequential_group != 0
        && source.sequential_rank != 0 && source.has_full_entry_bracket) {
        bool paired = false;
        for (const auto& row : placement_) {
            const auto& peer = row.second;
            if (peer.family == PineOrderFamily::Entry
                && peer.sequential_group == source.sequential_group
                && peer.sequential_rank != 0 && peer.sequential_rank != source.sequential_rank
                && peer.has_full_entry_bracket
                && !(source.is_long && peer.replaced_opening
                     && peer.replacement_predecessor_market)) {
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
    // A same-id default-percent replacement over an opposite open book is a
    // source transaction (reduce the carried side by its frozen own size),
    // not the ordinary auto-reversal shape. The replacement fact is captured
    // before submit_or_replace retires its predecessor.
    if (source.reverse_to) {
        result.shape = source.replaced_opening && source.replacement_predecessor_market
            && !source.is_long
            ? native_order::OpeningShape::Transact : native_order::OpeningShape::ReverseTo;
    }
    return result;
}

NativePrecommitVerdict PineExecutionAdapter::validate_precommit(const NativePrecommitView& view) const {
    if (risk_.halted) return NativePrecommitVerdict::Refuse;
    if (intraday_loss_orders_blocked()) return NativePrecommitVerdict::Refuse;
    if (risk_.max_position_size > 0.0 && std::abs(view.inspected_opened_units) > risk_.max_position_size)
        return NativePrecommitVerdict::Refuse;
    if (risk_.max_cons_loss_days > 0 && day_ledger_.consecutive_loss_days >= risk_.max_cons_loss_days)
        return NativePrecommitVerdict::Refuse;
    if (!view.account.would_open) return NativePrecommitVerdict::Proceed;
    const auto snapshot = placement_.find(view.target.incarnation);
    if (snapshot == placement_.end()) return NativePrecommitVerdict::Refuse;
    const auto& source = snapshot->second;
    const bool variable_default = config_.default_qty_type != static_cast<int>(QtyType::FIXED);
    if (variable_default && !source.frozen_market_instruction && config_.pyramiding > 0
        && view.inspected_closed_units == 0.0
        && require_host().physical_position().lot_count
            >= static_cast<std::size_t>(config_.pyramiding)) {
        return NativePrecommitVerdict::Refuse;
    }
    const double margin_pct = view.account.incoming_short
        ? config_.margin_short : config_.margin_long;
    if (!(margin_pct > 0.0) || !std::isfinite(margin_pct))
        return NativePrecommitVerdict::Proceed;
    const double fraction = margin_pct / 100.0;
    if (finite_positive(source.sizing.frozen_units) && !source.sizing.at_fill) {
        const double frozen_required = std::abs(source.sizing.frozen_units)
            * source.sizing.price * staged_.syminfo.pointvalue * source.sizing.fx * fraction;
        if (!std::isfinite(frozen_required) || !std::isfinite(source.sizing.equity)
            || frozen_required > source.sizing.equity) {
            return NativePrecommitVerdict::Refuse;
        }
        // The frozen tuple protects a rate rollover (the FX opening checkpoint
        // owns that later adjustment), but an ordinary price gap is still
        // rechecked at the fill just as the legacy KI-54 admission path does.
        const double active_fx = active_staged_fx(view.cursor.point.effective_time_ms);
        if (active_fx == source.sizing.fx) {
            const double fill_required = view.account.resulting_abs_notional * fraction;
            const bool variable_batch = source.frozen_market_instruction
                && (config_.default_qty_type == static_cast<int>(QtyType::CASH)
                    || (config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
                        && config_.default_qty_value < 100.0));
            // An all-in source reversal remains admitted against the source
            // call snapshot.  Re-marking the carried side at a later gap
            // would manufacture buying power that the legacy precommit did
            // not grant (the ShortSeed all-in rejection controls).
            const bool all_in_reversal = source.reverse_to
                && config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
                && config_.default_qty_value >= 100.0;
            const double fill_equity = (variable_batch || all_in_reversal)
                ? source.sizing.equity : view.account.marked_equity;
            if (!std::isfinite(fill_required) || !std::isfinite(fill_equity)
                || fill_required > fill_equity) {
                return NativePrecommitVerdict::Refuse;
            }
        }
        return NativePrecommitVerdict::Proceed;
    }
    const double required = view.account.resulting_abs_notional * fraction;
    // A 1x LONG opening may be admitted against its pre-entry realized
    // budget even when the entry commission makes the post-entry marked
    // equity fractionally short.  The adapter immediately runs the
    // opening-price margin checkpoint from on_native_applied, which produces
    // the source-required 4x/one-contract reduction before any same-bar
    // exit.  Rejecting here would erase that observable margin event.
    const bool opening_margin_checkpoint =
        source.family == PineOrderFamily::Entry && margin_pct == 100.0
        && finite_positive(source.requested_qty)
        && std::isfinite(view.account.realized_balance)
        && required <= view.account.realized_balance;
    if (opening_margin_checkpoint) return NativePrecommitVerdict::Proceed;
    if (!std::isfinite(required) || !std::isfinite(view.account.marked_equity)
        || required > view.account.marked_equity) {
        return NativePrecommitVerdict::Refuse;
    }
    return NativePrecommitVerdict::Proceed;
}

std::int64_t PineExecutionAdapter::chart_day_key(std::int64_t timestamp_ms) const noexcept {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm fields{};
    const auto utc = [&]() {
        return ::gmtime_r(&seconds, &fields) != nullptr;
    };
    const std::string& timezone = staged_.chart_timezone;
    if (timezone.empty() || timezone == "UTC" || timezone == "Etc/UTC") {
        if (!utc()) return std::numeric_limits<std::int64_t>::min();
    } else {
        try {
            pine_tz::ScopedTimezone guard(timezone);
            if (::localtime_r(&seconds, &fields) == nullptr) {
                if (!utc()) return std::numeric_limits<std::int64_t>::min();
            }
        } catch (...) {
            // A malformed staged timezone must not turn a source-policy
            // read into an unhashable partial state.  The run-spec validator
            // owns rejection; preserve the legacy UTC fallback meanwhile.
            if (!utc()) return std::numeric_limits<std::int64_t>::min();
        }
    }
    return static_cast<std::int64_t>(fields.tm_mday) * 100
        + static_cast<std::int64_t>(fields.tm_mon + 1);
}

compat::pine::CapClock PineExecutionAdapter::cap_clock(
        const NativeDecisionContext& context) const {
    const std::int64_t key = chart_day_key(context.sub_bar_open_ms);
    return {context.sub_bar_open_ms,
            staged_.syminfo.session.empty() ? "24x7" : staged_.syminfo.session,
            staged_.syminfo.timezone.empty() ? "UTC" : staged_.syminfo.timezone,
            static_cast<int>(key / 100), static_cast<int>(key % 100)};
}

compat::pine::Calculation PineExecutionAdapter::cap_calculation(
        const NativeDecisionContext& context) const {
    return {config_.process_orders_on_close, config_.calc_on_order_fills,
            coof_recalc_active_, context.driver_statistics.intrabar_path_enabled,
            stream_mode_, !stream_mode_, !config_.close_entries_rule_any,
            context.coordinate.interval_index};
}

compat::pine::MatchedAttempt PineExecutionAdapter::cap_attempt(
        const PlacementSnapshot& snapshot) const {
    compat::pine::OrderKind kind = compat::pine::OrderKind::Other;
    if (snapshot.family == PineOrderFamily::Entry) {
        kind = (finite_positive(snapshot.exit_levels.limit)
                || finite_positive(snapshot.exit_levels.stop))
            ? compat::pine::OrderKind::Entry : compat::pine::OrderKind::Market;
    }
    const auto position = require_host().physical_position();
    const auto projected = static_cast<PositionSide>(snapshot.projection_position_side);
    // Factor-A's no-op filter is defined against the matched request's
    // pre-dispatch position.  Applied notifications observe the physical
    // book afterwards, so retain the truthful placement-side snapshot for a
    // flat opening rather than misclassifying its first fill as a no-op.
    const auto side = projected == PositionSide::LONG ? compat::pine::Side::Long
        : (projected == PositionSide::SHORT ? compat::pine::Side::Short
                                            : compat::pine::Side::Flat);
    const int live_entries = projected == PositionSide::FLAT
        ? 0 : static_cast<int>(position.lot_count);
    return {kind, snapshot.source_sequence, snapshot.projection_created_bar,
            snapshot.is_long, side, live_entries,
            config_.pyramiding};
}

bool PineExecutionAdapter::cap_placement_denied(const NativeDecisionContext& context) {
    return cap.active() && cap.placement(cap_clock(context)) == compat::pine::Placement::Deny;
}

bool PineExecutionAdapter::intraday_loss_orders_blocked() const noexcept {
    return risk_.intraday_block_day != std::numeric_limits<std::int64_t>::min()
        && risk_.intraday_block_day == day_ledger_.current_day;
}

void PineExecutionAdapter::update_risk_state(double mark_price) {
    if (std::isfinite(mark_price)) {
        const double equity = require_host().native_marked_equity(mark_price);
        if (std::isfinite(equity)) {
            if (!std::isfinite(risk_.observed_peak_equity)
                || equity > risk_.observed_peak_equity) {
                risk_.observed_peak_equity = equity;
            }
            const double drawdown = risk_.observed_peak_equity - equity;
            if (drawdown > risk_.observed_max_drawdown)
                risk_.observed_max_drawdown = drawdown;
        }
    }
    if (risk_.halted) return;
    if (risk_.max_drawdown > 0.0 && std::isfinite(risk_.observed_peak_equity)) {
        const double threshold = risk_.max_drawdown_percent
            ? risk_.observed_peak_equity * risk_.max_drawdown / 100.0
            : risk_.max_drawdown;
        if (risk_.observed_max_drawdown >= threshold) {
            risk_.halted = true;
            return;
        }
    }
    if (risk_.max_cons_loss_days > 0
        && day_ledger_.consecutive_loss_days >= risk_.max_cons_loss_days) {
        risk_.halted = true;
    }
}

bool PineExecutionAdapter::intraday_loss_breached(double mark_price) const noexcept {
    if (!(risk_.max_intraday_loss > 0.0) || intraday_loss_orders_blocked()
        || !std::isfinite(day_ledger_.intraday_start_equity)
        || !std::isfinite(mark_price)) {
        return false;
    }
    const double equity = require_host().native_marked_equity(mark_price);
    const double loss = day_ledger_.intraday_start_equity - equity;
    const double threshold = risk_.max_intraday_loss_percent
        ? day_ledger_.intraday_start_equity * risk_.max_intraday_loss / 100.0
        : risk_.max_intraday_loss;
    if (!(threshold > 0.0) || !(loss > 0.0) || !std::isfinite(loss)) return false;
    const double epsilon = 1e-9 * std::max(1.0, std::abs(threshold));
    return loss + epsilon >= threshold;
}

bool PineExecutionAdapter::submit_margin_call_slice(
        double mark_price, const NativeDecisionContext& context, bool execute_current) {
    const auto position = require_host().physical_position();
    const double held = std::abs(position.signed_units);
    const double margin_pct = position.signed_units > 0.0
        ? config_.margin_long : config_.margin_short;
    if (!source_margin_call_enabled_ || !(held > 0.0)
        || !finite_positive(mark_price) || !finite_positive(margin_pct)
        || !finite_positive(staged_.syminfo.pointvalue)) {
        return false;
    }
    const double fx = active_staged_fx(context.sub_bar_open_ms);
    const double fraction = margin_pct / 100.0;
    const double unit_margin = mark_price * staged_.syminfo.pointvalue * fx * fraction;
    const double required = held * unit_margin;
    const double equity = require_host().native_marked_equity(mark_price);
    if (!finite_positive(unit_margin) || !std::isfinite(equity)
        || !(required > equity)) {
        return false;
    }
    const double raw_minimum = (required - equity) / unit_margin;
    if (!(raw_minimum > 0.0) || !std::isfinite(raw_minimum)) return false;
    double minimum = floor_quantity_grid(raw_minimum, staged_.quantity_grid);
    double units = minimum > 0.0
        ? floor_quantity_grid(4.0 * minimum, staged_.quantity_grid) : 0.0;
    if (!(units > 0.0) && staged_.quantity_grid
        && *staged_.quantity_grid <= 1.0 && raw_minimum < 1.0) {
        const double candidate = std::min(1.0, held);
        const double rounded = floor_quantity_grid(candidate, staged_.quantity_grid);
        const double guard = std::max(1e-12, std::abs(candidate) * 1e-12);
        if (candidate >= held - guard || std::abs(rounded - candidate) <= guard)
            units = candidate;
    }
    units = std::min(held, units);
    if (!(units > 0.0) || !std::isfinite(units)) return false;

    native_order::Request request;
    request.intent = native_order::Reduce{native_order::ExplicitUnits{units}};
    request.label = "__margin_call__";
    request.comment = "Margin call";
    if (!execute_current) request.trigger = native_order::Stop{mark_price};
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Margin;
    snapshot.source_id = request.label;
    snapshot.requested_qty = units;
    snapshot.sizing = sizing_snapshot();
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false,
                                            "__margin_call__");
    if (!accepted) return false;
    if (execute_current) {
        (void)require_host().execute_current({*accepted, NativeCurrentPriceRule::NearestTick});
    }
    return true;
}

void PineExecutionAdapter::schedule_margin_call_path(
        const Bar& bar, const NativeDecisionContext& context) {
    const auto position = require_host().physical_position();
    if (position.signed_units == 0.0) return;
    const double adverse = position.signed_units > 0.0 ? bar.low : bar.high;
    if (!finite_positive(adverse) || adverse == bar.open) return;
    if (position.signed_units > 0.0 ? !(adverse < bar.open) : !(adverse > bar.open))
        return;
    (void)submit_margin_call_slice(adverse, context, false);
}

bool PineExecutionAdapter::submit_intraday_loss_close(
        double mark_price, const NativeDecisionContext& context, bool execute_current) {
    if (!intraday_loss_breached(mark_price)
        || require_host().physical_position().signed_units == 0.0) {
        return false;
    }
    native_order::Request request;
    request.intent = native_order::Flatten{};
    // The legacy forced-close report has an empty exit id and this exact
    // comment.  An empty generic label is supported by the request algebra.
    request.comment = "Close Position (Max intraday Loss)";
    if (!execute_current) request.trigger = native_order::Stop{mark_price};
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Risk;
    snapshot.source_id = "__intraday_loss__";
    snapshot.comment = request.comment;
    snapshot.sizing = sizing_snapshot();
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false,
                                            "__intraday_loss_close__");
    if (!accepted) return false;
    if (execute_current) {
        const auto result = require_host().execute_current(
            {*accepted, NativeCurrentPriceRule::NearestTick});
        if (const auto* applied = std::get_if<native_order::ExecutionAppliedEvent>(&result);
            applied && applied->closed_units > 0.0) {
            risk_.intraday_block_day = chart_day_key(context.sub_bar_open_ms);
            risk_.intraday_cancel_pending = true;
        }
    }
    return true;
}

void PineExecutionAdapter::schedule_intraday_loss_path(
        const Bar& bar, const NativeDecisionContext& context) {
    const auto position = require_host().physical_position();
    if (position.signed_units == 0.0) return;
    const double adverse = position.signed_units > 0.0 ? bar.low : bar.high;
    if (!finite_positive(adverse) || adverse == bar.open) return;
    if (position.signed_units > 0.0 ? !(adverse < bar.open) : !(adverse > bar.open))
        return;
    (void)submit_intraday_loss_close(adverse, context, false);
}

void PineExecutionAdapter::execute_due_cap_close(const NativeDecisionContext& context) {
    const auto due = cap.due_cause();
    if (!due || context.coordinate.interval_index <= due->trigger_bar
        || require_host().physical_position().signed_units == 0.0) {
        return;
    }
    native_order::Request request;
    request.intent = native_order::Flatten{};
    request.comment = "Close Position (Max number of filled orders in one day)";
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::CloseAll;
    snapshot.source_id = "__intraday_cap_close__";
    snapshot.comment = request.comment;
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false,
                                            "__intraday_cap_close__");
    if (accepted) {
        (void)require_host().execute_current({*accepted, NativeCurrentPriceRule::NearestTick});
        cap.after_immediate_close_attempt();
    }
}

void PineExecutionAdapter::execute_cap_close_now(const compat::pine::CloseNow& close) {
    if (require_host().physical_position().signed_units == 0.0) {
        cap.after_immediate_close_attempt();
        return;
    }
    native_order::Request request;
    request.intent = native_order::Flatten{};
    request.comment = close.request.comment;
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::CloseAll;
    snapshot.source_id = "__intraday_cap_close__";
    snapshot.comment = close.request.comment;
    snapshot.forced_execution_price = close.price;
    const auto accepted = submit_or_replace(std::move(request), std::move(snapshot), false,
                                            "__intraday_cap_close__");
    if (accepted) {
        (void)require_host().execute_current({*accepted, NativeCurrentPriceRule::NearestTick});
    }
    cap.after_immediate_close_attempt();
}

void PineExecutionAdapter::observe_intraday_cap(
        const native_order::ExecutionAppliedEvent& event,
        const PlacementSnapshot& snapshot, const NativeDecisionContext& context) {
    if (snapshot.family == PineOrderFamily::Margin || snapshot.family == PineOrderFamily::Risk
        || snapshot.source_id == "__intraday_cap_close__")
        return;
    const auto clock = cap_clock(context);
    const auto calculation = cap_calculation(context);
    const auto attempt = cap_attempt(snapshot);
    const auto origin = cap.origin(clock, calculation, event.handle().incarnation, cap_latest_fill_);
    const auto admission = cap.pre_dispatch(clock, calculation, attempt, cap_latest_fill_);
    if (admission.dispatch == compat::pine::Dispatch::Decline) {
        cap.decline(event.handle().incarnation);
        return;
    }
    cap.outcome(compat::pine::FillOutcome::Committed, origin);
    const auto position = require_host().physical_position();
    const Bar& prices = policy_script_bar_valid_ ? policy_script_bar_ : coof_script_bar_;
    const auto decision = cap.post_dispatch(admission, calculation, attempt,
        position.signed_units > 0.0 ? compat::pine::Side::Long
        : (position.signed_units < 0.0 ? compat::pine::Side::Short
                                       : compat::pine::Side::Flat),
        current_position_cycle_,
        {event.resolved_price, prices.open, prices.high, prices.low});
    if (const auto* now = std::get_if<compat::pine::CloseNow>(&decision)) {
        execute_cap_close_now(*now);
    }
    cap_latest_fill_ = event.ordinal;
}

void PineExecutionAdapter::observe_intraday_cap_noop(
        bool is_long, const NativeDecisionContext& context) {
    if (!cap.active()) return;
    PlacementSnapshot snapshot;
    snapshot.family = PineOrderFamily::Entry;
    snapshot.is_long = is_long;
    snapshot.projection_position_side = is_long
        ? static_cast<std::int32_t>(PositionSide::LONG)
        : static_cast<std::int32_t>(PositionSide::SHORT);
    snapshot.projection_created_bar = context.coordinate.interval_index;
    snapshot.source_sequence = source_sequence_;
    const auto clock = cap_clock(context);
    const auto calculation = cap_calculation(context);
    const auto attempt = cap_attempt(snapshot);
    const auto origin = cap.origin(clock, calculation, 0, cap_latest_fill_);
    const auto admission = cap.pre_dispatch(clock, calculation, attempt, cap_latest_fill_);
    if (admission.dispatch == compat::pine::Dispatch::Decline) {
        cap.decline(0);
        return;
    }
    cap.outcome(compat::pine::FillOutcome::NoEffect, origin);
    const auto position = require_host().physical_position();
    const Bar& prices = policy_script_bar_valid_ ? policy_script_bar_ : coof_script_bar_;
    const auto decision = cap.post_dispatch(admission, calculation, attempt,
        position.signed_units > 0.0 ? compat::pine::Side::Long
        : (position.signed_units < 0.0 ? compat::pine::Side::Short
                                       : compat::pine::Side::Flat),
        current_position_cycle_,
        {context.coordinate.path_phase == NativePathPhase::None ? prices.close
                                                                 : require_host().current_execution_point()->price,
         prices.open, prices.high, prices.low});
    if (const auto* now = std::get_if<compat::pine::CloseNow>(&decision)) {
        execute_cap_close_now(*now);
    }
}

void PineExecutionAdapter::source_batch_end() {
    cap.source_batch_end();
}

void PineExecutionAdapter::on_bar_open(const Bar& bar, const NativeDecisionContext& context) {
    // Terminal entry refusals have no Applied notification.  Consume their
    // generic receipt before the next matching point so their deferred
    // per-origin bracket legs cannot close a different cohort member.
    observe_terminal_receipts();
    // The preceding source broker batch is complete at this next opening.
    // This is deliberately after any POOC after-calculation matching of the
    // prior script bar, so a same-batch cap transfer remains available to its
    // designated sibling.
    source_batch_end();
    source_shadow_pending_.clear();
    coof_script_bar_ = bar;
    coof_script_bar_valid_ = true;
    policy_script_bar_ = bar;
    policy_script_bar_valid_ = true;
    flush_coof_tail();
    if (close_all_pending_script_bar_ != context.script_bar_open_ms)
        close_all_pending_script_bar_ = std::numeric_limits<std::int64_t>::min();
    pooc_open_script_bar_ = context.script_bar_open_ms;
    pooc_open_basis_ = std::abs(require_host().physical_position().signed_units);
    day_ledger_.current_day = chart_day_key(context.sub_bar_open_ms);
    if (day_ledger_.intraday_loss_day != day_ledger_.current_day) {
        day_ledger_.intraday_loss_day = day_ledger_.current_day;
        day_ledger_.intraday_start_equity = require_host().native_marked_equity(bar.open);
        day_ledger_.intraday_realized = 0.0;
    }
    execute_due_cap_close(context);
    update_risk_state(bar.open);
    apply_fx_open_margin_slice(bar, context);
    (void)submit_margin_call_slice(bar.open, context, true);
    schedule_margin_call_path(bar, context);
    (void)submit_intraday_loss_close(bar.open, context, true);
    schedule_intraday_loss_path(bar, context);
    schedule_preopen_margin_slice(bar, context);
    cap.ordinary_open(context.coordinate.interval_index);
}

void PineExecutionAdapter::on_bar_close(const Bar& bar, const NativeDecisionContext&) {
    update_risk_state(bar.close);
}

void PineExecutionAdapter::on_applied(const native_order::ExecutionAppliedEvent& event,
                                      const NativeDecisionContext& context) {
    // materialize_relative_exits can submit new legs. A submission inserts
    // into placement_ and may rehash it, so no reference or iterator into
    // placement_ may survive that call.
    std::optional<PlacementSnapshot> placement_snapshot;
    if (const auto placement = placement_.find(event.handle().incarnation);
        placement != placement_.end()) {
        placement_snapshot = placement->second;
    }
    bool preclose_intraday_loss = false;
    if (event.closed_trade_count > 0 && risk_.max_intraday_loss > 0.0
        && !intraday_loss_orders_blocked()
        && std::isfinite(day_ledger_.intraday_start_equity)) {
        double closed_pnl = 0.0;
        const auto& host = require_host();
        for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
            const auto index = event.first_trade_index + i;
            if (index < static_cast<std::size_t>(host.trade_count()))
                closed_pnl += host.get_trade(static_cast<int>(index)).pnl;
        }
        const double after = host.native_marked_equity(event.resolved_price);
        const double before = after - closed_pnl;
        const double loss = day_ledger_.intraday_start_equity - before;
        const double threshold = risk_.max_intraday_loss_percent
            ? day_ledger_.intraday_start_equity * risk_.max_intraday_loss / 100.0
            : risk_.max_intraday_loss;
        const double epsilon = 1e-9 * std::max(1.0, std::abs(threshold));
        preclose_intraday_loss = std::isfinite(before) && threshold > 0.0
            && loss > 0.0 && loss + epsilon >= threshold;
    }
    const double live_position = require_host().physical_position().signed_units;
    const int next_sign = live_position > 0.0 ? 1 : (live_position < 0.0 ? -1 : 0);
    if (next_sign != 0 && (current_position_sign_ == 0 || current_position_sign_ != next_sign)) {
        ++current_position_cycle_;
        position_open_script_bar_ = context.script_bar_open_ms;
    }
    current_position_sign_ = next_sign;
    if (placement_snapshot && placement_snapshot->opening
        && std::abs(event.opened_units) > 0.0) {
        {
            auto& facts = cohorts_by_id_[placement_snapshot->source_id];
            facts.cycle = event.cycle_after;
            if (std::find(facts.opened.begin(), facts.opened.end(), event.handle()) == facts.opened.end())
                facts.opened.push_back(event.handle());
            facts.live_units_by_origin[event.handle().incarnation] += std::abs(event.opened_units);
        }
        materialize_relative_exits(*placement_snapshot, event);
    }
    const bool current_debit_observed =
        current_debited_applied_ordinals_.erase(event.ordinal) != 0;
    if (!current_debit_observed && placement_snapshot
        && !placement_snapshot->from_entry.empty()) {
        consume_cohort_units(placement_snapshot->from_entry, event);
    }
    if (placement_snapshot && placement_snapshot->family == PineOrderFamily::CloseAll
        && event.closed_units > 0.0) {
        for (auto& cohort : cohorts_by_id_) cohort.second.live_units_by_origin.clear();
    }
    if (require_host().physical_position().signed_units == 0.0) {
        position_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
        for (auto& cohort : cohorts_by_id_) {
            cohort.second.opened.clear();
            cohort.second.live_units_by_origin.clear();
        }
    }
    if (short_seed_.final_short.incarnation != 0 && event.handle() == short_seed_.final_short
        && config_.default_qty_type != static_cast<int>(QtyType::FIXED)) {
        short_seed_.report_swap_pending = true;
    }
    if (event.terminal) retire(event.handle());
    if (event.ordinal != day_ledger_.observed_applied_ordinal) {
        day_ledger_.observed_applied_ordinal = event.ordinal;
        const auto day = chart_day_key(context.sub_bar_open_ms);
        if (event.closed_trade_count > 0) {
            const auto& host = require_host();
            for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
                const auto index = event.first_trade_index + i;
                if (index >= static_cast<std::size_t>(host.trade_count())) continue;
                const double pnl = host.get_trade(static_cast<int>(index)).pnl;
                day_ledger_.intraday_realized += pnl;
                if (pnl < 0.0 && day != day_ledger_.last_loss_day) {
                    day_ledger_.last_loss_day = day;
                    if (day_ledger_.consecutive_loss_days
                        == std::numeric_limits<int>::max()) {
                        throw std::overflow_error("closed trade counter exhausted");
                    }
                    ++day_ledger_.consecutive_loss_days;
                } else if (pnl > 0.0) {
                    day_ledger_.consecutive_loss_days = 0;
                }
            }
        }
    }
    if (placement_snapshot) {
        observe_intraday_cap(event, *placement_snapshot, context);
        if (placement_snapshot->family == PineOrderFamily::Risk
            && event.closed_units > 0.0) {
            risk_.intraday_block_day = chart_day_key(context.sub_bar_open_ms);
            risk_.intraday_cancel_pending = true;
        }
    }
    if (preclose_intraday_loss) {
        risk_.intraday_block_day = chart_day_key(context.sub_bar_open_ms);
        risk_.intraday_cancel_pending = true;
        if (require_host().physical_position().signed_units != 0.0) {
            native_order::Request request;
            request.intent = native_order::Flatten{};
            request.comment = "Close Position (Max intraday Loss)";
            PlacementSnapshot snapshot;
            snapshot.family = PineOrderFamily::Risk;
            snapshot.source_id = "__intraday_loss__";
            snapshot.comment = request.comment;
            snapshot.sizing = sizing_snapshot();
            const auto accepted = submit_or_replace(
                std::move(request), std::move(snapshot), false,
                "__intraday_loss_close__");
            if (accepted) {
                (void)require_host().execute_current(
                    {*accepted, NativeCurrentPriceRule::NearestTick});
            }
        }
    }
    update_risk_state(event.resolved_price);
    if (risk_.intraday_cancel_pending) {
        risk_.intraday_cancel_pending = false;
        cancel_all();
    }
    if (placement_snapshot && placement_snapshot->opening
        && std::abs(event.opened_units) > 0.0 && policy_script_bar_valid_) {
        // Legacy processes the opening-affordability checkpoint at the
        // matched entry price before the remainder of that bar's path.  The
        // native callback is at exactly that current execution point, so the
        // adapter can issue the generic reduction synchronously without a
        // second matching loop.
        const bool commissioned_short_opening =
            require_host().physical_position().signed_units < 0.0
            && config_.margin_short == 100.0
            && config_.commission_type == static_cast<int>(CommissionType::PERCENT)
            && config_.commission_value > 0.0
            && finite_positive(placement_snapshot->requested_qty);
        if (!commissioned_short_opening) {
            (void)submit_margin_call_slice(event.resolved_price, context, true);
            schedule_margin_call_path(policy_script_bar_, context);
        }
        schedule_intraday_loss_path(policy_script_bar_, context);
    }
    apply_fx_opening_margin_slice(event, context);
    refresh_pending_view();
}

int PineExecutionAdapter::short_seed_collision_role_v1(native_order::RequestHandle handle) const noexcept {
    if (!short_seed_.active) return 0;
    if (handle == short_seed_.long_entry) return 1;
    if (handle == short_seed_.materialize_long) return 2;
    if (handle == short_seed_.final_short) return 3;
    return 0;
}

bool PineExecutionAdapter::take_intraday_loss_relabel(std::uint64_t ordinal) noexcept {
    return intraday_loss_relabel_ordinals_.erase(ordinal) != 0;
}

void PineExecutionAdapter::set_risk_direction(int direction) noexcept { risk_.direction = direction; }
void PineExecutionAdapter::set_risk_max_cons_loss_days(int value) noexcept { risk_.max_cons_loss_days = value; }
void PineExecutionAdapter::set_risk_max_drawdown(double value, bool percent) noexcept {
    risk_.max_drawdown = value;
    if (percent) risk_.max_drawdown_percent = true;
}
void PineExecutionAdapter::set_risk_max_intraday_loss(double value, bool percent) noexcept {
    risk_.max_intraday_loss = value;
    if (percent) risk_.max_intraday_loss_percent = true;
}
void PineExecutionAdapter::set_risk_max_position_size(double value) noexcept { risk_.max_position_size = value; }
void PineExecutionAdapter::set_margin_call_enabled(bool enabled) noexcept {
    source_margin_call_enabled_ = enabled;
}
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

int PendingIntentView::copy_v1(int index, pf_pending_order_v1_t* out) const noexcept {
    if (!owner_ || !out || index < 0
        || index >= static_cast<int>(owner_->pending_view_handles_.size())) {
        return -1;
    }
    const auto handle = owner_->pending_view_handles_[static_cast<std::size_t>(index)];
    const auto it = owner_->placement_.find(handle.incarnation);
    if (it == owner_->placement_.end()) return -1;
    const PlacementSnapshot& snapshot = it->second;

    std::memset(out, 0, sizeof(*out));
    out->struct_version = 1;
    out->size = static_cast<std::uint32_t>(sizeof(*out));
    copy_pending_string(snapshot.source_id, out->id, &out->id_truncated, &out->id_hash64);
    copy_pending_string(snapshot.from_entry, out->from_entry, &out->from_entry_truncated,
                        &out->from_entry_hash64);
    copy_pending_string(snapshot.oca_name, out->oca_name, &out->oca_name_truncated,
                        &out->oca_name_hash64);
    copy_pending_string(snapshot.comment, out->comment, &out->comment_truncated,
                        &out->comment_hash64);
    out->type = mirror_order_type(snapshot.family);
    out->is_long = snapshot.is_long ? 1U : 0U;
    out->limit_price = snapshot.exit_levels.limit;
    out->stop_price = snapshot.exit_levels.stop;
    out->trail_points = snapshot.exit_levels.trail_points;
    out->trail_price = snapshot.exit_levels.trail_price;
    out->trail_offset = snapshot.exit_levels.trail_offset;
    out->profit_ticks = snapshot.exit_levels.profit_ticks;
    out->loss_ticks = snapshot.exit_levels.loss_ticks;
    out->qty = snapshot.requested_qty;
    out->qty_type = snapshot.qty_type;
    out->qty_percent = snapshot.qty_percent;
    out->oca_type = snapshot.oca_type;
    out->created_bar = snapshot.projection_created_bar;
    out->created_seq = static_cast<std::int64_t>(snapshot.source_sequence);
    out->incarnation = handle.incarnation;
    out->created_by_same_id_replacement = snapshot.projection_predecessor != 0
        && snapshot.family != PineOrderFamily::Order ? 1U : 0U;
    out->replaced_default_market_incarnation = snapshot.projection_predecessor_market
        ? snapshot.projection_predecessor : 0;
    // A live row has no terminal cancellation receipt.  Its false/zero receipt
    // projections are therefore an absence fact, not a compatibility default.
    out->declined_by_replaced_short_market = 0U;
    out->replaced_exit_order_incarnation = snapshot.projection_predecessor_exit
        ? snapshot.projection_predecessor : 0;
    out->recreated_after_named_cancelled_entry_incarnation = 0;
    out->named_cancel_surviving_exit_incarnation = 0;
    out->stop_limit_activated = 0U;
    out->coof_suppress_stop_on_entry_bar = 0U;
    out->coof_suppress_limit_on_entry_bar = 0U;
    out->created_during_coof_recalc = snapshot.projection_created_during_coof ? 1U : 0U;
    out->coof_born_at_close_recalc = snapshot.projection_coof_at_terminal ? 1U : 0U;
    out->coof_born_mid_bar = snapshot.projection_coof_mid_bar ? 1U : 0U;
    out->coof_cascade_seg_i = -1;
    out->coof_cascade_inflight_fires = 0U;
    out->created_position_side = snapshot.projection_position_side;
    out->created_position_cycle_seq = snapshot.placement_cycle;
    out->created_after_position_close_in_bar = snapshot.projection_after_close ? 1U : 0U;
    out->over_pyramiding_cap_at_placement = snapshot.projection_over_pyramiding ? 1U : 0U;
    out->same_id_stop_deferred_close_all_bar = -1;
    out->same_id_stop_deferred_close_all_incarnation = 0;
    out->reverses_same_bar_market_from_flat = 0U;
    out->paired_flat_market_candidate = 0U;
    out->paired_flat_market_own_qty = kNaN;
    out->paired_flat_market_signal_close = kNaN;
    out->paired_flat_market_signal_equity = kNaN;
    out->paired_flat_market_signal_margin_pct = kNaN;
    out->paired_flat_market_signal_pointvalue = kNaN;
    out->paired_flat_market_signal_fx = kNaN;
    out->paired_flat_market_peer_seq = 0;
    out->paired_flat_market_transaction_qty = kNaN;
    out->default_flat_market_gross_candidate = 0U;
    out->tv_carry_qty = snapshot.projection_tv_carry_qty;
    out->frozen_default_qty = snapshot.sizing.frozen_units;
    out->default_stop_placement_qty = snapshot.sizing.frozen_units;
    out->default_stop_placement_equity = snapshot.projection_default_stop_equity;
    out->default_stop_placement_signal_close = snapshot.projection_default_stop_signal_close;
    out->default_stop_sizing_price = snapshot.sizing.price;
    out->sizing_equity = snapshot.sizing.equity;
    out->sizing_price = snapshot.sizing.price;
    out->sizing_fx = snapshot.sizing.fx;
    out->sizing_mark = snapshot.sizing.mark;
    out->opening_affordability_exemption_candidate = 0U;
    out->explicit_flat_admission_candidate = 0U;
    out->explicit_placement_equity = snapshot.projection_explicit_equity;
    out->explicit_slipped_signal_close = snapshot.projection_explicit_signal_close;
    out->affordability_placement_equity = snapshot.projection_affordability_equity;
    out->affordability_signal_price = snapshot.projection_affordability_signal_price;
    out->affordability_held_qty = snapshot.projection_affordability_held_qty;
    out->affordability_close_only = snapshot.frozen_market_targeted_close ? 1U : 0U;
    out->rounded_signal_cost_close_only = 0U;
    out->signal_close_mc_bar = -1;
    out->signal_close_mc_entry_incarnation = 0;
    out->signal_close_mc_fill_seq = 0;
    out->signal_close_mc_remaining_qty = kNaN;
    out->requested_partial = (!snapshot.opening && std::isfinite(snapshot.qty_percent)
        && snapshot.qty_percent < 100.0) || (!snapshot.opening
        && std::isfinite(snapshot.requested_qty)) ? 1U : 0U;
    out->full_percent_exit_request = !snapshot.opening && !std::isfinite(snapshot.requested_qty)
        && (!std::isfinite(snapshot.qty_percent) || snapshot.qty_percent == 100.0) ? 1U : 0U;
    out->pooc_global_full_exit_dynamic_qty = 0U;
    out->pooc_global_full_exit_tracks_bound_adds = 0U;
    out->pooc_global_full_exit_bound_add = 0U;
    out->created_while_in_position = !snapshot.opening
        && snapshot.projection_position_side != static_cast<std::int32_t>(PositionSide::FLAT)
        ? 1U : 0U;
    out->sbmt_member = snapshot.frozen_market_instruction ? 1U : 0U;
    out->sbmt_own_qty = snapshot.frozen_market_instruction
        ? snapshot.frozen_market_own_units : kNaN;
    out->sbmt_tx_qty = snapshot.frozen_market_instruction
        ? snapshot.frozen_market_transaction_units : kNaN;
    out->sbmt_kept_over_cap = snapshot.frozen_market_instruction
        && snapshot.projection_over_pyramiding ? 1U : 0U;
    out->sbmt_close_qty = snapshot.frozen_market_targeted_close
        ? snapshot.requested_qty : kNaN;
    out->sbmt_close_buy = snapshot.frozen_market_targeted_close
        && snapshot.projection_position_side == static_cast<std::int32_t>(PositionSide::SHORT)
        ? 1U : 0U;
    out->suppress_as_declined_reversal_close = 0U;
    out->dormant_bracket = 0U;
    out->dormant_reissue_pending = 0U;
    out->dormant_original_stop_price = kNaN;
    out->dormant_hold_bar = -1;
    out->dormant_reversal_kill_bar = -1;
    out->dormant_trail_best = kNaN;
    out->dormant_trail_best_start = kNaN;
    out->dormant_trail_leg_dead = 0U;
    out->suppressed_close_consumed_ledger_qty = kNaN;
    out->suppressed_close_retired_ledger_qty = kNaN;
    out->short_seed_collision_role = short_seed_collision_role(index);
    out->replaced_order_incarnation = snapshot.projection_predecessor;
    out->birth_timestamp = snapshot.placement_sub_open_ms;
    out->birth_bar = snapshot.projection_created_bar;
    out->pine_frozen_market_instruction_kind = snapshot.frozen_market_instruction ? 1U : 0U;
    out->pine_frozen_market_instruction_own_units = snapshot.frozen_market_own_units;
    out->pine_frozen_market_instruction_transaction_units =
        snapshot.frozen_market_transaction_units;
    copy_pending_string(snapshot.frozen_market_targeted_close ? snapshot.source_id : std::string{},
                        out->pine_frozen_market_instruction_target_id,
                        &out->pine_frozen_market_instruction_target_id_truncated,
                        &out->pine_frozen_market_instruction_target_id_hash64);
    return 0;
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
