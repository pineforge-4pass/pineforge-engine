#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/market_driver.hpp>
#include <pineforge/native_fx_curve.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v18 {

/// Where a host stands. Read it off native_state().kind; nothing else reports it.
/// Unconfigured is a fresh host, Ready a staged spec, Running a consumed begin
/// (with NativeRunPhase saying which driving), Completed a finished run whose
/// lots and live requests stay visible but not actionable, and Failed a durable
/// first failure: discard the host, replay on a fresh instance, never reconfigure
/// in place. Pinned by tests/test_native_host_repairs.cpp.
enum class NativeLifecycleKind : std::uint8_t {
    Unconfigured = 0,
    Ready = 1,
    Running = 2,
    Completed = 3,
    Failed = 4,
};

/// Which driving is Running: a batch run(), a stream's internal warmup, or its
/// realtime leg. Folded into native_continuation_hash() on purpose — a consumer
/// mid-warmup and one mid-realtime are not interchangeable continuations — which
/// is why a batch and a stream over identical bars record different per-bar
/// broker-state hashes while booking identical trades.
enum class NativeRunPhase : std::uint8_t {
    Batch = 0,
    Warmup = 1,
    Realtime = 2,
};

/// How a Completed run ended: a batch reached the last bar, or a stream was
/// ended. Reporting only; the book, the report and every hash are what the run
/// left.
enum class NativeCompletion : std::uint8_t {
    BatchComplete = 0,
    StreamEnded = 1,
};

/// The durable first failure of a run, in NativeFailure::code. InvalidSpecification
/// is a refused spec at configure, Contract a lifecycle misuse (a second
/// configure, a run number at or under the high water), Preflight a bar array the
/// driver refused, UnsupportedSource a source-only setter or command on a bare
/// host, CallbackException a host callback that threw or a C callback that
/// returned non-zero, and Aborted a cooperative abort. The rest are internal
/// exhaustion states with no rollback promise. Set once and latched: later run /
/// stream_* / configure_native calls refuse.
enum class NativeFailureCode : std::uint16_t {
    None = 0,
    InvalidSpecification = 1,
    Contract = 2,
    Preflight = 3,
    UnsupportedSource = 4,
    CallbackException = 5,
    SettlementFailure = 6,
    Allocation = 7,
    CounterExhausted = 8,
    Aborted = 9,
    ProjectionMismatch = 10,
    Calendar = 11,
    Unexpected = 12,
};

/// Which operation was in flight when the failure latched — the second half of
/// "what went wrong", beside NativeFailureCode. Presentation only; the kernel
/// takes no decision on it.
enum class NativeFailureOperation : std::uint16_t {
    None = 0,
    Configure = 1,
    Begin = 2,
    Command = 3,
    Input = 4,
    Callback = 5,
    Settlement = 6,
    Mutation = 7,
    Stream = 8,
};

/// In-run failure references. Identifiers belong to NativeFailed.spec->identity;
/// they never carry a second RunIdentity string. Kind is a bit-union of the
/// typed alternatives below (Cause=1, Recipient=2, Cursor=4).
enum class NativeFailureContextKind : std::uint8_t {
    None = 0,
    Cause = 1,
    Recipient = 2,
    CauseRecipient = 3,
    Cursor = 4,
    CauseCursor = 5,
    RecipientCursor = 6,
    CauseRecipientCursor = 7,
};

/// The event ordinal a failure was caused by, 0 when absent. Populated only when
/// native_failure_has_cause() is true for the context's kind.
struct NativeInRunCause {
    std::uint64_t ordinal = 0;  // 0 = absent
};

/// The request incarnation a failure was addressed to, 0 when absent. Populated
/// only when native_failure_has_recipient() is true.
struct NativeInRunRecipient {
    std::uint64_t incarnation = 0;  // 0 = absent
};

/// Where on the modeled path a failure happened: the driver point's coordinate and
/// its interpolation position t. Populated only when native_failure_has_cursor()
/// is true.
struct NativeInRunCursor {
    NativeCoordinate point{};
    double t = 0.0;
};

/// The three in-run facts a failure may carry, tagged by kind. Copy and move are
/// allocation-free — this is what a failed host reports from inside a callback —
/// and the predicates below say which alternatives are populated rather than
/// asking a caller to decode the bit-union itself.
struct NativeFailureContext {
    NativeFailureContextKind kind = NativeFailureContextKind::None;
    NativeInRunCause cause{};
    NativeInRunRecipient recipient{};
    NativeInRunCursor cursor{};
};

/// The bit-union under NativeFailureContextKind: Cause=1, Recipient=2, Cursor=4.
/// Prefer the three predicates below to testing these bits by hand.
constexpr std::uint8_t native_failure_context_bits(NativeFailureContextKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}
/// Whether a context of this kind populates its NativeInRunCause.
constexpr bool native_failure_has_cause(NativeFailureContextKind kind) noexcept {
    return (native_failure_context_bits(kind) & 1u) != 0;
}
/// Whether a context of this kind populates its NativeInRunRecipient.
constexpr bool native_failure_has_recipient(NativeFailureContextKind kind) noexcept {
    return (native_failure_context_bits(kind) & 2u) != 0;
}
/// Whether a context of this kind populates its NativeInRunCursor.
constexpr bool native_failure_has_cursor(NativeFailureContextKind kind) noexcept {
    return (native_failure_context_bits(kind) & 4u) != 0;
}
/// Whether this context populates its NativeInRunCause. The overload a reader of
/// native_state().failure.context wants.
constexpr bool native_failure_has_cause(const NativeFailureContext& context) noexcept {
    return native_failure_has_cause(context.kind);
}
/// Whether this context populates its NativeInRunRecipient.
constexpr bool native_failure_has_recipient(const NativeFailureContext& context) noexcept {
    return native_failure_has_recipient(context.kind);
}
/// Whether this context populates its NativeInRunCursor.
constexpr bool native_failure_has_cursor(const NativeFailureContext& context) noexcept {
    return native_failure_has_cursor(context.kind);
}

/// The kind that says exactly these three alternatives are populated.
constexpr NativeFailureContextKind native_failure_context_kind(
        bool cause, bool recipient, bool cursor) noexcept {
    return static_cast<NativeFailureContextKind>(
            (cause ? 1u : 0u) | (recipient ? 2u : 0u) | (cursor ? 4u : 0u));
}

/// A context carrying one causing event ordinal. A zero ordinal is "absent" and
/// yields the None kind rather than a Cause with nothing in it.
inline NativeFailureContext native_failure_cause(std::uint64_t ordinal) noexcept {
    NativeFailureContext context;
    if (ordinal == 0) return context;
    context.kind = NativeFailureContextKind::Cause;
    context.cause.ordinal = ordinal;
    return context;
}
/// A context carrying one addressed request incarnation. A zero incarnation is
/// "absent", as above.
inline NativeFailureContext native_failure_recipient(std::uint64_t incarnation) noexcept {
    NativeFailureContext context;
    if (incarnation == 0) return context;
    context.kind = NativeFailureContextKind::Recipient;
    context.recipient.incarnation = incarnation;
    return context;
}
/// A context carrying one path cursor. A cursor is always meaningful, so there is
/// no absent spelling.
inline NativeFailureContext native_failure_cursor(NativeCoordinate point, double t = 0.0) noexcept {
    NativeFailureContext context;
    context.kind = NativeFailureContextKind::Cursor;
    context.cursor.point = point;
    context.cursor.t = t;
    return context;
}

/// Copies only ordinals/incarnation/cursor scalars. Foreign run identities are
/// dropped rather than stored under the failed spec's identity.
inline NativeFailureContext native_failure_context_in_run(
        const native_order::RunIdentity& run,
        const native_order::EventId* cause,
        const native_order::RequestHandle* recipient,
        const native_order::MatchCursor* cursor) noexcept {
    NativeFailureContext context;
    bool has_cause = false;
    bool has_recipient = false;
    bool has_cursor = false;
    if (cause != nullptr && cause->ordinal != 0 && cause->run == run) {
        has_cause = true;
        context.cause.ordinal = cause->ordinal;
    }
    if (recipient != nullptr && recipient->incarnation != 0 && recipient->run == run) {
        has_recipient = true;
        context.recipient.incarnation = recipient->incarnation;
    }
    if (cursor != nullptr) {
        has_cursor = true;
        context.cursor.point = cursor->point;
        context.cursor.t = cursor->t;
    }
    context.kind = native_failure_context_kind(has_cause, has_recipient, has_cursor);
    return context;
}

/// The durable record native_state().failure answers: the code, the operation it
/// happened in, an optional event ordinal and discriminator, and the
/// allocation-free context. last_error() is presentation text beside it and is
/// never the authority. Copy and move do not allocate.
struct NativeFailure {
    NativeFailureCode code = NativeFailureCode::None;
    NativeFailureOperation operation = NativeFailureOperation::None;
    std::uint64_t ordinal = 0;       // 0 = absent
    std::uint32_t discriminator = 0;
    NativeFailureContext context{};
};

/// The NativeLifecycle alternatives, one per NativeLifecycleKind. Ready, Running
/// and Completed own a copy of the staged spec; Failed owns it only when configure
/// got that far. NativeStateView is the flattened read a host uses; this variant is
/// the consumer's own storage.
struct NativeUnconfigured {};
struct NativeReady { NativeRunSpec spec; };
struct NativeRunning { NativeRunSpec spec; NativeRunPhase phase = NativeRunPhase::Batch; };
struct NativeCompleted { NativeRunSpec spec; NativeCompletion completion = NativeCompletion::BatchComplete; };
struct NativeFailed { std::optional<NativeRunSpec> spec; NativeFailure failure; };

/// The lifecycle as one value. Exhaustive over the five alternatives above.
using NativeLifecycle = std::variant<NativeUnconfigured, NativeReady, NativeRunning,
                                     NativeCompleted, NativeFailed>;

/// The RunIdentity the failed spec carried, or nullptr when configure failed
/// before staging one. This is how a caller names the run a failure belongs to
/// without the failure carrying a second identity string of its own.
inline const native_order::RunIdentity* native_failed_run_identity(
        const NativeFailed& failed) noexcept {
    return failed.spec ? &failed.spec->identity : nullptr;
}

static_assert(std::is_trivially_copyable_v<NativeInRunCause>);
static_assert(std::is_trivially_copyable_v<NativeInRunRecipient>);
static_assert(std::is_trivially_copyable_v<NativeInRunCursor>);
static_assert(std::is_trivially_copyable_v<NativeFailureContext>);
static_assert(std::is_trivially_copyable_v<NativeFailure>);
static_assert(std::is_nothrow_copy_constructible_v<NativeFailure>);
static_assert(std::is_nothrow_copy_assignable_v<NativeFailure>);
static_assert(std::is_nothrow_move_constructible_v<NativeFailed>);
static_assert(std::is_nothrow_move_assignable_v<NativeFailed>);

/// The flattened run state native_state() answers: the lifecycle kind, a borrowed
/// pointer to the staged spec (nullptr when Unconfigured), the phase, how a
/// Completed run ended, the durable failure, the consumed run-number high water
/// and the monotonic decision floor. The spec pointer is valid until the next
/// configure or begin. C spelling: strategy_native_state_v1.
struct NativeStateView {
    NativeLifecycleKind kind = NativeLifecycleKind::Unconfigured;
    const NativeRunSpec* spec = nullptr;
    NativeRunPhase phase = NativeRunPhase::Batch;
    NativeCompletion completion = NativeCompletion::BatchComplete;
    NativeFailure failure{};
    uint64_t consumed_high_water = 0;
    int64_t decision_floor_ms = 0;
};

/// The book as one aggregate: signed units, the volume-weighted average entry
/// price and the number of open physical lots. physical_position() answers it;
/// native_open_lots(mark) is the same book lot by lot. C spelling:
/// strategy_native_position_v1.
struct NativePhysicalPosition {
    double signed_units = 0.0;
    double average_price = 0.0;
    std::size_t lot_count = 0;
};

/// Owning value row for one open physical lot, copied at query time by
/// native_open_lots(mark) — the lot-by-lot view of the same book
/// physical_position() aggregates. Every field is what the book already
/// holds; nothing is computed that the kernel does not already keep.
///
/// Identity: `ordinal` is the lot's position in the book (oldest first),
/// `entry_incarnation` the request record whose fill opened it (never
/// reused; 0 only for a legacy synthetic lot) and `cycle` the position cycle
/// the lot belongs to (the value BindOpening / BindOpenings name).
///
/// Booking: `entry_label` / `entry_comment` are the opening request's own,
/// `entry_time_ms` / `entry_bar_index` its fill point, `entry_price` the
/// booked price, `signed_units` the lot's remaining units (> 0 long, < 0
/// short) and `entry_commission` the entry fee still on the lot in account
/// currency — a partial realization takes its share with it.
///
/// Marked: `unrealized_pnl` is the lot's own term of native_marked_equity(mark)
/// — the move from `entry_price` to `mark`, in account currency, less
/// `entry_commission` — so the marked equity is the realized balance plus the
/// sum of these rows. `favorable_excursion` / `adverse_excursion` are the
/// largest moves for and against the lot the kernel has sampled along the
/// delivered path, in account currency, with `mark` itself folded in; both
/// are gross of fees and never negative. A host that owns lot excursions
/// (owns_lot_excursions) keeps its own sampler, so for that run the kernel's
/// two fields fold `mark` alone. A NaN `mark` keeps every booking fact,
/// leaves `unrealized_pnl` NaN and folds nothing into the excursions.
struct NativeOpenLot {
    std::size_t ordinal = 0;
    std::uint64_t entry_incarnation = 0;
    std::int64_t cycle = 0;
    native_order::Side side = native_order::Side::Long;
    std::string entry_label;
    std::string entry_comment;
    std::int64_t entry_time_ms = 0;
    int entry_bar_index = -1;
    double entry_price = 0.0;
    double signed_units = 0.0;
    double entry_commission = 0.0;
    double mark = std::numeric_limits<double>::quiet_NaN();
    double unrealized_pnl = std::numeric_limits<double>::quiet_NaN();
    double favorable_excursion = 0.0;
    double adverse_excursion = 0.0;
};

/// The account row that follows an applied execution in the event history: the
/// shared ordinal, the effective time, the marked equity, the realized balance and
/// the signed book after it. One per applied execution, so an applied event and
/// its observation share an ordinal — advance an event cursor by the last returned
/// ordinal, never mid-group.
struct NativeAccountObservation {
    uint64_t ordinal = 0;
    int64_t effective_time_ms = 0;
    double marked_equity = 0.0;
    double realized_balance = 0.0;
    double signed_units = 0.0;
};

/// Which of NativeMarketEvent's three alternatives is populated.
enum class NativeEventKind : std::uint8_t {
    Command = 0,
    Driver = 1,
    Account = 2,
};

/// One owning row of native_events(after_ordinal): a command event, a driver point
/// or an account observation, tagged by kind. Later commands and the next run's
/// reset do not invalidate a row already returned. C spelling:
/// strategy_native_events_v1, which flattens the same rows into one tagged POD.
struct NativeMarketEvent {
    NativeEventKind kind = NativeEventKind::Command;
    uint64_t ordinal = 0;
    std::optional<native_order::CommandEvent> command;
    std::optional<NativeDriverPoint> driver;
    std::optional<NativeAccountObservation> account;
};

/// Whether a setup call applied its value or refused it. Failed leaves engine
/// storage untouched.
enum class NativeSetupStatus : std::uint8_t { Applied = 0, Failed = 1 };

/// What configure_native answers: the status and, on refusal, the first error
/// field validate_native_run_spec found. A refusal changes nothing — there is no
/// partial apply.
struct NativeSetupResult {
    NativeSetupStatus status = NativeSetupStatus::Failed;
    NativeRunSpecValidation validation{};
};

/// What configure_native_fx_curve answers: the status and, on refusal, the curve
/// validation with the index of the first bad point. A refusal leaves the staged
/// curve as it was.
struct NativeFxCurveSetupResult {
    NativeSetupStatus status = NativeSetupStatus::Failed;
    NativeFxCurveValidation validation{};
};

/// Which price a current execution settles at: the active callback's quote as
/// presented, or that quote on the instrument's nearest tick. Configured
/// directional slippage applies once either way.
enum class NativeCurrentPriceRule : std::uint8_t { AsPresented = 0, NearestTick = 1 };
/// Where a price came from: the callback's own market decision point, or the
/// execution it is anchored to. A current execution inherits its cause's quote, so
/// a chain does not compound slippage.
enum class NativeCurrentQuoteKind : std::uint8_t { MarketDecision = 0, ExecutionAnchor = 1 };

/// Read-only owning-value facts for one candidate. The host is already the
/// engine, so no engine handle or prepared execution token is exposed here.
struct NativeExecutionTermsFacts {
    native_order::RequestHandle target;
    native_order::DefinitionRef definition;
    native_order::MatchCursor cursor;
    native_order::DriverEligibilityClass driver_class;
    native_order::TriggerState trigger_state;
    native_order::Remaining remaining;
    native_order::Allowance allowance;
    native_order::ExecutionScope scope;
    double scope_exposure_units = 0.0;
    NativePhysicalPosition position;
    double opposite_book_units = 0.0;
    bool is_buy = false;
    native_order::NativeCandidatePriceKind price_kind =
        native_order::NativeCandidatePriceKind::PointPrice;
    /// A retained crossing can share a rounded cursor quote with another
    /// request's level. The receipt keeps this distinction durable.
    bool shared_cursor_collision = false;
    double raw_price = 0.0;
    std::optional<double> trigger_level;
    NativeCurrentQuoteKind quote_kind = NativeCurrentQuoteKind::MarketDecision;
    NativeCurrentPriceRule price_rule = NativeCurrentPriceRule::AsPresented;
    double default_resolved_price = 0.0;
    std::int64_t fx_effective_time_ms = 0;
    double active_fx = 1.0;
    double pending_group_deduction = 0.0;
};

/// Ephemeral factual view of one prepared execution before any physical effect.
struct NativePrecommitView {
    native_order::RequestHandle target;
    native_order::DefinitionRef definition;
    native_order::MatchCursor cursor;
    native_order::ExecutionPlan plan;
    native_order::ExecutionScope scope;
    double raw_price = 0.0;
    double resolved_price = 0.0;
    double inspected_closed_units = 0.0;
    double inspected_opened_units = 0.0;
    double inspected_current_ticket = 0.0;
    execution::Status settlement_readiness = execution::Status::Applied;
    execution::AccountEffectProjection account;
    std::vector<double> closed_row_pnl;
    bool current = false;
};

/// The host is consulted before generic opening-margin admission.  Admit keeps
/// the native default gate; AdmitWithHostMargin lets a host that owns the
/// source-compatible margin rule take responsibility for that one check.
/// Proceed remains an alias for the v7 spelling used by existing C++ callers.
enum class NativePrecommitVerdict : std::uint8_t {
    Admit = 0,
    Proceed = Admit,
    Refuse = 1,
    AdmitWithHostMargin = 2,
};

/// Ephemeral factual view of one kernel-issued liquidation before its units
/// are fixed. `position` is the physical book being liquidated, `mark` the
/// sizing price the kernel measured the breach at, `equity` the marked equity
/// there and `required` the maintenance requirement of the whole position at
/// that same mark. A host that answers with a value owns the slice quantity.
struct NativeMarginCallView {
    NativePhysicalPosition position;
    double mark = 0.0;
    double equity = 0.0;
    double required = 0.0;
    native_order::MatchCursor cursor;
};

/// Which kernel check point is about to test the maintenance requirement.
/// BarOpen is the script bar's open, after on_native_bar_open and before the
/// bar's own matching; AfterApplied is the re-arm that follows a point's
/// applied fills, which is the kernel's only mid-path check; Calculation is
/// the script calculation of a CalculationOnly model; FxRoll is a step of the
/// run's declared NativeFxCurve: the first driver point the account converts
/// at a different rate than the point before it, offered immediately before
/// that point is matched, with the price where the walk left it -- the
/// requirement moved though no price did. A run that declares no curve has no
/// such point, and a CalculationOnly model, which measures at its calculation
/// alone, is not offered it. These are the kernel's own points: a broker model
/// that checks somewhere else is a host policy, expressed by suppressing the
/// points it does not share.
enum class NativeMarginCheckKind : std::uint32_t {
    BarOpen = 0,
    AfterApplied = 1,
    Calculation = 2,
    FxRoll = 3,
};

/// Ephemeral factual view of one kernel check point, offered to the host
/// before the check runs. `mark` is the price the kernel would measure the
/// breach at; the modeled path phase is `cursor.point.path_phase`.
struct NativeMarginCheckPoint {
    NativeMarginCheckKind kind = NativeMarginCheckKind::BarOpen;
    NativePhysicalPosition position;
    double mark = 0.0;
    native_order::MatchCursor cursor;
    /// Whether the model already holds a liquidation resting from an earlier
    /// admitted point. A host that suppresses points -- and therefore owns
    /// when a slice sized on a book that has since shrunk is re-sized -- needs
    /// to tell "re-size the live slice" from "take a new one" apart. The
    /// default host admits every point and never reads this.
    bool liquidation_resting = false;
};

/// Ephemeral factual view of the numbers the kernel is about to compare, at
/// one check point, BEFORE the breach test. `equity` is the marked equity on
/// the model's own basis and `required` the kernel's maintenance requirement
/// of the whole position at `mark`; both are exactly what the kernel would
/// compare if the host answered nullopt.
struct NativeMarginRequirementView {
    NativeMarginCheckKind kind = NativeMarginCheckKind::BarOpen;
    NativePhysicalPosition position;
    double mark = 0.0;
    double equity = 0.0;
    double required = 0.0;
    native_order::MatchCursor cursor;
};

/// The host's answer to one requirement view. `required` and `equity` replace
/// the kernel's two numbers for this check point only -- they are a broker's
/// money rule (a rounded requirement, a fee-adjusted equity), never a second
/// account. `force_breach` makes the kernel proceed past `required > equity`
/// even when the answered numbers do not meet it; the sizing policy and
/// resolve_margin_call_units then decide the slice as usual.
struct NativeMarginDecision {
    double required = 0.0;
    double equity = 0.0;
    bool force_breach = false;
};

/// Which trigger of an anchored leg the owner's fill is about to supply.
enum class NativeAnchoredTrigger : std::uint8_t {
    Limit = 0,
    Stop = 1,
    TrailArm = 2,
};

/// Ephemeral read-only facts of one anchored-leg materialization (L7b),
/// offered to the host exactly once, before the ArmedEvent is built. `owner`
/// is the request whose fill arms the leg, `owner_applied_ordinal` that fill's
/// ExecutionAppliedEvent, `owner_lot_incarnation` the lot it opened,
/// `owner_fill_price` its resolved price and `owner_cursor` its cursor. `leg`
/// is the anchored request, `leg_side` the side it trades on (a closing leg
/// trades against the lot the fill opened), `trigger` which trigger receives
/// the level, `offset` the anchor's resolved offset in price units,
/// `price_tick` the run's tick (0 when the run has none) and `kernel_level`
/// the level the kernel would install: fill + offset after the anchor's own
/// rounding. A host that answers with a value owns the installed level; the
/// kernel's representability check still applies.
struct NativeAnchoredLevelView {
    native_order::RequestHandle owner;
    std::uint64_t owner_applied_ordinal = 0;
    std::uint64_t owner_lot_incarnation = 0;
    double owner_fill_price = 0.0;
    native_order::MatchCursor owner_cursor;
    native_order::RequestHandle leg;
    native_order::Side leg_side = native_order::Side::Long;
    NativeAnchoredTrigger trigger = NativeAnchoredTrigger::Limit;
    double offset = 0.0;
    double price_tick = 0.0;
    double kernel_level = 0.0;
};

/// The run's generic risk ledger (L9), as the kernel holds it. Every field is
/// zero / absent for a run that declares no NativeRunSpec::risk.
///
/// `blocked` is whether openings are refused right now, and `reason` names the
/// limit that did it — a drawdown or consecutive-loss-days block lasts to the
/// end of the run, the two intraday blocks to the end of their own day.
/// `day_ordinal` is the risk day the ledger is on (the spec's own day basis),
/// `fills_today` the applied fills counted in it, `consecutive_loss_days` the
/// streak of days that closed with a realized loss, `peak_equity` the running
/// peak of the marked equity and `day_open_equity` the equity the current day
/// opened at. `has_day` is false before the first evaluated point.
struct NativeRiskState {
    bool blocked = false;
    std::optional<native_order::RiskLimitKind> reason;
    bool has_day = false;
    std::int64_t day_ordinal = 0;
    std::uint64_t fills_today = 0;
    std::uint32_t consecutive_loss_days = 0;
    double peak_equity = 0.0;
    double day_open_equity = 0.0;
};

/// What current_execution_point() answers inside a callback: the point's decision
/// context, its price, which quote that price is, and the ordinal the quote came
/// from. nullopt outside a decision point. C spelling: pf_native_decision_v1's
/// price and quote_kind, on every callback.
struct NativeCurrentPointView {
    NativeDecisionContext decision;
    double price = 0.0;
    NativeCurrentQuoteKind quote_kind = NativeCurrentQuoteKind::MarketDecision;
    std::uint64_t quote_origin_ordinal = 0;
};

/// Read-only projection of a live generic Trail request. Before its arm is
/// reached, activated is false and the numeric/ordinal fields are zero. Once
/// armed, best_price and current_level are the exact raw matcher values and
/// activation_ordinal identifies the TrailArm event that began tracking.
struct NativeTrailState {
    bool activated = false;
    double best_price = 0.0;
    double current_level = 0.0;
    std::uint64_t activation_ordinal = 0;
};

/// Owning value row for one live request, copied at query time. The
/// definition is the accepted (and, for an anchored leg, already materialized)
/// request; remaining is what is left to execute; trigger_state is where the
/// request's own trigger has reached. Later commands do not invalidate a row
/// that was already returned.
struct NativeWorkingRequest {
    native_order::DefinitionRef definition;
    native_order::RemainingProjection remaining = native_order::RemainingProjectionUnbound{};
    native_order::TriggerState trigger_state = native_order::MarketReady{};
};

/// Which of a request's two free-text identity fields a bulk predicate reads.
/// Both are host text the kernel only copies and compares: Comment is the
/// established cancel_where selector and keeps value 0, Label is the field a
/// host that names its orders puts its own id in.
enum class NativeRequestField : std::uint8_t { Comment = 0, Label = 1 };

/// Why a current-execution command cannot be consumed here. Every value is a fact
/// about the command or the phase, never a host verdict: the host's own veto is
/// validate_execution_precommit. strategy_native_execute_current_v1 answers the
/// same verdicts in C.
enum class NativeCurrentRefusal : std::uint8_t {
    NoExecutionContext = 0, Reentrant = 1, InvalidHandle = 2, NotWorking = 3,
    NotAcceptedInCallback = 4, UnsupportedRequest = 5, UnreadyOwner = 6,
    InvalidSelection = 7, ConfigurationMismatch = 8,
};

/// A synchronous execution of one request accepted or replaced in this very
/// callback: a target handle and a price rule, and nothing else. Membership,
/// quantity and ownership come from the request; this command cannot supply
/// another selection, a saved cursor, a price, a ticket or a prepared financial
/// plan. Pinned by tests/test_native_current_execution.cpp.
struct NativeCurrentExecution {
    native_order::RequestHandle target;
    NativeCurrentPriceRule price_rule = NativeCurrentPriceRule::AsPresented;
};

/// Recomputed observations, never an apply token. Readiness is the financial
/// pre-source preparation boundary, independent of account projection validity
/// and excluding opening admission and late counter/lifecycle checks.
struct NativeCurrentExecutionPreview {
    std::optional<NativeCurrentRefusal> refusal;
    std::optional<execution::Status> settlement_readiness;
    execution::AccountEffectProjection account;
    std::vector<double> closed_row_pnl;
    std::optional<native_order::MatchRejectReason> terms_rejection;
    std::optional<native_order::CancelReason> terms_cancellation;
};

/// What execute_current answers: a refusal, or the applied outcome. Execution
/// revalidates, so a preview obtained earlier is an observation and never an
/// apply token.
using NativeCurrentExecutionResult = std::variant<NativeCurrentRefusal,
    native_order::ExecutionAppliedEvent, native_order::NoEffectEvent,
    native_order::MatchRejectedEvent, native_order::CancelledEvent>;

/// Borrowed begin-call facts. The bar/input/syminfo/override pointers expire when
/// prepare_native_begin returns; retained configuration must copy them by
/// value (for example into NativeRunSpec::intrabar).
struct NativeBeginArgs {
    const Bar* bars = nullptr;
    int n = 0;
    std::string input_tf;
    std::string script_tf;
    bool bar_magnifier = false;
    int magnifier_samples = 4;
    MagnifierDistribution magnifier_distribution = MagnifierDistribution::ENDPOINTS;
    bool magnifier_volume_weighted = false;
    int magnifier_volume_weighted_min_samples = 2;
    int magnifier_volume_weighted_max_samples = 64;
    const InputsMap* inputs = nullptr;
    /// The rich run overload's symbol metadata is borrowed only for this
    /// callback.  A provider that uses it must copy the fields it needs into
    /// its retained NativeRunSpec/staged metadata before returning.
    const SymInfo* syminfo = nullptr;
    const void* overrides_opaque = nullptr;
    bool is_stream = false;
    int warmup_n = 0;
    /// Which public overload began the run: the bare run(bars, n) lifecycle
    /// (true) or a timeframe-aware / magnified / stream begin (false).  A host
    /// may keep lifecycle surfaces (for example its higher-timeframe series
    /// evaluators) off for the bare overload; empty timeframes alone do not
    /// identify it, they only request auto-detection.
    bool simple_run = false;
};

/// Accepted input facts presented before the generic consumer aggregates the
/// bar into its script interval or evaluates any matching point. This is not a
/// source-language callback: native hosts may observe raw input cadence through
/// it without taking ownership of matching or aggregation.
struct NativeInputContext {
    native_calendar::NativeInterval input_interval{};
    native_calendar::NativeInterval script_interval{};
    int input_index = 0;
    bool completes_script_interval = false;
};

/// One accepted realtime print before native matching at its current decision
/// point. The Bar is a value presentation of that print (O=H=L=C=price,
/// volume=print quantity, timestamp=print timestamp); no source-language
/// policy is embedded here. Sequence zero retains the public TradeTick
/// sentinel meaning “provider did not supply a sequence”.
struct NativeTickContext {
    NativeDecisionContext decision{};
    std::uint64_t sequence = 0;
};

/// One completed higher-timeframe bucket of a declared subscription
/// (NativeRunSpec::subscriptions), presented before the calculation of the
/// input bar it is delivered on.
///
/// `subscription` indexes NativeRunSpec::subscriptions. `interval` is the
/// calendar span of the bucket's FIRST contributing input bar, read through the
/// run's own session calendar; it is left zeroed when that lookup has no answer.
/// For a series built from the auxiliary feed (NativeSeriesSource::AuxiliaryFeed)
/// "contributing bar" reads "contributing FEED bar" here, while `delivered_at_ms`
/// stays the accepted input bar the delivery rides on.
/// `completion` is Confirmed when the bucket completed on its own last
/// contributing input bar and LazyComplete when the next period's first input
/// closed it. `delivered_at_ms` is the timestamp of the input bar the delivery
/// rides on: the bucket's last contributing bar under lookahead_off and its
/// first under lookahead_on.
struct NativeTimeframeBarContext {
    std::size_t subscription = 0;
    native_calendar::NativeInterval interval{};
    NativeCompletionKind completion = NativeCompletionKind::Confirmed;
    std::int64_t delivered_at_ms = 0;
};

/// Why the kernel is asking the host to calculate. BarClose is the script
/// bar's own calculation and is delivered for every run, whatever the spec's
/// NativeCalculationTrigger is: every calculation is routed through
/// on_native_recalculate, whose default forwards to on_native_bar, so a host
/// that only implements on_native_bar sees exactly what it saw before.
/// OrderFill is one recalculation at the cursor of an applied execution
/// (NativeCalculationTrigger::BarCloseAndFills and above) and carries that
/// event as its cause. Tick is one recalculation at a modeled path point or
/// an observed print (NativeCalculationTrigger::EveryModeledPoint). SubBar is
/// reserved: a lower-timeframe sub-bar has its own hook, on_native_sub_bar,
/// and is never delivered through on_native_recalculate.
enum class NativeCalculationReason : std::uint8_t {
    BarClose = 0,
    OrderFill = 1,
    Tick = 2,
    SubBar = 3,
};

// Most-derived native strategy host. Binds NativeExecutionConsumer in the
// protected engine constructor. Noncopyable and nonmovable. Lives in the
// same inline engine epoch as BacktestEngine so old-header/new-library
// linkage cannot resolve an unversioned constructor against a different
// base layout.
#define PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18 1
/// The public native host: an abstract subclass of BacktestEngine with no
/// PineScript on it. Subclass it, override on_native_bar (the only pure-virtual),
/// configure_native(spec), then run() or the stream_* family. Noncopyable and
/// nonmovable: the constructor binds the native consumer, and there is no
/// attach/replace switch. Do not override the inherited on_bar (it is final) and
/// do not write protected engine fields. The whole surface, feature by feature, is
/// docs/pages/native-engine.md; a worked host is examples/native/hello_kernel.cpp.
class NativeStrategyHost : public BacktestEngine {
public:
    NativeStrategyHost();
    NativeStrategyHost(const NativeStrategyHost&) = delete;
    NativeStrategyHost& operator=(const NativeStrategyHost&) = delete;
    NativeStrategyHost(NativeStrategyHost&&) = delete;
    NativeStrategyHost& operator=(NativeStrategyHost&&) = delete;
    ~NativeStrategyHost() override;

    /// The engine's own bar entry, taken over by the native consumer and sealed. A
    /// native host calculates in on_native_bar; this override is what makes overriding
    /// on_bar a compile error rather than a silently dead callback.
    void on_bar(const Bar& bar) final;

    /// Offered once per begin, before the run starts, with the begin's own arguments —
    /// the bars, the timeframe literals, the magnifier settings, and the rich
    /// overload's InputsMap / SymInfo / opaque overrides. A provider that reads them
    /// must copy what it needs before returning: the views expire with the call. The
    /// default does nothing, which is every bare host. No C spelling: a C run is
    /// declared up front with strategy_configure_native_ext_v1.
    virtual void prepare_native_begin(const NativeBeginArgs&) {}
    /// Offered once per successful begin, after the reset and before any bar. It is
    /// the one place declare_timeframe_subscriptions and declare_auxiliary_feed are
    /// legal, and the kernel registers the declared series only after it returns — so
    /// a host that registers evaluators of its own here keeps them. native_series_bar
    /// answers nullopt for every index inside it, because nothing is registered yet.
    /// C spelling: pf_native_callbacks_v1::on_run_begin.
    virtual void on_native_run_begin() {}
    /// Called once for every accepted confirmed input bar, before that bar is
    /// aggregated or matched. It has no current execution point.
    virtual void on_native_input(const Bar&, const NativeInputContext&) {}
    /// Called once for every accepted realtime print, before matching at that
    /// point. inspect_current_execution/execute_current are legal here.
    virtual void on_native_tick(const Bar&, const NativeTickContext&) {}
    /// One completed bucket of a declared higher-timeframe subscription,
    /// delivered on an accepted input bar before that input is aggregated,
    /// matched or calculated. Never called for a spec whose `subscriptions`
    /// are empty. native_series_bar() already answers with this bar here.
    /// A series built from the auxiliary feed may deliver several buckets on
    /// one input, oldest first.
    virtual void on_native_timeframe_bar(const Bar&, const NativeTimeframeBarContext&) {}
    /// Precedes the matching pass at the script bar's open decision point.
    /// inspect_current_execution/execute_current are legal in this hook.
    virtual void on_native_bar_open(const Bar&, const NativeDecisionContext&) {}
    /// The current decision point remains valid for the complete callback.
    /// A host may therefore execute a command after its own script-body work
    /// returns, before the consumer advances beyond this calculation point.
    virtual void on_native_bar(const Bar& bar, const NativeDecisionContext& context) = 0;

    /// EVERY calculation of the run arrives here first, including the script
    /// bar's own close calculation (reason BarClose, cause nullptr), whose
    /// default forwarding keeps on_native_bar the complete contract for a host
    /// that never opts into another cadence.
    ///
    /// reason OrderFill: one recalculation at an applied execution's cursor,
    /// driven from the applied-notification drain after that event's
    /// on_native_applied and bounded by
    /// NativeRunSpec::max_recalculations_per_point. `cause` is that event and
    /// is valid only for this call. reason Tick: one recalculation at a
    /// modeled path point or an observed print, with a null cause.
    ///
    /// `bar` is the bar the calculation is about: the script bar under
    /// delivery in batch, the print's value bar for a stream Tick. It is the
    /// COMPLETE script bar even mid-path; current_partial_bar() is the
    /// lookahead-free bar so far at this cursor. Commands and
    /// execute_current are legal here exactly as in on_native_applied.
    virtual void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                                       NativeCalculationReason reason,
                                       const native_order::ExecutionAppliedEvent* cause) {
        (void)reason;
        (void)cause;
        on_native_bar(bar, ctx);
    }

    /// One completed lower-timeframe sub-bar of an IntrabarPath::lower_tf
    /// path, delivered after that sub-bar's whole matching path and before the
    /// next sub-bar's. Never called for a run without a retained lower feed:
    /// a synthesized path and a plain confirmed bar have no sub-bars of their
    /// own. The decision point is the sub-bar's last modeled point, so
    /// commands and execute_current are legal and a request born here follows
    /// the ordinary birth rule.
    virtual void on_native_sub_bar(const Bar& sub, const NativeDecisionContext& ctx) {
        (void)sub;
        (void)ctx;
    }

    /// The calculate-on-fill hook: offered once per applied execution, FIFO, after the
    /// account record and the group/owner/dependency drains. Commands are legal here,
    /// and a request born here is eligible on the remaining path suffix of a
    /// continuous segment. Event values stay valid for the call. Throwing latches
    /// CallbackException. C spelling: pf_native_callbacks_v1::on_applied.
    virtual void on_native_applied(const native_order::ExecutionAppliedEvent&,
                                   const NativeDecisionContext&) {}

    /// The fill-terms hook, consulted at every matching candidate. Return a resolved
    /// price, and units for an unresolved HostSized request; the default is the
    /// identity price with no units, which is what every bare host wants. Answering no
    /// units for a HostSized candidate is MatchRejectReason::TermsUnresolved. It does
    /// not supply a second matcher, book or cash path. C spelling: the units half
    /// only, pf_native_callbacks_v1::on_close_units.
    virtual native_order::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const {
        return {facts.default_resolved_price, std::nullopt,
                native_order::OpeningShape::Transact};
    }
    /// The last gate before a physical effect, offered once per Applied-ready attempt
    /// and never during inspect_current_execution. Proceed takes the kernel's own
    /// path, Refuse records a nonfinancial HostPrecommit rejection, and
    /// AdmitWithHostMargin hands that one opening margin check to the host. The
    /// default proceeds. No C spelling: its view is a deep C++ aggregate; a C host
    /// gates an opening with PF_NATIVE_INTENT_SIZED's placement-time admission or
    /// with on_margin_requirement.
    virtual NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView&) const {
        return NativePrecommitVerdict::Admit;
    }

    /// Consulted at EVERY kernel check point, BEFORE the breach test, exactly
    /// as resolve_execution_terms is consulted before a fill is booked. The
    /// kernel still owns the mechanism -- the level solve, the check points,
    /// the kernel-originated request, its Superseded re-pricing, the receipt
    /// and on_native_margin_call; this hook only supplies the two numbers that
    /// comparison is made of, where brokers legitimately differ. nullopt keeps
    /// the kernel's own. A host may therefore raise a call the kernel would
    /// not make (a rounded requirement, a fee-adjusted equity, force_breach)
    /// or veto one it would (answer numbers that do not breach). Source-
    /// language money quirks -- TradingView's ten-significant-digit rounding,
    /// for one -- belong in this hook, never in the run spec.
    virtual std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView&) const {
        return std::nullopt;
    }
    /// Consulted at each kernel check point before anything is evaluated. A
    /// host whose broker model does not check there answers false, and the
    /// kernel does not evaluate, re-arm or withdraw at that point: the margin
    /// state is left exactly as the last admitted check point left it.
    /// Every point the run's check mode reaches is offered, including the ones
    /// where the book is flat or the live side has no maintenance fraction --
    /// withdrawing a resting liquidation is part of the check. CalculationOnly
    /// rests nothing, so it offers only the points it could act on.
    virtual bool margin_check_allowed(const NativeMarginCheckPoint&) const {
        return true;
    }
    /// The kernel's own liquidation sizing, offered to the host before the
    /// reduction rests. Returning nullopt keeps the run spec's sizing policy;
    /// a returned value is clamped into (0, held] and wins over it. It keeps
    /// the last word on units, including over a forced breach.
    virtual std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView&) const {
        return std::nullopt;
    }
    /// A kernel-issued liquidation that filled. It is delivered after the
    /// ordinary on_native_applied for the same fill, with the same cursor.
    virtual void on_native_margin_call(const native_order::MarginCallEvent&) {}

    /// The level an anchored leg (FromOwnerFill) is about to be armed at,
    /// offered to the host exactly once per materialization, before the
    /// ArmedEvent is built. Returning nullopt installs the kernel level
    /// (fill + offset after the anchor's rounding); a returned value is the
    /// level to install, still subject to the kernel's representability
    /// check, whose failure is the existing PreparationError path. The
    /// mechanism (the arm, the once-only materialization, the ArmedEvent,
    /// matching) stays the kernel's; only the policy of where the level sits
    /// is the host's, exactly as resolve_execution_terms owns the fill price.
    virtual std::optional<double> resolve_anchored_level(
            const NativeAnchoredLevelView&) const {
        return std::nullopt;
    }

    /// RULING A48 — the ONE generic per-lot excursion capability. A host that
    /// returns true here takes ownership of every open lot's favorable/adverse
    /// excursion: the consumer stops sampling excursion at matched trigger
    /// prices and the closing row takes both magnitudes from
    /// closed_lot_excursion(). Facts in, magnitudes out; nothing about the
    /// host's price model crosses the boundary in either direction.
    virtual bool owns_lot_excursions() const noexcept { return false; }
    /// The per-lot excursion a host owns, consulted for every closing row once
    /// owns_lot_excursions() answers true. Returning the declined value gives that row
    /// the kernel's own zero magnitudes, because nothing was sampled for it — the
    /// consumer stops sampling at matched trigger prices for the whole run as soon as
    /// ownership is declared. C spelling: pf_native_callbacks_v1::on_lot_excursion,
    /// where installing the hook IS declaring ownership.
    virtual ClosedLotExcursion closed_lot_excursion(
            const ClosedLotExcursionFacts&) const {
        return {};
    }

    /// The bar so far at the current cursor, folded from the modeled points
    /// this script bar has already presented: open of its first point,
    /// running high/low, close at the cursor. Volume is the activity actually
    /// consumed so far — the completed lower-timeframe sub-bars of an
    /// intrabar path, or the prints of an observed stream — and stays 0 for a
    /// modeled path with no intrabar volume of its own. Valid in the bar-open,
    /// applied, tick, sub-bar and recalculation callbacks; nullopt outside a
    /// path walk, including in the bar's own close calculation, where the host
    /// already holds the complete bar.
    std::optional<Bar> current_partial_bar() const;
    /// How many recalculations the kernel has driven this run, and how many it
    /// suppressed because a point had already spent its
    /// max_recalculations_per_point budget. Observation only.
    std::uint64_t native_recalculation_count() const;
    /// How many recalculations max_recalculations_per_point dropped at their matching
    /// point. The executions themselves were still applied and still delivered to
    /// on_native_applied; only the calculation they would have driven was skipped. C
    /// spelling: strategy_native_recalculations_v1, beside the driven count.
    std::uint64_t native_recalculations_skipped() const;

    /// The active callback's quote and calendar-derived decision context, as an owning
    /// value. nullopt outside a decision point. C spelling:
    /// pf_native_decision_v1::price / ::quote_kind, on every callback.
    std::optional<NativeCurrentPointView> current_execution_point() const;
    /// Where one live trail's own trigger has reached: activated, the running best,
    /// the current level and the ordinal it activated at. nullopt when the handle is
    /// not a live trail. C spelling: strategy_native_trail_state_v1, with
    /// PF_NATIVE_ABSENT for the empty. Pinned by tests/test_native_trail_state_l5k.cpp.
    std::optional<NativeTrailState> trail_state(
        const native_order::RequestHandle& target) const;
    /// A read-only preview of a current execution: the settlement readiness, any typed
    /// refusal or terms outcome, and the ordered closed-row P&L for an Applied-ready
    /// command. It is never an apply token — execute_current revalidates, and editing
    /// the preview cannot authorize or alter a fill. No C spelling:
    /// strategy_native_execute_current_v1 answers the same verdicts.
    NativeCurrentExecutionPreview inspect_current_execution(const NativeCurrentExecution&) const;
    /// Consume, synchronously, a request accepted or replaced in this very callback.
    /// Answers a refusal or the applied outcome; applied effects and relationship
    /// drains are visible before the call returns. Only the named target is consumed.
    /// C spelling: strategy_native_execute_current_v1.
    NativeCurrentExecutionResult execute_current(const NativeCurrentExecution&);

    /// The latest completed bucket delivered for a declared subscription, or
    /// nullopt before its first delivery / for an unknown index. Legal inside
    /// every native callback, including on_native_timeframe_bar itself. A
    /// gaps = true series answers nullopt again on every input bar it
    /// delivered nothing on.
    std::optional<Bar> native_series_bar(std::size_t subscription) const;

    /// Declare this run's higher-timeframe series from inside
    /// on_native_run_begin, for a host whose series are known only to its own
    /// begin-time registration. The list REPLACES the staged spec's
    /// `subscriptions`, and the kernel registers from the staged spec after
    /// this callback returns, so a host's own registration cannot erase the
    /// kernel's and the run's continuation identity folds what actually ran.
    /// Legal only inside on_native_run_begin: anywhere else, and for a list
    /// this run's input timeframe would refuse (the same validation
    /// configure_native applies), it stages nothing, changes nothing and
    /// answers false. Not virtual: the host calls the kernel here, never the
    /// other way round.
    ///
    /// The bool cannot tell those two refusals apart:
    /// declare_timeframe_subscriptions_result below answers the same call by
    /// name and this spelling is exactly `... .status == Applied`.
    bool declare_timeframe_subscriptions(
        std::vector<NativeTimeframeSubscription> subscriptions);
    /// The same call, answered the way configure_native answers the identical
    /// validation: Applied, or Failed with the first error field
    /// validate_native_timeframe_subscriptions found. A call made outside
    /// on_native_run_begin -- or on a host that has already failed -- judged
    /// no list at all and is NativeRunSpecError::WrongPhase at
    /// NativeRunSpecField::None. Same commands, same staging, same
    /// refusals as the bool spelling above.
    NativeSetupResult declare_timeframe_subscriptions_result(
        std::vector<NativeTimeframeSubscription> subscriptions);

    /// The same begin-time hook for NativeRunSpec::auxiliary_feed: the feed
    /// REPLACES the staged spec's own (nullopt withdraws it), and the kernel
    /// registers from the staged spec after on_native_run_begin returns. It is
    /// judged together with the series staged at that moment, so a host that
    /// names both here declares the feed first and its AuxiliaryFeed series
    /// second. Legal only inside on_native_run_begin: anywhere else, for a
    /// feed this run's input timeframe would refuse, and for one that would
    /// leave a staged AuxiliaryFeed series without its bars, it changes
    /// nothing and answers false. Not virtual.
    ///
    /// Three refusals, one bit: declare_auxiliary_feed_result below answers
    /// the same call by name, and this spelling is `... .status == Applied`.
    bool declare_auxiliary_feed(std::optional<NativeAuxiliaryFeed> feed);
    /// The same call, answered the way configure_native answers the identical
    /// validation: Applied, or Failed with the first error field the judged
    /// feed-and-series pair produced (AuxiliaryFeedNotFinerThanInput for a
    /// feed the input refuses, SubscriptionWithoutAuxiliaryFeed for a
    /// withdrawal that would strand a staged series). Outside
    /// on_native_run_begin nothing was judged: WrongPhase at
    /// NativeRunSpecField::None.
    NativeSetupResult declare_auxiliary_feed_result(std::optional<NativeAuxiliaryFeed> feed);

    /// A realtime stream's later bars of its declared auxiliary feed. They
    /// join the feed behind every bar it holds and ride on the next accepted
    /// input whose period they opened before — the routing rule a batch of the
    /// same bars applies. Legal between stream inputs on a Realtime run that
    /// declared a feed. Refused by name, changing nothing and without failing
    /// the host: bars out of order or not after the feed's last bar, a bar
    /// with invalid OHLCV, and a bar that opened inside an input period
    /// already accepted (its slice is closed; no batch could build that
    /// series). Calling it from inside a callback is the contract failure
    /// every reentrant stream input is.
    bool append_auxiliary_bars(const Bar* bars, std::size_t n);

    /// The only setup call. Copies the candidate spec, normalizes it and stages it
    /// atomically: Unconfigured or a Completed run with a larger run number becomes
    /// Ready, and a refusal is Failed with no partial apply. Calling it again while
    /// Ready is a Contract failure — use a new host to change unconsumed setup. C
    /// spelling: strategy_configure_native_v1 / strategy_configure_native_ext_v1.
    NativeSetupResult configure_native(const NativeRunSpec& spec);
    /// Stage the run's immutable FX epoch, legal only while Ready. Parallel
    /// timestamp/rate arrays of equal length, strictly increasing timestamps, finite
    /// positive rates; an empty curve clears it and restores the scalar account_fx
    /// fallback. Refused with WrongPhase once the run is Running. C spelling:
    /// strategy_configure_native_fx_curve_v1.
    NativeFxCurveSetupResult configure_native_fx_curve(const NativeFxCurve& curve);
    /// The whole run state as one owning read. The only observation of the lifecycle
    /// and of the durable failure; last_error() is presentation text beside it. C
    /// spelling: strategy_native_state_v1.
    NativeStateView native_state() const;

    /// Accept one complete request. Answers Accepted with a timeline ordinal and a
    /// RequestHandle, or Rejected with a rejection ordinal and a reason. Acceptance is
    /// not a fill: no lot and no fee moves here. Legal from a native callback in
    /// Batch/Warmup/Realtime, or between realtime inputs on the same thread. C
    /// spelling: strategy_native_submit_v1.
    native_order::SubmitResult submit(const native_order::Request& request);
    /// Replace one live request: validate first, then retire that incarnation and
    /// birth a successor with a new handle, a new priority and a predecessor link. A
    /// ReplaceRejected leaves the target live; a same-run absent, replaced or terminal
    /// handle is NotWorking, a foreign or malformed one InvalidHandle. Every outcome
    /// is an event, and no outcome moves a lot. C spelling:
    /// strategy_native_replace_v1.
    native_order::ReplaceResult replace(const native_order::RequestHandle& target,
                                        const native_order::Request& request);
    /// The deliberately narrow market-default convenience: the same acceptance path as
    /// submit, but it REFUSES a nondefault trigger, capacity, owner or group rather
    /// than dropping it. No C spelling, by design — the same request is
    /// strategy_native_submit_v1 with PF_NATIVE_TRIGGER_MARKET and a zero-filled
    /// struct.
    native_order::SubmitResult submit_market(const native_order::Request& request);
    /// The same market-default convenience for a replace; see submit_market.
    native_order::ReplaceResult replace_market(const native_order::RequestHandle& target,
                                               const native_order::Request& request);
    /// Replace with options. ReplaceOptions{retain_trigger_state = true} carries the
    /// predecessor's live trigger state — a tracking trail's best, an already active
    /// stop — into the successor instead of restarting it. Predecessor and successor
    /// must hold the same trigger alternative, and a retained best must still produce
    /// a representable level; otherwise the replacement is rejected and the
    /// predecessor stays live.
    native_order::ReplaceResult replace(const native_order::RequestHandle& target,
                                        const native_order::Request& request,
                                        native_order::ReplaceOptions options);
    /// Cancel one live request by handle. A live request becomes Cancelled; a same-run
    /// absent, replaced or already terminal handle is NotWorking; a foreign or
    /// malformed handle is InvalidHandle. Every outcome is an event. C spelling:
    /// strategy_native_cancel_v1.
    native_order::CancelResult cancel(const native_order::RequestHandle& target);
    /// Working-book snapshot and bulk cancellation. cancel_all returns how
    /// many requests left the book (one CancelledEvent each, dependants
    /// included); cancel_where cancels exactly the live requests carrying that
    /// comment and returns how many of them it cancelled.
    ///
    /// The second form chooses which identity field the text is compared
    /// against: NativeRequestField::Comment is the one-argument form, and
    /// NativeRequestField::Label addresses the requests by their label, the
    /// one-call equivalent of cancelling every order a host issued under its
    /// own id. Neither form indexes anything: both walk the live book once,
    /// like cancel_all, so a label may be reused, replaced or left empty
    /// without any bookkeeping to keep in step. Text that matches nothing is
    /// not a command.
    std::vector<NativeWorkingRequest> native_working_requests() const;
    /// Withdraw every live request, dependants of a cancelled owner included, and
    /// answer how many left the book. One CancelledEvent per request. C spelling:
    /// strategy_native_cancel_all_v1.
    std::size_t cancel_all();
    /// Withdraw exactly the live requests carrying this comment, and answer how many
    /// of those left the book. An unknown comment is not a command. The comment is
    /// free host text the kernel only copies and compares, and it is not indexed: this
    /// walks the live book once, exactly as cancel_all does.
    std::size_t cancel_where(std::string_view comment);
    /// The same predicate over either identity text. NativeRequestField::Label
    /// addresses requests by Request::label — the one call that withdraws every live
    /// request a host issued under one of its own order ids. "" is the text a request
    /// carrying no such field matches. C spelling:
    /// strategy_native_cancel_where_v1(host, text, PF_NATIVE_FIELD_LABEL).
    std::size_t cancel_where(std::string_view text, NativeRequestField field);
    /// Open a roster a later close can bind to. The handle is what a
    /// native_order::BindCohort owner names. C spelling:
    /// strategy_native_cohort_open_v1.
    native_order::CohortHandle cohort_open();
    /// Enroll one accepted opening's handle in a roster. C spelling:
    /// strategy_native_cohort_add_v1.
    void cohort_add(native_order::CohortHandle cohort, native_order::RequestHandle origin);
    /// Take one opening back off a roster. C spelling:
    /// strategy_native_cohort_remove_v1.
    void cohort_remove(native_order::CohortHandle cohort, native_order::RequestHandle origin);

    /// The book as one aggregate, copied out. C spelling:
    /// strategy_native_position_v1.
    NativePhysicalPosition physical_position() const;
    /// The book lot by lot, oldest first, marked at `mark`: one NativeOpenLot
    /// per physical lot (physical_position().lot_count rows), copied at query
    /// time. Legal wherever physical_position() is; observation only, it
    /// moves no fill, no hash and no row.
    std::vector<NativeOpenLot> native_open_lots(double mark) const;
    /// The account's equity marked at this price: the realized balance plus every open
    /// lot's own fee-net term, which is exactly what native_open_lots(mark) sums. It
    /// moves nothing. C spelling: strategy_native_marked_equity_v1.
    double native_marked_equity(double mark) const;
    /// The units a kernel-sized intent resolves to under this run's spec at a
    /// sizing price, a marked equity and an account FX rate -- as a pure query.
    /// units = cash / (price * point_value * fx), cash the basis value or
    /// fraction * equity, net of the percent fee reserve when the intent asks
    /// for it, then the intent's grid policy: this is the same function the
    /// kernel runs at acceptance (SizeTime::AtAcceptance) and at the candidate
    /// (AtMatch), so a host that gates a command on its quantity before it
    /// submits reads the number here rather than keeping its own copy of the
    /// conversion. nullopt when the run is not configured or the basis is
    /// unresolvable at those inputs (non-positive money or denominator, a
    /// below-one-step quotient under SnapToGrid). Observation only: it moves
    /// nothing and freezes nothing.
    std::optional<double> native_sized_units(const native_order::Sized& sized, double price,
                                             double equity, double fx) const;
    /// The price at which the marked equity falls below the run's maintenance
    /// requirement for the live position's side. nullopt when the run declares
    /// no margin model, the side has no maintenance fraction, the book is flat,
    /// or no finite price solves the breach (a long at full maintenance).
    std::optional<double> native_liquidation_price() const;
    /// The run's generic risk ledger. Every field is its zero for a run that
    /// declares no NativeRunSpec::risk; observation only, it moves nothing.
    NativeRiskState native_risk_state() const;
    /// Owning snapshots copied at query time. Later commands/reset do not
    /// invalidate already returned values.
    std::vector<NativeMarketEvent> native_events(uint64_t after_ordinal) const;
    /// The run's monotonic decision floor in epoch milliseconds — the same value
    /// NativeStateView::decision_floor_ms carries. Every request's birth is compared
    /// against this floor, not against a later lowered clock, and a refused preflight
    /// does not raise it. C spelling: pf_native_state_v1::decision_floor_ms.
    int64_t native_decision_floor() const;
    /// The highest run_number this host has consumed. It lives OUTSIDE per-run reset,
    /// so a later run on the same host needs a strictly larger number; a fresh host
    /// reads 0 and may therefore replay the same logical run. C spelling:
    /// pf_native_state_v1::consumed_high_water.
    uint64_t native_consumed_high_water() const;
    /// The consumer's continuation identity: what a stream resumes against. It folds
    /// the run's resolved timezone identity, whose zone file paths are absolute paths
    /// on the machine that ran it, so the same spec over the same bars hashes
    /// differently on two hosts even for "UTC". Compare it between runs in ONE
    /// process; never pin it as a constant. For a portable constant use
    /// native_run_spec_digest(spec). C spelling:
    /// strategy_native_continuation_hash_v1.
    uint64_t native_continuation_hash() const;

    friend class NativeExecutionConsumer;
};

}  // inline namespace engine_script_run_v18
}  // namespace pineforge
