#include "native_journal_codec.hpp"
#include <pineforge/detail/journal_codec.hpp>
#include <new>

namespace pineforge::native_order {
inline namespace native_order_v8 {
namespace {
namespace jc = detail::journal_codec;
using Status = detail::ScratchStatus;

template<class T> struct Optional : std::false_type {};
template<class T> struct Optional<std::optional<T>> : std::true_type {};
template<class T> struct Vector : std::false_type {};
template<class T, class A> struct Vector<std::vector<T, A>> : std::true_type {};
template<class T> struct Variant : std::false_type {};
template<class... T> struct Variant<std::variant<T...>> : std::true_type {};
template<class T> struct Shared : std::false_type {};
template<class T> struct Shared<std::shared_ptr<T>> : std::true_type {};

// The same declared field order is used for counting, slicing, validation and
// diagnostic decode. Scalars are never copied as object representations.
#define EMPTY(T) template<class A> void fields(A&, T&) {}
#define VALUE(T, ...) template<class A> void fields(A& a, T& x) { a(__VA_ARGS__); }
EMPTY(Flatten) EMPTY(OwnerOpenedUnits) EMPTY(Market) EMPTY(Absolute)
EMPTY(ImmediateRemaining) EMPTY(Independent) EMPTY(NoGroup)
EMPTY(RemainingUnbound) EMPTY(RemainingFlattenAll) EMPTY(RemainingDeferred) EMPTY(NoTarget)
EMPTY(RemainingProjectionUnbound) EMPTY(RemainingProjectionFlattenAll)
EMPTY(RemainingProjectionDeferred) EMPTY(RemainingProjectionNoTarget)
EMPTY(BookTransaction) EMPTY(UnboundBookClose) EMPTY(MarketReady) EMPTY(LimitReady)
EMPTY(StopIdle) EMPTY(StopActive) EMPTY(StopLimitPending) EMPTY(StopLimitLive)
EMPTY(TrailWaitArm) EMPTY(AllowanceUnset) EMPTY(PendingNone) EMPTY(PositionFlat)
EMPTY(execution::Book)
VALUE(RunIdentity, x.session_key, x.run_number)
VALUE(RequestHandle, x.run, x.incarnation)
VALUE(EventId, x.run, x.ordinal)
VALUE(Birth, x.acceptance_ordinal, x.decision_time_lower_bound)
VALUE(CohortHandle, x.value)
VALUE(Transact, x.signed_units)
VALUE(ReverseTo, x.signed_units)
VALUE(HostSized, x.kind, x.side)
VALUE(CashValue, x.cash)
VALUE(EquityFraction, x.fraction)
VALUE(Sized, x.side, x.basis, x.time, x.price, x.grid_policy, x.reserve_percent_fee)
VALUE(ExplicitUnits, x.units)
VALUE(ScopeFraction, x.fraction, x.claim, x.basis)
VALUE(Reduce, x.size)
VALUE(Limit, x.price, x.fill_through)
VALUE(Stop, x.price)
VALUE(StopLimit, x.stop, x.limit)
VALUE(TrailTicks, x.ticks)
VALUE(Trail, x.offset, x.arm_price, x.ticks, x.best_seed)
VALUE(FromOwnerFill, x.offset, x.ticks, x.rounding)
VALUE(PointBudget, x.units)
VALUE(WaitForApplied, x.parent, x.visibility, x.first_match, x.scope)
VALUE(BindOpening, x.opening, x.cycle)
VALUE(BindOpenings, x.openings, x.cycle, x.external_openings)
VALUE(BindCohort, x.cohort)
VALUE(Member, x.group, x.cohort, x.effect)
VALUE(Request, x.intent, x.label, x.comment, x.trigger, x.capacity, x.owner, x.group, x.anchor)
VALUE(NativeCoordinate, x.ordinal, x.interval_index, x.input_interval_index,
      x.open_ms, x.eligible_open_ms, x.last_traded_close_ms, x.next_period_open_ms,
      x.next_input_open_ms, x.effective_time_ms, x.source_price_time_ms,
      x.provenance, x.path_phase, x.completion)
VALUE(MatchCursor, x.point, x.t)
VALUE(RemainingUnits, x.q)
VALUE(RemainingProjectionUnits, x.q)
VALUE(Wait, x.parent)
VALUE(ArmedTransaction, x.parent, x.cause, x.cause_cursor)
VALUE(BookClose, x.cycle, x.side, x.binding_event, x.binding_cursor)
VALUE(EnrollmentFromCommand, x.accepted)
VALUE(EnrollmentFromApplied, x.cause, x.cursor)
VALUE(OpeningClose, x.opening, x.cycle, x.side, x.enrollment)
VALUE(OpeningsClose, x.openings, x.cycle, x.side, x.enrollment, x.external_openings)
VALUE(CohortClose, x.cohort)
VALUE(SelectedExposure, x.cycle, x.incarnations)
VALUE(execution::OpeningExposure, x.incarnation, x.cycle)
VALUE(TrailTrack, x.best, x.activation_ordinal)
VALUE(TrailActive, x.best_at_trigger, x.activation_ordinal)
VALUE(AllowanceUnits, x.point_ordinal, x.initial, x.left)
VALUE(AllowanceAllScope, x.point_ordinal)
VALUE(AllowanceDeferred, x.point_ordinal)
VALUE(PendingDeferred, x.total, x.count, x.tail_receipt)
VALUE(PositionNonflat, x.cycle, x.side)
VALUE(RequestDefinition, x.handle, x.request, x.birth, x.predecessor, x.origin,
      x.root, x.priority, x.kept_binding)
VALUE(ExecutionTerms, x.resolved_price, x.units, x.shape, x.grid_policy)
VALUE(TermsResolvedInput, x.price_kind, x.shared_cursor_collision, x.raw_price,
      x.default_resolved_price, x.terms)
VALUE(AcceptedEvent, x.ordinal, x.definition, x.surface)
VALUE(RejectedEvent, x.ordinal, x.request, x.reason, x.surface)
VALUE(ReplacedEvent, x.ordinal, x.predecessor_definition, x.successor_definition, x.surface)
VALUE(ReplaceRejectedEvent, x.ordinal, x.live_definition, x.attempted, x.reason, x.surface)
VALUE(CancelledEvent, x.ordinal, x.definition, x.reason, x.cause,
      x.prior_authority, x.unexecuted, x.pending)
VALUE(NotWorkingEvent, x.ordinal, x.target, x.attempted, x.surface)
VALUE(InvalidHandleEvent, x.ordinal, x.target, x.attempted, x.surface)
VALUE(NoEffectEvent, x.ordinal, x.definition, x.remaining, x.authority, x.cursor)
VALUE(MatchRejectedEvent, x.ordinal, x.reason, x.definition, x.remaining,
      x.authority, x.cursor, x.attempted_terms)
VALUE(ExecutionAppliedEvent, x.ordinal, x.definition, x.raw_price, x.resolved_price,
      x.current_ticket, x.first_trade_index, x.closed_trade_count, x.opened_lot_incarnation,
      x.closed_units, x.opened_units, x.filled_working, x.remaining_before, x.remaining_after,
      x.allowance_before, x.allowance_after, x.terminal, x.terminal_reason, x.cycle_before,
      x.cycle_after, x.scope, x.cursor)
VALUE(CloseBoundEvent, x.ordinal, x.definition, x.cycle, x.side, x.cursor, x.before, x.after)
VALUE(ActivatedEvent, x.ordinal, x.definition, x.kind, x.before, x.after, x.reached_price, x.cursor)
VALUE(ReservationReducedEvent, x.ordinal, x.definition, x.cause, x.recipient,
      x.effect, x.requested_delta, x.actual_deduction, x.before, x.after)
VALUE(DeferredGroupAdjustmentEvent, x.ordinal, x.definition, x.cause, x.recipient,
      x.effect, x.deferred_delta, x.pending_before, x.pending_after, x.previous_pending_receipt)
VALUE(QuantityBoundEvent, x.ordinal, x.definition, x.source, x.source_units,
      x.prior_adjustment_ids, x.pending_total, x.effective_deduction, x.remaining)
VALUE(ArmedEvent, x.ordinal, x.definition, x.before, x.after, x.enrollment, x.quantity_resolution)
VALUE(TermsResolvedEvent, x.ordinal, x.definition, x.cursor, x.input,
      x.prior_adjustment_ids, x.pending_total, x.effective_deduction,
      x.remaining_before, x.remaining_after, x.allowance_after)
VALUE(MarginCallEvent, x.ordinal, x.definition, x.applied, x.cursor, x.side,
      x.mark, x.equity, x.required, x.liquidation_price, x.units, x.position_before, x.position_after)
VALUE(NativeRiskEvent, x.ordinal, x.kind, x.limit, x.observed, x.day_ordinal, x.cursor)
#undef EMPTY
#undef VALUE

struct Save {
    jc::Writer& out;
    template<class... T> void operator()(const T&... values) { (value(values), ...); }
    template<class T> void value(const T& x) {
        if (out.status != Status::ok) return;
        if constexpr (std::is_same_v<T, bool>) {
            const std::byte byte = x ? std::byte{1} : std::byte{0};
            out.bytes(&byte, 1);
        } else if constexpr (std::is_integral_v<T>) {
            out.word(static_cast<std::uint64_t>(x));
        } else if constexpr (std::is_enum_v<T>) {
            value(static_cast<std::underlying_type_t<T>>(x));
        } else if constexpr (std::is_same_v<T, double>) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &x, sizeof bits);
            out.word(bits);
        } else if constexpr (std::is_same_v<T, std::string>) {
            out.text(x);
        } else if constexpr (std::is_same_v<T, SessionKey>) {
            out.text(x.string());
        } else if constexpr (Optional<T>::value || Shared<T>::value) {
            value(bool(x));
            if (x) value(*x);
        } else if constexpr (Vector<T>::value) {
            out.word(x.size());
            for (const auto& item : x) value(item);
        } else if constexpr (Variant<T>::value) {
            out.word(x.index());
            std::visit([&](const auto& item) { value(item); }, x);
        } else if constexpr (std::is_base_of_v<OpeningReadback, T>) {
            out.word(x.count());
            for (std::uint64_t i = 0; i != x.count() && out.status == Status::ok; ++i) {
                RequestHandle handle;
                out.status = x.opening_at(i, handle);
                if (out.status != Status::ok) return;
                value(handle);
            }
        } else {
            fields(*this, const_cast<T&>(x));
        }
    }
};

class DiagnosticOpenings final : public OpeningReadback {
public:
    std::vector<RequestHandle> rows;
    std::uint64_t count() const noexcept override { return rows.size(); }
    Status opening_at(std::uint64_t index, RequestHandle& out) const noexcept override {
        if (index >= rows.size()) return Status::not_found;
        out = rows[static_cast<std::size_t>(index)];
        return Status::ok;
    }
};

template<bool Owning> struct Load {
    jc::Reader& in;
    template<class... T> void operator()(T&... values) { (value(values), ...); }
    template<std::size_t I = 0, class T> void variant(T& x, std::uint64_t index) {
        if constexpr (I < std::variant_size_v<T>) {
            if (index == I) {
                x.template emplace<I>();
                value(std::get<I>(x));
            } else variant<I + 1>(x, index);
        } else in.status = Status::corrupt;
    }
    template<class T> void value(T& x) {
        if (in.status != Status::ok) return;
        if constexpr (std::is_same_v<T, bool>) x = in.presence();
        else if constexpr (std::is_integral_v<T>) in.integer(x);
        else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            in.integer(raw);
            x = static_cast<T>(raw);
        } else if constexpr (std::is_same_v<T, double>) in.real(x);
        else if constexpr (std::is_same_v<T, std::string>) in.text(x, Owning);
        else if constexpr (std::is_same_v<T, SessionKey>) {
            std::string text;
            in.text(text, Owning);
            if constexpr (Owning) if (in.status == Status::ok) x = text;
        } else if constexpr (Optional<T>::value) {
            if (in.presence()) {
                x.emplace();
                value(*x);
            } else x.reset();
        } else if constexpr (Shared<T>::value) {
            using Item = std::remove_const_t<typename T::element_type>;
            if (!in.presence()) { x.reset(); return; }
            if constexpr (std::is_same_v<Item, OpeningReadback>) {
                const auto count = in.word();
                if (count > in.remaining() / 24) { in.status = Status::corrupt; return; }
                if constexpr (Owning) {
                    auto created = std::make_shared<DiagnosticOpenings>();
                    created->rows.resize(static_cast<std::size_t>(count));
                    for (auto& row : created->rows) value(row);
                    x = std::move(created);
                } else {
                    for (std::uint64_t i = 0; i != count && in.status == Status::ok; ++i) {
                        RequestHandle row;
                        value(row);
                    }
                }
            } else if constexpr (Owning) {
                auto created = std::make_shared<Item>();
                value(*created);
                x = std::move(created);
            } else {
                Item local{};
                value(local);
            }
        } else if constexpr (Vector<T>::value) {
            const auto count = in.word();
            using Item = typename T::value_type;
            constexpr std::uint64_t minimum =
                std::is_same_v<Item,RequestHandle> || std::is_same_v<Item,EventId> ? 24 : 8;
            if (count > in.remaining() / minimum || count > x.max_size()) {
                in.status = Status::corrupt;
                return;
            }
            if constexpr (Owning) {
                x.resize(static_cast<std::size_t>(count));
                for (auto& row : x) value(row);
            } else {
                for (std::uint64_t i = 0; i != count && in.status == Status::ok; ++i) {
                    typename T::value_type row{};
                    value(row);
                }
            }
        } else if constexpr (Variant<T>::value) variant(x, in.word());
        else fields(*this, x);
    }
};

template<bool Owning> Status decode(const detail::ScratchByteSource& source,
                                   detail::ScratchBuffer page, CommandEvent& event) noexcept {
    if (!page.data || !page.size) return Status::programming_error;
    try {
        jc::Reader in{source, page};
        Load<Owning>{in}(event);
        return in.finish();
    } catch (const std::bad_alloc&) { return Status::programming_error; }
    catch (...) { return Status::corrupt; }
}
} // namespace

JournalRecordSource::JournalRecordSource(const CommandEvent& event) noexcept : event_(&event) {
    jc::Writer out;
    Save{out}(event);
    length_ = out.position;
    status_ = out.status;
}
detail::ScratchStatus JournalRecordSource::read(std::uint64_t offset,
                                              detail::ScratchBuffer buffer) const noexcept {
    if (status_ != Status::ok) return status_;
    if (offset > length_ || buffer.size > length_ - offset || (buffer.size && !buffer.data))
        return Status::programming_error;
    jc::Writer out{0, offset, buffer};
    Save{out}(*event_);
    return out.status;
}
JournalRecordSource encode_journal_record(const CommandEvent& event) noexcept {
    return JournalRecordSource(event);
}
detail::ScratchStatus decode_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, CommandEvent& event) noexcept {
    CommandEvent result;
    const auto status = decode<true>(source, page, result);
    if (status == Status::ok) event = std::move(result);
    return status;
}
detail::ScratchStatus scan_journal_record(const detail::ScratchByteSource& source,
    detail::ScratchBuffer page, std::uint64_t& ordinal) noexcept {
    CommandEvent result;
    const auto status = decode<false>(source, page, result);
    if (status == Status::ok)
        ordinal = std::visit([](const auto& event) { return event.ordinal; }, result);
    return status;
}
} // inline namespace native_order_v8
} // namespace pineforge::native_order
