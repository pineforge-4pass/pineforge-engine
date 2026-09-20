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
inline namespace native_run_spec_v3 {

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

// Who records the per-script-bar report series. HostRecorded leaves the
// equity curve, its metrics and any range-end row entirely to the host, which
// is what every host that drives its own recording already does.
// KernelRecorded asks the consumer to mark one equity point per script
// calculation, so a bare host gets a truthful curve, finite drawdown/run-up
// metrics and a report whose walk is not degenerate. Recording is reporting:
// it books no cash and places no order.
// KernelRecordedAtHostMarks records the very same series, at the points the
// host marks: a host whose report cadence is not one point per calculation —
// a source adapter that re-enters its script on a fill, or publishes a bar
// its script never calculates — keeps that cadence and still stops owning
// what a report point is. The consumer never records on its own initiative
// under it, so it leaves the continuation identity exactly where
// HostRecorded leaves it (see hash_spec in native_execution_consumer.cpp).
enum class NativeReportPolicy : std::uint32_t {
    HostRecorded = 0,
    KernelRecorded = 1,
    KernelRecordedAtHostMarks = 2,
};

enum class NativeOpenDirections : std::uint32_t {
    None = 0,
    Long = 1,
    Short = 2,
    Both = 3,
};

// When the kernel asks the host to calculate. BarClose is the whole default
// surface: exactly one calculation per script bar, at its close, which is
// what every host that drives its own cadence already gets. The other two
// are a strict superset of the one before them, so a host never loses the
// close calculation by opting in.
//
// BarCloseAndFills additionally recalculates once at the cursor of each
// applied execution, from the existing applied-notification drain and
// bounded by max_recalculations_per_point. EveryModeledPoint additionally
// recalculates at every modeled point of the delivered path (each confirmed
// OHLC waypoint, each intrabar sample) and at every observed print.
//
// This is a generic cadence, not a source-language policy: TradingView's
// waypoint-only COOF refill, its two-fills-at-open rule and its script-state
// rollback stay in the source layer, which never sets this field.
enum class NativeCalculationTrigger : std::uint32_t {
    BarClose = 0,
    BarCloseAndFills = 1,
    EveryModeledPoint = 2,
};

// What a bar-open callback is handed. Complete keeps the established view:
// on_native_bar_open receives the whole script bar, which is what a host that
// schedules against the bar's own high/low needs. OpenOnly masks that
// lookahead for hosts that must decide at the open with open-only
// information: H = L = C = open and volume 0. It changes no matching, no
// fill and no other callback; mid-bar callbacks answer current_partial_bar()
// for the lookahead-free bar so far.
enum class NativeOpenBarView : std::uint32_t {
    Complete = 0,
    OpenOnly = 1,
};

// A generic instrument price grid. The kernel otherwise treats price_tick as
// the slippage multiplier only, so an unset grid leaves every booked price
// exactly as the path presented it. QuantizeFills books the fill on the tick
// ladder; QuantizeFillsAndTriggers additionally tests a resting trigger
// against the tick-quantized path. Source-language tick quirks are not
// spelled here: they remain source-layer policy on top of None.
enum class NativePriceGrid : std::uint32_t {
    None = 0,
    QuantizeFills = 1,
    QuantizeFillsAndTriggers = 2,
};

// HalfUp is the nearest tick with ties away from zero. Directional rounds
// toward the price region the resting order needs: a buy limit rounds down
// and a sell limit up, a stop the other way, which is also the adverse side
// of a market fill.
enum class NativeGridRounding : std::uint32_t {
    HalfUp = 0,
    Directional = 1,
};

// Which units a kernel-issued liquidation reduces (L4).  RestoreMinimum is
// the fewest units that restore the marked equity to the maintenance
// requirement at the sizing mark; ShortfallMultiple books that same restore
// scaled by `shortfall_multiple` (TradingView's 4.0 is the adapter's choice,
// never the default here); Flatten closes the whole position.
enum class NativeLiquidationSizing : std::uint32_t {
    RestoreMinimum = 0,
    ShortfallMultiple = 1,
    Flatten = 2,
};

// When the kernel tests the maintenance requirement. PathAdverseExtreme
// evaluates it against the most adverse price the remaining modeled script
// path still reaches and rests the reduction at the liquidation level, so the
// fill lands where the account actually runs out of margin. CalculationOnly
// tests the mark only at a script calculation point and rests nothing.
enum class NativeLiquidationCheck : std::uint32_t {
    PathAdverseExtreme = 0,
    CalculationOnly = 1,
};

// A generic per-side broker margin model (L4). It is entirely opt-in: a spec
// that leaves `NativeRunSpec::margin` unset keeps the one-scalar
// `initial_margin_fraction` gate and has no liquidation path at all.
//
// `initial_long` / `initial_short` are the opening-admission fractions (not
// percents) applied to the resulting absolute notional of an opening, per
// side. Both must be finite and positive.
//
// `maintenance_long` / `maintenance_short` are the liquidation fractions.
// Absent means that side never liquidates. Present means the kernel solves
// for the liquidation level -- the price at which the marked equity falls
// below the maintenance requirement -- and rests a kernel-originated Reduce
// with Stop{level} while the requirement is breached on the modeled path.
// A maintenance fraction equal to 1.0 has no finite level for a LONG: at
// full maintenance a long's equity and requirement move together, so the
// breach is a constant and no price solves it. Source-language money rules
// for that case are source-layer policy, never spelled here.
//
// `liquidation_min_units` is the broker's minimum liquidation trade: a
// computed reduction below it flattens the position instead.
struct NativeMarginModel {
    double initial_long = 0.0;
    double initial_short = 0.0;
    std::optional<double> maintenance_long;
    std::optional<double> maintenance_short;
    NativeLiquidationSizing sizing = NativeLiquidationSizing::RestoreMinimum;
    double shortfall_multiple = 1.0;
    std::optional<double> liquidation_min_units;
    NativeLiquidationCheck check = NativeLiquidationCheck::PathAdverseExtreme;
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

// One declared higher-timeframe series of the run's own symbol, the native
// equivalent of request.security(syminfo.tickerid, tf, ...). The kernel
// aggregates the accepted input into `tf` buckets and delivers each completed
// bucket to the host; nothing here configures a source language.
//
// `tf` must pair with NativeRunSpec::input_tf exactly as script_tf does
// (native_calendar::compatibility) and may not be strictly finer than the
// input: a lower-timeframe array is a different contract.
//
// `authoritative_bars` are the exchange's own bars of that timeframe, at most
// one per completed bucket. When present, a completed bucket takes its
// OHLCV from the bar keyed to the same period, and those stamps become the
// period partition: the feed store is TradingView-calibrated, so "W"/"M"
// buckets are built from installed DAILY bars and a session with no stamp of
// its own folds into the next trade date's bar. A host that supplies them
// inherits those rules (docs/pages/native-engine.md).
//
// `lookahead` is Pine's barmerge.lookahead_off (false, the default: the
// bucket is delivered when its last contributing input bar is accepted) or
// lookahead_on (true: the completed bucket's final values are delivered at
// its FIRST contributing input bar).
struct NativeTimeframeSubscription {
    std::string tf;
    std::vector<Bar> authoritative_bars;
    bool lookahead = false;
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
    // Opt-in instrument grid. None keeps unquantized prices; both quantizing
    // modes require price_tick > 0 and round before slippage.
    NativePriceGrid price_grid = NativePriceGrid::None;
    NativeGridRounding grid_rounding = NativeGridRounding::HalfUp;
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
    // Opt-in generic margin model. Mutually exclusive with
    // initial_margin_fraction, which remains the one-scalar spelling. Folded
    // into the continuation digest only when it is present, so a spec that
    // declares none keeps its pre-margin-model identity byte for byte.
    std::optional<NativeMarginModel> margin;
    NativeReportPolicy report_policy = NativeReportPolicy::HostRecorded;
    // Report a position still open at run end as a mark-to-market closed row
    // at the last close. KernelRecorded only, and reporting only: the live
    // book, the realized sums and every hash are left exactly as the run left
    // them. Inert under HostRecorded, whose host owns the whole report series.
    bool report_open_position_at_end = false;
    // Calculation timing. BarClose is the established cadence and the whole
    // default surface; the other triggers only add calculations, never move
    // or remove one. max_recalculations_per_point bounds the fill cascade at
    // one matching point: further executions at that point are still applied
    // and still delivered to on_native_applied, they just stop driving a new
    // calculation. Zero is a legal bound and means "deliver, never
    // recalculate". All three fold into the continuation hash only once the
    // trigger or the open-bar view is non-default, so a spec that leaves the
    // cadence alone keeps the continuation identity it had before these
    // fields existed.
    NativeCalculationTrigger calculation = NativeCalculationTrigger::BarClose;
    std::uint32_t max_recalculations_per_point = 8;
    NativeOpenBarView open_bar_view = NativeOpenBarView::Complete;
    IntrabarPath intrabar{};
    // Declared higher-timeframe series. Empty is the whole default surface:
    // no evaluator is registered, no feed is prepared, and the run spec's
    // continuation digest is the pre-subscription one.
    std::vector<NativeTimeframeSubscription> subscriptions;
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
    ReportPolicy,
    PriceGrid, GridRounding,
    SubscriptionTimeframe, SubscriptionBars,
    MarginModel, MarginInitial, MarginMaintenance, MarginSizing,
    MarginShortfallMultiple, MarginMinUnits, MarginCheck,
    Calculation, OpenBarView,
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
    UnknownReportPolicy,
    UnknownPriceGrid,
    UnknownGridRounding,
    GridRequiresPriceTick,
    // A declared higher-timeframe series whose literal does not parse, or
    // whose pairing with input_tf is not one script_tf would accept.
    InvalidSubscriptionTimeframe,
    // A declared series strictly finer than input_tf. Lower-timeframe arrays
    // are a separate contract; this is never silently promoted.
    SubscriptionFinerThanInput,
    // Two declared series of the same period. The feed store is keyed by the
    // timeframe's duration, so duplicates could not own their own bars.
    DuplicateSubscriptionTimeframe,
    // Authoritative bars that are not strictly increasing in time.
    UnorderedSubscriptionBars,
    // Subscriptions declared with no detected input timeframe to pair with.
    SubscriptionWithoutTimeframe,
    // Both the generic margin model and the one-scalar initial-margin gate
    // were set. They are two spellings of the same admission authority.
    MarginModelConflict,
    UnknownLiquidationSizing,
    UnknownLiquidationCheck,
    // A calculation trigger / open-bar view outside its enumeration.
    UnknownCalculationTrigger,
    UnknownOpenBarView,
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

// Exact FNV-1a content digest for the declared higher-timeframe series. It
// includes each subscription's timeframe literal, publication mode and
// authoritative bars in caller order, so a continuation cannot silently reuse
// another begin's series. Callers fold it only when `subscriptions` is
// non-empty, keeping the default spec's continuation identity unchanged.
std::uint64_t native_timeframe_subscriptions_digest(
        const std::vector<NativeTimeframeSubscription>& subscriptions) noexcept;

// Machine-independent digest of a run spec: exactly the fields the consumer
// folds into the continuation identity for the spec, and nothing else — no
// timezone resources, no session identity beyond the spec's own. Two specs
// with equal digests drive identical continuation identities on every machine
// with the same tz resources.
//
// It is the consumer's own spec fold (`hash_spec`) over a freshly seeded
// accumulator: the same FNV-1a offset basis `continuation_hash()` starts from,
// with nothing folded before the spec (no semantic-version markers, no state)
// and the relative-generation base set to `spec.identity.run_number`, exactly
// as the consumer seeds it for the run this spec describes. The folded
// generation distance is therefore zero and the digest is a property of the
// spec value alone. It is NOT a continuation hash and never comparable with
// one: a raw continuation hash also folds the resolved timezone resources
// (zoneinfo root and zone file paths), which differ per machine, so only this
// digest is portable enough to pin as a constant.
std::uint64_t native_run_spec_digest(const NativeRunSpec& spec) noexcept;

// Exact FNV-1a content digest for the generic margin model. It includes every
// per-side fraction, the liquidation policy and its parameters, so two runs
// that differ only in their margin model cannot share a continuation identity.
// Callers fold it only when `margin` is present, keeping the default spec's
// continuation identity unchanged.
std::uint64_t native_margin_model_digest(const NativeMarginModel& margin) noexcept;

static_assert(std::is_trivially_copyable_v<NativeRunSpecValidation>);
static_assert(std::is_nothrow_move_constructible_v<NativeRunSpec>);
static_assert(std::is_nothrow_move_assignable_v<NativeRunSpec>);

}  // inline namespace native_run_spec_v3
} // namespace pineforge
