#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/market_driver.hpp>
#include <pineforge/native_fx_curve.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v18 {

enum class NativeLifecycleKind : std::uint8_t {
    Unconfigured = 0,
    Ready = 1,
    Running = 2,
    Completed = 3,
    Failed = 4,
};

enum class NativeRunPhase : std::uint8_t {
    Batch = 0,
    Warmup = 1,
    Realtime = 2,
};

enum class NativeCompletion : std::uint8_t {
    BatchComplete = 0,
    StreamEnded = 1,
};

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

// In-run failure references. Identifiers belong to NativeFailed.spec->identity;
// they never carry a second RunIdentity string. Kind is a bit-union of the
// typed alternatives below (Cause=1, Recipient=2, Cursor=4).
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

struct NativeInRunCause {
    std::uint64_t ordinal = 0;  // 0 = absent
};

struct NativeInRunRecipient {
    std::uint64_t incarnation = 0;  // 0 = absent
};

struct NativeInRunCursor {
    NativeCoordinate point{};
    double t = 0.0;
};

struct NativeFailureContext {
    NativeFailureContextKind kind = NativeFailureContextKind::None;
    NativeInRunCause cause{};
    NativeInRunRecipient recipient{};
    NativeInRunCursor cursor{};
};

constexpr std::uint8_t native_failure_context_bits(NativeFailureContextKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}
constexpr bool native_failure_has_cause(NativeFailureContextKind kind) noexcept {
    return (native_failure_context_bits(kind) & 1u) != 0;
}
constexpr bool native_failure_has_recipient(NativeFailureContextKind kind) noexcept {
    return (native_failure_context_bits(kind) & 2u) != 0;
}
constexpr bool native_failure_has_cursor(NativeFailureContextKind kind) noexcept {
    return (native_failure_context_bits(kind) & 4u) != 0;
}
constexpr bool native_failure_has_cause(const NativeFailureContext& context) noexcept {
    return native_failure_has_cause(context.kind);
}
constexpr bool native_failure_has_recipient(const NativeFailureContext& context) noexcept {
    return native_failure_has_recipient(context.kind);
}
constexpr bool native_failure_has_cursor(const NativeFailureContext& context) noexcept {
    return native_failure_has_cursor(context.kind);
}

constexpr NativeFailureContextKind native_failure_context_kind(
        bool cause, bool recipient, bool cursor) noexcept {
    return static_cast<NativeFailureContextKind>(
            (cause ? 1u : 0u) | (recipient ? 2u : 0u) | (cursor ? 4u : 0u));
}

inline NativeFailureContext native_failure_cause(std::uint64_t ordinal) noexcept {
    NativeFailureContext context;
    if (ordinal == 0) return context;
    context.kind = NativeFailureContextKind::Cause;
    context.cause.ordinal = ordinal;
    return context;
}
inline NativeFailureContext native_failure_recipient(std::uint64_t incarnation) noexcept {
    NativeFailureContext context;
    if (incarnation == 0) return context;
    context.kind = NativeFailureContextKind::Recipient;
    context.recipient.incarnation = incarnation;
    return context;
}
inline NativeFailureContext native_failure_cursor(NativeCoordinate point, double t = 0.0) noexcept {
    NativeFailureContext context;
    context.kind = NativeFailureContextKind::Cursor;
    context.cursor.point = point;
    context.cursor.t = t;
    return context;
}

// Copies only ordinals/incarnation/cursor scalars. Foreign run identities are
// dropped rather than stored under the failed spec's identity.
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

struct NativeFailure {
    NativeFailureCode code = NativeFailureCode::None;
    NativeFailureOperation operation = NativeFailureOperation::None;
    std::uint64_t ordinal = 0;       // 0 = absent
    std::uint32_t discriminator = 0;
    NativeFailureContext context{};
};

struct NativeUnconfigured {};
struct NativeReady { NativeRunSpec spec; };
struct NativeRunning { NativeRunSpec spec; NativeRunPhase phase = NativeRunPhase::Batch; };
struct NativeCompleted { NativeRunSpec spec; NativeCompletion completion = NativeCompletion::BatchComplete; };
struct NativeFailed { std::optional<NativeRunSpec> spec; NativeFailure failure; };

using NativeLifecycle = std::variant<NativeUnconfigured, NativeReady, NativeRunning,
                                     NativeCompleted, NativeFailed>;

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

struct NativeStateView {
    NativeLifecycleKind kind = NativeLifecycleKind::Unconfigured;
    const NativeRunSpec* spec = nullptr;
    NativeRunPhase phase = NativeRunPhase::Batch;
    NativeCompletion completion = NativeCompletion::BatchComplete;
    NativeFailure failure{};
    uint64_t consumed_high_water = 0;
    int64_t decision_floor_ms = 0;
};

struct NativePhysicalPosition {
    double signed_units = 0.0;
    double average_price = 0.0;
    std::size_t lot_count = 0;
};

struct NativeAccountObservation {
    uint64_t ordinal = 0;
    int64_t effective_time_ms = 0;
    double marked_equity = 0.0;
    double realized_balance = 0.0;
    double signed_units = 0.0;
};

enum class NativeEventKind : std::uint8_t {
    Command = 0,
    Driver = 1,
    Account = 2,
};

struct NativeMarketEvent {
    NativeEventKind kind = NativeEventKind::Command;
    uint64_t ordinal = 0;
    std::optional<native_order::CommandEvent> command;
    std::optional<NativeDriverPoint> driver;
    std::optional<NativeAccountObservation> account;
};

enum class NativeSetupStatus : std::uint8_t { Applied = 0, Failed = 1 };

struct NativeSetupResult {
    NativeSetupStatus status = NativeSetupStatus::Failed;
    NativeRunSpecValidation validation{};
};

struct NativeFxCurveSetupResult {
    NativeSetupStatus status = NativeSetupStatus::Failed;
    NativeFxCurveValidation validation{};
};

enum class NativeCurrentPriceRule : std::uint8_t { AsPresented = 0, NearestTick = 1 };
enum class NativeCurrentQuoteKind : std::uint8_t { MarketDecision = 0, ExecutionAnchor = 1 };

// Read-only owning-value facts for one candidate. The host is already the
// engine, so no engine handle or prepared execution token is exposed here.
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
    // A retained crossing can share a rounded cursor quote with another
    // request's level. The receipt keeps this distinction durable.
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

// Ephemeral factual view of one prepared execution before any physical effect.
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

// The host is consulted before generic opening-margin admission.  Admit keeps
// the native default gate; AdmitWithHostMargin lets a host that owns the
// source-compatible margin rule take responsibility for that one check.
// Proceed remains an alias for the v7 spelling used by existing C++ callers.
enum class NativePrecommitVerdict : std::uint8_t {
    Admit = 0,
    Proceed = Admit,
    Refuse = 1,
    AdmitWithHostMargin = 2,
};

struct NativeCurrentPointView {
    NativeDecisionContext decision;
    double price = 0.0;
    NativeCurrentQuoteKind quote_kind = NativeCurrentQuoteKind::MarketDecision;
    std::uint64_t quote_origin_ordinal = 0;
};

// Read-only projection of a live generic Trail request. Before its arm is
// reached, activated is false and the numeric/ordinal fields are zero. Once
// armed, best_price and current_level are the exact raw matcher values and
// activation_ordinal identifies the TrailArm event that began tracking.
struct NativeTrailState {
    bool activated = false;
    double best_price = 0.0;
    double current_level = 0.0;
    std::uint64_t activation_ordinal = 0;
};

// Owning value row for one live request, copied at query time. The
// definition is the accepted (and, for an anchored leg, already materialized)
// request; remaining is what is left to execute; trigger_state is where the
// request's own trigger has reached. Later commands do not invalidate a row
// that was already returned.
struct NativeWorkingRequest {
    native_order::DefinitionRef definition;
    native_order::RemainingProjection remaining = native_order::RemainingProjectionUnbound{};
    native_order::TriggerState trigger_state = native_order::MarketReady{};
};

enum class NativeCurrentRefusal : std::uint8_t {
    NoExecutionContext = 0, Reentrant = 1, InvalidHandle = 2, NotWorking = 3,
    NotAcceptedInCallback = 4, UnsupportedRequest = 5, UnreadyOwner = 6,
    InvalidSelection = 7, ConfigurationMismatch = 8,
};

struct NativeCurrentExecution {
    native_order::RequestHandle target;
    NativeCurrentPriceRule price_rule = NativeCurrentPriceRule::AsPresented;
};

// Recomputed observations, never an apply token. Readiness is the financial
// pre-source preparation boundary, independent of account projection validity
// and excluding opening admission and late counter/lifecycle checks.
struct NativeCurrentExecutionPreview {
    std::optional<NativeCurrentRefusal> refusal;
    std::optional<execution::Status> settlement_readiness;
    execution::AccountEffectProjection account;
    std::vector<double> closed_row_pnl;
    std::optional<native_order::MatchRejectReason> terms_rejection;
    std::optional<native_order::CancelReason> terms_cancellation;
};

using NativeCurrentExecutionResult = std::variant<NativeCurrentRefusal,
    native_order::ExecutionAppliedEvent, native_order::NoEffectEvent,
    native_order::MatchRejectedEvent, native_order::CancelledEvent>;

// Borrowed begin-call facts. The bar/input/syminfo/override pointers expire when
// prepare_native_begin returns; retained configuration must copy them by
// value (for example into NativeRunSpec::intrabar).
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
    // The rich run overload's symbol metadata is borrowed only for this
    // callback.  A provider that uses it must copy the fields it needs into
    // its retained NativeRunSpec/staged metadata before returning.
    const SymInfo* syminfo = nullptr;
    const void* overrides_opaque = nullptr;
    bool is_stream = false;
    int warmup_n = 0;
    // Which public overload began the run: the bare run(bars, n) lifecycle
    // (true) or a timeframe-aware / magnified / stream begin (false).  A host
    // may keep lifecycle surfaces (for example its higher-timeframe series
    // evaluators) off for the bare overload; empty timeframes alone do not
    // identify it, they only request auto-detection.
    bool simple_run = false;
};

// Accepted input facts presented before the generic consumer aggregates the
// bar into its script interval or evaluates any matching point. This is not a
// source-language callback: native hosts may observe raw input cadence through
// it without taking ownership of matching or aggregation.
struct NativeInputContext {
    native_calendar::NativeInterval input_interval{};
    native_calendar::NativeInterval script_interval{};
    int input_index = 0;
    bool completes_script_interval = false;
};

// One accepted realtime print before native matching at its current decision
// point. The Bar is a value presentation of that print (O=H=L=C=price,
// volume=print quantity, timestamp=print timestamp); no source-language
// policy is embedded here. Sequence zero retains the public TradeTick
// sentinel meaning “provider did not supply a sequence”.
struct NativeTickContext {
    NativeDecisionContext decision{};
    std::uint64_t sequence = 0;
};

// One completed higher-timeframe bucket of a declared subscription
// (NativeRunSpec::subscriptions), presented before the calculation of the
// input bar it is delivered on.
//
// `subscription` indexes NativeRunSpec::subscriptions. `interval` is the
// calendar span of the bucket's FIRST contributing input bar, read through the
// run's own session calendar; it is left zeroed when that lookup has no answer.
// `completion` is Confirmed when the bucket completed on its own last
// contributing input bar and LazyComplete when the next period's first input
// closed it. `delivered_at_ms` is the timestamp of the input bar the delivery
// rides on: the bucket's last contributing bar under lookahead_off and its
// first under lookahead_on.
struct NativeTimeframeBarContext {
    std::size_t subscription = 0;
    native_calendar::NativeInterval interval{};
    NativeCompletionKind completion = NativeCompletionKind::Confirmed;
    std::int64_t delivered_at_ms = 0;
};

// Most-derived native strategy host. Binds NativeExecutionConsumer in the
// protected engine constructor. Noncopyable and nonmovable. Lives in the
// same inline engine epoch as BacktestEngine so old-header/new-library
// linkage cannot resolve an unversioned constructor against a different
// base layout.
#define PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18 1
class NativeStrategyHost : public BacktestEngine {
public:
    NativeStrategyHost();
    NativeStrategyHost(const NativeStrategyHost&) = delete;
    NativeStrategyHost& operator=(const NativeStrategyHost&) = delete;
    NativeStrategyHost(NativeStrategyHost&&) = delete;
    NativeStrategyHost& operator=(NativeStrategyHost&&) = delete;
    ~NativeStrategyHost() override;

    void on_bar(const Bar& bar) final;

    virtual void prepare_native_begin(const NativeBeginArgs&) {}
    virtual void on_native_run_begin() {}
    // Called once for every accepted confirmed input bar, before that bar is
    // aggregated or matched. It has no current execution point.
    virtual void on_native_input(const Bar&, const NativeInputContext&) {}
    // Called once for every accepted realtime print, before matching at that
    // point. inspect_current_execution/execute_current are legal here.
    virtual void on_native_tick(const Bar&, const NativeTickContext&) {}
    // One completed bucket of a declared higher-timeframe subscription,
    // delivered on an accepted input bar before that input is aggregated,
    // matched or calculated. Never called for a spec whose `subscriptions`
    // are empty. native_series_bar() already answers with this bar here.
    virtual void on_native_timeframe_bar(const Bar&, const NativeTimeframeBarContext&) {}
    // Precedes the matching pass at the script bar's open decision point.
    // inspect_current_execution/execute_current are legal in this hook.
    virtual void on_native_bar_open(const Bar&, const NativeDecisionContext&) {}
    // The current decision point remains valid for the complete callback.
    // A host may therefore execute a command after its own script-body work
    // returns, before the consumer advances beyond this calculation point.
    virtual void on_native_bar(const Bar& bar, const NativeDecisionContext& context) = 0;

    virtual void on_native_applied(const native_order::ExecutionAppliedEvent&,
                                   const NativeDecisionContext&) {}

    virtual native_order::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const {
        return {facts.default_resolved_price, std::nullopt,
                native_order::OpeningShape::Transact};
    }
    virtual NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView&) const {
        return NativePrecommitVerdict::Admit;
    }

    // RULING A48 — the ONE generic per-lot excursion capability. A host that
    // returns true here takes ownership of every open lot's favorable/adverse
    // excursion: the consumer stops sampling excursion at matched trigger
    // prices and the closing row takes both magnitudes from
    // closed_lot_excursion(). Facts in, magnitudes out; nothing about the
    // host's price model crosses the boundary in either direction.
    virtual bool owns_lot_excursions() const noexcept { return false; }
    virtual ClosedLotExcursion closed_lot_excursion(
            const ClosedLotExcursionFacts&) const {
        return {};
    }

    std::optional<NativeCurrentPointView> current_execution_point() const;
    std::optional<NativeTrailState> trail_state(
        const native_order::RequestHandle& target) const;
    NativeCurrentExecutionPreview inspect_current_execution(const NativeCurrentExecution&) const;
    NativeCurrentExecutionResult execute_current(const NativeCurrentExecution&);

    // The latest completed bucket delivered for a declared subscription, or
    // nullopt before its first delivery / for an unknown index. Legal inside
    // every native callback, including on_native_timeframe_bar itself.
    std::optional<Bar> native_series_bar(std::size_t subscription) const;

    NativeSetupResult configure_native(const NativeRunSpec& spec);
    NativeFxCurveSetupResult configure_native_fx_curve(const NativeFxCurve& curve);
    NativeStateView native_state() const;

    native_order::SubmitResult submit(const native_order::Request& request);
    native_order::ReplaceResult replace(const native_order::RequestHandle& target,
                                        const native_order::Request& request);
    native_order::SubmitResult submit_market(const native_order::Request& request);
    native_order::ReplaceResult replace_market(const native_order::RequestHandle& target,
                                               const native_order::Request& request);
    native_order::ReplaceResult replace(const native_order::RequestHandle& target,
                                        const native_order::Request& request,
                                        native_order::ReplaceOptions options);
    native_order::CancelResult cancel(const native_order::RequestHandle& target);
    // Working-book snapshot and bulk cancellation. cancel_all returns how
    // many requests left the book (one CancelledEvent each, dependants
    // included); cancel_where cancels exactly the live requests carrying that
    // comment and returns how many of them it cancelled.
    std::vector<NativeWorkingRequest> native_working_requests() const;
    std::size_t cancel_all();
    std::size_t cancel_where(std::string_view comment);
    native_order::CohortHandle cohort_open();
    void cohort_add(native_order::CohortHandle cohort, native_order::RequestHandle origin);
    void cohort_remove(native_order::CohortHandle cohort, native_order::RequestHandle origin);

    NativePhysicalPosition physical_position() const;
    double native_marked_equity(double mark) const;
    // Owning snapshots copied at query time. Later commands/reset do not
    // invalidate already returned values.
    std::vector<NativeMarketEvent> native_events(uint64_t after_ordinal) const;
    int64_t native_decision_floor() const;
    uint64_t native_consumed_high_water() const;
    uint64_t native_continuation_hash() const;

    friend class NativeExecutionConsumer;
};

}  // inline namespace engine_script_run_v18
}  // namespace pineforge
