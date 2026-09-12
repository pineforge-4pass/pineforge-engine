#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace pineforge::native_order {
inline namespace native_order_v2 {
namespace {

constexpr std::uint8_t kLivePush = 1;
constexpr std::uint8_t kLiveErase = 2;
constexpr std::uint8_t kLiveUpdate = 3;
constexpr std::uint8_t kLiveErasePush = 4;

template <class T>
void reserve_n(std::vector<T>& values, std::size_t n) {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    if (n == 0) return;
    if (values.size() > values.max_size() - n) {
        throw std::length_error("native working-request capacity exhausted");
    }
    const std::size_t required = values.size() + n;
    if (required <= values.capacity()) return;
    std::size_t cap = values.capacity() == 0 ? 1 : values.capacity();
    while (cap < required) {
        if (cap > values.max_size() / 2) {
            cap = values.max_size();
            break;
        }
        cap *= 2;
    }
    values.reserve(std::max(required, cap));
}

uint64_t event_ordinal(const CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

bool finite_nonzero(double value) noexcept {
    return std::isfinite(value) && value != 0.0;
}

const Flatten* as_flatten(const OrderIntent& intent) noexcept {
    return std::get_if<Flatten>(&intent);
}
const Reduce* as_reduce(const OrderIntent& intent) noexcept {
    return std::get_if<Reduce>(&intent);
}
const Transact* as_transact(const OrderIntent& intent) noexcept {
    return std::get_if<Transact>(&intent);
}

const ExplicitUnits* explicit_size(const Reduce& reduce) noexcept {
    return std::get_if<ExplicitUnits>(&reduce.size);
}

bool is_owner_opened(const Reduce& reduce) noexcept {
    return std::holds_alternative<OwnerOpenedUnits>(reduce.size);
}

double working_units(const Remaining& remaining) noexcept {
    if (const auto* units = std::get_if<RemainingUnits>(&remaining)) return units->q;
    return 0.0;
}

RemainingProjection project_remaining(const Remaining& remaining) {
    if (std::holds_alternative<RemainingUnbound>(remaining)) return RemainingProjectionUnbound{};
    if (std::holds_alternative<RemainingFlattenAll>(remaining)) {
        return RemainingProjectionFlattenAll{};
    }
    return RemainingProjectionUnits{working_units(remaining)};
}

bool checked_add_positive(double a, double b, double* out) noexcept {
    if (!std::isfinite(a) || !std::isfinite(b) || a < 0.0 || b < 0.0) return false;
    const double sum = a + b;
    if (!std::isfinite(sum)) return false;
    if (a > 0.0 && b > 0.0 && !(sum > a && sum > b)) return false;
    *out = sum;
    return true;
}

bool checked_sub_cap(double before, double deduct, double* after, bool* exhausted) noexcept {
    if (!std::isfinite(before) || !std::isfinite(deduct) || before <= 0.0 || deduct <= 0.0) {
        return false;
    }
    if (deduct == before) {
        *after = 0.0;
        *exhausted = true;
        return true;
    }
    if (deduct > before) return false;
    const double next = before - deduct;
    if (!std::isfinite(next) || !(next > 0.0 && next < before)) return false;
    *after = next;
    *exhausted = false;
    return true;
}

TriggerState trigger_state_from(const Trigger& trigger) {
    if (std::holds_alternative<Market>(trigger)) return MarketReady{};
    if (std::holds_alternative<Limit>(trigger)) return LimitReady{};
    if (std::holds_alternative<Stop>(trigger)) return StopIdle{};
    if (std::holds_alternative<StopLimit>(trigger)) return StopLimitPending{};
    return TrailWaitArm{};
}

bool fillable_state(const TriggerState& state) noexcept {
    return std::holds_alternative<MarketReady>(state) || std::holds_alternative<LimitReady>(state)
        || std::holds_alternative<StopActive>(state) || std::holds_alternative<StopLimitLive>(state)
        || std::holds_alternative<TrailActive>(state);
}

Side side_from_opened(double opened_units) noexcept {
    return opened_units < 0.0 ? Side::Short : Side::Long;
}

bool position_matches(const PositionIdentity& position, int64_t cycle, Side side) noexcept {
    const auto* nonflat = std::get_if<PositionNonflat>(&position);
    return nonflat && nonflat->cycle == cycle && nonflat->side == side && cycle > 0;
}

bool opening_alive(const OpeningObservation& observation,
                   const RequestHandle& opening,
                   int64_t cycle,
                   const Side* required_side = nullptr) noexcept {
    if (observation.queried_opening != opening || observation.queried_cycle != cycle) return false;
    if (!observation.has_live_matching_lot || cycle <= 0) return false;
    const auto* nonflat = std::get_if<PositionNonflat>(&observation.current_position);
    if (!nonflat || nonflat->cycle != cycle) return false;
    if (required_side && nonflat->side != *required_side) return false;
    return true;
}

bool book_close_alive(const TargetObservation& observation, const BookClose& close) noexcept {
    return position_matches(observation.current_position, close.cycle, close.side);
}

bool opening_close_alive(const TargetObservation& observation, const OpeningClose& close) noexcept {
    if (!observation.opening) return false;
    if (!opening_alive(*observation.opening, close.opening, close.cycle, &close.side)) return false;
    return position_matches(observation.current_position, close.cycle, close.side);
}

bool driver_class_matches_cursor(DriverEligibilityClass driver,
                                  const MatchCursor& cursor) noexcept {
    using pineforge::NativePriceProvenance;
    using pineforge::NativePathPhase;
    switch (cursor.point.provenance) {
        case NativePriceProvenance::ObservedPrint:
            return driver == DriverEligibilityClass::ObservedPrint;
        case NativePriceProvenance::CarriedOpen:
            return driver == DriverEligibilityClass::CarriedOpen;
        case NativePriceProvenance::AfterCalculationClose:
            // Completion describes the slot, not whether the input was ticks
            // or a confirmed bar. The consumer supplies that transient fact.
            return driver == DriverEligibilityClass::TickAfterCalculation
                || driver == DriverEligibilityClass::ConfirmedAfterCalculationClose;
        case NativePriceProvenance::Confirmed:
            if (cursor.point.path_phase == NativePathPhase::High
                || cursor.point.path_phase == NativePathPhase::Low
                || cursor.point.path_phase == NativePathPhase::Close) {
                return driver == DriverEligibilityClass::ConfirmedExcursion;
            }
            return driver == DriverEligibilityClass::ConfirmedOpen;
        case NativePriceProvenance::ModeledOHLCOpen:
            return driver == DriverEligibilityClass::ConfirmedOpen;
        case NativePriceProvenance::ModeledOHLCClose:
            return driver == DriverEligibilityClass::ConfirmedExcursion
                || driver == DriverEligibilityClass::ConfirmedAfterCalculationClose;
        case NativePriceProvenance::PartialFinalized:
        case NativePriceProvenance::Calculation:
            return driver == DriverEligibilityClass::ConfirmedAfterCalculationClose;
    }
    return false;
}

const MatchCursor& transition_cursor(const TriggerTransition& transition) noexcept {
    return std::visit([](const auto& payload) -> const MatchCursor& { return payload.cursor; },
                      transition);
}

bool stop_price_reached(bool is_buy, double level, double reached) noexcept {
    if (!std::isfinite(reached) || !finite_positive(level)) return false;
    return is_buy ? reached >= level : reached <= level;
}

bool same_p_continuation(const LiveRequest& live, const MatchCursor& cursor) noexcept {
    if (cursor.point.ordinal == 0) return false;
    if (cursor.point.effective_time_ms < live.birth().decision_time_lower_bound) return false;
    auto same = [&](const MatchCursor& cause) noexcept {
        return cause.point.ordinal == cursor.point.ordinal;
    };
    if (const auto* armed = std::get_if<ArmedTransaction>(&live.authority)) {
        return same(armed->cause_cursor);
    }
    if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        if (const auto* from = std::get_if<EnrollmentFromApplied>(&close->enrollment)) {
            return same(from->cursor);
        }
    }
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        return same(close->binding_cursor);
    }
    return false;
}

bool trigger_cursor_eligible(const LiveRequest& live, const MatchCursor& cursor) noexcept {
    if (point_eligible(live.birth(), cursor.point.ordinal, cursor.point.effective_time_ms)) {
        return true;
    }
    return same_p_continuation(live, cursor);
}

const ExecutionAppliedEvent* as_applied(const CommandEvent& event) noexcept {
    return std::get_if<ExecutionAppliedEvent>(&event);
}

int receipt_cmp(uint64_t oa, uint64_t ia, GroupEffect ea, uint64_t ob, uint64_t ib,
                GroupEffect eb) noexcept {
    if (oa < ob) return -1;
    if (oa > ob) return 1;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    const auto a = static_cast<std::uint8_t>(ea);
    const auto b = static_cast<std::uint8_t>(eb);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

std::optional<RequestRejectReason> validate_levels(const Trigger& trigger) {
    if (const auto* limit = std::get_if<Limit>(&trigger)) {
        if (!finite_positive(limit->price)) return RequestRejectReason::InvalidTrigger;
        return std::nullopt;
    }
    if (const auto* stop = std::get_if<Stop>(&trigger)) {
        if (!finite_positive(stop->price)) return RequestRejectReason::InvalidTrigger;
        return std::nullopt;
    }
    if (const auto* stop_limit = std::get_if<StopLimit>(&trigger)) {
        if (!finite_positive(stop_limit->stop) || !finite_positive(stop_limit->limit)) {
            return RequestRejectReason::InvalidTrigger;
        }
        return std::nullopt;
    }
    if (const auto* trail = std::get_if<Trail>(&trigger)) {
        if (!finite_positive(trail->offset)) return RequestRejectReason::InvalidTrigger;
        if (trail->arm_price && !finite_positive(*trail->arm_price)) {
            return RequestRejectReason::InvalidTrigger;
        }
    }
    return std::nullopt;
}

bool on_optional_grid(double value, std::optional<double> grid) {
    return !grid || quantity_on_grid(value, *grid);
}

void fill_applied_cursor(ExecutionAppliedEvent& event, const MatchCursor& cursor) {
    event.cursor = cursor;
}

CancelledEvent make_cancelled(uint64_t ordinal, const LiveRequest& live, CancelReason reason,
                              std::optional<EventId> cause) {
    CancelledEvent event;
    event.ordinal = ordinal;
    event.definition = live.definition;
    event.reason = reason;
    event.cause = std::move(cause);
    event.prior_authority = live.authority;
    event.unexecuted = project_remaining(live.remaining);
    event.pending = live.pending;
    return event;
}

}  // namespace

struct PreparedSubmit::Impl {
    WorkingRequestCore::MutationPlan plan;
    SubmitResult result;
};
PreparedSubmit::PreparedSubmit() noexcept = default;
PreparedSubmit::PreparedSubmit(PreparedSubmit&&) noexcept = default;
PreparedSubmit& PreparedSubmit::operator=(PreparedSubmit&&) noexcept = default;
PreparedSubmit::PreparedSubmit(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedSubmit::~PreparedSubmit() = default;
PreparedSubmit::operator bool() const noexcept { return static_cast<bool>(impl_); }
const SubmitResult& PreparedSubmit::predicted() const {
    if (!impl_) throw std::logic_error("empty prepared submit");
    return impl_->result;
}
EventId PreparedSubmit::predicted_event_id() const {
    if (!impl_) throw std::logic_error("empty prepared submit");
    return EventId{impl_->plan.run, impl_->result.event_ordinal};
}

struct PreparedReplace::Impl {
    WorkingRequestCore::MutationPlan plan;
    ReplaceResult result;
};
PreparedReplace::PreparedReplace() noexcept = default;
PreparedReplace::PreparedReplace(PreparedReplace&&) noexcept = default;
PreparedReplace& PreparedReplace::operator=(PreparedReplace&&) noexcept = default;
PreparedReplace::PreparedReplace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedReplace::~PreparedReplace() = default;
PreparedReplace::operator bool() const noexcept { return static_cast<bool>(impl_); }
const ReplaceResult& PreparedReplace::predicted() const {
    if (!impl_) throw std::logic_error("empty prepared replace");
    return impl_->result;
}
EventId PreparedReplace::predicted_event_id() const {
    if (!impl_) throw std::logic_error("empty prepared replace");
    return EventId{impl_->plan.run, impl_->result.event_ordinal};
}

struct PreparedCancel::Impl {
    WorkingRequestCore::MutationPlan plan;
    CancelResult result;
};
PreparedCancel::PreparedCancel() noexcept = default;
PreparedCancel::PreparedCancel(PreparedCancel&&) noexcept = default;
PreparedCancel& PreparedCancel::operator=(PreparedCancel&&) noexcept = default;
PreparedCancel::PreparedCancel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedCancel::~PreparedCancel() = default;
PreparedCancel::operator bool() const noexcept { return static_cast<bool>(impl_); }
const CancelResult& PreparedCancel::predicted() const {
    if (!impl_) throw std::logic_error("empty prepared cancel");
    return impl_->result;
}
EventId PreparedCancel::predicted_event_id() const {
    if (!impl_) throw std::logic_error("empty prepared cancel");
    return EventId{impl_->plan.run, impl_->result.event_ordinal};
}

struct PreparedMutation::Impl {
    WorkingRequestCore::MutationPlan plan;
};
PreparedMutation::PreparedMutation() noexcept = default;
PreparedMutation::PreparedMutation(PreparedMutation&&) noexcept = default;
PreparedMutation& PreparedMutation::operator=(PreparedMutation&&) noexcept = default;
PreparedMutation::PreparedMutation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedMutation::~PreparedMutation() = default;
PreparedMutation::operator bool() const noexcept { return static_cast<bool>(impl_); }

struct PreparedExecution::Impl {
    WorkingRequestCore::MutationPlan terminal;
    CommandEvent retain_event{ExecutionAppliedEvent{}};
    LiveRequest retain_live{};
    bool can_retain = false;
    bool flatten = false;
    bool opening = false;
    BookClose book_close{};
    OpeningClose opening_close{};
    ExecutionProposal proposal{};
};
PreparedExecution::PreparedExecution() noexcept = default;
PreparedExecution::PreparedExecution(PreparedExecution&&) noexcept = default;
PreparedExecution& PreparedExecution::operator=(PreparedExecution&&) noexcept = default;
PreparedExecution::PreparedExecution(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedExecution::~PreparedExecution() = default;
PreparedExecution::operator bool() const noexcept { return static_cast<bool>(impl_); }

void WorkingRequestCore::require_identity(const RunIdentity& identity) {
    if (identity.session_key.empty() || identity.run_number == 0) {
        throw std::invalid_argument(
                "native run identity requires a nonempty session key and positive run number");
    }
}

void WorkingRequestCore::require_distinct_counters(uint64_t& next_order_incarnation,
                                                   uint64_t& next_timeline_ordinal) {
    if (&next_order_incarnation == &next_timeline_ordinal) {
        throw std::invalid_argument(
                "native order incarnation and timeline counters must be distinct");
    }
}

void WorkingRequestCore::require_grid(std::optional<double> quantity_grid) {
    if (!quantity_grid) return;
    if (!std::isfinite(*quantity_grid) || *quantity_grid <= 0.0) {
        throw std::invalid_argument("quantity grid requires a finite positive step");
    }
}

uint64_t WorkingRequestCore::usable_ordinal(uint64_t next) const {
    if (next == 0) {
        throw std::invalid_argument("native timeline ordinal must be nonzero");
    }
    if (next == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native timeline ordinal exhausted");
    }
    if (next <= last_ordinal_) {
        throw std::invalid_argument("native timeline ordinal reused or regressed");
    }
    return next;
}

uint64_t WorkingRequestCore::usable_incarnation(uint64_t next) const {
    if (next == 0) {
        throw std::invalid_argument("native order incarnation must be nonzero");
    }
    if (next == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native order incarnation exhausted");
    }
    if (next <= last_incarnation_) {
        throw std::invalid_argument("native order incarnation reused or regressed");
    }
    return next;
}

bool WorkingRequestCore::bump_epoch() noexcept {
    if (epoch_ == std::numeric_limits<uint64_t>::max()) return false;
    ++epoch_;
    return true;
}

void WorkingRequestCore::require_epoch_room() const {
    if (epoch_ >= std::numeric_limits<uint64_t>::max() - 1) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
}

void WorkingRequestCore::clear_unbound() noexcept {
    identity_.session_key.clear();
    identity_.run_number = 0;
    live_.clear();
    history_.clear();
    last_ordinal_ = 0;
    last_incarnation_ = 0;
    ordinal_index_.clear();
    receipts_.clear();
    epoch_ = 0;
    if (instance_) instance_->expired = true;
    instance_.reset();
}

void WorkingRequestCore::bind_plan(MutationPlan& plan) const {
    plan.instance = instance_;
    plan.generation = instance_ ? instance_->generation : 0;
}

WorkingRequestCore::MutationPlan WorkingRequestCore::begin_plan() const {
    require_identity(identity_);
    if (!instance_ || instance_->expired) {
        throw std::invalid_argument(
                "native run identity requires a nonempty session key and positive run number");
    }
    require_epoch_room();
    MutationPlan plan;
    plan.run = identity_;
    plan.history_size = history_.size();
    plan.last_ordinal = last_ordinal_;
    bind_plan(plan);
    plan.epoch = epoch_ + 1;
    return plan;
}

void WorkingRequestCore::seal_plan(MutationPlan& plan) {
    require_epoch_room();
    reserve_plan(plan);
    if (!bump_epoch()) {
        throw std::overflow_error("native working-request epoch exhausted");
    }
    plan.epoch = epoch_;
    bind_plan(plan);
}

void WorkingRequestCore::reserve_plan(const MutationPlan& plan) {
    reserve_n(history_, plan.events.size());
    reserve_n(ordinal_index_, plan.events.size());
    if (plan.live_change == kLivePush || plan.live_change == kLiveErasePush) reserve_n(live_, 1);
    if (plan.add_receipt) reserve_n(receipts_, 1);
}

PreparedMutation WorkingRequestCore::finish_mutation(MutationPlan plan) {
    seal_plan(plan);
    auto impl = std::make_unique<PreparedMutation::Impl>();
    impl->plan = std::move(plan);
    return PreparedMutation(std::move(impl));
}

WorkingRequestCore::TargetKind WorkingRequestCore::classify(
        const RequestHandle& handle, std::size_t* live_index) const {
    if (handle.incarnation == 0 || handle.run.run_number == 0 || handle.run.session_key.empty()
        || handle.run != identity_) {
        return TargetKind::InvalidHandle;
    }
    for (std::size_t i = 0; i < live_.size(); ++i) {
        if (live_[i].handle().incarnation == handle.incarnation) {
            if (live_index) *live_index = i;
            return TargetKind::Live;
        }
    }
    return TargetKind::NotWorking;
}

const LiveRequest* WorkingRequestCore::find_live(const RequestHandle& handle) const {
    std::size_t index = 0;
    if (classify(handle, &index) != TargetKind::Live) return nullptr;
    return &live_[index];
}

const CommandEvent* WorkingRequestCore::event_at(const EventId& id) const {
    if (id.run != identity_ || id.ordinal == 0) return nullptr;
    const auto it = std::lower_bound(
            ordinal_index_.begin(), ordinal_index_.end(), id.ordinal,
            [](const std::pair<uint64_t, std::size_t>& row, uint64_t ordinal) {
                return row.first < ordinal;
            });
    if (it == ordinal_index_.end() || it->first != id.ordinal) return nullptr;
    if (it->second >= history_.size()) return nullptr;
    const CommandEvent* event = &history_[it->second];
    if (event_ordinal(*event) != id.ordinal) return nullptr;
    return event;
}

bool WorkingRequestCore::authenticate_receipt_outcome(const CommandEvent& event,
                                                      const EventId& cause,
                                                      const RequestHandle& recipient,
                                                      GroupEffect effect) const {
    if (const auto* reduced = std::get_if<ReservationReducedEvent>(&event)) {
        return effect == GroupEffect::Reduce && reduced->cause == cause
            && reduced->recipient == recipient && reduced->effect == GroupEffect::Reduce
            && reduced->definition && reduced->definition->handle == recipient;
    }
    if (const auto* deferred = std::get_if<DeferredGroupAdjustmentEvent>(&event)) {
        return effect == GroupEffect::Reduce && deferred->cause == cause
            && deferred->recipient == recipient && deferred->effect == GroupEffect::Reduce
            && deferred->definition && deferred->definition->handle == recipient;
    }
    if (const auto* cancelled = std::get_if<CancelledEvent>(&event)) {
        const bool allowed_effect = effect == GroupEffect::Cancel
            || (effect == GroupEffect::Reduce
                && std::holds_alternative<RemainingProjectionFlattenAll>(cancelled->unexecuted)
                && cancelled->definition
                && as_flatten(cancelled->definition->request.intent));
        return allowed_effect && cancelled->reason == CancelReason::Group
            && cancelled->cause == cause && cancelled->definition
            && cancelled->definition->handle == recipient;
    }
    return false;
}

bool WorkingRequestCore::collect_pending_chain(const PendingAdjustments& pending,
                                               const RequestHandle& recipient,
                                               std::vector<EventId>* ids,
                                               double* total) const {
    ids->clear();
    *total = 0.0;
    const auto* deferred = std::get_if<PendingDeferred>(&pending);
    if (!deferred) return true;
    if (deferred->count == 0 || deferred->tail_receipt.ordinal == 0
        || deferred->tail_receipt.run != identity_) {
        return false;
    }
    EventId cursor = deferred->tail_receipt;
    for (uint64_t i = 0; i < deferred->count; ++i) {
        const CommandEvent* event = event_at(cursor);
        const auto* payload = event ? std::get_if<DeferredGroupAdjustmentEvent>(event) : nullptr;
        if (!payload || payload->recipient != recipient || payload->effect != GroupEffect::Reduce
            || !payload->definition || payload->definition->handle != recipient) {
            return false;
        }
        ids->push_back(cursor);
        if (i + 1 < deferred->count) {
            if (!payload->previous_pending_receipt
                || payload->previous_pending_receipt->ordinal >= cursor.ordinal) return false;
            cursor = *payload->previous_pending_receipt;
        } else if (payload->previous_pending_receipt) {
            return false;
        }
    }
    if (ids->size() != deferred->count) return false;
    std::reverse(ids->begin(), ids->end());
    // Reproduce the exact accumulation order used when each receipt was
    // committed. Reassociating these additions tail-first changes binary64
    // totals for valid chains such as 0.1, 0.2, 0.3.
    double sum = 0.0;
    for (std::size_t i = 0; i < ids->size(); ++i) {
        const CommandEvent* event = event_at((*ids)[i]);
        const auto* payload = event ? std::get_if<DeferredGroupAdjustmentEvent>(event) : nullptr;
        if (!payload || !finite_positive(payload->deferred_delta)) return false;
        if (i == 0) {
            if (!std::holds_alternative<PendingNone>(payload->pending_before)) return false;
        } else {
            const auto* before = std::get_if<PendingDeferred>(&payload->pending_before);
            if (!before || before->total != sum || before->count != i
                || before->tail_receipt != (*ids)[i - 1]) return false;
        }
        if (!checked_add_positive(sum, payload->deferred_delta, &sum)) return false;
        if (payload->pending_after.total != sum || payload->pending_after.count != i + 1
            || payload->pending_after.tail_receipt != (*ids)[i]) return false;
    }
    if (sum != deferred->total) return false;
    *total = sum;
    return true;
}

WorkingRequestCore::ReceiptLookup WorkingRequestCore::receipt_lookup(
        const EventId& cause, const RequestHandle& recipient, GroupEffect effect,
        uint64_t* outcome) const {
    ReceiptKey needle{cause, recipient, effect, 0};
    const auto it = std::lower_bound(
            receipts_.begin(), receipts_.end(), needle,
            [](const ReceiptKey& a, const ReceiptKey& b) {
                return receipt_cmp(a.cause.ordinal, a.recipient.incarnation, a.effect,
                                   b.cause.ordinal, b.recipient.incarnation, b.effect)
                    < 0;
            });
    if (it == receipts_.end()) return ReceiptLookup::Absent;
    if (it->cause != cause || it->recipient != recipient || it->effect != effect) {
        return ReceiptLookup::Absent;
    }
    const CommandEvent* event = event_at(EventId{identity_, it->outcome_ordinal});
    if (!event || !authenticate_receipt_outcome(*event, cause, recipient, effect)) {
        return ReceiptLookup::Conflict;
    }
    if (outcome) *outcome = it->outcome_ordinal;
    return ReceiptLookup::Present;
}

std::optional<InstallError> WorkingRequestCore::validate_plan(const MutationPlan& plan) const noexcept {
    if (plan.consumed) return InstallError::AlreadyConsumed;
    const auto locked = plan.instance.lock();
    if (!locked || locked != instance_ || !instance_ || instance_->expired
        || plan.generation != instance_->generation || plan.run != identity_) {
        return InstallError::WrongCoreOrRun;
    }
    if (plan.epoch != epoch_ || plan.history_size != history_.size()
        || plan.last_ordinal != last_ordinal_
        || epoch_ == std::numeric_limits<uint64_t>::max()) {
        return InstallError::StalePreparation;
    }
    return std::nullopt;
}

bool WorkingRequestCore::trail_level_ok(double best, double offset, bool is_buy,
                                        double* stop) const noexcept {
    if (!finite_positive(offset) || !std::isfinite(best)) return false;
    const double level = is_buy ? best + offset : best - offset;
    if (!std::isfinite(level)) return false;
    if (is_buy && !(level > best)) return false;
    if (!is_buy && !(level < best)) return false;
    if (stop) *stop = level;
    return true;
}

InstallResult WorkingRequestCore::commit(MutationPlan& plan) noexcept {
    if (const auto error = validate_plan(plan)) return *error;
    const std::size_t first = history_.size();
    const std::size_t count = plan.events.size();
    for (auto& event : plan.events) {
        const uint64_t ordinal = event_ordinal(event);
        history_.push_back(std::move(event));
        ordinal_index_.push_back({ordinal, history_.size() - 1});
        last_ordinal_ = ordinal;
    }
    if (plan.live_change == kLivePush) {
        live_.push_back(std::move(plan.live_row));
    } else if (plan.live_change == kLiveErase) {
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(plan.live_index));
    } else if (plan.live_change == kLiveUpdate) {
        live_[plan.live_index] = std::move(plan.live_row);
    } else if (plan.live_change == kLiveErasePush) {
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(plan.live_index));
        live_.push_back(std::move(plan.live_row));
    }
    if (plan.add_receipt) {
        receipts_.push_back(ReceiptKey{std::move(plan.receipt_cause), std::move(plan.receipt_recipient),
                                       plan.receipt_effect, plan.receipt_outcome});
    }
    if (plan.consume_incarnation) last_incarnation_ = plan.incarnation_used;
    plan.consumed = true;
    bump_epoch();
    return Installed{EventRange{first, count}};
}

WorkingRequestCore::WorkingRequestCore(RunIdentity identity) {
    require_identity(identity);
    identity_ = std::move(identity);
    instance_ = std::make_shared<InstanceBinding>();
}

WorkingRequestCore::WorkingRequestCore(WorkingRequestCore&& other) noexcept {
    *this = std::move(other);
}

WorkingRequestCore& WorkingRequestCore::operator=(WorkingRequestCore&& other) noexcept {
    if (this == &other) return *this;
    identity_ = std::move(other.identity_);
    live_ = std::move(other.live_);
    history_ = std::move(other.history_);
    last_ordinal_ = other.last_ordinal_;
    last_incarnation_ = other.last_incarnation_;
    epoch_ = other.epoch_;
    ordinal_index_ = std::move(other.ordinal_index_);
    receipts_ = std::move(other.receipts_);
    instance_ = std::move(other.instance_);
    if (instance_) {
        if (instance_->generation != std::numeric_limits<uint64_t>::max()) {
            ++instance_->generation;
        } else {
            instance_->expired = true;
        }
    }
    other.identity_.session_key.clear();
    other.identity_.run_number = 0;
    other.live_.clear();
    other.history_.clear();
    other.last_ordinal_ = 0;
    other.last_incarnation_ = 0;
    other.epoch_ = 0;
    other.ordinal_index_.clear();
    other.receipts_.clear();
    other.instance_.reset();
    return *this;
}

void WorkingRequestCore::reset(RunIdentity identity) {
    require_identity(identity);
    WorkingRequestCore next(std::move(identity));
    *this = std::move(next);
}

std::optional<RequestRejectReason> WorkingRequestCore::validate_request(
        const Request& request,
        const CommandContext& context,
        const std::optional<RequestHandle>& replace_target) const {
    require_grid(context.quantity_grid);
    if (as_flatten(request.intent)) {
    } else if (const auto* reduce = as_reduce(request.intent)) {
        if (const auto* units = explicit_size(*reduce)) {
            if (!finite_positive(units->units)) return RequestRejectReason::InvalidQuantity;
            if (!on_optional_grid(units->units, context.quantity_grid)) {
                return RequestRejectReason::OffGrid;
            }
        }
    } else if (const auto* transact = as_transact(request.intent)) {
        if (!finite_nonzero(transact->signed_units)) return RequestRejectReason::InvalidQuantity;
        if (!on_optional_grid(transact->signed_units, context.quantity_grid)) {
            return RequestRejectReason::OffGrid;
        }
    } else {
        return RequestRejectReason::InvalidQuantity;
    }

    const bool market_only = context.surface == CommandSurface::MarketOnly;
    if (market_only && !std::holds_alternative<Market>(request.trigger)) {
        return RequestRejectReason::InvalidTrigger;
    }
    if (const auto reason = validate_levels(request.trigger)) return reason;

    const bool flatten = as_flatten(request.intent) != nullptr;
    const bool owner_opened =
            as_reduce(request.intent) && is_owner_opened(*as_reduce(request.intent));
    if (market_only && !std::holds_alternative<ImmediateRemaining>(request.capacity)) {
        return RequestRejectReason::InvalidCapacity;
    }
    if (const auto* budget = std::get_if<PointBudget>(&request.capacity)) {
        if (flatten) return RequestRejectReason::InvalidCapacity;
        if (!finite_positive(budget->units)) return RequestRejectReason::InvalidCapacity;
        if (!on_optional_grid(budget->units, context.quantity_grid)) {
            return RequestRejectReason::OffGrid;
        }
    }

    if (market_only && !std::holds_alternative<Independent>(request.owner)) {
        return RequestRejectReason::InvalidOwner;
    }
    if (std::holds_alternative<Independent>(request.owner)) {
    } else if (const auto* wait = std::get_if<WaitForApplied>(&request.owner)) {
        if (replace_target && wait->parent == *replace_target) {
            return RequestRejectReason::InvalidOwner;
        }
        if (!find_live(wait->parent)) return RequestRejectReason::InvalidOwner;
    } else if (const auto* bind = std::get_if<BindOpening>(&request.owner)) {
        if (as_transact(request.intent)) return RequestRejectReason::InvalidOwner;
        if (bind->cycle <= 0 || bind->opening.incarnation == 0 || bind->opening.run != identity_) {
            return RequestRejectReason::InvalidOwner;
        }
        if (!context.opening) return RequestRejectReason::InvalidOwner;
        const auto& observation = *context.opening;
        if (!opening_alive(observation, bind->opening, bind->cycle)) {
            return RequestRejectReason::InvalidOwner;
        }
        if (std::holds_alternative<PositionFlat>(observation.current_position)) {
            return RequestRejectReason::InvalidOwner;
        }
    } else {
        return RequestRejectReason::InvalidOwner;
    }

    if (owner_opened) {
        const bool wait_reduce = std::holds_alternative<WaitForApplied>(request.owner)
                && as_reduce(request.intent);
        if (!wait_reduce) return RequestRejectReason::InvalidQuantityBasis;
    }

    if (market_only && !std::holds_alternative<NoGroup>(request.group)) {
        return RequestRejectReason::InvalidGroup;
    }
    if (const auto* member = std::get_if<Member>(&request.group)) {
        if (member->group == 0) return RequestRejectReason::InvalidGroup;
        if (member->effect != GroupEffect::Cancel && member->effect != GroupEffect::Reduce) {
            return RequestRejectReason::InvalidGroup;
        }
    }
    return std::nullopt;
}

LiveRequest WorkingRequestCore::make_live(DefinitionRef definition, const CommandContext& context,
                                          EventId accepted) const {
    LiveRequest live;
    live.definition = std::move(definition);
    const Request& request = live.request();
    live.trigger_state = trigger_state_from(request.trigger);
    live.allowance = AllowanceUnset{};
    live.pending = PendingNone{};
    if (std::holds_alternative<Independent>(request.owner)) {
        if (const auto* transact = as_transact(request.intent)) {
            live.remaining = RemainingUnits{std::abs(transact->signed_units)};
            live.authority = BookTransaction{};
        } else if (const auto* reduce = as_reduce(request.intent)) {
            live.remaining = RemainingUnits{explicit_size(*reduce)->units};
            live.authority = UnboundBookClose{};
        } else {
            live.remaining = RemainingFlattenAll{};
            live.authority = UnboundBookClose{};
        }
        return live;
    }
    if (const auto* wait = std::get_if<WaitForApplied>(&request.owner)) {
        live.authority = Wait{wait->parent};
        if (const auto* transact = as_transact(request.intent)) {
            live.remaining = RemainingUnits{std::abs(transact->signed_units)};
        } else if (const auto* reduce = as_reduce(request.intent)) {
            if (is_owner_opened(*reduce)) live.remaining = RemainingUnbound{};
            else live.remaining = RemainingUnits{explicit_size(*reduce)->units};
        } else {
            live.remaining = RemainingFlattenAll{};
        }
        return live;
    }
    const auto& bind = std::get<BindOpening>(request.owner);
    const auto& nonflat = std::get<PositionNonflat>(context.opening->current_position);
    OpeningClose close;
    close.opening = bind.opening;
    close.cycle = bind.cycle;
    close.side = nonflat.side;
    close.enrollment = EnrollmentFromCommand{accepted};
    live.authority = close;
    if (const auto* reduce = as_reduce(request.intent)) {
        live.remaining = RemainingUnits{explicit_size(*reduce)->units};
    } else {
        live.remaining = RemainingFlattenAll{};
    }
    return live;
}

bool WorkingRequestCore::trigger_permits_driver(const Trigger& trigger,
                                                const TriggerState& state,
                                                DriverEligibilityClass driver_class,
                                                bool existing_matching_bit) const noexcept {
    const bool market = std::holds_alternative<Market>(trigger)
            && !std::holds_alternative<StopActive>(state)
            && !std::holds_alternative<TrailActive>(state);
    switch (driver_class) {
        case DriverEligibilityClass::ObservedPrint:
            return market ? existing_matching_bit : true;
        case DriverEligibilityClass::CarriedOpen:
            if (market) return existing_matching_bit;
            return std::holds_alternative<Limit>(trigger);
        case DriverEligibilityClass::TickAfterCalculation:
            if (market) return existing_matching_bit;
            return true;
        case DriverEligibilityClass::ConfirmedOpen:
            return market ? existing_matching_bit : true;
        case DriverEligibilityClass::ConfirmedExcursion:
            return !market;
        case DriverEligibilityClass::ConfirmedAfterCalculationClose:
            return market ? existing_matching_bit : false;
    }
    return false;
}

bool WorkingRequestCore::working_is_buy(const LiveRequest& live) const noexcept {
    if (const auto* transact = as_transact(live.request().intent)) {
        return transact->signed_units > 0.0;
    }
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        return close->side == Side::Short;
    }
    if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        return close->side == Side::Short;
    }
    return false;
}

EligibilityFacts WorkingRequestCore::eligibility_facts(
        const LiveRequest& live, const EvaluationContext& context) const noexcept {
    EligibilityFacts facts;
    facts.trigger = &live.request().trigger;
    facts.trigger_state = &live.trigger_state;
    facts.remaining = &live.remaining;
    facts.allowance = &live.allowance;
    facts.authority = &live.authority;
    facts.waiting = std::holds_alternative<Wait>(live.authority);
    facts.needs_close_bind = std::holds_alternative<UnboundBookClose>(live.authority);
    facts.birth_ok = point_eligible(live.birth(), context.cursor.point.ordinal,
                                    context.cursor.point.effective_time_ms);
    if (facts.waiting) {
        facts.driver_ok = false;
        facts.ready_to_match = false;
        facts.is_buy = working_is_buy(live);
        return facts;
    }
    facts.driver_ok = trigger_permits_driver(live.request().trigger, live.trigger_state,
                                            context.driver_class, context.existing_matching_bit);
    facts.is_buy = working_is_buy(live);
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        facts.position_side = close->side;
    } else if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        facts.position_side = close->side;
    }
    facts.ready_to_match = facts.birth_ok && facts.driver_ok && !facts.waiting;
    return facts;
}

bool WorkingRequestCore::evaluation_eligible(const LiveRequest& live,
                                             const EvaluationContext& context) const noexcept {
    const EligibilityFacts facts = eligibility_facts(live, context);
    return facts.ready_to_match;
}

std::vector<RequestHandle> WorkingRequestCore::waiting_children(const RequestHandle& parent) const {
    std::vector<RequestHandle> handles;
    for (const auto& live : live_) {
        if (const auto* wait = std::get_if<Wait>(&live.authority)) {
            if (wait->parent == parent) handles.push_back(live.handle());
        }
    }
    std::sort(handles.begin(), handles.end(),
              [](const RequestHandle& a, const RequestHandle& b) {
                  return a.incarnation < b.incarnation;
              });
    return handles;
}

std::vector<RequestHandle> WorkingRequestCore::bound_close_handles() const {
    std::vector<RequestHandle> handles;
    for (const auto& live : live_) {
        if (std::holds_alternative<BookClose>(live.authority)
            || std::holds_alternative<OpeningClose>(live.authority)) {
            handles.push_back(live.handle());
        }
    }
    std::sort(handles.begin(), handles.end(),
              [](const RequestHandle& a, const RequestHandle& b) {
                  return a.incarnation < b.incarnation;
              });
    return handles;
}

std::vector<RequestHandle> WorkingRequestCore::group_recipients(const EventId& applied) const {
    std::vector<RequestHandle> handles;
    const CommandEvent* event = event_at(applied);
    const auto* payload = event ? as_applied(*event) : nullptr;
    if (!payload || !payload->definition) return handles;
    const auto* member = std::get_if<Member>(&payload->definition->request.group);
    if (!member) return handles;
    if (member->effect == GroupEffect::Cancel && !payload->terminal) return handles;
    if (member->effect == GroupEffect::Reduce && !(payload->filled_working > 0.0)) return handles;
    for (const auto& live : live_) {
        const auto* other = std::get_if<Member>(&live.request().group);
        if (!other || other->group != member->group || other->cohort == member->cohort) continue;
        if (live.handle() == payload->handle()) continue;
        if (live.birth().acceptance_ordinal >= payload->ordinal) continue;
        handles.push_back(live.handle());
    }
    std::sort(handles.begin(), handles.end(),
              [](const RequestHandle& a, const RequestHandle& b) {
                  return a.incarnation < b.incarnation;
              });
    return handles;
}

PreparedSubmit WorkingRequestCore::prepare_submit(const Request& request,
                                                  const CommandContext& context,
                                                  uint64_t& next_order_incarnation,
                                                  uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    Request staged = request;
    MutationPlan plan = begin_plan();
    const auto reason = validate_request(staged, context, std::nullopt);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    auto impl = std::make_unique<PreparedSubmit::Impl>();
    if (reason) {
        impl->result = SubmitResult{SubmitStatus::Rejected, ordinal, std::nullopt, reason};
        plan.events.emplace_back(
                RejectedEvent{ordinal, std::move(staged), *reason, context.surface});
        seal_plan(plan);
        impl->plan = std::move(plan);
        impl->result.event_ordinal = ordinal;
        return PreparedSubmit(std::move(impl));
    }
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    RequestHandle handle{identity_, incarnation};
    Birth birth{ordinal, context.decision_time_ms};
    auto definition = std::make_shared<RequestDefinition>(
            RequestDefinition{handle, staged, birth, std::nullopt});
    LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
    AcceptedEvent accepted;
    accepted.ordinal = ordinal;
    accepted.definition = definition;
    accepted.surface = context.surface;
    plan.events.emplace_back(std::move(accepted));
    plan.live_change = kLivePush;
    plan.live_row = std::move(live);
    plan.consume_incarnation = true;
    plan.incarnation_used = incarnation;
    seal_plan(plan);
    impl->plan = std::move(plan);
    impl->result = SubmitResult{SubmitStatus::Accepted, ordinal, handle, std::nullopt};
    return PreparedSubmit(std::move(impl));
}

PreparedReplace WorkingRequestCore::prepare_replace(const RequestHandle& target,
                                                    const Request& request,
                                                    const CommandContext& context,
                                                    uint64_t& next_order_incarnation,
                                                    uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    RequestHandle staged_target = target;
    Request staged = request;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    auto impl = std::make_unique<PreparedReplace::Impl>();
    if (kind != TargetKind::Live) {
        const ReplaceStatus status = kind == TargetKind::InvalidHandle
                ? ReplaceStatus::InvalidHandle
                : ReplaceStatus::NotWorking;
        impl->result = ReplaceResult{status, ordinal, std::nullopt, std::nullopt};
        if (kind == TargetKind::InvalidHandle) {
            plan.events.emplace_back(InvalidHandleEvent{ordinal, std::move(staged_target),
                                                        std::move(staged), context.surface});
        } else {
            plan.events.emplace_back(NotWorkingEvent{ordinal, std::move(staged_target),
                                                     std::move(staged), context.surface});
        }
        seal_plan(plan);
        impl->plan = std::move(plan);
        return PreparedReplace(std::move(impl));
    }
    const auto reason = validate_request(staged, context, staged_target);
    if (reason) {
        impl->result = ReplaceResult{ReplaceStatus::ReplaceRejected, ordinal, std::nullopt, reason};
        ReplaceRejectedEvent rejected;
        rejected.ordinal = ordinal;
        rejected.live_definition = live_[live_index].definition;
        rejected.attempted = std::move(staged);
        rejected.reason = *reason;
        rejected.surface = context.surface;
        plan.events.emplace_back(std::move(rejected));
        seal_plan(plan);
        impl->plan = std::move(plan);
        return PreparedReplace(std::move(impl));
    }
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    RequestHandle successor{identity_, incarnation};
    Birth birth{ordinal, context.decision_time_ms};
    auto definition = std::make_shared<RequestDefinition>(
            RequestDefinition{successor, staged, birth, staged_target});
    LiveRequest live = make_live(definition, context, EventId{identity_, ordinal});
    ReplacedEvent replaced;
    replaced.ordinal = ordinal;
    replaced.predecessor_definition = live_[live_index].definition;
    replaced.successor_definition = definition;
    replaced.surface = context.surface;
    plan.events.emplace_back(std::move(replaced));
    plan.live_change = kLiveErasePush;
    plan.live_index = live_index;
    plan.live_row = std::move(live);
    plan.consume_incarnation = true;
    plan.incarnation_used = incarnation;
    seal_plan(plan);
    impl->plan = std::move(plan);
    impl->result = ReplaceResult{ReplaceStatus::Replaced, ordinal, successor, std::nullopt};
    return PreparedReplace(std::move(impl));
}

PreparedCancel WorkingRequestCore::prepare_cancel(const RequestHandle& target,
                                                  uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    RequestHandle staged_target = target;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    auto impl = std::make_unique<PreparedCancel::Impl>();
    if (kind != TargetKind::Live) {
        const CancelStatus status = kind == TargetKind::InvalidHandle
                ? CancelStatus::InvalidHandle
                : CancelStatus::NotWorking;
        impl->result = CancelResult{status, ordinal};
        if (kind == TargetKind::InvalidHandle) {
            plan.events.emplace_back(
                    InvalidHandleEvent{ordinal, std::move(staged_target), std::nullopt});
        } else {
            plan.events.emplace_back(
                    NotWorkingEvent{ordinal, std::move(staged_target), std::nullopt});
        }
        seal_plan(plan);
        impl->plan = std::move(plan);
        return PreparedCancel(std::move(impl));
    }
    impl->result = CancelResult{CancelStatus::Cancelled, ordinal};
    plan.events.emplace_back(make_cancelled(ordinal, live_[live_index], CancelReason::User,
                                            EventId{identity_, ordinal}));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    seal_plan(plan);
    impl->plan = std::move(plan);
    return PreparedCancel(std::move(impl));
}

InstalledCommand<SubmitResult> WorkingRequestCore::install_submit(PreparedSubmit&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto installed = commit(prepared.impl_->plan);
    if (const auto* error = std::get_if<InstallError>(&installed)) return *error;
    return CommandInstalled<SubmitResult>{std::move(prepared.impl_->result),
                                          std::get<Installed>(installed).events};
}

InstalledCommand<ReplaceResult> WorkingRequestCore::install_replace(PreparedReplace&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto installed = commit(prepared.impl_->plan);
    if (const auto* error = std::get_if<InstallError>(&installed)) return *error;
    return CommandInstalled<ReplaceResult>{std::move(prepared.impl_->result),
                                           std::get<Installed>(installed).events};
}

InstalledCommand<CancelResult> WorkingRequestCore::install_cancel(PreparedCancel&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto installed = commit(prepared.impl_->plan);
    if (const auto* error = std::get_if<InstallError>(&installed)) return *error;
    return CommandInstalled<CancelResult>{std::move(prepared.impl_->result),
                                          std::get<Installed>(installed).events};
}

SubmitResult WorkingRequestCore::submit(const Request& request,
                                        int64_t decision_time_ms,
                                        uint64_t& next_order_incarnation,
                                        uint64_t& next_timeline_ordinal,
                                        std::optional<double> quantity_grid) {
    auto prepared = prepare_submit(request, CommandContext{decision_time_ms, quantity_grid, std::nullopt},
                                   next_order_incarnation, next_timeline_ordinal);
    auto installed = install_submit(std::move(prepared));
    if (std::holds_alternative<InstallError>(installed)) {
        throw std::logic_error("native submit install failed");
    }
    auto& ok = std::get<CommandInstalled<SubmitResult>>(installed);
    ++next_timeline_ordinal;
    if (ok.result.status == SubmitStatus::Accepted) ++next_order_incarnation;
    return std::move(ok.result);
}

ReplaceResult WorkingRequestCore::replace(const RequestHandle& target,
                                          const Request& request,
                                          int64_t decision_time_ms,
                                          uint64_t& next_order_incarnation,
                                          uint64_t& next_timeline_ordinal,
                                          std::optional<double> quantity_grid) {
    auto prepared = prepare_replace(target, request,
                                    CommandContext{decision_time_ms, quantity_grid, std::nullopt},
                                    next_order_incarnation, next_timeline_ordinal);
    auto installed = install_replace(std::move(prepared));
    if (std::holds_alternative<InstallError>(installed)) {
        throw std::logic_error("native replace install failed");
    }
    auto& ok = std::get<CommandInstalled<ReplaceResult>>(installed);
    ++next_timeline_ordinal;
    if (ok.result.status == ReplaceStatus::Replaced) ++next_order_incarnation;
    return std::move(ok.result);
}

CancelResult WorkingRequestCore::cancel(const RequestHandle& target,
                                        uint64_t& next_timeline_ordinal) {
    auto prepared = prepare_cancel(target, next_timeline_ordinal);
    auto installed = install_cancel(std::move(prepared));
    if (std::holds_alternative<InstallError>(installed)) {
        throw std::logic_error("native cancel install failed");
    }
    auto& ok = std::get<CommandInstalled<CancelResult>>(installed);
    ++next_timeline_ordinal;
    return std::move(ok.result);
}

InstallResult WorkingRequestCore::install_mutation(PreparedMutation&& prepared) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    return commit(prepared.impl_->plan);
}

namespace {

Allowance initialize_allowance(const Remaining& remaining, const Capacity& capacity, uint64_t point) {
    if (std::holds_alternative<RemainingFlattenAll>(remaining)) return AllowanceAllScope{point};
    if (std::holds_alternative<RemainingUnbound>(remaining)) return AllowanceUnset{};
    const double q = working_units(remaining);
    double initial = q;
    if (const auto* budget = std::get_if<PointBudget>(&capacity)) initial = std::min(q, budget->units);
    return AllowanceUnits{point, initial, initial};
}

bool same_point_allowance(const Allowance& allowance, uint64_t point) noexcept {
    if (const auto* units = std::get_if<AllowanceUnits>(&allowance)) {
        return units->point_ordinal == point;
    }
    if (const auto* all = std::get_if<AllowanceAllScope>(&allowance)) {
        return all->point_ordinal == point;
    }
    return false;
}

}  // namespace

Preparation<PreparedMutation> WorkingRequestCore::prepare_evaluation(
        const RequestHandle& target,
        const EvaluationContext& context,
        const TargetObservation& observation,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)) {
        return NoChange{NoChangeReason::StillWaiting};
    }
    const EligibilityFacts facts = eligibility_facts(live, context);
    if (!facts.birth_ok || !facts.driver_ok) return NoChange{NoChangeReason::NotEligible};

    if (std::holds_alternative<UnboundBookClose>(live.authority)) {
        if (std::holds_alternative<PositionFlat>(observation.current_position)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            NoEffectEvent none;
            none.ordinal = ordinal;
            none.definition = live.definition;
            none.remaining = project_remaining(live.remaining);
            none.authority = live.authority;
            none.cursor = context.cursor;
            plan.events.emplace_back(std::move(none));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
        const auto* nonflat = std::get_if<PositionNonflat>(&observation.current_position);
        if (!nonflat || nonflat->cycle <= 0) {
            return PreparationError{CoreFailure::MissingObservation, EventId{identity_, 0}, target};
        }
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        MutationPlan plan = begin_plan();
        LiveRequest updated = live;
        BookClose bound;
        bound.cycle = nonflat->cycle;
        bound.side = nonflat->side;
        bound.binding_event = EventId{identity_, ordinal};
        bound.binding_cursor = context.cursor;
        CloseBoundEvent bound_event;
        bound_event.ordinal = ordinal;
        bound_event.definition = live.definition;
        bound_event.cycle = bound.cycle;
        bound_event.side = bound.side;
        bound_event.cursor = context.cursor;
        bound_event.before = live.authority;
        bound_event.after = bound;
        updated.authority = bound;
        updated.allowance = initialize_allowance(live.remaining, live.request().capacity,
                                                 context.cursor.point.ordinal);
        plan.events.emplace_back(std::move(bound_event));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    }

    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        if (!book_close_alive(observation, *close)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::OwnerGone,
                                                    EventId{identity_, ordinal}));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
    }
    if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        if (!opening_close_alive(observation, *close)) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            MutationPlan plan = begin_plan();
            plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::OwnerGone,
                                                    EventId{identity_, ordinal}));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
    }

    if (same_point_allowance(live.allowance, context.cursor.point.ordinal)) {
        return NoChange{NoChangeReason::NoTransition};
    }
    LiveRequest updated = live;
    updated.allowance = initialize_allowance(live.remaining, live.request().capacity,
                                             context.cursor.point.ordinal);
    MutationPlan plan = begin_plan();
    plan.live_change = kLiveUpdate;
    plan.live_index = live_index;
    plan.live_row = std::move(updated);
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_trigger(
        const RequestHandle& target,
        const TriggerTransition& transition,
        DriverEligibilityClass driver_class,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    LiveRequest updated = live_[live_index];
    if (std::holds_alternative<Wait>(updated.authority)
        || std::holds_alternative<UnboundBookClose>(updated.authority)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    const MatchCursor& cursor = transition_cursor(transition);
    if (!trigger_cursor_eligible(updated, cursor)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    if (!driver_class_matches_cursor(driver_class, cursor)
        || !trigger_permits_driver(updated.request().trigger, updated.trigger_state,
                                    driver_class, false)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    const bool is_buy = working_is_buy(updated);
    MutationPlan plan = begin_plan();

    auto emit_activated = [&](ActivationKind kind, TriggerState after, const MatchCursor& cursor,
                              double price) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        ActivatedEvent activated;
        activated.ordinal = ordinal;
        activated.definition = updated.definition;
        activated.kind = kind;
        activated.before = updated.trigger_state;
        activated.after = after;
        activated.reached_price = price;
        activated.cursor = cursor;
        updated.trigger_state = after;
        plan.events.emplace_back(std::move(activated));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    };

    if (const auto* begin = std::get_if<BeginTrailTracking>(&transition)) {
        if (!std::holds_alternative<TrailWaitArm>(updated.trigger_state)) {
            if (std::holds_alternative<TrailTrack>(updated.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        if (!std::isfinite(begin->reached_price)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        const auto* trail = std::get_if<Trail>(&updated.request().trigger);
        if (!trail || !trail_level_ok(begin->reached_price, trail->offset, is_buy, nullptr)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        if (trail->arm_price
            && !stop_price_reached(!is_buy, *trail->arm_price, begin->reached_price)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        return emit_activated(ActivationKind::TrailArm, TrailTrack{begin->reached_price},
                              begin->cursor, begin->reached_price);
    }
    if (const auto* extremum = std::get_if<ObserveTrailExtremum>(&transition)) {
        auto* track = std::get_if<TrailTrack>(&updated.trigger_state);
        if (!track) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const double next_best = is_buy ? std::min(track->best, extremum->reached_price)
                                        : std::max(track->best, extremum->reached_price);
        if (next_best == track->best) return NoChange{NoChangeReason::NoTransition};
        const auto* trail = std::get_if<Trail>(&updated.request().trigger);
        if (!trail || !trail_level_ok(next_best, trail->offset, is_buy, nullptr)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
        track->best = next_best;
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    }
    if (const auto* stop = std::get_if<ActivateStop>(&transition)) {
        if (!std::holds_alternative<StopIdle>(updated.trigger_state)) {
            if (std::holds_alternative<StopActive>(updated.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const auto* stop_px = std::get_if<Stop>(&updated.request().trigger);
        if (!stop_px || !stop_price_reached(is_buy, stop_px->price, stop->reached_price)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        return emit_activated(ActivationKind::Stop, StopActive{}, stop->cursor, stop->reached_price);
    }
    if (const auto* stop_limit = std::get_if<ActivateStopLimit>(&transition)) {
        if (!std::holds_alternative<StopLimitPending>(updated.trigger_state)) {
            if (std::holds_alternative<StopLimitLive>(updated.trigger_state)) {
                return NoChange{NoChangeReason::NoTransition};
            }
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        const auto* stop_limit_px = std::get_if<StopLimit>(&updated.request().trigger);
        if (!stop_limit_px
            || !stop_price_reached(is_buy, stop_limit_px->stop, stop_limit->reached_price)) {
            return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0},
                                    target};
        }
        return emit_activated(ActivationKind::StopLimit, StopLimitLive{}, stop_limit->cursor,
                              stop_limit->reached_price);
    }
    const auto& trail_hit = std::get<ActivateTrail>(transition);
    const auto* track = std::get_if<TrailTrack>(&updated.trigger_state);
    if (!track) {
        if (std::holds_alternative<TrailActive>(updated.trigger_state)) {
            return NoChange{NoChangeReason::NoTransition};
        }
        return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0}, target};
    }
    const auto* trail = std::get_if<Trail>(&updated.request().trigger);
    double level = 0.0;
    if (!trail || !trail_level_ok(track->best, trail->offset, is_buy, &level)
        || !stop_price_reached(is_buy, level, trail_hit.reached_price)) {
        return PreparationError{CoreFailure::UnsupportedTransition, EventId{identity_, 0}, target};
    }
    return emit_activated(ActivationKind::TrailTrigger, TrailActive{track->best}, trail_hit.cursor,
                          trail_hit.reached_price);
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_no_effect(
        const RequestHandle& target,
        const EvaluationContext& context,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    NoEffectEvent none;
    none.ordinal = ordinal;
    none.definition = live.definition;
    none.remaining = project_remaining(live.remaining);
    none.authority = live.authority;
    none.cursor = context.cursor;
    plan.events.emplace_back(std::move(none));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_match_rejected(
        const RequestHandle& target,
        const EvaluationContext& context,
        MatchRejectReason reason,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    MatchRejectedEvent rejected;
    rejected.ordinal = ordinal;
    rejected.reason = reason;
    rejected.definition = live.definition;
    rejected.remaining = project_remaining(live.remaining);
    rejected.authority = live.authority;
    rejected.cursor = context.cursor;
    plan.events.emplace_back(std::move(rejected));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

Preparation<PreparedExecution> WorkingRequestCore::prepare_execution(
        const RequestHandle& target,
        const ExecutionProposal& proposal,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(target, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    if (std::holds_alternative<Wait>(live.authority)
        || std::holds_alternative<UnboundBookClose>(live.authority)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    if (!point_eligible(live.birth(), proposal.cursor.point.ordinal,
                        proposal.cursor.point.effective_time_ms)) {
        return NoChange{NoChangeReason::NotEligible};
    }
    if (!fillable_state(live.trigger_state)) return NoChange{NoChangeReason::NotEligible};

    const uint64_t point = proposal.cursor.point.ordinal;
    double allowance_left = 0.0;
    const bool has_units_allowance = std::holds_alternative<AllowanceUnits>(live.allowance);
    const bool has_all_allowance = std::holds_alternative<AllowanceAllScope>(live.allowance);
    if (has_units_allowance) {
        const auto& units = std::get<AllowanceUnits>(live.allowance);
        if (units.point_ordinal != point) return NoChange{NoChangeReason::NotEligible};
        allowance_left = units.left;
        if (!(allowance_left > 0.0) && !std::holds_alternative<RemainingFlattenAll>(live.remaining)) {
            return NoChange{NoChangeReason::NotEligible};
        }
    } else if (has_all_allowance) {
        if (std::get<AllowanceAllScope>(live.allowance).point_ordinal != point) {
            return NoChange{NoChangeReason::NotEligible};
        }
    } else {
        return NoChange{NoChangeReason::NotEligible};
    }

    const bool flatten = as_flatten(live.request().intent) != nullptr;
    const bool reduce = as_reduce(live.request().intent) != nullptr;
    const auto* transact = as_transact(live.request().intent);
    double cap = flatten ? 0.0 : working_units(live.remaining);
    if (has_units_allowance) cap = std::min(cap, allowance_left);

    if (flatten) {
        if (!std::holds_alternative<execution::Flatten>(proposal.physical_action)) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (reduce) {
        const auto* phys = std::get_if<order_action::Reduce>(&proposal.physical_action);
        if (!phys || !finite_positive(phys->units) || phys->units > cap) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else if (transact) {
        const auto* phys = std::get_if<order_action::Transact>(&proposal.physical_action);
        if (!phys || !finite_nonzero(phys->signed_units)
            || ((phys->signed_units > 0.0) != (transact->signed_units > 0.0))
            || std::abs(phys->signed_units) > cap) {
            return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
        }
    } else {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    const bool opening_auth = std::holds_alternative<OpeningClose>(live.authority);
    const bool book_close = std::holds_alternative<BookClose>(live.authority);
    if (opening_auth) {
        const auto& close = std::get<OpeningClose>(live.authority);
        const auto* scope = std::get_if<execution::OpeningExposure>(&proposal.scope);
        if (!scope || scope->incarnation != close.opening.incarnation || scope->cycle != close.cycle) {
            return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
        }
    } else if (!std::holds_alternative<execution::Book>(proposal.scope)) {
        return PreparationError{CoreFailure::InvalidScope, EventId{identity_, 0}, target};
    }
    (void)book_close;

    if (!std::isfinite(proposal.inspected_closed_units) || proposal.inspected_closed_units < 0.0
        || !std::isfinite(proposal.inspected_opened_units)
        || !std::isfinite(proposal.resolved_price) || proposal.resolved_price <= 0.0) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    double filled = proposal.inspected_closed_units;
    if (transact) {
        if (!checked_add_positive(proposal.inspected_closed_units,
                                  std::abs(proposal.inspected_opened_units), &filled)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        }
    }
    if (!(filled > 0.0) || !std::isfinite(filled)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    if (!flatten && filled > working_units(live.remaining)) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }
    if (has_units_allowance && filled > allowance_left) {
        return PreparationError{CoreFailure::InvalidProposal, EventId{identity_, 0}, target};
    }

    RemainingProjection remaining_after = RemainingProjectionFlattenAll{};
    bool units_exhausted = flatten;
    if (!flatten) {
        double after = 0.0;
        if (!checked_sub_cap(working_units(live.remaining), std::min(filled, working_units(live.remaining)),
                             &after, &units_exhausted)) {
            if (filled != working_units(live.remaining)) {
                return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                        target};
            }
            units_exhausted = true;
            after = 0.0;
        }
        remaining_after = RemainingProjectionUnits{after};
    }

    Allowance allowance_after = live.allowance;
    if (has_units_allowance) {
        auto units = std::get<AllowanceUnits>(live.allowance);
        bool allow_ex = false;
        double left = 0.0;
        if (filled == units.left) {
            units.left = 0.0;
        } else if (!checked_sub_cap(units.left, filled, &left, &allow_ex)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, EventId{identity_, 0},
                                    target};
        } else {
            units.left = left;
        }
        allowance_after = units;
    }

    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    ExecutionAppliedEvent base;
    base.ordinal = ordinal;
    base.definition = live.definition;
    base.raw_price = proposal.raw_price;
    base.resolved_price = proposal.resolved_price;
    base.current_ticket = proposal.inspected_current_ticket;
    base.closed_units = proposal.inspected_closed_units;
    base.opened_units = proposal.inspected_opened_units;
    base.filled_working = filled;
    base.remaining_before = project_remaining(live.remaining);
    base.allowance_before = live.allowance;
    base.allowance_after = allowance_after;
    base.scope = proposal.scope;
    fill_applied_cursor(base, proposal.cursor);

    ExecutionAppliedEvent terminal = base;
    terminal.terminal = true;
    terminal.remaining_after = flatten ? RemainingProjectionFlattenAll{}
            : (units_exhausted ? RemainingProjectionUnits{0.0}
                               : remaining_after);
    terminal.terminal_reason = flatten ? AppliedTerminalReason::Flattened
            : (units_exhausted ? AppliedTerminalReason::WorkingUnitsSatisfied
                               : AppliedTerminalReason::TargetExhausted);

    ExecutionAppliedEvent retain = base;
    retain.terminal = false;
    retain.remaining_after = remaining_after;
    retain.terminal_reason = std::nullopt;

    MutationPlan plan = begin_plan();
    plan.events.emplace_back(terminal);
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    seal_plan(plan);

    auto impl = std::make_unique<PreparedExecution::Impl>();
    impl->terminal = std::move(plan);
    impl->retain_event = CommandEvent{std::move(retain)};
    impl->can_retain = !flatten && !units_exhausted;
    if (impl->can_retain) {
        LiveRequest kept = live;
        kept.remaining = RemainingUnits{std::get<RemainingProjectionUnits>(remaining_after).q};
        kept.allowance = allowance_after;
        impl->retain_live = std::move(kept);
    }
    impl->flatten = flatten;
    impl->opening = opening_auth;
    if (opening_auth) impl->opening_close = std::get<OpeningClose>(live.authority);
    if (std::holds_alternative<BookClose>(live.authority)) {
        impl->book_close = std::get<BookClose>(live.authority);
    }
    impl->proposal = proposal;
    return PreparedExecution(std::move(impl));
}

InstallResult WorkingRequestCore::install_execution(PreparedExecution&& prepared,
                                                    const CommittedExecutionFacts& facts) noexcept {
    if (!prepared.impl_) return InstallError::StalePreparation;
    auto& impl = *prepared.impl_;
    if (const auto error = validate_plan(impl.terminal)) return *error;
    if (facts.result.status != execution::Status::Applied) return InstallError::WrongCoreOrRun;
    if (facts.result.closed_units != impl.proposal.inspected_closed_units
        || facts.result.opened_units != impl.proposal.inspected_opened_units
        || facts.result.current_ticket != impl.proposal.inspected_current_ticket) {
        return InstallError::WrongCoreOrRun;
    }

    auto stamp = [&](ExecutionAppliedEvent& event) {
        event.current_ticket = facts.result.current_ticket;
        event.first_trade_index = facts.result.first_trade_index;
        event.closed_trade_count = facts.result.closed_trade_count;
        event.opened_lot_incarnation = facts.result.opened_lot_incarnation;
        event.cycle_before = facts.cycle_before;
        event.cycle_after = facts.cycle_after;
    };

    bool retain = impl.can_retain;
    if (retain && impl.opening && !opening_close_alive(facts.post_target, impl.opening_close)) {
        retain = false;
    }
    if (retain && std::holds_alternative<BookClose>(impl.retain_live.authority)
        && !book_close_alive(facts.post_target, impl.book_close)) {
        retain = false;
    }

    if (retain) {
        auto* event = std::get_if<ExecutionAppliedEvent>(&impl.retain_event);
        if (!event) return InstallError::WrongCoreOrRun;
        stamp(*event);
        impl.terminal.events.clear();
        impl.terminal.events.push_back(std::move(impl.retain_event));
        impl.terminal.live_change = kLiveUpdate;
        impl.terminal.live_row = std::move(impl.retain_live);
        return commit(impl.terminal);
    }

    auto* event = std::get_if<ExecutionAppliedEvent>(&impl.terminal.events.front());
    if (!event) return InstallError::WrongCoreOrRun;
    stamp(*event);
    if (impl.can_retain && event->terminal_reason == AppliedTerminalReason::TargetExhausted) {
        // already staged
    } else if (impl.can_retain) {
        event->terminal = true;
        event->terminal_reason = AppliedTerminalReason::TargetExhausted;
    }
    impl.terminal.live_change = kLiveErase;
    return commit(impl.terminal);
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_group_effect(
        const EventId& applied,
        const RequestHandle& recipient,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    const CommandEvent* event = event_at(applied);
    const auto* payload = event ? as_applied(*event) : nullptr;
    if (!payload || !payload->definition) {
        return PreparationError{CoreFailure::InvalidCause, applied, recipient};
    }
    const auto* member = std::get_if<Member>(&payload->definition->request.group);
    if (!member) return NoChange{NoChangeReason::NoTransition};
    if (member->effect == GroupEffect::Cancel && !payload->terminal) {
        return NoChange{NoChangeReason::NoTransition};
    }
    if (member->effect == GroupEffect::Reduce && !(payload->filled_working > 0.0)) {
        return NoChange{NoChangeReason::NoTransition};
    }
    uint64_t seen = 0;
    const ReceiptLookup lookup = receipt_lookup(applied, recipient, member->effect, &seen);
    if (lookup == ReceiptLookup::Present) return NoChange{NoChangeReason::AlreadyApplied};
    if (lookup == ReceiptLookup::Conflict) {
        return PreparationError{CoreFailure::ConflictingReceipt, applied, recipient};
    }
    if (!receipts_.empty()) {
        const auto& last = receipts_.back();
        if (receipt_cmp(applied.ordinal, recipient.incarnation, member->effect, last.cause.ordinal,
                        last.recipient.incarnation, last.effect)
            < 0) {
            return PreparationError{CoreFailure::InvalidCause, applied, recipient};
        }
    }
    std::size_t live_index = 0;
    if (classify(recipient, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const auto* other = std::get_if<Member>(&live.request().group);
    if (!other || other->group != member->group || other->cohort == member->cohort
        || live.birth().acceptance_ordinal >= payload->ordinal) {
        return PreparationError{CoreFailure::InvalidCause, applied, recipient};
    }

    MutationPlan plan = begin_plan();
    plan.add_receipt = true;
    plan.receipt_cause = applied;
    plan.receipt_recipient = recipient;
    plan.receipt_effect = member->effect;

    if (std::holds_alternative<RemainingUnbound>(live.remaining)) {
        if (member->effect == GroupEffect::Cancel) {
            const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
            plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::Group, applied));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            plan.receipt_outcome = ordinal;
            return finish_mutation(std::move(plan));
        }
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        PendingDeferred after;
        PendingAdjustments before = live.pending;
        if (const auto* pending = std::get_if<PendingDeferred>(&live.pending)) {
            if (!checked_add_positive(pending->total, payload->filled_working, &after.total)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
            }
            after.count = pending->count + 1;
            after.tail_receipt = EventId{identity_, ordinal};
        } else {
            after.total = payload->filled_working;
            after.count = 1;
            after.tail_receipt = EventId{identity_, ordinal};
            if (!std::isfinite(after.total) || !(after.total > 0.0)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
            }
        }
        DeferredGroupAdjustmentEvent deferred;
        deferred.ordinal = ordinal;
        deferred.definition = live.definition;
        deferred.cause = applied;
        deferred.recipient = recipient;
        deferred.effect = GroupEffect::Reduce;
        deferred.deferred_delta = payload->filled_working;
        deferred.pending_before = before;
        deferred.pending_after = after;
        if (const auto* pending = std::get_if<PendingDeferred>(&before)) {
            deferred.previous_pending_receipt = pending->tail_receipt;
        }
        LiveRequest updated = live;
        updated.pending = after;
        plan.events.emplace_back(std::move(deferred));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        plan.receipt_outcome = ordinal;
        return finish_mutation(std::move(plan));
    }

    if (member->effect == GroupEffect::Cancel
        || std::holds_alternative<RemainingFlattenAll>(live.remaining)) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::Group, applied));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
        plan.receipt_outcome = ordinal;
        return finish_mutation(std::move(plan));
    }

    const double before_q = working_units(live.remaining);
    const double deduct = std::min(payload->filled_working, before_q);
    double after_q = 0.0;
    bool exhausted = false;
    if (!checked_sub_cap(before_q, deduct, &after_q, &exhausted)) {
        return PreparationError{CoreFailure::UnrepresentableReservation, applied, recipient};
    }
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    ReservationReducedEvent reduced;
    reduced.ordinal = ordinal;
    reduced.definition = live.definition;
    reduced.cause = applied;
    reduced.recipient = recipient;
    reduced.effect = GroupEffect::Reduce;
    reduced.requested_delta = payload->filled_working;
    reduced.actual_deduction = deduct;
    reduced.before = RemainingUnits{before_q};
    reduced.after = RemainingProjectionUnits{after_q};
    plan.events.emplace_back(std::move(reduced));
    plan.receipt_outcome = ordinal;
    if (exhausted) {
        uint64_t cancel_ord = ordinal + 1;
        if (cancel_ord == 0 || cancel_ord == std::numeric_limits<uint64_t>::max()
            || cancel_ord <= last_ordinal_) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
        LiveRequest snapshot = live;
        snapshot.remaining = RemainingUnits{before_q};
        plan.events.emplace_back(make_cancelled(cancel_ord, snapshot, CancelReason::Group, applied));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
    } else {
        LiveRequest updated = live;
        updated.remaining = RemainingUnits{after_q};
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
    }
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_owner_applied(
        const EventId& applied,
        const RequestHandle& child,
        const std::optional<OpeningObservation>& observation,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    const CommandEvent* event = event_at(applied);
    const auto* payload = event ? as_applied(*event) : nullptr;
    if (!payload) return PreparationError{CoreFailure::InvalidCause, applied, child};
    std::size_t live_index = 0;
    if (classify(child, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const auto* wait = std::get_if<Wait>(&live.authority);
    if (!wait || wait->parent != payload->handle()) {
        return NoChange{NoChangeReason::NoTransition};
    }
    if (payload->ordinal <= live.birth().acceptance_ordinal) {
        return NoChange{NoChangeReason::NoTransition};
    }
    const bool closing = as_reduce(live.request().intent) || as_flatten(live.request().intent);
    const bool opened = payload->opened_units != 0.0;
    if (closing && !opened && !payload->terminal) {
        return NoChange{NoChangeReason::StillWaiting};
    }
    if (closing && !opened && payload->terminal) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        MutationPlan plan = begin_plan();
        plan.events.emplace_back(
                make_cancelled(ordinal, live, CancelReason::UnsupportedRelation, applied));
        plan.live_change = kLiveErase;
        plan.live_index = live_index;
        return finish_mutation(std::move(plan));
    }

    MutationPlan plan = begin_plan();
    LiveRequest updated = live;
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    if (!closing) {
        ArmedTransaction armed;
        armed.parent = wait->parent;
        armed.cause = applied;
        armed.cause_cursor = payload->cursor;
        ArmedEvent armed_event;
        armed_event.ordinal = ordinal;
        armed_event.definition = live.definition;
        armed_event.before = live.authority;
        armed_event.after = armed;
        armed_event.enrollment = EnrollmentFromApplied{applied, payload->cursor};
        updated.authority = armed;
        plan.events.emplace_back(std::move(armed_event));
        plan.live_change = kLiveUpdate;
        plan.live_index = live_index;
        plan.live_row = std::move(updated);
        return finish_mutation(std::move(plan));
    }

    if (!observation) {
        return PreparationError{CoreFailure::MissingObservation, applied, child};
    }
    const int64_t cycle = payload->cycle_after;
    const Side side = side_from_opened(payload->opened_units);
    if (!opening_alive(*observation, payload->handle(), cycle, &side)) {
        return PreparationError{CoreFailure::ObservationMismatch, applied, child};
    }

    Remaining remaining = live.remaining;
    std::optional<EventId> quantity_resolution;
    const bool owner_opened =
            as_reduce(live.request().intent) && is_owner_opened(*as_reduce(live.request().intent));
    if (owner_opened) {
        const double source = std::abs(payload->opened_units);
        if (!finite_positive(source)) {
            return PreparationError{CoreFailure::NonrepresentableQuantity, applied, child};
        }
        std::vector<EventId> ids;
        double pending_total = 0.0;
        if (!collect_pending_chain(live.pending, child, &ids, &pending_total)) {
            return PreparationError{CoreFailure::ConflictingReceipt, applied, child};
        }
        const double deduct = std::min(pending_total, source);
        double after = source;
        bool exhausted = deduct == source;
        if (deduct > 0.0 && !exhausted) {
            if (!checked_sub_cap(source, deduct, &after, &exhausted)) {
                return PreparationError{CoreFailure::UnrepresentableReservation, applied, child};
            }
        } else if (exhausted) {
            after = 0.0;
        }
        QuantityBoundEvent bound;
        bound.ordinal = ordinal;
        bound.definition = live.definition;
        bound.source = applied;
        bound.source_units = source;
        bound.prior_adjustment_ids = ids;
        bound.pending_total = pending_total;
        bound.effective_deduction = deduct;
        bound.remaining = RemainingProjectionUnits{after};
        plan.events.emplace_back(std::move(bound));
        quantity_resolution = EventId{identity_, ordinal};
        updated.pending = PendingNone{};
        if (exhausted) {
            uint64_t cancel_ord = ordinal + 1;
            if (cancel_ord <= last_ordinal_ || cancel_ord == 0) {
                throw std::invalid_argument("native timeline ordinal reused or regressed");
            }
            plan.events.emplace_back(make_cancelled(cancel_ord, live, CancelReason::Group, applied));
            plan.live_change = kLiveErase;
            plan.live_index = live_index;
            return finish_mutation(std::move(plan));
        }
        remaining = RemainingUnits{after};
        updated.remaining = remaining;
    }

    OpeningClose close;
    close.opening = payload->handle();
    close.cycle = cycle;
    close.side = side;
    close.enrollment = EnrollmentFromApplied{applied, payload->cursor};
    const uint64_t arm_ord = owner_opened ? ordinal + 1 : ordinal;
    if (owner_opened) {
        if (arm_ord <= last_ordinal_ || arm_ord == 0) {
            throw std::invalid_argument("native timeline ordinal reused or regressed");
        }
    }
    ArmedEvent armed_event;
    armed_event.ordinal = arm_ord;
    armed_event.definition = live.definition;
    armed_event.before = live.authority;
    armed_event.after = close;
    armed_event.enrollment = close.enrollment;
    armed_event.quantity_resolution = quantity_resolution;
    updated.authority = close;
    if (!owner_opened) {
        plan.events.emplace_back(std::move(armed_event));
    } else {
        plan.events.emplace_back(std::move(armed_event));
    }
    plan.live_change = kLiveUpdate;
    plan.live_index = live_index;
    plan.live_row = std::move(updated);
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_bound_expiry(
        const EventId& physical_cause,
        const RequestHandle& child,
        const TargetObservation& observation,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    std::size_t live_index = 0;
    if (classify(child, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    bool gone = false;
    if (const auto* close = std::get_if<BookClose>(&live.authority)) {
        gone = !book_close_alive(observation, *close);
    } else if (const auto* close = std::get_if<OpeningClose>(&live.authority)) {
        gone = !opening_close_alive(observation, *close);
    } else {
        return NoChange{NoChangeReason::NoTransition};
    }
    if (!gone) return NoChange{NoChangeReason::NoTransition};
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    plan.events.emplace_back(make_cancelled(ordinal, live, CancelReason::OwnerGone, physical_cause));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

Preparation<PreparedMutation> WorkingRequestCore::prepare_parent_terminal(
        const EventId& terminal_or_replaced,
        const RequestHandle& child,
        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    const CommandEvent* event = event_at(terminal_or_replaced);
    if (!event) {
        return PreparationError{CoreFailure::InvalidCause, terminal_or_replaced, child};
    }
    std::size_t live_index = 0;
    if (classify(child, &live_index) != TargetKind::Live) {
        return NoChange{NoChangeReason::NotWorking};
    }
    const LiveRequest& live = live_[live_index];
    const auto* wait = std::get_if<Wait>(&live.authority);
    if (!wait) return NoChange{NoChangeReason::NoTransition};

    bool parent_ended = false;
    if (const auto* cancelled = std::get_if<CancelledEvent>(event)) {
        parent_ended = cancelled->handle() == wait->parent;
    } else if (const auto* replaced = std::get_if<ReplacedEvent>(event)) {
        parent_ended = replaced->predecessor() == wait->parent;
    } else if (const auto* none = std::get_if<NoEffectEvent>(event)) {
        parent_ended = none->handle() == wait->parent;
    } else if (const auto* rejected = std::get_if<MatchRejectedEvent>(event)) {
        parent_ended = rejected->handle() == wait->parent;
    } else if (const auto* applied = as_applied(*event)) {
        parent_ended = applied->handle() == wait->parent && applied->terminal;
    }
    if (!parent_ended) return NoChange{NoChangeReason::NoTransition};

    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    MutationPlan plan = begin_plan();
    plan.events.emplace_back(
            make_cancelled(ordinal, live, CancelReason::OwnerGone, terminal_or_replaced));
    plan.live_change = kLiveErase;
    plan.live_index = live_index;
    return finish_mutation(std::move(plan));
}

static_assert(std::is_nothrow_move_constructible_v<PreparedSubmit>);
static_assert(std::is_nothrow_move_constructible_v<PreparedReplace>);
static_assert(std::is_nothrow_move_constructible_v<PreparedCancel>);
static_assert(std::is_nothrow_move_constructible_v<PreparedMutation>);
static_assert(std::is_nothrow_move_constructible_v<PreparedExecution>);

}  // inline namespace native_order_v2
}  // namespace pineforge::native_order
