#pragma once

#include "execution.hpp"
#include "execution_close_scope.hpp"
#include "market_driver.hpp"
#include "native_order_identity.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge::native_order {
inline namespace native_order_v2 {

// Isolated working-request/value core: current LIVE requests and immutable
// command history. It does not own positions, cash, paid fees, matching,
// calendar, host phase, or a second physical book.
//
// Identity types remain native_order_v1. Request/core/event values are v2.
// Physical execution::Action is unchanged; native Reduce uses a typed size
// source instead of a dummy units field.

using Flatten = execution::Flatten;
using Transact = order_action::Transact;

struct ExplicitUnits {
    double units = 0.0;
};
struct OwnerOpenedUnits {};
using ReductionSize = std::variant<ExplicitUnits, OwnerOpenedUnits>;
struct Reduce {
    ReductionSize size;
};
using OrderIntent = std::variant<Flatten, Reduce, Transact>;

struct Market {};
struct Limit {
    double price = 0.0;
};
struct Stop {
    double price = 0.0;
};
struct StopLimit {
    double stop = 0.0;
    double limit = 0.0;
};
struct Trail {
    double offset = 0.0;
    std::optional<double> arm_price;
};
using Trigger = std::variant<Market, Limit, Stop, StopLimit, Trail>;

struct ImmediateRemaining {};
struct PointBudget {
    double units = 0.0;
};
using Capacity = std::variant<ImmediateRemaining, PointBudget>;

struct Independent {};
struct WaitForApplied {
    RequestHandle parent;
};
struct BindOpening {
    RequestHandle opening;
    int64_t cycle = 0;
};
using Owner = std::variant<Independent, WaitForApplied, BindOpening>;

enum class GroupEffect : std::uint8_t { Cancel = 0, Reduce = 1 };
struct NoGroup {};
struct Member {
    std::uint64_t group = 0;
    std::int64_t cohort = 0;
    GroupEffect effect = GroupEffect::Cancel;
};
using Group = std::variant<NoGroup, Member>;

// Aggregate field order keeps market construction:
//   Request{Transact{1.0}, "buy", "comment"}
//   Request{Flatten{}, "flat", ""}
//   Request{Reduce{ExplicitUnits{3}}, "close", ""}
// New fields default to Market / ImmediateRemaining / Independent / NoGroup.
// market_request() converts a physical execution::Action (explicit Reduce
// units only) into that same market Request. Host market-only methods must
// reject nondefault extras; they must not drop them.
struct Request {
    OrderIntent intent{};
    std::string label{};
    std::string comment{};
    Trigger trigger = Market{};
    Capacity capacity = ImmediateRemaining{};
    Owner owner = Independent{};
    Group group = NoGroup{};
};

inline OrderIntent intent_from_physical(const execution::Action& action) {
    if (std::holds_alternative<execution::Flatten>(action)) return Flatten{};
    if (const auto* transact = std::get_if<order_action::Transact>(&action)) {
        return Transact{transact->signed_units};
    }
    const auto& reduce = std::get<order_action::Reduce>(action);
    return Reduce{ExplicitUnits{reduce.units}};
}

inline Request market_request(execution::Action action,
                              std::string label = {},
                              std::string comment = {}) {
    return Request{intent_from_physical(action), std::move(label), std::move(comment)};
}

inline bool has_market_defaults(const Request& request) noexcept {
    return std::holds_alternative<Market>(request.trigger)
        && std::holds_alternative<ImmediateRemaining>(request.capacity)
        && std::holds_alternative<Independent>(request.owner)
        && std::holds_alternative<NoGroup>(request.group);
}

struct EventId {
    RunIdentity run;
    uint64_t ordinal = 0;
};

inline bool operator==(const EventId& a, const EventId& b) {
    return a.ordinal == b.ordinal && a.run == b.run;
}
inline bool operator!=(const EventId& a, const EventId& b) { return !(a == b); }

struct MatchCursor {
    NativeCoordinate point{};
    double t = 0.0;
};

inline bool operator==(const MatchCursor& a, const MatchCursor& b) noexcept {
    return a.point.ordinal == b.point.ordinal && a.t == b.t
        && a.point.interval_index == b.point.interval_index
        && a.point.effective_time_ms == b.point.effective_time_ms
        && a.point.provenance == b.point.provenance
        && a.point.path_phase == b.point.path_phase;
}
inline bool operator!=(const MatchCursor& a, const MatchCursor& b) noexcept { return !(a == b); }

inline std::uint8_t display_waypoint(const MatchCursor& cursor) noexcept {
    return static_cast<std::uint8_t>(cursor.point.path_phase);
}

enum class Side : std::uint8_t { Long = 0, Short = 1 };

struct RemainingUnbound {};
struct RemainingFlattenAll {};
struct RemainingUnits {
    double q = 0.0;
};
using Remaining = std::variant<RemainingUnbound, RemainingFlattenAll, RemainingUnits>;

struct RemainingProjectionUnbound {};
struct RemainingProjectionFlattenAll {};
struct RemainingProjectionUnits {
    double q = 0.0;
};
using RemainingProjection =
        std::variant<RemainingProjectionUnbound, RemainingProjectionFlattenAll,
                     RemainingProjectionUnits>;

struct BookTransaction {};
struct Wait {
    RequestHandle parent;
};
struct ArmedTransaction {
    RequestHandle parent;
    EventId cause;
    MatchCursor cause_cursor{};
};
struct UnboundBookClose {};
struct BookClose {
    int64_t cycle = 0;
    Side side = Side::Long;
    EventId binding_event;
    MatchCursor binding_cursor{};
};
struct EnrollmentFromCommand {
    EventId accepted;
};
struct EnrollmentFromApplied {
    EventId cause;
    MatchCursor cursor{};
};
using Enrollment = std::variant<EnrollmentFromCommand, EnrollmentFromApplied>;
struct OpeningClose {
    RequestHandle opening;
    int64_t cycle = 0;
    Side side = Side::Long;
    Enrollment enrollment;
};
using Authority = std::variant<BookTransaction, Wait, ArmedTransaction, UnboundBookClose, BookClose,
                               OpeningClose>;

struct MarketReady {};
struct LimitReady {};
struct StopIdle {};
struct StopActive {};
struct StopLimitPending {};
struct StopLimitLive {};
struct TrailWaitArm {};
struct TrailTrack {
    double best = 0.0;
};
struct TrailActive {
    double best_at_trigger = 0.0;
};
using TriggerState = std::variant<MarketReady, LimitReady, StopIdle, StopActive, StopLimitPending,
                                  StopLimitLive, TrailWaitArm, TrailTrack, TrailActive>;

struct AllowanceUnset {};
struct AllowanceUnits {
    uint64_t point_ordinal = 0;
    double initial = 0.0;
    double left = 0.0;
};
struct AllowanceAllScope {
    uint64_t point_ordinal = 0;
};
using Allowance = std::variant<AllowanceUnset, AllowanceUnits, AllowanceAllScope>;

struct PendingNone {};
struct PendingDeferred {
    double total = 0.0;
    uint64_t count = 0;
    EventId tail_receipt;
};
using PendingAdjustments = std::variant<PendingNone, PendingDeferred>;

struct PositionFlat {};
struct PositionNonflat {
    int64_t cycle = 0;
    Side side = Side::Long;
};
using PositionIdentity = std::variant<PositionFlat, PositionNonflat>;

struct OpeningObservation {
    RequestHandle queried_opening;
    int64_t queried_cycle = 0;
    PositionIdentity current_position;
    bool has_live_matching_lot = false;
};

struct TargetObservation {
    PositionIdentity current_position;
    std::optional<OpeningObservation> opening;
};

struct RequestDefinition {
    RequestHandle handle;
    Request request;
    Birth birth;
    std::optional<RequestHandle> predecessor;
};
using DefinitionRef = std::shared_ptr<const RequestDefinition>;

struct LiveRequest {
    DefinitionRef definition;
    Remaining remaining = RemainingUnbound{};
    Authority authority = UnboundBookClose{};
    TriggerState trigger_state = MarketReady{};
    Allowance allowance = AllowanceUnset{};
    PendingAdjustments pending = PendingNone{};

    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
    const Birth& birth() const noexcept { return definition->birth; }
    const std::optional<RequestHandle>& predecessor() const noexcept {
        return definition->predecessor;
    }
};

inline bool point_eligible(const LiveRequest& live,
                           uint64_t point_ordinal,
                           int64_t effective_time_ms) noexcept {
    return point_eligible(live.birth(), point_ordinal, effective_time_ms);
}

// v2 exact binary64 grid: r=abs(q)/s, n=round(r) half away from zero, g=n*s.
// Intermediates finite, 1<=n<=2^53, abs(abs(q)-g) <= 4*ulp(max(abs(q),abs(g)))
// and < s/2. ulp is nextafter toward +inf. Does not rewrite q.
inline bool quantity_on_grid(double q, double step) noexcept {
    if (!std::isfinite(q) || !std::isfinite(step) || step <= 0.0) return false;
    const double abs_q = std::abs(q);
    const double r = abs_q / step;
    if (!std::isfinite(r)) return false;
    const double n = std::round(r);
    if (!std::isfinite(n) || n < 1.0 || n > 0x1p53) return false;
    const double g = n * step;
    if (!std::isfinite(g)) return false;
    const double err = std::abs(abs_q - g);
    const double span = std::max(abs_q, std::abs(g));
    const double ulp = std::nextafter(span, std::numeric_limits<double>::infinity()) - span;
    const double ulp_bound = 4.0 * ulp;
    const double half_step = step / 2.0;
    if (!std::isfinite(err) || !std::isfinite(ulp_bound) || !std::isfinite(half_step))
        return false;
    return err <= ulp_bound && err < half_step;
}

enum class RequestRejectReason : std::uint8_t {
    InvalidQuantity = 0,
    OffGrid = 1,
    InvalidTrigger = 2,
    InvalidCapacity = 3,
    InvalidOwner = 4,
    InvalidQuantityBasis = 5,
    InvalidGroup = 6,
};

enum class SubmitStatus { Accepted, Rejected };
enum class ReplaceStatus { Replaced, ReplaceRejected, NotWorking, InvalidHandle };
enum class CancelStatus { Cancelled, NotWorking, InvalidHandle };

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Rejected;
    uint64_t event_ordinal = 0;
    std::optional<RequestHandle> handle;
    std::optional<RequestRejectReason> reason;
};

struct ReplaceResult {
    ReplaceStatus status = ReplaceStatus::InvalidHandle;
    uint64_t event_ordinal = 0;
    std::optional<RequestHandle> successor;
    std::optional<RequestRejectReason> reason;
};

struct CancelResult {
    CancelStatus status = CancelStatus::InvalidHandle;
    uint64_t event_ordinal = 0;
};

enum class MatchRejectReason : std::uint8_t {
    NonpositivePrice = 0,
    OpeningDirection = 1,
    MaxAbsUnits = 2,
    MaxOpenLots = 3,
    InitialMargin = 4,
};

enum class CancelReason : std::uint8_t {
    User = 0,
    Group = 1,
    OwnerGone = 2,
    UnsupportedRelation = 3,
};

enum class AppliedTerminalReason : std::uint8_t {
    WorkingUnitsSatisfied = 0,
    Flattened = 1,
    TargetExhausted = 2,
};

enum class ActivationKind : std::uint8_t {
    Stop = 0,
    StopLimit = 1,
    TrailArm = 2,
    TrailTrigger = 3,
};

enum class CoreFailure : std::uint8_t {
    Invariant = 0,
    InvalidCause = 1,
    NonrepresentableQuantity = 2,
    MissingObservation = 3,
    StaleHandle = 4,
    UnsupportedTransition = 5,
    ConflictingReceipt = 6,
    UnrepresentableReservation = 7,
    InvalidScope = 8,
    InvalidProposal = 9,
    ObservationMismatch = 10,
};

enum class DriverEligibilityClass : std::uint8_t {
    ObservedPrint = 0,
    CarriedOpen = 1,
    TickAfterCalculation = 2,
    ConfirmedOpen = 3,
    ConfirmedExcursion = 4,
    ConfirmedAfterCalculationClose = 5,
};

enum class CommandSurface : std::uint8_t { General = 0, MarketOnly = 1 };

struct AcceptedEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    CommandSurface surface = CommandSurface::General;
    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
    const Birth& birth() const noexcept { return definition->birth; }
};

struct RejectedEvent {
    uint64_t ordinal = 0;
    Request request;
    RequestRejectReason reason = RequestRejectReason::InvalidQuantity;
    CommandSurface surface = CommandSurface::General;
};

struct ReplacedEvent {
    uint64_t ordinal = 0;
    DefinitionRef predecessor_definition;
    DefinitionRef successor_definition;
    CommandSurface surface = CommandSurface::General;
    const RequestHandle& predecessor() const noexcept { return predecessor_definition->handle; }
    const Request& predecessor_request() const noexcept { return predecessor_definition->request; }
    const RequestHandle& successor() const noexcept { return successor_definition->handle; }
    const Request& successor_request() const noexcept { return successor_definition->request; }
    const Birth& successor_birth() const noexcept { return successor_definition->birth; }
};

struct ReplaceRejectedEvent {
    uint64_t ordinal = 0;
    DefinitionRef live_definition;
    Request attempted;
    RequestRejectReason reason = RequestRejectReason::InvalidQuantity;
    CommandSurface surface = CommandSurface::General;
    const RequestHandle& target() const noexcept { return live_definition->handle; }
    const Request& live_request() const noexcept { return live_definition->request; }
};

struct CancelledEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    CancelReason reason = CancelReason::User;
    std::optional<EventId> cause;
    Authority prior_authority = UnboundBookClose{};
    RemainingProjection unexecuted = RemainingProjectionUnbound{};
    PendingAdjustments pending = PendingNone{};
    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
};

struct NotWorkingEvent {
    uint64_t ordinal = 0;
    RequestHandle target;
    std::optional<Request> attempted;
    CommandSurface surface = CommandSurface::General;
};

struct InvalidHandleEvent {
    uint64_t ordinal = 0;
    RequestHandle target;
    std::optional<Request> attempted;
    CommandSurface surface = CommandSurface::General;
};

struct NoEffectEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    RemainingProjection remaining = RemainingProjectionUnbound{};
    Authority authority = UnboundBookClose{};
    MatchCursor cursor{};
    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
    const Birth& birth() const noexcept { return definition->birth; }
};

struct MatchRejectedEvent {
    uint64_t ordinal = 0;
    MatchRejectReason reason = MatchRejectReason::OpeningDirection;
    DefinitionRef definition;
    RemainingProjection remaining = RemainingProjectionUnbound{};
    Authority authority = UnboundBookClose{};
    MatchCursor cursor{};
    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
    const Birth& birth() const noexcept { return definition->birth; }
};

struct CloseBoundEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    int64_t cycle = 0;
    Side side = Side::Long;
    MatchCursor cursor{};
    Authority before = UnboundBookClose{};
    Authority after = UnboundBookClose{};
};

struct ActivatedEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    ActivationKind kind = ActivationKind::Stop;
    TriggerState before = StopIdle{};
    TriggerState after = StopActive{};
    double reached_price = 0.0;
    MatchCursor cursor{};
};

struct ExecutionAppliedEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    double raw_price = 0.0;
    double resolved_price = 0.0;
    double current_ticket = 0.0;
    std::size_t first_trade_index = 0;
    std::size_t closed_trade_count = 0;
    uint64_t opened_lot_incarnation = 0;
    double closed_units = 0.0;
    double opened_units = 0.0;
    double filled_working = 0.0;
    RemainingProjection remaining_before = RemainingProjectionUnbound{};
    RemainingProjection remaining_after = RemainingProjectionUnbound{};
    Allowance allowance_before = AllowanceUnset{};
    Allowance allowance_after = AllowanceUnset{};
    bool terminal = true;
    std::optional<AppliedTerminalReason> terminal_reason;
    int64_t cycle_before = 0;
    int64_t cycle_after = 0;
    execution::CloseScope scope = execution::Book{};
    MatchCursor cursor{};
    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
    const Birth& birth() const noexcept { return definition->birth; }
    int64_t effective_time_ms() const noexcept { return cursor.point.effective_time_ms; }
    int64_t interval_open_ms() const noexcept { return cursor.point.open_ms; }
    int64_t interval_last_traded_close_ms() const noexcept {
        return cursor.point.last_traded_close_ms;
    }
    int interval_index() const noexcept { return cursor.point.interval_index; }
    std::uint8_t provenance() const noexcept {
        return static_cast<std::uint8_t>(cursor.point.provenance);
    }
};

struct ReservationReducedEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    EventId cause;
    RequestHandle recipient;
    GroupEffect effect = GroupEffect::Reduce;
    double requested_delta = 0.0;
    double actual_deduction = 0.0;
    RemainingUnits before{};
    RemainingProjection after = RemainingProjectionUnits{};
};

struct DeferredGroupAdjustmentEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    EventId cause;
    RequestHandle recipient;
    GroupEffect effect = GroupEffect::Reduce;
    double deferred_delta = 0.0;
    PendingAdjustments pending_before = PendingNone{};
    PendingDeferred pending_after{};
    std::optional<EventId> previous_pending_receipt;
};

struct QuantityBoundEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    EventId source;
    double source_units = 0.0;
    std::vector<EventId> prior_adjustment_ids;
    double pending_total = 0.0;
    double effective_deduction = 0.0;
    RemainingProjection remaining = RemainingProjectionUnits{};
};

struct ArmedEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    Authority before = Wait{};
    Authority after = ArmedTransaction{};
    Enrollment enrollment = EnrollmentFromApplied{};
    std::optional<EventId> quantity_resolution;
};

using CommandEvent = std::variant<AcceptedEvent,
                                  RejectedEvent,
                                  ReplacedEvent,
                                  ReplaceRejectedEvent,
                                  CancelledEvent,
                                  NotWorkingEvent,
                                  InvalidHandleEvent,
                                  NoEffectEvent,
                                  MatchRejectedEvent,
                                  ExecutionAppliedEvent,
                                  CloseBoundEvent,
                                  ActivatedEvent,
                                  ReservationReducedEvent,
                                  DeferredGroupAdjustmentEvent,
                                  QuantityBoundEvent,
                                  ArmedEvent>;

struct CommandContext {
    int64_t decision_time_ms = 0;
    std::optional<double> quantity_grid;
    std::optional<OpeningObservation> opening;
    CommandSurface surface = CommandSurface::General;
};

struct EvaluationContext {
    MatchCursor cursor{};
    DriverEligibilityClass driver_class = DriverEligibilityClass::ObservedPrint;
    bool existing_matching_bit = false;
};

struct BeginTrailTracking {
    MatchCursor cursor{};
    double reached_price = 0.0;
};
struct ObserveTrailExtremum {
    MatchCursor cursor{};
    double reached_price = 0.0;
};
struct ActivateStop {
    MatchCursor cursor{};
    double reached_price = 0.0;
};
struct ActivateStopLimit {
    MatchCursor cursor{};
    double reached_price = 0.0;
};
struct ActivateTrail {
    MatchCursor cursor{};
    double reached_price = 0.0;
};
using TriggerTransition = std::variant<BeginTrailTracking, ObserveTrailExtremum, ActivateStop,
                                       ActivateStopLimit, ActivateTrail>;

struct ExecutionProposal {
    MatchCursor cursor{};
    double raw_price = 0.0;
    double resolved_price = 0.0;
    execution::Action physical_action{};
    execution::CloseScope scope = execution::Book{};
    PositionIdentity pre_fill = PositionFlat{};
    double inspected_closed_units = 0.0;
    double inspected_opened_units = 0.0;
    double inspected_current_ticket = 0.0;
};

struct CommittedExecutionFacts {
    execution::Result result{};
    int64_t cycle_before = 0;
    int64_t cycle_after = 0;
    TargetObservation post_target{};
    execution::Action committed_action{};
};

struct EventRange {
    std::size_t first_index = 0;
    std::size_t count = 0;
};

struct Installed {
    EventRange events{};
};

enum class InstallError : std::uint8_t {
    StalePreparation = 0,
    WrongCoreOrRun = 1,
    AlreadyConsumed = 2,
};

using InstallResult = std::variant<Installed, InstallError>;

enum class NoChangeReason : std::uint8_t {
    NoTransition = 0,
    StillWaiting = 1,
    NotEligible = 2,
    NotWorking = 3,
    AlreadyApplied = 4,
};

struct NoChange {
    NoChangeReason reason = NoChangeReason::NoTransition;
};

struct PreparationError {
    CoreFailure code = CoreFailure::Invariant;
    EventId cause;
    RequestHandle target;
};

template <class T>
using Preparation = std::variant<T, NoChange, PreparationError>;

template <class R>
struct CommandInstalled {
    R result{};
    EventRange events{};
};

template <class R>
using InstalledCommand = std::variant<CommandInstalled<R>, InstallError>;

class PreparedSubmit;
class PreparedReplace;
class PreparedCancel;
class PreparedMutation;
class PreparedExecution;

struct EligibilityFacts {
    bool birth_ok = false;
    bool waiting = false;
    bool driver_ok = false;
    bool needs_close_bind = false;
    bool ready_to_match = false;
    Side position_side = Side::Long;
    bool is_buy = false;
    const Trigger* trigger = nullptr;
    const TriggerState* trigger_state = nullptr;
    const Remaining* remaining = nullptr;
    const Allowance* allowance = nullptr;
    const Authority* authority = nullptr;
};

class WorkingRequestCore {
public:
    explicit WorkingRequestCore(RunIdentity identity);
    WorkingRequestCore(const WorkingRequestCore&) = delete;
    WorkingRequestCore& operator=(const WorkingRequestCore&) = delete;
    // Move transfers the complete run. The source becomes empty/unbound;
    // commands throw invalid_argument without mutation until reset rebinds it.
    // Self move-assignment preserves the current run.
    WorkingRequestCore(WorkingRequestCore&& other) noexcept;
    WorkingRequestCore& operator=(WorkingRequestCore&& other) noexcept;

    void reset(RunIdentity identity);

    const RunIdentity& identity() const noexcept { return identity_; }
    const std::vector<LiveRequest>& live() const noexcept { return live_; }
    const std::vector<CommandEvent>& history() const noexcept { return history_; }
    const LiveRequest* find_live(const RequestHandle& handle) const;
    const CommandEvent* event_at(const EventId& id) const;

    // R1 producer convenience: prepare then install one command. BindOpening
    // enrollment still requires CommandContext.opening via prepare_submit.
    SubmitResult submit(const Request& request,
                        int64_t decision_time_ms,
                        uint64_t& next_order_incarnation,
                        uint64_t& next_timeline_ordinal,
                        std::optional<double> quantity_grid = std::nullopt);

    ReplaceResult replace(const RequestHandle& target,
                          const Request& request,
                          int64_t decision_time_ms,
                          uint64_t& next_order_incarnation,
                          uint64_t& next_timeline_ordinal,
                          std::optional<double> quantity_grid = std::nullopt);

    CancelResult cancel(const RequestHandle& target, uint64_t& next_timeline_ordinal);

    // Consumer-only typed prepare/install. Prepare may reserve storage and
    // invalidate live/history references; it does not change logical state.
    // Tokens are move-only, bound to this instance/run/epoch, and do not
    // survive another prepare/install, reset, or move.
    PreparedSubmit prepare_submit(const Request& request,
                                  const CommandContext& context,
                                  uint64_t& next_order_incarnation,
                                  uint64_t& next_timeline_ordinal);
    PreparedReplace prepare_replace(const RequestHandle& target,
                                    const Request& request,
                                    const CommandContext& context,
                                    uint64_t& next_order_incarnation,
                                    uint64_t& next_timeline_ordinal);
    PreparedCancel prepare_cancel(const RequestHandle& target, uint64_t& next_timeline_ordinal);

    InstalledCommand<SubmitResult> install_submit(PreparedSubmit&& prepared) noexcept;
    InstalledCommand<ReplaceResult> install_replace(PreparedReplace&& prepared) noexcept;
    InstalledCommand<CancelResult> install_cancel(PreparedCancel&& prepared) noexcept;

    Preparation<PreparedMutation> prepare_evaluation(const RequestHandle& target,
                                                     const EvaluationContext& context,
                                                     const TargetObservation& observation,
                                                     uint64_t& next_timeline_ordinal);
    Preparation<PreparedMutation> prepare_trigger(const RequestHandle& target,
                                                  const TriggerTransition& transition,
                                                  DriverEligibilityClass driver_class,
                                                  uint64_t& next_timeline_ordinal);
    InstallResult install_mutation(PreparedMutation&& prepared) noexcept;

    Preparation<PreparedExecution> prepare_execution(const RequestHandle& target,
                                                     const ExecutionProposal& proposal,
                                                     uint64_t& next_timeline_ordinal);
    InstallResult install_execution(PreparedExecution&& prepared,
                                    const CommittedExecutionFacts& facts) noexcept;
    Preparation<PreparedMutation> prepare_no_effect(const RequestHandle& target,
                                                    const EvaluationContext& context,
                                                    uint64_t& next_timeline_ordinal);
    Preparation<PreparedMutation> prepare_match_rejected(const RequestHandle& target,
                                                         const EvaluationContext& context,
                                                         MatchRejectReason reason,
                                                         uint64_t& next_timeline_ordinal);

    std::vector<RequestHandle> group_recipients(const EventId& applied) const;
    std::vector<RequestHandle> waiting_children(const RequestHandle& parent) const;
    std::vector<RequestHandle> bound_close_handles() const;

    Preparation<PreparedMutation> prepare_group_effect(const EventId& applied,
                                                       const RequestHandle& recipient,
                                                       uint64_t& next_timeline_ordinal);
    Preparation<PreparedMutation> prepare_owner_applied(
            const EventId& applied,
            const RequestHandle& child,
            const std::optional<OpeningObservation>& observation,
            uint64_t& next_timeline_ordinal);
    Preparation<PreparedMutation> prepare_bound_expiry(const EventId& physical_cause,
                                                       const RequestHandle& child,
                                                       const TargetObservation& observation,
                                                       uint64_t& next_timeline_ordinal);
    Preparation<PreparedMutation> prepare_parent_terminal(const EventId& terminal_or_replaced,
                                                          const RequestHandle& child,
                                                          uint64_t& next_timeline_ordinal);

    EligibilityFacts eligibility_facts(const LiveRequest& live,
                                       const EvaluationContext& context) const noexcept;
    bool evaluation_eligible(const LiveRequest& live,
                             const EvaluationContext& context) const noexcept;
    bool trigger_permits_driver(const Trigger& trigger,
                                const TriggerState& state,
                                DriverEligibilityClass driver_class,
                                bool existing_matching_bit) const noexcept;
    bool working_is_buy(const LiveRequest& live) const noexcept;

private:
    enum class TargetKind { Live, NotWorking, InvalidHandle };
    friend class PreparedSubmit;
    friend class PreparedReplace;
    friend class PreparedCancel;
    friend class PreparedMutation;
    friend class PreparedExecution;

    static void require_identity(const RunIdentity& identity);
    static void require_grid(std::optional<double> quantity_grid);
    static void require_distinct_counters(uint64_t& next_order_incarnation,
                                          uint64_t& next_timeline_ordinal);
    std::optional<RequestRejectReason> validate_request(
            const Request& request,
            const CommandContext& context,
            const std::optional<RequestHandle>& replace_target) const;
    LiveRequest make_live(DefinitionRef definition,
                          const CommandContext& context,
                          EventId accepted) const;
    enum class ReceiptLookup : std::uint8_t { Absent = 0, Present = 1, Conflict = 2 };
    bool collect_pending_chain(const PendingAdjustments& pending,
                               const RequestHandle& recipient,
                               std::vector<EventId>* ids,
                               double* total) const;
    ReceiptLookup receipt_lookup(const EventId& cause, const RequestHandle& recipient,
                                 GroupEffect effect, uint64_t* outcome) const;
    bool authenticate_receipt_outcome(const CommandEvent& event, const EventId& cause,
                                      const RequestHandle& recipient, GroupEffect effect) const;
    bool trail_level_ok(double best, double offset, bool is_buy, double* stop) const noexcept;

    uint64_t usable_ordinal(uint64_t next) const;
    uint64_t usable_incarnation(uint64_t next) const;
    TargetKind classify(const RequestHandle& handle, std::size_t* live_index) const;
    bool bump_epoch() noexcept;
    void require_epoch_room() const;
    void clear_unbound() noexcept;

    struct InstanceBinding {
        uint64_t generation = 1;
        bool expired = false;
    };
    struct MutationPlan {
        std::weak_ptr<InstanceBinding> instance;
        uint64_t generation = 0;
        uint64_t epoch = 0;
        RunIdentity run;
        std::size_t history_size = 0;
        uint64_t last_ordinal = 0;
        bool consumed = false;
        std::vector<CommandEvent> events;
        std::uint8_t live_change = 0;  // 0 none, 1 push, 2 erase, 3 update
        std::size_t live_index = 0;
        LiveRequest live_row{};
        bool consume_incarnation = false;
        uint64_t incarnation_used = 0;
        bool add_receipt = false;
        EventId receipt_cause{};
        RequestHandle receipt_recipient{};
        GroupEffect receipt_effect = GroupEffect::Reduce;
        uint64_t receipt_outcome = 0;
    };
    std::optional<InstallError> validate_plan(const MutationPlan& plan) const noexcept;
    InstallResult commit(MutationPlan& plan) noexcept;
    MutationPlan begin_plan() const;
    void reserve_plan(const MutationPlan& plan);
    void bind_plan(MutationPlan& plan) const;
    void seal_plan(MutationPlan& plan);
    PreparedMutation finish_mutation(MutationPlan plan);

    RunIdentity identity_;
    std::shared_ptr<InstanceBinding> instance_;
    std::vector<LiveRequest> live_;
    std::vector<CommandEvent> history_;
    uint64_t last_ordinal_ = 0;
    uint64_t last_incarnation_ = 0;
    uint64_t epoch_ = 0;
    std::vector<std::pair<uint64_t, std::size_t>> ordinal_index_;
    struct ReceiptKey {
        EventId cause;
        RequestHandle recipient;
        GroupEffect effect = GroupEffect::Reduce;
        uint64_t outcome_ordinal = 0;
    };
    std::vector<ReceiptKey> receipts_;
};

class PreparedSubmit {
public:
    PreparedSubmit() noexcept;
    PreparedSubmit(PreparedSubmit&&) noexcept;
    PreparedSubmit& operator=(PreparedSubmit&&) noexcept;
    ~PreparedSubmit();
    PreparedSubmit(const PreparedSubmit&) = delete;
    PreparedSubmit& operator=(const PreparedSubmit&) = delete;
    explicit operator bool() const noexcept;
    const SubmitResult& predicted() const;
    EventId predicted_event_id() const;

private:
    friend class WorkingRequestCore;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PreparedSubmit(std::unique_ptr<Impl> impl) noexcept;
};

class PreparedReplace {
public:
    PreparedReplace() noexcept;
    PreparedReplace(PreparedReplace&&) noexcept;
    PreparedReplace& operator=(PreparedReplace&&) noexcept;
    ~PreparedReplace();
    PreparedReplace(const PreparedReplace&) = delete;
    PreparedReplace& operator=(const PreparedReplace&) = delete;
    explicit operator bool() const noexcept;
    const ReplaceResult& predicted() const;
    EventId predicted_event_id() const;

private:
    friend class WorkingRequestCore;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PreparedReplace(std::unique_ptr<Impl> impl) noexcept;
};

class PreparedCancel {
public:
    PreparedCancel() noexcept;
    PreparedCancel(PreparedCancel&&) noexcept;
    PreparedCancel& operator=(PreparedCancel&&) noexcept;
    ~PreparedCancel();
    PreparedCancel(const PreparedCancel&) = delete;
    PreparedCancel& operator=(const PreparedCancel&) = delete;
    explicit operator bool() const noexcept;
    const CancelResult& predicted() const;
    EventId predicted_event_id() const;

private:
    friend class WorkingRequestCore;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PreparedCancel(std::unique_ptr<Impl> impl) noexcept;
};

class PreparedMutation {
public:
    PreparedMutation() noexcept;
    PreparedMutation(PreparedMutation&&) noexcept;
    PreparedMutation& operator=(PreparedMutation&&) noexcept;
    ~PreparedMutation();
    PreparedMutation(const PreparedMutation&) = delete;
    PreparedMutation& operator=(const PreparedMutation&) = delete;
    explicit operator bool() const noexcept;

private:
    friend class WorkingRequestCore;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PreparedMutation(std::unique_ptr<Impl> impl) noexcept;
};

class PreparedExecution {
public:
    PreparedExecution() noexcept;
    PreparedExecution(PreparedExecution&&) noexcept;
    PreparedExecution& operator=(PreparedExecution&&) noexcept;
    ~PreparedExecution();
    PreparedExecution(const PreparedExecution&) = delete;
    PreparedExecution& operator=(const PreparedExecution&) = delete;
    explicit operator bool() const noexcept;

private:
    friend class WorkingRequestCore;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PreparedExecution(std::unique_ptr<Impl> impl) noexcept;
};

static_assert(std::is_nothrow_move_constructible_v<Request>);
static_assert(std::is_nothrow_move_assignable_v<Request>);
static_assert(std::is_nothrow_move_constructible_v<LiveRequest>);
static_assert(std::is_nothrow_move_assignable_v<LiveRequest>);
static_assert(std::is_nothrow_move_constructible_v<CommandEvent>);
static_assert(std::is_nothrow_move_assignable_v<CommandEvent>);
static_assert(std::is_nothrow_move_constructible_v<SubmitResult>);
static_assert(std::is_nothrow_move_constructible_v<ReplaceResult>);
static_assert(std::is_nothrow_move_constructible_v<CancelResult>);
static_assert(std::is_nothrow_move_constructible_v<NoEffectEvent>);
static_assert(std::is_nothrow_move_constructible_v<MatchRejectedEvent>);
static_assert(std::is_nothrow_move_constructible_v<ExecutionAppliedEvent>);
static_assert(std::is_nothrow_move_constructible_v<MatchCursor>);

}  // inline namespace native_order_v2
}  // namespace pineforge::native_order
