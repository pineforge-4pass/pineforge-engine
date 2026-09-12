#pragma once

#include <pineforge/bar.hpp>
#include <pineforge/native_calendar.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace pineforge {

// Semantic versions hashed into native continuation identity.
inline constexpr const char* kNativeDriverSemanticVersion = "native-driver/v1";
inline constexpr const char* kNativeConsumerSemanticVersion = "native-consumer/v1";
inline constexpr const char* kNativeCalendarSemanticVersion = "native-calendar/v1";

enum class NativePriceProvenance : std::uint8_t {
    Confirmed = 0,
    ObservedPrint = 1,
    ModeledOHLCOpen = 2,
    ModeledOHLCClose = 3,
    CarriedOpen = 4,
    AfterCalculationClose = 5,
    PartialFinalized = 6,
    Calculation = 7,
};

enum class NativePathPhase : std::uint8_t {
    None = 0,
    Open = 1,
    High = 2,
    Low = 3,
    Close = 4,
};

enum class NativeCompletionKind : std::uint8_t {
    Confirmed = 0,
    LazyComplete = 1,
    SessionShortened = 2,
    PartialFinalized = 3,
};

struct NativeCoordinate {
    uint64_t ordinal = 0;
    int interval_index = 0;
    int64_t open_ms = 0;
    int64_t last_traded_close_ms = 0;
    int64_t next_period_open_ms = 0;
    int64_t next_input_open_ms = 0;
    int64_t effective_time_ms = 0;
    int64_t source_price_time_ms = 0;
    NativePriceProvenance provenance = NativePriceProvenance::Confirmed;
    NativePathPhase path_phase = NativePathPhase::None;
    NativeCompletionKind completion = NativeCompletionKind::Confirmed;
};

struct NativeDriverPoint {
    NativeCoordinate coordinate;
    double raw_price = 0.0;
    std::optional<uint64_t> sequence;
    bool matching = false;
    bool excursion = false;
};

// Presentation snapshot copied onto the callback stack. Mutating these
// fields cannot change the consumer's decision floor, matching time, or
// after-calculation coordinate.
struct NativeDecisionContext {
    NativeCoordinate coordinate;
    int64_t decision_floor_ms = 0;
    native_calendar::NativeInterval input_interval{};
    native_calendar::NativeInterval script_interval{};
};

// Pump-produced events obtain ordinals from the consumer allocator.
class INativeDriverSink {
public:
    virtual ~INativeDriverSink() = default;
    virtual uint64_t allocate_native_ordinal() = 0;
    virtual void on_native_matching_point(const NativeDriverPoint& point) = 0;
    virtual void on_native_excursion(const NativeDriverPoint& point) = 0;
    virtual void on_native_calculation(const Bar& script_bar, const NativeCoordinate& coordinate) = 0;
};

bool native_bar_structurally_valid(const Bar& bar) noexcept;

}  // namespace pineforge
