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

/// Encodings coincide with the versioned native-v1 C transport. These values
/// describe native execution; they do not configure source strategy policies.
enum class NativeFeeKind : std::uint32_t {
    Percent = 0,
    CashPerUnit = 1,
    CashPerExecution = 2,
};

/// When a request born at a script calculation may first match.
/// NextEligiblePoint (the default) waits for a later eligible matching point — the
/// next modeled opening, an observed print, or a carried open — and never fills on
/// the bar's already presented open/high/low/close. AfterCalculation additionally
/// offers a modeled close point after that calculation, still obeying the birth
/// ordinal and floor; it is not a replay of observed prints. The Pine adapter
/// projects process_orders_on_close onto AfterCalculation. Pinned by
/// tests/test_native_resting_driver_contract.cpp.
enum class NativeCloseExecution : std::uint32_t {
    NextEligiblePoint = 0,
    AfterCalculation = 1,
};

/// Abort presentation is a run-level policy rather than an exception-path
/// convention. Generic hosts retain an error diagnostic by default; a host
/// that models cooperative cancellation can opt into a quiet status result.
enum class NativeAbortReporting : std::uint32_t {
    Error = 0,
    Quiet = 1,
};

/// Who records the per-script-bar report series. HostRecorded leaves the
/// equity curve, its metrics and any range-end row entirely to the host, which
/// is what every host that drives its own recording already does.
/// KernelRecorded asks the consumer to mark one equity point per script
/// calculation, so a bare host gets a truthful curve, finite drawdown/run-up
/// metrics and a report whose walk is not degenerate. Recording is reporting:
/// it books no cash and places no order.
/// KernelRecordedAtHostMarks records the very same series, at the points the
/// host marks: a host whose report cadence is not one point per calculation —
/// a source adapter that re-enters its script on a fill, or publishes a bar
/// its script never calculates — keeps that cadence and still stops owning
/// what a report point is. The consumer never records on its own initiative
/// under it, so it leaves the continuation identity exactly where
/// HostRecorded leaves it (see hash_spec in native_execution_consumer.cpp).
enum class NativeReportPolicy : std::uint32_t {
    HostRecorded = 0,
    KernelRecorded = 1,
    KernelRecordedAtHostMarks = 2,
};

/// Which opening directions the run admits at all. Both is the default and the
/// whole established surface; a refused opening is
/// MatchRejectReason::OpeningDirection, and it rejects the ENTIRE transaction,
/// including a proposed close remainder. Closing-only reductions stay legal under
/// None. Pinned by tests/test_native_resting_matching_contract.cpp.
enum class NativeOpenDirections : std::uint32_t {
    None = 0,
    Long = 1,
    Short = 2,
    Both = 3,
};

/// When the kernel asks the host to calculate. BarClose is the whole default
/// surface: exactly one calculation per script bar, at its close, which is
/// what every host that drives its own cadence already gets. The other two
/// are a strict superset of the one before them, so a host never loses the
/// close calculation by opting in.
///
/// BarCloseAndFills additionally recalculates once at the cursor of each
/// applied execution, from the existing applied-notification drain and
/// bounded by max_recalculations_per_point. EveryModeledPoint additionally
/// recalculates at every modeled point of the delivered path (each confirmed
/// OHLC waypoint, each intrabar sample) and at every observed print.
///
/// This is a generic cadence, not a source-language policy: TradingView's
/// waypoint-only COOF refill, its two-fills-at-open rule and its script-state
/// rollback stay in the source layer, which never sets this field.
enum class NativeCalculationTrigger : std::uint32_t {
    BarClose = 0,
    BarCloseAndFills = 1,
    EveryModeledPoint = 2,
};

/// What a bar-open callback is handed. Complete keeps the established view:
/// on_native_bar_open receives the whole script bar, which is what a host that
/// schedules against the bar's own high/low needs. OpenOnly masks that
/// lookahead for hosts that must decide at the open with open-only
/// information: H = L = C = open and volume 0. It changes no matching, no
/// fill and no other callback; mid-bar callbacks answer current_partial_bar()
/// for the lookahead-free bar so far.
enum class NativeOpenBarView : std::uint32_t {
    Complete = 0,
    OpenOnly = 1,
};

/// A generic instrument price grid. The kernel otherwise treats price_tick as
/// the slippage multiplier only, so an unset grid leaves every booked price
/// exactly as the path presented it. QuantizeFills books the fill on the tick
/// ladder; QuantizeFillsAndTriggers additionally tests a resting trigger
/// against the tick-quantized path. Source-language tick quirks are not
/// spelled here: they remain source-layer policy on top of None.
enum class NativePriceGrid : std::uint32_t {
    None = 0,
    QuantizeFills = 1,
    QuantizeFillsAndTriggers = 2,
};

/// HalfUp is the nearest tick with ties away from zero. Directional rounds
/// toward the price region the resting order needs: a buy limit rounds down
/// and a sell limit up, a stop the other way, which is also the adverse side
/// of a market fill.
enum class NativeGridRounding : std::uint32_t {
    HalfUp = 0,
    Directional = 1,
};

/// Which units a kernel-issued liquidation reduces (L4).  RestoreMinimum is
/// the fewest units that restore the marked equity to the maintenance
/// requirement at the sizing mark; ShortfallMultiple books that same restore
/// scaled by `shortfall_multiple` (never 4.0 by default: a broker wanting a
/// multiple declares it); Flatten closes the whole position. A host whose
/// slice rule is not one of these answers it through
/// resolve_margin_call_units and declares none of them (the Pine adapter does).
enum class NativeLiquidationSizing : std::uint32_t {
    RestoreMinimum = 0,
    ShortfallMultiple = 1,
    Flatten = 2,
};

/// When the kernel tests the maintenance requirement. PathAdverseExtreme
/// evaluates it against the most adverse price the remaining modeled script
/// path still reaches and rests the reduction at the liquidation level, so the
/// fill lands where the account actually runs out of margin. CalculationOnly
/// tests the mark only at a script calculation point and rests nothing.
/// PathAdverseExtremeMark is the period-mark broker: it measures the breach at
/// that same adverse mark and rests the reduction AT THAT MARK, so the fill
/// lands on the adverse waypoint the breach was measured at instead of on a
/// solved level. It never solves a level, which is also what makes it the one
/// mode that still checks where no level exists (a LONG at full maintenance —
/// see NativeMarginModel). The resting price is the default resolved price of
/// that fill, so resolve_execution_terms still has the last word on it.
enum class NativeLiquidationCheck : std::uint32_t {
    PathAdverseExtreme = 0,
    CalculationOnly = 1,
    PathAdverseExtremeMark = 2,
};

/// Which equity the maintenance requirement is tested against. MarkedEquity is
/// the account's marked equity exactly as marked_equity() computes it: the open
/// entries' commissions have already reduced it. MarkedEquityBeforeOpenCommission
/// is the same mark-to-market equity taken before that reduction (initial
/// capital + realized net profit + open profit), i.e. a broker whose margin
/// equity does not charge the still-open entries' commission against the
/// account. Nothing else about the account model changes: this is one term of
/// one comparison, never a second accounting truth.
enum class NativeMarginEquityBasis : std::uint32_t {
    MarkedEquity = 0,
    MarkedEquityBeforeOpenCommission = 1,
};

/// Which base the liquidation level is solved from. MarkedEquity is the
/// intercept of marked_equity(): initial capital + realized net profit minus
/// the open entries' commissions. RealizedOnly drops that last term, so the
/// level is solved from initial capital + realized net profit alone. The two
/// agree whenever no open entry has paid a commission.
enum class NativeLiquidationLevelBase : std::uint32_t {
    MarkedEquity = 0,
    RealizedOnly = 1,
};

/// A generic per-side broker margin model (L4). It is entirely opt-in: a spec
/// that leaves `NativeRunSpec::margin` unset keeps the one-scalar
/// `initial_margin_fraction` gate and has no liquidation path at all.
///
/// `initial_long` / `initial_short` are the opening-admission fractions (not
/// percents) applied to the resulting absolute notional of an opening, per
/// side. Both must be finite and never negative.
///
/// ZERO is the MAINTENANCE-ONLY spelling for that side: the kernel enforces no
/// opening requirement on it, at the candidate gate and at placement alike, and
/// the HOST owns opening admission there. Opening admission and liquidation are
/// two different broker functions: a host that runs its own pre-trade check
/// (and says so at the candidate gate by answering
/// NativePrecommitVerdict::AdmitWithHostMargin) still wants the kernel's
/// liquidation mechanism, and this is how it asks for one without the other.
/// A side may waive either function but not both: `initial_* == 0.0` is legal
/// only where that side's `maintenance_*` is set (and positive), and a side
/// with neither is refused as MarginSideUndeclared. A model whose `initial_*`
/// are positive behaves exactly as it always has.
///
/// `maintenance_long` / `maintenance_short` are the liquidation fractions.
/// Absent means that side never liquidates. Present means the kernel solves
/// for the liquidation level -- the price at which the marked equity falls
/// below the maintenance requirement -- and rests a kernel-originated Reduce
/// with Stop{level} while the requirement is breached on the modeled path.
/// A maintenance fraction equal to 1.0 has no finite level for a LONG: at
/// full maintenance a long's equity and requirement move together, so the
/// breach is a constant and no price solves it. That degenerate slope rests
/// nothing under PathAdverseExtreme -- there is no price to rest at -- while
/// PathAdverseExtremeMark, which never solves a level, still measures the
/// breach at the adverse mark and still consults the host's requirement hook.
/// Source-language money rules beyond that are source-layer policy, never
/// spelled here.
///
/// `liquidation_min_units` is the broker's minimum liquidation trade: a
/// computed reduction below it flattens the position instead.
///
/// `basis` and `level_base` are the two places brokers legitimately disagree
/// about money: which equity the requirement is tested against, and which base
/// the reported level is solved from. Both default to the marked-equity model
/// the kernel has always used, and both fold into the run spec's digest only
/// when moved off it.
///
/// `liquidation_label` / `liquidation_comment` name the TICKET the kernel's own
/// liquidation is booked under. A broker's forced liquidation carries the
/// broker's identifiers, and a reporting layer that classifies a closed row by
/// its ticket id -- which is the ordinary way to say "this row was a margin
/// call" -- can only do so if those identifiers are the broker's. Empty keeps
/// the kernel's own ("__kernel_liquidation__" / "Margin liquidation"); a set
/// value is used verbatim for the request's label and comment and therefore
/// for the closed row's exit id and comment. Like the two money bases, each
/// folds into the model's digest only when set, so a model that does not name
/// its ticket digests exactly as it did before they existed.
struct NativeMarginModel {
    double initial_long = 0.0;
    double initial_short = 0.0;
    std::optional<double> maintenance_long;
    std::optional<double> maintenance_short;
    NativeLiquidationSizing sizing = NativeLiquidationSizing::RestoreMinimum;
    double shortfall_multiple = 1.0;
    std::optional<double> liquidation_min_units;
    NativeLiquidationCheck check = NativeLiquidationCheck::PathAdverseExtreme;
    NativeMarginEquityBasis basis = NativeMarginEquityBasis::MarkedEquity;
    NativeLiquidationLevelBase level_base = NativeLiquidationLevelBase::MarkedEquity;
    std::string liquidation_label;
    std::string liquidation_comment;
};

/// One risk threshold (L9). `value` is account currency when `percent` is
/// false, and a percentage of the limit's own basis equity — the running peak
/// for a drawdown, the day's opening equity for an intraday loss — when it is
/// true. Percent is out of 100: 2.5 means two and a half percent.
struct NativeLossLimit {
    double value = 0.0;
    bool percent = false;
};

/// Which day a risk limit's "day" is. SessionDay is the run's own session
/// calendar: the trading date of the session that contains the instant
/// (native_calendar::session_day_ordinal), so an overnight session is one day.
/// CalendarDayInTimezone is the plain civil date in the spec's scheduling
/// timezone, which is what a host that reports by wall-clock date wants. The
/// two differ exactly where a session crosses midnight.
enum class NativeRiskDay : std::uint32_t {
    SessionDay = 0,
    CalendarDayInTimezone = 1,
};

/// What a breach does. BlockOpenings refuses every opening while the block
/// lasts and leaves the live book alone. FlattenAndBlock first closes the book
/// with one kernel-originated Flatten and then blocks.
enum class NativeRiskAction : std::uint32_t {
    BlockOpenings = 0,
    FlattenAndBlock = 1,
};

/// Generic account risk limits (L9), entirely opt-in: a spec that leaves
/// `NativeRunSpec::risk` unset evaluates nothing, appends no event and folds
/// nothing into the continuation digest. The existing per-opening caps
/// (max_abs_units, max_open_lots, allowed_open_directions) are unchanged and
/// independent of this block.
///
/// Every limit is measured at three points of each script bar — its open, its
/// own close calculation, and after each applied drain — against the marked
/// equity there:
///   max_drawdown              equity has fallen from its running peak by at
///                             least the limit (percent: of that peak)
///   max_intraday_loss         equity is below the day's opening equity by at
///                             least the limit (percent: of that opening
///                             equity)
///   max_consecutive_loss_days N consecutive days each closing with a realized
///                             loss; a day that closes with a realized profit
///                             restarts the count, a day with no realized
///                             result leaves it where it was
///   max_fills_per_day         N applied fills within the day, counted as they
///                             settle, so the fills of one matching point all
///                             settle and the limit blocks the next admission
///
/// A drawdown or consecutive-loss-days breach blocks openings to the end of
/// the run; the two intraday limits block to the end of their own day.
/// TradingView's own strategy.risk.* rules are not this model: its chart-day
/// key, its cancel-pending behaviour and its epsilon stay in the source layer,
/// which never sets this block.
struct NativeRiskLimits {
    std::optional<NativeLossLimit> max_drawdown;
    std::optional<NativeLossLimit> max_intraday_loss;
    std::optional<std::uint32_t> max_consecutive_loss_days;
    std::optional<std::uint32_t> max_fills_per_day;
    NativeRiskDay day_basis = NativeRiskDay::SessionDay;
    NativeRiskAction action = NativeRiskAction::BlockOpenings;
};

/// Native hosts normally require every confirmed bar to name a canonical input
/// slot.  A host whose feed carries provider labels can retain the caller's
/// strictly-increasing timestamps as its decision labels instead.  This is a
/// feed-shape policy, not a source-language one, and it remains a run-spec
/// value so the two modes never share a continuation.
enum class NativeSlotLabelPolicy : std::uint32_t {
    Canonical = 0,
    FeedTolerant = 1,
    /// Deprecated spelling of FeedTolerant; identical value, kept so existing
    /// hosts and the source adapter compile unchanged.
    LegacyTolerant = FeedTolerant,
};

/// Generic ordering for a modeled OHLC path. Auto retains the open-proximity
/// rule; the forced modes make the first excursion explicit for replay/live
/// hosts without relying on process-global or source-language state.
enum class NativePathOrder : std::uint32_t {
    Auto = 0,
    HighFirst = 1,
    LowFirst = 2,
};

/// Explicit, opt-in admission exceptions for a tolerated input-feed shape.
/// They are separate from slot labels because a host may need the tolerant
/// price / unavailable-volume admission while retaining canonical calendar
/// labels.  This is a feed-shape policy, not a source-language one.
enum class NativeFeedTolerance : std::uint32_t {
    None = 0,
    /// Match the batch structural check: finite OHLC values need not be
    /// positive, and NaN volume means unavailable activity.
    BatchStructuralBars = 1u << 0,
    /// Source-compatible stream warmups admit finite, non-negative interim
    /// OHLC values.  The final warmup close remains strictly positive.
    WarmupNonNegativeOHLC = 1u << 1,
};

/// Deprecated spelling of the tolerance type. Same type, same values, same
/// hash; kept so existing hosts and the source adapter compile unchanged.
using NativeLegacyTolerance = NativeFeedTolerance;

/// Whether one NativeFeedTolerance bit is set in a run's mask. Prefer it to
/// testing the bits by hand; `enabled` is the run's own value and `requested` the
/// single bit being asked about.
constexpr bool native_feed_tolerance_enabled(
        NativeFeedTolerance enabled, NativeFeedTolerance requested) noexcept {
    return (static_cast<std::uint32_t>(enabled)
            & static_cast<std::uint32_t>(requested)) != 0u;
}

/// Deprecated spelling of native_feed_tolerance_enabled.
constexpr bool native_legacy_tolerance_enabled(
        NativeFeedTolerance enabled, NativeFeedTolerance requested) noexcept {
    return native_feed_tolerance_enabled(enabled, requested);
}

/// An owned lower-timeframe execution path.  It is deliberately a run-spec
/// value rather than a caller borrow: public begin arguments expire when the
/// begin call returns, whereas native matching may need the lower bars later
/// while sealing an aggregated script bar.
struct IntrabarPath {
    /// No intrabar path: the confirmed OHLC waypoints are the whole modeled walk, and
    /// on_native_sub_bar never fires. The default.
    struct none {};
    enum class SampleEligibility : std::uint32_t {
        /// Native hosts retain continuous matching between generated samples
        /// unless they explicitly request point-only sample eligibility.
        ContinuousSegments = 0,
        DistributionSamples = 1,
    };
    /// A RETAINED finer feed: the host's own lower-timeframe bars, the literal they
    /// are at, and the sampling policy over them. It is the only alternative that has
    /// sub-bars of its own, so it is what makes on_native_sub_bar reachable, and it is
    /// what gives the margin model a delivered sample to re-evaluate at instead of a
    /// whole-bar waypoint. C spelling: PF_NATIVE_INTRABAR_LOWER_TF.
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
    /// A synthesized path has no retained lower feed. The driver samples each
    /// script bar's own OHLC path through the generic sampler declared in
    /// include/pineforge/magnifier.hpp. Its point-only eligibility is inherent
    /// to this mode, so there is no separate SampleEligibility member.
    struct synthesized {
        int samples = 4;
        MagnifierDistribution distribution = MagnifierDistribution::ENDPOINTS;
        bool volume_weighted = false;
        int volume_weighted_min_samples = 2;
        int volume_weighted_max_samples = 64;
    };
    /// The three alternatives as one value. Exhaustive; use the accessors below rather
    /// than std::get_if at call sites.
    using value_type = std::variant<none, lower_tf, synthesized>;

    value_type value = none{};

    /// Whether this run declares no intrabar path at all.
    bool is_none() const noexcept { return std::holds_alternative<none>(value); }
    /// The retained finer feed, or nullptr when this is not the lower_tf alternative.
    const lower_tf* lower() const noexcept { return std::get_if<lower_tf>(&value); }
    /// The mutable retained finer feed, or nullptr; a staged spec's feed is copied by
    /// configure_native, so mutating it afterwards changes nothing about the run.
    lower_tf* lower() noexcept { return std::get_if<lower_tf>(&value); }
    /// The synthesized sampling policy, or nullptr when this is not that alternative.
    const synthesized* synthesized_path() const noexcept {
        return std::get_if<synthesized>(&value);
    }
    /// The mutable synthesized sampling policy, or nullptr.
    synthesized* synthesized_path() noexcept { return std::get_if<synthesized>(&value); }
};

/// One declared higher-timeframe series of the run's own symbol, the native
/// equivalent of request.security(syminfo.tickerid, tf, ...). The kernel
/// aggregates the accepted input into `tf` buckets and delivers each completed
/// bucket to the host; nothing here configures a source language.
///
/// A subscription is a series INSTANCE, not a period: several may declare the
/// same `tf`, each gets its own evaluator and bucket state, and each is
/// identified — in NativeTimeframeBarContext::subscription and in
/// native_series_bar() — by its own index in NativeRunSpec::subscriptions.
/// The one thing same-period instances cannot each own is a different
/// `authoritative_bars` feed; see DuplicateSubscriptionTimeframe.
///
/// `tf` must pair with NativeRunSpec::input_tf exactly as script_tf does
/// (native_calendar::compatibility) and may not be strictly finer than the
/// input: a lower-timeframe array is a different contract.
///
/// `authoritative_bars` are the exchange's own bars of that timeframe, at most
/// one per completed bucket. When present, a completed bucket takes its
/// OHLCV from the bar keyed to the same period, and those stamps become the
/// period partition: the feed store is TradingView-calibrated, so "W"/"M"
/// buckets are built from installed DAILY bars and a session with no stamp of
/// its own folds into the next trade date's bar. A host that supplies them
/// inherits those rules (docs/pages/native-engine.md).
///
/// `lookahead` is Pine's barmerge.lookahead_off (false, the default: the
/// bucket is delivered when its last contributing input bar is accepted) or
/// lookahead_on (true: the completed bucket's final values are delivered at
/// its FIRST contributing input bar).
///
/// `gaps` is Pine's barmerge.gaps_off (false, the default: a delivered bucket
/// stands until the next delivery replaces it) or gaps_on (true: the series is
/// CLEARED on every accepted input that delivers no bucket of its own, so
/// native_series_bar() answers nullopt — the empty that stands for na — on
/// exactly the bars the series does not publish on). It changes nothing about
/// which buckets complete, when they are delivered, or what they contain.
///
/// `source` names the bars the series is built from. Input (the default, and
/// the whole surface described above) aggregates the accepted input.
/// AuxiliaryFeed aggregates NativeRunSpec::auxiliary_feed instead — the finer
/// bars the input does not have — so `tf` then pairs with the FEED's timeframe
/// exactly as script_tf pairs with input_tf, and may be finer than the input,
/// equal to it or coarser. See NativeAuxiliaryFeed for the routing rule.
enum class NativeSeriesSource : std::uint8_t {
    Input = 0,
    AuxiliaryFeed = 1,
};

struct NativeTimeframeSubscription {
    std::string tf;
    std::vector<Bar> authoritative_bars;
    bool lookahead = false;
    bool gaps = false;
    NativeSeriesSource source = NativeSeriesSource::Input;
};

/// An auxiliary feed of the run's OWN symbol at a timeframe strictly finer
/// than the input: the bars the input feed does not have. It drives nothing by
/// itself — no matching point, no calculation, no script bar — and is read
/// only by the declared series whose `source` is AuxiliaryFeed.
///
/// `tf` must parse, and input_tf must pair over it exactly as a script_tf pairs
/// over an input_tf, strictly coarser: "1" under a "15" input, "15" under "D".
/// `bars` are strictly increasing in time and may be empty (a stream that
/// learns every bar live, NativeStrategyHost::append_auxiliary_bars).
///
/// Routing is by time and by nothing else. When an input bar is accepted, every
/// feed bar not yet consumed that opened BEFORE that input's period ended
/// (NativeInterval::next_period_open_ms of the input's own interval) is folded,
/// in feed order, into each AuxiliaryFeed series, ahead of that input's
/// aggregation, matching and calculation — the delivery point a series built
/// from the input has. So bars inside the input ride on it; bars in a hole of
/// the input ride on the next accepted input; bars earlier than the first
/// input are folded on that first input, which is how a host supplies history
/// the input does not reach; and bars later than the last input's period are
/// never folded. There is no chart-slice mapping, no label re-keying and no
/// deferred publication: those are a source layer's.
struct NativeAuxiliaryFeed {
    std::string tf;
    std::vector<Bar> bars;
};

/// One complete setup value, staged/copied by NativeStrategyHost before it is
/// applied at begin. This aggregate owns no host phase, consumed-run counter,
/// parsed-calendar authority, physical account, or C transport presence mask.
/// Empty required strings and zero capital/value/FX defaults make an incomplete
/// value invalid; price_tick == 0 explicitly selects unquantized prices. No
/// timezone/timeframe/instrument facts are inferred.
struct NativeRunSpec {
    native_order::RunIdentity identity;
    std::string input_tf;
    std::string script_tf;
    /// A public begin with fewer than two bars may not establish a timeframe.
    /// This preserves that explicit state without inventing a clock literal.
    bool timeframe_undetected = false;
    /// Strict native hosts retain the canonical slot-label rule. A provider
    /// feed may opt into raw, strictly-increasing caller labels.  The declared
    /// type spellings below are the ones the native C++ ABI guard pins.
    NativeSlotLabelPolicy slot_label_policy = NativeSlotLabelPolicy::Canonical;
    NativeFeedTolerance legacy_tolerance = NativeFeedTolerance::None;
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
    /// Opt-in instrument grid. None keeps unquantized prices; both quantizing
    /// modes require price_tick > 0 and round before slippage.
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
    /// Opt-in generic margin model. Mutually exclusive with
    /// initial_margin_fraction, which remains the one-scalar spelling. Folded
    /// into the continuation digest only when it is present, so a spec that
    /// declares none keeps its pre-margin-model identity byte for byte.
    std::optional<NativeMarginModel> margin;
    /// Opt-in generic account risk limits. Absent is the whole default
    /// surface: nothing is measured, no risk event is appended, and the
    /// continuation digest is the pre-risk one. Folded into it only when the
    /// block is present, exactly as `margin` is.
    std::optional<NativeRiskLimits> risk;
    NativeReportPolicy report_policy = NativeReportPolicy::HostRecorded;
    /// Report a position still open at run end as a mark-to-market closed row
    /// at the last close. KernelRecorded only, and reporting only: the live
    /// book, the realized sums and every hash are left exactly as the run left
    /// them. Inert under HostRecorded, whose host owns the whole report series.
    bool report_open_position_at_end = false;
    /// Calculation timing. BarClose is the established cadence and the whole
    /// default surface; the other triggers only add calculations, never move
    /// or remove one. max_recalculations_per_point bounds the fill cascade at
    /// one matching point: further executions at that point are still applied
    /// and still delivered to on_native_applied, they just stop driving a new
    /// calculation. Zero is a legal bound and means "deliver, never
    /// recalculate". All three fold into the continuation hash only once the
    /// trigger or the open-bar view is non-default, so a spec that leaves the
    /// cadence alone keeps the continuation identity it had before these
    /// fields existed.
    NativeCalculationTrigger calculation = NativeCalculationTrigger::BarClose;
    std::uint32_t max_recalculations_per_point = 8;
    NativeOpenBarView open_bar_view = NativeOpenBarView::Complete;
    IntrabarPath intrabar{};
    /// Declared higher-timeframe series. Empty is the whole default surface:
    /// no evaluator is registered, no feed is prepared, and the run spec's
    /// continuation digest is the pre-subscription one.
    std::vector<NativeTimeframeSubscription> subscriptions;
    /// Opt-in auxiliary finer feed. Absent is the whole default surface:
    /// nothing is stored, nothing is routed, and the continuation digest is the
    /// pre-feed one. Folded into it only when the block is present, exactly as
    /// `margin` and `risk` are.
    std::optional<NativeAuxiliaryFeed> auxiliary_feed;
};

/// Which field a validation refused, in deterministic first-error order. It is
/// what lets a host report "which field" instead of "invalid"; every enumerator
/// names a NativeRunSpec member or one of its nested blocks.
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
    MarginEquityBasis, MarginLevelBase,
    Calculation, OpenBarView,
    RiskLimits, RiskDrawdown, RiskIntradayLoss, RiskLossDays, RiskFillsPerDay,
    RiskDayBasis, RiskAction,
    AuxiliaryFeedTimeframe, AuxiliaryFeedBars, SubscriptionSource,
};

/// Why a field was refused. Read it beside NativeRunSpecValidation::field: the
/// error says what is wrong and the field says where. Each feature's own suite
/// pins the refusals of the fields it owns — the subscription and auxiliary-feed
/// rows in tests/test_native_auxiliary_feed.cpp, the margin rows in
/// tests/test_native_margin_model.cpp, the risk rows in
/// tests/test_native_risk_limits.cpp.
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
    // Two declared series of the same period whose `authoritative_bars`
    // differ. Same-period series are otherwise independent instances, but the
    // feed store is keyed by the timeframe's duration, so they could not own
    // their own conflicting bars. Declaring the same bars twice, or leaving
    // one (or both) empty, is accepted.
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
    // A margin equity basis / liquidation level base outside its enumeration.
    UnknownMarginEquityBasis,
    UnknownLiquidationLevelBase,
    // A calculation trigger / open-bar view outside its enumeration.
    UnknownCalculationTrigger,
    UnknownOpenBarView,
    // A risk day basis / breach action outside its enumeration.
    UnknownRiskDay,
    UnknownRiskAction,
    // A declared risk count of zero. A limit of "no losing day at all" or
    // "no fill at all" is a blocked run, never a threshold, so it is named
    // rather than silently enforced.
    ZeroRiskLimit,
    // A margin-model side that declares neither an opening requirement
    // (`initial_*` is zero) nor a liquidation one (`maintenance_*` is unset).
    // The two are separate broker functions and a side may waive either, but
    // waiving both states nothing at all, so it is named rather than read as
    // "unlimited leverage, never liquidated".
    MarginSideUndeclared,
    // An auxiliary feed whose literal does not parse, or whose pairing under
    // input_tf is not one an input would accept under a script_tf.
    InvalidAuxiliaryFeedTimeframe,
    // An auxiliary feed that is not strictly finer than input_tf. The input
    // already carries its own timeframe and everything coarser.
    AuxiliaryFeedNotFinerThanInput,
    // Auxiliary bars that are not strictly increasing in time, or that carry
    // a non-finite price or volume.
    UnorderedAuxiliaryFeedBars,
    InvalidAuxiliaryFeedBar,
    // An auxiliary feed declared with no detected input timeframe to be
    // finer than.
    AuxiliaryFeedWithoutTimeframe,
    // A series source outside its enumeration.
    UnknownSeriesSource,
    // A series built from the auxiliary feed in a spec that declares none.
    SubscriptionWithoutAuxiliaryFeed,
    // A series built from the auxiliary feed and strictly finer than it.
    SubscriptionFinerThanAuxiliaryFeed,
    // Not a field at all: the CALL was refused before any field was judged,
    // because the host was not in the phase that call is legal in. Only a
    // setup call answers it -- validate_native_run_spec and the two
    // validate_native_* functions below judge a value, never a phase, and
    // never report it. NativeFxCurveError::WrongPhase is the same word for
    // the same reason on the curve. Read with NativeRunSpecField::None: there
    // is no "where", only a "when".
    WrongPhase,
};

/// Allocation-free facts suitable for the host's durable failure variant.
/// Render text at the presentation boundary, never store exception.what().
struct NativeRunSpecValidation {
    NativeRunSpecError error = NativeRunSpecError::None;
    NativeRunSpecField field = NativeRunSpecField::None;

    constexpr bool ok() const noexcept { return error == NativeRunSpecError::None; }
    constexpr explicit operator bool() const noexcept { return ok(); }
};

/// Complete validation, with deterministic first-error field order. Every
/// string is semantic UTF-8 without embedded NUL (all cross C-string v1).
/// Required: identity key, tickerid, scheduling timezone, and both timeframe
/// literals unless timeframe_undetected is explicitly set.
/// Empty chart timezone is preserved as optional observation metadata.
/// Calendar parsing/compatibility remain in native_calendar. Batch monthly
/// pairings are accepted here; stream-only restrictions belong to begin.
/// Calendar parsing may allocate. Allocation/other dependency exceptions are
/// converted into typed failure facts; the supplied spec is never changed.
NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec& spec) noexcept;

/// Validate the WHOLE value first, then canonicalize its admitted numeric
/// negative zero (fee_value) to positive zero. Failure preserves every input
/// bit/string/optional. Positive-only fields cannot admit either zero sign;
/// price_tick admits and preserves both zero signs, and absent optionals have no
/// payload. Literal strings/numbers are never otherwise rewritten. There is no
/// second validated/live configuration wrapper.
/// Host usage: copy input into a candidate, normalize candidate, then stage
/// that same spec atomically; own copy-allocation/lifecycle failure handling.
NativeRunSpecValidation normalize_native_run_spec(NativeRunSpec& spec) noexcept;

/// Exactly the part of validate_native_run_spec that judges declared
/// higher-timeframe series, against a stated input timeframe: the pairing rule
/// (as script_tf pairs, never strictly finer), the literals, the order of any
/// authoritative bars, the one conflicting-feed refusal, and the
/// undetected-timeframe rule. A host that declares its series at begin
/// (NativeStrategyHost::declare_timeframe_subscriptions) is judged by this same
/// function, so a list accepted there is one configure_native would also have
/// accepted. Calendar parsing may allocate; failures are converted into typed
/// facts and nothing is changed.
NativeRunSpecValidation validate_native_timeframe_subscriptions(
        const std::vector<NativeTimeframeSubscription>& subscriptions,
        const std::string& input_tf, bool timeframe_undetected) noexcept;

/// The same judgement for a run that declares an auxiliary feed: the feed is
/// judged first (validate_native_auxiliary_feed), then every series, a series
/// built from the feed pairing with the FEED's timeframe. The three-argument
/// form above is this one with no feed, where such a series is refused as
/// SubscriptionWithoutAuxiliaryFeed.
NativeRunSpecValidation validate_native_timeframe_subscriptions(
        const std::vector<NativeTimeframeSubscription>& subscriptions,
        const std::string& input_tf, bool timeframe_undetected,
        const std::optional<NativeAuxiliaryFeed>& auxiliary_feed) noexcept;

/// Exactly the part of validate_native_run_spec that judges the auxiliary
/// feed, against a stated input timeframe: the literal, the strictly-finer
/// pairing under the input, the order and structure of its bars, and the
/// undetected-timeframe rule. An absent feed is always valid. A host that
/// declares its feed at begin (NativeStrategyHost::declare_auxiliary_feed) is
/// judged by this same function.
NativeRunSpecValidation validate_native_auxiliary_feed(
        const std::optional<NativeAuxiliaryFeed>& auxiliary_feed,
        const std::string& input_tf, bool timeframe_undetected) noexcept;

/// Exact FNV-1a content digest for a retained intrabar path. It includes the
/// mode, lower bars in caller order when present, and every sampling parameter,
/// so continuation identity cannot silently reuse a path from another begin.
std::uint64_t native_intrabar_path_digest(const IntrabarPath& path) noexcept;

/// Exact FNV-1a content digest for the declared higher-timeframe series. It
/// includes each subscription's timeframe literal, publication modes and
/// authoritative bars in caller order, so a continuation cannot silently reuse
/// another begin's series. Callers fold it only when `subscriptions` is
/// non-empty, keeping the default spec's continuation identity unchanged, and
/// `gaps` folds only where a series set it, keeping every series declared
/// before that field existed at the digest it already had. `source` folds the
/// same way: only where a series left the input.
std::uint64_t native_timeframe_subscriptions_digest(
        const std::vector<NativeTimeframeSubscription>& subscriptions) noexcept;

/// Exact FNV-1a content digest for the declared auxiliary feed: its timeframe
/// literal and every bar in caller order, so a continuation cannot silently
/// reuse another begin's feed. Callers fold it only when `auxiliary_feed` is
/// present, keeping the default spec's continuation identity unchanged.
std::uint64_t native_auxiliary_feed_digest(const NativeAuxiliaryFeed& feed) noexcept;

/// Machine-independent digest of a run spec: exactly the fields the consumer
/// folds into the continuation identity for the spec, and nothing else — no
/// timezone resources, no session identity beyond the spec's own. Two specs
/// with equal digests drive identical continuation identities on every machine
/// with the same tz resources.
///
/// It is the consumer's own spec fold (`hash_spec`) over a freshly seeded
/// accumulator: the same FNV-1a offset basis `continuation_hash()` starts from,
/// with nothing folded before the spec (no semantic-version markers, no state)
/// and the relative-generation base set to `spec.identity.run_number`, exactly
/// as the consumer seeds it for the run this spec describes. The folded
/// generation distance is therefore zero and the digest is a property of the
/// spec value alone. It is NOT a continuation hash and never comparable with
/// one: a raw continuation hash also folds the resolved timezone resources
/// (zoneinfo root and zone file paths), which differ per machine, so only this
/// digest is portable enough to pin as a constant.
std::uint64_t native_run_spec_digest(const NativeRunSpec& spec) noexcept;

/// Exact FNV-1a content digest for the generic margin model. It includes every
/// per-side fraction, the liquidation policy and its parameters, so two runs
/// that differ only in their margin model cannot share a continuation identity.
/// Callers fold it only when `margin` is present, keeping the default spec's
/// continuation identity unchanged.
std::uint64_t native_margin_model_digest(const NativeMarginModel& margin) noexcept;

/// Exact FNV-1a content digest for the generic risk limits. It includes every
/// threshold, its percent flag, the day basis and the breach action, so two
/// runs that differ only in their risk limits cannot share a continuation
/// identity. Callers fold it only when `risk` is present, keeping the default
/// spec's continuation identity unchanged.
std::uint64_t native_risk_limits_digest(const NativeRiskLimits& risk) noexcept;

static_assert(std::is_trivially_copyable_v<NativeRunSpecValidation>);
static_assert(std::is_nothrow_move_constructible_v<NativeRunSpec>);
static_assert(std::is_nothrow_move_assignable_v<NativeRunSpec>);

}  // inline namespace native_run_spec_v3
} // namespace pineforge
