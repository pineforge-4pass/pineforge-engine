#include "native_execution_consumer.hpp"
#include "engine_internal.hpp"
#include "native_matching.hpp"

#include <pineforge/execution_close_scope.hpp>
#include <pineforge/market_driver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace pineforge {
inline namespace engine_script_run_v17 {
namespace {

template<class T>
void reserve_next(std::vector<T>& values) {
    if (values.size() == values.max_size())
        throw std::length_error("native observation capacity exhausted");
    if (values.size() < values.capacity()) return;
    const auto grown = values.capacity() > values.max_size() / 2
        ? values.max_size() : std::max<std::size_t>(1, values.capacity() * 2);
    values.reserve(grown);
}

struct Fnv {
    uint64_t h = 1469598103934665603ULL;
    // Native run generations (`RunIdentity::run_number`, the consumed
    // high-water) are anti-stale counters that advance on every begin of a
    // reused host.  The continuation digest folds them relative to the run
    // it describes so a reused host's Nth run and a fresh host's first run
    // share one continuation identity while a leaked prior-generation handle
    // or event still folds a non-zero distance (A41(1)).
    uint64_t run_base = 0;
    void bytes(const void* p, size_t n) noexcept {
        const auto* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }
    void u(uint64_t v) noexcept { bytes(&v, sizeof v); }
    void i(int64_t v) noexcept { bytes(&v, sizeof v); }
    // Exact attempted IEEE-754 bits. Does not canonicalize NaN payloads or
    // signed zero; rejected request quantities keep their original encoding.
    void d(double v) noexcept { bytes(&v, sizeof v); }
    void b(bool v) noexcept { unsigned char c = v ? 1 : 0; bytes(&c, 1); }
    void s(const std::string& v) noexcept { u(v.size()); bytes(v.data(), v.size()); }
};

void hash_coordinate(Fnv& f, const NativeCoordinate& c) noexcept;
void hash_birth(Fnv& f, const native_order::Birth& birth) noexcept;
void hash_optional_handle(Fnv& f, const std::optional<native_order::RequestHandle>& handle) noexcept;

// L8 price grid. Generic hosts opt in through the run spec; the kernel
// otherwise keeps price_tick a pure slippage multiplier, so a spec that
// leaves price_grid at None books exactly the prices it books today.
bool price_grid_on(const NativeRunSpec& spec) noexcept {
    return spec.price_grid != NativePriceGrid::None;
}

// The pre-slippage fill basis. Directional rounds toward the region a resting
// order needs: a limit toward its own favorable side, a stop or a market fill
// toward the adverse one.
double grid_fill_basis(const NativeRunSpec& spec, double price, bool buy,
                       bool limit_governed) noexcept {
    if (!price_grid_on(spec)) return price;
    if (spec.grid_rounding == NativeGridRounding::Directional) {
        return native_matching::grid_round_directional(
            price, spec.price_tick, limit_governed ? !buy : buy);
    }
    return native_matching::grid_round_half_up(price, spec.price_tick);
}

// Limit-or-better survives the grid: the protection cap moves to the tick on
// the order's own side, never past its level.
double grid_limit_cap(const NativeRunSpec& spec, double level, bool buy) noexcept {
    if (!price_grid_on(spec)) return level;
    return native_matching::grid_round_directional(level, spec.price_tick, !buy);
}

// Only the second mode tests triggers against the quantized path; an
// inactive threshold is the matcher's existing raw-level arithmetic.
native_matching::GridThreshold grid_threshold(const NativeRunSpec& spec) noexcept {
    native_matching::GridThreshold grid;
    if (spec.price_grid != NativePriceGrid::QuantizeFillsAndTriggers) return grid;
    grid.tick = spec.price_tick;
    grid.half_up = spec.grid_rounding == NativeGridRounding::HalfUp;
    return grid;
}

void hash_spec(Fnv& f, const NativeRunSpec& spec) noexcept {
    f.s(spec.identity.session_key); f.u(spec.identity.run_number - f.run_base);
    f.s(spec.input_tf); f.s(spec.script_tf);
    f.b(spec.timeframe_undetected);
    f.u(static_cast<uint64_t>(spec.slot_label_policy));
    f.u(static_cast<uint64_t>(spec.legacy_tolerance));
    f.u(static_cast<uint64_t>(spec.path_order));
    f.s(spec.ticker); f.s(spec.tickerid); f.s(spec.type);
    f.s(spec.currency); f.s(spec.basecurrency); f.s(spec.description); f.s(spec.volumetype);
    f.s(spec.timezone); f.s(spec.session); f.s(spec.chart_timezone);
    f.d(spec.initial_capital); f.d(spec.point_value); f.d(spec.account_fx); f.d(spec.price_tick);
    f.u(spec.slippage_ticks); f.u(static_cast<uint64_t>(spec.fee_kind)); f.d(spec.fee_value);
    f.b(spec.quantity_grid.has_value()); if (spec.quantity_grid) f.d(*spec.quantity_grid);
    f.u(static_cast<uint64_t>(spec.close_execution));
    f.u(static_cast<uint64_t>(spec.abort_reporting));
    f.b(spec.max_abs_units.has_value()); if (spec.max_abs_units) f.d(*spec.max_abs_units);
    f.b(spec.max_open_lots.has_value()); if (spec.max_open_lots) f.u(*spec.max_open_lots);
    f.u(static_cast<uint64_t>(spec.allowed_open_directions));
    f.b(spec.initial_margin_fraction.has_value());
    if (spec.initial_margin_fraction) f.d(*spec.initial_margin_fraction);
    f.u(native_intrabar_path_digest(spec.intrabar));
    // Report recording is opt-in, so it folds only when it is on: a spec that
    // leaves the kernel out of its report keeps the continuation identity it
    // had before the policy existed (same conditional shape as the precommit
    // digest below).
    if (spec.report_policy != NativeReportPolicy::HostRecorded) {
        f.u(static_cast<uint64_t>(spec.report_policy));
        f.b(spec.report_open_position_at_end);
    }
    // A38/L8: the price grid is folded only where a host actually opted in.
    // A defaulted grid folds nothing, so every established continuation hash
    // survives this spec extension unchanged.
    if (price_grid_on(spec)) {
        f.u(static_cast<uint64_t>(spec.price_grid));
        f.u(static_cast<uint64_t>(spec.grid_rounding));
    }
}

void hash_handle(Fnv& f, const native_order::RequestHandle& handle) noexcept {
    f.s(handle.run.session_key);
    f.u(handle.run.run_number - f.run_base);
    f.u(handle.incarnation);
}

void hash_cohort_handle(Fnv& f, native_order::CohortHandle handle) noexcept {
    f.u(handle.value);
}

void hash_event_id(Fnv& f, const native_order::EventId& id) noexcept {
    f.s(id.run.session_key);
    f.u(id.run.run_number - f.run_base);
    f.u(id.ordinal);
}

void hash_cursor(Fnv& f, const native_order::MatchCursor& cursor) noexcept {
    hash_coordinate(f, cursor.point);
    f.d(cursor.t);
}

void hash_surface(Fnv& f, native_order::CommandSurface surface) noexcept {
    f.u(static_cast<uint64_t>(surface));
}

void hash_intent(Fnv& f, const native_order::OrderIntent& intent) noexcept {
    f.u(intent.index());
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, native_order::Flatten>) {
        } else if constexpr (std::is_same_v<T, native_order::Reduce>) {
            f.u(payload.size.index());
            if (const auto* units = std::get_if<native_order::ExplicitUnits>(&payload.size)) {
                f.d(units->units);
            }
        } else if constexpr (std::is_same_v<T, native_order::Transact>) {
            f.d(payload.signed_units);
        } else if constexpr (std::is_same_v<T, native_order::ReverseTo>) {
            f.d(payload.signed_units);
        } else if constexpr (std::is_same_v<T, native_order::HostSized>) {
            f.u(static_cast<uint64_t>(payload.kind));
            f.b(payload.side.has_value());
            if (payload.side) f.u(static_cast<uint64_t>(*payload.side));
        } else {
            static_assert(!sizeof(T), "unhashed native order intent");
        }
    }, intent);
}

void hash_execution_terms(Fnv& f, const native_order::ExecutionTerms& terms) noexcept {
    f.d(terms.resolved_price);
    f.b(terms.units.has_value());
    if (terms.units) f.d(*terms.units);
    f.u(static_cast<uint64_t>(terms.shape));
    f.u(static_cast<uint64_t>(terms.grid_policy));
}

void hash_optional_execution_terms(Fnv& f,
                                   const std::optional<native_order::ExecutionTerms>& terms) noexcept {
    f.b(terms.has_value());
    if (terms) hash_execution_terms(f, *terms);
}

bool same_double_bits(double left, double right) noexcept {
    return native_matching::double_bits(left) == native_matching::double_bits(right);
}

bool identity_terms(const native_order::ExecutionTerms& terms, double default_price) noexcept {
    return same_double_bits(terms.resolved_price, default_price)
        && !terms.units.has_value()
        && terms.shape == native_order::OpeningShape::Transact
        && terms.grid_policy == native_order::ExecutionGridPolicy::SnapToGrid;
}

bool valid_execution_grid_policy(native_order::ExecutionGridPolicy policy) noexcept {
    switch (policy) {
    case native_order::ExecutionGridPolicy::SnapToGrid:
    case native_order::ExecutionGridPolicy::ExplicitUnits:
        return true;
    }
    return false;
}

bool explicit_reduction_units_representable(double units, double exposure) noexcept {
    if (!std::isfinite(units) || !std::isfinite(exposure)
        || !(units > 0.0) || !(exposure > 0.0) || units > exposure) {
        return false;
    }
    return order_action::plan(exposure, order_action::Reduce{units}).has_value();
}

bool execution_terms_grid_representable(
        const native_order::ExecutionTerms& terms,
        const native_order::HostSized* host_sized, bool unresolved,
        double scope_exposure_units, const NativeRunSpec* spec) noexcept {
    if (!valid_execution_grid_policy(terms.grid_policy)) return false;
    if (terms.grid_policy == native_order::ExecutionGridPolicy::ExplicitUnits) {
        return unresolved && host_sized
            && host_sized->kind == native_order::HostSizedKind::Close
            && terms.shape == native_order::OpeningShape::Transact
            && terms.units
            && explicit_reduction_units_representable(
                *terms.units, scope_exposure_units);
    }
    if (!terms.units || !(*terms.units > 0.0)) return true;
    return spec && (!spec->quantity_grid
        || native_order::quantity_on_grid(*terms.units, *spec->quantity_grid));
}

bool path_uses_high_first(const Bar& bar, NativePathOrder order) noexcept {
    switch (order) {
    case NativePathOrder::HighFirst:
        return true;
    case NativePathOrder::LowFirst:
        return false;
    case NativePathOrder::Auto:
        return std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
    }
    return false;
}

class NativePathOrderScope {
public:
    explicit NativePathOrderScope(NativePathOrder order)
        : prior_(internal::path_order_override()) {
        const int mode = order == NativePathOrder::HighFirst ? 1
            : (order == NativePathOrder::LowFirst ? 2 : 0);
        internal::set_path_order_override(mode);
    }
    ~NativePathOrderScope() { internal::set_path_order_override(prior_); }

    NativePathOrderScope(const NativePathOrderScope&) = delete;
    NativePathOrderScope& operator=(const NativePathOrderScope&) = delete;

private:
    int prior_ = 0;
};

bool remaining_path_coordinate(const NativeCoordinate& coordinate) noexcept {
    const bool continuous_provenance = coordinate.provenance == NativePriceProvenance::Confirmed
        || coordinate.provenance == NativePriceProvenance::ModeledOHLCClose;
    return continuous_provenance
        && coordinate.path_phase != NativePathPhase::None
        && coordinate.path_phase != NativePathPhase::Open;
}

double allowance_left_at(const native_order::Allowance& allowance, uint64_t point) noexcept {
    if (const auto* units = std::get_if<native_order::AllowanceUnits>(&allowance)) {
        return units->point_ordinal == point ? units->left : 0.0;
    }
    if (const auto* all = std::get_if<native_order::AllowanceAllScope>(&allowance)) {
        return all->point_ordinal == point ? std::numeric_limits<double>::infinity() : 0.0;
    }
    return 0.0;
}

bool same_allowance_bits(const native_order::Allowance& left,
                         const native_order::Allowance& right) noexcept {
    if (left.index() != right.index()) return false;
    if (const auto* a = std::get_if<native_order::AllowanceUnits>(&left)) {
        const auto& b = std::get<native_order::AllowanceUnits>(right);
        return a->point_ordinal == b.point_ordinal
            && same_double_bits(a->initial, b.initial)
            && same_double_bits(a->left, b.left);
    }
    if (const auto* a = std::get_if<native_order::AllowanceAllScope>(&left)) {
        return a->point_ordinal
            == std::get<native_order::AllowanceAllScope>(right).point_ordinal;
    }
    if (const auto* a = std::get_if<native_order::AllowanceDeferred>(&left)) {
        return a->point_ordinal
            == std::get<native_order::AllowanceDeferred>(right).point_ordinal;
    }
    return true;
}

const native_order::HostSized* host_sized_intent(
        const native_order::LiveRequest& live) noexcept {
    return std::get_if<native_order::HostSized>(&live.request().intent);
}

void hash_terms_input(Fnv& f, const native_order::TermsResolvedInput& input) noexcept {
    f.u(static_cast<uint64_t>(input.price_kind));
    f.b(input.shared_cursor_collision);
    f.d(input.raw_price);
    f.d(input.default_resolved_price);
    hash_execution_terms(f, input.terms);
}

void hash_trigger(Fnv& f, const native_order::Trigger& trigger) noexcept {
    f.u(trigger.index());
    if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
        f.d(limit->price);
        // Folded only when set, so every bounded limit keeps its prior digest.
        if (limit->fill_through) f.b(true);
    }
    if (const auto* stop = std::get_if<native_order::Stop>(&trigger)) f.d(stop->price);
    if (const auto* sl = std::get_if<native_order::StopLimit>(&trigger)) {
        f.d(sl->stop);
        f.d(sl->limit);
    }
    if (const auto* trail = std::get_if<native_order::Trail>(&trigger)) {
        f.d(trail->offset);
        f.b(trail->arm_price.has_value());
        if (trail->arm_price) f.d(*trail->arm_price);
    }
}

void hash_capacity(Fnv& f, const native_order::Capacity& capacity) noexcept {
    f.u(capacity.index());
    if (const auto* budget = std::get_if<native_order::PointBudget>(&capacity)) f.d(budget->units);
}

void hash_owner(Fnv& f, const native_order::Owner& owner) noexcept {
    f.u(owner.index());
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, native_order::Independent>) {
        } else if constexpr (std::is_same_v<T, native_order::WaitForApplied>) {
            hash_handle(f, value.parent);
        } else if constexpr (std::is_same_v<T, native_order::BindOpening>) {
            hash_handle(f, value.opening);
            f.i(value.cycle);
        } else if constexpr (std::is_same_v<T, native_order::BindOpenings>) {
            f.i(value.cycle); f.u(value.openings.size());
            for (const auto& handle : value.openings) hash_handle(f, handle);
        } else if constexpr (std::is_same_v<T, native_order::BindCohort>) {
            hash_cohort_handle(f, value.cohort);
        } else {
            static_assert(!sizeof(T), "unhashed native owner");
        }
    }, owner);
}

void hash_group(Fnv& f, const native_order::Group& group) noexcept {
    f.u(group.index());
    if (const auto* member = std::get_if<native_order::Member>(&group)) {
        f.u(member->group);
        f.i(member->cohort);
        f.u(static_cast<uint64_t>(member->effect));
    }
}

void hash_request(Fnv& f, const native_order::Request& request) noexcept {
    hash_intent(f, request.intent);
    f.s(request.label);
    f.s(request.comment);
    hash_trigger(f, request.trigger);
    hash_capacity(f, request.capacity);
    hash_owner(f, request.owner);
    hash_group(f, request.group);
}

void hash_remaining(Fnv& f, const native_order::Remaining& remaining) noexcept {
    f.u(remaining.index());
    if (const auto* units = std::get_if<native_order::RemainingUnits>(&remaining)) f.d(units->q);
}

void hash_remaining_projection(Fnv& f, const native_order::RemainingProjection& remaining) noexcept {
    f.u(remaining.index());
    if (const auto* units = std::get_if<native_order::RemainingProjectionUnits>(&remaining)) {
        f.d(units->q);
    }
}

void hash_authority(Fnv& f, const native_order::Authority& authority) noexcept {
    f.u(authority.index());
    const auto hash_enrollment = [&](const native_order::Enrollment& enrollment) {
        f.u(enrollment.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, native_order::EnrollmentFromCommand>) {
                hash_event_id(f, value.accepted);
            } else if constexpr (std::is_same_v<T, native_order::EnrollmentFromApplied>) {
                hash_event_id(f, value.cause);
                hash_cursor(f, value.cursor);
            } else {
                static_assert(!sizeof(T), "unhashed native enrollment");
            }
        }, enrollment);
    };
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, native_order::BookTransaction>) {
        } else if constexpr (std::is_same_v<T, native_order::Wait>) {
            hash_handle(f, value.parent);
        } else if constexpr (std::is_same_v<T, native_order::ArmedTransaction>) {
            hash_handle(f, value.parent);
            hash_event_id(f, value.cause);
            hash_cursor(f, value.cause_cursor);
        } else if constexpr (std::is_same_v<T, native_order::UnboundBookClose>) {
        } else if constexpr (std::is_same_v<T, native_order::BookClose>) {
            f.i(value.cycle);
            f.u(static_cast<uint64_t>(value.side));
            hash_event_id(f, value.binding_event);
            hash_cursor(f, value.binding_cursor);
        } else if constexpr (std::is_same_v<T, native_order::OpeningClose>) {
            hash_handle(f, value.opening);
            f.i(value.cycle);
            f.u(static_cast<uint64_t>(value.side));
            hash_enrollment(value.enrollment);
        } else if constexpr (std::is_same_v<T, native_order::OpeningsClose>) {
            f.i(value.cycle); f.u(static_cast<uint64_t>(value.side));
            f.u(value.openings.size());
            for (const auto& handle : value.openings) hash_handle(f, handle);
            hash_enrollment(value.enrollment);
        } else if constexpr (std::is_same_v<T, native_order::CohortClose>) {
            hash_cohort_handle(f, value.cohort);
        } else {
            static_assert(!sizeof(T), "unhashed native authority");
        }
    }, authority);
}

void hash_cohorts(Fnv& f, const native_order::WorkingRequestCore& requests) noexcept {
    const auto& cohorts = requests.cohorts();
    f.u(cohorts.size());
    for (const auto& roster : cohorts) {
        hash_cohort_handle(f, roster.handle);
        f.u(roster.origins.size());
        for (const auto& origin : roster.origins) hash_handle(f, origin);
    }
    const auto& receipts = requests.cohort_receipts();
    f.u(receipts.size());
    for (const auto& receipt : receipts) {
        f.u(static_cast<std::uint64_t>(receipt.operation));
        f.u(static_cast<std::uint64_t>(receipt.status));
        hash_cohort_handle(f, receipt.cohort);
        hash_handle(f, receipt.origin);
    }
}

void hash_trigger_state(Fnv& f, const native_order::TriggerState& state) noexcept {
    f.u(state.index());
    if (const auto* track = std::get_if<native_order::TrailTrack>(&state)) f.d(track->best);
    if (const auto* active = std::get_if<native_order::TrailActive>(&state)) {
        f.d(active->best_at_trigger);
    }
}

void hash_allowance(Fnv& f, const native_order::Allowance& allowance) noexcept {
    f.u(allowance.index());
    if (const auto* units = std::get_if<native_order::AllowanceUnits>(&allowance)) {
        f.u(units->point_ordinal);
        f.d(units->initial);
        f.d(units->left);
    }
    if (const auto* all = std::get_if<native_order::AllowanceAllScope>(&allowance)) {
        f.u(all->point_ordinal);
    }
    if (const auto* deferred = std::get_if<native_order::AllowanceDeferred>(&allowance)) {
        f.u(deferred->point_ordinal);
    }
}

void hash_pending(Fnv& f, const native_order::PendingAdjustments& pending) noexcept {
    f.u(pending.index());
    if (const auto* deferred = std::get_if<native_order::PendingDeferred>(&pending)) {
        f.d(deferred->total);
        f.u(deferred->count);
        hash_event_id(f, deferred->tail_receipt);
    }
}

void hash_definition(Fnv& f, const native_order::DefinitionRef& definition) noexcept {
    f.b(static_cast<bool>(definition));
    if (!definition) return;
    hash_handle(f, definition->handle);
    hash_request(f, definition->request);
    hash_birth(f, definition->birth);
    hash_optional_handle(f, definition->predecessor);
}

void hash_scope(Fnv& f, const native_order::ExecutionScope& scope) noexcept {
    f.u(scope.index());
    if (const auto* opening = std::get_if<execution::OpeningExposure>(&scope)) {
        f.u(opening->incarnation);
        f.i(opening->cycle);
    }
    if (const auto* selected = std::get_if<native_order::SelectedExposure>(&scope)) {
        f.i(selected->cycle); f.u(selected->incarnations.size());
        for (auto incarnation : selected->incarnations) f.u(incarnation);
    }
}

void hash_birth(Fnv& f, const native_order::Birth& birth) noexcept {
    f.u(birth.acceptance_ordinal);
    f.i(birth.decision_time_lower_bound);
}

void hash_optional_request(Fnv& f, const std::optional<native_order::Request>& request) noexcept {
    f.b(request.has_value());
    if (request) hash_request(f, *request);
}

void hash_optional_handle(Fnv& f, const std::optional<native_order::RequestHandle>& handle) noexcept {
    f.b(handle.has_value());
    if (handle) hash_handle(f, *handle);
}

void hash_failure(Fnv& f, const NativeFailure& failure) noexcept {
    f.u(static_cast<uint64_t>(failure.code));
    f.u(static_cast<uint64_t>(failure.operation));
    f.u(failure.ordinal);
    f.u(failure.discriminator);
    f.u(static_cast<uint64_t>(failure.context.kind));
    f.u(failure.context.cause.ordinal);
    f.u(failure.context.recipient.incarnation);
    hash_coordinate(f, failure.context.cursor.point);
    f.d(failure.context.cursor.t);
}

void hash_coordinate(Fnv& f, const NativeCoordinate& c) noexcept {
    f.u(c.ordinal);
    f.i(c.interval_index);
    f.i(c.open_ms);
    f.i(c.eligible_open_ms);
    f.i(c.last_traded_close_ms);
    f.i(c.next_period_open_ms);
    f.i(c.next_input_open_ms);
    f.i(c.effective_time_ms);
    f.i(c.source_price_time_ms);
    f.u(static_cast<uint64_t>(c.provenance));
    f.u(static_cast<uint64_t>(c.path_phase));
    f.u(static_cast<uint64_t>(c.completion));
}

void hash_interval(Fnv& f, const native_calendar::NativeInterval& interval) noexcept {
    f.i(interval.open_ms);
    f.i(interval.eligible_open_ms);
    f.i(interval.last_traded_close_ms);
    f.i(interval.next_period_open_ms);
    f.i(interval.next_input_open_ms);
}

void hash_driver_statistics(Fnv& f, const NativeDriverStatistics& statistics) noexcept {
    f.b(statistics.intrabar_path_enabled);
    f.i(statistics.sub_bars_per_script_bar);
    f.i(statistics.samples_per_sub_bar);
    f.u(statistics.sub_bars_processed);
    f.u(statistics.sample_ticks_processed);
}

void hash_current_point(Fnv& f, const NativeCurrentPointView& point) noexcept {
    hash_coordinate(f, point.decision.coordinate);
    f.i(point.decision.decision_floor_ms);
    hash_interval(f, point.decision.input_interval);
    hash_interval(f, point.decision.script_interval);
    f.i(point.decision.sub_index);
    f.i(point.decision.sub_count);
    f.b(point.decision.is_terminal_sub_bar);
    f.i(point.decision.sub_bar_open_ms);
    f.i(point.decision.script_bar_open_ms);
    hash_driver_statistics(f, point.decision.driver_statistics);
    f.d(point.price);
    f.u(static_cast<uint64_t>(point.quote_kind));
    f.u(point.quote_origin_ordinal);
}

void hash_input_context(Fnv& f, const NativeInputContext& context) noexcept {
    hash_interval(f, context.input_interval);
    hash_interval(f, context.script_interval);
    f.i(context.input_index);
    f.b(context.completes_script_interval);
}

void hash_tick_context(Fnv& f, const NativeTickContext& context) noexcept {
    hash_coordinate(f, context.decision.coordinate);
    f.i(context.decision.decision_floor_ms);
    hash_interval(f, context.decision.input_interval);
    hash_interval(f, context.decision.script_interval);
    f.i(context.decision.sub_index);
    f.i(context.decision.sub_count);
    f.b(context.decision.is_terminal_sub_bar);
    f.i(context.decision.sub_bar_open_ms);
    f.i(context.decision.script_bar_open_ms);
    hash_driver_statistics(f, context.decision.driver_statistics);
    f.u(context.sequence);
}

void hash_bar(Fnv& f, const Bar& bar) noexcept {
    f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close); f.d(bar.volume);
    f.i(bar.timestamp);
}

void hash_command(Fnv& f, const native_order::CommandEvent& event) noexcept {
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        f.u(payload.ordinal);
        if constexpr (std::is_same_v<T, native_order::AcceptedEvent>) {
            f.u(1);
            hash_definition(f, payload.definition);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::RejectedEvent>) {
            f.u(2);
            hash_request(f, payload.request);
            f.u(static_cast<uint64_t>(payload.reason));
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::ReplacedEvent>) {
            f.u(3);
            hash_definition(f, payload.predecessor_definition);
            hash_definition(f, payload.successor_definition);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::ReplaceRejectedEvent>) {
            f.u(4);
            hash_definition(f, payload.live_definition);
            hash_request(f, payload.attempted);
            f.u(static_cast<uint64_t>(payload.reason));
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::CancelledEvent>) {
            f.u(5);
            hash_definition(f, payload.definition);
            f.u(static_cast<uint64_t>(payload.reason));
            f.b(payload.cause.has_value());
            if (payload.cause) hash_event_id(f, *payload.cause);
            hash_authority(f, payload.prior_authority);
            hash_remaining_projection(f, payload.unexecuted);
            hash_pending(f, payload.pending);
        } else if constexpr (std::is_same_v<T, native_order::NotWorkingEvent>) {
            f.u(6);
            hash_handle(f, payload.target);
            hash_optional_request(f, payload.attempted);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::InvalidHandleEvent>) {
            f.u(7);
            hash_handle(f, payload.target);
            hash_optional_request(f, payload.attempted);
            hash_surface(f, payload.surface);
        } else if constexpr (std::is_same_v<T, native_order::NoEffectEvent>) {
            f.u(8);
            hash_definition(f, payload.definition);
            hash_remaining_projection(f, payload.remaining);
            hash_authority(f, payload.authority);
            hash_cursor(f, payload.cursor);
        } else if constexpr (std::is_same_v<T, native_order::MatchRejectedEvent>) {
            f.u(9);
            f.u(static_cast<uint64_t>(payload.reason));
            hash_definition(f, payload.definition);
            hash_remaining_projection(f, payload.remaining);
            hash_authority(f, payload.authority);
            hash_cursor(f, payload.cursor);
            hash_optional_execution_terms(f, payload.attempted_terms);
        } else if constexpr (std::is_same_v<T, native_order::ExecutionAppliedEvent>) {
            f.u(10);
            hash_definition(f, payload.definition);
            f.d(payload.raw_price);
            f.d(payload.resolved_price);
            f.d(payload.current_ticket);
            f.u(payload.first_trade_index);
            f.u(payload.closed_trade_count);
            f.u(payload.opened_lot_incarnation);
            f.d(payload.closed_units);
            f.d(payload.opened_units);
            f.d(payload.filled_working);
            hash_remaining_projection(f, payload.remaining_before);
            hash_remaining_projection(f, payload.remaining_after);
            hash_allowance(f, payload.allowance_before);
            hash_allowance(f, payload.allowance_after);
            f.b(payload.terminal);
            f.b(payload.terminal_reason.has_value());
            if (payload.terminal_reason) {
                f.u(static_cast<uint64_t>(*payload.terminal_reason));
            }
            f.i(payload.cycle_before);
            f.i(payload.cycle_after);
            hash_scope(f, payload.scope);
            hash_cursor(f, payload.cursor);
        } else if constexpr (std::is_same_v<T, native_order::CloseBoundEvent>) {
            f.u(11);
            hash_definition(f, payload.definition);
            f.i(payload.cycle);
            f.u(static_cast<uint64_t>(payload.side));
            hash_cursor(f, payload.cursor);
            hash_authority(f, payload.before);
            hash_authority(f, payload.after);
        } else if constexpr (std::is_same_v<T, native_order::ActivatedEvent>) {
            f.u(12);
            hash_definition(f, payload.definition);
            f.u(static_cast<uint64_t>(payload.kind));
            hash_trigger_state(f, payload.before);
            hash_trigger_state(f, payload.after);
            f.d(payload.reached_price);
            hash_cursor(f, payload.cursor);
        } else if constexpr (std::is_same_v<T, native_order::ReservationReducedEvent>) {
            f.u(13);
            hash_definition(f, payload.definition);
            hash_event_id(f, payload.cause);
            hash_handle(f, payload.recipient);
            f.u(static_cast<uint64_t>(payload.effect));
            f.d(payload.requested_delta);
            f.d(payload.actual_deduction);
            f.d(payload.before.q);
            hash_remaining_projection(f, payload.after);
        } else if constexpr (std::is_same_v<T, native_order::DeferredGroupAdjustmentEvent>) {
            f.u(14);
            hash_definition(f, payload.definition);
            hash_event_id(f, payload.cause);
            hash_handle(f, payload.recipient);
            f.u(static_cast<uint64_t>(payload.effect));
            f.d(payload.deferred_delta);
            hash_pending(f, payload.pending_before);
            f.d(payload.pending_after.total);
            f.u(payload.pending_after.count);
            hash_event_id(f, payload.pending_after.tail_receipt);
            f.b(payload.previous_pending_receipt.has_value());
            if (payload.previous_pending_receipt) {
                hash_event_id(f, *payload.previous_pending_receipt);
            }
        } else if constexpr (std::is_same_v<T, native_order::QuantityBoundEvent>) {
            f.u(15);
            hash_definition(f, payload.definition);
            hash_event_id(f, payload.source);
            f.d(payload.source_units);
            f.u(payload.prior_adjustment_ids.size());
            for (const auto& id : payload.prior_adjustment_ids) hash_event_id(f, id);
            f.d(payload.pending_total);
            f.d(payload.effective_deduction);
            hash_remaining_projection(f, payload.remaining);
        } else if constexpr (std::is_same_v<T, native_order::ArmedEvent>) {
            f.u(16);
            hash_definition(f, payload.definition);
            hash_authority(f, payload.before);
            hash_authority(f, payload.after);
            f.u(payload.enrollment.index());
            if (const auto* from_cmd = std::get_if<native_order::EnrollmentFromCommand>(&payload.enrollment)) {
                hash_event_id(f, from_cmd->accepted);
            }
            if (const auto* from_app = std::get_if<native_order::EnrollmentFromApplied>(&payload.enrollment)) {
                hash_event_id(f, from_app->cause);
                hash_cursor(f, from_app->cursor);
            }
            f.b(payload.quantity_resolution.has_value());
            if (payload.quantity_resolution) hash_event_id(f, *payload.quantity_resolution);
        } else if constexpr (std::is_same_v<T, native_order::TermsResolvedEvent>) {
            f.u(17);
            hash_definition(f, payload.definition);
            hash_cursor(f, payload.cursor);
            hash_terms_input(f, payload.input);
            f.u(payload.prior_adjustment_ids.size());
            for (const auto& id : payload.prior_adjustment_ids) hash_event_id(f, id);
            f.d(payload.pending_total);
            f.d(payload.effective_deduction);
            hash_remaining_projection(f, payload.remaining_before);
            hash_remaining_projection(f, payload.remaining_after);
            hash_allowance(f, payload.allowance_after);
        } else {
            static_assert(!sizeof(T), "unhashed native command event");
        }
    }, event);
}

void hash_driver_point(Fnv& f, const NativeDriverPoint& point) noexcept {
    hash_coordinate(f, point.coordinate);
    f.d(point.raw_price);
    f.b(point.sequence.has_value());
    if (point.sequence) f.u(*point.sequence);
    f.b(point.matching);
    f.b(point.excursion);
}

void hash_account_row(Fnv& f, const NativeAccountObservation& row) noexcept {
    f.u(row.ordinal);
    f.i(row.effective_time_ms);
    f.d(row.marked_equity);
    f.d(row.realized_balance);
    f.d(row.signed_units);
}

void hash_tz_identity(
        Fnv& f, const std::optional<native_calendar::TimezoneIdentityDescriptor>& id) noexcept {
    f.b(id.has_value());
    if (!id) return;
    f.u(id->semantics_version);
    f.u(static_cast<uint64_t>(id->kind));
    f.s(id->input);
    f.s(id->effective_definition);
    f.s(id->zoneinfo_root);
    f.u(id->resource_paths.size());
    for (const auto& path : id->resource_paths) f.s(path);
}

CommissionType fee_to_commission(NativeFeeKind kind) {
    switch (kind) {
    case NativeFeeKind::Percent: return CommissionType::PERCENT;
    case NativeFeeKind::CashPerUnit: return CommissionType::CASH_PER_CONTRACT;
    case NativeFeeKind::CashPerExecution: return CommissionType::CASH_PER_ORDER;
    }
    return CommissionType::PERCENT;
}

uint64_t command_ordinal(const native_order::CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

}  // namespace

std::unique_ptr<IExecutionConsumer> make_native_execution_consumer() {
    return std::make_unique<NativeExecutionConsumer>();
}

bool NativeExecutionConsumer::failed() const noexcept {
    return std::holds_alternative<NativeFailed>(state_);
}

bool NativeExecutionConsumer::recoverable_abort() const noexcept {
    const auto* failed_state = std::get_if<NativeFailed>(&state_);
    return failed_state != nullptr && failed_state->failure.code == NativeFailureCode::Aborted;
}

void NativeExecutionConsumer::latch_failure(NativeFailure failure) noexcept {
    if (std::holds_alternative<NativeFailed>(state_)) return;
    std::optional<NativeRunSpec> spec;
    if (auto* r = std::get_if<NativeReady>(&state_)) spec = std::move(r->spec);
    else if (auto* n = std::get_if<NativeRunning>(&state_)) spec = std::move(n->spec);
    else if (auto* c = std::get_if<NativeCompleted>(&state_)) spec = std::move(c->spec);
    NativeFailed failed;
    failed.spec = std::move(spec);
    failed.failure = failure;
    state_.emplace<NativeFailed>(std::move(failed));
}

void NativeExecutionConsumer::fail(BacktestEngine& engine, NativeFailure failure) noexcept {
    latch_failure(failure);
    engine.last_run_status_ = 1;
}

void NativeExecutionConsumer::render(BacktestEngine& engine, const char* text) const {
    engine.last_error_ = text ? text : "";
}

void NativeExecutionConsumer::present_refusal(BacktestEngine& engine, const char* text) {
    render(engine, text);
    engine.last_run_status_ = 1;
}

bool NativeExecutionConsumer::refuse_mixed_input_mode(BacktestEngine& engine, InputMode requested) {
    if (input_mode_ != InputMode::Unselected && input_mode_ != requested) {
        present_refusal(engine, "native stream cannot mix confirmed bars and ticks");
        return true;
    }
    return false;
}

void NativeExecutionConsumer::select_input_mode(InputMode requested) {
    if (input_mode_ == InputMode::Unselected) input_mode_ = requested;
}

bool NativeExecutionConsumer::admit_public_begin(BacktestEngine& engine, const char* not_ready_text) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    // v10 §4: begin outside Ready refuses without consuming identity/history.
    // Unconfigured/Completed stay put. A begin while Running is a contract
    // failure, including reentry from on_native_run_begin / on_native_bar.
    if (preparing_begin_ || in_callback_ || processing_input_
        || std::holds_alternative<NativeRunning>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native begin is forbidden while running");
        return false;
    }
    if (!std::holds_alternative<NativeReady>(state_)) {
        present_refusal(engine, not_ready_text);
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::apply_staged_ingress(BacktestEngine& engine) {
    // BacktestEngine owns the copied C/C++ FX arrays while a host is not yet
    // running.  Applying them here makes staging independent of whether the
    // provider configured the host before or during this begin call.
    // A provider may intentionally leave an unconfigured/completed host for
    // admit_public_begin to diagnose.  Do not turn that ordinary lifecycle
    // refusal into a mutation failure merely because old staged bytes exist.
    if (!std::holds_alternative<NativeReady>(state_)
        || (engine.account_currency_fx_timestamps_.empty()
        && engine.account_currency_fx_rates_.empty())) {
        return true;
    }
    NativeFxCurve curve;
    curve.effective_from_ms = engine.account_currency_fx_timestamps_;
    curve.account_per_quote = engine.account_currency_fx_rates_;
    const auto result = configure_fx_curve(curve);
    if (result.status == NativeSetupStatus::Applied) return true;
    fail(engine, NativeFailure{NativeFailureCode::InvalidSpecification,
                               NativeFailureOperation::Configure,
                               0, static_cast<std::uint32_t>(result.validation.error)});
    render(engine, "native staged account-currency FX was rejected");
    return false;
}

bool NativeExecutionConsumer::prepare_public_begin(
        BacktestEngine& engine, const NativeBeginArgs& args) {
    interval_cache_.clear();
    if (preparing_begin_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native prepare_native_begin cannot reenter");
        return false;
    }
    auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
    if (!host) return true;
    preparing_begin_ = true;
    try {
        host->prepare_native_begin(args);
    } catch (const std::exception& e) {
        preparing_begin_ = false;
        // A provider's begin-time validation is a public-entry refusal.  It
        // has not started a native run or consumed an identity, so preserve a
        // reusable Unconfigured/Completed host just as other begin refusals
        // do. Callback exceptions after begin_ready remain terminal.
        render(engine, e.what());
        return false;
    } catch (...) {
        preparing_begin_ = false;
        render(engine, "native pre-begin provider exception");
        return false;
    }
    preparing_begin_ = false;
    if (failed() || !apply_staged_ingress(engine)) return false;
    return validate_undetected_begin(engine, args);
}

bool NativeExecutionConsumer::admit_public_stream_input(BacktestEngine& engine,
                                                        NativeFailureOperation operation) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    // Public stream/run inputs are serialized. Nested calls from a native
    // callback or an in-flight input are a Running contract failure, not a
    // second pump and not a Completed downgrade. Internal pump_batch and
    // deliver_tick are not public entry points.
    if (in_callback_ || processing_input_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, operation});
        render(engine, "native public input cannot reenter an active callback or input");
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::check_abort_or_projection(BacktestEngine& engine,
                                                        NativeFailureOperation operation,
                                                        uint64_t ordinal) {
    if (failed()) return false;
    if (engine.abort_requested_.load(std::memory_order_relaxed)) {
        fail(engine, NativeFailure{NativeFailureCode::Aborted, operation, ordinal});
        const auto* spec = spec_ptr();
        if (!spec || spec->abort_reporting == NativeAbortReporting::Error) {
            render(engine, "native run aborted");
        } else {
            engine.last_error_.clear();
        }
        return false;
    }
    if (!projection_ok(engine)) {
        fail(engine, NativeFailure{NativeFailureCode::ProjectionMismatch, operation, ordinal});
        render(engine, "native projection mismatch");
        return false;
    }
    return true;
}

const NativeRunSpec* NativeExecutionConsumer::spec_ptr() const {
    if (const auto* n = std::get_if<NativeRunning>(&state_)) return &n->spec;
    if (const auto* r = std::get_if<NativeReady>(&state_)) return &r->spec;
    if (const auto* c = std::get_if<NativeCompleted>(&state_)) return &c->spec;
    if (const auto* f = std::get_if<NativeFailed>(&state_)) {
        if (f->spec) return &*f->spec;
    }
    return nullptr;
}

bool NativeExecutionConsumer::commands_allowed() const {
    if (failed() || consuming_request_) return false;
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running) return false;
    if (in_callback_) return true;
    if (processing_input_) return false;
    return running->phase == NativeRunPhase::Realtime;
}

NativeStateView NativeExecutionConsumer::view() const {
    NativeStateView v;
    v.consumed_high_water = consumed_high_water_;
    v.decision_floor_ms = decision_floor();
    v.spec = spec_ptr();
    if (std::holds_alternative<NativeUnconfigured>(state_)) {
        v.kind = NativeLifecycleKind::Unconfigured;
    } else if (auto* r = std::get_if<NativeReady>(&state_)) {
        v.kind = NativeLifecycleKind::Ready;
        v.spec = &r->spec;
    } else if (auto* n = std::get_if<NativeRunning>(&state_)) {
        v.kind = NativeLifecycleKind::Running;
        v.spec = &n->spec;
        v.phase = n->phase;
    } else if (auto* c = std::get_if<NativeCompleted>(&state_)) {
        v.kind = NativeLifecycleKind::Completed;
        v.spec = &c->spec;
        v.completion = c->completion;
    } else if (auto* f = std::get_if<NativeFailed>(&state_)) {
        v.kind = NativeLifecycleKind::Failed;
        v.failure = f->failure;
        if (f->spec) v.spec = &*f->spec;
    }
    return v;
}

void NativeExecutionConsumer::refuse_source_mutation(const char* operation) {
    // Source-free ingress is staging while no run is active: before a first
    // begin, between completed runs, and after a cooperative abort. The
    // existing in-run refusal stays fail-closed.
    if (recoverable_abort()
        || (!failed() && (std::holds_alternative<NativeUnconfigured>(state_)
                          || std::holds_alternative<NativeReady>(state_)
                          || std::holds_alternative<NativeCompleted>(state_)
                          || preparing_begin_))) {
        return;
    }
    NativeFailure failure;
    failure.code = NativeFailureCode::UnsupportedSource;
    failure.operation = NativeFailureOperation::Mutation;
    latch_failure(failure);
    throw std::runtime_error(std::string("native host refuses source mutation: ") +
                             (operation ? operation : ""));
}

bool NativeExecutionConsumer::stage_account_currency_fx_series(
        const std::vector<std::int64_t>& timestamps, const std::vector<double>& rates) {
    if (timestamps.size() != rates.size()) return false;
    NativeFxCurve candidate;
    try {
        candidate.effective_from_ms = timestamps;
        candidate.account_per_quote = rates;
    } catch (...) {
        return false;
    }
    if (validate_native_fx_curve(candidate).error != NativeFxCurveError::None) return false;
    if (candidate.effective_from_ms.empty()) {
        staged_fx_curve_.reset();
        staged_ingress_fx_ = true;
        return true;
    }
    try {
        std::optional<NativeFxCurve> replacement;
        replacement.emplace(std::move(candidate));
        staged_fx_curve_.swap(replacement);
        staged_ingress_fx_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

uint64_t NativeExecutionConsumer::continuation_hash() const noexcept {
    Fnv f;
    f.run_base = requests_.identity().run_number;
    f.s(kNativeConsumerSemanticVersion);
    f.s(kNativeDriverSemanticVersion);
    f.s(kNativeCalendarSemanticVersion);
    f.u(static_cast<uint64_t>(state_.index()));
    if (const auto* running = std::get_if<NativeRunning>(&state_)) {
        f.u(static_cast<uint64_t>(running->phase));
    }
    if (const auto* completed = std::get_if<NativeCompleted>(&state_)) {
        f.u(static_cast<uint64_t>(completed->completion));
    }
    if (const auto* failed_state = std::get_if<NativeFailed>(&state_)) {
        hash_failure(f, failed_state->failure);
    }
    f.s(bound_session_key_);
    f.i(decision_floor_ms_);
    f.b(has_floor_);
    f.u(next_timeline_ordinal_);
    f.b(in_callback_);
    f.u(static_cast<uint64_t>(callback_phase_));
    f.b(preparing_begin_);
    f.b(input_callback_context_.has_value());
    if (input_callback_context_) hash_input_context(f, *input_callback_context_);
    f.b(input_callback_bar_.has_value());
    if (input_callback_bar_) hash_bar(f, *input_callback_bar_);
    f.b(tick_callback_context_.has_value());
    if (tick_callback_context_) hash_tick_context(f, *tick_callback_context_);
    f.b(tick_callback_bar_.has_value());
    if (tick_callback_bar_) hash_bar(f, *tick_callback_bar_);
    hash_coordinate(f, callback_context_.coordinate);
    f.i(callback_context_.decision_floor_ms);
    hash_interval(f, callback_context_.input_interval);
    hash_interval(f, callback_context_.script_interval);
    f.i(callback_context_.sub_index);
    f.i(callback_context_.sub_count);
    f.b(callback_context_.is_terminal_sub_bar);
    f.i(callback_context_.sub_bar_open_ms);
    f.i(callback_context_.script_bar_open_ms);
    hash_driver_statistics(f, callback_context_.driver_statistics);
    f.b(consuming_request_);
    f.b(draining_notifications_);
    f.b(current_frame_.has_value());
    if (current_frame_) {
        hash_current_point(f, current_frame_->point);
        f.u(current_frame_->acceptance_cutoff);
    }
    f.u(pre_open_birth_point_ordinal_);
    f.i(pre_open_birth_time_ms_);
    f.u(pre_open_births_.size());
    for (const auto& handle : pre_open_births_) hash_handle(f, handle);
    f.u(applied_notifications_.size() - notification_head_);
    for (std::size_t i = notification_head_; i < applied_notifications_.size(); ++i) {
        const auto& notification = applied_notifications_[i];
        f.u(notification.history_index);
        f.u(notification.ordinal);
        hash_current_point(f, notification.point);
    }
    f.b(processing_input_);
    f.u(static_cast<uint64_t>(input_mode_));
    f.i(next_interval_index_);
    if (const auto* spec = spec_ptr()) hash_spec(f, *spec);
    f.b(staged_ingress_fx_);
    f.b(staged_fx_curve_.has_value());
    if (staged_fx_curve_) f.u(native_fx_curve_digest(*staged_fx_curve_));
    hash_tz_identity(f, tz_identity_);
    f.s(requests_.identity().session_key);
    f.u(requests_.live().size());
    for (const auto& live : requests_.live()) {
        hash_definition(f, live.definition);
        hash_remaining(f, live.remaining);
        hash_authority(f, live.authority);
        hash_trigger_state(f, live.trigger_state);
        hash_allowance(f, live.allowance);
        hash_pending(f, live.pending);
    }
    // The host-maintained roster is durable matching authority.  Fold it
    // immediately after the request table so a membership-only change cannot
    // share a continuation identity with an otherwise identical run.
    hash_cohorts(f, requests_);
    sync_history_digest();
    if (driver_digest_.count != driver_log_.size()) {
        driver_digest_.reset();
        for (const auto& point : driver_log_) fold_driver_digest(point);
    }
    if (account_digest_.count != account_log_.size()) {
        account_digest_.reset();
        for (const auto& row : account_log_) fold_account_digest(row);
    }
    f.u(history_digest_.count);
    f.u(history_digest_.h);
    f.b(current_input_open_.has_value());
    if (current_input_open_) f.i(*current_input_open_);
    f.b(observed_input_cursor_.has_value());
    if (observed_input_cursor_) f.i(*observed_input_cursor_);
    f.b(next_tradable_synthesis_cursor_.has_value());
    if (next_tradable_synthesis_cursor_) f.i(*next_tradable_synthesis_cursor_);
    f.b(last_accepted_input_.has_value());
    if (last_accepted_input_) hash_interval(f, *last_accepted_input_);
    f.b(last_observed_slot_open_.has_value());
    if (last_observed_slot_open_) f.i(*last_observed_slot_open_);
    f.b(last_finalized_input_.has_value());
    if (last_finalized_input_) hash_interval(f, *last_finalized_input_);
    f.b(has_tick_sequence_);
    f.u(last_tick_sequence_);
    f.i(script_.key);
    f.b(script_.has_data);
    f.b(script_.sealed);
    hash_interval(f, script_.interval);
    hash_bar(f, script_.agg);
    f.i(script_.first_open_ms);
    f.i(script_.first_source_time_ms);
    f.i(script_.latest_close_ms);
    f.i(script_.first_index);
    f.i(script_.last_index);
    f.b(script_.modeled_ohlc);
    f.b(has_forming_);
    if (has_forming_) hash_bar(f, forming_);
    f.b(has_last_price_);
    f.d(last_price_);
    f.i(last_print_time_ms_);
    hash_driver_statistics(f, driver_statistics_);
    f.u(static_cast<uint64_t>(pairing_.pairing));
    f.i(pairing_.group_factor);
    f.u(driver_digest_.count);
    f.u(driver_digest_.h);
    f.u(account_digest_.count);
    f.u(account_digest_.h);
    if (precommit_digest_.count != 0) {
        f.u(precommit_digest_.count);
        f.u(precommit_digest_.h);
    }
    return f.h;
}

bool NativeExecutionConsumer::timeframe_args_ok(const std::string& input_tf,
                                                const std::string& script_tf) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    if (spec->timeframe_undetected) return input_tf.empty() && script_tf.empty();
    if (!input_tf.empty() && input_tf != spec->input_tf) return false;
    if (!script_tf.empty() && script_tf != spec->script_tf) return false;
    return true;
}

bool NativeExecutionConsumer::has_undetected_timeframe() const noexcept {
    const auto* spec = spec_ptr();
    return spec && spec->timeframe_undetected;
}

bool NativeExecutionConsumer::legacy_tolerant_slot_labels() const noexcept {
    const auto* spec = spec_ptr();
    return spec && spec->slot_label_policy == NativeSlotLabelPolicy::LegacyTolerant;
}

bool NativeExecutionConsumer::uses_raw_label_partition() const noexcept {
    return has_undetected_timeframe()
        || (legacy_tolerant_slot_labels()
            && pairing_.pairing == native_calendar::TimeframePairing::Passthrough);
}

native_calendar::NativeInterval NativeExecutionConsumer::timestamp_partition(
        std::int64_t timestamp) noexcept {
    // No duration is available in this state. Each boundary is the current
    // bar timestamp, so no inferred aggregation or clock grid is introduced.
    return {timestamp, timestamp, timestamp, timestamp, timestamp};
}

std::optional<native_calendar::NativeInterval>
NativeExecutionConsumer::input_interval_at(std::int64_t timestamp) const {
    if (interval_cache_.input_ts == timestamp) {
        return interval_cache_.input_interval;
    }
    if (uses_raw_label_partition()) return timestamp_partition(timestamp);
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, timestamp);
    if (!interval && legacy_tolerant_slot_labels()) interval = timestamp_partition(timestamp);
    interval_cache_.input_ts = timestamp;
    interval_cache_.input_interval = interval;
    return interval;
}

std::optional<native_calendar::NativeInterval>
NativeExecutionConsumer::script_interval_at(std::int64_t timestamp) const {
    if (interval_cache_.script_ts == timestamp) {
        return interval_cache_.script_interval;
    }
    if (uses_raw_label_partition()) return timestamp_partition(timestamp);
    auto interval = native_calendar::interval_containing(calendar_, script_tf_, timestamp);
    if (!interval && legacy_tolerant_slot_labels()) interval = timestamp_partition(timestamp);
    interval_cache_.script_ts = timestamp;
    interval_cache_.script_interval = interval;
    return interval;
}

bool NativeExecutionConsumer::validate_undetected_begin(
        BacktestEngine& engine, const NativeBeginArgs& args) {
    if (!has_undetected_timeframe()) return true;
    if (!args.input_tf.empty() || !args.script_tf.empty()) {
        present_refusal(engine, "native undetected timeframe requires empty timeframe arguments");
        return false;
    }
    if (args.n >= 2) {
        present_refusal(engine, "native undetected timeframe requires fewer than two bars");
        return false;
    }
    if (args.is_stream) {
        present_refusal(engine, "native stream requires a detected timeframe");
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::apply_spec(BacktestEngine& engine, const NativeRunSpec& spec) {
    engine.initial_capital_ = spec.initial_capital;
    engine.syminfo_.pointvalue = spec.point_value;
    engine.account_currency_fx_ = spec.account_fx;
    if (staged_fx_curve_) {
        engine.account_currency_fx_timestamps_ = staged_fx_curve_->effective_from_ms;
        engine.account_currency_fx_rates_ = staged_fx_curve_->account_per_quote;
    } else {
        engine.account_currency_fx_timestamps_.clear();
        engine.account_currency_fx_rates_.clear();
    }
    engine.syminfo_.mintick = spec.price_tick;
    engine.syminfo_mintick_ = spec.price_tick;
    engine.commission_type_ = fee_to_commission(spec.fee_kind);
    engine.commission_value_ = spec.fee_value;
    engine.syminfo_.ticker = spec.ticker;
    engine.syminfo_.tickerid = spec.tickerid;
    engine.syminfo_.type = spec.type;
    engine.syminfo_.currency = spec.currency;
    engine.syminfo_.basecurrency = spec.basecurrency;
    engine.syminfo_.description = spec.description;
    engine.syminfo_.volumetype = spec.volumetype;
    engine.syminfo_.timezone = spec.timezone;
    engine.syminfo_.session = spec.session;
    engine.chart_timezone_ = spec.chart_timezone;
    engine.slippage_ = 0;
    applied_ = spec;
    auto parsed_session = native_calendar::parse_session(spec.session, spec.timezone);
    if (!parsed_session) return false;
    intrabar_tf_.reset();
    if (!spec.timeframe_undetected) {
        auto parsed_input = native_calendar::parse_timeframe(spec.input_tf);
        auto parsed_script = native_calendar::parse_timeframe(spec.script_tf);
        if (!parsed_input || !parsed_script) return false;
        input_tf_ = std::move(*parsed_input);
        script_tf_ = std::move(*parsed_script);
    } else {
        input_tf_ = native_calendar::Timeframe{};
        script_tf_ = native_calendar::Timeframe{};
    }
    if (const auto* lower = spec.intrabar.lower()) {
        auto parsed_intrabar = native_calendar::parse_timeframe(lower->tf);
        if (!parsed_intrabar) return false;
        intrabar_tf_ = std::move(*parsed_intrabar);
    }
    calendar_ = std::move(*parsed_session);
    pairing_ = spec.timeframe_undetected ? native_calendar::TimeframeCompatibility{}
                                         : native_calendar::compatibility(input_tf_, script_tf_);
    tz_identity_ = native_calendar::timezone_identity_descriptor(spec.timezone);
    return true;
}

bool NativeExecutionConsumer::projection_ok(const BacktestEngine& engine) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    if (engine.initial_capital_ != spec->initial_capital) return false;
    if (engine.syminfo_.pointvalue != spec->point_value) return false;
    if (engine.account_currency_fx_ != spec->account_fx) return false;
    if (staged_fx_curve_) {
        if (engine.account_currency_fx_timestamps_ != staged_fx_curve_->effective_from_ms
            || engine.account_currency_fx_rates_ != staged_fx_curve_->account_per_quote) {
            return false;
        }
    } else if (!engine.account_currency_fx_timestamps_.empty()
               || !engine.account_currency_fx_rates_.empty()) {
        return false;
    }
    if (engine.syminfo_.mintick != spec->price_tick) return false;
    if (engine.commission_type_ != fee_to_commission(spec->fee_kind)) return false;
    if (engine.commission_value_ != spec->fee_value) return false;
    if (engine.syminfo_.ticker != spec->ticker) return false;
    if (engine.syminfo_.tickerid != spec->tickerid) return false;
    if (engine.syminfo_.type != spec->type) return false;
    if (engine.syminfo_.currency != spec->currency) return false;
    if (engine.syminfo_.basecurrency != spec->basecurrency) return false;
    if (engine.syminfo_.description != spec->description) return false;
    if (engine.syminfo_.volumetype != spec->volumetype) return false;
    if (engine.syminfo_.timezone != spec->timezone) return false;
    if (engine.syminfo_.session != spec->session) return false;
    if (engine.chart_timezone_ != spec->chart_timezone) return false;
    return true;
}

NativeSetupResult NativeExecutionConsumer::configure(BacktestEngine& engine,
                                                     const NativeRunSpec& spec) {
    NativeSetupResult result;
    const NativeRunSpec* prior_spec = nullptr;
    if (failed() && !recoverable_abort()) {
        result.validation.error = NativeRunSpecError::CalendarFailure;
        render(engine, "native host already failed");
        return result;
    }
    if (recoverable_abort()) {
        const auto* aborted = std::get_if<NativeFailed>(&state_);
        if (!aborted || !aborted->spec) {
            result.validation.error = NativeRunSpecError::CalendarFailure;
            render(engine, "native aborted host has no reusable run spec");
            return result;
        }
        prior_spec = &*aborted->spec;
    }
    if (std::holds_alternative<NativeRunning>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
        render(engine, "configure refused while running");
        return result;
    }
    if (std::holds_alternative<NativeReady>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
        render(engine, "configure refused while ready; use a fresh host");
        return result;
    }
    NativeRunSpec candidate = spec;
    const auto validation = normalize_native_run_spec(candidate);
    result.validation = validation;
    if (!validation) {
        fail(engine, NativeFailure{NativeFailureCode::InvalidSpecification,
                                   NativeFailureOperation::Configure});
        render(engine, "native run spec rejected");
        return result;
    }
    if (const auto* completed = std::get_if<NativeCompleted>(&state_)) {
        prior_spec = &completed->spec;
    }
    if (prior_spec) {
        if (candidate.identity.session_key != prior_spec->identity.session_key
            && candidate.identity.session_key != bound_session_key_) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
            render(engine, "native session key cannot change on a reused host");
            return result;
        }
        if (candidate.identity.run_number <= consumed_high_water_) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
            render(engine, "native run number must exceed consumed high-water");
            return result;
        }
    }
    auto parsed_session = native_calendar::parse_session(candidate.session, candidate.timezone);
    if (!parsed_session) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
        render(engine, "native calendar parse failed at configure");
        return result;
    }
    if (!candidate.timeframe_undetected) {
        auto parsed_input = native_calendar::parse_timeframe(candidate.input_tf);
        auto parsed_script = native_calendar::parse_timeframe(candidate.script_tf);
        if (!parsed_input || !parsed_script) {
            fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
            render(engine, "native calendar parse failed at configure");
            return result;
        }
        input_tf_ = std::move(*parsed_input);
        script_tf_ = std::move(*parsed_script);
    } else {
        input_tf_ = native_calendar::Timeframe{};
        script_tf_ = native_calendar::Timeframe{};
    }
    intrabar_tf_.reset();
    if (const auto* lower = candidate.intrabar.lower()) {
        auto parsed_intrabar = native_calendar::parse_timeframe(lower->tf);
        if (!parsed_intrabar) {
            fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
            render(engine, "native intrabar timeframe parse failed at configure");
            return result;
        }
        intrabar_tf_ = std::move(*parsed_intrabar);
    }
    calendar_ = std::move(*parsed_session);
    pairing_ = candidate.timeframe_undetected ? native_calendar::TimeframeCompatibility{}
                                              : native_calendar::compatibility(input_tf_, script_tf_);
    // Direct native FX setup is per-ready-spec as before. C/C++ staged ingress
    // persists across a completed/aborted handle and is reapplied by the next
    // provider begin, so it must survive that provider's configure call.
    if (!staged_ingress_fx_) staged_fx_curve_.reset();
    state_ = NativeReady{std::move(candidate)};
    result.status = NativeSetupStatus::Applied;
    engine.last_error_.clear();
    return result;
}

NativeFxCurveSetupResult NativeExecutionConsumer::configure_fx_curve(
        const NativeFxCurve& curve) {
    NativeFxCurveSetupResult result;
    if (!std::holds_alternative<NativeReady>(state_)) {
        result.validation = {NativeFxCurveError::WrongPhase, 0};
        return result;
    }

    result.validation = validate_native_fx_curve(curve);
    if (result.validation.error != NativeFxCurveError::None) return result;

    if (curve.effective_from_ms.empty()) {
        staged_fx_curve_.reset();
        result.status = NativeSetupStatus::Applied;
        return result;
    }

    try {
        std::optional<NativeFxCurve> replacement;
        replacement.emplace(curve);
        staged_fx_curve_.swap(replacement);
    } catch (...) {
        result.validation = {NativeFxCurveError::AllocationFailure, 0};
        return result;
    }
    result.status = NativeSetupStatus::Applied;
    return result;
}

bool NativeExecutionConsumer::begin_ready(BacktestEngine& engine, NativeRunPhase phase,
                                          int64_t initial_floor_ms) {
    auto* ready = std::get_if<NativeReady>(&state_);
    if (!ready) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native begin requires Ready");
        return false;
    }
    NativeRunSpec spec = ready->spec;
    if (bound_session_key_.empty()) bound_session_key_ = spec.identity.session_key;
    else if (spec.identity.session_key != bound_session_key_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native session key already bound");
        return false;
    }
    if (spec.identity.run_number <= consumed_high_water_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native run number does not exceed high-water");
        return false;
    }
    consumed_high_water_ = spec.identity.run_number;
    if (!apply_spec(engine, spec)) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Begin});
        render(engine, "native calendar apply failed");
        return false;
    }
    engine.reset_run_state();
    // RULING A48: one generic capability, wired once per run. A host that
    // declares ownership supplies the closing-row magnitudes; the kernel then
    // keeps no excursion model of its own for this run.
    if (auto* excursion_owner = dynamic_cast<NativeStrategyHost*>(&engine)) {
        if (excursion_owner->owns_lot_excursions()) {
            engine.lot_excursion_hook_ =
                [excursion_owner](const ClosedLotExcursionFacts& facts) {
                    return excursion_owner->closed_lot_excursion(facts);
                };
        }
    }
    engine.diag_input_bars_processed_ = 0;
    engine.diag_script_bars_processed_ = 0;
    requests_.reset(spec.identity);
    clear_cohort_target_cache();
    terminal_receipt_high_water_ = 0;
    next_timeline_ordinal_ = 1;
    decision_floor_ms_ = initial_floor_ms;
    has_floor_ = true;
    input_mode_ = InputMode::Unselected;
    current_frame_.reset();
    callback_phase_ = CallbackPhase::None;
    pre_open_birth_point_ordinal_ = 0;
    pre_open_birth_time_ms_ = 0;
    pre_open_births_.clear();
    applied_notifications_.clear();
    notification_head_ = 0;
    consuming_request_ = false;
    draining_notifications_ = false;
    next_interval_index_ = 0;
    current_input_open_.reset();
    observed_input_cursor_.reset();
    next_tradable_synthesis_cursor_.reset();
    last_accepted_input_.reset();
    last_observed_slot_open_.reset();
    last_finalized_input_.reset();
    last_tick_sequence_ = 0;
    has_tick_sequence_ = false;
    script_ = ScriptBucket{};
    has_forming_ = false;
    has_last_price_ = false;
    // Stream print state is run-scoped: a reused host must not carry the
    // previous run's last print into the next run's decision coordinates
    // (source_price_time/effective_time fallbacks) or its continuation digest.
    last_price_ = 0.0;
    last_print_time_ms_ = 0;
    driver_log_.clear();
    account_log_.clear();
    history_digest_.reset();
    driver_digest_.reset();
    account_digest_.reset();
    precommit_digest_.reset();
    driver_statistics_ = NativeDriverStatistics{};
    driver_statistics_.intrabar_path_enabled = !spec.intrabar.is_none();
    callback_context_ = NativeDecisionContext{};
    callback_context_.driver_statistics = driver_statistics_;
    input_callback_context_.reset();
    input_callback_bar_.reset();
    tick_callback_context_.reset();
    tick_callback_bar_.reset();
    state_ = NativeRunning{std::move(spec), phase};
    if (!check_abort_or_projection(engine, NativeFailureOperation::Begin)) return false;
    if (auto* host = dynamic_cast<NativeStrategyHost*>(&engine)) {
        in_callback_ = true;
        try {
            host->on_native_run_begin();
        } catch (const std::exception& e) {
            in_callback_ = false;
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback});
            render(engine, e.what());
            return false;
        } catch (...) {
            in_callback_ = false;
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback});
            render(engine, "native callback exception");
            return false;
        }
        in_callback_ = false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Callback)) return false;
    }
    return true;
}

bool NativeExecutionConsumer::preflight_bars(BacktestEngine& engine, const Bar* bars, int n,
                                             bool stream, bool preserve_status) {
    const auto* spec = spec_ptr();
    if (!spec) {
        present_refusal(engine, "native preflight requires a staged spec");
        return false;
    }
    const auto result = preflight_native_inputs(
        *spec, bars, n,
        stream ? NativeInputPolicy::StreamWarmup : NativeInputPolicy::Batch);
    if (result) return true;
    const int status_before = engine.last_run_status_;
    const auto refuse = [&](const char* text) {
        present_refusal(engine, text);
        if (preserve_status) engine.last_run_status_ = status_before;
    };
    const auto indexed = [&](const char* field, const char* detail) {
        std::string message = "bar[" + std::to_string(result.index) + "]." + field;
        if (detail) message += detail;
        refuse(message.c_str());
    };
    const auto timestamped = [&](const char* detail) {
        indexed("timestamp", detail);
    };
    switch (result.error) {
    case NativeInputPreflightError::NullArray:
        // The warmup stage names itself so an invalid warmup request still
        // carries the warmup word through the generic field/index renderer.
        refuse(stream ? "native warmup bars require a non-null array"
                      : "native bars require a non-null array");
        break;
    case NativeInputPreflightError::InvalidCount:
        refuse(stream ? "native warmup bar count is invalid"
                      : "native bar count is invalid");
        break;
    case NativeInputPreflightError::StructuralInvalid:
        if (bars != nullptr && result.index >= 0 && result.index < n) {
            const Bar& bar = bars[result.index];
            if (!std::isfinite(bar.open)) {
                indexed("open", " must be finite");
            } else if (!std::isfinite(bar.high)) {
                indexed("high", " must be finite");
            } else if (!std::isfinite(bar.low)) {
                indexed("low", " must be finite");
            } else if (!std::isfinite(bar.close)) {
                indexed("close", " must be finite");
            } else if (bar.open < 0.0 && stream) {
                indexed("open", " must be non-negative");
            } else if (bar.high < 0.0 && stream) {
                indexed("high", " must be non-negative");
            } else if (bar.low < 0.0 && stream) {
                indexed("low", " must be non-negative");
            } else if (bar.close < 0.0 && stream) {
                indexed("close", " must be non-negative");
            } else if (bar.low > std::min(bar.open, bar.close)) {
                indexed("low", " must not exceed open or close");
            } else if (bar.high < std::max(bar.open, bar.close)) {
                indexed("high", " must not be below open or close");
            } else {
                indexed("volume", " must be non-negative finite or NaN (unavailable)");
            }
        } else {
            refuse("native bar failed structural validation");
        }
        break;
    case NativeInputPreflightError::Unaligned:
        timestamped(" is not aligned to the configured calendar");
        break;
    case NativeInputPreflightError::OffGridLabel:
        if (spec->slot_label_policy == NativeSlotLabelPolicy::Canonical) {
            refuse(
                "native confirmed bar timestamp is not a canonical slot label");
        } else {
            timestamped(" is not a canonical slot label");
        }
        break;
    case NativeInputPreflightError::NotStrictlyIncreasing:
        timestamped(" must be strictly increasing");
        break;
    case NativeInputPreflightError::OverlappingSlot:
        timestamped(" overlaps the previous input slot");
        break;
    case NativeInputPreflightError::InSessionGap:
        timestamped(" follows an in-session gap");
        break;
    case NativeInputPreflightError::CalendarFailure:
        refuse("native calendar parse failed during input preflight");
        break;
    case NativeInputPreflightError::TimestampDeltaOverflow:
        timestamped(" delta exceeds int64 range");
        break;
    case NativeInputPreflightError::None:
        break;
    }
    return false;
}

bool NativeExecutionConsumer::preflight_intrabar_path(BacktestEngine& engine) {
    const auto* spec = spec_ptr();
    if (!spec || spec->intrabar.is_none() || spec->intrabar.synthesized_path()) return true;
    const auto& lower = *spec->intrabar.lower();
    if (!intrabar_tf_ || lower.bars.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        present_refusal(engine, "native intrabar path timeframe or bar count is invalid");
        return false;
    }
    NativeRunSpec path_spec = *spec;
    path_spec.input_tf = lower.tf;
    path_spec.script_tf = lower.tf;
    const auto result = preflight_native_inputs(
        path_spec, lower.bars.empty() ? nullptr : lower.bars.data(),
        static_cast<int>(lower.bars.size()), NativeInputPolicy::Batch);
    if (result) return true;
    present_refusal(engine, "native intrabar path failed validation");
    return false;
}

uint64_t NativeExecutionConsumer::take_ordinal(BacktestEngine&) {
    const uint64_t ordinal = next_timeline_ordinal_;
    if (ordinal == 0 || ordinal == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native timeline ordinal exhausted");
    }
    ++next_timeline_ordinal_;
    return ordinal;
}

void NativeExecutionConsumer::raise_floor(int64_t t) {
    if (!has_floor_ || t > decision_floor_ms_) {
        decision_floor_ms_ = t;
        has_floor_ = true;
    }
}

NativeCoordinate NativeExecutionConsumer::coordinate_from(
        const native_calendar::NativeInterval& interval,
        int index, int64_t effective,
        NativePriceProvenance provenance,
        NativePathPhase phase) const {
    NativeCoordinate c;
    c.interval_index = index;
    c.open_ms = interval.open_ms;
    c.eligible_open_ms = interval.eligible_open_ms;
    c.last_traded_close_ms = interval.last_traded_close_ms;
    c.next_period_open_ms = interval.next_period_open_ms;
    c.next_input_open_ms = interval.next_input_open_ms;
    c.effective_time_ms = effective;
    c.source_price_time_ms = effective;
    c.provenance = provenance;
    c.path_phase = phase;
    return c;
}

void NativeExecutionConsumer::sync_history_digest() const noexcept {
    const auto& hist = requests_.history();
    if (history_digest_.count > hist.size()) history_digest_.reset();
    if (history_digest_.count == hist.size()) return;
    Fnv f;
    f.run_base = requests_.identity().run_number;
    f.h = history_digest_.h;
    for (std::size_t i = history_digest_.count; i < hist.size(); ++i) {
        hash_command(f, hist[i]);
    }
    history_digest_.h = f.h;
    history_digest_.count = hist.size();
}

void NativeExecutionConsumer::fold_driver_digest(const NativeDriverPoint& point) const noexcept {
    Fnv f;
    f.run_base = requests_.identity().run_number;
    f.h = driver_digest_.h;
    hash_driver_point(f, point);
    driver_digest_.h = f.h;
    ++driver_digest_.count;
}

void NativeExecutionConsumer::fold_account_digest(const NativeAccountObservation& row) const noexcept {
    Fnv f;
    f.run_base = requests_.identity().run_number;
    f.h = account_digest_.h;
    hash_account_row(f, row);
    account_digest_.h = f.h;
    ++account_digest_.count;
}

void NativeExecutionConsumer::record_driver(const NativeDriverPoint& point) {
    driver_log_.push_back(point);
    // The driver log is an owning readback surface. Its continuation digest is
    // queried only by native-state consumers, so append the point now and
    // fold the derived digest lazily in continuation_hash() rather than at
    // every ordinary source-route waypoint.
}

void NativeExecutionConsumer::reserve_driver_log(std::size_t expected_points) {
    if (expected_points > driver_log_.max_size()) {
        throw std::length_error("native driver-log capacity exhausted");
    }
    if (expected_points > driver_log_.capacity()) driver_log_.reserve(expected_points);
    requests_.reserve(expected_points);
}

void NativeExecutionConsumer::apply_excursion(BacktestEngine& engine, double price) {
    // RULING A48: a host that declared ownership of per-lot excursion
    // accounting samples its own lots, so the kernel keeps no excursion
    // model for this run — neither at a matched trigger price nor at any
    // point of the delivered path.
    if (engine.lot_excursion_hook_) return;
    if (!std::isfinite(price)) return;
    for (auto& lot : engine.pyramid_entries_) {
        const bool is_long = engine.position_side_ == PositionSide::LONG;
        const double fav = (is_long ? (price - lot.price) : (lot.price - price)) * lot.qty;
        lot.max_runup = std::max(lot.max_runup, std::max(0.0, fav));
        lot.max_drawdown = std::max(lot.max_drawdown, std::max(0.0, -fav));
    }
}

native_order::DriverEligibilityClass NativeExecutionConsumer::classify_driver(
        const NativeDriverPoint& point, bool continuous) const noexcept {
    using Class = native_order::DriverEligibilityClass;
    switch (point.coordinate.provenance) {
    case NativePriceProvenance::CurrentExecution:
        return Class::CurrentExecution;
    case NativePriceProvenance::ObservedPrint:
        return Class::ObservedPrint;
    case NativePriceProvenance::CarriedOpen:
        return Class::CarriedOpen;
    case NativePriceProvenance::AfterCalculationClose:
        return input_mode_ == InputMode::ObservedTicks
            ? Class::TickAfterCalculation
            : Class::ConfirmedAfterCalculationClose;
    case NativePriceProvenance::ModeledOHLCOpen:
        return Class::ConfirmedOpen;
    case NativePriceProvenance::Confirmed:
        return continuous ? Class::ConfirmedExcursion : Class::ConfirmedOpen;
    case NativePriceProvenance::ModeledOHLCClose:
        return continuous ? Class::ConfirmedExcursion : Class::ConfirmedAfterCalculationClose;
    case NativePriceProvenance::PartialFinalized:
    case NativePriceProvenance::Calculation:
        return Class::ConfirmedAfterCalculationClose;
    }
    return static_cast<Class>(255); // unknown is never an observed print
}

native_order::MatchCursor NativeExecutionConsumer::make_cursor(
        const NativeDriverPoint& point, double t) const noexcept {
    native_order::MatchCursor cursor;
    cursor.point = point.coordinate;
    cursor.t = t;
    return cursor;
}

native_order::PositionIdentity NativeExecutionConsumer::read_position(
        const BacktestEngine& engine) const {
    if (engine.position_side_ == PositionSide::FLAT || engine.pyramid_entries_.empty()
        || engine.position_cycle_seq_ <= 0) {
        return native_order::PositionFlat{};
    }
    const native_order::Side side = engine.position_side_ == PositionSide::SHORT
        ? native_order::Side::Short : native_order::Side::Long;
    return native_order::PositionNonflat{engine.position_cycle_seq_, side};
}

native_order::OpeningObservation NativeExecutionConsumer::read_opening(
        const BacktestEngine& engine, const native_order::RequestHandle& opening,
        int64_t cycle) const {
    native_order::OpeningObservation out;
    out.queried_opening = opening;
    out.queried_cycle = cycle;
    out.current_position = read_position(engine);
    out.has_live_matching_lot = false;
    if (opening.run != requests_.identity()) return out;
    if (cycle != engine.position_cycle_seq_ || engine.position_cycle_seq_ <= 0) return out;
    if (engine.position_side_ == PositionSide::FLAT) return out;
    for (const auto& lot : engine.pyramid_entries_) {
        if (lot.entry_incarnation == opening.incarnation) {
            out.has_live_matching_lot = true;
            break;
        }
    }
    return out;
}

native_order::TargetObservation NativeExecutionConsumer::read_target(
        const BacktestEngine& engine, const native_order::LiveRequest* live) const {
    native_order::TargetObservation out;
    out.current_position = read_position(engine);
    if (!live) return out;
    if (const auto* opening = std::get_if<native_order::OpeningClose>(&live->authority)) {
        out.opening = read_opening(engine, opening->opening, opening->cycle);
    } else if (const auto* openings = std::get_if<native_order::OpeningsClose>(&live->authority)) {
        out.openings = read_openings(engine, openings->openings, openings->cycle);
    } else if (const auto* cohort = std::get_if<native_order::CohortClose>(&live->authority)) {
        const auto* position = std::get_if<native_order::PositionNonflat>(&out.current_position);
        if (!position) return out;
        std::vector<native_order::RequestHandle> handles;
        handles.reserve(engine.pyramid_entries_.size());
        for (const auto& lot : engine.pyramid_entries_) {
            native_order::RequestHandle handle{requests_.identity(), lot.entry_incarnation};
            if (requests_.cohort_contains(cohort->cohort, handle)) {
                handles.push_back(std::move(handle));
            }
        }
        out.openings = read_openings(engine, handles, position->cycle);
    } else if (const auto* bind = std::get_if<native_order::BindOpenings>(&live->request().owner)) {
        out.openings = read_openings(engine, bind->openings, bind->cycle);
    } else if (const auto* bind = std::get_if<native_order::BindOpening>(&live->request().owner)) {
        out.opening = read_opening(engine, bind->opening, bind->cycle);
    }
    return out;
}

const native_order::TargetObservation* NativeExecutionConsumer::cached_cohort_target(
        const BacktestEngine& engine, const native_order::LiveRequest& live) {
    const auto* spec = spec_ptr();
    if (!spec || !spec->intrabar.is_none()
        || !std::holds_alternative<native_order::CohortClose>(live.authority)) {
        return nullptr;
    }
    for (std::size_t index = 0; index < cohort_target_cache_size_; ++index) {
        if (cohort_target_cache_[index].handle == live.handle())
            return &cohort_target_cache_[index].target;
    }
    if (cohort_target_cache_size_ == cohort_target_cache_.size()) return nullptr;
    auto& entry = cohort_target_cache_[cohort_target_cache_size_++];
    entry.handle = live.handle();
    entry.target = read_target(engine, &live);
    return &entry.target;
}

void NativeExecutionConsumer::clear_cohort_target_cache() noexcept {
    cohort_target_cache_size_ = 0;
}

void NativeExecutionConsumer::retarget_cohort_target_cache(
        const native_order::RequestHandle& predecessor,
        const native_order::RequestHandle& successor) noexcept {
    for (std::size_t index = 0; index < cohort_target_cache_size_; ++index) {
        if (cohort_target_cache_[index].handle == predecessor)
            cohort_target_cache_[index].handle = successor;
    }
}

std::optional<native_order::Side> NativeExecutionConsumer::cohort_side(
        const BacktestEngine& engine, const native_order::LiveRequest& live) const {
    if (!std::holds_alternative<native_order::CohortClose>(live.authority)) {
        return std::nullopt;
    }
    const auto target = read_target(engine, &live);
    for (const auto& opening : target.openings) {
        if (!opening.has_live_matching_lot) continue;
        if (const auto* position = std::get_if<native_order::PositionNonflat>(
                &opening.current_position)) {
            return position->side;
        }
    }
    return std::nullopt;
}

bool NativeExecutionConsumer::request_is_buy(
        const BacktestEngine& engine, const native_order::LiveRequest& live) const {
    if (std::holds_alternative<native_order::CohortClose>(live.authority)) {
        const auto side = cohort_side(engine, live);
        return side && *side == native_order::Side::Short;
    }
    return requests_.working_is_buy(live);
}

native_order::CommandContext NativeExecutionConsumer::make_command_context(
        const BacktestEngine& engine, const native_order::Request& request,
        native_order::CommandSurface surface) const {
    native_order::CommandContext ctx;
    // A request submitted by the generic pre-open provider is born at this
    // open rather than at the prior decision floor.  Its one-point delivery
    // authorization is carried separately and consumed by match_discrete.
    const bool applied_point_is_current = callback_phase_ == CallbackPhase::Applied
        && current_frame_
        && (current_frame_->point.decision.coordinate.effective_time_ms >= decision_floor()
            || (remaining_path_coordinate(current_frame_->point.decision.coordinate)
                && !std::holds_alternative<native_order::Market>(request.trigger)));
    ctx.decision_time_ms = (callback_phase_ == CallbackPhase::PreOpen || applied_point_is_current)
            && current_frame_
        ? current_frame_->point.decision.coordinate.effective_time_ms : decision_floor();
    if (const auto* spec = spec_ptr()) ctx.quantity_grid = spec->quantity_grid;
    ctx.surface = surface;
    if (const auto* bind = std::get_if<native_order::BindOpening>(&request.owner)) {
        ctx.opening = read_opening(engine, bind->opening, bind->cycle);
    } else if (const auto* bind = std::get_if<native_order::BindOpenings>(&request.owner)) {
        ctx.openings = read_openings(engine, bind->openings, bind->cycle);
    }
    return ctx;
}

void NativeExecutionConsumer::refresh_target_scalars(
        const BacktestEngine& engine, native_order::TargetObservation& target) const noexcept {
    target.current_position = read_position(engine);
    refresh_openings(engine, target.openings);
    if (!target.opening) return;
    target.opening->current_position = target.current_position;
    target.opening->has_live_matching_lot = false;
    const auto& opening = target.opening->queried_opening;
    if (opening.run != requests_.identity()) return;
    if (target.opening->queried_cycle != engine.position_cycle_seq_
        || engine.position_cycle_seq_ <= 0) {
        return;
    }
    if (engine.position_side_ == PositionSide::FLAT) return;
    for (const auto& lot : engine.pyramid_entries_) {
        if (lot.entry_incarnation == opening.incarnation) {
            target.opening->has_live_matching_lot = true;
            break;
        }
    }
}

bool NativeExecutionConsumer::admit_opening_inspect(
        const BacktestEngine& engine, double resolved_price,
        const execution::SettlementInspection& inspect,
        bool skip_initial_margin,
        native_order::MatchRejectReason* reason) const {
    const auto* spec = spec_ptr();
    if (!spec || !inspect.would_open) return true;
    const auto mask = static_cast<uint32_t>(spec->allowed_open_directions);
    if (inspect.incoming_short && (mask & 2u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
    if (!inspect.incoming_short && (mask & 1u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
    if (spec->max_abs_units && inspect.resulting_abs_units > *spec->max_abs_units) {
        if (reason) *reason = native_order::MatchRejectReason::MaxAbsUnits;
        return false;
    }
    if (spec->max_open_lots && inspect.resulting_lot_count > *spec->max_open_lots) {
        if (reason) *reason = native_order::MatchRejectReason::MaxOpenLots;
        return false;
    }
    if (!skip_initial_margin && spec->initial_margin_fraction) {
        const double equity = engine.marked_equity(resolved_price) - inspect.current_ticket;
        const double required = inspect.resulting_abs_notional * *spec->initial_margin_fraction;
        if (!std::isfinite(equity) || !std::isfinite(required) || required > equity) {
            if (reason) *reason = native_order::MatchRejectReason::InitialMargin;
            return false;
        }
    }
    return true;
}

void NativeExecutionConsumer::fail_preparation(
        BacktestEngine& engine, const native_order::PreparationError& error,
        NativeFailureOperation operation) {
    NativeFailureCode code = NativeFailureCode::Contract;
    switch (error.code) {
    case native_order::CoreFailure::NonrepresentableQuantity:
    case native_order::CoreFailure::UnrepresentableReservation:
    case native_order::CoreFailure::InvalidProposal:
    case native_order::CoreFailure::InvalidScope:
        code = NativeFailureCode::SettlementFailure;
        break;
    case native_order::CoreFailure::MissingObservation:
    case native_order::CoreFailure::ObservationMismatch:
        code = NativeFailureCode::SettlementFailure;
        break;
    default:
        code = NativeFailureCode::Contract;
        break;
    }
    NativeFailure failure{code, operation, error.cause.ordinal,
                          static_cast<uint32_t>(error.code)};
    failure.context = native_failure_context_in_run(
            requests_.identity(), &error.cause, &error.target, nullptr);
    fail(engine, failure);
    render(engine, "native working-request preparation failed");
}

void NativeExecutionConsumer::catch_up_timeline() noexcept {
    if (requests_.history().empty()) return;
    const uint64_t last = command_ordinal(requests_.history().back());
    if (last >= next_timeline_ordinal_) next_timeline_ordinal_ = last + 1;
}

bool NativeExecutionConsumer::install_mutation(
        BacktestEngine& engine, native_order::PreparedMutation&& prepared,
        NativeFailureOperation operation, uint64_t ordinal) {
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, operation, ordinal});
        render(engine, "native mutation install missing preparation");
        return false;
    }
    const auto result = requests_.install_mutation(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&result)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, operation, ordinal,
                                   static_cast<uint32_t>(*err)});
        render(engine, "native mutation install failed");
        return false;
    }
    note_terminal_events(std::get<native_order::Installed>(result).events);
    clear_cohort_target_cache();
    // The continuation digest is a readback value.  Keep its append cursor
    // lazy: a source host that projects its own broker hash must not re-fold
    // every immutable history event at each ordinary command boundary.
    // continuation_hash() synchronizes it before exposing the value.
    catch_up_timeline();
    return true;
}

bool NativeExecutionConsumer::install_execution(
        BacktestEngine& engine, native_order::PreparedExecution&& prepared,
        const native_order::CommittedExecutionFacts& facts, uint64_t ordinal) {
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract,
                                   NativeFailureOperation::Settlement, ordinal});
        render(engine, "native execution install missing preparation");
        return false;
    }
    const auto result = requests_.install_execution(std::move(prepared), facts);
    if (const auto* err = std::get_if<native_order::InstallError>(&result)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract,
                                   NativeFailureOperation::Settlement, ordinal,
                                   static_cast<uint32_t>(*err)});
        render(engine, "native execution install failed after settlement");
        return false;
    }
    note_terminal_events(std::get<native_order::Installed>(result).events);
    clear_cohort_target_cache();
    catch_up_timeline();
    return true;
}

void NativeExecutionConsumer::drain_dependency_queue(
        BacktestEngine& engine,
        std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> seeds,
        NativeFailureOperation operation) {
    if (failed() || seeds.empty()) return;
    try {
        struct Item {
            uint64_t cause_ordinal = 0;
            native_order::EventId cause;
            native_order::RequestHandle child;
        };
        std::vector<Item> queue;
        auto less_item = [](const Item& a, const Item& b) {
            if (a.cause_ordinal != b.cause_ordinal) return a.cause_ordinal < b.cause_ordinal;
            return a.child.incarnation < b.child.incarnation;
        };
        auto enqueue_children = [&](const native_order::EventId& cause,
                                    const native_order::RequestHandle& parent,
                                    std::size_t sort_from) {
            auto children = requests_.waiting_children(parent);
            for (auto& child : children) {
                queue.push_back(Item{cause.ordinal, cause, std::move(child)});
            }
            if (sort_from < queue.size()) {
                std::sort(queue.begin() + static_cast<std::ptrdiff_t>(sort_from), queue.end(),
                          less_item);
            }
        };
        for (auto& seed : seeds) {
            enqueue_children(seed.first, seed.second, queue.size());
        }
        std::sort(queue.begin(), queue.end(), less_item);
        for (std::size_t i = 0; i < queue.size() && !failed(); ++i) {
            const native_order::EventId cause = queue[i].cause;
            const native_order::RequestHandle child = queue[i].child;
            auto prep = requests_.prepare_parent_terminal(cause, child, next_timeline_ordinal_);
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, operation);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation), operation,
                                               cause.ordinal)) {
                return;
            }
            if (requests_.find_live(child) == nullptr && !requests_.history().empty()) {
                native_order::EventId next_cause = cause;
                next_cause.ordinal = command_ordinal(requests_.history().back());
                enqueue_children(next_cause, child, i + 1);
            }
        }
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, operation,
                                   seeds.empty() ? 0 : seeds.front().first.ordinal});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::drain_parent_terminal(
        BacktestEngine& engine, const native_order::EventId& cause,
        const native_order::RequestHandle& parent,
        NativeFailureOperation operation) {
    // Most source replacements have no WaitForApplied descendants.  Avoid
    // constructing the dependency queue (and its seed allocation) for that
    // ordinary no-op while retaining the exact queue path once a child exists.
    if (!requests_.has_waiting_children(parent)) return;
    std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> seeds;
    try {
        seeds.push_back({cause, parent});
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, operation, cause.ordinal});
        render(engine, e.what());
        return;
    }
    drain_dependency_queue(engine, std::move(seeds), operation);
}

void NativeExecutionConsumer::drain_after_applied(
        BacktestEngine& engine, const native_order::EventId& applied,
        const native_order::RequestHandle& filler) {
    try {
        std::vector<std::pair<native_order::EventId, native_order::RequestHandle>> seeds;
        auto note_absent = [&](const native_order::RequestHandle& parent) {
            if (requests_.find_live(parent) != nullptr || requests_.history().empty()) return;
            native_order::EventId cause = applied;
            cause.ordinal = command_ordinal(requests_.history().back());
            seeds.push_back({std::move(cause), parent});
        };

        const auto recipients = requests_.group_recipients(applied);
        for (const auto& recipient : recipients) {
            if (failed()) return;
            auto prep = requests_.prepare_group_effect(applied, recipient, next_timeline_ordinal_);
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal)) {
                return;
            }
            note_absent(recipient);
        }

        const auto children = requests_.waiting_children(filler);
        for (const auto& child : children) {
            if (failed()) return;
            std::optional<native_order::OpeningObservation> observation;
            if (requests_.find_live(child)) {
                observation = read_opening(engine, filler, engine.position_cycle_seq_);
            }
            auto prep = requests_.prepare_owner_applied(applied, child, observation,
                                                        next_timeline_ordinal_);
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal)) {
                return;
            }
            note_absent(child);
        }

        const auto bound = requests_.bound_close_handles();
        for (const auto& handle : bound) {
            if (failed()) return;
            const auto* live = requests_.find_live(handle);
            auto target = read_target(engine, live);
            auto prep = requests_.prepare_bound_expiry(applied, handle, target,
                                                       next_timeline_ordinal_);
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) continue;
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement,
                                               applied.ordinal)) {
                return;
            }
            note_absent(handle);
        }

        seeds.push_back({applied, filler});
        drain_dependency_queue(engine, std::move(seeds), NativeFailureOperation::Settlement);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                   NativeFailureOperation::Settlement, applied.ordinal});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::observe_trails(
        BacktestEngine& engine, const NativeDriverPoint& point,
        const native_order::MatchCursor& cursor, bool continuous, double price) {
    if (!std::isfinite(price) || failed()) return;
    // A path with only market/limit/stop requests has no trailing state to
    // advance.  In particular, do not snapshot every live cohort request or
    // resolve its target side at each waypoint merely to discover that fact.
    const auto& live_requests = requests_.live();
    if (std::none_of(live_requests.begin(), live_requests.end(), [](const auto& live) {
            return std::holds_alternative<native_order::TrailTrack>(live.trigger_state);
        })) {
        return;
    }
    native_order::EvaluationContext evaluation;
    evaluation.cursor = cursor;
    evaluation.driver_class = classify_driver(point, continuous);
    evaluation.existing_matching_bit = point.matching;
    std::vector<native_order::RequestHandle> handles;
    handles.reserve(live_requests.size());
    for (const auto& live : live_requests) handles.push_back(live.handle());
    for (const auto& handle : handles) {
        if (failed()) return;
        const auto* live = requests_.find_live(handle);
        if (!live) continue;
        evaluation.cohort_side = cohort_side(engine, *live);
        if (std::holds_alternative<native_order::CohortClose>(live->authority)
            && !evaluation.cohort_side) {
            continue;
        }
        if (!requests_.evaluation_eligible(*live, evaluation)) continue;
        const auto* track = std::get_if<native_order::TrailTrack>(&live->trigger_state);
        if (!track) continue;
        const bool buy = request_is_buy(engine, *live);
        if (!native_matching::trail_best_improves(track->best, price, buy)) continue;
        native_order::Preparation<native_order::PreparedMutation> prep;
        try {
            prep = requests_.prepare_trigger(
                handle, native_order::ObserveTrailExtremum{cursor, price},
                evaluation.driver_class, next_timeline_ordinal_, evaluation.cohort_side);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Settlement, cursor.point.ordinal});
            render(engine, e.what());
            return;
        }
        if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
            fail_preparation(engine, *err, NativeFailureOperation::Settlement);
            return;
        }
        if (std::holds_alternative<native_order::NoChange>(prep)) continue;
        auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
        if (!mutation || !install_mutation(engine, std::move(*mutation),
                                           NativeFailureOperation::Settlement,
                                           cursor.point.ordinal)) {
            return;
        }
    }
}

void NativeExecutionConsumer::match_point(BacktestEngine& engine, const NativeDriverPoint& point) {
    match_discrete(engine, point);
}

void NativeExecutionConsumer::match_discrete(BacktestEngine& engine, const NativeDriverPoint& point) {
    match_path(engine, point, false, point.raw_price, point.raw_price);
    if (pre_open_birth_point_ordinal_ == point.coordinate.ordinal) {
        pre_open_birth_point_ordinal_ = 0;
        pre_open_birth_time_ms_ = 0;
        pre_open_births_.clear();
    }
}

bool NativeExecutionConsumer::pre_open_birth_eligible(
        const native_order::RequestHandle& handle, const NativeDriverPoint& point) const noexcept {
    if (pre_open_birth_point_ordinal_ != point.coordinate.ordinal
        || pre_open_birth_time_ms_ != point.coordinate.effective_time_ms
        || point.coordinate.path_phase != NativePathPhase::Open) {
        return false;
    }
    return std::find(pre_open_births_.begin(), pre_open_births_.end(), handle)
        != pre_open_births_.end();
}

void NativeExecutionConsumer::record_pre_open_birth(
        const native_order::Request& request, const native_order::RequestHandle& handle) {
    if (callback_phase_ != CallbackPhase::PreOpen || !current_frame_
        || !std::holds_alternative<native_order::Market>(request.trigger)
        || !std::holds_alternative<native_order::ImmediateRemaining>(request.capacity)
        || current_frame_->point.decision.coordinate.path_phase != NativePathPhase::Open) {
        return;
    }
    pre_open_birth_point_ordinal_ = current_frame_->point.decision.coordinate.ordinal;
    pre_open_birth_time_ms_ = current_frame_->point.decision.coordinate.effective_time_ms;
    if (std::find(pre_open_births_.begin(), pre_open_births_.end(), handle)
        == pre_open_births_.end()) {
        pre_open_births_.push_back(handle);
    }
}

void NativeExecutionConsumer::match_segment(
        BacktestEngine& engine, const NativeDriverPoint& dest, double from_price) {
    match_path(engine, dest, true, from_price, dest.raw_price);
}

std::vector<native_order::OpeningObservation> NativeExecutionConsumer::read_openings(
        const BacktestEngine& engine, const std::vector<native_order::RequestHandle>& handles,
        int64_t cycle) const {
    std::vector<native_order::OpeningObservation> out;
    out.reserve(handles.size());
    for (const auto& handle : handles) {
        native_order::OpeningObservation row;
        row.queried_opening = handle;
        row.queried_cycle = cycle;
        out.push_back(std::move(row));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.queried_opening.incarnation < b.queried_opening.incarnation;
    });
    refresh_openings(engine, out);
    return out;
}

void NativeExecutionConsumer::refresh_openings(const BacktestEngine& engine,
        std::vector<native_order::OpeningObservation>& openings) const noexcept {
    const auto position = read_position(engine);
    for (auto& row : openings) {
        row.current_position = position;
        row.has_live_matching_lot = false;
    }
    if (engine.position_side_ == PositionSide::FLAT || engine.position_cycle_seq_ <= 0) return;
    for (const auto& lot : engine.pyramid_entries_) {
        auto it = std::lower_bound(openings.begin(), openings.end(), lot.entry_incarnation,
            [](const auto& row, uint64_t incarnation) {
                return row.queried_opening.incarnation < incarnation;
            });
        for (; it != openings.end()
               && it->queried_opening.incarnation == lot.entry_incarnation; ++it) {
            if (it->queried_opening.run == requests_.identity()
                && it->queried_cycle == engine.position_cycle_seq_)
                it->has_live_matching_lot = true;
        }
    }
}

execution::Action NativeExecutionConsumer::narrow_action(
        const native_order::ExecutionPlan& plan) {
    return std::visit([](const auto& payload) -> execution::Action {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, execution::Flatten>
                      || std::is_same_v<T, order_action::Reduce>
                      || std::is_same_v<T, order_action::Transact>) {
            return payload;
        } else {
            throw std::logic_error("native reversal plan reached an action-only settlement");
        }
    }, plan);
}

NativeExecutionTermsFacts NativeExecutionConsumer::build_terms_facts(
        const BacktestEngine& engine, const native_order::LiveRequest& live,
        const native_order::EvaluationContext& evaluation,
        native_order::NativeCandidatePriceKind price_kind,
        NativeCurrentPriceRule price_rule, double raw_price,
        double default_resolved_price, bool shared_cursor_collision) const {
    NativeExecutionTermsFacts out;
    out.target = live.handle();
    out.definition = live.definition;
    out.cursor = evaluation.cursor;
    out.driver_class = evaluation.driver_class;
    out.trigger_state = live.trigger_state;
    out.remaining = live.remaining;
    out.allowance = evaluation.driver_class == native_order::DriverEligibilityClass::CurrentExecution
        ? native_order::WorkingRequestCore::evaluated_allowance(
              live, evaluation.cursor.point.ordinal)
        : live.allowance;
    out.position = position(engine);
    out.is_buy = std::holds_alternative<native_order::UnboundBookClose>(live.authority)
        ? engine.position_side_ == PositionSide::SHORT : request_is_buy(engine, live);
    out.price_kind = price_kind;
    out.shared_cursor_collision = shared_cursor_collision;
    out.raw_price = raw_price;
    out.default_resolved_price = default_resolved_price;
    out.price_rule = price_rule;
    out.quote_kind = evaluation.driver_class == native_order::DriverEligibilityClass::CurrentExecution
        && current_frame_ ? current_frame_->point.quote_kind
        : NativeCurrentQuoteKind::MarketDecision;
    out.fx_effective_time_ms = evaluation.cursor.point.effective_time_ms;
    out.active_fx = engine.account_currency_fx_at(out.fx_effective_time_ms);
    if (const auto* pending = std::get_if<native_order::PendingDeferred>(&live.pending)) {
        out.pending_group_deduction = pending->total;
    }

    double roster_units = 0.0;
    for (const auto& lot : engine.pyramid_entries_) {
        roster_units += lot.qty;
        if (!std::isfinite(roster_units)) {
            throw std::logic_error("native terms roster units are not finite");
        }
    }
    const bool opposite = (out.is_buy && engine.position_side_ == PositionSide::SHORT)
        || (!out.is_buy && engine.position_side_ == PositionSide::LONG);
    out.opposite_book_units = opposite ? roster_units : 0.0;

    const auto target = read_target(engine, &live);
    if (const auto* opening = std::get_if<native_order::OpeningClose>(&live.authority)) {
        const execution::OpeningExposure scope{opening->opening.incarnation, opening->cycle};
        out.scope = scope;
        for (const auto& lot : engine.pyramid_entries_) {
            if (lot.entry_incarnation == opening->opening.incarnation) {
                out.scope_exposure_units += lot.qty;
            }
        }
    } else if (const auto* openings = std::get_if<native_order::OpeningsClose>(&live.authority)) {
        native_order::SelectedExposure scope;
        scope.cycle = openings->cycle;
        for (const auto& row : target.openings) {
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        out.scope = scope;
        for (const auto& lot : engine.pyramid_entries_) {
            if (std::find(scope.incarnations.begin(), scope.incarnations.end(),
                          lot.entry_incarnation) != scope.incarnations.end()) {
                out.scope_exposure_units += lot.qty;
            }
        }
    } else if (std::holds_alternative<native_order::CohortClose>(live.authority)) {
        native_order::SelectedExposure scope;
        if (const auto* position = std::get_if<native_order::PositionNonflat>(
                &target.current_position)) {
            scope.cycle = position->cycle;
        }
        for (const auto& row : target.openings) {
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        out.scope = scope;
        for (const auto& lot : engine.pyramid_entries_) {
            if (std::find(scope.incarnations.begin(), scope.incarnations.end(),
                          lot.entry_incarnation) != scope.incarnations.end()) {
                out.scope_exposure_units += lot.qty;
            }
        }
    } else {
        out.scope = execution::Book{};
        out.scope_exposure_units = roster_units;
    }

    const auto& trigger = live.request().trigger;
    if (std::holds_alternative<native_order::LimitReady>(live.trigger_state)
        || std::holds_alternative<native_order::StopLimitLive>(live.trigger_state)) {
        if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
            out.trigger_level = limit->price;
        } else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&trigger)) {
            out.trigger_level = stop_limit->limit;
        } else {
            throw std::logic_error("native limit state has no limit trigger");
        }
    } else if (std::holds_alternative<native_order::StopActive>(live.trigger_state)) {
        const auto* stop = std::get_if<native_order::Stop>(&trigger);
        if (!stop) throw std::logic_error("native stop state has no stop trigger");
        out.trigger_level = stop->price;
    } else if (const auto* active = std::get_if<native_order::TrailActive>(&live.trigger_state)) {
        const auto* trail = std::get_if<native_order::Trail>(&trigger);
        double stop = 0.0;
        if (!trail || !native_matching::checked_trail_stop(
                active->best_at_trigger, trail->offset, out.is_buy, &stop)) {
            throw std::logic_error("native active trail has no representable trigger");
        }
        out.trigger_level = stop;
    } else if (!std::holds_alternative<native_order::MarketReady>(live.trigger_state)
               && evaluation.driver_class != native_order::DriverEligibilityClass::CurrentExecution) {
        throw std::logic_error("native nonfillable state reached terms coordinator");
    }
    return out;
}

native_order::ExecutionPlan NativeExecutionConsumer::plan_from_terms(
        native_order::HostSizedKind kind, std::optional<native_order::Side> side,
        native_order::OpeningShape shape, double after, double allowance_left,
        double opposite_book_units) {
    if (kind == native_order::HostSizedKind::Close) {
        return order_action::Reduce{std::min(after, allowance_left)};
    }
    if (!side) throw std::logic_error("native host-sized opening has no side");
    const double signed_after = *side == native_order::Side::Long ? after : -after;
    switch (shape) {
    case native_order::OpeningShape::Transact:
        return order_action::Transact{*side == native_order::Side::Long
            ? std::min(after, allowance_left) : -std::min(after, allowance_left)};
    case native_order::OpeningShape::ReverseTo:
        return execution::ReverseTo{signed_after};
    case native_order::OpeningShape::CloseOpposite:
        if (after == opposite_book_units) return execution::Flatten{};
        return order_action::Reduce{after};
    }
    throw std::logic_error("native host-sized opening shape is unknown");
}

NativeExecutionConsumer::ResolvedCandidate NativeExecutionConsumer::inspect_candidate(
        const BacktestEngine& engine, const native_order::LiveRequest& live,
        const native_order::MatchCursor& cursor, double resolved,
        const native_order::ExecutionPlan* plan_override) const {
    ResolvedCandidate candidate;
    candidate.target = read_target(engine, &live);
    if (const auto* opening = std::get_if<native_order::OpeningClose>(&live.authority)) {
        if (opening->opening.run != requests_.identity())
            throw std::logic_error("native opening scope identity mismatch");
        const execution::OpeningExposure scope{opening->opening.incarnation, opening->cycle};
        candidate.scope = scope;
        candidate.financial_scope = scope;
    } else if (const auto* openings = std::get_if<native_order::OpeningsClose>(&live.authority)) {
        native_order::SelectedExposure scope;
        scope.cycle = openings->cycle;
        scope.incarnations.reserve(candidate.target.openings.size());
        for (const auto& row : candidate.target.openings) {
            if (row.queried_opening.run != requests_.identity())
                throw std::logic_error("native selected scope identity mismatch");
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        candidate.selected = execution::SelectedOpeningSet{scope.cycle, scope.incarnations};
        candidate.scope = std::move(scope);
    } else if (std::holds_alternative<native_order::CohortClose>(live.authority)) {
        const auto* position = std::get_if<native_order::PositionNonflat>(
            &candidate.target.current_position);
        if (!position) throw std::logic_error("native cohort selection is flat");
        native_order::SelectedExposure scope;
        scope.cycle = position->cycle;
        scope.incarnations.reserve(candidate.target.openings.size());
        for (const auto& row : candidate.target.openings) {
            if (row.queried_opening.run != requests_.identity()) {
                throw std::logic_error("native cohort scope identity mismatch");
            }
            if (row.has_live_matching_lot) scope.incarnations.push_back(row.queried_opening.incarnation);
        }
        if (scope.incarnations.empty()) throw std::logic_error("native cohort selection is empty");
        candidate.selected = execution::SelectedOpeningSet{scope.cycle, scope.incarnations};
        candidate.scope = std::move(scope);
    } else if (!std::holds_alternative<native_order::BookTransaction>(live.authority)
               && !std::holds_alternative<native_order::ArmedTransaction>(live.authority)
               && !std::holds_alternative<native_order::BookClose>(live.authority)
               && !std::holds_alternative<native_order::UnboundBookClose>(live.authority)) {
        throw std::logic_error("native unresolved execution authority");
    }
    if (plan_override) {
        candidate.physical = *plan_override;
    } else if (!std::holds_alternative<native_order::RemainingFlattenAll>(live.remaining)
        && !std::holds_alternative<native_order::Flatten>(live.request().intent)) {
        const auto* remaining = std::get_if<native_order::RemainingUnits>(&live.remaining);
        if (!remaining) throw std::logic_error("native unresolved execution quantity");
        double qty = remaining->q;
        if (const auto* allowance = std::get_if<native_order::AllowanceUnits>(&live.allowance)) {
            if (allowance->point_ordinal == cursor.point.ordinal) qty = std::min(qty, allowance->left);
        }
        if (std::holds_alternative<native_order::Transact>(live.request().intent))
            candidate.physical = native_order::Transact{request_is_buy(engine, live) ? qty : -qty};
        else if (const auto* reverse = std::get_if<native_order::ReverseTo>(&live.request().intent))
            candidate.physical = execution::ReverseTo{reverse->signed_units};
        else
            candidate.physical = order_action::Reduce{qty};
    }
    candidate.fill = execution::Fill{resolved, live.request().label, live.request().comment,
                                    live.handle().incarnation, std::nullopt};
    if (const auto* reversal = std::get_if<execution::ReverseTo>(&candidate.physical)) {
        candidate.inspect = engine.inspect_native_reversal_v1(*reversal, candidate.fill);
    } else {
        const auto action = narrow_action(candidate.physical);
        candidate.inspect = candidate.selected
            ? engine.inspect_native_settlement_selected(action, candidate.fill, *candidate.selected)
            : engine.inspect_native_settlement_scoped(action, candidate.fill, candidate.financial_scope);
    }
    // Carry the inspection's binary64 ticket through every preview and the
    // real settlement commit; proportional allocation must not replace it.
    const double inspected_ticket = candidate.inspect.current_ticket;
    candidate.fill.commission_account = inspected_ticket;
    return candidate;
}

NativeCurrentPointView NativeExecutionConsumer::execution_anchor(
        const native_order::MatchCursor& cursor, double resolved) const {
    NativeCurrentPointView out;
    out.decision.coordinate = cursor.point;
    out.decision.decision_floor_ms = std::max(decision_floor(), cursor.point.effective_time_ms);
    if (auto input = input_interval_at(cursor.point.open_ms))
        out.decision.input_interval = *input;
    if (auto script = script_interval_at(cursor.point.open_ms))
        out.decision.script_interval = *script;
    out.decision.sub_index = callback_context_.sub_index;
    out.decision.sub_count = callback_context_.sub_count;
    out.decision.is_terminal_sub_bar = callback_context_.is_terminal_sub_bar;
    out.decision.sub_bar_open_ms = callback_context_.sub_bar_open_ms;
    out.decision.script_bar_open_ms = callback_context_.script_bar_open_ms;
    out.price = resolved;
    out.quote_kind = NativeCurrentQuoteKind::ExecutionAnchor;
    return out;
}

std::optional<NativeCurrentExecutionResult> NativeExecutionConsumer::terminal_from_history(
        BacktestEngine& engine, const native_order::RequestHandle& cause_handle,
        NativeFailureOperation operation) {
    if (requests_.history().empty()) {
        throw std::logic_error("native terminal history is empty");
    }
    const auto event = requests_.history().back();
    NativeCurrentExecutionResult outcome;
    if (const auto* rejected = std::get_if<native_order::MatchRejectedEvent>(&event)) {
        outcome = *rejected;
    } else if (const auto* no_effect = std::get_if<native_order::NoEffectEvent>(&event)) {
        outcome = *no_effect;
    } else if (const auto* cancelled = std::get_if<native_order::CancelledEvent>(&event)) {
        outcome = *cancelled;
    } else {
        throw std::logic_error("native terminal outcome missing");
    }
    const native_order::EventId cause{cause_handle.run, command_ordinal(event)};
    drain_parent_terminal(engine, cause, cause_handle, operation);
    if (failed()) return std::nullopt;
    return outcome;
}

std::optional<NativeCurrentExecutionResult> NativeExecutionConsumer::consume_matched_request(
        BacktestEngine& engine, const native_order::RequestHandle& handle,
        const native_order::EvaluationContext& evaluation, double raw_price,
        double default_resolved_price, const NativeCurrentPointView& notification_point,
        native_order::NativeCandidatePriceKind price_kind,
        NativeCurrentPriceRule price_rule, bool shared_cursor_collision) {
    const auto P = evaluation.cursor.point.ordinal;
    const bool current = evaluation.driver_class == native_order::DriverEligibilityClass::CurrentExecution;
    try {
        auto* live = requests_.find_live(handle);
        if (!live) return std::nullopt;
        // All resolver, inspection, admission and financial reads see the
        // execution coordinate's FX activation instant.
        engine.current_bar_.timestamp = evaluation.cursor.point.effective_time_ms;
        auto terminal = [&](std::optional<native_order::MatchRejectReason> rejection,
                            std::optional<native_order::ExecutionTerms> attempted = std::nullopt)
                -> std::optional<NativeCurrentExecutionResult> {
            auto prep = rejection
                ? requests_.prepare_match_rejected(
                    handle, evaluation, *rejection, std::move(attempted), next_timeline_ordinal_)
                : requests_.prepare_no_effect(handle, evaluation, next_timeline_ordinal_);
            if (const auto* error = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *error, NativeFailureOperation::Settlement);
                return std::nullopt;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation) return std::nullopt;
            if (!install_mutation(engine, std::move(*mutation), NativeFailureOperation::Settlement, P))
                return std::nullopt;
            return terminal_from_history(engine, handle, NativeFailureOperation::Settlement);
        };

        const auto* host_sized = host_sized_intent(*live);
        const bool unresolved = host_sized
            && (std::holds_alternative<native_order::RemainingDeferred>(live->remaining)
                || std::holds_alternative<native_order::NoTarget>(live->remaining));
        // This is the lone terms pre-resolver shortcut. The ordinary queued
        // evaluation path already owns the equivalent terminal.
        if (unresolved && host_sized->kind == native_order::HostSizedKind::Close
            && std::holds_alternative<native_order::UnboundBookClose>(live->authority)
            && engine.position_side_ == PositionSide::FLAT) {
            return terminal(std::nullopt);
        }

        const auto terms_facts = build_terms_facts(engine, *live, evaluation, price_kind,
                                                   price_rule, raw_price, default_resolved_price,
                                                   shared_cursor_collision);
        native_order::ExecutionTerms terms;
        try {
            auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
            if (!host) throw std::logic_error("native terms require a native host");
            terms = host->resolve_execution_terms(terms_facts);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement, P});
            render(engine, e.what());
            return std::nullopt;
        } catch (...) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement, P});
            render(engine, "native terms callback exception");
            return std::nullopt;
        }
        // A virtual can invoke a guarded public entry point and latch the run,
        // request abort, or perturb projection state without throwing.
        if (!check_abort_or_projection(engine, NativeFailureOperation::Settlement, P)) {
            return std::nullopt;
        }

        const bool identity = identity_terms(terms, default_resolved_price);
        const auto nonidentity_attempt = identity
            ? std::optional<native_order::ExecutionTerms>{}
            : std::optional<native_order::ExecutionTerms>{terms};
        if (!unresolved && (terms.units || terms.shape != native_order::OpeningShape::Transact)) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (unresolved && !terms.units) {
            return terminal(native_order::MatchRejectReason::TermsUnresolved, terms);
        }
        if (unresolved && host_sized->kind == native_order::HostSizedKind::Close
            && terms.shape != native_order::OpeningShape::Transact) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (unresolved && terms.shape != native_order::OpeningShape::Transact
            && terms.shape != native_order::OpeningShape::ReverseTo
            && terms.shape != native_order::OpeningShape::CloseOpposite) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (terms.units && (!std::isfinite(*terms.units) || *terms.units < 0.0)) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (!execution_terms_grid_representable(
                terms, host_sized, unresolved, terms_facts.scope_exposure_units,
                spec_ptr())) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }

        double after = 0.0;
        double deduction = 0.0;
        bool exhausted = false;
        double binding_allowance = 0.0;
        if (unresolved) {
            if (!native_order::WorkingRequestCore::effective_host_units(
                    live->pending, *terms.units, &deduction, &after, &exhausted)) {
                native_order::TermsResolvedInput probe;
                probe.price_kind = price_kind;
                probe.shared_cursor_collision = shared_cursor_collision;
                probe.raw_price = raw_price;
                probe.default_resolved_price = default_resolved_price;
                probe.terms = terms;
                auto prepared = requests_.prepare_terms(handle, evaluation, probe,
                                                        next_timeline_ordinal_);
                if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
                    fail_preparation(engine, *error, NativeFailureOperation::Settlement);
                } else {
                    fail(engine, NativeFailure{NativeFailureCode::Contract,
                                               NativeFailureOperation::Settlement, P});
                    render(engine, "native host-sized arithmetic probe did not fail");
                }
                return std::nullopt;
            }
            if (const auto* budget = std::get_if<native_order::PointBudget>(&live->request().capacity)) {
                binding_allowance = std::min(after, budget->units);
            } else {
                binding_allowance = after;
            }
        }

        const bool explicit_reversal = std::holds_alternative<native_order::ReverseTo>(
            live->request().intent);
        const bool shape_requires_opposite = explicit_reversal || (unresolved
            && (terms.shape == native_order::OpeningShape::ReverseTo
                || terms.shape == native_order::OpeningShape::CloseOpposite));
        if (shape_requires_opposite && terms_facts.opposite_book_units == 0.0) {
            return terminal(native_order::MatchRejectReason::NoOppositeExposure,
                            unresolved ? std::optional<native_order::ExecutionTerms>{terms}
                                       : nonidentity_attempt);
        }
        if (unresolved && terms.shape == native_order::OpeningShape::CloseOpposite
            && *terms.units > terms_facts.opposite_book_units) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }
        if (unresolved && terms.shape != native_order::OpeningShape::Transact
            && after > binding_allowance) {
            return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
        }

        std::optional<native_order::ExecutionPlan> plan;
        if (host_sized) {
            if (unresolved) {
                if (after > 0.0) {
                    plan = plan_from_terms(host_sized->kind, host_sized->side, terms.shape,
                                           after, binding_allowance, terms_facts.opposite_book_units);
                }
            } else if (const auto* remaining = std::get_if<native_order::RemainingUnits>(
                           &live->remaining)) {
                plan = plan_from_terms(host_sized->kind, host_sized->side,
                    native_order::OpeningShape::Transact, remaining->q,
                    allowance_left_at(terms_facts.allowance, P),
                terms_facts.opposite_book_units);
            }
        }

        const double resolved_price = terms.resolved_price;
        const bool zero_units_terminal = unresolved && *terms.units == 0.0;
        // A group-exhausting binding has no executable plan, just like the
        // zero-units terminal.  The current finite-price boundary must not
        // attempt its provisional inspection on the still-deferred row: the
        // receipt itself installs the terminal Cancelled{Group} outcome.
        const bool no_plan_terminal = zero_units_terminal
            || (unresolved && after == 0.0 && deduction > 0.0);
        // Price rejection is deliberately ahead of receipt installation. The
        // sole finite-current exception needs a stack-only plan inspection to
        // distinguish a pure close from an opening plan.
        if (!std::isfinite(resolved_price)) {
            if (current) throw std::overflow_error("native current price is not finite");
            return terminal(native_order::MatchRejectReason::NonpositivePrice, nonidentity_attempt);
        }
        if (!current && resolved_price <= 0.0 && !zero_units_terminal) {
            return terminal(native_order::MatchRejectReason::NonpositivePrice, nonidentity_attempt);
        }
        if (current && resolved_price <= 0.0 && !no_plan_terminal) {
            const auto provisional = inspect_candidate(engine, *live, evaluation.cursor, resolved_price,
                                                       plan ? &*plan : nullptr);
            if (provisional.inspect.would_open) {
                return terminal(native_order::MatchRejectReason::NonpositivePrice,
                                nonidentity_attempt);
            }
        }
        if (!no_plan_terminal
            && (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state))) {
            std::optional<double> level;
            bool fill_through = false;
            if (const auto* limit = std::get_if<native_order::Limit>(&live->request().trigger)) {
                level = limit->price;
                fill_through = limit->fill_through;
            } else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(
                           &live->request().trigger)) {
                level = stop_limit->limit;
            }
            // A fill-through limit is a touch trigger: its terms may settle
            // past the level.
            if (!level || (!fill_through
                           && ((terms_facts.is_buy && resolved_price > *level)
                               || (!terms_facts.is_buy && resolved_price < *level)))) {
                return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
            }
        }

        native_order::TermsResolvedInput terms_input;
        terms_input.price_kind = price_kind;
        terms_input.shared_cursor_collision = shared_cursor_collision;
        terms_input.raw_price = raw_price;
        terms_input.default_resolved_price = default_resolved_price;
        terms_input.terms = terms;
        if (unresolved || !identity) {
            auto prepared = requests_.prepare_terms(handle, evaluation, terms_input,
                                                    next_timeline_ordinal_);
            if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
                fail_preparation(engine, *error, NativeFailureOperation::Settlement);
                return std::nullopt;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prepared);
            if (!mutation) {
                fail(engine, NativeFailure{NativeFailureCode::Contract,
                                           NativeFailureOperation::Settlement, P});
                render(engine, "native terms preparation did not produce a mutation");
                return std::nullopt;
            }
            if (!install_mutation(engine, std::move(*mutation),
                                  NativeFailureOperation::Settlement, P)) {
                return std::nullopt;
            }
            live = requests_.find_live(handle);
            if (!live) {
                return terminal_from_history(engine, handle, NativeFailureOperation::Settlement);
            }
            if (host_sized && !plan) {
                const auto* remaining = std::get_if<native_order::RemainingUnits>(&live->remaining);
                if (!remaining) {
                    fail(engine, NativeFailure{NativeFailureCode::Contract,
                                               NativeFailureOperation::Settlement, P});
                    render(engine, "native bound terms have no remaining quantity");
                    return std::nullopt;
                }
                plan = plan_from_terms(host_sized->kind, host_sized->side,
                    native_order::OpeningShape::Transact, remaining->q,
                    allowance_left_at(live->allowance, P), terms_facts.opposite_book_units);
            }
        }

        auto candidate = inspect_candidate(engine, *live, evaluation.cursor, resolved_price,
                                           plan ? &*plan : nullptr);
        const auto& inspect = candidate.inspect;
        if (inspect.status == execution::Status::NoEffect) return terminal(std::nullopt);
        if (inspect.status != execution::Status::Applied) {
            fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                NativeFailureOperation::Settlement, P, static_cast<uint32_t>(inspect.status)});
            render(engine, "native settlement inspection failed");
            return std::nullopt;
        }
        if (inspect.would_open && resolved_price <= 0.0)
            return terminal(native_order::MatchRejectReason::NonpositivePrice, nonidentity_attempt);

        native_order::ExecutionProposal proposal;
        proposal.cursor = evaluation.cursor;
        proposal.pre_open_birth_eligible = evaluation.pre_open_birth_eligible;
        proposal.raw_price = raw_price;
        proposal.resolved_price = resolved_price;
        proposal.physical_action = candidate.physical;
        proposal.scope = candidate.scope;
        proposal.pre_fill = read_position(engine);
        proposal.pre_target = candidate.target;
        proposal.inspected_closed_units = inspect.closed_units;
        proposal.inspected_opened_units = inspect.opened_units;
        proposal.inspected_current_ticket = *candidate.fill.commission_account;
        const int64_t cycle_before = engine.position_cycle_seq_;
        const native_order::EventId applied_id{handle.run, next_timeline_ordinal_};
        auto prepared = requests_.prepare_execution(handle, proposal, next_timeline_ordinal_);
        if (const auto* error = std::get_if<native_order::PreparationError>(&prepared)) {
            fail_preparation(engine, *error, NativeFailureOperation::Settlement);
            return std::nullopt;
        }
        auto* token = std::get_if<native_order::PreparedExecution>(&prepared);
        if (!token) {
            if (host_sized) {
                fail(engine, NativeFailure{NativeFailureCode::Contract,
                                           NativeFailureOperation::Settlement, P});
                render(engine, "native host-sized candidate silently skipped after binding");
            }
            return std::nullopt;
        }
        execution::PhysicalExecutionContext ctx;
        ctx.effective_time_ms = evaluation.cursor.point.effective_time_ms;
        ctx.interval_index = evaluation.cursor.point.interval_index;
        NativePrecommitView view;
        view.target = handle;
        view.definition = live->definition;
        view.cursor = evaluation.cursor;
        view.plan = candidate.physical;
        view.scope = candidate.scope;
        view.raw_price = raw_price;
        view.resolved_price = resolved_price;
        view.inspected_closed_units = inspect.closed_units;
        view.inspected_opened_units = inspect.opened_units;
        view.inspected_current_ticket = proposal.inspected_current_ticket;
        view.current = current;
        if (const auto* reversal = std::get_if<execution::ReverseTo>(&candidate.physical)) {
            view.settlement_readiness = engine.preview_native_settlement_commit(
                *reversal, candidate.fill, ctx, view.account, view.closed_row_pnl);
        } else {
            const auto action = narrow_action(candidate.physical);
            view.settlement_readiness = engine.preview_native_settlement_commit(
                action, candidate.fill, ctx, candidate.financial_scope,
                candidate.selected ? &*candidate.selected : nullptr,
                view.account, view.closed_row_pnl);
        }
        NativePrecommitVerdict verdict = NativePrecommitVerdict::Admit;
        if (view.settlement_readiness == execution::Status::Applied) {
            try {
                auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
                if (!host) throw std::logic_error("native precommit requires a native host");
                verdict = host->validate_execution_precommit(view);
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return std::nullopt;
            } catch (...) {
                fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                           NativeFailureOperation::Settlement, P});
                render(engine, "native precommit callback exception");
                return std::nullopt;
            }
            if (!check_abort_or_projection(engine, NativeFailureOperation::Settlement, P)) {
                return std::nullopt;
            }
            if (verdict == NativePrecommitVerdict::AdmitWithHostMargin) {
                const auto* admitted_spec = spec_ptr();
                if (admitted_spec && admitted_spec->initial_margin_fraction) {
                    Fnv digest;
                    digest.run_base = requests_.identity().run_number;
                    digest.h = precommit_digest_.h;
                    digest.u(P);
                    digest.u(static_cast<std::uint64_t>(verdict));
                    hash_handle(digest, handle);
                    precommit_digest_.h = digest.h;
                    ++precommit_digest_.count;
                }
            }
            if (verdict == NativePrecommitVerdict::Refuse) {
                return terminal(native_order::MatchRejectReason::HostPrecommit,
                                nonidentity_attempt);
            }
        }
        native_order::MatchRejectReason reason{};
        if (inspect.would_open
            && !admit_opening_inspect(
                engine, resolved_price, inspect,
                verdict == NativePrecommitVerdict::AdmitWithHostMargin, &reason)) {
            return terminal(reason, nonidentity_attempt);
        }
        // Allocate before financial effects, with geometric growth rather than
        // recopying the complete observation/notification prefix on each fill.
        reserve_next(account_log_);
        reserve_next(applied_notifications_);
        AppliedNotification notification;
        notification.history_index = requests_.history().size();
        notification.ordinal = applied_id.ordinal;
        notification.point = notification_point;
        if (!current) {
            notification.point.price = resolved_price;
            notification.point.quote_origin_ordinal = applied_id.ordinal;
        }
        const auto settled = std::get_if<execution::ReverseTo>(&candidate.physical)
            ? engine.settle_native_reversal_at_v1(*std::get_if<execution::ReverseTo>(
                  &candidate.physical), candidate.fill, ctx)
            : candidate.selected
                ? engine.settle_native_execution_selected_at(
                    narrow_action(candidate.physical), candidate.fill, ctx, *candidate.selected)
                : engine.settle_native_execution_scoped_at(
                    narrow_action(candidate.physical), candidate.fill, ctx, candidate.financial_scope);
        if (settled.status != execution::Status::Applied) {
            fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                NativeFailureOperation::Settlement, P, static_cast<uint32_t>(settled.status)});
            render(engine, "native settlement commit failed");
            return std::nullopt;
        }
        refresh_target_scalars(engine, candidate.target);
        native_order::CommittedExecutionFacts committed;
        committed.result = settled;
        committed.cycle_before = cycle_before;
        committed.cycle_after = engine.position_cycle_seq_;
        committed.post_target = std::move(candidate.target);
        committed.committed_action = candidate.physical;
        if (!install_execution(engine, std::move(*token), committed, P)) return std::nullopt;
        const auto& applied = std::get<native_order::ExecutionAppliedEvent>(
            requests_.history().at(notification.history_index));
        if (applied.ordinal != applied_id.ordinal || (current && !applied.terminal))
            throw std::logic_error("native execution receipt mismatch");
        NativeCurrentExecutionResult outcome{applied};
        NativeAccountObservation observation;
        observation.ordinal = applied_id.ordinal;
        observation.effective_time_ms = ctx.effective_time_ms;
        observation.marked_equity = engine.marked_equity(resolved_price);
        observation.realized_balance = engine.initial_capital_ + engine.net_profit_sum_;
        for (const auto& lot : engine.pyramid_entries_) observation.signed_units += lot.qty;
        if (engine.position_side_ == PositionSide::SHORT) observation.signed_units = -observation.signed_units;
        account_log_.push_back(observation);
        fold_account_digest(observation);
        engine.bar_index_ = ctx.interval_index;
        drain_after_applied(engine, applied_id, handle);
        if (failed()) return std::nullopt;
        enqueue_applied_notification(std::move(notification));
        return outcome;
    } catch (const std::bad_alloc& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Settlement, P});
        render(engine, e.what());
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::SettlementFailure, NativeFailureOperation::Settlement, P});
        render(engine, e.what());
    } catch (...) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Settlement, P});
    }
    return std::nullopt;
}

void NativeExecutionConsumer::match_path(
        BacktestEngine& engine, const NativeDriverPoint& point,
        bool continuous, double from_price, double to_price) {
    if (failed()) return;
    if (point.coordinate.provenance == NativePriceProvenance::CurrentExecution) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Settlement,
                                   point.coordinate.ordinal});
        render(engine, "current execution points require a guarded target");
        return;
    }
    const auto* spec = spec_ptr();
    if (!spec) return;
    // With no live request there is no trigger, allowance, trail, receipt or
    // callback work to perform. Keep the one physical excursion effect the
    // regular segment path would have applied to an already-open position.
    if (requests_.live().empty()) {
        if (continuous) apply_excursion(engine, to_price);
        return;
    }
    const auto driver_class = classify_driver(point, continuous);
    const auto grid = grid_threshold(*spec);
    const uint64_t P = point.coordinate.ordinal;
    requests_.refresh_point_allowances(P, read_position(engine));
    double t_cursor = 0.0;
    double cursor_price = from_price;
    native_order::MatchCursor path_cursor = make_cursor(point, t_cursor);
    observe_trails(engine, point, path_cursor, continuous,
                   native_matching::price_at(from_price, to_price, t_cursor));

    enum class Kind : std::uint8_t {
        Evaluate = 0,
        ActivateStop = 1,
        ActivateStopLimit = 2,
        BeginTrail = 3,
        ActivateTrail = 4,
        Fill = 5,
    };
    struct Candidate {
        native_order::RequestHandle handle;
        uint64_t incarnation = 0;
        double t = 0.0;
        double price = 0.0;
        Kind kind = Kind::Fill;
        std::optional<double> trigger_level;
        bool at_level = false;
        bool shared_cursor_collision = false;
    };
    struct CandidateProvenance {
        native_order::RequestHandle handle;
        std::uint64_t point_ordinal = 0;
        Kind kind = Kind::Fill;
        std::size_t trigger_state_index = 0;
        bool is_buy = false;
        std::optional<double> trigger_level;
        double t = 0.0;
        double raw_price = 0.0;
        bool at_level = false;
    };
    std::vector<CandidateProvenance> candidate_provenance;

    auto same_optional_bits = [](const std::optional<double>& left,
                                 const std::optional<double>& right) {
        return left.has_value() == right.has_value()
            && (!left || native_matching::double_bits(*left)
                         == native_matching::double_bits(*right));
    };
    auto same_provenance_key = [&](const CandidateProvenance& row,
                                   const native_order::LiveRequest& live,
                                   Kind kind, bool buy,
                                   const std::optional<double>& level,
                                   double t) {
        return row.handle == live.handle()
            && row.point_ordinal == P
            && row.kind == kind
            && row.trigger_state_index == live.trigger_state.index()
            && row.is_buy == buy
            && same_optional_bits(row.trigger_level, level)
            && native_matching::double_bits(row.t) == native_matching::double_bits(t);
    };
    auto retained_origin = [&](const native_order::LiveRequest& live, Kind kind, bool buy,
                               const std::optional<double>& level, double t)
            -> const CandidateProvenance* {
        for (auto it = candidate_provenance.rbegin(); it != candidate_provenance.rend(); ++it) {
            if (same_provenance_key(*it, live, kind, buy, level, t)) return &*it;
        }
        return nullptr;
    };
    auto level_for = [&](const native_order::LiveRequest& live, Kind kind,
                         bool buy) -> std::optional<double> {
        const auto& trigger = live.request().trigger;
        const auto& state = live.trigger_state;
        if (std::holds_alternative<native_order::LimitReady>(state)
            || std::holds_alternative<native_order::StopLimitLive>(state)) {
            if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) return limit->price;
            if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&trigger)) {
                return stop_limit->limit;
            }
        }
        if (std::holds_alternative<native_order::StopIdle>(state)
            || std::holds_alternative<native_order::StopActive>(state)) {
            if (const auto* stop = std::get_if<native_order::Stop>(&trigger)) return stop->price;
        }
        if (std::holds_alternative<native_order::StopLimitPending>(state)) {
            if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&trigger)) {
                return stop_limit->stop;
            }
        }
        if (std::holds_alternative<native_order::TrailWaitArm>(state)) {
            if (const auto* trail = std::get_if<native_order::Trail>(&trigger)) return trail->arm_price;
        }
        if (const auto* tracking = std::get_if<native_order::TrailTrack>(&state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            double stop = 0.0;
            if (trail && native_matching::checked_trail_stop(
                    tracking->best, trail->offset, buy, &stop)) return stop;
        }
        if (const auto* active = std::get_if<native_order::TrailActive>(&state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            double stop = 0.0;
            if (trail && native_matching::checked_trail_stop(
                    active->best_at_trigger, trail->offset, buy, &stop)) return stop;
        }
        (void)kind;
        return std::nullopt;
    };
    auto erase_provenance_for = [&](const native_order::RequestHandle& handle) {
        candidate_provenance.erase(
            std::remove_if(candidate_provenance.begin(), candidate_provenance.end(),
                [&](const CandidateProvenance& row) { return row.handle == handle; }),
            candidate_provenance.end());
    };
    auto cause_floor = [&](const native_order::LiveRequest& live) {
        double t_min = t_cursor;
        if (const auto* armed = std::get_if<native_order::ArmedTransaction>(&live.authority)) {
            if (armed->cause_cursor.point.ordinal == P) t_min = std::max(t_min, armed->cause_cursor.t);
        }
        if (const auto* opening = std::get_if<native_order::OpeningClose>(&live.authority)) {
            if (const auto* from = std::get_if<native_order::EnrollmentFromApplied>(&opening->enrollment)) {
                if (from->cursor.point.ordinal == P) t_min = std::max(t_min, from->cursor.t);
            }
        }
        return t_min;
    };

    // The driver point is allocated before matching this monotonic segment.
    // Therefore an acceptance ordinal after P can only have been created by a
    // callback at the current path cursor. Admit that birth at t_cursor; the
    // ordinary geometric search then sees only the unconsumed suffix. Requests
    // accepted before this segment and discrete points retain the existing gate.
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };

    auto needs_evaluation = [&](const native_order::LiveRequest& live,
                                const native_order::EligibilityFacts& facts) {
        if (facts.needs_close_bind) return true;
        if (std::holds_alternative<native_order::AllowanceUnset>(live.allowance)) return true;
        if (const auto* units = std::get_if<native_order::AllowanceUnits>(&live.allowance)) {
            return units->point_ordinal != P;
        }
        if (const auto* all = std::get_if<native_order::AllowanceAllScope>(&live.allowance)) {
            return all->point_ordinal != P;
        }
        if (const auto* deferred = std::get_if<native_order::AllowanceDeferred>(&live.allowance)) {
            return deferred->point_ordinal != P;
        }
        return false;
    };

    auto side_from_target = [](const native_order::TargetObservation& target)
            -> std::optional<native_order::Side> {
        for (const auto& opening : target.openings) {
            if (!opening.has_live_matching_lot) continue;
            if (const auto* position = std::get_if<native_order::PositionNonflat>(
                    &opening.current_position)) {
                return position->side;
            }
        }
        return std::nullopt;
    };

    auto provenance_still_matches = [&](const CandidateProvenance& row) {
        const auto* live = requests_.find_live(row.handle);
        if (!live) return false;
        const auto* target = cached_cohort_target(engine, *live);
        const bool buy = target ? requests_.working_is_buy(*live, side_from_target(*target))
                                : request_is_buy(engine, *live);
        return row.is_buy == buy
            && row.trigger_state_index == live->trigger_state.index()
            && same_optional_bits(row.trigger_level, level_for(*live, row.kind, buy));
    };

    std::set<std::pair<uint64_t, std::uint8_t>> skipped;
    double skip_t = t_cursor;
    auto skip_key = [](uint64_t incarnation, Kind kind) {
        return std::pair<uint64_t, std::uint8_t>{incarnation, static_cast<std::uint8_t>(kind)};
    };

    while (!failed()) {
        candidate_provenance.erase(
            std::remove_if(candidate_provenance.begin(), candidate_provenance.end(),
                [&](const CandidateProvenance& row) {
                    return row.point_ordinal != P || row.t < t_cursor
                        || !provenance_still_matches(row);
                }),
            candidate_provenance.end());
        if (skip_t != t_cursor) {
            skipped.clear();
            skip_t = t_cursor;
        }
        native_order::EvaluationContext eval;
        eval.cursor = make_cursor(point, t_cursor);
        eval.driver_class = driver_class;
        eval.existing_matching_bit = point.matching;
        std::optional<Candidate> winner;
        // Candidate selection makes no request-core mutation; only the
        // selected winner can replace or retire a later request afterwards.
        // Snapshot pointers through that selection pass. The ordinary route
        // has only the two bracket siblings, so keep it on the stack.
        std::array<const native_order::LiveRequest*, 8> inline_snapshot{};
        std::vector<const native_order::LiveRequest*> overflow_snapshot;
        const auto& live_requests = requests_.live();
        const native_order::LiveRequest* const* snapshot = inline_snapshot.data();
        const std::size_t snapshot_size = live_requests.size();
        if (snapshot_size <= inline_snapshot.size()) {
            for (std::size_t i = 0; i < snapshot_size; ++i)
                inline_snapshot[i] = &live_requests[i];
        } else {
            overflow_snapshot.reserve(snapshot_size);
            for (const auto& live : live_requests) overflow_snapshot.push_back(&live);
            snapshot = overflow_snapshot.data();
        }
        for (std::size_t snapshot_index = 0; snapshot_index < snapshot_size; ++snapshot_index) {
            const auto* live = snapshot[snapshot_index];
            if (!live) continue;
            const auto& handle = live->handle();
            native_order::EvaluationContext candidate_eval = eval;
            candidate_eval.pre_open_birth_eligible = pre_open_birth_eligible(handle, point)
                || born_on_remaining_path(*live);
            const native_order::TargetObservation* candidate_target = nullptr;
            std::optional<native_order::TargetObservation> uncached_target;
            if (std::holds_alternative<native_order::CohortClose>(live->authority)) {
                // Candidate selection is read-only. Reuse its complete target
                // observation for the side and trigger calculations, then
                // rebuild at the selected mutation boundary below.
                candidate_target = cached_cohort_target(engine, *live);
                if (!candidate_target) {
                    uncached_target = read_target(engine, live);
                    candidate_target = &*uncached_target;
                }
                candidate_eval.cohort_side = side_from_target(*candidate_target);
            }
            if (std::holds_alternative<native_order::CohortClose>(live->authority)
                && !candidate_eval.cohort_side) {
                erase_provenance_for(handle);
                continue;
            }
            const auto facts = requests_.eligibility_facts(*live, candidate_eval);
            if (!facts.birth_ok || facts.waiting || !facts.driver_ok) {
                erase_provenance_for(handle);
                continue;
            }
            const double t_min = cause_floor(*live);
            if (t_min > 1.0) {
                erase_provenance_for(handle);
                continue;
            }
            const native_matching::GeometricHit start{
                t_min, t_min == t_cursor ? cursor_price
                                        : native_matching::price_at(from_price, to_price, t_min)};
            Candidate row;
            row.handle = handle;
            row.incarnation = handle.incarnation;
            if (needs_evaluation(*live, facts)) {
                erase_provenance_for(handle);
                row.t = t_min;
                row.price = start.price;
                row.kind = Kind::Evaluate;
            } else {
                const bool buy = std::holds_alternative<native_order::CohortClose>(live->authority)
                    ? requests_.working_is_buy(*live, candidate_eval.cohort_side)
                    : request_is_buy(engine, *live);
                // A callback-born priced request begins immediately after the
                // birth print. It may cross a later level on this suffix, but
                // it does not inherit an already-consumed/equal crossing from
                // the request that produced the callback.
                const bool include_current = !born_on_remaining_path(*live);
                const auto& trigger = live->request().trigger;
                const auto& state = live->trigger_state;
                std::optional<native_matching::GeometricHit> hit;
                Kind kind = Kind::Fill;
                if (std::holds_alternative<native_order::StopIdle>(state)) {
                    const auto* stop = std::get_if<native_order::Stop>(&trigger);
                    if (!stop) continue;
                    hit = native_matching::first_region_entry(
                        from_price, to_price, start, stop->price, !buy, include_current, grid);
                    kind = Kind::ActivateStop;
                } else if (std::holds_alternative<native_order::StopLimitPending>(state)) {
                    const auto* sl = std::get_if<native_order::StopLimit>(&trigger);
                    if (!sl) continue;
                    hit = native_matching::first_region_entry(
                        from_price, to_price, start, sl->stop, !buy, include_current, grid);
                    kind = Kind::ActivateStopLimit;
                } else if (std::holds_alternative<native_order::TrailWaitArm>(state)) {
                    const auto* trail = std::get_if<native_order::Trail>(&trigger);
                    if (!trail) continue;
                    if (!trail->arm_price) {
                        hit = start;
                    } else {
                        hit = native_matching::first_region_entry(
                            from_price, to_price, start, *trail->arm_price, buy,
                            include_current, grid);
                    }
                    kind = Kind::BeginTrail;
                } else if (const auto* track = std::get_if<native_order::TrailTrack>(&state)) {
                    const auto* trail = std::get_if<native_order::Trail>(&trigger);
                    if (!trail) continue;
                    double stop = 0.0;
                    if (!native_matching::checked_trail_stop(track->best, trail->offset, buy, &stop)) {
                        fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                                                   NativeFailureOperation::Settlement, P});
                        render(engine, "native trailing offset is not representable");
                        return;
                    }
                    hit = native_matching::trail_stop_hit(
                        from_price, to_price, start, track->best, trail->offset, buy, grid);
                    kind = Kind::ActivateTrail;
                } else if (std::holds_alternative<native_order::LimitReady>(state)
                           || std::holds_alternative<native_order::StopLimitLive>(state)) {
                    double level = 0.0;
                    if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
                        level = limit->price;
                    } else if (const auto* sl = std::get_if<native_order::StopLimit>(&trigger)) {
                        level = sl->limit;
                    } else {
                        continue;
                    }
                    hit = native_matching::first_region_entry(
                        from_price, to_price, start, level, buy, include_current, grid);
                    kind = Kind::Fill;
                } else if (std::holds_alternative<native_order::MarketReady>(state)
                           || std::holds_alternative<native_order::StopActive>(state)
                           || std::holds_alternative<native_order::TrailActive>(state)) {
                    if (!facts.ready_to_match) {
                        erase_provenance_for(handle);
                        continue;
                    }
                    if (std::holds_alternative<native_order::MarketReady>(state) && continuous) {
                        continue;
                    }
                    hit = start;
                    kind = Kind::Fill;
                } else {
                    continue;
                }
                if (!hit) continue;
                if (kind == Kind::Fill) {
                    if (const auto* units = std::get_if<native_order::AllowanceUnits>(&live->allowance)) {
                        if (units->point_ordinal == P && units->left == 0.0) {
                            erase_provenance_for(handle);
                            continue;
                        }
                    }
                    if (std::holds_alternative<native_order::RemainingUnbound>(live->remaining)) {
                        erase_provenance_for(handle);
                        continue;
                    }
                }
                row.t = hit->t;
                row.price = hit->price;
                row.kind = kind;
                row.trigger_level = level_for(*live, kind, buy);
                row.at_level = hit->at_level;
            }
            if (!std::isfinite(row.price) || row.t < t_cursor || row.t > 1.0) {
                erase_provenance_for(handle);
                continue;
            }
            if (skipped.count(skip_key(row.incarnation, row.kind))) {
                erase_provenance_for(handle);
                continue;
            }
            if (row.kind != Kind::Evaluate) {
                const bool buy = std::holds_alternative<native_order::CohortClose>(live->authority)
                    ? requests_.working_is_buy(*live, candidate_eval.cohort_side)
                    : request_is_buy(engine, *live);
                if (!row.trigger_level) row.trigger_level = level_for(*live, row.kind, buy);
                if (!row.at_level) {
                    if (const auto* retained = retained_origin(
                            *live, row.kind, buy, row.trigger_level, row.t)) {
                        row.at_level = retained->at_level;
                        row.trigger_level = retained->trigger_level;
                    }
                }
                CandidateProvenance provenance;
                provenance.handle = handle;
                provenance.point_ordinal = P;
                provenance.kind = row.kind;
                provenance.trigger_state_index = live->trigger_state.index();
                provenance.is_buy = buy;
                provenance.trigger_level = row.trigger_level;
                provenance.t = row.t;
                provenance.raw_price = row.price;
                provenance.at_level = row.at_level;
                candidate_provenance.push_back(std::move(provenance));
                if (row.at_level && row.trigger_level) {
                    row.shared_cursor_collision = native_matching::double_bits(row.price)
                        != native_matching::double_bits(*row.trigger_level);
                }
            }
            if (!winner
                || row.t < winner->t
                || (row.t == winner->t && row.incarnation < winner->incarnation)
                || (row.t == winner->t && row.incarnation == winner->incarnation
                    && static_cast<uint8_t>(row.kind) < static_cast<uint8_t>(winner->kind))) {
                winner = row;
            }
        }
        if (!winner) break;
        if (continuous && winner->t > t_cursor) {
            apply_excursion(engine, winner->price);
            path_cursor = make_cursor(point, winner->t);
            observe_trails(engine, point, path_cursor, continuous, winner->price);
        }
        t_cursor = winner->t;
        cursor_price = winner->price;
        path_cursor = make_cursor(point, t_cursor);
        eval.cursor = path_cursor;
        const auto* live = requests_.find_live(winner->handle);
        if (!live) continue;
        eval.pre_open_birth_eligible = pre_open_birth_eligible(winner->handle, point)
            || born_on_remaining_path(*live);
        const auto* winner_target = cached_cohort_target(engine, *live);
        eval.cohort_side = winner_target ? side_from_target(*winner_target)
                                         : cohort_side(engine, *live);
        if (std::holds_alternative<native_order::CohortClose>(live->authority)
            && !eval.cohort_side) {
            skipped.insert(skip_key(winner->incarnation, winner->kind));
            continue;
        }
        if (winner->kind == Kind::Evaluate) {
            try {
                if (winner_target) {
                    if (requests_.refresh_allowance(winner->handle, eval, *winner_target)) {
                        continue;
                    }
                } else if (std::holds_alternative<native_order::BookClose>(live->authority)) {
                    native_order::TargetObservation obs;
                    obs.current_position = read_position(engine);
                    if (requests_.refresh_allowance(winner->handle, eval, obs)) {
                        continue;
                    }
                } else {
                    const auto obs = read_target(engine, live);
                    if (requests_.refresh_allowance(winner->handle, eval, obs)) {
                        continue;
                    }
                }
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return;
            }
            native_order::Preparation<native_order::PreparedMutation> prep;
            try {
                if (winner_target) {
                    prep = requests_.prepare_evaluation(
                        winner->handle, eval, *winner_target, next_timeline_ordinal_);
                } else {
                    prep = requests_.prepare_evaluation(
                        winner->handle, eval, read_target(engine, live), next_timeline_ordinal_);
                }
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return;
            }
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) {
                skipped.insert(skip_key(winner->incarnation, winner->kind));
                continue;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement, P)) {
                return;
            }
            if (requests_.find_live(winner->handle) == nullptr && !requests_.history().empty()) {
                try {
                    drain_parent_terminal(
                            engine,
                            native_order::EventId{winner->handle.run,
                                                  command_ordinal(requests_.history().back())},
                            winner->handle, NativeFailureOperation::Settlement);
                } catch (const std::exception& e) {
                    fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                               NativeFailureOperation::Settlement, P});
                    render(engine, e.what());
                    return;
                }
            }
            continue;
        }
        if (winner->kind != Kind::Fill) {
            native_order::TriggerTransition transition;
            if (winner->kind == Kind::ActivateStop) {
                transition = native_order::ActivateStop{path_cursor, winner->price};
            } else if (winner->kind == Kind::ActivateStopLimit) {
                transition = native_order::ActivateStopLimit{path_cursor, winner->price};
            } else if (winner->kind == Kind::BeginTrail) {
                transition = native_order::BeginTrailTracking{path_cursor, winner->price};
            } else {
                transition = native_order::ActivateTrail{path_cursor, winner->price};
            }
            native_order::Preparation<native_order::PreparedMutation> prep;
            try {
                prep = requests_.prepare_trigger(winner->handle, transition,
                                                driver_class, next_timeline_ordinal_, eval.cohort_side);
            } catch (const std::exception& e) {
                fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                           NativeFailureOperation::Settlement, P});
                render(engine, e.what());
                return;
            }
            if (const auto* err = std::get_if<native_order::PreparationError>(&prep)) {
                fail_preparation(engine, *err, NativeFailureOperation::Settlement);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(prep)) {
                skipped.insert(skip_key(winner->incarnation, winner->kind));
                continue;
            }
            auto* mutation = std::get_if<native_order::PreparedMutation>(&prep);
            if (!mutation || !install_mutation(engine, std::move(*mutation),
                                               NativeFailureOperation::Settlement, P)) {
                return;
            }
            if (winner->kind == Kind::ActivateStop || winner->kind == Kind::ActivateTrail) {
                const auto* activated = requests_.find_live(winner->handle);
                if (activated) {
                    const bool buy = request_is_buy(engine, *activated);
                    CandidateProvenance transfer;
                    transfer.handle = winner->handle;
                    transfer.point_ordinal = P;
                    transfer.kind = Kind::Fill;
                    transfer.trigger_state_index = activated->trigger_state.index();
                    transfer.is_buy = buy;
                    transfer.trigger_level = level_for(*activated, Kind::Fill, buy);
                    transfer.t = winner->t;
                    transfer.raw_price = winner->price;
                    transfer.at_level = winner->at_level;
                    candidate_provenance.push_back(std::move(transfer));
                }
            }
            continue;
        }

        live = requests_.find_live(winner->handle);
        if (!live) continue;
        const bool buy = request_is_buy(engine, *live);
        const bool limit_governed =
            std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state);
        const double slip = static_cast<double>(spec->slippage_ticks) * spec->price_tick;
        // An opted-in grid books on the tick ladder BEFORE slippage, which is
        // itself a whole number of ticks.
        const double basis = grid_fill_basis(*spec, winner->price, buy, limit_governed);
        double resolved = native_matching::apply_slippage(basis, slip, buy);
        const auto& trigger = live->request().trigger;
        // Limit protection applies only after finite slippage arithmetic.
        // min/max must not turn an overflowed price into an executable limit.
        if (std::isfinite(resolved) && limit_governed) {
            double level = 0.0;
            bool fill_through = false;
            if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
                level = limit->price;
                fill_through = limit->fill_through;
            } else if (const auto* sl = std::get_if<native_order::StopLimit>(&trigger)) {
                level = sl->limit;
            }
            if (!fill_through) {
                resolved = native_matching::protect_limit(
                    resolved, grid_limit_cap(*spec, level, buy), buy);
            }
        }
        native_order::NativeCandidatePriceKind price_kind =
            native_order::NativeCandidatePriceKind::PointPrice;
        if (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state)
            || std::holds_alternative<native_order::StopActive>(live->trigger_state)
            || std::holds_alternative<native_order::TrailActive>(live->trigger_state)) {
            price_kind = winner->at_level
                ? native_order::NativeCandidatePriceKind::TriggerLevel
                : native_order::NativeCandidatePriceKind::PointPrice;
        } else if (!std::holds_alternative<native_order::MarketReady>(live->trigger_state)) {
            throw std::logic_error("native nonfillable state reached matched coordinator");
        }
        const auto anchor = execution_anchor(path_cursor, resolved);
        consuming_request_ = true;
        auto outcome = consume_matched_request(engine, winner->handle, eval, winner->price,
            resolved, anchor, price_kind, NativeCurrentPriceRule::AsPresented,
            price_kind == native_order::NativeCandidatePriceKind::TriggerLevel
                && winner->shared_cursor_collision);
        consuming_request_ = false;
        if (failed()) return;
        if (!outcome) skipped.insert(skip_key(winner->incarnation, winner->kind));
        drain_applied_notifications(engine);
    }
    if (continuous && t_cursor < 1.0 && !failed()) {
        apply_excursion(engine, to_price);
        observe_trails(engine, point, make_cursor(point, 1.0), continuous, to_price);
    }
}

std::optional<NativeCurrentPointView> NativeExecutionConsumer::current_execution_point() const {
    if (!in_callback_ || !current_frame_ || !std::holds_alternative<NativeRunning>(state_))
        return std::nullopt;
    return current_frame_->point;
}

std::optional<NativeTrailState> NativeExecutionConsumer::trail_state(
        const BacktestEngine& engine, const native_order::RequestHandle& target) const {
    const auto* live = requests_.find_live(target);
    if (!live || !std::holds_alternative<native_order::Trail>(live->request().trigger)) {
        return std::nullopt;
    }

    NativeTrailState state;
    if (std::holds_alternative<native_order::TrailWaitArm>(live->trigger_state)) {
        return state;
    }

    if (const auto* tracking = std::get_if<native_order::TrailTrack>(&live->trigger_state)) {
        state.best_price = tracking->best;
    } else if (const auto* active = std::get_if<native_order::TrailActive>(
                   &live->trigger_state)) {
        state.best_price = active->best_at_trigger;
    } else {
        return std::nullopt;
    }
    state.activated = true;

    const auto& trail = std::get<native_order::Trail>(live->request().trigger);
    if (!native_matching::checked_trail_stop(
            state.best_price, trail.offset, request_is_buy(engine, *live),
            &state.current_level)) {
        return std::nullopt;
    }
    for (auto it = requests_.history().rbegin(); it != requests_.history().rend(); ++it) {
        const auto* activated = std::get_if<native_order::ActivatedEvent>(&*it);
        if (activated && activated->definition
            && activated->definition->handle == target
            && activated->kind == native_order::ActivationKind::TrailArm) {
            state.activation_ordinal = activated->ordinal;
            break;
        }
    }
    return state;
}

std::optional<NativeCurrentRefusal> NativeExecutionConsumer::validate_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const {
    using Refusal = NativeCurrentRefusal;
    if (!std::holds_alternative<NativeRunning>(state_)) return Refusal::NoExecutionContext;
    if (consuming_request_) return Refusal::Reentrant;
    if (!current_execution_point()) return Refusal::NoExecutionContext;
    if (callback_phase_ != CallbackPhase::PreOpen
        && callback_phase_ != CallbackPhase::Bar
        && callback_phase_ != CallbackPhase::Applied
        && callback_phase_ != CallbackPhase::Tick) {
        return Refusal::NoExecutionContext;
    }
    if (!projection_ok(engine)) return Refusal::ConfigurationMismatch;
    if (command.target.incarnation == 0 || command.target.run != requests_.identity())
        return Refusal::InvalidHandle;
    const auto* live = requests_.find_live(command.target);
    if (!live) return Refusal::NotWorking;
    if (live->birth().acceptance_ordinal <= current_frame_->acceptance_cutoff)
        return Refusal::NotAcceptedInCallback;
    if (command.price_rule != NativeCurrentPriceRule::AsPresented
        && command.price_rule != NativeCurrentPriceRule::NearestTick)
        return Refusal::UnsupportedRequest;
    const auto& request = live->request();
    if (!std::holds_alternative<native_order::Market>(request.trigger)
        || !std::holds_alternative<native_order::ImmediateRemaining>(request.capacity))
        return Refusal::UnsupportedRequest;
    if (std::holds_alternative<native_order::WaitForApplied>(request.owner)
        || std::holds_alternative<native_order::Wait>(live->authority)
        || std::holds_alternative<native_order::RemainingUnbound>(live->remaining))
        return Refusal::UnreadyOwner;
    if (const auto* reduce = std::get_if<native_order::Reduce>(&request.intent)) {
        if (!std::holds_alternative<native_order::ExplicitUnits>(reduce->size))
            return Refusal::UnsupportedRequest;
    } else if (std::holds_alternative<native_order::Transact>(request.intent)) {
        if (!std::holds_alternative<native_order::Independent>(request.owner))
            return Refusal::UnsupportedRequest;
    } else if (std::holds_alternative<native_order::ReverseTo>(request.intent)) {
        if (!std::holds_alternative<native_order::Independent>(request.owner))
            return Refusal::UnsupportedRequest;
    } else if (const auto* sized = std::get_if<native_order::HostSized>(&request.intent)) {
        if (sized->kind == native_order::HostSizedKind::Open
            && !std::holds_alternative<native_order::Independent>(request.owner)) {
            return Refusal::UnsupportedRequest;
        }
    } else if (!std::holds_alternative<native_order::Flatten>(request.intent)) {
        return Refusal::UnsupportedRequest;
    }
    if (!std::holds_alternative<native_order::Independent>(request.owner)
        && !std::holds_alternative<native_order::BindOpening>(request.owner)
        && !std::holds_alternative<native_order::BindOpenings>(request.owner)
        && !std::holds_alternative<native_order::BindCohort>(request.owner))
        return Refusal::UnsupportedRequest;
    const auto target = read_target(engine, live);
    if (std::holds_alternative<native_order::OpeningClose>(live->authority)) {
        if (!target.opening || !target.opening->has_live_matching_lot) return Refusal::UnreadyOwner;
    } else if (std::holds_alternative<native_order::OpeningsClose>(live->authority)) {
        if (target.openings.empty()) return Refusal::InvalidSelection;
        bool any_live = false;
        for (const auto& row : target.openings) any_live |= row.has_live_matching_lot;
        if (!any_live) return Refusal::UnreadyOwner;
    } else if (std::holds_alternative<native_order::CohortClose>(live->authority)) {
        bool any_live = false;
        for (const auto& row : target.openings) any_live |= row.has_live_matching_lot;
        if (!any_live) return Refusal::UnreadyOwner;
    }
    return std::nullopt;
}

NativeCoordinate NativeExecutionConsumer::current_execution_coordinate(uint64_t ordinal) const {
    auto coordinate = current_frame_->point.decision.coordinate;
    coordinate.ordinal = ordinal;
    coordinate.provenance = NativePriceProvenance::CurrentExecution;
    // A31(b): an applied notification keeps its exact decision coordinate
    // current. A newborn command in that callback must therefore both be born
    // and execute there, even if input delivery has already raised the future
    // decision floor. Other current-execution sites retain the floor rule.
    const bool applied_point_is_current = callback_phase_ == CallbackPhase::Applied
        && current_frame_
        && current_frame_->point.decision.coordinate.effective_time_ms >= decision_floor();
    if (!applied_point_is_current) {
        coordinate.effective_time_ms = std::max(coordinate.effective_time_ms, decision_floor());
    }
    return coordinate;
}

double NativeExecutionConsumer::current_price(const BacktestEngine& engine,
        const native_order::LiveRequest& live, NativeCurrentPriceRule rule) const {
    const double basis = rule == NativeCurrentPriceRule::NearestTick
        ? engine.bar_fill_price(current_frame_->point.price) : current_frame_->point.price;
    // An Independent close has not yet been bound during read-only preview.
    const bool buy = std::holds_alternative<native_order::UnboundBookClose>(live.authority)
        ? engine.position_side_ == PositionSide::SHORT : request_is_buy(engine, live);
    const auto* spec = spec_ptr();
    // Current execution is a market fill by contract, so the grid rounds it
    // on the adverse side before the same single slippage step.
    const double on_grid = grid_fill_basis(*spec, basis, buy, /*limit_governed=*/false);
    return native_matching::apply_slippage(on_grid,
        static_cast<double>(spec->slippage_ticks) * spec->price_tick, buy);
}

NativeCurrentExecutionPreview NativeExecutionConsumer::inspect_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const {
    NativeCurrentExecutionPreview out;
    out.refusal = validate_current_execution(engine, command);
    if (out.refusal) return out;
    const auto* live = requests_.find_live(command.target);
    native_order::MatchCursor cursor;
    cursor.point = current_execution_coordinate(next_timeline_ordinal_);
    native_order::EvaluationContext evaluation;
    evaluation.cursor = cursor;
    evaluation.driver_class = native_order::DriverEligibilityClass::CurrentExecution;
    evaluation.existing_matching_bit = true;
    evaluation.cohort_side = cohort_side(engine, *live);
    const auto* host_sized = host_sized_intent(*live);
    const bool unresolved = host_sized
        && (std::holds_alternative<native_order::RemainingDeferred>(live->remaining)
            || std::holds_alternative<native_order::NoTarget>(live->remaining));
    if (unresolved && host_sized->kind == native_order::HostSizedKind::Close
        && std::holds_alternative<native_order::UnboundBookClose>(live->authority)
        && engine.position_side_ == PositionSide::FLAT) {
        out.settlement_readiness = execution::Status::NoEffect;
        return out;
    }
    const double raw_price = current_frame_->point.price;
    const double default_resolved = current_price(engine, *live, command.price_rule);
    const auto facts = build_terms_facts(engine, *live, evaluation,
        native_order::NativeCandidatePriceKind::CurrentQuote, command.price_rule,
        raw_price, default_resolved);
    native_order::ExecutionTerms terms;
    struct PreviewSeal {
        bool& value;
        explicit PreviewSeal(bool& v) : value(v) { value = true; }
        ~PreviewSeal() { value = false; }
    };
    {
        PreviewSeal seal(consuming_request_);
        const auto* host = dynamic_cast<const NativeStrategyHost*>(&engine);
        if (!host) throw std::logic_error("native terms require a native host");
        terms = host->resolve_execution_terms(facts);
    }
    // A hook-owned public call may latch or perturb configuration. Preview
    // reports the refusal without adding another mutation of its own.
    if (failed() || engine.abort_requested_.load(std::memory_order_relaxed)
        || !projection_ok(engine)) {
        out.refusal = NativeCurrentRefusal::ConfigurationMismatch;
        return out;
    }
    if (!unresolved && (terms.units || terms.shape != native_order::OpeningShape::Transact)) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && !terms.units) {
        out.terms_rejection = native_order::MatchRejectReason::TermsUnresolved;
        return out;
    }
    if (unresolved && host_sized->kind == native_order::HostSizedKind::Close
        && terms.shape != native_order::OpeningShape::Transact) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && terms.shape != native_order::OpeningShape::Transact
        && terms.shape != native_order::OpeningShape::ReverseTo
        && terms.shape != native_order::OpeningShape::CloseOpposite) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (terms.units && (!std::isfinite(*terms.units) || *terms.units < 0.0)) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (!execution_terms_grid_representable(
            terms, host_sized, unresolved, facts.scope_exposure_units, spec_ptr())) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    double after = 0.0;
    double deduction = 0.0;
    bool exhausted = false;
    double binding_allowance = 0.0;
    if (unresolved) {
        if (!native_order::WorkingRequestCore::effective_host_units(
                live->pending, *terms.units, &deduction, &after, &exhausted)) {
            throw std::overflow_error("native host-sized deduction is not representable");
        }
        if (const auto* budget = std::get_if<native_order::PointBudget>(&live->request().capacity)) {
            binding_allowance = std::min(after, budget->units);
        } else {
            binding_allowance = after;
        }
    }
    const bool explicit_reversal = std::holds_alternative<native_order::ReverseTo>(
        live->request().intent);
    const bool requires_opposite = explicit_reversal || (unresolved
        && (terms.shape == native_order::OpeningShape::ReverseTo
            || terms.shape == native_order::OpeningShape::CloseOpposite));
    if (requires_opposite && facts.opposite_book_units == 0.0) {
        out.terms_rejection = native_order::MatchRejectReason::NoOppositeExposure;
        return out;
    }
    if (unresolved && terms.shape == native_order::OpeningShape::CloseOpposite
        && *terms.units > facts.opposite_book_units) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && terms.shape != native_order::OpeningShape::Transact
        && after > binding_allowance) {
        out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
        return out;
    }
    if (unresolved && *terms.units == 0.0) {
        out.settlement_readiness = execution::Status::NoEffect;
        return out;
    }
    if (unresolved && after == 0.0 && deduction > 0.0) {
        out.terms_cancellation = native_order::CancelReason::Group;
        return out;
    }
    std::optional<native_order::ExecutionPlan> plan;
    if (host_sized) {
        if (unresolved) {
            plan = plan_from_terms(host_sized->kind, host_sized->side, terms.shape,
                                   after, binding_allowance, facts.opposite_book_units);
        } else if (const auto* remaining = std::get_if<native_order::RemainingUnits>(
                       &live->remaining)) {
            plan = plan_from_terms(host_sized->kind, host_sized->side,
                native_order::OpeningShape::Transact, remaining->q,
                allowance_left_at(facts.allowance, cursor.point.ordinal),
                facts.opposite_book_units);
        }
    }
    const auto candidate = inspect_candidate(engine, *live, cursor, terms.resolved_price,
                                             plan ? &*plan : nullptr);
    execution::PhysicalExecutionContext context;
    context.effective_time_ms = cursor.point.effective_time_ms;
    context.interval_index = cursor.point.interval_index;
    if (const auto* reversal = std::get_if<execution::ReverseTo>(&candidate.physical)) {
        out.settlement_readiness = engine.preview_native_settlement_commit(
            *reversal, candidate.fill, context, out.account, out.closed_row_pnl);
    } else {
        out.settlement_readiness = engine.preview_native_settlement_commit(
            narrow_action(candidate.physical), candidate.fill, context, candidate.financial_scope,
            candidate.selected ? &*candidate.selected : nullptr, out.account, out.closed_row_pnl);
    }
    return out;
}

NativeCurrentExecutionResult NativeExecutionConsumer::execute_current(
        BacktestEngine& engine, const NativeCurrentExecution& command) {
    if (!std::holds_alternative<NativeRunning>(state_)) return NativeCurrentRefusal::NoExecutionContext;
    if (consuming_request_) return NativeCurrentRefusal::Reentrant;
    if (!current_execution_point()) return NativeCurrentRefusal::NoExecutionContext;
    // The in-callback guard must run before allocating a point or inspecting a
    // temporarily modified fee, FX, price tick, calendar or admission setting.
    if (!check_abort_or_projection(engine, NativeFailureOperation::Command))
        throw std::runtime_error("native current execution projection/abort failure");
    try {
        if (auto refusal = validate_current_execution(engine, command)) return *refusal;
        consuming_request_ = true;
        NativeDriverPoint point;
        const auto ordinal = take_ordinal(engine);
        if (failed()) throw std::runtime_error("native current point allocation failed");
        point.coordinate = current_execution_coordinate(ordinal);
        point.raw_price = current_frame_->point.price;
        point.matching = true;
        record_driver(point);
        native_order::EvaluationContext evaluation;
        evaluation.cursor = make_cursor(point, 0.0);
        evaluation.driver_class = native_order::DriverEligibilityClass::CurrentExecution;
        evaluation.existing_matching_bit = true;
        const auto* live = requests_.find_live(command.target);
        evaluation.cohort_side = live ? cohort_side(engine, *live) : std::nullopt;
        const auto history_before = requests_.history().size();
        auto prep = requests_.prepare_evaluation(command.target, evaluation,
            read_target(engine, live), next_timeline_ordinal_);
        if (const auto* error = std::get_if<native_order::PreparationError>(&prep)) {
            fail_preparation(engine, *error, NativeFailureOperation::Settlement);
            throw std::runtime_error("native current evaluation failed");
        }
        if (auto* mutation = std::get_if<native_order::PreparedMutation>(&prep)) {
            if (!install_mutation(engine, std::move(*mutation), NativeFailureOperation::Settlement,
                                  point.coordinate.ordinal))
                throw std::runtime_error("native current evaluation install failed");
        }
        live = requests_.find_live(command.target);
        if (live && !same_allowance_bits(
                native_order::WorkingRequestCore::evaluated_allowance(
                    *live, point.coordinate.ordinal), live->allowance)) {
            throw std::logic_error("native current evaluated allowance mismatch");
        }
        if (!live) {
            // Flat Independent closes terminate during the existing evaluation.
            if (requests_.history().size() <= history_before)
                throw std::logic_error("native current evaluation lost target without outcome");
            const auto* no_effect = std::get_if<native_order::NoEffectEvent>(&requests_.history().back());
            if (!no_effect) throw std::logic_error("native current evaluation has no terminal outcome");
            NativeCurrentExecutionResult outcome{*no_effect};
            const native_order::EventId cause{command.target.run, no_effect->ordinal};
            drain_parent_terminal(engine, cause, command.target, NativeFailureOperation::Settlement);
            if (failed()) throw std::runtime_error("native current terminal drain failed");
            consuming_request_ = false;
            return outcome;
        }
        const auto anchor = current_frame_->point;
        const double resolved = current_price(engine, *live, command.price_rule);
        auto outcome = consume_matched_request(engine, command.target, evaluation,
            anchor.price, resolved, anchor,
            native_order::NativeCandidatePriceKind::CurrentQuote, command.price_rule);
        if (failed() || !outcome) throw std::runtime_error("native current execution failed");
        consuming_request_ = false;
        return std::move(*outcome);
    } catch (const std::bad_alloc& e) {
        consuming_request_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Settlement});
        render(engine, e.what());
        throw;
    } catch (const std::exception& e) {
        consuming_request_ = false;
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                                                NativeFailureOperation::Settlement});
        render(engine, e.what());
        throw;
    } catch (...) {
        consuming_request_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Settlement});
        throw;
    }
}

void NativeExecutionConsumer::enqueue_applied_notification(AppliedNotification notification) {
    applied_notifications_.push_back(std::move(notification));
}

void NativeExecutionConsumer::finish_callback(BacktestEngine& engine, uint64_t ordinal) {
    in_callback_ = false;
    callback_phase_ = CallbackPhase::None;
    current_frame_.reset();
    if (check_abort_or_projection(engine, NativeFailureOperation::Callback, ordinal))
        drain_applied_notifications(engine);
}

void NativeExecutionConsumer::invoke_applied_callback(
        BacktestEngine& engine, const AppliedNotification& notification) {
    try {
        auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
        if (!host) throw std::logic_error("native notification requires native host");
        // Owning event and frame values survive submit/replace and further
        // synchronous executions reallocating the append-only history/queue.
        const auto applied = std::get<native_order::ExecutionAppliedEvent>(
            requests_.history().at(notification.history_index));
        if (applied.ordinal != notification.ordinal)
            throw std::logic_error("native notification identity mismatch");
        current_frame_ = CurrentExecutionFrame{notification.point, next_timeline_ordinal_ - 1};
        callback_context_ = notification.point.decision;
        callback_context_.decision_floor_ms = decision_floor();
        current_frame_->point.decision.decision_floor_ms = decision_floor();
        engine.current_bar_.timestamp = std::max(
            notification.point.decision.coordinate.effective_time_ms, decision_floor());
        in_callback_ = true;
        callback_phase_ = CallbackPhase::Applied;
        const auto presented = callback_context_;
        host->on_native_applied(applied, presented);
        finish_callback(engine, notification.ordinal);
    } catch (const std::bad_alloc& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Callback,
                                   notification.ordinal});
        render(engine, e.what());
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, notification.ordinal});
        render(engine, e.what());
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, notification.ordinal});
    }
}

void NativeExecutionConsumer::drain_applied_notifications(BacktestEngine& engine) {
    if (in_callback_ || consuming_request_ || draining_notifications_ || failed()) return;
    draining_notifications_ = true;
    while (notification_head_ < applied_notifications_.size() && !failed()) {
        const auto notification = applied_notifications_[notification_head_++];
        raise_floor(notification.point.decision.coordinate.effective_time_ms);
        invoke_applied_callback(engine, notification);
    }
    draining_notifications_ = false;
    if (!failed()) {
        applied_notifications_.clear();
        notification_head_ = 0;
    }
}

void NativeExecutionConsumer::invoke_bar_open_callback(
        BacktestEngine& engine, const Bar& bar, const NativeDriverPoint& point) {
    auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
    if (!host) return;
    callback_context_.coordinate = point.coordinate;
    callback_context_.decision_floor_ms = decision_floor();
    if (auto input = input_interval_at(point.coordinate.open_ms)) {
        callback_context_.input_interval = *input;
    }
    if (auto script = script_interval_at(point.coordinate.open_ms)) {
        callback_context_.script_interval = *script;
    }
    if (callback_context_.sub_count <= 0) callback_context_.sub_count = 1;
    if (callback_context_.sub_index < 0) callback_context_.sub_index = 0;
    if (callback_context_.sub_bar_open_ms == 0) {
        callback_context_.sub_bar_open_ms = point.coordinate.open_ms;
    }
    if (callback_context_.script_bar_open_ms == 0) {
        callback_context_.script_bar_open_ms = point.coordinate.open_ms;
    }
    NativeCurrentPointView current;
    current.decision = callback_context_;
    current.price = point.raw_price;
    current.quote_kind = NativeCurrentQuoteKind::MarketDecision;
    current.quote_origin_ordinal = point.coordinate.ordinal;
    current_frame_ = CurrentExecutionFrame{current, next_timeline_ordinal_ - 1};
    engine.current_bar_ = bar;
    engine.current_bar_.timestamp = point.coordinate.effective_time_ms;
    in_callback_ = true;
    callback_phase_ = CallbackPhase::PreOpen;
    try {
        const NativeDecisionContext presented = callback_context_;
        host->on_native_bar_open(bar, presented);
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       point.coordinate.ordinal});
            render(engine, e.what());
        }
        return;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       point.coordinate.ordinal});
            render(engine, "native bar-open callback exception");
        }
        return;
    }
    finish_callback(engine, point.coordinate.ordinal);
}

bool NativeExecutionConsumer::invoke_input_callback(
        BacktestEngine& engine, const Bar& bar, const NativeInputContext& context) {
    auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
    if (!host) return true;
    input_callback_context_ = context;
    input_callback_bar_ = bar;
    in_callback_ = true;
    try {
        host->on_native_input(bar, context);
    } catch (const std::exception& e) {
        in_callback_ = false;
        input_callback_context_.reset();
        input_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input});
            render(engine, e.what());
        }
        return false;
    } catch (...) {
        in_callback_ = false;
        input_callback_context_.reset();
        input_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input});
            render(engine, "native input callback exception");
        }
        return false;
    }
    in_callback_ = false;
    input_callback_context_.reset();
    input_callback_bar_.reset();
    return !failed();
}

bool NativeExecutionConsumer::invoke_tick_callback(
        BacktestEngine& engine, const Bar& bar, const NativeTickContext& context) {
    auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
    if (!host) return true;
    NativeTickContext presented = context;
    presented.decision.decision_floor_ms = decision_floor();
    tick_callback_context_ = presented;
    tick_callback_bar_ = bar;
    callback_context_ = presented.decision;
    NativeCurrentPointView current;
    current.decision = callback_context_;
    current.price = bar.close;
    current.quote_kind = NativeCurrentQuoteKind::MarketDecision;
    current.quote_origin_ordinal = callback_context_.coordinate.ordinal;
    current_frame_ = CurrentExecutionFrame{current, next_timeline_ordinal_ - 1};
    in_callback_ = true;
    callback_phase_ = CallbackPhase::Tick;
    try {
        host->on_native_tick(bar, presented);
    } catch (const std::bad_alloc& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        tick_callback_context_.reset();
        tick_callback_bar_.reset();
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Input,
                                   context.decision.coordinate.ordinal});
        render(engine, e.what());
        return false;
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        tick_callback_context_.reset();
        tick_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input,
                                       context.decision.coordinate.ordinal});
            render(engine, e.what());
        }
        return false;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        tick_callback_context_.reset();
        tick_callback_bar_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Input,
                                       context.decision.coordinate.ordinal});
            render(engine, "native tick callback exception");
        }
        return false;
    }
    finish_callback(engine, context.decision.coordinate.ordinal);
    tick_callback_context_.reset();
    tick_callback_bar_.reset();
    return !failed();
}

void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
                                              const NativeCoordinate& coordinate) {
    auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
    if (!host) return;
    callback_context_.coordinate = coordinate;
    callback_context_.decision_floor_ms = decision_floor_ms_;
    auto input = input_interval_at(bar.timestamp);
    auto script = script_interval_at(bar.timestamp);
    if (input) callback_context_.input_interval = *input;
    if (script) callback_context_.script_interval = *script;
    if (callback_context_.sub_count <= 0) callback_context_.sub_count = 1;
    if (callback_context_.sub_index < 0) callback_context_.sub_index = 0;
    if (callback_context_.sub_bar_open_ms == 0) callback_context_.sub_bar_open_ms = coordinate.open_ms;
    if (callback_context_.script_bar_open_ms == 0) callback_context_.script_bar_open_ms = coordinate.open_ms;
    NativeCurrentPointView point;
    point.decision = callback_context_;
    point.price = bar.close;
    current_frame_ = CurrentExecutionFrame{point, next_timeline_ordinal_ - 1};
    in_callback_ = true;
    callback_phase_ = CallbackPhase::Bar;
    try {
        const NativeDecisionContext presented = callback_context_;
        host->on_native_bar(bar, presented);
    } catch (const std::exception& e) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render(engine, e.what());
        }
        return;
    } catch (...) {
        in_callback_ = false;
        callback_phase_ = CallbackPhase::None;
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render(engine, "native callback exception");
        }
        return;
    }
    ++engine.diag_script_bars_processed_;
    finish_callback(engine, coordinate.ordinal);
}

void NativeExecutionConsumer::deliver_confirmed_script(BacktestEngine& engine, const Bar& bar,
                                                       const NativeCoordinate& base) {
    const auto* spec = spec_ptr();
    const bool high_first = path_uses_high_first(
        bar, spec ? spec->path_order : NativePathOrder::Auto);
    callback_context_ = NativeDecisionContext{};
    callback_context_.sub_index = 0;
    callback_context_.sub_count = 1;
    callback_context_.is_terminal_sub_bar = true;
    callback_context_.sub_bar_open_ms = base.open_ms;
    callback_context_.script_bar_open_ms = base.open_ms;
    driver_statistics_.sub_bars_per_script_bar = 1;
    driver_statistics_.samples_per_sub_bar = 0;
    callback_context_.driver_statistics = driver_statistics_;
    auto emit_discrete = [&](double price, int64_t time, NativePriceProvenance provenance,
                             NativePathPhase phase, bool matching) {
        NativeDriverPoint point;
        point.coordinate = base;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = time;
        point.coordinate.source_price_time_ms = time;
        point.coordinate.provenance = provenance;
        point.coordinate.path_phase = phase;
        point.raw_price = price;
        point.matching = matching;
        record_driver(point);
        if (phase == NativePathPhase::Open) {
            invoke_bar_open_callback(engine, bar, point);
            if (failed()) return;
        }
        match_discrete(engine, point);
        raise_floor(time);
    };
    auto emit_segment = [&](double from, double to, int64_t time, NativePathPhase phase) {
        NativeDriverPoint point;
        point.coordinate = base;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = time;
        point.coordinate.source_price_time_ms = time;
        point.coordinate.provenance = NativePriceProvenance::Confirmed;
        point.coordinate.path_phase = phase;
        point.raw_price = to;
        point.matching = false;
        point.excursion = true;
        record_driver(point);
        match_segment(engine, point, from);
        raise_floor(time);
    };
    const int64_t open_time = script_.first_source_time_ms != 0
        ? script_.first_source_time_ms
        : (script_.first_open_ms != 0 ? script_.first_open_ms : bar.timestamp);
    const int64_t close_time = calculation_time(base);
    emit_discrete(bar.open, open_time, NativePriceProvenance::ModeledOHLCOpen,
                  NativePathPhase::Open, true);
    if (failed()) return;
    double prev = bar.open;
    if (high_first) {
        emit_segment(prev, bar.high, open_time, NativePathPhase::High);
        if (failed()) return;
        prev = bar.high;
        emit_segment(prev, bar.low, open_time, NativePathPhase::Low);
        prev = bar.low;
    } else {
        emit_segment(prev, bar.low, open_time, NativePathPhase::Low);
        if (failed()) return;
        prev = bar.low;
        emit_segment(prev, bar.high, open_time, NativePathPhase::High);
        prev = bar.high;
    }
    if (failed()) return;
    emit_segment(prev, bar.close, close_time, NativePathPhase::Close);
    if (failed()) return;
    NativeCoordinate calc = base;
    calc.ordinal = take_ordinal(engine);
    calc.effective_time_ms = close_time;
    calc.provenance = NativePriceProvenance::Calculation;
    calc.path_phase = NativePathPhase::None;
    raise_floor(close_time);
    engine.current_bar_ = bar;
    engine.bar_index_ = calc.interval_index;
    engine.current_bar_.timestamp = close_time;
    invoke_callback(engine, bar, calc);
    if (failed()) return;
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        emit_discrete(bar.close, close_time, NativePriceProvenance::AfterCalculationClose,
                      NativePathPhase::Close, true);
        if (failed()) return;
    }
    record_script_report_point(engine, base.open_ms);
}

void NativeExecutionConsumer::deliver_intrabar_script(
        BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base) {
    const auto* spec = spec_ptr();
    const auto* lower = spec ? spec->intrabar.lower() : nullptr;
    const auto* synthesized = spec ? spec->intrabar.synthesized_path() : nullptr;
    if (!lower && !synthesized) {
        deliver_confirmed_script(engine, bar, base);
        return;
    }

    std::vector<const Bar*> sub_bars;
    if (lower) {
        const int64_t begin = base.open_ms;
        const int64_t end = script_.interval.next_input_open_ms;
        for (const auto& candidate : lower->bars) {
            if (candidate.timestamp >= begin && candidate.timestamp < end) {
                sub_bars.push_back(&candidate);
            }
        }
        // The generic lower-feed path follows the legacy pump's fallback: a
        // script bar with no assigned lower bars walks its own OHLC path.
        if (sub_bars.empty()) {
            deliver_confirmed_script(engine, bar, base);
            return;
        }
    } else {
        // synthesized is intentionally independent of [begin, end): raw
        // caller labels have a zero-width partition but still carry their
        // own complete OHLC path.
        sub_bars.push_back(&bar);
    }

    Bar script_bar = bar;
    script_bar.timestamp = base.open_ms;
    const int sample_count = lower ? lower->samples : synthesized->samples;
    const auto distribution = lower ? lower->distribution : synthesized->distribution;
    const bool volume_weighted = lower ? lower->volume_weighted : synthesized->volume_weighted;
    const int volume_weighted_min_samples = lower
        ? lower->volume_weighted_min_samples : synthesized->volume_weighted_min_samples;
    const int volume_weighted_max_samples = lower
        ? lower->volume_weighted_max_samples : synthesized->volume_weighted_max_samples;
    double mean_volume = 0.0;
    if (volume_weighted) {
        for (const Bar* sub : sub_bars) mean_volume += sub->volume;
        mean_volume /= static_cast<double>(sub_bars.size());
    }
    std::vector<double> samples;
    callback_context_ = NativeDecisionContext{};
    callback_context_.sub_count = static_cast<int>(sub_bars.size());
    callback_context_.script_bar_open_ms = base.open_ms;
    driver_statistics_.intrabar_path_enabled = true;
    driver_statistics_.sub_bars_per_script_bar = static_cast<int>(sub_bars.size());
    driver_statistics_.samples_per_sub_bar = 0;
    callback_context_.driver_statistics = driver_statistics_;
    const bool direct_sub_bar_corners = lower && sub_bars.size() > 1;
    const bool distribution_samples = synthesized || lower->sample_eligibility
        == IntrabarPath::SampleEligibility::DistributionSamples;

    for (std::size_t sub_index = 0; sub_index < sub_bars.size(); ++sub_index) {
        const Bar& sub = *sub_bars[sub_index];
        callback_context_.sub_index = static_cast<int>(sub_index);
        callback_context_.is_terminal_sub_bar = sub_index + 1 == sub_bars.size();
        callback_context_.sub_bar_open_ms = sub.timestamp;
        // DistributionSamples and synthesized paths consume the generic
        // sampler from include/pineforge/magnifier.hpp as ordered point
        // decisions. This reproduces the read-only consumption ordering at
        // src/source/pine_scheduler.cpp:806-960 without source policy here.
        {
            // The sampler still serves the byte-identical legacy route through
            // its scoped internal order. Install this run's generic policy only
            // while materializing the native driver's point sequence.
            NativePathOrderScope path_scope(spec->path_order);
            if (!distribution_samples || direct_sub_bar_corners) {
                // A retained lower bar already supplies its four exact turning
                // points. Continuous eligibility traverses those segments directly;
                // likewise, a path containing several retained lower bars has no
                // missing intrabar detail for a synthetic sampler to recover.
                sample_price_path(sub, 4, MagnifierDistribution::ENDPOINTS, samples);
            } else if (volume_weighted) {
                sample_price_path_volume_weighted(
                    sub, sample_count, mean_volume, volume_weighted_min_samples,
                    volume_weighted_max_samples, distribution, samples);
            } else {
                sample_price_path(sub, sample_count, distribution, samples);
            }
        }
        if (samples.empty()) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Input});
            render(engine, "native intrabar path produced no samples");
            return;
        }
        ++driver_statistics_.sub_bars_processed;
        driver_statistics_.samples_per_sub_bar = static_cast<int>(samples.size());
        callback_context_.driver_statistics = driver_statistics_;
        double previous = samples.front();
        for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
            const double price = samples[sample_index];
            ++driver_statistics_.sample_ticks_processed;
            callback_context_.driver_statistics = driver_statistics_;
            NativeDriverPoint point;
            point.coordinate = base;
            point.coordinate.ordinal = take_ordinal(engine);
            point.coordinate.effective_time_ms = sub.timestamp;
            point.coordinate.source_price_time_ms = sub.timestamp;
            if (distribution_samples) {
                // A sampled value is a one-price decision bar, not an
                // interpolated leg of its containing OHLC bar. Giving every
                // one its open coordinate makes O=H=L=C=price: a reached
                // resting level fills at the level, while a gap through the
                // level keeps this sample's quote. Its driver ordinal still
                // orders it strictly after the preceding sample.
                point.coordinate.path_phase = NativePathPhase::Open;
                point.coordinate.provenance = NativePriceProvenance::ModeledOHLCOpen;
            } else {
                point.coordinate.path_phase = sample_index == 0 ? NativePathPhase::Open
                    : (sample_index + 1 == samples.size() ? NativePathPhase::Close
                       : (price == sub.high ? NativePathPhase::High
                          : (price == sub.low ? NativePathPhase::Low : NativePathPhase::None)));
                point.coordinate.provenance = sample_index == 0
                    ? NativePriceProvenance::ModeledOHLCOpen
                    : (sample_index + 1 == samples.size()
                        ? NativePriceProvenance::ModeledOHLCClose
                        : NativePriceProvenance::Confirmed);
            }
            point.raw_price = price;
            point.matching = distribution_samples || sample_index == 0;
            point.excursion = sample_index != 0;
            record_driver(point);
            if (sub_index == 0 && sample_index == 0) {
                invoke_bar_open_callback(engine, script_bar, point);
                if (failed()) return;
            }
            if (distribution_samples || sample_index == 0) {
                match_discrete(engine, point);
                if (!failed()) apply_excursion(engine, price);
            } else {
                match_segment(engine, point, previous);
            }
            if (failed()) return;
            raise_floor(sub.timestamp);
            previous = price;
        }
    }

    NativeCoordinate calculation = base;
    calculation.ordinal = take_ordinal(engine);
    calculation.effective_time_ms = calculation_time(base);
    calculation.source_price_time_ms = sub_bars.back()->timestamp;
    calculation.provenance = NativePriceProvenance::Calculation;
    calculation.path_phase = NativePathPhase::None;
    callback_context_.sub_index = static_cast<int>(sub_bars.size() - 1);
    callback_context_.is_terminal_sub_bar = true;
    callback_context_.sub_bar_open_ms = sub_bars.back()->timestamp;
    callback_context_.script_bar_open_ms = base.open_ms;
    raise_floor(calculation.effective_time_ms);
    engine.current_bar_ = script_bar;
    engine.bar_index_ = calculation.interval_index;
    engine.current_bar_.timestamp = calculation.effective_time_ms;
    invoke_callback(engine, script_bar, calculation);
    if (failed()) return;
    if (spec->close_execution == NativeCloseExecution::AfterCalculation) {
        NativeDriverPoint point;
        point.coordinate = calculation;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
        point.coordinate.path_phase = NativePathPhase::Close;
        point.raw_price = script_bar.close;
        point.matching = true;
        record_driver(point);
        match_point(engine, point);
        if (failed()) return;
    }
    record_script_report_point(engine, base.open_ms);
}

int64_t NativeExecutionConsumer::calculation_time(const NativeCoordinate& base) const noexcept {
    int64_t t = base.last_traded_close_ms;
    if (base.next_period_open_ms > t) t = base.next_period_open_ms;
    if (script_.latest_close_ms > t) t = script_.latest_close_ms;
    return t;
}

// One report point per script calculation, in the order the source scheduler
// uses (pine_strategy_host.cpp): fold the extremes at this calculation's
// close, then append the script bar's own point. The label is the script
// interval's open, not current_bar_.timestamp, which the intrabar walk
// overwrites — that keeps the curve identical with and without a path.
void NativeExecutionConsumer::record_script_report_point(
        BacktestEngine& engine, int64_t script_open_ms) const {
    const auto* spec = spec_ptr();
    if (!spec || spec->report_policy != NativeReportPolicy::KernelRecorded) return;
    engine.update_equity_extremes();
    engine.record_equity_point(script_open_ms);
}

// A position still open when the feed ends is reported as the rows a close at
// the last bar's close would record — one per physical lot, through the same
// non-mutating row builder every full close uses. Reporting only: the live
// book, the realized sums, the equity curve and every hash are left exactly as
// the run left them, so enabling this cannot move a fill or a continuation.
// The mark is the raw close on the price grid (bar_fill_price) with no
// slippage: slippage models a market order's fill uncertainty, and this row is
// a mark, not an order. The exit is dated on the script bar's own label.
void NativeExecutionConsumer::record_open_position_report_rows(BacktestEngine& engine) const {
    const auto* spec = spec_ptr();
    if (!spec || spec->report_policy != NativeReportPolicy::KernelRecorded
        || !spec->report_open_position_at_end) return;
    engine.range_end_trades_.clear();
    if (engine.position_side_ == PositionSide::FLAT || engine.pyramid_entries_.empty()) return;
    if (!std::isfinite(engine.current_bar_.close)) return;
    const bool was_long = engine.position_side_ == PositionSide::LONG;
    const double fill_price = engine.bar_fill_price(engine.current_bar_.close);
    execution::PhysicalExecutionContext context;
    context.effective_time_ms = engine.equity_curve_.empty()
        ? engine.current_bar_.timestamp
        : engine.equity_curve_.back().time_ms;
    context.interval_index = engine.bar_index_;
    for (const auto& lot : engine.pyramid_entries_) {
        Trade row = engine.build_close_trade_with_costs(
            lot, lot.qty, fill_price, was_long,
            engine.allocated_entry_commission(lot, lot.qty),
            engine.calc_commission(fill_price, lot.qty), context);
        row.open_at_end = true;
        engine.range_end_trades_.push_back(std::move(row));
    }
}

void NativeExecutionConsumer::deliver_aggregate_calculation(
        BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base) {
    callback_context_ = NativeDecisionContext{};
    callback_context_.sub_index = 0;
    callback_context_.sub_count = 1;
    callback_context_.is_terminal_sub_bar = true;
    callback_context_.sub_bar_open_ms = base.open_ms;
    callback_context_.script_bar_open_ms = base.open_ms;
    const int64_t close_time = calculation_time(base);
    NativeCoordinate calc = base;
    calc.ordinal = take_ordinal(engine);
    calc.effective_time_ms = close_time;
    calc.source_price_time_ms = script_.latest_close_ms;
    calc.provenance = NativePriceProvenance::Calculation;
    calc.path_phase = NativePathPhase::None;
    raise_floor(close_time);
    engine.current_bar_ = bar;
    engine.bar_index_ = calc.interval_index;
    engine.current_bar_.timestamp = close_time;
    invoke_callback(engine, bar, calc);
    if (failed()) return;
    const auto* spec = spec_ptr();
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        NativeDriverPoint point;
        point.coordinate = calc;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = close_time;
        point.coordinate.source_price_time_ms = script_.latest_close_ms;
        point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
        point.coordinate.path_phase = NativePathPhase::Close;
        point.raw_price = bar.close;
        point.matching = true;
        record_driver(point);
        match_point(engine, point);
        if (failed()) return;
    }
    record_script_report_point(engine, base.open_ms);
}

void NativeExecutionConsumer::seal_script(BacktestEngine& engine, NativeCompletionKind kind) {
    if (!script_.has_data || script_.sealed) return;
    NativeCoordinate base;
    base.interval_index = script_.first_index;
    base.open_ms = script_.interval.open_ms;
    base.eligible_open_ms = script_.interval.eligible_open_ms;
    base.last_traded_close_ms = script_.interval.last_traded_close_ms;
    base.next_period_open_ms = script_.interval.next_period_open_ms;
    base.next_input_open_ms = script_.interval.next_input_open_ms;
    base.completion = kind;
    if (script_.modeled_ohlc) {
        if (const auto* spec = spec_ptr(); spec && !spec->intrabar.is_none()) {
            deliver_intrabar_script(engine, script_.agg, base);
        } else {
            deliver_confirmed_script(engine, script_.agg, base);
        }
    } else {
        deliver_aggregate_calculation(engine, script_.agg, base);
    }
    script_.sealed = true;
    script_.has_data = false;
}

bool NativeExecutionConsumer::contribute_input(
        BacktestEngine& engine, const Bar& bar,
        const native_calendar::NativeInterval& interval,
        int index, InputContribution kind) {
    auto script_interval = script_interval_at(interval.open_ms);
    if (!script_interval) {
        render(engine, "native script interval lookup failed");
        return false;
    }
    const int64_t script_key = script_interval->open_ms;
    if (script_.has_data && !script_.sealed && script_.key != script_key) {
        seal_script(engine, NativeCompletionKind::LazyComplete);
        if (failed()) return false;
        script_ = ScriptBucket{};
    }
    const int64_t source_close = (kind == InputContribution::ConfirmedBar)
        ? std::max(interval.last_traded_close_ms, interval.next_period_open_ms)
        : (last_print_time_ms_ != 0 ? last_print_time_ms_ : interval.last_traded_close_ms);
    const int64_t source_open = (kind == InputContribution::ConfirmedBar)
        ? bar.timestamp
        : (kind == InputContribution::ObservedTickSlot
            ? (last_print_time_ms_ != 0 ? last_print_time_ms_ : interval.eligible_open_ms)
            : interval.eligible_open_ms);
    if (!script_.has_data) {
        script_.key = script_key;
        script_.interval = *script_interval;
        script_.agg = bar;
        script_.has_data = true;
        script_.first_open_ms = interval.open_ms;
        script_.first_source_time_ms = source_open;
        script_.latest_close_ms = source_close;
        script_.first_index = index;
        script_.last_index = index;
        script_.sealed = false;
        script_.modeled_ohlc = (kind == InputContribution::ConfirmedBar);
    } else {
        script_.agg.high = std::max(script_.agg.high, bar.high);
        script_.agg.low = std::min(script_.agg.low, bar.low);
        script_.agg.close = bar.close;
        script_.agg.volume += bar.volume;
        script_.latest_close_ms = std::max(script_.latest_close_ms, source_close);
        script_.last_index = index;
        if (kind != InputContribution::ConfirmedBar) script_.modeled_ohlc = false;
    }
    current_input_open_ = interval.open_ms;
    observed_input_cursor_ = interval.open_ms;
    last_accepted_input_ = interval;
    const auto* spec = spec_ptr();
    // A modeled intrabar path supplies its own decision points during seal.
    // Raising to the input/script completion here would fence requests born
    // at an earlier sub-bar out of its remaining points. No-path and stream
    // aggregation retain the historical script-bar floor exactly.
    const bool intrabar_points_drive_floor = kind == InputContribution::ConfirmedBar
        && spec && !spec->intrabar.is_none();
    if (!intrabar_points_drive_floor) {
        raise_floor(native_canonical_input_completion(interval));
    }
    const bool exhausted =
        interval.next_period_open_ms >= script_.interval.next_period_open_ms;
    if (exhausted) {
        seal_script(engine, NativeCompletionKind::Confirmed);
        if (failed()) return false;
        script_ = ScriptBucket{};
    }
    engine.current_bar_ = bar;
    engine.bar_index_ = index;
    next_interval_index_ = index + 1;
    if (!failed() && kind != InputContribution::QuietCarried)
        ++engine.diag_input_bars_processed_;
    return !failed();
}

bool NativeExecutionConsumer::consume_confirmed_input(BacktestEngine& engine, const Bar& bar,
                                                      int index, bool last) {
    (void)last;
    processing_input_ = true;
    const auto* running = std::get_if<NativeRunning>(&state_);
    const bool tolerant_realtime = legacy_tolerant_slot_labels() && running
        && running->phase == NativeRunPhase::Realtime;
    if (tolerant_realtime && last_accepted_input_) {
        // LegacyTolerant preserves arbitrary historical labels, but the
        // realtime confirmed-bar API still advances on the caller's raw label
        // grid (ab9714be pine_stream.cpp:167-205).  Validate that boundary
        // before any driver, digest, aggregation, or callback mutation.
        std::int64_t unit_ms = 0;
        switch (input_tf_.unit()) {
        case native_calendar::TimeframeUnit::Second: unit_ms = 1000; break;
        case native_calendar::TimeframeUnit::Minute: unit_ms = 60'000; break;
        case native_calendar::TimeframeUnit::Day: unit_ms = 86'400'000; break;
        case native_calendar::TimeframeUnit::Week: unit_ms = 604'800'000; break;
        case native_calendar::TimeframeUnit::Month: break;
        }
        const auto count = static_cast<std::int64_t>(input_tf_.count());
        if (!(unit_ms > 0) || !(count > 0)
            || count > std::numeric_limits<std::int64_t>::max() / unit_ms) {
            processing_input_ = false;
            present_refusal(engine, "native confirmed bar timeframe is not a fixed grid");
            return false;
        }
        const std::int64_t step = count * unit_ms;
        const std::int64_t previous = last_accepted_input_->open_ms;
        if (previous > std::numeric_limits<std::int64_t>::max() - step
            || bar.timestamp > std::numeric_limits<std::int64_t>::max() - step) {
            processing_input_ = false;
            present_refusal(engine, "native confirmed bar timestamp overflows the input grid");
            return false;
        }
        const std::int64_t expected = previous + step;
        if (bar.timestamp < expected || (bar.timestamp - expected) % step != 0) {
            processing_input_ = false;
            present_refusal(engine,
                "native confirmed bar timestamp is out of order or off the input grid");
            return false;
        }
        for (std::int64_t missing = expected; missing < bar.timestamp;) {
            if (native_calendar::in_session(calendar_, missing)) {
                processing_input_ = false;
                present_refusal(engine, "native stream has an in-session gap");
                return false;
            }
            if (missing > std::numeric_limits<std::int64_t>::max() - step) {
                processing_input_ = false;
                present_refusal(engine, "native confirmed bar timestamp overflows the input grid");
                return false;
            }
            missing += step;
        }
    }
    auto interval = input_interval_at(bar.timestamp);
    if (!interval) {
        processing_input_ = false;
        present_refusal(engine, "native input is not aligned");
        return false;
    }
    const bool realtime_labels = running
        && running->phase == NativeRunPhase::Realtime;
    const bool canonical_labels = !legacy_tolerant_slot_labels()
        || realtime_labels;
    if (canonical_labels
        && !native_confirmed_bar_label_admitted(*interval, bar.timestamp)) {
        processing_input_ = false;
        present_refusal(engine, "native confirmed bar timestamp is not a canonical slot label");
        return false;
    }
    if (last_accepted_input_) {
        if (canonical_labels
            && interval->open_ms <= last_accepted_input_->open_ms) {
            processing_input_ = false;
            present_refusal(engine, "native duplicate overlapping input slot");
            return false;
        }
        if (canonical_labels && realtime_labels) {
            int64_t expected_label = last_accepted_input_->next_input_open_ms;
            if (expected_label <= last_accepted_input_->open_ms) {
                int64_t unit_ms = 0;
                switch (input_tf_.unit()) {
                case native_calendar::TimeframeUnit::Second: unit_ms = 1000; break;
                case native_calendar::TimeframeUnit::Minute: unit_ms = 60 * 1000; break;
                case native_calendar::TimeframeUnit::Day: unit_ms = 24 * 60 * 60 * 1000; break;
                case native_calendar::TimeframeUnit::Week:
                    unit_ms = 7 * 24 * 60 * 60 * 1000;
                    break;
                case native_calendar::TimeframeUnit::Month: break;
                }
                const int64_t count = input_tf_.count();
                if (unit_ms <= 0 || count <= 0
                    || unit_ms > std::numeric_limits<int64_t>::max() / count
                    || last_accepted_input_->open_ms
                        > std::numeric_limits<int64_t>::max() - unit_ms * count) {
                    processing_input_ = false;
                    present_refusal(engine, "native confirmed bar timestamp overflows");
                    return false;
                }
                expected_label = last_accepted_input_->open_ms + unit_ms * count;
            }
            if (bar.timestamp != expected_label) {
                processing_input_ = false;
                present_refusal(engine, "native stream has an in-session gap");
                return false;
            }
        }
    }
    const auto script_interval = script_interval_at(interval->open_ms);
    if (!script_interval) {
        processing_input_ = false;
        present_refusal(engine, "native script interval lookup failed");
        return false;
    }
    NativeInputContext input_context;
    input_context.input_interval = *interval;
    input_context.script_interval = *script_interval;
    input_context.input_index = index;
    input_context.completes_script_interval =
        interval->next_period_open_ms >= script_interval->next_period_open_ms;
    if (!invoke_input_callback(engine, bar, input_context)) {
        processing_input_ = false;
        return false;
    }
    last_accepted_input_ = *interval;
    last_observed_slot_open_ = interval->open_ms;
    last_finalized_input_ = *interval;
    last_price_ = bar.close;
    has_last_price_ = true;
    if (!contribute_input(engine, bar, *interval, index, InputContribution::ConfirmedBar)) {
        processing_input_ = false;
        return false;
    }
    processing_input_ = false;
    return !failed();
}

void NativeExecutionConsumer::pump_batch(BacktestEngine& engine, const Bar* bars, int n) {
    for (int i = 0; i < n; ++i) {
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return;
        if (!consume_confirmed_input(engine, bars[i], i, i + 1 == n)) {
            if (!failed()) {
                fail(engine, NativeFailure{NativeFailureCode::Preflight,
                                           NativeFailureOperation::Input});
            }
            return;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return;
    }
}

void NativeExecutionConsumer::run_simple(BacktestEngine& engine, const Bar* bars, int n) {
    NativeBeginArgs args{bars, n, {}, {}, false, 4,
        MagnifierDistribution::ENDPOINTS, engine.magnifier_volume_weighted_, 2};
    args.simple_run = true;
    if (!prepare_public_begin(engine, args)) return;
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!preflight_bars(engine, bars, n, false) || !preflight_intrabar_path(engine)) return;
        const int64_t initial = n > 0 ? bars[0].timestamp
                                      : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, bars, n);
        if (failed()) return;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::run_tf(BacktestEngine& engine,
                                     const Bar* input_bars, int n_input,
                                     const std::string& input_tf,
                                     const std::string& script_tf,
                                     bool bar_magnifier, int magnifier_samples,
                                     MagnifierDistribution magnifier_dist) {
    const NativeBeginArgs args{input_bars, n_input, input_tf, script_tf, bar_magnifier,
        magnifier_samples, magnifier_dist, engine.magnifier_volume_weighted_, 2};
    if (!prepare_public_begin(engine, args)) return;
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return;
        }
        const auto* configured = spec_ptr();
        if ((bar_magnifier || magnifier_samples != 4
             || magnifier_dist != MagnifierDistribution::ENDPOINTS)
            && (!configured || configured->intrabar.is_none())) {
            present_refusal(engine, "native magnifier arguments require an intrabar path");
            return;
        }
        if (!preflight_bars(engine, input_bars, n_input, false)
            || !preflight_intrabar_path(engine)) return;
        const int64_t initial = n_input > 0 ? input_bars[0].timestamp
                                            : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, input_bars, n_input);
        if (failed()) return;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::run_rich(BacktestEngine& engine,
                                       const Bar* input_bars, int n_input,
                                       const std::string& input_tf, const std::string& script_tf,
                                       const std::unordered_map<std::string, std::string>& inputs,
                                       const SymInfo& syminfo,
                                       const source::StrategyOverrides* overrides,
                                       bool bar_magnifier, int magnifier_samples,
                                       MagnifierDistribution magnifier_dist) {
    NativeBeginArgs args{input_bars, n_input, input_tf, script_tf, bar_magnifier,
        magnifier_samples, magnifier_dist, engine.magnifier_volume_weighted_, 2};
    args.inputs = &inputs;
    args.syminfo = &syminfo;
    args.overrides_opaque = overrides;
    if (!prepare_public_begin(engine, args)) return;
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return;
        }
        if (!preflight_bars(engine, input_bars, n_input, false)
            || !preflight_intrabar_path(engine)) return;
        const int64_t initial = n_input > 0 ? input_bars[0].timestamp
                                            : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, input_bars, n_input);
        if (failed()) return;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

bool NativeExecutionConsumer::stream_begin(BacktestEngine& engine,
                                           const Bar* warmup_bars, int n_warmup,
                                           const std::string& input_tf,
                                           const std::string& script_tf) {
    // Preserve the live stream before asking the provider to stage/configure a
    // new run.  The legacy route diagnoses this state first; in particular,
    // no warmup copy, adapter reset, or broker/spec mutation may occur.
    if (engine.stream_phase_ == BacktestEngine::StreamPhase::REALTIME) {
        render(engine, "stream is already realtime");
        return false;
    }
    if (const auto* running = std::get_if<NativeRunning>(&state_);
        running && running->phase == NativeRunPhase::Realtime) {
        render(engine, "stream is already realtime");
        return false;
    }
    // Native hosts that already have a strict staged spec can be rejected
    // before the provider is entered.  Source providers deliberately use the
    // legacy-tolerant policy and perform their equivalent borrowed-array check
    // in prepare_native_begin, where the warmup flag is formed.
    if (!failed()) {
        const auto* staged = spec_ptr();
        if (staged && staged->slot_label_policy == NativeSlotLabelPolicy::Canonical
            && !preflight_bars(engine, warmup_bars, n_warmup, true, true)) {
            return false;
        }
    }
    NativeBeginArgs args{warmup_bars, n_warmup, input_tf, script_tf, false, 4,
        MagnifierDistribution::ENDPOINTS, engine.magnifier_volume_weighted_, 2};
    args.is_stream = true;
    args.warmup_n = n_warmup;
    if (!prepare_public_begin(engine, args)) return false;
    if (!admit_public_begin(engine, "native stream_begin requires Ready")) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return false;
        }
        if (staged_fx_curve_) {
            present_refusal(engine,
                "timestamped account-currency FX is not supported by streaming");
            return false;
        }
        const auto* spec = spec_ptr();
        auto parsed = native_calendar::parse_timeframe(spec->input_tf);
        auto script = native_calendar::parse_timeframe(spec->script_tf);
        if (!parsed || !script) {
            present_refusal(engine, "native stream timeframe parse failed");
            return false;
        }
        const auto stream_pair = native_calendar::stream_compatibility(*parsed, *script);
        if (stream_pair.pairing == native_calendar::TimeframePairing::StreamMonthlyInputRefused) {
            present_refusal(engine, "native stream refuses monthly input");
            return false;
        }
        if (n_warmup <= 0 || warmup_bars == nullptr) {
            present_refusal(engine, "native stream warmup requires at least one bar");
            return false;
        }
        if (!preflight_bars(engine, warmup_bars, n_warmup, true)
            || !preflight_intrabar_path(engine)) return false;
        const auto* preflight_spec = spec_ptr();
        if (preflight_spec && n_warmup > 0
            && (!std::isfinite(warmup_bars[n_warmup - 1].close)
                || warmup_bars[n_warmup - 1].close <= 0.0)
            && native_legacy_tolerance_enabled(
                preflight_spec->legacy_tolerance,
                NativeLegacyTolerance::WarmupNonNegativeOHLC)) {
            present_refusal(engine, "stream warmup final close must be finite and positive");
            return false;
        }
        if (!begin_ready(engine, NativeRunPhase::Warmup, warmup_bars[0].timestamp)) return false;
        pump_batch(engine, warmup_bars, n_warmup);
        if (failed()) return false;
        if (auto* running = std::get_if<NativeRunning>(&state_)) {
            running->phase = NativeRunPhase::Realtime;
        }
        engine.stream_phase_ = BacktestEngine::StreamPhase::REALTIME;
        engine.stream_observe_actions_ = true;
        engine.stream_order_actions_.clear();
        engine.stream_action_sequence_ = 0;
        if (!has_last_price_ && n_warmup > 0) {
            last_price_ = warmup_bars[n_warmup - 1].close;
            has_last_price_ = true;
        }
        return true;
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_bar(BacktestEngine& engine, const Bar& bar) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            present_refusal(engine, "native stream_push_bar requires realtime");
            return false;
        }
        if (refuse_mixed_input_mode(engine, InputMode::ConfirmedBars)) return false;
        if (!native_bar_structurally_valid(bar)) {
            present_refusal(engine, "native confirmed bar has invalid OHLCV");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        if (!consume_confirmed_input(engine, bar, next_interval_index_, false)) return false;
        select_input_mode(InputMode::ConfirmedBars);
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::preflight_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) {
    if (n < 0 || (n > 0 && ticks == nullptr)) {
        present_refusal(engine, "native tick array is invalid");
        return false;
    }
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running || running->phase != NativeRunPhase::Realtime) {
        present_refusal(engine, "native stream_push_tick requires realtime");
        return false;
    }
    if (refuse_mixed_input_mode(engine, InputMode::ObservedTicks)) return false;
    if (n == 0) return true;
    uint64_t prev_sequence = last_tick_sequence_;
    bool prev_has_sequence = has_tick_sequence_;
    uint64_t ordinals = next_timeline_ordinal_;
    std::optional<int64_t> volume_slot;
    double volume = 0.0;
    if (has_forming_) {
        volume_slot = forming_.timestamp;
        volume = forming_.volume;
    }
    int64_t prev_array_ts = 0;
    bool has_array_prev = false;
    for (int i = 0; i < n; ++i) {
        const TradeTick& tick = ticks[i];
        if (!std::isfinite(tick.price) || tick.price <= 0.0) {
            present_refusal(engine, "native tick price must be finite and positive");
            return false;
        }
        if (!std::isfinite(tick.quantity) || tick.quantity < 0.0) {
            present_refusal(engine, "native tick quantity must be finite and non-negative");
            return false;
        }
        if (has_floor_ && tick.timestamp < decision_floor_ms_) {
            present_refusal(engine, "native tick timestamp is backwards or regresses the decision floor");
            return false;
        }
        if (has_array_prev && tick.timestamp < prev_array_ts) {
            present_refusal(engine, "native tick timestamp is backwards or out of order");
            return false;
        }
        prev_array_ts = tick.timestamp;
        has_array_prev = true;
        if (tick.sequence != 0) {
            if (prev_has_sequence && tick.sequence <= prev_sequence) {
                present_refusal(engine, "native tick sequence must increase");
                return false;
            }
            prev_sequence = tick.sequence;
            prev_has_sequence = true;
        }
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, tick.timestamp);
        if (!interval) {
            present_refusal(engine, "native tick is not aligned");
            return false;
        }
        const bool in_forming = has_forming_ && forming_.timestamp == interval->open_ms;
        if (last_finalized_input_ && interval->open_ms <= last_finalized_input_->open_ms
            && !in_forming) {
            present_refusal(engine, "native tick would reopen a closed input slot");
            return false;
        }
        if (!volume_slot || *volume_slot != interval->open_ms) {
            volume_slot = interval->open_ms;
            volume = 0.0;
        }
        volume += tick.quantity;
        if (!std::isfinite(volume)) {
            present_refusal(engine, "native tick volume overflow");
            return false;
        }
        if (ordinals == 0 || ordinals == std::numeric_limits<uint64_t>::max()) {
            present_refusal(engine, "native timeline ordinal exhausted");
            return false;
        }
        ++ordinals;
    }
    return true;
}

bool NativeExecutionConsumer::emit_quiet_carried_open(
        BacktestEngine& engine, const native_calendar::NativeInterval& interval) {
    if (!has_last_price_) return true;
    if (next_tradable_synthesis_cursor_ == interval.open_ms) return true;
    NativeDriverPoint point;
    point.coordinate = coordinate_from(interval, next_interval_index_,
                                       interval.eligible_open_ms,
                                       NativePriceProvenance::CarriedOpen,
                                       NativePathPhase::Open);
    point.coordinate.ordinal = take_ordinal(engine);
    point.raw_price = last_price_;
    point.matching = true;
    record_driver(point);
    match_point(engine, point);
    next_tradable_synthesis_cursor_ = interval.open_ms;
    if (failed()) return false;
    Bar quiet{last_price_, last_price_, last_price_, last_price_, 0.0, interval.open_ms};
    if (!contribute_input(engine, quiet, interval, next_interval_index_,
                          InputContribution::QuietCarried)) {
        return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::finalize_observed_tick_slot(
        BacktestEngine& engine,
        const native_calendar::NativeInterval& interval,
        NativeCompletionKind kind) {
    if (!has_forming_ || forming_.timestamp != interval.open_ms) return true;
    if (kind == NativeCompletionKind::PartialFinalized) {
        const bool equal = pairing_.pairing == native_calendar::TimeframePairing::Passthrough;
        if (equal) {
            NativeCoordinate calc;
            calc.interval_index = next_interval_index_;
            calc.open_ms = interval.open_ms;
            calc.eligible_open_ms = interval.eligible_open_ms;
            calc.last_traded_close_ms = interval.last_traded_close_ms;
            calc.next_period_open_ms = interval.next_period_open_ms;
            calc.next_input_open_ms = interval.next_input_open_ms;
            calc.source_price_time_ms = last_print_time_ms_;
            calc.completion = kind;
            calc.effective_time_ms = std::max(decision_floor_ms_, last_print_time_ms_);
            calc.provenance = NativePriceProvenance::PartialFinalized;
            calc.ordinal = take_ordinal(engine);
            raise_floor(calc.effective_time_ms);
            engine.current_bar_ = forming_;
            invoke_callback(engine, forming_, calc);
            if (failed()) return false;
            const auto* spec = spec_ptr();
            if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
                NativeDriverPoint point;
                point.coordinate = calc;
                point.coordinate.ordinal = take_ordinal(engine);
                point.coordinate.effective_time_ms = calc.effective_time_ms;
                point.coordinate.source_price_time_ms = last_print_time_ms_;
                point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
                point.raw_price = forming_.close;
                point.matching = true;
                record_driver(point);
                match_point(engine, point);
            }
        }
        has_forming_ = false;
        return !failed();
    }
    const Bar formed = forming_;
    has_forming_ = false;
    if (!contribute_input(engine, formed, interval, next_interval_index_,
                          InputContribution::ObservedTickSlot)) {
        return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::finalize_elapsed_slots(BacktestEngine& engine,
                                                     int64_t exclusive_end_ms) {
    std::optional<int64_t> cursor;
    if (last_finalized_input_) cursor = last_finalized_input_->next_input_open_ms;
    else if (last_accepted_input_) cursor = last_accepted_input_->next_input_open_ms;
    else if (last_observed_slot_open_) cursor = last_observed_slot_open_;
    while (cursor) {
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, *cursor);
        if (!interval) break;
        if (interval->next_period_open_ms > exclusive_end_ms) break;
        if (last_finalized_input_ && last_finalized_input_->open_ms == interval->open_ms) {
            if (interval->next_input_open_ms <= interval->open_ms) break;
            cursor = interval->next_input_open_ms;
            continue;
        }
        const bool forming_here = has_forming_ && forming_.timestamp == interval->open_ms;
        const bool observed = forming_here
            || (last_observed_slot_open_ && *last_observed_slot_open_ == interval->open_ms)
            || (last_accepted_input_ && last_accepted_input_->open_ms == interval->open_ms);
        const bool tradable = interval->last_traded_close_ms > interval->eligible_open_ms
            && native_calendar::in_session(calendar_, interval->eligible_open_ms);
        if (forming_here) {
            if (!finalize_observed_tick_slot(engine, *interval, NativeCompletionKind::Confirmed)) {
                return false;
            }
        } else if (!observed && tradable) {
            if (!emit_quiet_carried_open(engine, *interval)) return false;
        }
        last_finalized_input_ = *interval;
        if (interval->next_input_open_ms <= interval->open_ms) break;
        cursor = interval->next_input_open_ms;
        if (failed()) return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::deliver_tick(BacktestEngine& engine, const TradeTick& tick) {
    processing_input_ = true;
    select_input_mode(InputMode::ObservedTicks);
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, tick.timestamp);
    if (!interval) {
        processing_input_ = false;
        present_refusal(engine, "native tick is not aligned");
        return false;
    }
    if (!finalize_elapsed_slots(engine, interval->open_ms)) {
        processing_input_ = false;
        return false;
    }
    NativeDriverPoint point;
    point.coordinate = coordinate_from(*interval, next_interval_index_, tick.timestamp,
                                       NativePriceProvenance::ObservedPrint,
                                       NativePathPhase::None);
    point.coordinate.ordinal = take_ordinal(engine);
    point.coordinate.source_price_time_ms = tick.timestamp;
    point.raw_price = tick.price;
    if (tick.sequence != 0) point.sequence = tick.sequence;
    point.matching = true;
    point.excursion = true;
    record_driver(point);
    NativeTickContext tick_context;
    tick_context.decision.coordinate = point.coordinate;
    tick_context.decision.input_interval = *interval;
    if (const auto script_interval = script_interval_at(tick.timestamp)) {
        tick_context.decision.script_interval = *script_interval;
        tick_context.decision.script_bar_open_ms = script_interval->open_ms;
    } else {
        tick_context.decision.script_bar_open_ms = point.coordinate.open_ms;
    }
    tick_context.decision.sub_index = 0;
    tick_context.decision.sub_count = 1;
    tick_context.decision.is_terminal_sub_bar = true;
    tick_context.decision.sub_bar_open_ms = tick.timestamp;
    tick_context.decision.driver_statistics = driver_statistics_;
    tick_context.sequence = tick.sequence;
    const Bar tick_bar{tick.price, tick.price, tick.price, tick.price,
                       tick.quantity, tick.timestamp};
    if (!invoke_tick_callback(engine, tick_bar, tick_context)) {
        processing_input_ = false;
        return false;
    }
    match_point(engine, point);
    apply_excursion(engine, tick.price);
    raise_floor(tick.timestamp);
    last_price_ = tick.price;
    has_last_price_ = true;
    last_print_time_ms_ = tick.timestamp;
    if (!has_forming_) {
        forming_ = Bar{tick.price, tick.price, tick.price, tick.price, tick.quantity,
                       interval->open_ms};
        has_forming_ = true;
    } else {
        forming_.high = std::max(forming_.high, tick.price);
        forming_.low = std::min(forming_.low, tick.price);
        forming_.close = tick.price;
        forming_.volume += tick.quantity;
    }
    last_observed_slot_open_ = interval->open_ms;
    if (tick.sequence != 0) {
        last_tick_sequence_ = tick.sequence;
        has_tick_sequence_ = true;
    }
    processing_input_ = false;
    return !failed();
}

bool NativeExecutionConsumer::stream_push_tick(BacktestEngine& engine, const TradeTick& tick) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!preflight_ticks(engine, &tick, 1)) return false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        return deliver_tick(engine, tick);
    } catch (const std::exception& e) {
        processing_input_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!preflight_ticks(engine, ticks, n)) return false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        for (int i = 0; i < n; ++i) {
            if (!deliver_tick(engine, ticks[i])) return false;
            if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        }
        return !failed();
    } catch (const std::exception& e) {
        processing_input_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Stream)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            present_refusal(engine, "native stream_advance_time requires realtime");
            return false;
        }
        if (refuse_mixed_input_mode(engine, InputMode::ObservedTicks)) return false;
        if (has_floor_ && timestamp_ms < decision_floor_ms_) {
            present_refusal(engine, "native time advance regresses the decision floor");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Stream)) return false;
        select_input_mode(InputMode::ObservedTicks);
        if (has_last_price_ || has_forming_) {
            processing_input_ = true;
            const bool ok = finalize_elapsed_slots(engine, timestamp_ms);
            processing_input_ = false;
            if (!ok) return false;
        }
        if (script_.has_data && !script_.sealed
            && timestamp_ms >= script_.interval.next_period_open_ms) {
            processing_input_ = true;
            seal_script(engine, NativeCompletionKind::Confirmed);
            script_ = ScriptBucket{};
            processing_input_ = false;
            if (failed()) return false;
        }
        raise_floor(timestamp_ms);
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Stream)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!std::holds_alternative<NativeRunning>(state_)) {
            present_refusal(engine, "native stream_end requires a running host");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Stream)) return false;
        if (finalize_partial_input_bar && has_forming_) {
            auto forming_interval = native_calendar::interval_containing(
                calendar_, input_tf_, forming_.timestamp);
            if (forming_interval) {
                if (!finalize_observed_tick_slot(engine, *forming_interval,
                                                 NativeCompletionKind::PartialFinalized)) {
                    return false;
                }
            }
        }
        if (failed()) return false;
        record_open_position_report_rows(engine);
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) {
            render(engine, "native stream_end lost running state");
            return false;
        }
        NativeRunSpec spec = std::move(running->spec);
        state_.emplace<NativeCompleted>(NativeCompleted{std::move(spec), NativeCompletion::StreamEnded});
        engine.stream_phase_ = BacktestEngine::StreamPhase::IDLE;
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

native_order::SubmitResult NativeExecutionConsumer::submit_with_surface(
        BacktestEngine& engine, const native_order::Request& request,
        native_order::CommandSurface surface) {
    if (!commands_allowed()) {
        throw std::runtime_error("native submit refused outside allowed phase");
    }
    native_order::CommandContext ctx;
    native_order::PreparedSubmit prepared;
    try {
        ctx = make_command_context(engine, request, surface);
        prepared = requests_.prepare_submit(
            request, ctx, engine.next_order_incarnation_, next_timeline_ordinal_);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command});
        render(engine, "native submit produced no preparation");
        throw std::runtime_error("native submit produced no preparation");
    }
    auto installed = requests_.install_submit(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command,
                                   0, static_cast<uint32_t>(*err)});
        render(engine, "native submit install failed");
        throw std::runtime_error("native submit install failed");
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::SubmitResult>>(installed);
    note_terminal_events(ok.events);
    clear_cohort_target_cache();
    catch_up_timeline();
    if (ok.result.status == native_order::SubmitStatus::Accepted) {
        ++engine.next_order_incarnation_;
        if (ok.result.handle) record_pre_open_birth(request, *ok.result.handle);
    }
    return std::move(ok.result);
}

native_order::ReplaceResult NativeExecutionConsumer::replace_with_surface(
        BacktestEngine& engine, const native_order::RequestHandle& target,
        const native_order::Request& request, native_order::CommandSurface surface) {
    if (!commands_allowed()) {
        throw std::runtime_error("native replace refused outside allowed phase");
    }
    native_order::CommandContext ctx;
    native_order::PreparedReplace prepared;
    try {
        ctx = make_command_context(engine, request, surface);
        prepared = requests_.prepare_replace(
            target, request, ctx, engine.next_order_incarnation_, next_timeline_ordinal_);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command});
        render(engine, "native replace produced no preparation");
        throw std::runtime_error("native replace produced no preparation");
    }
    const native_order::EventId predicted = prepared.predicted_event_id();
    const auto predicted_status = prepared.predicted().status;
    auto installed = requests_.install_replace(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command,
                                   predicted.ordinal, static_cast<uint32_t>(*err)});
        render(engine, "native replace install failed");
        throw std::runtime_error("native replace install failed");
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::ReplaceResult>>(installed);
    note_terminal_events(ok.events);
    if (predicted_status == native_order::ReplaceStatus::Replaced && ok.result.successor) {
        const auto* successor = requests_.find_live(*ok.result.successor);
        if (successor && std::holds_alternative<native_order::CohortClose>(successor->authority)) {
            // A resting cohort replacement changes trigger/definition facts,
            // not the physical opening roster captured by this derived view.
            // Preserve it across ordinary source bracket reissues; dependency
            // mutations below still clear it through install_mutation().
            retarget_cohort_target_cache(target, *ok.result.successor);
        } else {
            clear_cohort_target_cache();
        }
    } else if (predicted_status != native_order::ReplaceStatus::NotWorking) {
        clear_cohort_target_cache();
    }
    catch_up_timeline();
    if (predicted_status == native_order::ReplaceStatus::Replaced) {
        ++engine.next_order_incarnation_;
        if (ok.result.successor) record_pre_open_birth(request, *ok.result.successor);
        try {
            drain_parent_terminal(engine, predicted, target, NativeFailureOperation::Command);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Command, predicted.ordinal});
            render(engine, e.what());
            throw;
        }
        if (failed()) {
            throw std::runtime_error("native replace dependency cleanup failed");
        }
    }
    return std::move(ok.result);
}

native_order::SubmitResult NativeExecutionConsumer::submit(BacktestEngine& engine,
                                                           const native_order::Request& request) {
    return submit_with_surface(engine, request, native_order::CommandSurface::General);
}

native_order::ReplaceResult NativeExecutionConsumer::replace(
        BacktestEngine& engine,
        const native_order::RequestHandle& target,
        const native_order::Request& request) {
    return replace_with_surface(engine, target, request, native_order::CommandSurface::General);
}

native_order::SubmitResult NativeExecutionConsumer::submit_market(
        BacktestEngine& engine, const native_order::Request& request) {
    return submit_with_surface(engine, request, native_order::CommandSurface::MarketOnly);
}

native_order::ReplaceResult NativeExecutionConsumer::replace_market(
        BacktestEngine& engine,
        const native_order::RequestHandle& target,
        const native_order::Request& request) {
    return replace_with_surface(engine, target, request, native_order::CommandSurface::MarketOnly);
}

native_order::CancelResult NativeExecutionConsumer::cancel(
        BacktestEngine& engine, const native_order::RequestHandle& target) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cancel refused outside allowed phase");
    }
    native_order::PreparedCancel prepared;
    try {
        prepared = requests_.prepare_cancel(target, next_timeline_ordinal_);
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
    if (!prepared) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command});
        render(engine, "native cancel produced no preparation");
        throw std::runtime_error("native cancel produced no preparation");
    }
    const auto predicted_status = prepared.predicted().status;
    const native_order::EventId predicted = prepared.predicted_event_id();
    auto installed = requests_.install_cancel(std::move(prepared));
    if (const auto* err = std::get_if<native_order::InstallError>(&installed)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Command,
                                   predicted.ordinal, static_cast<uint32_t>(*err)});
        render(engine, "native cancel install failed");
        throw std::runtime_error("native cancel install failed");
    }
    auto& ok = std::get<native_order::CommandInstalled<native_order::CancelResult>>(installed);
    note_terminal_events(ok.events);
    clear_cohort_target_cache();
    catch_up_timeline();
    if (predicted_status == native_order::CancelStatus::Cancelled) {
        try {
            drain_parent_terminal(engine, predicted, target, NativeFailureOperation::Command);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Command, predicted.ordinal});
            render(engine, e.what());
            throw;
        }
        if (failed()) {
            throw std::runtime_error("native cancel dependency cleanup failed");
        }
    }
    return std::move(ok.result);
}

native_order::CohortHandle NativeExecutionConsumer::cohort_open(BacktestEngine& engine) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cohort_open refused outside allowed phase");
    }
    try {
        const auto cohort = requests_.cohort_open();
        clear_cohort_target_cache();
        return cohort;
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
}

void NativeExecutionConsumer::cohort_add(
        BacktestEngine& engine, native_order::CohortHandle cohort,
        native_order::RequestHandle origin) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cohort_add refused outside allowed phase");
    }
    try {
        requests_.cohort_add(cohort, std::move(origin));
        clear_cohort_target_cache();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
}

void NativeExecutionConsumer::cohort_remove(
        BacktestEngine& engine, native_order::CohortHandle cohort,
        native_order::RequestHandle origin) {
    if (!commands_allowed()) {
        throw std::runtime_error("native cohort_remove refused outside allowed phase");
    }
    try {
        requests_.cohort_remove(cohort, std::move(origin));
        clear_cohort_target_cache();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Command});
        render(engine, e.what());
        throw;
    }
}

NativePhysicalPosition NativeExecutionConsumer::position(const BacktestEngine& engine) const {
    NativePhysicalPosition out;
    const auto n = engine.pyramid_entries_.size();
    out.lot_count = n;
    if (n == 0) return out;
    if (n == 1) {
        const auto& lot = engine.pyramid_entries_[0];
        out.signed_units = engine.position_side_ == PositionSide::SHORT ? -lot.qty : lot.qty;
        // R4-D L10z review fix 5: the single-lot fast path keeps the weighted
        // path's zero-quantity guard, so an empty lot reports no average.
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

double NativeExecutionConsumer::marked(const BacktestEngine& engine, double price) const {
    return engine.marked_equity(price);
}

std::vector<NativeMarketEvent> NativeExecutionConsumer::events_after(uint64_t after_ordinal) const {
    const auto& history = requests_.history();
    auto command_begin = history.end();
    if (!history.empty() && command_ordinal(history.back()) > after_ordinal) {
        auto it = history.end();
        while (it != history.begin()) {
            auto prev = std::prev(it);
            if (command_ordinal(*prev) <= after_ordinal) {
                command_begin = it;
                break;
            }
            it = prev;
            if (std::distance(it, history.end()) > 32) {
                command_begin = std::upper_bound(history.begin(), it, after_ordinal,
                    [](uint64_t ordinal, const native_order::CommandEvent& event) {
                        return ordinal < command_ordinal(event);
                    });
                break;
            }
        }
        if (it == history.begin()) command_begin = history.begin();
    }

    auto driver_begin = driver_log_.end();
    if (!driver_log_.empty() && driver_log_.back().coordinate.ordinal > after_ordinal) {
        auto it = driver_log_.end();
        while (it != driver_log_.begin()) {
            auto prev = std::prev(it);
            if (prev->coordinate.ordinal <= after_ordinal) {
                driver_begin = it;
                break;
            }
            it = prev;
            if (std::distance(it, driver_log_.end()) > 32) {
                driver_begin = std::upper_bound(driver_log_.begin(), it, after_ordinal,
                    [](uint64_t ordinal, const NativeDriverPoint& point) {
                        return ordinal < point.coordinate.ordinal;
                    });
                break;
            }
        }
        if (it == driver_log_.begin()) driver_begin = driver_log_.begin();
    }

    auto account_begin = account_log_.end();
    if (!account_log_.empty() && account_log_.back().ordinal > after_ordinal) {
        auto it = account_log_.end();
        while (it != account_log_.begin()) {
            auto prev = std::prev(it);
            if (prev->ordinal <= after_ordinal) {
                account_begin = it;
                break;
            }
            it = prev;
            if (std::distance(it, account_log_.end()) > 32) {
                account_begin = std::upper_bound(account_log_.begin(), it, after_ordinal,
                    [](uint64_t ordinal, const NativeAccountObservation& account) {
                        return ordinal < account.ordinal;
                    });
                break;
            }
        }
        if (it == account_log_.begin()) account_begin = account_log_.begin();
    }

    const std::size_t command_count = static_cast<std::size_t>(std::distance(command_begin, history.end()));
    const std::size_t driver_count = static_cast<std::size_t>(std::distance(driver_begin, driver_log_.end()));
    const std::size_t account_count = static_cast<std::size_t>(std::distance(account_begin, account_log_.end()));

    std::vector<NativeMarketEvent> out;
    out.reserve(command_count + driver_count + account_count);

    for (auto it = command_begin; it != history.end(); ++it) {
        const auto& event = *it;
        const uint64_t ordinal = command_ordinal(event);
        out.emplace_back(NativeMarketEvent{NativeEventKind::Command, ordinal, event, std::nullopt, std::nullopt});
    }
    for (auto it = driver_begin; it != driver_log_.end(); ++it) {
        const auto& point = *it;
        out.emplace_back(NativeMarketEvent{NativeEventKind::Driver, point.coordinate.ordinal, std::nullopt, point, std::nullopt});
    }
    for (auto it = account_begin; it != account_log_.end(); ++it) {
        const auto& account = *it;
        out.emplace_back(NativeMarketEvent{NativeEventKind::Account, account.ordinal, std::nullopt, std::nullopt, account});
    }
    const auto cmp = [](const NativeMarketEvent& a, const NativeMarketEvent& b) {
        if (a.ordinal != b.ordinal) return a.ordinal < b.ordinal;
        return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    };
    if (account_count > 0) {
        std::sort(out.begin(), out.end(), cmp);
    } else if (driver_count > 0 && command_count > 0) {
        std::inplace_merge(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(command_count), out.end(), cmp);
    }
    return out;
}

uint64_t NativeExecutionConsumer::event_high_water() const noexcept {
    uint64_t high = 0;
    const auto& history = requests_.history();
    if (!history.empty()) high = std::max(high, command_ordinal(history.back()));
    if (!driver_log_.empty()) high = std::max(high, driver_log_.back().coordinate.ordinal);
    if (!account_log_.empty()) high = std::max(high, account_log_.back().ordinal);
    return high;
}

void NativeExecutionConsumer::note_terminal_events(
        const native_order::EventRange& events) noexcept {
    const auto& history = requests_.history();
    const std::size_t end = std::min(history.size(), events.first_index + events.count);
    for (std::size_t index = events.first_index; index < end; ++index) {
        const auto& event = history[index];
        const bool terminal = std::visit([](const auto& payload) {
            using Event = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Event, native_order::CancelledEvent>
                          || std::is_same_v<Event, native_order::MatchRejectedEvent>) {
                return true;
            } else if constexpr (std::is_same_v<Event, native_order::ExecutionAppliedEvent>) {
                return payload.terminal;
            }
            return false;
        }, event);
        if (terminal) {
            terminal_receipt_high_water_ = std::max(
                terminal_receipt_high_water_, command_ordinal(event));
        }
    }
}

void NativeExecutionConsumer::reject_inherited_on_bar(BacktestEngine& engine) {
    try {
        refuse_source_mutation("on_bar");
    } catch (const std::exception& e) {
        render(engine, e.what());
    }
}

NativeStrategyHost::NativeStrategyHost()
    : BacktestEngine(NativeConsumerBindTag{}) {}

NativeStrategyHost::~NativeStrategyHost() = default;

void NativeStrategyHost::on_bar(const Bar&) {
    as_native_consumer(execution_consumer()).reject_inherited_on_bar(*this);
}

NativeSetupResult NativeStrategyHost::configure_native(const NativeRunSpec& spec) {
    return as_native_consumer(execution_consumer()).configure(*this, spec);
}

NativeFxCurveSetupResult NativeStrategyHost::configure_native_fx_curve(
        const NativeFxCurve& curve) {
    return as_native_consumer(execution_consumer()).configure_fx_curve(curve);
}

NativeStateView NativeStrategyHost::native_state() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).view();
}

native_order::SubmitResult NativeStrategyHost::submit(const native_order::Request& request) {
    return as_native_consumer(execution_consumer()).submit(*this, request);
}

native_order::ReplaceResult NativeStrategyHost::replace(
        const native_order::RequestHandle& target, const native_order::Request& request) {
    return as_native_consumer(execution_consumer()).replace(*this, target, request);
}

native_order::SubmitResult NativeStrategyHost::submit_market(const native_order::Request& request) {
    return as_native_consumer(execution_consumer()).submit_market(*this, request);
}

native_order::ReplaceResult NativeStrategyHost::replace_market(
        const native_order::RequestHandle& target, const native_order::Request& request) {
    return as_native_consumer(execution_consumer()).replace_market(*this, target, request);
}

native_order::CancelResult NativeStrategyHost::cancel(const native_order::RequestHandle& target) {
    return as_native_consumer(execution_consumer()).cancel(*this, target);
}

native_order::CohortHandle NativeStrategyHost::cohort_open() {
    return as_native_consumer(execution_consumer()).cohort_open(*this);
}

void NativeStrategyHost::cohort_add(
        native_order::CohortHandle cohort, native_order::RequestHandle origin) {
    as_native_consumer(execution_consumer()).cohort_add(*this, cohort, std::move(origin));
}

void NativeStrategyHost::cohort_remove(
        native_order::CohortHandle cohort, native_order::RequestHandle origin) {
    as_native_consumer(execution_consumer()).cohort_remove(*this, cohort, std::move(origin));
}

std::optional<NativeCurrentPointView> NativeStrategyHost::current_execution_point() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .current_execution_point();
}

std::optional<NativeTrailState> NativeStrategyHost::trail_state(
        const native_order::RequestHandle& target) const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .trail_state(*this, target);
}

NativeCurrentExecutionPreview NativeStrategyHost::inspect_current_execution(
        const NativeCurrentExecution& command) const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .inspect_current_execution(*this, command);
}

NativeCurrentExecutionResult NativeStrategyHost::execute_current(const NativeCurrentExecution& command) {
    return as_native_consumer(execution_consumer()).execute_current(*this, command);
}

NativePhysicalPosition NativeStrategyHost::physical_position() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).position(*this);
}

double NativeStrategyHost::native_marked_equity(double mark) const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).marked(*this, mark);
}

std::vector<NativeMarketEvent> NativeStrategyHost::native_events(uint64_t after_ordinal) const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .events_after(after_ordinal);
}

int64_t NativeStrategyHost::native_decision_floor() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).decision_floor();
}

uint64_t NativeStrategyHost::native_consumed_high_water() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).high_water();
}

uint64_t NativeStrategyHost::native_continuation_hash() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .continuation_hash();
}

}  // inline namespace engine_script_run_v17
}  // namespace pineforge
