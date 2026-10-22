#pragma once

#include <pineforge/detail/native_working_codec.hpp>
#include <pineforge/exit_leg_lifecycle.hpp>
#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/reservation_expansion.hpp>
#include <pineforge/source/market_admission.hpp>

namespace pineforge::exit_legs::lifecycle_v1 {
template<class A> void scratch_fields(A& ar, Frame& v) {
    ar(v.event, v.bar, v.domain, v.phase);
}
template<class A> void scratch_fields(A& ar, Target& v) { ar(v.incarnation, v.owner); }
template<class A> void scratch_fields(A& ar, Prices& v) {
    ar(v.limit_price, v.stop_price, v.trail_points, v.trail_price,
       v.trail_offset, v.profit_ticks, v.loss_ticks);
}
template<class A> void scratch_fields(A& ar, Barrier& v) { ar(v.requested, v.target, v.revision); }
template<class A> void scratch_fields(A& ar, ObservationWindow& v) { ar(v.excluded, v.best, v.prefix); }
template<class A> void scratch_fields(A& ar, Retirement& v) { ar(v.generation, v.cause); }
template<class A> void scratch_fields(A& ar, Replacement& v) {
    ar(v.queue_predecessor, v.revival_definition, v.release);
}
template<class A> void scratch_fields(A& ar, Suspension& v) {
    ar(v.cause, v.legs, v.hold, v.revival_definition, v.replacement, v.window);
}
template<class A> void scratch_fields(A& ar, BindOwner& v) { ar(v.owner); }
template<class A> void scratch_fields(A& ar, Suspend& v) { ar(v.legs, v.hold, v.window, v.retire); }
template<class A> void scratch_fields(A& ar, StageReplacement& v) { ar(v.relation); }
template<class A> void scratch_fields(A& ar, Restore& v) { ar(v.legs); }
template<class A> void scratch_fields(A& ar, CompleteBarrier& v) { ar(v.completed, v.requested); }
template<class A> void scratch_fields(A& ar, Observe& v) { ar(v.high, v.low, v.direction, v.fold); }
template<class A> void scratch_fields(A& ar, Cancel& v) { ar(v.legs); }
template<class A> void scratch_fields(A& ar, Action& v) {
    ar(v.target, v.expected_revision, v.cause, v.operation);
}
}

namespace pineforge {
template<class A> void scratch_fields(A& ar, OrderBirth& v) {
    auto cause = v.cause(); auto bar = v.bar(); auto time = v.timestamp();
    auto domain = v.cursor().domain(); auto position = v.cursor().position();
    auto index = v.cursor().index(); auto count = v.cursor().count();
    auto price = v.cursor_price(); auto first = v.first_fill(); auto last = v.last_fill();
    auto ordinal = v.evaluation_ordinal();
    ar(cause, bar, time, domain, position, index, count, price, first, last, ordinal);
    if constexpr (!A::saving) {
        switch (cause) {
        case OrderBirthCause::Unattributed: v = {}; break;
        case OrderBirthCause::DirectCommand: v = OrderBirth::direct_command(bar, time); break;
        case OrderBirthCause::ChartEvaluation: v = OrderBirth::chart_evaluation(bar, time); break;
        case OrderBirthCause::FillEvaluation: {
            BirthCursor cursor;
            if (position == BirthCursorPosition::Point)
                cursor = BirthCursor::point(domain, index, count);
            else if (position == BirthCursorPosition::Segment)
                cursor = BirthCursor::segment(domain, index, count);
            else detail::working_store::check(detail::ScratchStatus::corrupt);
            v = OrderBirth::fill_evaluation(bar, time, cursor, price, first, last, ordinal);
            break;
        }
        default: detail::working_store::check(detail::ScratchStatus::corrupt);
        }
    }
}
template<class A> void scratch_fields(A& ar, ExitLegActivationBounds& v) {
    ar(v.position_cycle, v.stop_first_bar, v.limit_first_bar);
}
template<class A> void scratch_fields(A& ar, ExitLegActivation& v) {
    // The object being restored is mutable; the accessor lends its actual field.
    ar(const_cast<std::optional<ExitLegActivationBounds>&>(v.bounds()));
}
inline namespace reservation_expansion_v1 {
template<class A> void scratch_fields(A& ar, ReservationExpansionCapture& v) {
    ar(v.position_cycle, v.side, v.first_later_admission);
}
template<class A> void scratch_fields(A& ar, ReservationExpansion& v) {
    ar(const_cast<std::optional<ReservationExpansionCapture>&>(v.capture()));
}
template<class A> void scratch_fields(A& ar, ReservationGrowthSource& v) {
    ar(const_cast<std::optional<std::uint64_t>&>(v.reservation_owner()));
}
}
}

namespace pineforge::compat::pine {
template<class A> void scratch_fields(A& ar, LimitContinuation& v) {
    ar(v.cause, v.observed_fill_sequence);
}
template<class A> void scratch_fields(A& ar, ExitPlacementEvidence& v) {
    ar(v.position_cycle, v.entry_bar, v.direction, v.cursor_price,
       v.stop_level, v.limit_level, v.limit_continuation);
}
template<class A> void scratch_fields(A& ar, ExitActivationPolicy& v) {
    ar(const_cast<std::optional<ExitPlacementEvidence>&>(v.evidence()));
}
}

namespace pineforge::admission::market_admission_v2 {
template<class A> void scratch_fields(A& ar, Configuration& v) {
    ar(v.process_on_close, v.calc_on_fills, v.magnifier, v.fill_recalculation,
       v.scheduler, v.slippage, v.pyramiding, v.default_quantity_type,
       v.default_quantity_value, v.long_margin, v.short_margin, v.commission_value,
       v.commission_type, v.pointvalue, v.fx, v.quantity_step, v.mintick,
       v.risk_direction, v.loss_days_limit, v.drawdown_limit, v.intraday_loss_limit,
       v.position_limit, v.fill_cap_active, v.risk_halted);
}
template<class A> void scratch_fields(A& ar, PriceRequest& v) { ar(v.limit, v.stop); }
template<class A> void scratch_fields(A& ar, SizingObservation& v) {
    ar(v.quantity, v.equity, v.price, v.mark, v.fx);
}
template<class A> void scratch_fields(A& ar, CommandObservation& v) {
    ar(v.command, v.kind, v.birth, v.id, v.requested_quantity, v.quantity_type,
       v.buy, v.prices, v.oca_name, v.oca_type, v.configuration, v.bar,
       v.placement_side, v.placement_cycle, v.prior_close_quantity, v.held_quantity,
       v.held_entries, v.realized_equity, v.placement_equity, v.signal_close,
       v.quantized_fixed_quantity, v.original_sizing, v.explicit_equity, v.explicit_price);
}
template<class A> void scratch_fields(A& ar, ReviewReceipt& v) {
    ar(v.sequence, v.checkpoint, v.bar, v.target_command);
}
template<class A> void scratch_fields(A& ar, SizingRevision& v) {
    ar(v.sequence, v.cause_fill, v.bar, v.target_command);
}
template<class A> void scratch_fields(A& ar, Draft& v) {
    ar(const_cast<std::shared_ptr<const CommandObservation>&>(v.observation()),
       const_cast<std::optional<ReviewReceipt>&>(v.review()),
       const_cast<std::optional<SizingRevision>&>(v.sizing_revision()));
}
}
