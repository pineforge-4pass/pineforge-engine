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

enum class NativeOpenDirections : std::uint32_t {
    None = 0,
    Long = 1,
    Short = 2,
    Both = 3,
};

// An owned lower-timeframe execution path.  It is deliberately a run-spec
// value rather than a caller borrow: public begin arguments expire when the
// begin call returns, whereas native matching may need the lower bars later
// while sealing an aggregated script bar.
struct IntrabarPath {
    struct none {};
    struct lower_tf {
        std::vector<Bar> bars;
        std::string tf;
        int samples = 4;
        MagnifierDistribution distribution = MagnifierDistribution::ENDPOINTS;
        bool volume_weighted = false;
        int volume_weighted_min_samples = 2;
        int volume_weighted_max_samples = 64;
    };
    using value_type = std::variant<none, lower_tf>;

    value_type value = none{};

    bool is_none() const noexcept { return std::holds_alternative<none>(value); }
    const lower_tf* lower() const noexcept { return std::get_if<lower_tf>(&value); }
    lower_tf* lower() noexcept { return std::get_if<lower_tf>(&value); }
};

// One complete setup value, staged/copied by NativeStrategyHost before it is
// applied at begin. This aggregate owns no host phase, consumed-run counter,
// parsed-calendar authority, physical account, or C transport presence mask.
// Empty required strings and zero financial defaults make an incomplete
// value invalid; no timezone/timeframe/instrument facts are inferred.
struct NativeRunSpec {
    native_order::RunIdentity identity;
    std::string input_tf;
    std::string script_tf;

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
    double price_tick = 0.0;
    std::uint32_t slippage_ticks = 0; // <= INT_MAX; raw +/- ticks*tick, no snap.
    NativeFeeKind fee_kind = NativeFeeKind::Percent;
    double fee_value = 0.0;     // Percent/100 of absolute account notional, or
                               // account-currency cash per unit/execution.
    std::optional<double> quantity_grid; // Positive; admission only, no resize.
    NativeCloseExecution close_execution = NativeCloseExecution::NextEligiblePoint;
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
    FeeKind, FeeValue, QuantityGrid, CloseExecution, MaxAbsUnits, MaxOpenLots,
    AllowedOpenDirections, InitialMarginFraction,
    IntrabarTimeframe, IntrabarSamples, IntrabarDistribution, IntrabarVolumeSamples,
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
    UnknownOpenDirections,
    ZeroLotLimit,
    AllocationFailure,
    CalendarFailure,
    InvalidIntrabarPath,
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
// Required: identity key, both timeframe literals, tickerid, scheduling timezone.
// Empty chart timezone is preserved as optional observation metadata.
// Calendar parsing/compatibility remain in native_calendar. Batch monthly
// pairings are accepted here; stream-only restrictions belong to begin.
// Calendar parsing may allocate. Allocation/other dependency exceptions are
// converted into typed failure facts; the supplied spec is never changed.
NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec& spec) noexcept;

// Validate the WHOLE value first, then canonicalize its admitted numeric
// negative zero (fee_value) to positive zero. Failure preserves every input
// bit/string/optional. Positive-only fields cannot admit either zero sign;
// absent optionals have no payload. Literal strings/positive numbers are
// never rewritten. There is no second validated/live configuration wrapper.
// Host usage: copy input into a candidate, normalize candidate, then stage
// that same spec atomically; own copy-allocation/lifecycle failure handling.
NativeRunSpecValidation normalize_native_run_spec(NativeRunSpec& spec) noexcept;

// Exact FNV-1a content digest for a retained intrabar path.  It includes the
// lower bars in caller order and every sampling parameter, so continuation
// identity cannot silently reuse a path from another begin call.
std::uint64_t native_intrabar_path_digest(const IntrabarPath& path) noexcept;

static_assert(std::is_trivially_copyable_v<NativeRunSpecValidation>);
static_assert(std::is_nothrow_move_constructible_v<NativeRunSpec>);
static_assert(std::is_nothrow_move_assignable_v<NativeRunSpec>);

}  // inline namespace native_run_spec_v2
} // namespace pineforge
