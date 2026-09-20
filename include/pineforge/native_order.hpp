#pragma once

#include "execution.hpp"
#include "execution_reverse_to.hpp"
#include "execution_close_scope.hpp"
#include "market_driver.hpp"
#include "native_order_identity.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge::native_order {
inline namespace native_order_v6 {

// Isolated working-request/value core: current LIVE requests and immutable
// command history. It does not own positions, cash, paid fees, matching,
// calendar, host phase, or a second physical book.
//
// Identity types remain native_order_v1. Request/core/event values are v6.
// Physical execution::Action is unchanged; native Reduce uses a typed size
// source instead of a dummy units field. ExecutionPlan is a transient widening
// used at the core/consumer boundary.

using Flatten = execution::Flatten;
using Transact = order_action::Transact;

// A host-maintained, run-scoped roster identity.  Zero is invalid and is
// never allocated by WorkingRequestCore.
struct CohortHandle {
    std::uint64_t value = 0;
};
inline bool operator==(CohortHandle left, CohortHandle right) noexcept {
    return left.value == right.value;
}
inline bool operator!=(CohortHandle left, CohortHandle right) noexcept {
    return !(left == right);
}
inline bool operator<(CohortHandle left, CohortHandle right) noexcept {
    return left.value < right.value;
}

// Exact target exposure for an explicit reversal request. This is distinct
// from execution::ReverseTo, which is the transient resolved execution plan.
struct ReverseTo {
    double signed_units = 0.0;
};

enum class HostSizedKind : std::uint8_t { Open = 0, Close = 1 };
enum class Side : std::uint8_t { Long = 0, Short = 1 };
struct HostSized {
    HostSizedKind kind = HostSizedKind::Open;
    std::optional<Side> side;
};

// Resolved host sizing normally remains subject to the run's quantity grid.
// A host may instead authenticate literal units for a pure reduction; the
// consumer still proves that the positive quantity is representable within
// the selected exposure before it can reach settlement.
enum class ExecutionGridPolicy : std::uint8_t {
    SnapToGrid = 0,
    ExplicitUnits = 1,
};

// Native sizing bases (L3).  The kernel resolves them into units at the
// sizing point; the Pine adapter never emits them and keeps HostSized.
struct CashValue {
    double cash = 0.0;                 // account currency
};
struct EquityFraction {
    double fraction = 0.0;             // of marked equity at the sizing point (0.10 = 10 %)
};
using SizeBasis = std::variant<CashValue, EquityFraction>;

// AtMatch resolves the basis at the matching candidate; AtAcceptance freezes
// the resolved units when the request is accepted.
enum class SizeTime : std::uint8_t { AtMatch = 0, AtAcceptance = 1 };

// WHICH price the basis is converted at. This is the generic "size against the
// expected fill price" rule; it names no source language.
//
// Resolved (today's behaviour) uses the price the kernel would otherwise
// settle at: the candidate's default resolved price for AtMatch, the raw
// acceptance-point price for AtAcceptance.
//
// Signal is the decision-point price at placement -- the current bar's close,
// or the last print, at submit -- carried to the expected market fill: it is
// adjusted by the run's slippage for the request's own side
// (price +/- slippage_ticks * price_tick) and then rounded onto the run's
// price grid when NativeRunSpec::price_grid is on (HalfUp is the nearest
// tick; NativePriceGrid::None leaves it unrounded). It is frozen when the
// request is accepted, so it composes with SizeTime::AtAcceptance and, with
// SizeTime::AtMatch, the basis still converts at that frozen signal price at
// every later candidate. A Signal request accepted outside a decision point
// has no price to freeze and stays unresolvable.
enum class SizePrice : std::uint8_t { Resolved = 0, Signal = 1 };

// A kernel-sized opening.  units = cash / (price * point_value * fx), where
// cash is the basis value or fraction * marked equity, and price is the
// sizing-point price SizePrice names.  A host override of
// resolve_execution_terms still has the last word: it sees the kernel-resolved
// units as the facts' RemainingUnits and may return its own.
//
// SizeTime chooses WHEN the basis is resolved, SizePrice chooses WHICH price
// it converts at, and the two compose.  Either way the request reaches the
// candidate with a deferred quantity and exactly one terms pass: the kernel
// resolves (or republishes the acceptance-frozen) units, publishes them as the
// facts' RemainingUnits before resolve_execution_terms and
// validate_execution_precommit run, and uses the host's units when the host
// returns any.  An acceptance-time quantity is additionally an admission input
// at placement, so an opening the run cannot admit is
// RequestRejectReason::PlacementAdmission at submit rather than a rejected
// candidate later.
struct Sized {
    Side side = Side::Long;
    SizeBasis basis{};
    SizeTime time = SizeTime::AtMatch;
    // Which price the basis converts at; see SizePrice. Appended after `time`
    // because Sized has no positional aggregate initializer outside tests that
    // stop at the basis.
    SizePrice price = SizePrice::Resolved;
    // SnapToGrid floors the resolved units onto the run's quantity grid.
    // ExplicitUnits keeps the literal quotient, which the ordinary on-grid
    // terms gate then refuses when a grid is configured and the quotient is
    // off it; the two are identical on an ungridded run.
    ExecutionGridPolicy grid_policy = ExecutionGridPolicy::SnapToGrid;
    // Divide the sizing cash by (1 + fee) when the run's fee kind is
    // NativeFeeKind::Percent.  Every other fee kind is an exact no-op.
    bool reserve_percent_fee = false;
};

struct ExplicitUnits {
    double units = 0.0;
};
struct OwnerOpenedUnits {};
// Gross claims the whole bound scope; NetOfSiblings first subtracts the units
// already claimed by the live sibling reduces bound to that same scope.  The
// keys are opaque request/owner handles, never source identifiers.
enum class ScopeClaim : std::uint8_t { Gross = 0, NetOfSiblings = 1 };
// Which measurement of the bound scope the fraction is taken of. AtMatch (the
// default) reads the scope as it stands at the matching candidate.
// AtAcceptance freezes the scope SIZE when the request is accepted -- the
// placement-time live basis -- so two 50 % siblings on one 10-unit lot both
// claim 5 under Gross even after the first has already executed. NetOfSiblings
// then subtracts the live sibling claims from that frozen basis.
enum class ScopeBasis : std::uint8_t { AtMatch = 0, AtAcceptance = 1 };
// A fraction in (0, 1] of the bound scope, resolved at the matching candidate.
// The resolution is units = scope * fraction, one binary64 multiplication: a
// caller that spells its size as a percent converts percent -> fraction
// itself, so no second rounding step is introduced here.
struct ScopeFraction {
    double fraction = 1.0;
    ScopeClaim claim = ScopeClaim::Gross;
    // Appended last so every existing aggregate initializer keeps its meaning.
    ScopeBasis basis = ScopeBasis::AtMatch;
};
using ReductionSize = std::variant<ExplicitUnits, OwnerOpenedUnits, ScopeFraction>;
struct Reduce {
    ReductionSize size;
};
using OrderIntent = std::variant<Flatten, Reduce, Transact, ReverseTo, HostSized, Sized>;

struct Market {};
// `fill_through` makes the limit a touch trigger (market-if-touched): the
// level still gates when the request becomes executable, but its fill is not
// bounded by the level, so slippage may carry it past the level.
struct Limit {
    double price = 0.0;
    bool fill_through = false;
};
struct Stop {
    double price = 0.0;
};
struct StopLimit {
    double stop = 0.0;
    double limit = 0.0;
};
// Trail offset spelled in price ticks instead of a price distance. The
// acceptance path resolves it against the run's price tick, writes the
// product into Trail::offset and clears the spelling, so a stored definition
// always carries a plain price distance and can never resolve twice.
struct TrailTicks {
    double ticks = 0.0;
};
// offset is a price distance: the stop rides `offset` behind the running
// best. Zero is legal and means "ride the best": the exit is the first
// adverse move past it. Negative and nonfinite offsets are rejected.
struct Trail {
    double offset = 0.0;
    std::optional<double> arm_price;
    std::optional<TrailTicks> ticks = std::nullopt;
};
using Trigger = std::variant<Market, Limit, Stop, StopLimit, Trail>;

// Where a request's trigger level comes from. Absolute is the level written
// in the trigger itself. FromOwnerFill defers it to the owner's fill: the
// level becomes fill + offset when the owner arms the request, so a bracket
// leg can be placed before its parent has a price. offset is signed (adverse
// is negative) and is spelled in price ticks when `ticks` is set; acceptance
// resolves a tick spelling exactly like TrailTicks, in place, once.
struct Absolute {};
struct FromOwnerFill {
    double offset = 0.0;
    bool ticks = false;
};
using TriggerAnchor = std::variant<Absolute, FromOwnerFill>;

// Replacement behaviour that is not expressible in the successor request.
// retain_trigger_state carries the predecessor's live trigger state (a
// tracking trail's best, an already active stop) into the successor instead
// of restarting it; predecessor and successor must hold the same trigger
// alternative.
struct ReplaceOptions {
    bool retain_trigger_state = false;
};

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
struct BindOpenings {
    std::vector<RequestHandle> openings;
    int64_t cycle = 0;
};
struct BindCohort {
    CohortHandle cohort;
};
using Owner = std::variant<Independent, WaitForApplied, BindOpening, BindOpenings, BindCohort>;

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
    TriggerAnchor anchor = Absolute{};
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
        && std::holds_alternative<NoGroup>(request.group)
        && std::holds_alternative<Absolute>(request.anchor);
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

struct RemainingUnbound {};
struct RemainingFlattenAll {};
struct RemainingUnits {
    double q = 0.0;
};
struct RemainingDeferred {};
// A dynamic cohort has no currently live member.  This is a live deferral,
// not a terminal receipt: the consumer retries it at the next candidate.
struct NoTarget {};
using Remaining = std::variant<RemainingUnbound, RemainingFlattenAll, RemainingUnits,
                               RemainingDeferred, NoTarget>;

struct RemainingProjectionUnbound {};
struct RemainingProjectionFlattenAll {};
struct RemainingProjectionUnits {
    double q = 0.0;
};
struct RemainingProjectionDeferred {};
struct RemainingProjectionNoTarget {};
using RemainingProjection =
        std::variant<RemainingProjectionUnbound, RemainingProjectionFlattenAll,
                     RemainingProjectionUnits, RemainingProjectionDeferred,
                     RemainingProjectionNoTarget>;

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
// Immutable fixed cohort. Current physical liveness is observed, never stored
// here; later fragments of an enrolled provenance remain authorized.
struct OpeningsClose {
    std::vector<RequestHandle> openings;
    int64_t cycle = 0;
    Side side = Side::Long;
    Enrollment enrollment;
};
struct CohortClose {
    CohortHandle cohort;
};
using Authority = std::variant<BookTransaction, Wait, ArmedTransaction, UnboundBookClose, BookClose,
                               OpeningClose, OpeningsClose, CohortClose>;

// Native authorization receipt, converted to a call-local financial
// SelectedOpeningSet only at the consumer's settlement boundary.
struct SelectedExposure {
    int64_t cycle = 0;
    std::vector<uint64_t> incarnations;
};
using ExecutionScope = std::variant<execution::Book, execution::OpeningExposure, SelectedExposure>;

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
struct AllowanceDeferred {
    uint64_t point_ordinal = 0;
};
using Allowance = std::variant<AllowanceUnset, AllowanceUnits, AllowanceAllScope,
                               AllowanceDeferred>;

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
    std::vector<OpeningObservation> openings;
};

// Who authored a request. Host is every request a host submits, replaces or
// cancels, and it is the whole existing population: it folds nothing into the
// continuation digest, so no established hash moves. KernelLiquidation marks
// the margin model's own Reduce; KernelRisk is reserved for the risk lane.
enum class RequestOrigin : std::uint8_t {
    Host = 0,
    KernelLiquidation = 1,
    KernelRisk = 2,
};

struct RequestDefinition {
    RequestHandle handle;
    Request request;
    Birth birth;
    std::optional<RequestHandle> predecessor;
    // Appended last so every existing aggregate initializer keeps its meaning.
    RequestOrigin origin = RequestOrigin::Host;
};
using DefinitionRef = std::shared_ptr<const RequestDefinition>;

struct LiveRequest {
    DefinitionRef definition;
    Remaining remaining = RemainingUnbound{};
    Authority authority = UnboundBookClose{};
    TriggerState trigger_state = MarketReady{};
    Allowance allowance = AllowanceUnset{};
    PendingAdjustments pending = PendingNone{};
    // Placement-time sizing measurements frozen from the accepting command
    // context. Both stay empty for every request that did not ask to freeze
    // one, and an empty optional folds nothing into the continuation digest.
    std::optional<double> sizing_units;   // SizeTime::AtAcceptance
    std::optional<double> sizing_scope;   // ScopeBasis::AtAcceptance
    std::optional<double> sizing_price;   // SizePrice::Signal

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
    // A Sized{SizeTime::AtAcceptance} whose acceptance-resolved quantity does
    // not pass the run's opening admission (allowed directions, max_abs_units,
    // max_open_lots, initial margin) at the sizing price.
    PlacementAdmission = 7,
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
    TermsUnresolved = 5,
    InvalidTerms = 6,
    NoOppositeExposure = 7,
    HostPrecommit = 8,
};

enum class NativeCandidatePriceKind : std::uint8_t {
    PointPrice = 0,
    TriggerLevel = 1,
    CurrentQuote = 2,
};

enum class OpeningShape : std::uint8_t {
    Transact = 0,
    ReverseTo = 1,
    CloseOpposite = 2,
};

struct ExecutionTerms {
    double resolved_price = 0.0;
    std::optional<double> units;
    OpeningShape shape = OpeningShape::Transact;
    ExecutionGridPolicy grid_policy = ExecutionGridPolicy::SnapToGrid;
};

using ExecutionPlan = std::variant<execution::Flatten, order_action::Reduce,
                                   order_action::Transact, execution::ReverseTo>;

inline ExecutionPlan to_execution_plan(const execution::Action& action) {
    return std::visit([](const auto& alternative) -> ExecutionPlan {
        return alternative;
    }, action);
}

struct TermsResolvedInput {
    NativeCandidatePriceKind price_kind = NativeCandidatePriceKind::PointPrice;
    bool shared_cursor_collision = false;
    double raw_price = 0.0;
    double default_resolved_price = 0.0;
    ExecutionTerms terms;
};

enum class CancelReason : std::uint8_t {
    User = 0,
    Group = 1,
    OwnerGone = 2,
    UnsupportedRelation = 3,
    // A kernel-originated request the kernel itself withdrew: the liquidation
    // level or its units moved, or the requirement is no longer breached.
    Superseded = 4,
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
    CurrentExecution = 6,
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

struct TermsResolvedEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    MatchCursor cursor{};
    TermsResolvedInput input;
    std::vector<EventId> prior_adjustment_ids;
    double pending_total = 0.0;
    double effective_deduction = 0.0;
    RemainingProjection remaining_before = RemainingProjectionDeferred{};
    RemainingProjection remaining_after = RemainingProjectionUnits{};
    Allowance allowance_after = AllowanceUnset{};
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
    std::optional<ExecutionTerms> attempted_terms;
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
    ExecutionScope scope = execution::Book{};
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

// A kernel-issued liquidation that actually filled. It carries the margin
// facts of that fill, so a host reconstructs the outcome without recomputing
// the account: `mark` is the booked resolved price, `equity` and `required`
// are the marked equity and the maintenance requirement of the SURVIVING book
// at that price, `liquidation_price` is the level re-solved for what is left,
// and `position_before` / `position_after` are the signed book on either side
// of the reduction. `applied` names the ExecutionAppliedEvent that booked it.
struct MarginCallEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    EventId applied;
    MatchCursor cursor{};
    Side side = Side::Long;
    double mark = 0.0;
    double equity = 0.0;
    double required = 0.0;
    double liquidation_price = 0.0;
    double units = 0.0;
    double position_before = 0.0;
    double position_after = 0.0;
    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
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
                                  ArmedEvent,
                                  TermsResolvedEvent,
                                  MarginCallEvent>;

// Almost every prepared command yields one history event. Keep that ordinary
// transactional payload inline; the overflow vector preserves the existing
// arbitrary-length behavior for group/lifecycle plans that emit more events.
class InlineCommandEvents {
public:
    InlineCommandEvents() = default;
    InlineCommandEvents(InlineCommandEvents&& other) noexcept : size_(other.size_) {
        for (std::size_t i = 0; i < size_ && i < inline_.size(); ++i) {
            inline_[i] = std::move(other.inline_[i]);
        }
        overflow_ = std::move(other.overflow_);
        other.size_ = 0;
    }
    InlineCommandEvents& operator=(InlineCommandEvents&& other) noexcept {
        if (this != &other) {
            clear();
            size_ = other.size_;
            for (std::size_t i = 0; i < size_ && i < inline_.size(); ++i) {
                inline_[i] = std::move(other.inline_[i]);
            }
            overflow_ = std::move(other.overflow_);
            other.size_ = 0;
        }
        return *this;
    }
    InlineCommandEvents(const InlineCommandEvents& other) : size_(other.size_) {
        for (std::size_t i = 0; i < size_ && i < inline_.size(); ++i) {
            inline_[i] = other.inline_[i];
        }
        overflow_ = other.overflow_;
    }
    InlineCommandEvents& operator=(const InlineCommandEvents& other) {
        if (this != &other) {
            clear();
            size_ = other.size_;
            for (std::size_t i = 0; i < size_ && i < inline_.size(); ++i) {
                inline_[i] = other.inline_[i];
            }
            overflow_ = other.overflow_;
        }
        return *this;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    template<class Event>
    void emplace_back(Event&& event) {
        push_back(CommandEvent(std::forward<Event>(event)));
    }

    void push_back(CommandEvent event) {
        if (overflow_.empty() && size_ < inline_.size()) {
            inline_[size_++].emplace(std::move(event));
            return;
        }
        if (overflow_.empty()) {
            overflow_.reserve(inline_.size() * 2U);
            for (std::size_t i = 0; i < size_; ++i) {
                overflow_.push_back(std::move(*inline_[i]));
                inline_[i].reset();
            }
        }
        overflow_.push_back(std::move(event));
        ++size_;
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < size_ && i < inline_.size(); ++i) {
            inline_[i].reset();
        }
        overflow_.clear();
        size_ = 0;
    }

    CommandEvent& front() noexcept { return (*this)[0]; }
    const CommandEvent& front() const noexcept { return (*this)[0]; }
    CommandEvent& operator[](std::size_t index) noexcept {
        return overflow_.empty() ? *inline_[index] : overflow_[index];
    }
    const CommandEvent& operator[](std::size_t index) const noexcept {
        return overflow_.empty() ? *inline_[index] : overflow_[index];
    }

private:
    std::array<std::optional<CommandEvent>, 2> inline_{};
    std::vector<CommandEvent> overflow_{};
    std::size_t size_ = 0;
};

struct CommandContext {
    int64_t decision_time_ms = 0;
    std::optional<double> quantity_grid;
    std::optional<OpeningObservation> opening;
    CommandSurface surface = CommandSurface::General;
    std::vector<OpeningObservation> openings;
    // Kernel-resolved acceptance-time units for a Sized{AtAcceptance} request.
    // The execution consumer owns the account facts, so it supplies them here;
    // a producer that leaves it unset accepts the request with a size the
    // matching path then reports as TermsUnresolved.  The accepted request
    // carries the value in LiveRequest::sizing_units and still reaches the
    // candidate with a deferred remaining, so the host keeps its one override
    // pass.  Appended last so the existing positional aggregate initializers
    // keep their meaning.
    std::optional<double> sizing_units;
    // The run's price tick, needed only to resolve a tick-spelled trail
    // offset or trigger anchor. A tick spelling without a usable tick here is
    // rejected rather than silently read as a price distance.
    std::optional<double> price_tick;
    // Placement-time sizing measurements the execution consumer owns, supplied
    // only for the intent that asks for them. Appended last, exactly like
    // sizing_units, so the existing positional aggregate initializers keep
    // their meaning.
    //
    // sizing_scope: the bound scope's exposure at acceptance, for a
    //   Reduce{ScopeFraction{ScopeBasis::AtAcceptance}}.
    // sizing_price: the frozen signal price, for a Sized{SizePrice::Signal}.
    // sizing_admissible: false when the acceptance-resolved quantity of a
    //   Sized{SizeTime::AtAcceptance} fails the run's placement admission.
    std::optional<double> sizing_scope;
    std::optional<double> sizing_price;
    bool sizing_admissible = true;
};

struct EvaluationContext {
    MatchCursor cursor{};
    DriverEligibilityClass driver_class = DriverEligibilityClass::ObservedPrint;
    bool existing_matching_bit = false;
    // Generic current-point delivery. At Open, the consumer sets this only for
    // a market/immediate request born by the pre-open provider. On a continuous
    // OHLC segment, it also admits a request born by an applied callback onto
    // the unconsumed suffix. It is transient and never retained in a request.
    bool pre_open_birth_eligible = false;
    // Set only while resolving a CohortClose candidate.  It carries the
    // physical side of the currently live selected roster and is not retained
    // in a request definition.
    std::optional<Side> cohort_side;
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
    // Carries the generic pre-open delivery authorization from the matching
    // evaluation through the synchronous execution preparation.
    bool pre_open_birth_eligible = false;
    double raw_price = 0.0;
    double resolved_price = 0.0;
    ExecutionPlan physical_action{};
    ExecutionScope scope = execution::Book{};
    PositionIdentity pre_fill = PositionFlat{};
    double inspected_closed_units = 0.0;
    double inspected_opened_units = 0.0;
    double inspected_current_ticket = 0.0;
    TargetObservation pre_target{};
};

struct CommittedExecutionFacts {
    execution::Result result{};
    int64_t cycle_before = 0;
    int64_t cycle_after = 0;
    TargetObservation post_target{};
    ExecutionPlan committed_action{};
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

struct CohortRoster {
    CohortHandle handle{};
    // Canonical origin handles, ordered by origin incarnation rather than by
    // host insertion order.  A successor is normalized to its predecessor
    // root before entering this table.
    std::vector<RequestHandle> origins;
};

enum class CohortReceiptOperation : std::uint8_t { Add = 0, Remove = 1 };
enum class CohortReceiptStatus : std::uint8_t {
    Applied = 0,
    InvalidHandle = 1,
    UnknownOrigin = 2,
    TerminalOrigin = 3,
};
struct CohortReceipt {
    CohortReceiptOperation operation = CohortReceiptOperation::Add;
    CohortReceiptStatus status = CohortReceiptStatus::InvalidHandle;
    CohortHandle cohort{};
    RequestHandle origin{};
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
    const std::vector<CohortRoster>& cohorts() const noexcept { return cohorts_; }
    const std::vector<CohortReceipt>& cohort_receipts() const noexcept {
        return cohort_receipts_;
    }
    const LiveRequest* find_live(const RequestHandle& handle) const;
    const CommandEvent* event_at(const EventId& id) const;

    // Command-boundary roster maintenance.  A rejected add/remove records a
    // durable generic receipt but never emits a market event.
    CohortHandle cohort_open();
    void cohort_add(CohortHandle cohort, RequestHandle origin);
    void cohort_remove(CohortHandle cohort, RequestHandle origin);
    bool cohort_contains(CohortHandle cohort, const RequestHandle& opening) const;

    // R1 producer convenience: prepare then install one command. Bound opening
    // enrollment requires CommandContext observations via prepare_submit.
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
                          std::optional<double> quantity_grid = std::nullopt,
                          ReplaceOptions options = {});

    CancelResult cancel(const RequestHandle& target, uint64_t& next_timeline_ordinal);

    // Consumer-only typed prepare/install. Prepare may reserve storage and
    // invalidate live/history references; it does not change logical state.
    // Tokens are move-only, bound to this instance/run/epoch, and do not
    // survive another prepare/install, reset, or move.
    PreparedSubmit prepare_submit(const Request& request,
                                  const CommandContext& context,
                                  uint64_t& next_order_incarnation,
                                  uint64_t& next_timeline_ordinal,
                                  RequestOrigin origin = RequestOrigin::Host);
    PreparedReplace prepare_replace(const RequestHandle& target,
                                    const Request& request,
                                    const CommandContext& context,
                                    uint64_t& next_order_incarnation,
                                    uint64_t& next_timeline_ordinal,
                                    ReplaceOptions options = {});
    // `reason` lets the kernel withdraw its own request under the durable
    // Superseded receipt. Host cancels keep the default User reason.
    PreparedCancel prepare_cancel(const RequestHandle& target, uint64_t& next_timeline_ordinal,
                                  CancelReason reason = CancelReason::User);

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
                                                  uint64_t& next_timeline_ordinal,
                                                  std::optional<Side> cohort_side = std::nullopt);
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
    Preparation<PreparedMutation> prepare_match_rejected(
            const RequestHandle& target,
            const EvaluationContext& context,
            MatchRejectReason reason,
            std::optional<ExecutionTerms> attempted_terms,
            uint64_t& next_timeline_ordinal);
    Preparation<PreparedMutation> prepare_terms(const RequestHandle& target,
                                                 const EvaluationContext& context,
                                                 const TermsResolvedInput& input,
                                                 uint64_t& next_timeline_ordinal);

    // Append one MarginCallEvent to the immutable history. The target is the
    // kernel-originated request that filled, which is already terminal by the
    // time its margin facts are recorded, so no live row moves.
    Preparation<PreparedMutation> prepare_margin_call(const MarginCallEvent& event,
                                                      uint64_t& next_timeline_ordinal);

    // The allowance that prepare_evaluation would install for this point.
    static Allowance evaluated_allowance(const LiveRequest& live, uint64_t point) noexcept;
    // Consumer-only no-event form of the ordinary allowance
    // refresh. It preserves prepare_evaluation's eligibility and liveness
    // checks while avoiding a transient mutation envelope per driver point.
    void refresh_point_allowances(uint64_t point, const PositionIdentity& position) noexcept;
    bool refresh_allowance(const RequestHandle& target,
                           const EvaluationContext& context,
                           const TargetObservation& observation);
    bool refresh_cohort_allowance(const RequestHandle& target,
                                  const EvaluationContext& context,
                                  const TargetObservation& observation);
    // Pure arithmetic over the cached pending total. Outputs are assigned only
    // after every validation and subtraction succeeds.
    static bool effective_host_units(const PendingAdjustments& pending,
                                     double resolved_units,
                                     double* deduction,
                                     double* after,
                                     bool* exhausted) noexcept;

    void reserve(std::size_t expected_events);
    std::vector<RequestHandle> group_recipients(const EventId& applied) const;
    std::vector<RequestHandle> waiting_children(const RequestHandle& parent) const;
    bool has_waiting_children(const RequestHandle& parent) const noexcept;
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
    bool working_is_buy(const LiveRequest& live,
                        std::optional<Side> cohort_side = std::nullopt) const noexcept;

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
    // Resolves tick-spelled trail offsets and trigger anchors in place
    // against context.price_tick. Runs after validate_request, on the staged
    // copy that becomes the stored definition.
    static std::optional<RequestRejectReason> resolve_tick_spellings(
            Request& request, const CommandContext& context);
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
    const RequestDefinition* definition_for(const RequestHandle& handle) const noexcept;
    std::optional<RequestHandle> canonical_cohort_origin(const RequestHandle& origin) const;
    std::size_t cohort_index(CohortHandle cohort) const noexcept;

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
        InlineCommandEvents events;
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
    std::uint64_t next_cohort_handle_ = 1;
    std::vector<CohortRoster> cohorts_;
    std::vector<CohortReceipt> cohort_receipts_;
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
static_assert(std::variant_size_v<OrderIntent> == 6);
static_assert(std::variant_size_v<ReductionSize> == 3);
static_assert(std::variant_size_v<SizeBasis> == 2);
static_assert(std::variant_size_v<Remaining> == 5);
static_assert(std::variant_size_v<RemainingProjection> == 5);
static_assert(std::variant_size_v<Allowance> == 4);
static_assert(std::variant_size_v<CommandEvent> == 18);
static_assert(std::variant_size_v<ExecutionPlan> == 4);
static_assert(std::variant_size_v<ExecutionScope> == 3);
static_assert(std::variant_size_v<TriggerState> == 9);
static_assert(std::variant_size_v<TriggerAnchor> == 2);

}  // inline namespace native_order_v6
}  // namespace pineforge::native_order

namespace std {
template <>
struct hash<pineforge::native_order::CohortHandle> {
    std::size_t operator()(pineforge::native_order::CohortHandle value) const noexcept {
        return static_cast<std::size_t>(value.value ^ (value.value >> 32));
    }
};
}  // namespace std
