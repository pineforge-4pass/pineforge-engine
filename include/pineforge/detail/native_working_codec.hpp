#pragma once
#include <pineforge/native_order.hpp>
#include <pineforge/detail/working_store.hpp>

namespace pineforge::native_order {
inline namespace native_order_v7 {

#define PF_WORK_FIELDS(Type, ...) \
    template<class Ar> void scratch_fields(Ar& ar, Type& v) { ar(__VA_ARGS__); }
PF_WORK_FIELDS(ReverseTo, v.signed_units)
PF_WORK_FIELDS(HostSized, v.kind, v.side)
PF_WORK_FIELDS(CashValue, v.cash)
PF_WORK_FIELDS(EquityFraction, v.fraction)
PF_WORK_FIELDS(Sized, v.side, v.basis, v.time, v.price, v.grid_policy, v.reserve_percent_fee)
PF_WORK_FIELDS(ExplicitUnits, v.units)
PF_WORK_FIELDS(ScopeFraction, v.fraction, v.claim, v.basis)
PF_WORK_FIELDS(Reduce, v.size)
PF_WORK_FIELDS(Limit, v.price, v.fill_through)
PF_WORK_FIELDS(Stop, v.price)
PF_WORK_FIELDS(StopLimit, v.stop, v.limit)
PF_WORK_FIELDS(TrailTicks, v.ticks)
PF_WORK_FIELDS(Trail, v.offset, v.arm_price, v.ticks, v.best_seed)
PF_WORK_FIELDS(PointBudget, v.units)
PF_WORK_FIELDS(WaitForApplied, v.parent, v.visibility, v.first_match, v.scope)
PF_WORK_FIELDS(BindOpening, v.opening, v.cycle)
PF_WORK_FIELDS(CohortHandle, v.value)
PF_WORK_FIELDS(BindCohort, v.cohort)
PF_WORK_FIELDS(Member, v.group, v.cohort, v.effect)
PF_WORK_FIELDS(FromOwnerFill, v.offset, v.ticks, v.rounding)
PF_WORK_FIELDS(Request, v.intent, v.label, v.comment, v.trigger, v.capacity, v.owner, v.group, v.anchor)
PF_WORK_FIELDS(EventId, v.run, v.ordinal)
PF_WORK_FIELDS(MatchCursor, v.point.ordinal, v.point.effective_time_ms, v.point.interval_index,
               v.point.input_interval_index, v.point.open_ms, v.point.eligible_open_ms,
               v.point.last_traded_close_ms, v.point.next_period_open_ms, v.point.next_input_open_ms,
               v.point.source_price_time_ms, v.point.provenance, v.point.path_phase, v.point.completion, v.t)
PF_WORK_FIELDS(RemainingUnits, v.q)
PF_WORK_FIELDS(Wait, v.parent)
PF_WORK_FIELDS(ArmedTransaction, v.parent, v.cause, v.cause_cursor)
PF_WORK_FIELDS(BookClose, v.cycle, v.side, v.binding_event, v.binding_cursor)
PF_WORK_FIELDS(EnrollmentFromCommand, v.accepted)
PF_WORK_FIELDS(EnrollmentFromApplied, v.cause, v.cursor)
PF_WORK_FIELDS(OpeningClose, v.opening, v.cycle, v.side, v.enrollment)
PF_WORK_FIELDS(CohortClose, v.cohort)
PF_WORK_FIELDS(TrailTrack, v.best, v.activation_ordinal)
PF_WORK_FIELDS(TrailActive, v.best_at_trigger, v.activation_ordinal)
PF_WORK_FIELDS(AllowanceUnits, v.point_ordinal, v.initial, v.left)
PF_WORK_FIELDS(AllowanceAllScope, v.point_ordinal)
PF_WORK_FIELDS(AllowanceDeferred, v.point_ordinal)
PF_WORK_FIELDS(PendingDeferred, v.total, v.count, v.tail_receipt)
PF_WORK_FIELDS(PositionNonflat, v.cycle, v.side)
PF_WORK_FIELDS(RequestDefinition, v.handle, v.request, v.birth.acceptance_ordinal,
               v.birth.decision_time_lower_bound, v.predecessor, v.origin, v.root, v.priority,
               v.kept_binding)
PF_WORK_FIELDS(LiveRequest, v.definition, v.remaining, v.authority, v.trigger_state, v.allowance,
               v.pending, v.sizing_units, v.sizing_scope, v.sizing_price)
PF_WORK_FIELDS(CohortReceipt, v.operation, v.status, v.cohort, v.origin)
#undef PF_WORK_FIELDS

namespace working_codec {
namespace ws = ::pineforge::detail::working_store;

class StoredOpenings final : public OpeningReadback {
public:
    StoredOpenings(ws::Bytes bytes, std::uint64_t start, std::uint64_t count)
        : bytes_(bytes), start_(start), count_(count), next_offset_(start) {}
    std::uint64_t count() const noexcept override { return count_; }
    detail::ScratchStatus opening_at(std::uint64_t index, RequestHandle& out) const noexcept override {
        if (index >= count_) return detail::ScratchStatus::not_found;
        try {
            if (index < next_index_) { next_index_ = 0; next_offset_ = start_; }
            ws::Load ar(bytes_, next_offset_);
            RequestHandle value;
            while (next_index_ <= index) { ar(value); ++next_index_; }
            next_offset_ = ar.position(); out = value;
            return detail::ScratchStatus::ok;
        } catch (const detail::ScratchIoError&) { return detail::ScratchStatus::io_error; }
        catch (...) { return detail::ScratchStatus::corrupt; }
    }
private:
    ws::Bytes bytes_;
    std::uint64_t start_, count_;
    mutable std::uint64_t next_index_ = 0, next_offset_;
};

template<class Ar>
void external_openings(Ar& ar, std::shared_ptr<const OpeningReadback>& external) {
    bool present = bool(external); ar(present);
    if (!present) {
        if constexpr (!Ar::saving) external.reset();
        return;
    }
    std::uint64_t count = external ? external->count() : 0;
    ar(count);
    if constexpr (Ar::saving) {
        for (std::uint64_t i = 0; i < count; ++i) {
            RequestHandle handle;
            ws::check(external->opening_at(i, handle));
            ar(handle);
        }
    } else {
        if (count > ar.remaining() / 24) ws::check(detail::ScratchStatus::corrupt);
        const auto start = ar.position();
        for (std::uint64_t i = 0; i < count; ++i) {
            // Skip full identities without copying/interning their text.
            std::uint64_t size = 0; ar(size); ar.skip(size); ar.skip(16);
        }
        external = std::make_shared<StoredOpenings>(ar.source(), start, count);
    }
}
} // namespace working_codec

template<class Ar> void scratch_fields(Ar& ar, BindOpenings& v) {
    ar(v.openings, v.cycle);
    working_codec::external_openings(ar, v.external_openings);
}
template<class Ar> void scratch_fields(Ar& ar, OpeningsClose& v) {
    ar(v.openings, v.cycle, v.side, v.enrollment);
    working_codec::external_openings(ar, v.external_openings);
}

} // inline namespace native_order_v7
} // namespace pineforge::native_order

namespace pineforge::order_action {
template<class Ar> void scratch_fields(Ar& ar, Transact& v) { ar(v.signed_units); }
} // namespace pineforge::order_action
