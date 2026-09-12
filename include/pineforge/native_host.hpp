#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/market_driver.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v13 {

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

// Most-derived native strategy host. Binds NativeExecutionConsumer in the
// protected engine constructor. Noncopyable and nonmovable. Lives in the
// same inline engine epoch as BacktestEngine so old-header/new-library
// linkage cannot resolve an unversioned constructor against a different
// base layout.
#define PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V13 1
class NativeStrategyHost : public BacktestEngine {
public:
    NativeStrategyHost();
    NativeStrategyHost(const NativeStrategyHost&) = delete;
    NativeStrategyHost& operator=(const NativeStrategyHost&) = delete;
    NativeStrategyHost(NativeStrategyHost&&) = delete;
    NativeStrategyHost& operator=(NativeStrategyHost&&) = delete;
    ~NativeStrategyHost() override;

    void on_bar(const Bar& bar) final;

    virtual void on_native_run_begin() {}
    virtual void on_native_bar(const Bar& bar, const NativeDecisionContext& context) = 0;

    NativeSetupResult configure_native(const NativeRunSpec& spec);
    NativeStateView native_state() const;

    native_order::SubmitResult submit(const native_order::Request& request);
    native_order::ReplaceResult replace(const native_order::RequestHandle& target,
                                        const native_order::Request& request);
    native_order::SubmitResult submit_market(const native_order::Request& request);
    native_order::ReplaceResult replace_market(const native_order::RequestHandle& target,
                                               const native_order::Request& request);
    native_order::CancelResult cancel(const native_order::RequestHandle& target);

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

}  // inline namespace engine_script_run_v13
}  // namespace pineforge
