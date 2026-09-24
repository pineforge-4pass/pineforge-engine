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
inline namespace native_order_v7 {

// Isolated working-request/value core: current LIVE requests and immutable
// command history. It does not own positions, cash, paid fees, matching,
// calendar, host phase, or a second physical book.
//
// Identity types remain native_order_v1. Request/core/event values are v7.
// Physical execution::Action is unchanged; native Reduce uses a typed size
// source instead of a dummy units field. ExecutionPlan is a transient widening
// used at the core/consumer boundary.

using Flatten = execution::Flatten;
/// A signed book transaction: finite nonzero units on the side their sign names.
/// It opens, and it closes against a live opposite book on the way, so a
/// transaction larger than an opposite position still opens the remainder. An
/// opening denial rejects the ENTIRE transaction, including that remainder.
using Transact = order_action::Transact;

/// A host-maintained, run-scoped roster identity.  Zero is invalid and is
/// never allocated by WorkingRequestCore.
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

/// Exact target exposure for an explicit reversal request. This is distinct
/// from execution::ReverseTo, which is the transient resolved execution plan.
struct ReverseTo {
    double signed_units = 0.0;
};

enum class HostSizedKind : std::uint8_t { Open = 0, Close = 1 };
enum class Side : std::uint8_t { Long = 0, Short = 1 };
/// An opening or a closing whose quantity the HOST resolves, in
/// NativeStrategyHost::resolve_execution_terms. `side` is required for an
/// opening, which must declare where it trades. A host-sized request that reaches
/// a candidate with no units answered is MatchRejectReason::TermsUnresolved. It is
/// the adapter's sizing seam; a bare host that wants the kernel to resolve a cash
/// or equity basis uses Sized instead, and the C surface refuses HostSized under
/// any owner but BindCohort.
struct HostSized {
    HostSizedKind kind = HostSizedKind::Open;
    std::optional<Side> side;
};

/// Resolved host sizing normally remains subject to the run's quantity grid.
/// A host may instead authenticate literal units for a pure reduction; the
/// consumer still proves that the positive quantity is representable within
/// the selected exposure before it can reach settlement.
enum class ExecutionGridPolicy : std::uint8_t {
    SnapToGrid = 0,
    ExplicitUnits = 1,
};

/// Native sizing bases (L3).  The kernel resolves them into units at the
/// sizing point.  A source adapter lowers its own declaration-level default
/// quantity onto them and keeps only its rounding and admission quirks
/// (R5 R2); a host that owns its whole quantity still emits HostSized.
struct CashValue {
    double cash = 0.0;                 // account currency
};
/// A fraction in (0, 1] of MARKED equity at the sizing point. The other SizeBasis
/// alternative, CashValue, is an absolute account-currency amount; both resolve as
/// units = cash / (price * point_value * fx).
struct EquityFraction {
    double fraction = 0.0;             // of marked equity at the sizing point (0.10 = 10 %)
};
/// The two bases a Sized intent may name. Extended only by appending, so an
/// existing aggregate initializer keeps its meaning.
using SizeBasis = std::variant<CashValue, EquityFraction>;

/// AtMatch resolves the basis at the matching candidate; AtAcceptance freezes
/// the resolved units when the request is accepted.
enum class SizeTime : std::uint8_t { AtMatch = 0, AtAcceptance = 1 };

/// WHICH price the basis is converted at. This is the generic "size against the
/// expected fill price" rule; it names no source language.
///
/// Resolved (today's behaviour) uses the price the kernel would otherwise
/// settle at: the candidate's default resolved price for AtMatch, the raw
/// acceptance-point price for AtAcceptance.
///
/// Signal is the decision-point price at placement -- the current bar's close,
/// or the last print, at submit -- carried to the expected market fill: it is
/// adjusted by the run's slippage for the request's own side
/// (price +/- slippage_ticks * price_tick) and then rounded onto the run's
/// price grid when NativeRunSpec::price_grid is on (HalfUp is the nearest
/// tick; NativePriceGrid::None leaves it unrounded). It is frozen when the
/// request is accepted, so it composes with SizeTime::AtAcceptance and, with
/// SizeTime::AtMatch, the basis still converts at that frozen signal price at
/// every later candidate. A Signal request accepted outside a decision point
/// has no price to freeze and stays unresolvable.
///
/// SignalOnTick is the same decision-point rule measured on the INSTRUMENT's
/// own tick ladder (NativeRunSpec::price_tick) instead of the run's fill grid:
/// the decision price is rounded onto the ladder (nearest tick, ties away from
/// zero), carried to the expected fill by the side's tick slippage, and rounded
/// onto the ladder again. Two rulings are folded into it, both generic:
///
///   * price_tick is the instrument's own resolution and the run already
///     declares it as the slippage multiplier, so a price the market PRINTS
///     lives on that ladder whether or not the run also quantizes the prices it
///     BOOKS. NativeRunSpec::price_grid governs fills; it does not govern what
///     a signal was worth. A run that books unquantized fills can therefore
///     still size against the printed price, which Signal cannot express.
///   * the ladder is applied BEFORE the slippage, not only after. Slippage is
///     a whole number of ticks, so it carries a ladder price to another ladder
///     price: pre-rounding makes `round(p) +/- n*tick` exact and leaves the
///     second rounding a pure binary64 re-normalization, where rounding only
///     after composes two different quantizations of the same print.
///
/// Resolved and Signal are unchanged by this alternative, so every established
/// Sized resolution keeps its value.
enum class SizePrice : std::uint8_t { Resolved = 0, Signal = 1, SignalOnTick = 2 };

/// A kernel-sized opening.  units = cash / (price * point_value * fx), where
/// cash is the basis value or fraction * marked equity, and price is the
/// sizing-point price SizePrice names.  A host override of
/// resolve_execution_terms still has the last word: it sees the kernel-resolved
/// units as the facts' RemainingUnits and may return its own.
///
/// SizeTime chooses WHEN the basis is resolved, SizePrice chooses WHICH price
/// it converts at, and the two compose.  Either way the request reaches the
/// candidate with a deferred quantity and exactly one terms pass: the kernel
/// resolves (or republishes the acceptance-frozen) units, publishes them as the
/// facts' RemainingUnits before resolve_execution_terms and
/// validate_execution_precommit run, and uses the host's units when the host
/// returns any.  An acceptance-time quantity is additionally an admission input
/// at placement, so an opening the run cannot admit is
/// RequestRejectReason::PlacementAdmission at submit rather than a rejected
/// candidate later.
struct Sized {
    Side side = Side::Long;
    SizeBasis basis{};
    SizeTime time = SizeTime::AtMatch;
    /// Which price the basis converts at; see SizePrice. Appended after `time`
    /// because Sized has no positional aggregate initializer outside tests that
    /// stop at the basis.
    SizePrice price = SizePrice::Resolved;
    /// SnapToGrid floors the resolved units onto the run's quantity grid.
    /// ExplicitUnits keeps the literal quotient, which the ordinary on-grid
    /// terms gate then refuses when a grid is configured and the quotient is
    /// off it; the two are identical on an ungridded run.
    ExecutionGridPolicy grid_policy = ExecutionGridPolicy::SnapToGrid;
    /// Divide the sizing cash by (1 + fee) when the run's fee kind is
    /// NativeFeeKind::Percent.  Every other fee kind is an exact no-op.
    bool reserve_percent_fee = false;
};

/// A reduction of exactly these units, finite and positive. Capped by the live
/// exposure of the scope the reduce is bound to, and floored onto the run's
/// quantity_grid like every other engine quantity.
struct ExplicitUnits {
    double units = 0.0;
};
/// A reduction of exactly what the owner's fill opened, bound at the arm and
/// reported by a QuantityBoundEvent. It is the arm-time size, which is why no
/// SizeTime::AtArm exists.
struct OwnerOpenedUnits {};
/// Gross claims the whole bound scope; NetOfSiblings first subtracts the units
/// already claimed by the live sibling reduces bound to that same scope.  The
/// keys are opaque request/owner handles, never source identifiers.
enum class ScopeClaim : std::uint8_t { Gross = 0, NetOfSiblings = 1 };
/// Which measurement of the bound scope the fraction is taken of. AtMatch (the
/// default) reads the scope as it stands at the matching candidate.
/// AtAcceptance freezes the scope SIZE when the request is accepted -- the
/// placement-time live basis -- so two 50 % siblings on one 10-unit lot both
/// claim 5 under Gross even after the first has already executed. NetOfSiblings
/// then subtracts the live sibling claims from that frozen basis.
enum class ScopeBasis : std::uint8_t { AtMatch = 0, AtAcceptance = 1 };
/// A fraction in (0, 1] of the bound scope, resolved at the matching candidate.
/// The resolution is units = scope * fraction, one binary64 multiplication: a
/// caller that spells its size as a percent converts percent -> fraction
/// itself, so no second rounding step is introduced here.
struct ScopeFraction {
    double fraction = 1.0;
    ScopeClaim claim = ScopeClaim::Gross;
    /// Appended last so every existing aggregate initializer keeps its meaning.
    ScopeBasis basis = ScopeBasis::AtMatch;
};
/// How much a Reduce takes: literal units, the owner's own opening, or a fraction
/// of the bound scope.
using ReductionSize = std::variant<ExplicitUnits, OwnerOpenedUnits, ScopeFraction>;
/// A closing-only intent. It never opens, so it stays legal while the run's
/// opening limits are already exceeded, and it uses CURRENT exposure rather than
/// the acceptance-cycle book. On a flat book it terminalizes a NoEffectEvent: no
/// execution identity, no fill, no fee, no physical action.
struct Reduce {
    ReductionSize size;
};
/// What a request does. Six alternatives: Flatten (quantity-free whole-book
/// close), Reduce (closing only), Transact (a signed book transaction), ReverseTo
/// (an exact signed target exposure), HostSized (the host resolves the quantity)
/// and Sized (the kernel resolves it from a cash or equity basis). The variant's
/// size is pinned by the frozen C++ ABI fixtures, so appending an alternative is
/// an epoch decision.
using OrderIntent = std::variant<Flatten, Reduce, Transact, ReverseTo, HostSized, Sized>;

/// Match at the next eligible matching point, with no level to reach. A market
/// request accepted on bar N cannot fill on that bar's already delivered opening.
struct Market {};
/// `fill_through` makes the limit a touch trigger (market-if-touched): the
/// level still gates when the request becomes executable, but its fill is not
/// bounded by the level, so slippage may carry it past the level.
struct Limit {
    double price = 0.0;
    bool fill_through = false;
};
/// A stop trigger: the modeled path has to reach `price` from the adverse side. A
/// crossing books the level itself; a point already past it (a gapped open) books
/// that print. Under a quantizing trigger grid the core re-validates the
/// activation on the same ladder the matcher used, so a hit the matcher reports is
/// never refused.
struct Stop {
    double price = 0.0;
};
/// A stop that, once reached, becomes a limit at `limit`. The stop gates and the
/// limit bounds; an anchored FromOwnerFill level is refused for this trigger,
/// because the anchor materializes one level and this trigger has two.
struct StopLimit {
    double stop = 0.0;
    double limit = 0.0;
};
/// Trail offset spelled in price ticks instead of a price distance. The
/// acceptance path resolves it against the run's price tick, writes the
/// product into Trail::offset and clears the spelling, so a stored definition
/// always carries a plain price distance and can never resolve twice.
struct TrailTicks {
    double ticks = 0.0;
};
/// offset is a price distance: the stop rides `offset` behind the running
/// best. Zero is legal and means "ride the best": the exit is the first
/// adverse move past it. Negative and nonfinite offsets are rejected.
/// arm_price absent means the trail is already armed and starts riding at the
/// first print. Under a FromOwnerFill anchor it means the owner's fill
/// supplies the threshold, which is that anchor's whole point: absent is the
/// anchored spelling, 0.0 is the equivalent placeholder kept legal for hosts
/// that already write it, and any other written level is refused because the
/// arm would overwrite it.
///
/// best_seed is where the running best STARTS, appended last so every existing
/// {offset}, {offset, arm_price} and {offset, arm_price, ticks} initializer
/// keeps its meaning. Absent (the default) is the established behaviour: the
/// best is the arm's own print -- the arm threshold's own ladder point when a
/// crossing armed it, the first print the trail sees when it was submitted
/// already armed. A present seed is a floor on that start: at the arm the best
/// becomes the favourable one of the seed and the arm print (the higher for a
/// sell trail, the lower for a buy trail), so a host whose position already
/// reached a level before this request existed does not restart the ride at a
/// worse print. It is a generic broker shape -- a trail that rides from its
/// activation rather than from the next print -- and the level is the host's
/// own number: the kernel neither derives it nor knows what named it. It is
/// absolute (an anchor moves the arm threshold, never the seed), it must be
/// finite and positive, and under a price grid it is put on the ladder exactly
/// like an observed print. It is folded into the request digest only when
/// present, so every trail without one keeps the digest it had before the
/// field existed.
struct Trail {
    double offset = 0.0;
    std::optional<double> arm_price;
    std::optional<TrailTicks> ticks = std::nullopt;
    std::optional<double> best_seed = std::nullopt;
};
using Trigger = std::variant<Market, Limit, Stop, StopLimit, Trail>;

/// How a materialized anchored level is snapped onto the run's price tick
/// ladder (NativeRunSpec::price_tick). Raw keeps `fill + offset` exactly,
/// which is the established behaviour. HalfUp is the nearest tick with ties
/// away from zero. Directional rounds toward the price region the resting leg
/// needs, relative to the leg's trigger kind and side exactly as
/// NativeGridRounding documents it: a buy limit down and a sell limit up, a
/// stop the other way; a trail arm threshold is reached from the favourable
/// side like a limit and rounds like one. Either rounding needs a positive
/// price tick, checked at acceptance exactly like a tick-spelled offset. This
/// is a generic instrument grid: a source language's own trigger projection
/// is not spelled here (a host restates the level through its arm hook).
enum class NativeAnchorRounding : std::uint8_t {
    Raw = 0,
    HalfUp = 1,
    Directional = 2,
};

/// Where a request's trigger level comes from. Absolute is the level written
/// in the trigger itself. FromOwnerFill defers it to the owner's fill: the
/// level becomes fill + offset when the owner arms the request, so a bracket
/// leg can be placed before its parent has a price. offset is signed (adverse
/// is negative) and is spelled in price ticks when `ticks` is set; acceptance
/// resolves a tick spelling exactly like TrailTicks, in place, once. The
/// materialized level is then snapped per `rounding`, which is appended last
/// so every existing aggregate initializer keeps its meaning.
struct Absolute {};
/// Defer a level to the owner's fill: at the arm the level becomes fill + offset,
/// with `offset` signed (adverse is negative) and spelled in price ticks when
/// `ticks` is set, then rounded by `rounding`. Legal on Limit and Stop prices and
/// on a Trail arm threshold, and only under a WaitForApplied owner, which is the
/// one relation that arms. Until then the anchored field is left unwritten (0.0
/// for a Limit or Stop price, an absent or 0.0 Trail::arm_price), and the ArmedEvent
/// carries the materialized definition: from then on the request reads as that level.
struct FromOwnerFill {
    double offset = 0.0;
    bool ticks = false;
    NativeAnchorRounding rounding = NativeAnchorRounding::Raw;
};
using TriggerAnchor = std::variant<Absolute, FromOwnerFill>;

/// Replacement behaviour that is not expressible in the successor request.
/// retain_trigger_state carries the predecessor's live trigger state (a
/// tracking trail's best, an already active stop) into the successor instead
/// of restarting it; predecessor and successor must hold the same trigger
/// alternative.
///
/// keep_handle re-prices the request in place: the new definition keeps the
/// target's handle, and with it the request's lineage (its predecessor link
/// and chain root) and its dependents -- a child waiting on it keeps waiting,
/// a roster that names it keeps it -- where a plain replace retires the handle
/// and issues a successor. Everything else is the replace: the same checks and
/// refusals, a new immutable definition and birth, a fresh live row (remaining,
/// allowance and trigger state restart unless retain_trigger_state keeps the
/// last), and one ReplacedEvent, whose two definitions share the handle. The
/// re-price takes the number of the run's request sequence a successor's
/// incarnation would have taken, as the definition's priority
/// (RequestDefinition::priority): matching ranks it the newest request, where a
/// successor stands, and the requests after it are numbered as if the replace
/// had issued a handle. No handle is issued under that number.
///
/// keep_binding carries a close's book binding across the replace. A successor
/// that starts unbound (an Independent close) and whose predecessor was bound
/// to the book, or carried such a binding itself, records the binding's cycle
/// and side (RequestDefinition::kept_binding). Where the successor would bind,
/// at its first evaluation, a book that still has that cycle and side binds it
/// with no CloseBoundEvent; any other book binds it, or ends it, exactly as it
/// does a successor that carried nothing. So the option changes the events a
/// run records, never what it matches. Anywhere else it carries nothing.
/// Either option may be given alone.
struct ReplaceOptions {
    bool retain_trigger_state = false;
    bool keep_handle = false;
    bool keep_binding = false;
};

/// Take whatever the point allows, with no per-point cap. The default capacity,
/// and the only one the Pine adapter emits.
struct ImmediateRemaining {};
/// Cap what this request may execute at ONE matching point at `units`. Fixed at
/// submit and metered per point — including after an arm — so a request cannot be
/// drained faster than its owner's schedule allows.
struct PointBudget {
    double units = 0.0;
};
using Capacity = std::variant<ImmediateRemaining, PointBudget>;

/// Whether an owner-related request that arms is a working order before its
/// arm. Working (the default) is the established book: the request is listed
/// by native_working_requests() from acceptance on. PendingUntilArmed keeps
/// it out of that enumeration until its ArmedEvent; it is still a live
/// request the whole time -- accepted, addressable by handle (replace,
/// cancel, trail_state), counted by cancel_all / cancel_where, and folded
/// into the continuation identity -- and it never matches before the arm
/// under either value. Visibility governs enumeration, not addressing.
enum class NativeArmVisibility : std::uint8_t {
    Working = 0,
    PendingUntilArmed = 1,
};

/// When an armed request may first match. The arm happens inside its owner's
/// fill settlement, at the owner's fill print. AtArmPrint (the default) is the
/// established book: the armed request is a candidate at that very cursor, so
/// a level the fill print already satisfies matches at the print. AfterArmPrint
/// gives the armed request the birth rule of a request submitted from the
/// owner's fill callback: on the driver point that armed it, it sees only the
/// path after the print -- a level the print already satisfies does not match
/// there, a later crossing on that point still does -- and from the next
/// driver point on it is an ordinary working request. It governs the level
/// test of a priced trigger (limit, stop, stop-limit, trail arm); a market
/// trigger has no level and is unaffected. Both are broker models: a
/// contingent child that enters the book after its parent's trade cannot
/// trade on that print, a simulated bracket commonly may.
enum class NativeArmFirstMatch : std::uint8_t {
    AtArmPrint = 0,
    AfterArmPrint = 1,
};

/// What an armed CLOSING request (Reduce, Flatten, a HostSized close) closes.
/// OwnerLot (the default) is the established relation: the lot its owner's
/// fill opened, and nothing a later add brings. Book binds it, at the arm, to
/// the whole position that fill left -- the very book authority an
/// Independent close submitted from the owner's fill callback acquires -- so
/// a protective leg placed with its entry covers later adds and settles
/// through the book like any other close. A HostSized close may wait for its
/// owner only under Book: the owner-lot relation sizes from the units the
/// owner opened, whereas a book close is sized by the host at the match.
/// A waiting transaction closes nothing, so it must keep OwnerLot.
enum class NativeArmScope : std::uint8_t {
    OwnerLot = 0,
    Book = 1,
};

/// No owner: the request stands on its own book authority. Required for Sized, and
/// the default.
struct Independent {};
/// The one owner relation that arms (the ArmedEvent). `visibility`, then
/// `first_match`, then `scope` are appended last so every existing {parent},
/// {parent, visibility} initializer keeps its meaning.
struct WaitForApplied {
    RequestHandle parent;
    NativeArmVisibility visibility = NativeArmVisibility::Working;
    NativeArmFirstMatch first_match = NativeArmFirstMatch::AtArmPrint;
    NativeArmScope scope = NativeArmScope::OwnerLot;
};
/// Bind to ONE already-live opening, by its handle and the positive cycle it is
/// live in. A reduce bound this way closes that lot and nothing a later add
/// brings.
struct BindOpening {
    RequestHandle opening;
    int64_t cycle = 0;
};
/// Bind to a fixed cohort of already-live openings. Every handle must be unique,
/// nonzero, of this run and physically live in `cycle`; enrollment rejects the
/// ENTIRE request if any member is invalid. The accepted definition keeps the
/// original cohort in canonical handle order even as later retirement narrows the
/// live subset, and execution still closes lots in physical FIFO order.
struct BindOpenings {
    std::vector<RequestHandle> openings;
    int64_t cycle = 0;
};
/// Bind to a host-built roster (cohort_open / cohort_add / cohort_remove), read at
/// the match rather than frozen at submit. The C surface pairs it with exactly one
/// intent, a host-sized close, whose units the on_close_units hook answers.
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

/// Aggregate field order keeps market construction:
///   Request{Transact{1.0}, "buy", "comment"}
///   Request{Flatten{}, "flat", ""}
///   Request{Reduce{ExplicitUnits{3}}, "close", ""}
/// New fields default to Market / ImmediateRemaining / Independent / NoGroup.
/// market_request() converts a physical execution::Action (explicit Reduce
/// units only) into that same market Request. Host market-only methods must
/// reject nondefault extras; they must not drop them.
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
/// A resolved remaining quantity, in units. The kernel publishes a Sized intent's
/// own resolution here, BEFORE the host's terms hook runs, so an override sees the
/// kernel's number and may answer with a different one.
struct RemainingUnits {
    double q = 0.0;
};
struct RemainingDeferred {};
/// A dynamic cohort has no currently live member.  This is a live deferral,
/// not a terminal receipt: the consumer retries it at the next candidate.
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
/// The authority an unarmed WaitForApplied child holds: accepted and live, but
/// never a matching candidate until its owner's fill arms it — under either
/// visibility.
struct Wait {
    RequestHandle parent;
};
struct ArmedTransaction {
    RequestHandle parent;
    EventId cause;
    MatchCursor cause_cursor{};
};
struct UnboundBookClose {};
/// The authority an armed leg acquires under NativeArmScope::Book: the whole
/// position its owner's fill left, on this cycle and side, bound at the fill's
/// cursor. It is the same authority an Independent close has, which is what lets a
/// protective leg placed with its entry also cover later adds.
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
/// Immutable fixed cohort. Current physical liveness is observed, never stored
/// here; later fragments of an enrolled provenance remain authorized.
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

/// Native authorization receipt, converted to a call-local financial
/// SelectedOpeningSet only at the consumer's settlement boundary.
struct SelectedExposure {
    int64_t cycle = 0;
    std::vector<uint64_t> incarnations;
};
/// What one execution acted on: the whole book, one opening's exposure, or a bound
/// selection. Reported on ExecutionAppliedEvent::scope. The financial
/// execution::CloseScope remains the original two-alternative type; this is the
/// native view, and a selected close's committed row range identifies the actual
/// contributors.
using ExecutionScope = std::variant<execution::Book, execution::OpeningExposure, SelectedExposure>;

struct MarketReady {};
struct LimitReady {};
struct StopIdle {};
struct StopActive {};
struct StopLimitPending {};
struct StopLimitLive {};
struct TrailWaitArm {};
/// A trail riding its running best. `activation_ordinal` is the ordinal of
/// this request's own TrailArm ActivatedEvent -- NativeTrailState's
/// activation_ordinal, kept in state so it outlives the journal window -- and
/// 0 for a successor that retained its predecessor's tracking
/// (ReplaceOptions::retain_trigger_state), which never armed itself. A
/// re-price that kept the handle (ReplaceOptions::keep_handle) keeps it: that
/// same request armed.
struct TrailTrack {
    double best = 0.0;
    uint64_t activation_ordinal = 0;
};
/// A triggered trail; `activation_ordinal` carries TrailTrack's.
struct TrailActive {
    double best_at_trigger = 0.0;
    uint64_t activation_ordinal = 0;
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

/// Who authored a request. Host is every request a host submits, replaces or
/// cancels, and it is the whole existing population: it folds nothing into the
/// continuation digest, so no established hash moves. KernelLiquidation marks
/// the margin model's own Reduce or Flatten; KernelRisk marks the Flatten a
/// NativeRiskAction::FlattenAndBlock breach issues at its breach point
/// (native_execution_consumer.cpp, pinned by tests/test_native_risk_limits.cpp).
enum class RequestOrigin : std::uint8_t {
    Host = 0,
    KernelLiquidation = 1,
    KernelRisk = 2,
};

/// The immutable accepted form of one request: its handle, the request value as
/// accepted, its birth (acceptance ordinal and decision floor), its origin
/// (RequestOrigin::Host, or the kernel's own for a liquidation or a risk flatten)
/// and its authority. Every event carries a shared pointer to it, so a reader
/// holds the definition the kernel matched against and not a later re-read.
struct RequestDefinition {
    RequestHandle handle;
    Request request;
    Birth birth;
    std::optional<RequestHandle> predecessor;
    /// Appended last so every existing aggregate initializer keeps its meaning.
    RequestOrigin origin = RequestOrigin::Host;
    /// The first handle of this request's replace chain -- the request the
    /// chain began with, which is how a cohort roster names every request of
    /// the chain (cohort_add, cohort_contains). Empty for a request that
    /// replaced nothing: its chain begins with itself. Set by the core at the
    /// replace that creates the definition, from the predecessor's own root,
    /// so it never needs the predecessor's journal events. Appended after
    /// `origin` for the same reason `origin` was.
    std::optional<RequestHandle> root;
    /// Where the definition stands in the run's queue: the number of the
    /// run's request sequence the command that placed it took. Matching
    /// breaks a tie at one cursor oldest number first, and the working book
    /// is held in this order (WorkingRequestCore::live). 0, as every accepted
    /// request and replace successor has it, means the handle's incarnation,
    /// the number its command took; a re-price that kept the handle
    /// (ReplaceOptions::keep_handle) sets the number it took instead. Read it
    /// through LiveRequest::priority. Appended after `root`, like it.
    uint64_t priority = 0;
    /// The book binding a replace carried forward (ReplaceOptions::
    /// keep_binding): the cycle and side of the book its predecessor was bound
    /// to. The request starts unbound, and at its first evaluation a live book
    /// with this cycle and side binds it with no CloseBoundEvent. Empty for
    /// every other definition.
    std::optional<PositionNonflat> kept_binding;
};
using DefinitionRef = std::shared_ptr<const RequestDefinition>;

struct LiveRequest {
    DefinitionRef definition;
    Remaining remaining = RemainingUnbound{};
    Authority authority = UnboundBookClose{};
    TriggerState trigger_state = MarketReady{};
    Allowance allowance = AllowanceUnset{};
    PendingAdjustments pending = PendingNone{};
    /// Placement-time sizing measurements frozen from the accepting command
    /// context. Both stay empty for every request that did not ask to freeze
    /// one, and an empty optional folds nothing into the continuation digest.
    std::optional<double> sizing_units;   // SizeTime::AtAcceptance
    std::optional<double> sizing_scope;   // ScopeBasis::AtAcceptance
    std::optional<double> sizing_price;   // SizePrice::Signal / SignalOnTick

    const RequestHandle& handle() const noexcept { return definition->handle; }
    const Request& request() const noexcept { return definition->request; }
    const Birth& birth() const noexcept { return definition->birth; }
    const std::optional<RequestHandle>& predecessor() const noexcept {
        return definition->predecessor;
    }
    /// Its place in the queue (RequestDefinition::priority): the handle's
    /// incarnation, unless a re-price kept the handle and took a later number.
    uint64_t priority() const noexcept {
        return definition->priority != 0 ? definition->priority : definition->handle.incarnation;
    }
};

/// Whether a request may be considered at this driver point: the point's ordinal
/// must be strictly past the birth acceptance ordinal AND its effective time at or
/// past the birth's decision-time lower bound. This is the birth gate — it is why
/// a request accepted on bar N cannot fill on that bar's already delivered
/// opening — and a request born mid-path in on_native_applied is admitted at the
/// current cursor instead, on the unconsumed suffix of that segment.
inline bool point_eligible(const LiveRequest& live,
                           uint64_t point_ordinal,
                           int64_t effective_time_ms) noexcept {
    return point_eligible(live.birth(), point_ordinal, effective_time_ms);
}

/// v2 exact binary64 grid: r=abs(q)/s, n=round(r) half away from zero, g=n*s.
/// Intermediates finite, 1<=n<=2^53, abs(abs(q)-g) <= 4*ulp(max(abs(q),abs(g)))
/// and < s/2. ulp is nextafter toward +inf. Does not rewrite q.
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

/// Why an acceptance was refused, on a Rejected SubmitResult. These are decided at
/// submit, before any request record exists: an invalid quantity or basis, an
/// off-grid quantity, an owner the intent may not take, a trigger spelling the run
/// cannot resolve, or a frozen quantity the run's own opening admission refuses
/// (PlacementAdmission).
enum class RequestRejectReason : std::uint8_t {
    InvalidQuantity = 0,
    OffGrid = 1,
    InvalidTrigger = 2,
    InvalidCapacity = 3,
    InvalidOwner = 4,
    InvalidQuantityBasis = 5,
    InvalidGroup = 6,
    /// A Sized{SizeTime::AtAcceptance} whose acceptance-resolved quantity does
    /// not pass the run's opening admission (allowed directions, max_abs_units,
    /// max_open_lots, initial margin) at the sizing price.
    PlacementAdmission = 7,
};

enum class SubmitStatus { Accepted, Rejected };
enum class ReplaceStatus { Replaced, ReplaceRejected, NotWorking, InvalidHandle };
enum class CancelStatus { Cancelled, NotWorking, InvalidHandle };

/// What submit answers. Accepted carries a timeline ordinal and a RequestHandle
/// (session, run, incarnation); Rejected carries a rejection ordinal, no handle
/// and a reason. Acceptance is not a fill: no lot, no cash and no fee moves here,
/// and the ordinary fill appears later as an ExecutionAppliedEvent on the same
/// command history.
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

/// Why a CANDIDATE was refused at a matching point. Every refusal is terminal:
/// the MatchRejectedEvent ends the request, and a host that still wants it
/// submits again. OpeningDirection, MaxAbsUnits, MaxOpenLots, InitialMargin and
/// RiskLimit are the run's admission gates; TermsUnresolved is a host-sized or
/// basis-sized quantity that could not be resolved; NoOppositeExposure and
/// InvalidTerms are shapes the answered terms cannot take.
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
    /// An opening refused while the run spec's generic risk limits are
    /// blocking (L9). Reduces never reach this gate.
    RiskLimit = 9,
};

/// Which price a candidate is being offered at: the point's own price, or the
/// request's trigger level (a crossing that books the level itself). A host reads
/// it off the terms facts, and the trail suites read it back off the applied event
/// to tell a level fill from a path fill.
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

/// The settlement shape one execution takes. Derived by the kernel from the intent
/// and the answered terms; a host never constructs it.
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

/// Why a request left the book. Host is an explicit cancel; Group an OCA sibling's
/// effect; OwnerGone a parent that was rejected, replaced or cancelled, which ends
/// its waiting children; Superseded the kernel withdrawing its own liquidation
/// before re-pricing it. Every reason is carried on a CancelledEvent.
enum class CancelReason : std::uint8_t {
    User = 0,
    Group = 1,
    OwnerGone = 2,
    UnsupportedRelation = 3,
    /// A kernel-originated request the kernel itself withdrew: the liquidation
    /// level or its units moved, or the requirement is no longer breached.
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

/// One request was accepted: the definition, and the timeline ordinal it was
/// accepted at. The first event of every request's history.
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
    /// Whether the replace kept the handle (ReplaceOptions::keep_handle): the
    /// successor definition is the same request re-priced, and only such a
    /// definition carries a priority of its own.
    bool kept_handle() const noexcept { return successor_definition->priority != 0; }
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

/// A reduction or a flatten that had nothing to close: terminal, with no execution
/// identity, no fill, no fee and no physical action. It is an outcome, not a
/// failure.
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

/// A candidate the run refused, with its MatchRejectReason and the cursor it was
/// refused at. It carries an event ordinal but no execution identity, and it is
/// the request's last event: every match refusal erases the live request.
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

/// One committed execution: the definition, the cursor, the resolved price and
/// units, the scope it acted on, the committed closed-row range, the cycle on
/// either side, and the ticket it booked under. Delivered to on_native_applied and
/// recorded in native_events(); the account observation that follows it shares its
/// ordinal.
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

/// A Reduce{OwnerOpenedUnits} was bound, at its owner's fill, to what that fill
/// opened. It is how a bracket child's size becomes a number, and it precedes the
/// ArmedEvent of the same drain.
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

/// An anchored leg was materialized, exactly once: fill + offset, then the
/// rounding, then resolve_anchored_level, then the representability check, and the
/// resulting level written into the leg's trigger with its anchor becoming
/// Absolute. Every later reader — the live book, the C working rows, matching —
/// sees that level. Under NativeArmScope::Book it also carries the BookClose the
/// leg acquired.
struct ArmedEvent {
    uint64_t ordinal = 0;
    DefinitionRef definition;
    Authority before = Wait{};
    Authority after = ArmedTransaction{};
    Enrollment enrollment = EnrollmentFromApplied{};
    std::optional<EventId> quantity_resolution;
};

/// A kernel-issued liquidation that actually filled. It carries the margin
/// facts of that fill, so a host reconstructs the outcome without recomputing
/// the account: `mark` is the booked resolved price, `equity` and `required`
/// are the marked equity and the maintenance requirement of the SURVIVING book
/// at that price, `liquidation_price` is the level re-solved for what is left,
/// and `position_before` / `position_after` are the signed book on either side
/// of the reduction. `applied` names the ExecutionAppliedEvent that booked it.
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

/// Which generic risk limit of NativeRunSpec::risk a NativeRiskEvent reports.
enum class RiskLimitKind : std::uint8_t {
    MaxDrawdown = 0,
    MaxIntradayLoss = 1,
    MaxConsecutiveLossDays = 2,
    MaxFillsPerDay = 3,
};

/// One generic risk limit breaching (L9). It is not bound to a request: the
/// block it opens is an account fact, so this event carries no definition.
/// `limit` is the threshold in the unit the breach was measured in — account
/// currency for the two loss limits (a percent limit is already resolved
/// against its basis equity here), days or fills for the two counts — and
/// `observed` is the measured value that reached it. `day_ordinal` is the
/// risk day the breach happened on, on the spec's own day basis, and `cursor`
/// is the point it was measured at.
struct NativeRiskEvent {
    uint64_t ordinal = 0;
    RiskLimitKind kind = RiskLimitKind::MaxDrawdown;
    double limit = 0.0;
    double observed = 0.0;
    std::int64_t day_ordinal = 0;
    MatchCursor cursor{};
};

/// Every alternative of one request's history, as one value. native_events()
/// returns them as the NativeEventKind::Command rows, and
/// strategy_native_events_v1 flattens the same set into one tagged POD. The
/// variant's size is pinned by the frozen C++ ABI fixtures.
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
                                  MarginCallEvent,
                                  NativeRiskEvent>;

/// Almost every prepared command yields one history event. Keep that ordinary
/// transactional payload inline; the overflow vector preserves the existing
/// arbitrary-length behavior for group/lifecycle plans that emit more events.
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

/// What acceptance needs from the run to judge a command: the point's ordinal and
/// coordinate, the price tick the tick spellings resolve against, the quantity
/// grid, and the live book. Built by the consumer; a host never constructs one.
struct CommandContext {
    int64_t decision_time_ms = 0;
    std::optional<double> quantity_grid;
    std::optional<OpeningObservation> opening;
    CommandSurface surface = CommandSurface::General;
    std::vector<OpeningObservation> openings;
    /// Kernel-resolved acceptance-time units for a Sized{AtAcceptance} request.
    /// The execution consumer owns the account facts, so it supplies them here;
    /// a producer that leaves it unset accepts the request with a size the
    /// matching path then reports as TermsUnresolved.  The accepted request
    /// carries the value in LiveRequest::sizing_units and still reaches the
    /// candidate with a deferred remaining, so the host keeps its one override
    /// pass.  Appended last so the existing positional aggregate initializers
    /// keep their meaning.
    std::optional<double> sizing_units;
    /// The run's price tick, needed only to resolve a tick-spelled trail
    /// offset or trigger anchor. A tick spelling without a usable tick here is
    /// rejected rather than silently read as a price distance.
    std::optional<double> price_tick;
    /// Placement-time sizing measurements the execution consumer owns, supplied
    /// only for the intent that asks for them. Appended last, exactly like
    /// sizing_units, so the existing positional aggregate initializers keep
    /// their meaning.
    ///
    /// sizing_scope: the bound scope's exposure at acceptance, for a
    ///   Reduce{ScopeFraction{ScopeBasis::AtAcceptance}}.
    /// sizing_price: the frozen signal price, for a Sized whose SizePrice is
    ///   Signal or SignalOnTick.
    /// sizing_admissible: false when the acceptance-resolved quantity of a
    ///   Sized{SizeTime::AtAcceptance} fails the run's placement admission.
    std::optional<double> sizing_scope;
    std::optional<double> sizing_price;
    bool sizing_admissible = true;
};

struct EvaluationContext {
    MatchCursor cursor{};
    DriverEligibilityClass driver_class = DriverEligibilityClass::ObservedPrint;
    bool existing_matching_bit = false;
    /// Generic current-point delivery. At Open, the consumer sets this only for
    /// a market/immediate request born by the pre-open provider. On a continuous
    /// OHLC segment, it also admits a request born by an applied callback onto
    /// the unconsumed suffix. It is transient and never retained in a request.
    bool pre_open_birth_eligible = false;
    /// Set only while resolving a CohortClose candidate.  It carries the
    /// physical side of the currently live selected roster and is not retained
    /// in a request definition.
    std::optional<Side> cohort_side;
};

/// The one policy point of an anchored materialization. The core knows it
/// only as a callable over its own values: the leg's live row, the owner's
/// fill, the leg's side, the resolved offset (price units) and the kernel
/// level after the anchor rounding. It is consulted exactly once per
/// materialization, before the ArmedEvent is built, so the ArmedEvent and
/// every later reader see the installed level. A returned value is the level
/// to install (the kernel's representability check still applies and a
/// failure is the existing PreparationError path); nullopt keeps the kernel
/// level. An empty callable is exactly the pre-hook behaviour.
using AnchoredLevelResolver = std::function<std::optional<double>(
        const LiveRequest& leg, const ExecutionAppliedEvent& owner_fill, Side leg_side,
        double offset, double kernel_level)>;

/// What the consumer hands to prepare_owner_applied for an anchored leg's
/// materialization, the way acceptance receives CommandContext::price_tick.
/// price_tick is the ladder a rounded anchor snaps to; an anchor whose
/// rounding is Raw never reads it. resolve_level is the host's restatement,
/// carried as a value so the core stays host-free.
struct ArmContext {
    std::optional<double> price_tick;
    AnchoredLevelResolver resolve_level;
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

/// What the consumer hands to prepare_trigger under an opted-in price grid
/// (NativeRunSpec::price_grid == QuantizeFillsAndTriggers), the way acceptance
/// receives CommandContext::price_tick: the tick ladder and the rounding the
/// matcher tested the activation on. The core then re-validates the reached
/// print with the same grid arithmetic (L8b ruling: under that grid the
/// tick-quantized print IS the reached price, so a hit the matcher reports is
/// never refused), records the quantized print as the activation's reached
/// price, and keeps a trail's running best on the ladder. A default-constructed
/// value (no ladder) is exactly the raw compare and the raw print every
/// activation used before. Host-free: values only.
///
/// ladder_tick is a different question, appended last so every existing
/// aggregate initializer keeps its meaning: the run's DECLARED price tick
/// (NativeRunSpec::price_tick), whatever the quantization mode. Nothing is
/// tested or rounded on it; it is the ladder a level spelled in ticks names,
/// so a trailing stop a whole number of ticks from a best on that ladder is
/// the ladder point it names rather than the subtraction's ULP-off neighbour
/// (native_matching::ladder_trail_stop, R5 lane E16). Zero leaves the raw
/// arithmetic, which is what a run with no declared tick has.
struct ActivationGrid {
    double price_tick = 0.0;  // finite and positive when a ladder is in force
    bool half_up = true;      // nearest tick, ties away from zero; else directional
    double ladder_tick = 0.0; // the run's own price tick, quantized or not
};

struct ExecutionProposal {
    MatchCursor cursor{};
    /// Carries the generic pre-open delivery authorization from the matching
    /// evaluation through the synchronous execution preparation.
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

/// Where an install put its events: `first_index` is an ABSOLUTE journal
/// position -- the count of events the run committed before them -- so it
/// stays valid when the front of the journal is retired
/// (WorkingRequestCore::history_at reads it back).
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

/// A working-request preparation that could not produce a representable
/// instruction — a nonrepresentable quantity, most often. Reported as a settlement
/// failure, which fails the run: it is a contract breach, not a refused candidate.
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
    /// Canonical origin handles, ordered by origin incarnation rather than by
    /// host insertion order.  A successor is normalized to its predecessor
    /// root before entering this table.
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

/// One group-effect receipt: the applied event that caused it, the request it
/// acted on, the effect, and the ordinal of the event that recorded the
/// outcome. A drain that meets the same cause, recipient and effect again finds
/// it and applies nothing (WorkingRequestCore::group_effect_receipt).
struct GroupEffectReceipt {
    EventId cause{};
    RequestHandle recipient{};
    GroupEffect effect = GroupEffect::Reduce;
    uint64_t outcome_ordinal = 0;
};

class WorkingRequestCore {
public:
    explicit WorkingRequestCore(RunIdentity identity);
    WorkingRequestCore(const WorkingRequestCore&) = delete;
    WorkingRequestCore& operator=(const WorkingRequestCore&) = delete;
    /// Move transfers the complete run. The source becomes empty/unbound;
    /// commands throw invalid_argument without mutation until reset rebinds it.
    /// Self move-assignment preserves the current run.
    WorkingRequestCore(WorkingRequestCore&& other) noexcept;
    WorkingRequestCore& operator=(WorkingRequestCore&& other) noexcept;

    void reset(RunIdentity identity);

    const RunIdentity& identity() const noexcept { return identity_; }
    /// The working book, in queue order: ascending LiveRequest::priority,
    /// which is incarnation order until a re-price keeps a handle
    /// (ReplaceOptions::keep_handle) and takes its request to the back.
    const std::vector<LiveRequest>& live() const noexcept { return live_; }
    /// The command journal's retained window, oldest first. Commits append at
    /// the back and retire_history drops a prefix, so the window holds the
    /// absolute journal positions [history_base(), history_end()). A core
    /// nothing retires -- every core its consumer runs under a retaining
    /// policy -- keeps the whole journal and history_base() stays 0.
    const std::vector<CommandEvent>& history() const noexcept { return history_; }
    /// How many events have been retired from the front of the journal.
    std::size_t history_base() const noexcept { return history_base_; }
    /// The absolute journal length: every event this run has committed,
    /// retained or not. EventRange::first_index counts in this space.
    std::size_t history_end() const noexcept { return history_base_ + history_.size(); }
    /// The event at an absolute journal position; std::out_of_range when it
    /// is not retained.
    const CommandEvent& history_at(std::size_t absolute) const;
    /// Retire the journal prefix whose ordinals are <= through_ordinal. It
    /// stops ahead of the first event a live request still reads -- the head
    /// of a deferred group-adjustment chain (collect_pending_chain) -- and
    /// moves no live row, counter, receipt or roster: a group-effect receipt
    /// whose outcome event is retired stands on its own record
    /// (receipt_lookup), and the cohort lookups read the chain index, never
    /// the journal. Answers the number of events retired.
    std::size_t retire_history(uint64_t through_ordinal);
    /// The ordinal of the newest retired event, 0 while none is.
    uint64_t retired_through() const noexcept { return retired_through_; }
    const std::vector<CohortRoster>& cohorts() const noexcept { return cohorts_; }
    const std::vector<CohortReceipt>& cohort_receipts() const noexcept {
        return cohort_receipts_;
    }
    /// The core's two durable counters: the ordinal of the last committed
    /// event and the last incarnation a command consumed. A later command must
    /// exceed both.
    uint64_t last_ordinal() const noexcept { return last_ordinal_; }
    uint64_t last_incarnation() const noexcept { return last_incarnation_; }
    /// The group-effect receipts, in commit order: the idempotence keys a later
    /// drain consults. Read-only; the consumer's state continuation folds each
    /// once, when it commits.
    std::size_t group_effect_receipt_count() const noexcept { return receipts_.size(); }
    GroupEffectReceipt group_effect_receipt(std::size_t index) const;
    /// The incarnations this run issued -- every accepted request and every
    /// replace successor -- as disjoint ascending [first, last] ranges: the
    /// half of the chain index that tells a request that ended from one that
    /// never existed. Read-only; a consumer issues one dense range.
    const std::vector<std::pair<uint64_t, uint64_t>>& issued_incarnations() const noexcept {
        return issued_;
    }
    const LiveRequest* find_live(const RequestHandle& handle) const;
    const CommandEvent* event_at(const EventId& id) const;

    /// Command-boundary roster maintenance.  A rejected add/remove records a
    /// durable generic receipt but never emits a market event.
    CohortHandle cohort_open();
    void cohort_add(CohortHandle cohort, RequestHandle origin);
    void cohort_remove(CohortHandle cohort, RequestHandle origin);
    bool cohort_contains(CohortHandle cohort, const RequestHandle& opening) const;

    /// R1 producer convenience: prepare then install one command. Bound opening
    /// enrollment requires CommandContext observations via prepare_submit.
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

    /// Consumer-only typed prepare/install. Prepare may reserve storage and
    /// invalidate live/history references; it does not change logical state.
    /// Tokens are move-only, bound to this instance/run/epoch, and do not
    /// survive another prepare/install, reset, or move.
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
    /// `reason` lets the kernel withdraw its own request under the durable
    /// Superseded receipt. Host cancels keep the default User reason.
    PreparedCancel prepare_cancel(const RequestHandle& target, uint64_t& next_timeline_ordinal,
                                  CancelReason reason = CancelReason::User);

    InstalledCommand<SubmitResult> install_submit(PreparedSubmit&& prepared) noexcept;
    InstalledCommand<ReplaceResult> install_replace(PreparedReplace&& prepared) noexcept;
    InstalledCommand<CancelResult> install_cancel(PreparedCancel&& prepared) noexcept;

    Preparation<PreparedMutation> prepare_evaluation(const RequestHandle& target,
                                                     const EvaluationContext& context,
                                                     const TargetObservation& observation,
                                                     uint64_t& next_timeline_ordinal);
    /// `grid` carries the run's activation grid (ActivationGrid); the default
    /// is the raw rule, so every existing caller keeps its meaning.
    Preparation<PreparedMutation> prepare_trigger(const RequestHandle& target,
                                                  const TriggerTransition& transition,
                                                  DriverEligibilityClass driver_class,
                                                  uint64_t& next_timeline_ordinal,
                                                  std::optional<Side> cohort_side = std::nullopt,
                                                  const ActivationGrid& grid = {});
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

    /// Append one MarginCallEvent to the immutable history. The target is the
    /// kernel-originated request that filled, which is already terminal by the
    /// time its margin facts are recorded, so no live row moves.
    Preparation<PreparedMutation> prepare_margin_call(const MarginCallEvent& event,
                                                      uint64_t& next_timeline_ordinal);

    /// Append one NativeRiskEvent to the immutable history. It names an
    /// account-level breach rather than a request, so it moves no live row and
    /// carries no definition; the kernel's own response (an opening block, and
    /// with FlattenAndBlock a KernelRisk Flatten) is separate from the record.
    Preparation<PreparedMutation> prepare_risk_event(const NativeRiskEvent& event,
                                                     uint64_t& next_timeline_ordinal);

    /// The allowance that prepare_evaluation would install for this point.
    static Allowance evaluated_allowance(const LiveRequest& live, uint64_t point) noexcept;
    /// Consumer-only no-event form of the ordinary allowance
    /// refresh. It preserves prepare_evaluation's eligibility and liveness
    /// checks while avoiding a transient mutation envelope per driver point.
    void refresh_point_allowances(uint64_t point, const PositionIdentity& position) noexcept;
    bool refresh_allowance(const RequestHandle& target,
                           const EvaluationContext& context,
                           const TargetObservation& observation);
    /// The same refresh, handed the run's timeline: an unbound close that
    /// carried a binding the book still has (ReplaceOptions::keep_binding) is
    /// bound here too, to the BookClose its CloseBoundEvent would install, and
    /// takes that event's ordinal without recording it. The form above leaves
    /// every unbound close to the evaluation.
    bool refresh_allowance(const RequestHandle& target,
                           const EvaluationContext& context,
                           const TargetObservation& observation,
                           uint64_t& next_timeline_ordinal);
    bool refresh_cohort_allowance(const RequestHandle& target,
                                  const EvaluationContext& context,
                                  const TargetObservation& observation);
    /// Pure arithmetic over the cached pending total. Outputs are assigned only
    /// after every validation and subtraction succeeds.
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
    /// The same three lists written over `out`, whose capacity a caller that
    /// keeps it reuses (R5 lane L3): the same handles in the same order.
    void group_recipients(const EventId& applied, std::vector<RequestHandle>& out) const;
    void waiting_children(const RequestHandle& parent, std::vector<RequestHandle>& out) const;
    void bound_close_handles(std::vector<RequestHandle>& out) const;

    Preparation<PreparedMutation> prepare_group_effect(const EventId& applied,
                                                       const RequestHandle& recipient,
                                                       uint64_t& next_timeline_ordinal);
    /// `arm` carries the materialization facts of an anchored leg (the price
    /// tick a rounded anchor snaps to, the host's level restatement); a
    /// default-constructed value is exactly the pre-lane behaviour, so every
    /// existing caller keeps its meaning.
    Preparation<PreparedMutation> prepare_owner_applied(
            const EventId& applied,
            const RequestHandle& child,
            const std::optional<OpeningObservation>& observation,
            uint64_t& next_timeline_ordinal,
            const ArmContext& arm = {});
    Preparation<PreparedMutation> prepare_bound_expiry(const EventId& physical_cause,
                                                       const RequestHandle& child,
                                                       const TargetObservation& observation,
                                                       uint64_t& next_timeline_ordinal);
    Preparation<PreparedMutation> prepare_parent_terminal(const EventId& terminal_or_replaced,
                                                          const RequestHandle& child,
                                                          uint64_t& next_timeline_ordinal);

    /// The direct forms (R5 lane L3). Each apply_* makes the checks of the
    /// prepare_* it is named after, in the same order, and answers what that
    /// prepare_* followed at once by its install answers: the same NoChange or
    /// PreparationError, the same exception at the same point with nothing
    /// changed, and otherwise the same events at the same absolute journal
    /// positions, the same live rows, receipts, chain index and counters, and a
    /// token prepared before it just as stale. What it does not do is stage:
    /// no token, plan or row copy stands between the checks and the write --
    /// the live row is updated where it stands and each event is appended
    /// once. A caller that installs what it prepares straight away uses these;
    /// the prepare/install pairs stay for a caller that holds a token across
    /// other work, or that may decide not to install (an anchored arm whose
    /// restatement hook failed). An execution's direct form comes in the two
    /// halves its caller needs around the settlement it proposes:
    /// check_execution makes prepare_execution's checks and its seal (the same
    /// NoChange, PreparationError or exception, the same reservation and
    /// epoch) and holds nothing; apply_execution, after the settlement, makes
    /// them again and then does what install_execution does with the token
    /// prepare_execution would have made. A caller changes nothing in the core
    /// between the two; if the checks no longer pass, apply_execution answers
    /// StalePreparation and writes nothing.
    CommandInstalled<SubmitResult> apply_submit(const Request& request,
                                                const CommandContext& context,
                                                uint64_t& next_order_incarnation,
                                                uint64_t& next_timeline_ordinal,
                                                RequestOrigin origin = RequestOrigin::Host);
    CommandInstalled<ReplaceResult> apply_replace(const RequestHandle& target,
                                                  const Request& request,
                                                  const CommandContext& context,
                                                  uint64_t& next_order_incarnation,
                                                  uint64_t& next_timeline_ordinal,
                                                  ReplaceOptions options = {});
    CommandInstalled<CancelResult> apply_cancel(const RequestHandle& target,
                                                uint64_t& next_timeline_ordinal,
                                                CancelReason reason = CancelReason::User);
    Preparation<Installed> apply_evaluation(const RequestHandle& target,
                                            const EvaluationContext& context,
                                            const TargetObservation& observation,
                                            uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_trigger(const RequestHandle& target,
                                         const TriggerTransition& transition,
                                         DriverEligibilityClass driver_class,
                                         uint64_t& next_timeline_ordinal,
                                         std::optional<Side> cohort_side = std::nullopt,
                                         const ActivationGrid& grid = {});
    Preparation<Installed> apply_no_effect(const RequestHandle& target,
                                           const EvaluationContext& context,
                                           uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_match_rejected(const RequestHandle& target,
                                                const EvaluationContext& context,
                                                MatchRejectReason reason,
                                                std::optional<ExecutionTerms> attempted_terms,
                                                uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_terms(const RequestHandle& target,
                                       const EvaluationContext& context,
                                       const TermsResolvedInput& input,
                                       uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_margin_call(const MarginCallEvent& event,
                                             uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_risk_event(const NativeRiskEvent& event,
                                            uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_group_effect(const EventId& applied,
                                              const RequestHandle& recipient,
                                              uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_owner_applied(const EventId& applied,
                                               const RequestHandle& child,
                                               const std::optional<OpeningObservation>& observation,
                                               uint64_t& next_timeline_ordinal,
                                               const ArmContext& arm = {});
    Preparation<Installed> apply_bound_expiry(const EventId& physical_cause,
                                              const RequestHandle& child,
                                              const TargetObservation& observation,
                                              uint64_t& next_timeline_ordinal);
    Preparation<Installed> apply_parent_terminal(const EventId& terminal_or_replaced,
                                                 const RequestHandle& child,
                                                 uint64_t& next_timeline_ordinal);
    Preparation<std::monostate> check_execution(const RequestHandle& target,
                                                const ExecutionProposal& proposal,
                                                uint64_t& next_timeline_ordinal);
    InstallResult apply_execution(const RequestHandle& target,
                                  const ExecutionProposal& proposal,
                                  const CommittedExecutionFacts& facts,
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
    /// Resolves tick-spelled trail offsets and trigger anchors in place
    /// against context.price_tick. Runs after validate_request, on the staged
    /// copy that becomes the stored definition.
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
    /// The trailing level for a best and an offset, through the one geometry the
    /// matcher uses (native_matching::checked_trail_stop): finite, and strictly
    /// past the best for a positive offset. `ladder_tick` is the run's declared
    /// price tick (ActivationGrid::ladder_tick); zero keeps the raw subtraction.
    bool trail_level_ok(double best, double offset, bool is_buy, double* stop,
                        double ladder_tick = 0.0) const noexcept;
    /// Whether this run's core accepted a request under `handle`, live or
    /// not: what a cohort command answers UnknownOrigin by.
    bool issued(const RequestHandle& handle) const noexcept;
    /// The first handle of `origin`'s replace chain (RequestDefinition::root),
    /// read from the live row or, for a request that is no longer working,
    /// from the chain index; nullopt for a handle this run never issued.
    std::optional<RequestHandle> canonical_cohort_origin(const RequestHandle& origin) const;
    std::size_t cohort_index(CohortHandle cohort) const noexcept;
    /// Records an accepted request's incarnation and, for a replace
    /// successor, its chain root, in the chain index. Storage is reserved by
    /// reserve_plan, so this cannot throw inside commit.
    void index_committed(const CommandEvent& event) noexcept;
    /// The head of every live deferred group-adjustment chain: the lowest
    /// ordinal retire_history must keep, or nullopt when no chain is live.
    std::optional<uint64_t> pending_chain_floor() const;

    uint64_t usable_ordinal(uint64_t next) const;
    uint64_t usable_incarnation(uint64_t next) const;
    /// The working book's position of the request under `incarnation`, or
    /// live_.size(), while repriced_ names a re-priced request: a search by
    /// queue order (the priority repriced_ names, or the handle's own number).
    std::size_t repriced_position(uint64_t incarnation) const noexcept;
    /// refresh_allowance's body, and its bind for an unbound close that
    /// carried a binding.
    bool refresh_allowance(const RequestHandle& target, const EvaluationContext& context,
                           const TargetObservation& observation,
                           uint64_t* next_timeline_ordinal);
    bool refresh_kept_binding(LiveRequest& live, const EvaluationContext& context,
                              const TargetObservation& observation,
                              uint64_t& next_timeline_ordinal);
    TargetKind classify_repriced(uint64_t incarnation, std::size_t* live_index) const noexcept;
    const LiveRequest* find_repriced(uint64_t incarnation) const noexcept;
    /// Records that the request under `incarnation` now stands at
    /// `priority` (a keep_handle re-price), dropping the entries of requests
    /// that have left the book once they outnumber its rows. The caller has
    /// reserved one entry.
    void note_repriced(uint64_t incarnation, uint64_t priority) noexcept;
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
        // An ordinal the mutation takes with no event: a kept binding's
        // (ReplaceOptions::keep_binding). 0: none.
        uint64_t skipped_ordinal = 0;
        bool add_receipt = false;
        EventId receipt_cause{};
        RequestHandle receipt_recipient{};
        GroupEffect receipt_effect = GroupEffect::Reduce;
        uint64_t receipt_outcome = 0;
    };
    std::optional<InstallError> validate_plan(const MutationPlan& plan) const noexcept;
    // prepare_replace / apply_replace past their checks, for a replace that
    // keeps the handle or carries a binding (ReplaceOptions::keep_handle /
    // keep_binding), and the successor row the two share.
    PreparedReplace prepare_replace_kept(std::size_t live_index, const RequestHandle& staged_target,
                                         Request&& staged, uint64_t ordinal, uint64_t incarnation,
                                         const CommandContext& context, ReplaceOptions options,
                                         MutationPlan&& plan);
    // apply_replace's two instantiations: <false> is a plain replace,
    // <true> (through apply_replace_kept, out of line) one with the options.
    template <bool Kept>
    CommandInstalled<ReplaceResult> apply_replace_as(const RequestHandle& target,
                                                     const Request& request,
                                                     const CommandContext& context,
                                                     uint64_t& next_order_incarnation,
                                                     uint64_t& next_timeline_ordinal,
                                                     ReplaceOptions options);
    CommandInstalled<ReplaceResult> apply_replace_kept(const RequestHandle& target,
                                                       const Request& request,
                                                       const CommandContext& context,
                                                       uint64_t& next_order_incarnation,
                                                       uint64_t& next_timeline_ordinal,
                                                       ReplaceOptions options);
    LiveRequest kept_successor(std::size_t live_index, const RequestHandle& staged_target,
                               Request&& staged, uint64_t ordinal, uint64_t incarnation,
                               const CommandContext& context, ReplaceOptions options) const;
    InstallResult commit(MutationPlan& plan) noexcept;
    MutationPlan begin_plan() const;
    void reserve_plan(const MutationPlan& plan);
    void bind_plan(MutationPlan& plan) const;
    void seal_plan(MutationPlan& plan);
    PreparedMutation finish_mutation(MutationPlan plan);

    // The direct forms' halves of a plan (R5 lane L3). begin_direct makes
    // begin_plan's checks; seal_direct reserves what seal_plan would reserve
    // for `events` events and the live change and takes the seal's epoch;
    // append_direct appends one event exactly as commit does (the ordinal
    // index row, the chain index for a command that consumes an incarnation,
    // the last ordinal); finish_direct takes commit's epoch and answers the
    // range. Nothing after seal_direct can throw.
    void begin_direct() const;
    void seal_direct(std::size_t events, bool push_live, bool add_receipt,
                     bool consume_incarnation);
    template <class Event>
    void append_direct(Event&& event, bool consume_incarnation) noexcept;
    EventRange finish_direct(std::size_t first) noexcept;
    template <class Event>
    Installed end_direct(std::size_t live_index, Event&& event);
    // What prepare_execution computes for its events and its retained row,
    // as values: check_execution and apply_execution make the same checks
    // through it (execution_values), so what apply_execution writes is what
    // the token would have held.
    struct ExecutionValues {
        std::size_t live_index = 0;
        uint64_t ordinal = 0;
        double filled = 0.0;
        bool flatten = false;
        bool units_exhausted = false;
        RemainingProjection remaining_after = RemainingProjectionUnits{};
        Allowance allowance_after = AllowanceUnset{};
        ExecutionScope scope = execution::Book{};
    };
    Preparation<ExecutionValues> execution_values(const RequestHandle& target,
                                                  const ExecutionProposal& proposal,
                                                  uint64_t& next_timeline_ordinal) const;

    RunIdentity identity_;
    std::shared_ptr<InstanceBinding> instance_;
    std::vector<LiveRequest> live_;
    std::vector<CommandEvent> history_;
    uint64_t last_ordinal_ = 0;
    uint64_t last_incarnation_ = 0;
    uint64_t epoch_ = 0;
    // (ordinal, ABSOLUTE journal position) of every retained event, in
    // ordinal order: event_at's lookup. Retiring drops its prefix with the
    // journal's.
    std::vector<std::pair<uint64_t, std::size_t>> ordinal_index_;
    // The journal window: how many events were retired from its front, and
    // the ordinal of the newest of them.
    std::size_t history_base_ = 0;
    uint64_t retired_through_ = 0;
    // The chain index -- the only facts the cohort lookups need about a
    // request that is no longer working, kept instead of its definition: the
    // incarnations this run issued, as disjoint ascending [first, last]
    // ranges (one range for a consumer, which issues them densely), and the
    // chain root of every replace successor, ascending by successor
    // incarnation. Sixteen bytes per replace; nothing else grows with the run.
    std::vector<std::pair<uint64_t, uint64_t>> issued_;
    std::vector<std::pair<uint64_t, uint64_t>> successor_roots_;
    // The requests a keep_handle re-price moved in the queue, ascending by
    // incarnation: (incarnation, priority). The book is held by priority, so
    // a handle lookup reads its priority here; a handle absent here is at its
    // own incarnation. Derived from the book (each row's definition carries
    // its priority), so nothing folds it; an entry outlives its request
    // until the next re-price prunes it, and a lookup through it finds no
    // row because no other request takes that priority.
    std::vector<std::pair<uint64_t, uint64_t>> repriced_;
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
static_assert(std::variant_size_v<CommandEvent> == 19);
static_assert(std::variant_size_v<ExecutionPlan> == 4);
static_assert(std::variant_size_v<ExecutionScope> == 3);
static_assert(std::variant_size_v<TriggerState> == 9);
static_assert(std::variant_size_v<TriggerAnchor> == 2);

}  // inline namespace native_order_v7
}  // namespace pineforge::native_order

namespace std {
template <>
struct hash<pineforge::native_order::CohortHandle> {
    std::size_t operator()(pineforge::native_order::CohortHandle value) const noexcept {
        return static_cast<std::size_t>(value.value ^ (value.value >> 32));
    }
};
}  // namespace std
