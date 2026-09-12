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
inline namespace engine_script_run_v12 {

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

struct NativeFailure {
    NativeFailureCode code = NativeFailureCode::None;
    NativeFailureOperation operation = NativeFailureOperation::None;
    std::uint64_t ordinal = 0;       // 0 = absent
    std::uint32_t discriminator = 0;
};

struct NativeUnconfigured {};
struct NativeReady { NativeRunSpec spec; };
struct NativeRunning { NativeRunSpec spec; NativeRunPhase phase = NativeRunPhase::Batch; };
struct NativeCompleted { NativeRunSpec spec; NativeCompletion completion = NativeCompletion::BatchComplete; };
struct NativeFailed { std::optional<NativeRunSpec> spec; NativeFailure failure; };

using NativeLifecycle = std::variant<NativeUnconfigured, NativeReady, NativeRunning,
                                     NativeCompleted, NativeFailed>;

static_assert(std::is_trivially_copyable_v<NativeFailure>);
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
#define PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V12 1
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

}  // inline namespace engine_script_run_v12
}  // namespace pineforge
