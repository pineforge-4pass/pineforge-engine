#include "native_execution_consumer.hpp"
#include "engine_internal.hpp"
#include "native_matching.hpp"

#include <pineforge/execution_close_scope.hpp>
#include <pineforge/market_driver.hpp>

#include <algorithm>
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
inline namespace engine_script_run_v15 {
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

void hash_spec(Fnv& f, const NativeRunSpec& spec) noexcept {
    f.s(spec.identity.session_key); f.u(spec.identity.run_number);
    f.s(spec.input_tf); f.s(spec.script_tf);
    f.s(spec.ticker); f.s(spec.tickerid); f.s(spec.type);
    f.s(spec.currency); f.s(spec.basecurrency); f.s(spec.description); f.s(spec.volumetype);
    f.s(spec.timezone); f.s(spec.session); f.s(spec.chart_timezone);
    f.d(spec.initial_capital); f.d(spec.point_value); f.d(spec.account_fx); f.d(spec.price_tick);
    f.u(spec.slippage_ticks); f.u(static_cast<uint64_t>(spec.fee_kind)); f.d(spec.fee_value);
    f.b(spec.quantity_grid.has_value()); if (spec.quantity_grid) f.d(*spec.quantity_grid);
    f.u(static_cast<uint64_t>(spec.close_execution));
    f.b(spec.max_abs_units.has_value()); if (spec.max_abs_units) f.d(*spec.max_abs_units);
    f.b(spec.max_open_lots.has_value()); if (spec.max_open_lots) f.u(*spec.max_open_lots);
    f.u(static_cast<uint64_t>(spec.allowed_open_directions));
    f.b(spec.initial_margin_fraction.has_value());
    if (spec.initial_margin_fraction) f.d(*spec.initial_margin_fraction);
}

void hash_handle(Fnv& f, const native_order::RequestHandle& handle) noexcept {
    f.s(handle.run.session_key);
    f.u(handle.run.run_number);
    f.u(handle.incarnation);
}

void hash_event_id(Fnv& f, const native_order::EventId& id) noexcept {
    f.s(id.run.session_key);
    f.u(id.run.run_number);
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
        && terms.shape == native_order::OpeningShape::Transact;
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
    if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) f.d(limit->price);
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
    if (const auto* wait = std::get_if<native_order::WaitForApplied>(&owner)) {
        hash_handle(f, wait->parent);
    }
    if (const auto* bind = std::get_if<native_order::BindOpening>(&owner)) {
        hash_handle(f, bind->opening);
        f.i(bind->cycle);
    }
    if (const auto* bind = std::get_if<native_order::BindOpenings>(&owner)) {
        f.i(bind->cycle); f.u(bind->openings.size());
        for (const auto& handle : bind->openings) hash_handle(f, handle);
    }
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
    if (const auto* wait = std::get_if<native_order::Wait>(&authority)) {
        hash_handle(f, wait->parent);
    } else if (const auto* armed = std::get_if<native_order::ArmedTransaction>(&authority)) {
        hash_handle(f, armed->parent);
        hash_event_id(f, armed->cause);
        hash_cursor(f, armed->cause_cursor);
    } else if (const auto* close = std::get_if<native_order::BookClose>(&authority)) {
        f.i(close->cycle);
        f.u(static_cast<uint64_t>(close->side));
        hash_event_id(f, close->binding_event);
        hash_cursor(f, close->binding_cursor);
    } else if (const auto* opening = std::get_if<native_order::OpeningClose>(&authority)) {
        hash_handle(f, opening->opening);
        f.i(opening->cycle);
        f.u(static_cast<uint64_t>(opening->side));
        f.u(opening->enrollment.index());
        if (const auto* from_cmd = std::get_if<native_order::EnrollmentFromCommand>(&opening->enrollment)) {
            hash_event_id(f, from_cmd->accepted);
        }
        if (const auto* from_app = std::get_if<native_order::EnrollmentFromApplied>(&opening->enrollment)) {
            hash_event_id(f, from_app->cause);
            hash_cursor(f, from_app->cursor);
        }
    } else if (const auto* openings = std::get_if<native_order::OpeningsClose>(&authority)) {
        f.i(openings->cycle); f.u(static_cast<uint64_t>(openings->side));
        f.u(openings->openings.size());
        for (const auto& handle : openings->openings) hash_handle(f, handle);
        f.u(openings->enrollment.index());
        if (const auto* cmd = std::get_if<native_order::EnrollmentFromCommand>(&openings->enrollment))
            hash_event_id(f, cmd->accepted);
        if (const auto* app = std::get_if<native_order::EnrollmentFromApplied>(&openings->enrollment)) {
            hash_event_id(f, app->cause); hash_cursor(f, app->cursor);
        }
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

void hash_current_point(Fnv& f, const NativeCurrentPointView& point) noexcept {
    hash_coordinate(f, point.decision.coordinate);
    f.i(point.decision.decision_floor_ms);
    hash_interval(f, point.decision.input_interval);
    hash_interval(f, point.decision.script_interval);
    f.d(point.price);
    f.u(static_cast<uint64_t>(point.quote_kind));
    f.u(point.quote_origin_ordinal);
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
    if (in_callback_ || processing_input_
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
        render(engine, "native run aborted");
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
    if (auto* r = std::get_if<NativeReady>(&state_)) return &r->spec;
    if (auto* n = std::get_if<NativeRunning>(&state_)) return &n->spec;
    if (auto* c = std::get_if<NativeCompleted>(&state_)) return &c->spec;
    if (auto* f = std::get_if<NativeFailed>(&state_)) {
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
    NativeFailure failure;
    failure.code = NativeFailureCode::UnsupportedSource;
    failure.operation = NativeFailureOperation::Mutation;
    latch_failure(failure);
    throw std::runtime_error(std::string("native host refuses source mutation: ") +
                             (operation ? operation : ""));
}

uint64_t NativeExecutionConsumer::continuation_hash() const noexcept {
    Fnv f;
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
    f.u(consumed_high_water_);
    f.s(bound_session_key_);
    f.i(decision_floor_ms_);
    f.b(has_floor_);
    f.u(next_timeline_ordinal_);
    f.b(in_callback_);
    f.b(consuming_request_);
    f.b(draining_notifications_);
    f.b(current_frame_.has_value());
    if (current_frame_) {
        hash_current_point(f, current_frame_->point);
        f.u(current_frame_->acceptance_cutoff);
    }
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
    f.b(staged_fx_curve_.has_value());
    if (staged_fx_curve_) f.u(native_fx_curve_digest(*staged_fx_curve_));
    hash_tz_identity(f, tz_identity_);
    f.s(requests_.identity().session_key);
    f.u(requests_.identity().run_number);
    f.u(requests_.live().size());
    for (const auto& live : requests_.live()) {
        hash_definition(f, live.definition);
        hash_remaining(f, live.remaining);
        hash_authority(f, live.authority);
        hash_trigger_state(f, live.trigger_state);
        hash_allowance(f, live.allowance);
        hash_pending(f, live.pending);
    }
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
    f.u(static_cast<uint64_t>(pairing_.pairing));
    f.i(pairing_.group_factor);
    f.u(driver_digest_.count);
    f.u(driver_digest_.h);
    f.u(account_digest_.count);
    f.u(account_digest_.h);
    return f.h;
}

bool NativeExecutionConsumer::timeframe_args_ok(const std::string& input_tf,
                                                const std::string& script_tf) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    if (!input_tf.empty() && input_tf != spec->input_tf) return false;
    if (!script_tf.empty() && script_tf != spec->script_tf) return false;
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
    engine.process_orders_on_close_ = false;
    engine.calc_on_order_fills_ = false;
    applied_ = spec;
    auto parsed_input = native_calendar::parse_timeframe(spec.input_tf);
    auto parsed_script = native_calendar::parse_timeframe(spec.script_tf);
    auto parsed_session = native_calendar::parse_session(spec.session, spec.timezone);
    if (!parsed_input || !parsed_script || !parsed_session) return false;
    input_tf_ = std::move(*parsed_input);
    script_tf_ = std::move(*parsed_script);
    calendar_ = std::move(*parsed_session);
    pairing_ = native_calendar::compatibility(input_tf_, script_tf_);
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
    if (failed()) {
        result.validation.error = NativeRunSpecError::CalendarFailure;
        render(engine, "native host already failed");
        return result;
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
    if (auto* completed = std::get_if<NativeCompleted>(&state_)) {
        if (candidate.identity.session_key != completed->spec.identity.session_key
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
    auto parsed_input = native_calendar::parse_timeframe(candidate.input_tf);
    auto parsed_script = native_calendar::parse_timeframe(candidate.script_tf);
    auto parsed_session = native_calendar::parse_session(candidate.session, candidate.timezone);
    if (!parsed_input || !parsed_script || !parsed_session) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
        render(engine, "native calendar parse failed at configure");
        return result;
    }
    input_tf_ = std::move(*parsed_input);
    script_tf_ = std::move(*parsed_script);
    calendar_ = std::move(*parsed_session);
    pairing_ = native_calendar::compatibility(input_tf_, script_tf_);
    staged_fx_curve_.reset();
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
    requests_.reset(spec.identity);
    next_timeline_ordinal_ = 1;
    decision_floor_ms_ = initial_floor_ms;
    has_floor_ = true;
    input_mode_ = InputMode::Unselected;
    current_frame_.reset();
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
    driver_log_.clear();
    account_log_.clear();
    history_digest_.reset();
    driver_digest_.reset();
    account_digest_.reset();
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
                                             bool stream) {
    const auto* spec = spec_ptr();
    if (!spec) {
        present_refusal(engine, "native preflight requires a staged spec");
        return false;
    }
    const auto result = preflight_native_inputs(
        *spec, bars, n,
        stream ? NativeInputPolicy::StreamWarmup : NativeInputPolicy::Batch);
    if (result) return true;
    switch (result.error) {
    case NativeInputPreflightError::NullArray:
        present_refusal(engine, "native bars require a non-null array");
        break;
    case NativeInputPreflightError::InvalidCount:
        present_refusal(engine, "native bar count is invalid");
        break;
    case NativeInputPreflightError::StructuralInvalid:
        present_refusal(engine, "native bar failed structural validation");
        break;
    case NativeInputPreflightError::Unaligned:
        present_refusal(engine, "native bar is not aligned to the configured calendar");
        break;
    case NativeInputPreflightError::OffGridLabel:
        present_refusal(engine, "native confirmed bar timestamp is not a canonical slot label");
        break;
    case NativeInputPreflightError::NotStrictlyIncreasing:
        present_refusal(engine, "native timestamps must be strictly increasing");
        break;
    case NativeInputPreflightError::OverlappingSlot:
        present_refusal(engine, "native input intervals overlap");
        break;
    case NativeInputPreflightError::InSessionGap:
        present_refusal(engine, "native stream has an in-session gap");
        break;
    case NativeInputPreflightError::CalendarFailure:
        present_refusal(engine, "native calendar parse failed during input preflight");
        break;
    case NativeInputPreflightError::None:
        break;
    }
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
    f.h = history_digest_.h;
    for (std::size_t i = history_digest_.count; i < hist.size(); ++i) {
        hash_command(f, hist[i]);
    }
    history_digest_.h = f.h;
    history_digest_.count = hist.size();
}

void NativeExecutionConsumer::fold_driver_digest(const NativeDriverPoint& point) const noexcept {
    Fnv f;
    f.h = driver_digest_.h;
    hash_driver_point(f, point);
    driver_digest_.h = f.h;
    ++driver_digest_.count;
}

void NativeExecutionConsumer::fold_account_digest(const NativeAccountObservation& row) const noexcept {
    Fnv f;
    f.h = account_digest_.h;
    hash_account_row(f, row);
    account_digest_.h = f.h;
    ++account_digest_.count;
}

void NativeExecutionConsumer::record_driver(const NativeDriverPoint& point) {
    driver_log_.push_back(point);
    fold_driver_digest(point);
}

void NativeExecutionConsumer::apply_excursion(BacktestEngine& engine, double price) {
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
    } else if (const auto* bind = std::get_if<native_order::BindOpenings>(&live->request().owner)) {
        out.openings = read_openings(engine, bind->openings, bind->cycle);
    } else if (const auto* bind = std::get_if<native_order::BindOpening>(&live->request().owner)) {
        out.opening = read_opening(engine, bind->opening, bind->cycle);
    }
    return out;
}

native_order::CommandContext NativeExecutionConsumer::make_command_context(
        const BacktestEngine& engine, const native_order::Request& request,
        native_order::CommandSurface surface) const {
    native_order::CommandContext ctx;
    ctx.decision_time_ms = decision_floor();
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
    if (spec->initial_margin_fraction) {
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
    sync_history_digest();
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
    sync_history_digest();
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
    native_order::EvaluationContext evaluation;
    evaluation.cursor = cursor;
    evaluation.driver_class = classify_driver(point, continuous);
    evaluation.existing_matching_bit = point.matching;
    std::vector<native_order::RequestHandle> handles;
    handles.reserve(requests_.live().size());
    for (const auto& live : requests_.live()) handles.push_back(live.handle());
    for (const auto& handle : handles) {
        if (failed()) return;
        const auto* live = requests_.find_live(handle);
        if (!live) continue;
        if (!requests_.evaluation_eligible(*live, evaluation)) continue;
        const auto* track = std::get_if<native_order::TrailTrack>(&live->trigger_state);
        if (!track) continue;
        const bool buy = requests_.working_is_buy(*live);
        if (!native_matching::trail_best_improves(track->best, price, buy)) continue;
        native_order::Preparation<native_order::PreparedMutation> prep;
        try {
            prep = requests_.prepare_trigger(
                handle, native_order::ObserveTrailExtremum{cursor, price},
                evaluation.driver_class, next_timeline_ordinal_);
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
        ? engine.position_side_ == PositionSide::SHORT : requests_.working_is_buy(live);
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
            candidate.physical = native_order::Transact{requests_.working_is_buy(live) ? qty : -qty};
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
    // The real commit pins this ticket; every preview must use its allocation too.
    candidate.fill.commission_account = candidate.inspect.current_ticket;
    return candidate;
}

NativeCurrentPointView NativeExecutionConsumer::execution_anchor(
        const native_order::MatchCursor& cursor, double resolved) const {
    NativeCurrentPointView out;
    out.decision.coordinate = cursor.point;
    out.decision.decision_floor_ms = std::max(decision_floor(), cursor.point.effective_time_ms);
    if (auto input = native_calendar::interval_containing(calendar_, input_tf_, cursor.point.open_ms))
        out.decision.input_interval = *input;
    if (auto script = native_calendar::interval_containing(calendar_, script_tf_, cursor.point.open_ms))
        out.decision.script_interval = *script;
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
            && std::holds_alternative<native_order::RemainingDeferred>(live->remaining);
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
        if (terms.units && *terms.units > 0.0) {
            const auto* spec = spec_ptr();
            if (!spec || (spec->quantity_grid
                          && !native_order::quantity_on_grid(*terms.units,
                                                              *spec->quantity_grid))) {
                return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
            }
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
        if (current && resolved_price <= 0.0 && !zero_units_terminal) {
            const auto provisional = inspect_candidate(engine, *live, evaluation.cursor, resolved_price,
                                                       plan ? &*plan : nullptr);
            if (provisional.inspect.would_open) {
                return terminal(native_order::MatchRejectReason::NonpositivePrice,
                                nonidentity_attempt);
            }
        }
        if (!zero_units_terminal
            && (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state))) {
            std::optional<double> level;
            if (const auto* limit = std::get_if<native_order::Limit>(&live->request().trigger)) {
                level = limit->price;
            } else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(
                           &live->request().trigger)) {
                level = stop_limit->limit;
            }
            if (!level || (terms_facts.is_buy && resolved_price > *level)
                || (!terms_facts.is_buy && resolved_price < *level)) {
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
        native_order::MatchRejectReason reason{};
        if (inspect.would_open && !admit_opening_inspect(engine, resolved_price, inspect, &reason))
            return terminal(reason, nonidentity_attempt);

        native_order::ExecutionProposal proposal;
        proposal.cursor = evaluation.cursor;
        proposal.raw_price = raw_price;
        proposal.resolved_price = resolved_price;
        proposal.physical_action = candidate.physical;
        proposal.scope = candidate.scope;
        proposal.pre_fill = read_position(engine);
        proposal.pre_target = candidate.target;
        proposal.inspected_closed_units = inspect.closed_units;
        proposal.inspected_opened_units = inspect.opened_units;
        proposal.inspected_current_ticket = inspect.current_ticket;
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
        view.inspected_current_ticket = inspect.current_ticket;
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
        if (view.settlement_readiness == execution::Status::Applied) {
            NativePrecommitVerdict verdict = NativePrecommitVerdict::Proceed;
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
            if (verdict == NativePrecommitVerdict::Refuse) {
                return terminal(native_order::MatchRejectReason::HostPrecommit,
                                nonidentity_attempt);
            }
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
    const auto driver_class = classify_driver(point, continuous);
    const uint64_t P = point.coordinate.ordinal;
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
    auto provenance_still_matches = [&](const CandidateProvenance& row) {
        const auto* live = requests_.find_live(row.handle);
        if (!live) return false;
        const bool buy = requests_.working_is_buy(*live);
        return row.is_buy == buy
            && row.trigger_state_index == live->trigger_state.index()
            && same_optional_bits(row.trigger_level, level_for(*live, row.kind, buy));
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
        std::vector<native_order::RequestHandle> snapshot;
        snapshot.reserve(requests_.live().size());
        for (const auto& live : requests_.live()) snapshot.push_back(live.handle());
        for (const auto& handle : snapshot) {
            const auto* live = requests_.find_live(handle);
            if (!live) continue;
            const auto facts = requests_.eligibility_facts(*live, eval);
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
                const bool buy = requests_.working_is_buy(*live);
                const auto& trigger = live->request().trigger;
                const auto& state = live->trigger_state;
                std::optional<native_matching::GeometricHit> hit;
                Kind kind = Kind::Fill;
                if (std::holds_alternative<native_order::StopIdle>(state)) {
                    const auto* stop = std::get_if<native_order::Stop>(&trigger);
                    if (!stop) continue;
                    hit = native_matching::first_region_entry(
                        from_price, to_price, start, stop->price, !buy, true);
                    kind = Kind::ActivateStop;
                } else if (std::holds_alternative<native_order::StopLimitPending>(state)) {
                    const auto* sl = std::get_if<native_order::StopLimit>(&trigger);
                    if (!sl) continue;
                    hit = native_matching::first_region_entry(
                        from_price, to_price, start, sl->stop, !buy, true);
                    kind = Kind::ActivateStopLimit;
                } else if (std::holds_alternative<native_order::TrailWaitArm>(state)) {
                    const auto* trail = std::get_if<native_order::Trail>(&trigger);
                    if (!trail) continue;
                    if (!trail->arm_price) {
                        hit = start;
                    } else {
                        hit = native_matching::first_region_entry(
                            from_price, to_price, start, *trail->arm_price, buy, true);
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
                        from_price, to_price, start, track->best, trail->offset, buy);
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
                        from_price, to_price, start, level, buy, true);
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
                const bool buy = requests_.working_is_buy(*live);
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
        if (winner->kind == Kind::Evaluate) {
            native_order::Preparation<native_order::PreparedMutation> prep;
            try {
                prep = requests_.prepare_evaluation(
                    winner->handle, eval, read_target(engine, live), next_timeline_ordinal_);
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
                                                driver_class, next_timeline_ordinal_);
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
                    const bool buy = requests_.working_is_buy(*activated);
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
        const bool buy = requests_.working_is_buy(*live);
        const double slip = static_cast<double>(spec->slippage_ticks) * spec->price_tick;
        double resolved = native_matching::apply_slippage(winner->price, slip, buy);
        const auto& trigger = live->request().trigger;
        // Limit protection applies only after finite slippage arithmetic.
        // min/max must not turn an overflowed price into an executable limit.
        if (std::isfinite(resolved)
            && (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
                || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state))) {
            double level = 0.0;
            if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) level = limit->price;
            else if (const auto* sl = std::get_if<native_order::StopLimit>(&trigger)) level = sl->limit;
            resolved = native_matching::protect_limit(resolved, level, buy);
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

std::optional<NativeCurrentRefusal> NativeExecutionConsumer::validate_current_execution(
        const BacktestEngine& engine, const NativeCurrentExecution& command) const {
    using Refusal = NativeCurrentRefusal;
    if (!std::holds_alternative<NativeRunning>(state_)) return Refusal::NoExecutionContext;
    if (consuming_request_) return Refusal::Reentrant;
    if (!current_execution_point()) return Refusal::NoExecutionContext;
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
        && !std::holds_alternative<native_order::BindOpenings>(request.owner))
        return Refusal::UnsupportedRequest;
    const auto target = read_target(engine, live);
    if (std::holds_alternative<native_order::OpeningClose>(live->authority)) {
        if (!target.opening || !target.opening->has_live_matching_lot) return Refusal::UnreadyOwner;
    } else if (std::holds_alternative<native_order::OpeningsClose>(live->authority)) {
        if (target.openings.empty()) return Refusal::InvalidSelection;
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
    // A modeled-open/interior fill can be notified after confirmed input has
    // advanced the decision floor. Keep its quote and interval as cause facts,
    // but consume a newly born command at the current authorized time.
    coordinate.effective_time_ms = std::max(coordinate.effective_time_ms, decision_floor());
    return coordinate;
}

double NativeExecutionConsumer::current_price(const BacktestEngine& engine,
        const native_order::LiveRequest& live, NativeCurrentPriceRule rule) const {
    const double basis = rule == NativeCurrentPriceRule::NearestTick
        ? engine.bar_fill_price(current_frame_->point.price) : current_frame_->point.price;
    // An Independent close has not yet been bound during read-only preview.
    const bool buy = std::holds_alternative<native_order::UnboundBookClose>(live.authority)
        ? engine.position_side_ == PositionSide::SHORT : requests_.working_is_buy(live);
    const auto* spec = spec_ptr();
    return native_matching::apply_slippage(basis,
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
    const auto* host_sized = host_sized_intent(*live);
    const bool unresolved = host_sized
        && std::holds_alternative<native_order::RemainingDeferred>(live->remaining);
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
    if (terms.units && *terms.units > 0.0) {
        const auto* spec = spec_ptr();
        if (!spec || (spec->quantity_grid
                      && !native_order::quantity_on_grid(*terms.units, *spec->quantity_grid))) {
            out.terms_rejection = native_order::MatchRejectReason::InvalidTerms;
            return out;
        }
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
        const auto presented = callback_context_;
        host->on_native_applied(applied, presented);
        finish_callback(engine, notification.ordinal);
    } catch (const std::bad_alloc& e) {
        in_callback_ = false;
        current_frame_.reset();
        fail(engine, NativeFailure{NativeFailureCode::Allocation, NativeFailureOperation::Callback,
                                   notification.ordinal});
        render(engine, e.what());
    } catch (const std::exception& e) {
        in_callback_ = false;
        current_frame_.reset();
        if (!failed()) fail(engine, NativeFailure{NativeFailureCode::CallbackException,
            NativeFailureOperation::Callback, notification.ordinal});
        render(engine, e.what());
    } catch (...) {
        in_callback_ = false;
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

void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
                                              const NativeCoordinate& coordinate) {
    auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
    if (!host) return;
    callback_context_.coordinate = coordinate;
    callback_context_.decision_floor_ms = decision_floor_ms_;
    auto input = native_calendar::interval_containing(calendar_, input_tf_, bar.timestamp);
    auto script = native_calendar::interval_containing(calendar_, script_tf_, bar.timestamp);
    if (input) callback_context_.input_interval = *input;
    if (script) callback_context_.script_interval = *script;
    NativeCurrentPointView point;
    point.decision = callback_context_;
    point.price = bar.close;
    current_frame_ = CurrentExecutionFrame{point, next_timeline_ordinal_ - 1};
    in_callback_ = true;
    try {
        const NativeDecisionContext presented = callback_context_;
        host->on_native_bar(bar, presented);
    } catch (const std::exception& e) {
        in_callback_ = false;
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
        current_frame_.reset();
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render(engine, "native callback exception");
        }
        return;
    }
    finish_callback(engine, coordinate.ordinal);
}

void NativeExecutionConsumer::deliver_confirmed_script(BacktestEngine& engine, const Bar& bar,
                                                       const NativeCoordinate& base) {
    // Native AUTO is local to this input; another legacy host may have a
    // thread-local forced path installed around a nested native run.
    const bool high_first = std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
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
    const auto* spec = spec_ptr();
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        emit_discrete(bar.close, close_time, NativePriceProvenance::AfterCalculationClose,
                      NativePathPhase::Close, true);
    }
}

int64_t NativeExecutionConsumer::calculation_time(const NativeCoordinate& base) const noexcept {
    int64_t t = base.last_traded_close_ms;
    if (base.next_period_open_ms > t) t = base.next_period_open_ms;
    if (script_.latest_close_ms > t) t = script_.latest_close_ms;
    return t;
}

void NativeExecutionConsumer::deliver_aggregate_calculation(
        BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base) {
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
    }
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
        deliver_confirmed_script(engine, script_.agg, base);
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
    auto script_interval = native_calendar::interval_containing(
        calendar_, script_tf_, interval.open_ms);
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
    raise_floor(native_canonical_input_completion(interval));
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
    return !failed();
}

bool NativeExecutionConsumer::consume_confirmed_input(BacktestEngine& engine, const Bar& bar,
                                                      int index, bool last) {
    (void)last;
    processing_input_ = true;
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, bar.timestamp);
    if (!interval) {
        processing_input_ = false;
        present_refusal(engine, "native input is not aligned");
        return false;
    }
    if (!native_confirmed_bar_label_admitted(*interval, bar.timestamp)) {
        processing_input_ = false;
        present_refusal(engine, "native confirmed bar timestamp is not a canonical slot label");
        return false;
    }
    if (last_accepted_input_) {
        if (interval->open_ms <= last_accepted_input_->open_ms) {
            processing_input_ = false;
            present_refusal(engine, "native duplicate overlapping input slot");
            return false;
        }
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (running && running->phase != NativeRunPhase::Batch) {
            auto expected = native_calendar::interval_containing(
                calendar_, input_tf_, last_accepted_input_->next_input_open_ms);
            if (!expected || expected->open_ms != interval->open_ms) {
                processing_input_ = false;
                present_refusal(engine, "native stream has an in-session gap");
                return false;
            }
        }
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
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!preflight_bars(engine, bars, n, false)) return;
        const int64_t initial = n > 0 ? bars[0].timestamp
                                      : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, bars, n);
        if (failed()) return;
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
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return;
        }
        if (bar_magnifier || magnifier_samples != 4
            || magnifier_dist != MagnifierDistribution::ENDPOINTS) {
            present_refusal(engine, "native run refuses unsupported magnifier arguments");
            return;
        }
        if (!preflight_bars(engine, input_bars, n_input, false)) return;
        const int64_t initial = n_input > 0 ? input_bars[0].timestamp
                                            : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, input_bars, n_input);
        if (failed()) return;
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
                                       const Bar*, int,
                                       const std::string&, const std::string&,
                                       const std::unordered_map<std::string, std::string>&,
                                       const SymInfo&, const StrategyOverrides*,
                                       bool, int, MagnifierDistribution) {
    try {
        refuse_source_mutation("run(inputs,syminfo,overrides)");
    } catch (const std::exception& e) {
        render(engine, e.what());
        engine.last_run_status_ = 1;
    }
}

bool NativeExecutionConsumer::stream_begin(BacktestEngine& engine,
                                           const Bar* warmup_bars, int n_warmup,
                                           const std::string& input_tf,
                                           const std::string& script_tf) {
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
        if (!preflight_bars(engine, warmup_bars, n_warmup, true)) return false;
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
            present_refusal(engine, "native tick timestamp regresses the decision floor");
            return false;
        }
        if (has_array_prev && tick.timestamp < prev_array_ts) {
            present_refusal(engine, "native tick timestamps must be nondecreasing");
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
            present_refusal(engine, "native tick volume is unrepresentable");
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
    sync_history_digest();
    catch_up_timeline();
    if (ok.result.status == native_order::SubmitStatus::Accepted) {
        ++engine.next_order_incarnation_;
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
    sync_history_digest();
    catch_up_timeline();
    if (predicted_status == native_order::ReplaceStatus::Replaced) {
        ++engine.next_order_incarnation_;
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
    sync_history_digest();
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

NativePhysicalPosition NativeExecutionConsumer::position(const BacktestEngine& engine) const {
    NativePhysicalPosition out;
    out.lot_count = engine.pyramid_entries_.size();
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
    std::vector<NativeMarketEvent> out;
    const auto& history = requests_.history();
    const auto command_begin = std::upper_bound(history.begin(), history.end(), after_ordinal,
        [](uint64_t ordinal, const native_order::CommandEvent& event) {
            return ordinal < std::visit([](const auto& p) { return p.ordinal; }, event);
        });
    for (auto it = command_begin; it != history.end(); ++it) {
        const auto& event = *it;
        const uint64_t ordinal = std::visit([](const auto& p) { return p.ordinal; }, event);
        NativeMarketEvent row;
        row.kind = NativeEventKind::Command;
        row.ordinal = ordinal;
        row.command = event;
        out.push_back(row);
    }
    const auto driver_begin = std::upper_bound(driver_log_.begin(), driver_log_.end(), after_ordinal,
        [](uint64_t ordinal, const NativeDriverPoint& point) {
            return ordinal < point.coordinate.ordinal;
        });
    for (auto it = driver_begin; it != driver_log_.end(); ++it) {
        const auto& point = *it;
        NativeMarketEvent row;
        row.kind = NativeEventKind::Driver;
        row.ordinal = point.coordinate.ordinal;
        row.driver = point;
        out.push_back(row);
    }
    const auto account_begin = std::upper_bound(account_log_.begin(), account_log_.end(), after_ordinal,
        [](uint64_t ordinal, const NativeAccountObservation& account) {
            return ordinal < account.ordinal;
        });
    for (auto it = account_begin; it != account_log_.end(); ++it) {
        const auto& account = *it;
        NativeMarketEvent row;
        row.kind = NativeEventKind::Account;
        row.ordinal = account.ordinal;
        row.account = account;
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(),
              [](const NativeMarketEvent& a, const NativeMarketEvent& b) {
                  if (a.ordinal != b.ordinal) return a.ordinal < b.ordinal;
                  return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
              });
    return out;
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

std::optional<NativeCurrentPointView> NativeStrategyHost::current_execution_point() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .current_execution_point();
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

}  // inline namespace engine_script_run_v15
}  // namespace pineforge
