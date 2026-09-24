// R5 lane L3: one value for every field of the order core's values -- a
// request, a definition, a live row, every alternative of a command event --
// so two cores (or two runs) can be held equal event for event and row for row.
//
// Unlike native_event_rows_fixture.hpp, which folds what a reader acts on,
// this folds EVERYTHING a value holds: every alternative's every member,
// optional presence, variant index, vector length and element, the full
// match cursor and its coordinate, the definition behind every DefinitionRef
// (handle, request, birth, predecessor, origin, chain root, queue priority and
// carried binding) and the run behind every handle and event id. Two values
// that differ anywhere fold to different words (up to the 64-bit fold), so
// the differential of test_native_direct_mutation.cpp compares the staged and
// the direct order core through it after every command.
//
// Source-free.
#pragma once

#include <pineforge/native_order.hpp>

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace full_fold {

namespace no = pineforge::native_order;

// One multiply-xorshift per 64-bit word: every word moves the whole state,
// and the fold is cheap enough to run after every command of a long book.
struct Fold {
    std::uint64_t h = 0x243F6A8885A308D3ull;
    void u(std::uint64_t v) {
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    void i(std::int64_t v) { u(static_cast<std::uint64_t>(v)); }
    void b(bool v) { u(v ? 1u : 0u); }
    void d(double x) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &x, sizeof bits);
        u(bits);
    }
    void s(const std::string& text) {
        u(text.size());
        std::size_t at = 0;
        for (; at + 8 <= text.size(); at += 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, text.data() + at, 8);
            u(word);
        }
        std::uint64_t tail = 0;
        std::memcpy(&tail, text.data() + at, text.size() - at);
        u(tail);
    }
    template <class T>
    void e(T value) { u(static_cast<std::uint64_t>(value)); }
};

inline void fold(Fold& f, const no::RunIdentity& run) {
    f.s(run.session_key.string());
    f.u(run.run_number);
}
inline void fold(Fold& f, const no::RequestHandle& handle) {
    fold(f, handle.run);
    f.u(handle.incarnation);
}
inline void fold(Fold& f, const no::Birth& birth) {
    f.u(birth.acceptance_ordinal);
    f.i(birth.decision_time_lower_bound);
}
inline void fold(Fold& f, const no::EventId& id) {
    fold(f, id.run);
    f.u(id.ordinal);
}
inline void fold(Fold& f, const no::CohortHandle& cohort) { f.u(cohort.value); }

template <class T>
void fold_optional(Fold& f, const std::optional<T>& value) {
    f.b(value.has_value());
    if (value) fold(f, *value);
}
inline void fold_optional_double(Fold& f, const std::optional<double>& value) {
    f.b(value.has_value());
    if (value) f.d(*value);
}
template <class T>
void fold_vector(Fold& f, const std::vector<T>& values) {
    f.u(values.size());
    for (const auto& value : values) fold(f, value);
}

inline void fold(Fold& f, const pineforge::NativeCoordinate& c) {
    f.u(c.ordinal);
    f.i(c.interval_index);
    f.i(c.open_ms);
    f.i(c.eligible_open_ms);
    f.i(c.last_traded_close_ms);
    f.i(c.next_period_open_ms);
    f.i(c.next_input_open_ms);
    f.i(c.effective_time_ms);
    f.i(c.source_price_time_ms);
    f.e(c.provenance);
    f.e(c.path_phase);
    f.e(c.completion);
}
inline void fold(Fold& f, const no::MatchCursor& cursor) {
    fold(f, cursor.point);
    f.d(cursor.t);
}

// ---- Request --------------------------------------------------------------
inline void fold(Fold& f, const no::SizeBasis& basis) {
    f.u(basis.index());
    if (const auto* cash = std::get_if<no::CashValue>(&basis)) f.d(cash->cash);
    if (const auto* equity = std::get_if<no::EquityFraction>(&basis)) f.d(equity->fraction);
}
inline void fold(Fold& f, const no::ReductionSize& size) {
    f.u(size.index());
    if (const auto* units = std::get_if<no::ExplicitUnits>(&size)) f.d(units->units);
    if (const auto* fraction = std::get_if<no::ScopeFraction>(&size)) {
        f.d(fraction->fraction);
        f.e(fraction->claim);
        f.e(fraction->basis);
    }
}
inline void fold(Fold& f, const no::OrderIntent& intent) {
    f.u(intent.index());
    if (const auto* reduce = std::get_if<no::Reduce>(&intent)) fold(f, reduce->size);
    if (const auto* transact = std::get_if<no::Transact>(&intent)) f.d(transact->signed_units);
    if (const auto* reverse = std::get_if<no::ReverseTo>(&intent)) f.d(reverse->signed_units);
    if (const auto* host = std::get_if<no::HostSized>(&intent)) {
        f.e(host->kind);
        f.b(host->side.has_value());
        if (host->side) f.e(*host->side);
    }
    if (const auto* sized = std::get_if<no::Sized>(&intent)) {
        f.e(sized->side);
        fold(f, sized->basis);
        f.e(sized->time);
        f.e(sized->price);
        f.e(sized->grid_policy);
        f.b(sized->reserve_percent_fee);
    }
}
inline void fold(Fold& f, const no::Trigger& trigger) {
    f.u(trigger.index());
    if (const auto* limit = std::get_if<no::Limit>(&trigger)) {
        f.d(limit->price);
        f.b(limit->fill_through);
    }
    if (const auto* stop = std::get_if<no::Stop>(&trigger)) f.d(stop->price);
    if (const auto* stop_limit = std::get_if<no::StopLimit>(&trigger)) {
        f.d(stop_limit->stop);
        f.d(stop_limit->limit);
    }
    if (const auto* trail = std::get_if<no::Trail>(&trigger)) {
        f.d(trail->offset);
        fold_optional_double(f, trail->arm_price);
        f.b(trail->ticks.has_value());
        if (trail->ticks) f.d(trail->ticks->ticks);
        fold_optional_double(f, trail->best_seed);
    }
}
inline void fold(Fold& f, const no::Capacity& capacity) {
    f.u(capacity.index());
    if (const auto* budget = std::get_if<no::PointBudget>(&capacity)) f.d(budget->units);
}
inline void fold(Fold& f, const no::Owner& owner) {
    f.u(owner.index());
    if (const auto* wait = std::get_if<no::WaitForApplied>(&owner)) {
        fold(f, wait->parent);
        f.e(wait->visibility);
        f.e(wait->first_match);
        f.e(wait->scope);
    }
    if (const auto* bind = std::get_if<no::BindOpening>(&owner)) {
        fold(f, bind->opening);
        f.i(bind->cycle);
    }
    if (const auto* binds = std::get_if<no::BindOpenings>(&owner)) {
        fold_vector(f, binds->openings);
        f.i(binds->cycle);
    }
    if (const auto* cohort = std::get_if<no::BindCohort>(&owner)) fold(f, cohort->cohort);
}
inline void fold(Fold& f, const no::Group& group) {
    f.u(group.index());
    if (const auto* member = std::get_if<no::Member>(&group)) {
        f.u(member->group);
        f.i(member->cohort);
        f.e(member->effect);
    }
}
inline void fold(Fold& f, const no::TriggerAnchor& anchor) {
    f.u(anchor.index());
    if (const auto* from = std::get_if<no::FromOwnerFill>(&anchor)) {
        f.d(from->offset);
        f.b(from->ticks);
        f.e(from->rounding);
    }
}
inline void fold(Fold& f, const no::Request& request) {
    fold(f, request.intent);
    f.s(request.label);
    f.s(request.comment);
    fold(f, request.trigger);
    fold(f, request.capacity);
    fold(f, request.owner);
    fold(f, request.group);
    fold(f, request.anchor);
}
inline void fold(Fold& f, const no::RequestDefinition& definition) {
    fold(f, definition.handle);
    fold(f, definition.request);
    fold(f, definition.birth);
    fold_optional(f, definition.predecessor);
    f.e(definition.origin);
    fold_optional(f, definition.root);
    // R5 lane V19-D: a re-price's queue priority and a carried book binding.
    f.u(definition.priority);
    f.b(definition.kept_binding.has_value());
    if (definition.kept_binding) {
        f.i(definition.kept_binding->cycle);
        f.e(definition.kept_binding->side);
    }
}
inline void fold(Fold& f, const no::DefinitionRef& definition) {
    f.b(static_cast<bool>(definition));
    if (definition) fold(f, *definition);
}

// ---- live state -----------------------------------------------------------
inline void fold(Fold& f, const no::Remaining& remaining) {
    f.u(remaining.index());
    if (const auto* units = std::get_if<no::RemainingUnits>(&remaining)) f.d(units->q);
}
inline void fold(Fold& f, const no::RemainingProjection& remaining) {
    f.u(remaining.index());
    if (const auto* units = std::get_if<no::RemainingProjectionUnits>(&remaining)) f.d(units->q);
}
inline void fold(Fold& f, const no::Enrollment& enrollment) {
    f.u(enrollment.index());
    if (const auto* command = std::get_if<no::EnrollmentFromCommand>(&enrollment)) {
        fold(f, command->accepted);
    }
    if (const auto* applied = std::get_if<no::EnrollmentFromApplied>(&enrollment)) {
        fold(f, applied->cause);
        fold(f, applied->cursor);
    }
}
inline void fold(Fold& f, const no::Authority& authority) {
    f.u(authority.index());
    if (const auto* wait = std::get_if<no::Wait>(&authority)) fold(f, wait->parent);
    if (const auto* armed = std::get_if<no::ArmedTransaction>(&authority)) {
        fold(f, armed->parent);
        fold(f, armed->cause);
        fold(f, armed->cause_cursor);
    }
    if (const auto* book = std::get_if<no::BookClose>(&authority)) {
        f.i(book->cycle);
        f.e(book->side);
        fold(f, book->binding_event);
        fold(f, book->binding_cursor);
    }
    if (const auto* opening = std::get_if<no::OpeningClose>(&authority)) {
        fold(f, opening->opening);
        f.i(opening->cycle);
        f.e(opening->side);
        fold(f, opening->enrollment);
    }
    if (const auto* openings = std::get_if<no::OpeningsClose>(&authority)) {
        fold_vector(f, openings->openings);
        f.i(openings->cycle);
        f.e(openings->side);
        fold(f, openings->enrollment);
    }
    if (const auto* cohort = std::get_if<no::CohortClose>(&authority)) fold(f, cohort->cohort);
}
inline void fold(Fold& f, const no::TriggerState& state) {
    f.u(state.index());
    if (const auto* track = std::get_if<no::TrailTrack>(&state)) {
        f.d(track->best);
        f.u(track->activation_ordinal);
    }
    if (const auto* active = std::get_if<no::TrailActive>(&state)) {
        f.d(active->best_at_trigger);
        f.u(active->activation_ordinal);
    }
}
inline void fold(Fold& f, const no::Allowance& allowance) {
    f.u(allowance.index());
    if (const auto* units = std::get_if<no::AllowanceUnits>(&allowance)) {
        f.u(units->point_ordinal);
        f.d(units->initial);
        f.d(units->left);
    }
    if (const auto* all = std::get_if<no::AllowanceAllScope>(&allowance)) f.u(all->point_ordinal);
    if (const auto* deferred = std::get_if<no::AllowanceDeferred>(&allowance)) {
        f.u(deferred->point_ordinal);
    }
}
inline void fold(Fold& f, const no::PendingDeferred& pending) {
    f.d(pending.total);
    f.u(pending.count);
    fold(f, pending.tail_receipt);
}
inline void fold(Fold& f, const no::PendingAdjustments& pending) {
    f.u(pending.index());
    if (const auto* deferred = std::get_if<no::PendingDeferred>(&pending)) fold(f, *deferred);
}
inline void fold(Fold& f, const no::LiveRequest& live) {
    fold(f, live.definition);
    fold(f, live.remaining);
    fold(f, live.authority);
    fold(f, live.trigger_state);
    fold(f, live.allowance);
    fold(f, live.pending);
    fold_optional_double(f, live.sizing_units);
    fold_optional_double(f, live.sizing_scope);
    fold_optional_double(f, live.sizing_price);
}

// ---- events ---------------------------------------------------------------
inline void fold(Fold& f, const no::ExecutionScope& scope) {
    f.u(scope.index());
    if (const auto* opening = std::get_if<pineforge::execution::OpeningExposure>(&scope)) {
        f.u(opening->incarnation);
        f.i(opening->cycle);
    }
    if (const auto* selected = std::get_if<no::SelectedExposure>(&scope)) {
        f.i(selected->cycle);
        f.u(selected->incarnations.size());
        for (auto incarnation : selected->incarnations) f.u(incarnation);
    }
}
inline void fold(Fold& f, const no::ExecutionTerms& terms) {
    f.d(terms.resolved_price);
    fold_optional_double(f, terms.units);
    f.e(terms.shape);
    f.e(terms.grid_policy);
}
inline void fold(Fold& f, const no::TermsResolvedInput& input) {
    f.e(input.price_kind);
    f.b(input.shared_cursor_collision);
    f.d(input.raw_price);
    f.d(input.default_resolved_price);
    fold(f, input.terms);
}

inline void fold_payload(Fold& f, const no::AcceptedEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); f.e(e.surface);
}
inline void fold_payload(Fold& f, const no::RejectedEvent& e) {
    f.u(e.ordinal); fold(f, e.request); f.e(e.reason); f.e(e.surface);
}
inline void fold_payload(Fold& f, const no::ReplacedEvent& e) {
    f.u(e.ordinal); fold(f, e.predecessor_definition); fold(f, e.successor_definition);
    f.e(e.surface);
}
inline void fold_payload(Fold& f, const no::ReplaceRejectedEvent& e) {
    f.u(e.ordinal); fold(f, e.live_definition); fold(f, e.attempted); f.e(e.reason);
    f.e(e.surface);
}
inline void fold_payload(Fold& f, const no::CancelledEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); f.e(e.reason); fold_optional(f, e.cause);
    fold(f, e.prior_authority); fold(f, e.unexecuted); fold(f, e.pending);
}
inline void fold_payload(Fold& f, const no::NotWorkingEvent& e) {
    f.u(e.ordinal); fold(f, e.target); fold_optional(f, e.attempted); f.e(e.surface);
}
inline void fold_payload(Fold& f, const no::InvalidHandleEvent& e) {
    f.u(e.ordinal); fold(f, e.target); fold_optional(f, e.attempted); f.e(e.surface);
}
inline void fold_payload(Fold& f, const no::NoEffectEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); fold(f, e.remaining); fold(f, e.authority);
    fold(f, e.cursor);
}
inline void fold_payload(Fold& f, const no::MatchRejectedEvent& e) {
    f.u(e.ordinal); f.e(e.reason); fold(f, e.definition); fold(f, e.remaining);
    fold(f, e.authority); fold(f, e.cursor); fold_optional(f, e.attempted_terms);
}
inline void fold_payload(Fold& f, const no::ExecutionAppliedEvent& e) {
    f.u(e.ordinal); fold(f, e.definition);
    f.d(e.raw_price); f.d(e.resolved_price); f.d(e.current_ticket);
    f.u(e.first_trade_index); f.u(e.closed_trade_count); f.u(e.opened_lot_incarnation);
    f.d(e.closed_units); f.d(e.opened_units); f.d(e.filled_working);
    fold(f, e.remaining_before); fold(f, e.remaining_after);
    fold(f, e.allowance_before); fold(f, e.allowance_after);
    f.b(e.terminal);
    f.b(e.terminal_reason.has_value());
    if (e.terminal_reason) f.e(*e.terminal_reason);
    f.i(e.cycle_before); f.i(e.cycle_after);
    fold(f, e.scope); fold(f, e.cursor);
}
inline void fold_payload(Fold& f, const no::CloseBoundEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); f.i(e.cycle); f.e(e.side); fold(f, e.cursor);
    fold(f, e.before); fold(f, e.after);
}
inline void fold_payload(Fold& f, const no::ActivatedEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); f.e(e.kind); fold(f, e.before); fold(f, e.after);
    f.d(e.reached_price); fold(f, e.cursor);
}
inline void fold_payload(Fold& f, const no::ReservationReducedEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); fold(f, e.cause); fold(f, e.recipient);
    f.e(e.effect); f.d(e.requested_delta); f.d(e.actual_deduction); f.d(e.before.q);
    fold(f, e.after);
}
inline void fold_payload(Fold& f, const no::DeferredGroupAdjustmentEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); fold(f, e.cause); fold(f, e.recipient);
    f.e(e.effect); f.d(e.deferred_delta); fold(f, e.pending_before); fold(f, e.pending_after);
    fold_optional(f, e.previous_pending_receipt);
}
inline void fold_payload(Fold& f, const no::QuantityBoundEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); fold(f, e.source); f.d(e.source_units);
    fold_vector(f, e.prior_adjustment_ids); f.d(e.pending_total); f.d(e.effective_deduction);
    fold(f, e.remaining);
}
inline void fold_payload(Fold& f, const no::ArmedEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); fold(f, e.before); fold(f, e.after);
    fold(f, e.enrollment); fold_optional(f, e.quantity_resolution);
}
inline void fold_payload(Fold& f, const no::TermsResolvedEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); fold(f, e.cursor); fold(f, e.input);
    fold_vector(f, e.prior_adjustment_ids); f.d(e.pending_total); f.d(e.effective_deduction);
    fold(f, e.remaining_before); fold(f, e.remaining_after); fold(f, e.allowance_after);
}
inline void fold_payload(Fold& f, const no::MarginCallEvent& e) {
    f.u(e.ordinal); fold(f, e.definition); fold(f, e.applied); fold(f, e.cursor); f.e(e.side);
    f.d(e.mark); f.d(e.equity); f.d(e.required); f.d(e.liquidation_price); f.d(e.units);
    f.d(e.position_before); f.d(e.position_after);
}
inline void fold_payload(Fold& f, const no::NativeRiskEvent& e) {
    f.u(e.ordinal); f.e(e.kind); f.d(e.limit); f.d(e.observed); f.i(e.day_ordinal);
    fold(f, e.cursor);
}
inline void fold(Fold& f, const no::CommandEvent& event) {
    f.u(event.index());
    std::visit([&](const auto& payload) { fold_payload(f, payload); }, event);
}

inline std::uint64_t value(const no::CommandEvent& event) {
    Fold f;
    fold(f, event);
    return f.h;
}
inline std::uint64_t value(const no::LiveRequest& live) {
    Fold f;
    fold(f, live);
    return f.h;
}

// The whole observable state of one core: its identity and counters, the
// journal window (base, end and every retained event), the ordinal index as
// event_at answers it, the live book row by row, the group-effect receipts,
// the chain index, and the cohort rosters and receipts.
inline std::uint64_t core_value(const no::WorkingRequestCore& core) {
    Fold f;
    fold(f, core.identity());
    f.u(core.last_ordinal());
    f.u(core.last_incarnation());
    f.u(core.history_base());
    f.u(core.history_end());
    f.u(core.retired_through());
    for (const auto& event : core.history()) {
        fold(f, event);
        const std::uint64_t ordinal =
                std::visit([](const auto& payload) { return payload.ordinal; }, event);
        const no::CommandEvent* found = core.event_at(no::EventId{core.identity(), ordinal});
        f.b(found == &event);
    }
    f.u(core.live().size());
    for (const auto& live : core.live()) fold(f, live);
    f.u(core.group_effect_receipt_count());
    for (std::size_t index = 0; index < core.group_effect_receipt_count(); ++index) {
        const auto receipt = core.group_effect_receipt(index);
        fold(f, receipt.cause);
        fold(f, receipt.recipient);
        f.e(receipt.effect);
        f.u(receipt.outcome_ordinal);
    }
    f.u(core.issued_incarnations().size());
    for (const auto& range : core.issued_incarnations()) {
        f.u(range.first);
        f.u(range.second);
    }
    f.u(core.cohorts().size());
    for (const auto& roster : core.cohorts()) {
        fold(f, roster.handle);
        fold_vector(f, roster.origins);
    }
    f.u(core.cohort_receipts().size());
    for (const auto& receipt : core.cohort_receipts()) {
        f.e(receipt.operation);
        f.e(receipt.status);
        fold(f, receipt.cohort);
        fold(f, receipt.origin);
    }
    return f.h;
}

// core_value by increments, for a caller that reads one core after every
// command of a long run. A committed event never changes -- the journal only
// appends at its back and retires a prefix -- and neither does a committed
// group-effect or cohort receipt, so each step folds those it has not folded
// yet, and in full everything that can change: the counters, the window's
// bounds, every live row, the chain index and the rosters. The words of a run
// are equal exactly when every core_value of that run would be.
class CoreTracker {
public:
    std::uint64_t step(const no::WorkingRequestCore& core) {
        Fold f;
        fold(f, core.identity());
        f.u(core.last_ordinal());
        f.u(core.last_incarnation());
        f.u(core.history_base());
        f.u(core.history_end());
        f.u(core.retired_through());
        for (std::size_t index = std::max(events_, core.history_base());
             index < core.history_end(); ++index) {
            const auto& event = core.history_at(index);
            fold(f, event);
            const std::uint64_t ordinal =
                    std::visit([](const auto& payload) { return payload.ordinal; }, event);
            f.b(core.event_at(no::EventId{core.identity(), ordinal}) == &event);
        }
        events_ = core.history_end();
        f.u(core.live().size());
        for (const auto& live : core.live()) fold(f, live);
        f.u(core.group_effect_receipt_count());
        for (std::size_t index = receipts_; index < core.group_effect_receipt_count(); ++index) {
            const auto receipt = core.group_effect_receipt(index);
            fold(f, receipt.cause);
            fold(f, receipt.recipient);
            f.e(receipt.effect);
            f.u(receipt.outcome_ordinal);
        }
        receipts_ = core.group_effect_receipt_count();
        f.u(core.issued_incarnations().size());
        for (const auto& range : core.issued_incarnations()) {
            f.u(range.first);
            f.u(range.second);
        }
        f.u(core.cohorts().size());
        for (const auto& roster : core.cohorts()) {
            fold(f, roster.handle);
            fold_vector(f, roster.origins);
        }
        const auto& cohort_receipts = core.cohort_receipts();
        f.u(cohort_receipts.size());
        for (std::size_t index = cohort_receipts_; index < cohort_receipts.size(); ++index) {
            f.e(cohort_receipts[index].operation);
            f.e(cohort_receipts[index].status);
            fold(f, cohort_receipts[index].cohort);
            fold(f, cohort_receipts[index].origin);
        }
        cohort_receipts_ = cohort_receipts.size();
        return f.h;
    }

private:
    std::size_t events_ = 0;
    std::size_t receipts_ = 0;
    std::size_t cohort_receipts_ = 0;
};

}  // namespace full_fold
