#pragma once

#include <pineforge/bar.hpp>
#include <pineforge/magnifier.hpp>
#include <pineforge/native_order_identity.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace native_run_spec_v2 {

// Encodings coincide with the versioned native-v1 C transport. These values
// describe native execution; they do not configure source strategy policies.
enum class NativeFeeKind : std::uint32_t {
    Percent = 0,
    CashPerUnit = 1,
    CashPerExecution = 2,
};

enum class NativeCloseExecution : std::uint32_t {
    NextEligiblePoint = 0,
    AfterCalculation = 1,
};

// Abort presentation is a run-level policy rather than an exception-path
// convention. Generic hosts retain an error diagnostic by default; a host
// that models cooperative cancellation can opt into a quiet status result.
enum class NativeAbortReporting : std::uint32_t {
    Error = 0,
    Quiet = 1,
};

enum class NativeOpenDirections : std::uint32_t {
    None = 0,
    Long = 1,
    Short = 2,
    Both = 3,
};

// Native hosts normally require every confirmed bar to name a canonical input
// slot.  A host that deliberately reproduces a legacy batch route can retain
// the caller's strictly-increasing timestamps as its decision labels instead.
// This remains a run-spec value so the two modes never share a continuation.
enum class NativeSlotLabelPolicy : std::uint32_t {
    Canonical = 0,
    LegacyTolerant = 1,
};

// Generic ordering for a modeled OHLC path. Auto retains the open-proximity
// rule; the forced modes make the first excursion explicit for replay/live
// hosts without relying on process-global or source-language state.
enum class NativePathOrder : std::uint32_t {
    Auto = 0,
    HighFirst = 1,
    LowFirst = 2,
};

// Explicit, opt-in compatibility exceptions for legacy batch input shape.
// They are separate from slot labels because a host may need legacy price/
// unavailable-volume admission while retaining canonical calendar labels.
enum class NativeLegacyTolerance : std::uint32_t {
    None = 0,
    // Match engine_run.cpp's legacy batch structural check: finite OHLC values
    // need not be positive, and NaN volume means unavailable activity.
    BatchStructuralBars = 1u << 0,
    // Source-compatible stream warmups admit finite, non-negative interim
    // OHLC values.  The final warmup close remains strictly positive.
    WarmupNonNegativeOHLC = 1u << 1,
};

constexpr bool native_legacy_tolerance_enabled(
        NativeLegacyTolerance enabled, NativeLegacyTolerance requested) noexcept {
    return (static_cast<std::uint32_t>(enabled)
            & static_cast<std::uint32_t>(requested)) != 0u;
}

// An owned lower-timeframe execution path.  It is deliberately a run-spec
// value rather than a caller borrow: public begin arguments expire when the
// begin call returns, whereas native matching may need the lower bars later
// while sealing an aggregated script bar.
struct IntrabarPath {
    struct none {};
    enum class SampleEligibility : std::uint32_t {
        // Native hosts retain continuous matching between generated samples
        // unless they explicitly request point-only sample eligibility.
        ContinuousSegments = 0,
        DistributionSamples = 1,
    };
    struct lower_tf {
        std::vector<Bar> bars;
        std::string tf;
        int samples = 4;
        MagnifierDistribution distribution = MagnifierDistribution::ENDPOINTS;
        bool volume_weighted = false;
        int volume_weighted_min_samples = 2;
        int volume_weighted_max_samples = 64;
        SampleEligibility sample_eligibility = SampleEligibility::ContinuousSegments;
    };
    // A synthesized path has no retained lower feed. The driver samples each
    // script bar's own OHLC path through the generic sampler declared in
    // include/pineforge/magnifier.hpp. Its point-only eligibility is inherent
    // to this mode, so there is no separate SampleEligibility member.
    struct synthesized {
        int samples = 4;
        MagnifierDistribution distribution = MagnifierDistribution::ENDPOINTS;
        bool volume_weighted = false;
        int volume_weighted_min_samples = 2;
        int volume_weighted_max_samples = 64;
    };
    using value_type = std::variant<none, lower_tf, synthesized>;

    value_type value = none{};

    bool is_none() const noexcept { return std::holds_alternative<none>(value); }
    const lower_tf* lower() const noexcept { return std::get_if<lower_tf>(&value); }
    lower_tf* lower() noexcept { return std::get_if<lower_tf>(&value); }
    const synthesized* synthesized_path() const noexcept {
        return std::get_if<synthesized>(&value);
    }
    synthesized* synthesized_path() noexcept { return std::get_if<synthesized>(&value); }
};

// One complete setup value, staged/copied by NativeStrategyHost before it is
// applied at begin. This aggregate owns no host phase, consumed-run counter,
// parsed-calendar authority, physical account, or C transport presence mask.
// Empty required strings and zero capital/value/FX defaults make an incomplete
// value invalid; price_tick == 0 explicitly selects unquantized prices. No
// timezone/timeframe/instrument facts are inferred.
struct NativeRunSpec {
    native_order::RunIdentity identity;
    std::string input_tf;
    std::string script_tf;
    // A public begin with fewer than two bars may not establish a timeframe.
    // This preserves that explicit state without inventing a clock literal.
    bool timeframe_undetected = false;
    // Strict native hosts retain the canonical slot-label rule. A legacy
    // source provider may opt into raw, strictly-increasing caller labels.
    NativeSlotLabelPolicy slot_label_policy = NativeSlotLabelPolicy::Canonical;
    NativeLegacyTolerance legacy_tolerance = NativeLegacyTolerance::None;
    NativePathOrder path_order = NativePathOrder::Auto;

    std::string ticker;
    std::string tickerid;
    std::string type;
    std::string currency;
    std::string basecurrency;
    std::string description;
    std::string volumetype;

    std::string timezone;
    std::string session;        // Empty and "24x7" are distinct all-day literals.
    std::string chart_timezone; // Optional observation metadata; empty stays empty.

    double initial_capital = 0.0;
    double point_value = 0.0;
    double account_fx = 0.0;    // One positive scalar, not a timestamped FX series.
    double price_tick = 0.0;       // Finite, nonnegative; zero means unquantized prices.
    std::uint32_t slippage_ticks = 0; // <= INT_MAX; raw +/- ticks*tick, no snap.
    NativeFeeKind fee_kind = NativeFeeKind::Percent;
    double fee_value = 0.0;     // Percent/100 of absolute account notional, or
                               // account-currency cash per unit/execution.
    std::optional<double> quantity_grid; // Positive; admission only, no resize.
    NativeCloseExecution close_execution = NativeCloseExecution::NextEligiblePoint;
    NativeAbortReporting abort_reporting = NativeAbortReporting::Error;
    std::optional<double> max_abs_units; // Positive resulting-book opening cap.
    std::optional<std::uint64_t> max_open_lots; // Positive surviving+new lot cap.
    NativeOpenDirections allowed_open_directions = NativeOpenDirections::Both;
    std::optional<double> initial_margin_fraction; // Positive fraction, not percent;
                                                 // no maintenance liquidation.
    IntrabarPath intrabar{};
};

enum class NativeRunSpecField : std::uint8_t {
    None,
    SessionKey, RunNumber, InputTimeframe, ScriptTimeframe,
    Ticker, TickerId, Type, Currency, BaseCurrency, Description, VolumeType,
    Timezone, Session, ChartTimezone,
    InitialCapital, PointValue, AccountFx, PriceTick, SlippageTicks,
    FeeKind, FeeValue, QuantityGrid, CloseExecution, AbortReporting, MaxAbsUnits, MaxOpenLots,
    AllowedOpenDirections, InitialMarginFraction,
    IntrabarTimeframe, IntrabarSamples, IntrabarDistribution, IntrabarVolumeSamples,
    IntrabarSampleEligibility,
    TimeframeUndetected,
    SlotLabelPolicy, LegacyTolerance,
    PathOrder,
};

enum class NativeRunSpecError : std::uint8_t {
    None,
    EmptyRequiredString,
    EmbeddedNul,
    InvalidUtf8,
    ZeroRunNumber,
    InvalidTimeframe,
    IncompatibleTimeframes,
    UnresolvedTimezone,
    InvalidSession,
    NotFinitePositive,
    SlippageOutOfRange,
    UnknownFeeKind,
    NotFiniteNonnegative,
    UnknownCloseExecution,
    UnknownAbortReporting,
    UnknownOpenDirections,
    ZeroLotLimit,
    AllocationFailure,
    CalendarFailure,
    InvalidIntrabarPath,
    UnknownIntrabarSampleEligibility,
    InvalidUndetectedTimeframe,
    UnknownSlotLabelPolicy,
    UnknownLegacyTolerance,
    UnknownPathOrder,
};

// Allocation-free facts suitable for the host's durable failure variant.
// Render text at the presentation boundary, never store exception.what().
struct NativeRunSpecValidation {
    NativeRunSpecError error = NativeRunSpecError::None;
    NativeRunSpecField field = NativeRunSpecField::None;

    constexpr bool ok() const noexcept { return error == NativeRunSpecError::None; }
    constexpr explicit operator bool() const noexcept { return ok(); }
};

// Complete validation, with deterministic first-error field order. Every
// string is semantic UTF-8 without embedded NUL (all cross C-string v1).
// Required: identity key, tickerid, scheduling timezone, and both timeframe
// literals unless timeframe_undetected is explicitly set.
// Empty chart timezone is preserved as optional observation metadata.
// Calendar parsing/compatibility remain in native_calendar. Batch monthly
// pairings are accepted here; stream-only restrictions belong to begin.
// Calendar parsing may allocate. Allocation/other dependency exceptions are
// converted into typed failure facts; the supplied spec is never changed.
NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec& spec) noexcept;

// Validate the WHOLE value first, then canonicalize its admitted numeric
// negative zero (fee_value) to positive zero. Failure preserves every input
// bit/string/optional. Positive-only fields cannot admit either zero sign;
// price_tick admits and preserves both zero signs, and absent optionals have no
// payload. Literal strings/numbers are never otherwise rewritten. There is no
// second validated/live configuration wrapper.
// Host usage: copy input into a candidate, normalize candidate, then stage
// that same spec atomically; own copy-allocation/lifecycle failure handling.
NativeRunSpecValidation normalize_native_run_spec(NativeRunSpec& spec) noexcept;

// Exact FNV-1a content digest for a retained intrabar path. It includes the
// mode, lower bars in caller order when present, and every sampling parameter,
// so continuation identity cannot silently reuse a path from another begin.
std::uint64_t native_intrabar_path_digest(const IntrabarPath& path) noexcept;

static_assert(std::is_trivially_copyable_v<NativeRunSpecValidation>);
static_assert(std::is_nothrow_move_constructible_v<NativeRunSpec>);
static_assert(std::is_nothrow_move_assignable_v<NativeRunSpec>);

}  // inline namespace native_run_spec_v2
} // namespace pineforge
