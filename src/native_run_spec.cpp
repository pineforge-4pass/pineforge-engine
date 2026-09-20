#include <pineforge/native_run_spec.hpp>
#include <pineforge/market_driver.hpp>
#include <pineforge/native_calendar.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

namespace pineforge {
inline namespace native_run_spec_v3 {
namespace {

using Error = NativeRunSpecError;
using Field = NativeRunSpecField;
using Result = NativeRunSpecValidation;

// Unicode scalar-value UTF-8: reject overlong sequences, surrogates,
// >U+10FFFF, isolated continuation bytes and truncated sequences. Do not
// reject valid non-ASCII identities or rewrite their normalization form.
bool valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        unsigned remaining = 0;
        unsigned low = 0x80;
        unsigned high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            remaining = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            remaining = 2;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            remaining = 3;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        } else {
            return false;
        }
        if (text.size() - i < remaining) return false;
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < low || first > high) return false;
        while (--remaining != 0) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if (next < 0x80 || next > 0xBF) return false;
        }
    }
    return true;
}

Result validate_string(std::string_view value, Field field, bool required) noexcept {
    if (required && value.empty()) return {Error::EmptyRequiredString, field};
    if (value.find('\0') != std::string_view::npos) return {Error::EmbeddedNul, field};
    if (!valid_utf8(value)) return {Error::InvalidUtf8, field};
    return {};
}

bool positive(double value) noexcept { return std::isfinite(value) && value > 0.0; }
bool nonnegative(double value) noexcept { return std::isfinite(value) && value >= 0.0; }

bool valid_distribution(MagnifierDistribution distribution) noexcept {
    switch (distribution) {
    case MagnifierDistribution::UNIFORM:
    case MagnifierDistribution::COSINE:
    case MagnifierDistribution::TRIANGLE:
    case MagnifierDistribution::ENDPOINTS:
    case MagnifierDistribution::FRONT_LOADED:
    case MagnifierDistribution::BACK_LOADED:
        return true;
    }
    return false;
}

bool valid_sample_eligibility(IntrabarPath::SampleEligibility eligibility) noexcept {
    switch (eligibility) {
    case IntrabarPath::SampleEligibility::ContinuousSegments:
    case IntrabarPath::SampleEligibility::DistributionSamples:
        return true;
    }
    return false;
}

bool valid_slot_label_policy(NativeSlotLabelPolicy policy) noexcept {
    switch (policy) {
    case NativeSlotLabelPolicy::Canonical:
    case NativeSlotLabelPolicy::FeedTolerant:
        return true;
    }
    return false;
}

bool valid_report_policy(NativeReportPolicy policy) noexcept {
    switch (policy) {
    case NativeReportPolicy::HostRecorded:
    case NativeReportPolicy::KernelRecorded:
    case NativeReportPolicy::KernelRecordedAtHostMarks:
        return true;
    }
    return false;
}

bool valid_calculation_trigger(NativeCalculationTrigger trigger) noexcept {
    switch (trigger) {
    case NativeCalculationTrigger::BarClose:
    case NativeCalculationTrigger::BarCloseAndFills:
    case NativeCalculationTrigger::EveryModeledPoint:
        return true;
    }
    return false;
}

bool valid_open_bar_view(NativeOpenBarView view) noexcept {
    switch (view) {
    case NativeOpenBarView::Complete:
    case NativeOpenBarView::OpenOnly:
        return true;
    }
    return false;
}

bool valid_path_order(NativePathOrder order) noexcept {
    switch (order) {
    case NativePathOrder::Auto:
    case NativePathOrder::HighFirst:
    case NativePathOrder::LowFirst:
        return true;
    }
    return false;
}

bool valid_price_grid(NativePriceGrid grid) noexcept {
    switch (grid) {
    case NativePriceGrid::None:
    case NativePriceGrid::QuantizeFills:
    case NativePriceGrid::QuantizeFillsAndTriggers:
        return true;
    }
    return false;
}

bool valid_grid_rounding(NativeGridRounding rounding) noexcept {
    switch (rounding) {
    case NativeGridRounding::HalfUp:
    case NativeGridRounding::Directional:
        return true;
    }
    return false;
}

// The native security feed store keys one feed per timeframe DURATION, and
// treats every monthly literal as one calendar period (tf_to_seconds returns
// -1 for any "*M"). Two declared series that share this key are independent
// evaluator instances, but they cannot own DIFFERENT authoritative bars, so
// the spec refuses exactly that conflict. Overflow-safe: a count whose
// duration is not representable answers 0, which never matches a valid key
// and leaves the pairing check to name the failure.
std::int64_t subscription_period_key(const native_calendar::Timeframe& tf) noexcept {
    if (!tf.valid()) return 0;
    std::int64_t unit_seconds = 0;
    switch (tf.unit()) {
    case native_calendar::TimeframeUnit::Second: unit_seconds = 1; break;
    case native_calendar::TimeframeUnit::Minute: unit_seconds = 60; break;
    case native_calendar::TimeframeUnit::Day: unit_seconds = 86400; break;
    case native_calendar::TimeframeUnit::Week: unit_seconds = 604800; break;
    case native_calendar::TimeframeUnit::Month: return -1;
    }
    const std::int64_t count = tf.count();
    if (unit_seconds <= 0 || count <= 0
        || count > std::numeric_limits<std::int64_t>::max() / unit_seconds) {
        return 0;
    }
    return count * unit_seconds;
}

// Bitwise bar equality: two same-period series may share one feed, so the
// question is only whether the bars they declare are the same value. Compared
// by representation so a feed carrying a NaN still equals its own copy.
bool same_bar_bits(const Bar& left, const Bar& right) noexcept {
    const auto bits = [](double value) noexcept {
        std::uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof raw);
        return raw;
    };
    return left.timestamp == right.timestamp
        && bits(left.open) == bits(right.open) && bits(left.high) == bits(right.high)
        && bits(left.low) == bits(right.low) && bits(left.close) == bits(right.close)
        && bits(left.volume) == bits(right.volume);
}

// Two declared feeds of one period conflict when both are stated and they are
// not the same bars. An empty feed states nothing and never conflicts.
bool conflicting_bars(const std::vector<Bar>& left, const std::vector<Bar>& right) noexcept {
    if (left.empty() || right.empty()) return false;
    if (left.size() != right.size()) return true;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!same_bar_bits(left[i], right[i])) return true;
    }
    return false;
}

// Everything a declared series can be judged on without a calendar: the
// pairing with a detected input timeframe, the literal itself, the order of
// the bars it supplies, and that the bars it is built from were declared.
Result subscription_shapes(const std::vector<NativeTimeframeSubscription>& subscriptions,
                           bool timeframe_undetected, bool has_auxiliary_feed) noexcept {
    if (!subscriptions.empty() && timeframe_undetected) {
        return {Error::SubscriptionWithoutTimeframe, Field::SubscriptionTimeframe};
    }
    for (const auto& subscription : subscriptions) {
        const auto tf =
            validate_string(subscription.tf, Field::SubscriptionTimeframe, true);
        if (!tf) return tf;
        const auto& bars = subscription.authoritative_bars;
        for (std::size_t i = 1; i < bars.size(); ++i) {
            if (bars[i].timestamp <= bars[i - 1].timestamp) {
                return {Error::UnorderedSubscriptionBars, Field::SubscriptionBars};
            }
        }
        switch (subscription.source) {
        case NativeSeriesSource::Input:
            break;
        case NativeSeriesSource::AuxiliaryFeed:
            if (!has_auxiliary_feed) {
                return {Error::SubscriptionWithoutAuxiliaryFeed, Field::SubscriptionSource};
            }
            break;
        default:
            return {Error::UnknownSeriesSource, Field::SubscriptionSource};
        }
    }
    return {};
}

// The declared auxiliary feed, judged without a calendar: the detected
// timeframe it must be finer than, its literal, and the bars themselves.
Result auxiliary_feed_shapes(const std::optional<NativeAuxiliaryFeed>& feed,
                             bool timeframe_undetected) noexcept {
    if (!feed) return {};
    if (timeframe_undetected) {
        return {Error::AuxiliaryFeedWithoutTimeframe, Field::AuxiliaryFeedTimeframe};
    }
    const auto tf = validate_string(feed->tf, Field::AuxiliaryFeedTimeframe, true);
    if (!tf) return tf;
    for (std::size_t i = 0; i < feed->bars.size(); ++i) {
        if (!native_bar_structurally_valid(feed->bars[i])) {
            return {Error::InvalidAuxiliaryFeedBar, Field::AuxiliaryFeedBars};
        }
        if (i > 0 && feed->bars[i].timestamp <= feed->bars[i - 1].timestamp) {
            return {Error::UnorderedAuxiliaryFeedBars, Field::AuxiliaryFeedBars};
        }
    }
    return {};
}

// The input pairs over the auxiliary feed exactly as a script timeframe pairs
// over an input, and strictly: a feed of the input's own period or coarser
// states nothing the input does not already carry.
Result auxiliary_feed_pairing(const NativeAuxiliaryFeed& feed,
                              const native_calendar::Timeframe& input,
                              std::optional<native_calendar::Timeframe>& parsed) {
    const Field active_field = Field::AuxiliaryFeedTimeframe;
    parsed = native_calendar::parse_timeframe(feed.tf);
    if (!parsed) return {Error::InvalidAuxiliaryFeedTimeframe, active_field};
    const auto pairing = native_calendar::compatibility(*parsed, input);
    switch (pairing.pairing) {
    case native_calendar::TimeframePairing::SameUnitMultiple:
    case native_calendar::TimeframePairing::FixedDivisible:
        if (pairing.group_factor > 1) return {};
        return {Error::AuxiliaryFeedNotFinerThanInput, active_field};
    case native_calendar::TimeframePairing::FixedToCalendar:
    case native_calendar::TimeframePairing::CalendarToCalendar:
        return {};
    case native_calendar::TimeframePairing::Passthrough:
    case native_calendar::TimeframePairing::ScriptFiner:
        return {Error::AuxiliaryFeedNotFinerThanInput, active_field};
    default:
        return {Error::InvalidAuxiliaryFeedTimeframe, active_field};
    }
}

// Declared higher-timeframe series pair with the INPUT timeframe exactly as
// script_tf does. A strictly finer request is named separately: it is a
// lower-timeframe array contract, never a silently promoted aggregate. A
// subscription is a series instance, not a period: several may share one
// timeframe, each with its own evaluator, bucket state and delivery index.
// Only their feeds are shared, so the one refusal left is two same-period
// series declaring DIFFERENT authoritative bars.
//
// A series built from the auxiliary feed pairs with the FEED's timeframe by
// the same rule, so it may be finer than the input; `auxiliary` is that
// parsed timeframe, null for a spec that declares no feed (the shapes pass
// has then already refused every such series).
Result subscription_pairings(const std::vector<NativeTimeframeSubscription>& subscriptions,
                             const native_calendar::Timeframe& input,
                             const native_calendar::Timeframe* auxiliary) {
    const Field active_field = Field::SubscriptionTimeframe;
    std::vector<std::int64_t> keys;
    keys.reserve(subscriptions.size());
    for (const auto& subscription : subscriptions) {
        const auto requested = native_calendar::parse_timeframe(subscription.tf);
        if (!requested) {
            return {Error::InvalidSubscriptionTimeframe, active_field};
        }
        const bool from_feed = subscription.source == NativeSeriesSource::AuxiliaryFeed;
        if (from_feed && auxiliary == nullptr) {
            return {Error::SubscriptionWithoutAuxiliaryFeed, Field::SubscriptionSource};
        }
        switch (native_calendar::compatibility(from_feed ? *auxiliary : input, *requested)
                    .pairing) {
        case native_calendar::TimeframePairing::Passthrough:
        case native_calendar::TimeframePairing::SameUnitMultiple:
        case native_calendar::TimeframePairing::FixedDivisible:
        case native_calendar::TimeframePairing::FixedToCalendar:
        case native_calendar::TimeframePairing::CalendarToCalendar:
            break;
        case native_calendar::TimeframePairing::ScriptFiner:
            return {from_feed ? Error::SubscriptionFinerThanAuxiliaryFeed
                              : Error::SubscriptionFinerThanInput,
                    active_field};
        default:
            return {Error::InvalidSubscriptionTimeframe, active_field};
        }
        const std::int64_t key = subscription_period_key(*requested);
        for (std::size_t seen = 0; seen < keys.size(); ++seen) {
            if (keys[seen] != key) continue;
            if (conflicting_bars(subscriptions[seen].authoritative_bars,
                                 subscription.authoritative_bars)) {
                return {Error::DuplicateSubscriptionTimeframe, Field::SubscriptionBars};
            }
        }
        keys.push_back(key);
    }
    return {};
}

bool valid_liquidation_sizing(NativeLiquidationSizing sizing) noexcept {
    switch (sizing) {
    case NativeLiquidationSizing::RestoreMinimum:
    case NativeLiquidationSizing::ShortfallMultiple:
    case NativeLiquidationSizing::Flatten:
        return true;
    }
    return false;
}

bool valid_liquidation_check(NativeLiquidationCheck check) noexcept {
    switch (check) {
    case NativeLiquidationCheck::PathAdverseExtreme:
    case NativeLiquidationCheck::CalculationOnly:
    case NativeLiquidationCheck::PathAdverseExtremeMark:
        return true;
    }
    return false;
}

bool valid_margin_equity_basis(NativeMarginEquityBasis basis) noexcept {
    switch (basis) {
    case NativeMarginEquityBasis::MarkedEquity:
    case NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission:
        return true;
    }
    return false;
}

bool valid_liquidation_level_base(NativeLiquidationLevelBase base) noexcept {
    switch (base) {
    case NativeLiquidationLevelBase::MarkedEquity:
    case NativeLiquidationLevelBase::RealizedOnly:
        return true;
    }
    return false;
}

// The generic margin model is the whole admission authority of the run that
// declares it. Both spellings at once is a configuration conflict, never a
// silent precedence rule.
Result validate_margin(const NativeRunSpec& spec) noexcept {
    if (!spec.margin) return {};
    if (spec.initial_margin_fraction) {
        return {Error::MarginModelConflict, Field::MarginModel};
    }
    const auto& margin = *spec.margin;
    // Opening admission and liquidation are two different broker functions,
    // and a side may declare either, or both. A stated initial fraction is
    // still finite and never negative; ZERO is the maintenance-only spelling
    // (the kernel enforces no opening requirement on that side -- the host
    // owns the pre-trade check -- while the kernel keeps the side's
    // liquidation), so it is not an invalid number here.
    if (!nonnegative(margin.initial_long) || !nonnegative(margin.initial_short)) {
        return {Error::NotFinitePositive, Field::MarginInitial};
    }
    if (margin.maintenance_long && !positive(*margin.maintenance_long)) {
        return {Error::NotFinitePositive, Field::MarginMaintenance};
    }
    if (margin.maintenance_short && !positive(*margin.maintenance_short)) {
        return {Error::NotFinitePositive, Field::MarginMaintenance};
    }
    // A side that waives the opening requirement must state what it does
    // enforce. Zero initial AND no maintenance is a side that declares
    // nothing at all, which is a configuration mistake, never a silent
    // "unlimited leverage, never liquidated".
    if ((margin.initial_long == 0.0 && !margin.maintenance_long)
        || (margin.initial_short == 0.0 && !margin.maintenance_short)) {
        return {Error::MarginSideUndeclared, Field::MarginInitial};
    }
    if (!valid_liquidation_sizing(margin.sizing)) {
        return {Error::UnknownLiquidationSizing, Field::MarginSizing};
    }
    if (!positive(margin.shortfall_multiple)) {
        return {Error::NotFinitePositive, Field::MarginShortfallMultiple};
    }
    if (margin.liquidation_min_units && !positive(*margin.liquidation_min_units)) {
        return {Error::NotFinitePositive, Field::MarginMinUnits};
    }
    if (!valid_liquidation_check(margin.check)) {
        return {Error::UnknownLiquidationCheck, Field::MarginCheck};
    }
    if (!valid_margin_equity_basis(margin.basis)) {
        return {Error::UnknownMarginEquityBasis, Field::MarginEquityBasis};
    }
    if (!valid_liquidation_level_base(margin.level_base)) {
        return {Error::UnknownLiquidationLevelBase, Field::MarginLevelBase};
    }
    return {};
}

bool valid_risk_day(NativeRiskDay day) noexcept {
    switch (day) {
    case NativeRiskDay::SessionDay:
    case NativeRiskDay::CalendarDayInTimezone:
        return true;
    }
    return false;
}

bool valid_risk_action(NativeRiskAction action) noexcept {
    switch (action) {
    case NativeRiskAction::BlockOpenings:
    case NativeRiskAction::FlattenAndBlock:
        return true;
    }
    return false;
}

// A declared block with no limit at all is legal and inert: presence is the
// opt-in, and the run keeps its own risk identity. Every declared limit must
// be a usable threshold, so a nonpositive loss value and a zero count are
// both named failures rather than a permanently blocked run.
Result validate_risk(const NativeRunSpec& spec) noexcept {
    if (!spec.risk) return {};
    const auto& risk = *spec.risk;
    if (risk.max_drawdown && !positive(risk.max_drawdown->value)) {
        return {Error::NotFinitePositive, Field::RiskDrawdown};
    }
    if (risk.max_intraday_loss && !positive(risk.max_intraday_loss->value)) {
        return {Error::NotFinitePositive, Field::RiskIntradayLoss};
    }
    if (risk.max_consecutive_loss_days && *risk.max_consecutive_loss_days == 0) {
        return {Error::ZeroRiskLimit, Field::RiskLossDays};
    }
    if (risk.max_fills_per_day && *risk.max_fills_per_day == 0) {
        return {Error::ZeroRiskLimit, Field::RiskFillsPerDay};
    }
    if (!valid_risk_day(risk.day_basis)) {
        return {Error::UnknownRiskDay, Field::RiskDayBasis};
    }
    if (!valid_risk_action(risk.action)) {
        return {Error::UnknownRiskAction, Field::RiskAction};
    }
    return {};
}

bool valid_legacy_tolerance(NativeFeedTolerance tolerance) noexcept {
    constexpr std::uint32_t kKnown =
        static_cast<std::uint32_t>(NativeFeedTolerance::BatchStructuralBars)
        | static_cast<std::uint32_t>(NativeFeedTolerance::WarmupNonNegativeOHLC);
    const auto bits = static_cast<std::uint32_t>(tolerance);
    return (bits & ~kKnown) == 0u;
}

Result validate_values(const NativeRunSpec& spec) noexcept {
    const bool require_timeframes = !spec.timeframe_undetected;
    const struct {
        const std::string& value;
        Field field;
        bool required;
    } strings[] = {
        {spec.identity.session_key, Field::SessionKey, true},
        {spec.input_tf, Field::InputTimeframe, require_timeframes},
        {spec.script_tf, Field::ScriptTimeframe, require_timeframes},
        {spec.ticker, Field::Ticker, false},
        {spec.tickerid, Field::TickerId, true},
        {spec.type, Field::Type, false},
        {spec.currency, Field::Currency, false},
        {spec.basecurrency, Field::BaseCurrency, false},
        {spec.description, Field::Description, false},
        {spec.volumetype, Field::VolumeType, false},
        {spec.timezone, Field::Timezone, true},
        {spec.session, Field::Session, false},
        {spec.chart_timezone, Field::ChartTimezone, false},
    };
    for (const auto& value : strings) {
        const auto result = validate_string(value.value, value.field, value.required);
        if (!result) return result;
    }
    if (spec.timeframe_undetected
        && (!spec.input_tf.empty() || !spec.script_tf.empty() || spec.intrabar.lower())) {
        return {Error::InvalidUndetectedTimeframe, Field::TimeframeUndetected};
    }
    if (!valid_slot_label_policy(spec.slot_label_policy)) {
        return {Error::UnknownSlotLabelPolicy, Field::SlotLabelPolicy};
    }
    if (!valid_legacy_tolerance(spec.legacy_tolerance)) {
        return {Error::UnknownLegacyTolerance, Field::LegacyTolerance};
    }
    if (!valid_path_order(spec.path_order)) {
        return {Error::UnknownPathOrder, Field::PathOrder};
    }
    if (spec.identity.run_number == 0) return {Error::ZeroRunNumber, Field::RunNumber};
    const struct { double value; Field field; } financial[] = {
        {spec.initial_capital, Field::InitialCapital},
        {spec.point_value, Field::PointValue},
        {spec.account_fx, Field::AccountFx},
    };
    for (const auto& value : financial) {
        if (!positive(value.value)) return {Error::NotFinitePositive, value.field};
    }
    // A38: zero is the explicit unquantized-price sentinel. Preserve either
    // zero sign for the exact-bit spec hash; only nonfinite and negative
    // values are invalid.
    if (!std::isfinite(spec.price_tick) || spec.price_tick < 0.0)
        return {Error::NotFinitePositive, Field::PriceTick};
    if (!valid_price_grid(spec.price_grid))
        return {Error::UnknownPriceGrid, Field::PriceGrid};
    if (!valid_grid_rounding(spec.grid_rounding))
        return {Error::UnknownGridRounding, Field::GridRounding};
    // L8: a quantizing grid needs a real tick ladder. price_tick == 0 keeps
    // its documented unquantized meaning instead of silently disabling the
    // grid the host asked for.
    if (spec.price_grid != NativePriceGrid::None && !(spec.price_tick > 0.0))
        return {Error::GridRequiresPriceTick, Field::PriceGrid};
    if (spec.slippage_ticks > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        return {Error::SlippageOutOfRange, Field::SlippageTicks};
    switch (spec.fee_kind) {
    case NativeFeeKind::Percent:
    case NativeFeeKind::CashPerUnit:
    case NativeFeeKind::CashPerExecution:
        break;
    default:
        return {Error::UnknownFeeKind, Field::FeeKind};
    }
    if (!std::isfinite(spec.fee_value) || spec.fee_value < 0.0)
        return {Error::NotFiniteNonnegative, Field::FeeValue};
    if (spec.quantity_grid && !positive(*spec.quantity_grid))
        return {Error::NotFinitePositive, Field::QuantityGrid};
    switch (spec.close_execution) {
    case NativeCloseExecution::NextEligiblePoint:
    case NativeCloseExecution::AfterCalculation:
        break;
    default:
        return {Error::UnknownCloseExecution, Field::CloseExecution};
    }
    switch (spec.abort_reporting) {
    case NativeAbortReporting::Error:
    case NativeAbortReporting::Quiet:
        break;
    default:
        return {Error::UnknownAbortReporting, Field::AbortReporting};
    }
    if (spec.max_abs_units && !positive(*spec.max_abs_units))
        return {Error::NotFinitePositive, Field::MaxAbsUnits};
    if (spec.max_open_lots && *spec.max_open_lots == 0)
        return {Error::ZeroLotLimit, Field::MaxOpenLots};
    switch (spec.allowed_open_directions) {
    case NativeOpenDirections::None:
    case NativeOpenDirections::Long:
    case NativeOpenDirections::Short:
    case NativeOpenDirections::Both:
        break;
    default:
        return {Error::UnknownOpenDirections, Field::AllowedOpenDirections};
    }
    if (spec.initial_margin_fraction && !positive(*spec.initial_margin_fraction))
        return {Error::NotFinitePositive, Field::InitialMarginFraction};
    if (const auto margin = validate_margin(spec); !margin) return margin;
    if (const auto risk = validate_risk(spec); !risk) return risk;
    if (!valid_report_policy(spec.report_policy))
        return {Error::UnknownReportPolicy, Field::ReportPolicy};
    // L5 calculation timing. Every bound is legal, including zero: a host may
    // ask for the fills to be delivered without ever driving a recalculation.
    if (!valid_calculation_trigger(spec.calculation))
        return {Error::UnknownCalculationTrigger, Field::Calculation};
    if (!valid_open_bar_view(spec.open_bar_view))
        return {Error::UnknownOpenBarView, Field::OpenBarView};
    if (const auto feed =
            auxiliary_feed_shapes(spec.auxiliary_feed, spec.timeframe_undetected);
        !feed) {
        return feed;
    }
    if (const auto series =
            subscription_shapes(spec.subscriptions, spec.timeframe_undetected,
                                spec.auxiliary_feed.has_value());
        !series) {
        return series;
    }
    if (spec.intrabar.value.index() > 2) {
        return {Error::InvalidIntrabarPath, Field::IntrabarTimeframe};
    }
    if (const auto* lower = spec.intrabar.lower()) {
        const auto tf = validate_string(lower->tf, Field::IntrabarTimeframe, true);
        if (!tf) return tf;
        if (lower->samples < 2 || lower->samples > (1 << 20)) {
            return {Error::InvalidIntrabarPath, Field::IntrabarSamples};
        }
        if (!valid_distribution(lower->distribution)) {
            return {Error::InvalidIntrabarPath, Field::IntrabarDistribution};
        }
        if (!valid_sample_eligibility(lower->sample_eligibility)) {
            return {Error::UnknownIntrabarSampleEligibility,
                    Field::IntrabarSampleEligibility};
        }
        if (lower->volume_weighted_min_samples < 2
            || lower->volume_weighted_max_samples < lower->volume_weighted_min_samples
            || lower->volume_weighted_max_samples > (1 << 20)) {
            return {Error::InvalidIntrabarPath, Field::IntrabarVolumeSamples};
        }
    }
    if (const auto* synthesized = spec.intrabar.synthesized_path()) {
        if (synthesized->samples < 2 || synthesized->samples > (1 << 20)) {
            return {Error::InvalidIntrabarPath, Field::IntrabarSamples};
        }
        if (!valid_distribution(synthesized->distribution)) {
            return {Error::InvalidIntrabarPath, Field::IntrabarDistribution};
        }
        if (synthesized->volume_weighted_min_samples < 2
            || synthesized->volume_weighted_max_samples
                < synthesized->volume_weighted_min_samples
            || synthesized->volume_weighted_max_samples > (1 << 20)) {
            return {Error::InvalidIntrabarPath, Field::IntrabarVolumeSamples};
        }
    }
    return {};
}

} // namespace

NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec& spec) noexcept {
    const auto values = validate_values(spec);
    if (!values) return values;

    Field active_field = Field::InputTimeframe;
    try {
        if (!spec.timeframe_undetected) {
            const auto input = native_calendar::parse_timeframe(spec.input_tf);
            if (!input) return {Error::InvalidTimeframe, active_field};
            active_field = Field::ScriptTimeframe;
            const auto script = native_calendar::parse_timeframe(spec.script_tf);
            if (!script) return {Error::InvalidTimeframe, active_field};
            if (const auto* lower = spec.intrabar.lower()) {
                active_field = Field::IntrabarTimeframe;
                const auto path_tf = native_calendar::parse_timeframe(lower->tf);
                if (!path_tf) return {Error::InvalidIntrabarPath, active_field};
                switch (native_calendar::compatibility(*path_tf, *script).pairing) {
                case native_calendar::TimeframePairing::Passthrough:
                case native_calendar::TimeframePairing::SameUnitMultiple:
                case native_calendar::TimeframePairing::FixedDivisible:
                case native_calendar::TimeframePairing::FixedToCalendar:
                case native_calendar::TimeframePairing::CalendarToCalendar:
                    break;
                default:
                    return {Error::InvalidIntrabarPath, active_field};
                }
            }
            // Configure admits the complete batch contract, including monthly.
            // The host must apply stream_compatibility separately at stream begin.
            switch (native_calendar::compatibility(*input, *script).pairing) {
            case native_calendar::TimeframePairing::Passthrough:
            case native_calendar::TimeframePairing::SameUnitMultiple:
            case native_calendar::TimeframePairing::FixedDivisible:
            case native_calendar::TimeframePairing::FixedToCalendar:
            case native_calendar::TimeframePairing::CalendarToCalendar:
                break;
            default:
                return {Error::IncompatibleTimeframes, active_field};
            }
            std::optional<native_calendar::Timeframe> auxiliary;
            if (spec.auxiliary_feed) {
                active_field = Field::AuxiliaryFeedTimeframe;
                if (const auto feed =
                        auxiliary_feed_pairing(*spec.auxiliary_feed, *input, auxiliary);
                    !feed) {
                    return feed;
                }
            }
            active_field = Field::SubscriptionTimeframe;
            if (const auto series = subscription_pairings(
                    spec.subscriptions, *input, auxiliary ? &*auxiliary : nullptr);
                !series) {
                return series;
            }
        }
        // Use calendar's timezone acceptance with an all-day literal first,
        // so malformed session syntax has its own stable failure field.
        active_field = Field::Timezone;
        if (!native_calendar::parse_session("", spec.timezone))
            return {Error::UnresolvedTimezone, active_field};
        active_field = Field::Session;
        if (!native_calendar::parse_session(spec.session, spec.timezone))
            return {Error::InvalidSession, active_field};
        active_field = Field::ChartTimezone;
        if (!spec.chart_timezone.empty()
            && !native_calendar::parse_session("", spec.chart_timezone))
            return {Error::UnresolvedTimezone, active_field};
        return {};
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailure, active_field};
    } catch (...) {
        return {Error::CalendarFailure, active_field};
    }
}

NativeRunSpecValidation normalize_native_run_spec(NativeRunSpec& spec) noexcept {
    const auto result = validate_native_run_spec(spec);
    if (!result) return result;
    if (spec.fee_value == 0.0) spec.fee_value = 0.0;
    return {};
}

std::uint64_t native_intrabar_path_digest(const IntrabarPath& path) noexcept {
    std::uint64_t state = 1469598103934665603ULL;
    const auto bytes = [&state](const void* data, std::size_t count) noexcept {
        const auto* values = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            state ^= values[i];
            state *= 1099511628211ULL;
        }
    };
    const auto u = [&bytes](std::uint64_t value) noexcept { bytes(&value, sizeof value); };
    const auto i = [&bytes](std::int64_t value) noexcept { bytes(&value, sizeof value); };
    const auto d = [&bytes](double value) noexcept { bytes(&value, sizeof value); };
    const auto s = [&u, &bytes](const std::string& value) noexcept {
        u(value.size());
        bytes(value.data(), value.size());
    };
    u(path.value.index());
    if (const auto* lower = path.lower()) {
        s(lower->tf);
        i(lower->samples);
        u(static_cast<std::uint64_t>(lower->distribution));
        u(lower->volume_weighted ? 1u : 0u);
        i(lower->volume_weighted_min_samples);
        i(lower->volume_weighted_max_samples);
        u(static_cast<std::uint64_t>(lower->sample_eligibility));
        u(lower->bars.size());
        for (const auto& bar : lower->bars) {
            d(bar.open); d(bar.high); d(bar.low); d(bar.close); d(bar.volume); i(bar.timestamp);
        }
    } else if (const auto* synthesized = path.synthesized_path()) {
        i(synthesized->samples);
        u(static_cast<std::uint64_t>(synthesized->distribution));
        u(synthesized->volume_weighted ? 1u : 0u);
        i(synthesized->volume_weighted_min_samples);
        i(synthesized->volume_weighted_max_samples);
    }
    return state;
}

std::uint64_t native_margin_model_digest(const NativeMarginModel& margin) noexcept {
    std::uint64_t state = 1469598103934665603ULL;
    const auto bytes = [&state](const void* data, std::size_t count) noexcept {
        const auto* values = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            state ^= values[i];
            state *= 1099511628211ULL;
        }
    };
    const auto u = [&bytes](std::uint64_t value) noexcept { bytes(&value, sizeof value); };
    const auto d = [&bytes](double value) noexcept { bytes(&value, sizeof value); };
    const auto o = [&u, &d](const std::optional<double>& value) noexcept {
        u(value.has_value() ? 1u : 0u);
        if (value) d(*value);
    };
    d(margin.initial_long);
    d(margin.initial_short);
    o(margin.maintenance_long);
    o(margin.maintenance_short);
    u(static_cast<std::uint64_t>(margin.sizing));
    d(margin.shortfall_multiple);
    o(margin.liquidation_min_units);
    u(static_cast<std::uint64_t>(margin.check));
    // The two money bases fold only where a host moved one of them off the
    // marked-equity model, so a margin model declared before they existed
    // digests to exactly the same number it did then (the same conditional
    // shape hash_spec uses for the model as a whole).
    if (margin.basis != NativeMarginEquityBasis::MarkedEquity
        || margin.level_base != NativeLiquidationLevelBase::MarkedEquity) {
        u(static_cast<std::uint64_t>(margin.basis));
        u(static_cast<std::uint64_t>(margin.level_base));
    }
    // Same conditional shape for the broker's own ticket names: a model that
    // leaves them empty keeps the number it had before they existed.
    if (!margin.liquidation_label.empty() || !margin.liquidation_comment.empty()) {
        u(margin.liquidation_label.size());
        bytes(margin.liquidation_label.data(), margin.liquidation_label.size());
        u(margin.liquidation_comment.size());
        bytes(margin.liquidation_comment.data(), margin.liquidation_comment.size());
    }
    return state;
}

std::uint64_t native_risk_limits_digest(const NativeRiskLimits& risk) noexcept {
    std::uint64_t state = 1469598103934665603ULL;
    const auto bytes = [&state](const void* data, std::size_t count) noexcept {
        const auto* values = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            state ^= values[i];
            state *= 1099511628211ULL;
        }
    };
    const auto u = [&bytes](std::uint64_t value) noexcept { bytes(&value, sizeof value); };
    const auto d = [&bytes](double value) noexcept { bytes(&value, sizeof value); };
    const auto loss = [&u, &d](const std::optional<NativeLossLimit>& value) noexcept {
        u(value.has_value() ? 1u : 0u);
        if (!value) return;
        d(value->value);
        u(value->percent ? 1u : 0u);
    };
    const auto count = [&u](const std::optional<std::uint32_t>& value) noexcept {
        u(value.has_value() ? 1u : 0u);
        if (value) u(*value);
    };
    loss(risk.max_drawdown);
    loss(risk.max_intraday_loss);
    count(risk.max_consecutive_loss_days);
    count(risk.max_fills_per_day);
    u(static_cast<std::uint64_t>(risk.day_basis));
    u(static_cast<std::uint64_t>(risk.action));
    return state;
}

NativeRunSpecValidation validate_native_timeframe_subscriptions(
        const std::vector<NativeTimeframeSubscription>& subscriptions,
        const std::string& input_tf, bool timeframe_undetected) noexcept {
    return validate_native_timeframe_subscriptions(subscriptions, input_tf,
                                                   timeframe_undetected, std::nullopt);
}

NativeRunSpecValidation validate_native_timeframe_subscriptions(
        const std::vector<NativeTimeframeSubscription>& subscriptions,
        const std::string& input_tf, bool timeframe_undetected,
        const std::optional<NativeAuxiliaryFeed>& auxiliary_feed) noexcept {
    const auto feed = validate_native_auxiliary_feed(auxiliary_feed, input_tf,
                                                     timeframe_undetected);
    if (!feed) return feed;
    const auto shapes = subscription_shapes(subscriptions, timeframe_undetected,
                                            auxiliary_feed.has_value());
    if (!shapes) return shapes;
    if (subscriptions.empty() || timeframe_undetected) return {};
    try {
        const auto input = native_calendar::parse_timeframe(input_tf);
        if (!input) return {Error::InvalidTimeframe, Field::InputTimeframe};
        std::optional<native_calendar::Timeframe> auxiliary;
        if (auxiliary_feed) auxiliary = native_calendar::parse_timeframe(auxiliary_feed->tf);
        return subscription_pairings(subscriptions, *input,
                                     auxiliary ? &*auxiliary : nullptr);
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailure, Field::SubscriptionTimeframe};
    } catch (...) {
        return {Error::CalendarFailure, Field::SubscriptionTimeframe};
    }
}

NativeRunSpecValidation validate_native_auxiliary_feed(
        const std::optional<NativeAuxiliaryFeed>& auxiliary_feed,
        const std::string& input_tf, bool timeframe_undetected) noexcept {
    const auto shapes = auxiliary_feed_shapes(auxiliary_feed, timeframe_undetected);
    if (!shapes) return shapes;
    if (!auxiliary_feed) return {};
    try {
        const auto input = native_calendar::parse_timeframe(input_tf);
        if (!input) return {Error::InvalidTimeframe, Field::InputTimeframe};
        std::optional<native_calendar::Timeframe> parsed;
        return auxiliary_feed_pairing(*auxiliary_feed, *input, parsed);
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailure, Field::AuxiliaryFeedTimeframe};
    } catch (...) {
        return {Error::CalendarFailure, Field::AuxiliaryFeedTimeframe};
    }
}

std::uint64_t native_timeframe_subscriptions_digest(
        const std::vector<NativeTimeframeSubscription>& subscriptions) noexcept {
    std::uint64_t state = 1469598103934665603ULL;
    const auto bytes = [&state](const void* data, std::size_t count) noexcept {
        const auto* values = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            state ^= values[i];
            state *= 1099511628211ULL;
        }
    };
    const auto u = [&bytes](std::uint64_t value) noexcept { bytes(&value, sizeof value); };
    const auto i = [&bytes](std::int64_t value) noexcept { bytes(&value, sizeof value); };
    const auto d = [&bytes](double value) noexcept { bytes(&value, sizeof value); };
    const auto s = [&u, &bytes](const std::string& value) noexcept {
        u(value.size());
        bytes(value.data(), value.size());
    };
    u(subscriptions.size());
    for (const auto& subscription : subscriptions) {
        s(subscription.tf);
        u(subscription.lookahead ? 1u : 0u);
        // Gaps folds only where a series set it — the same conditional shape
        // the spec fold uses for its own opt-in blocks — so a series declared
        // before this field existed keeps the digest it already had.
        if (subscription.gaps) u(2u);
        // The series source folds the same way: only where a series left the
        // input, so every series declared before the field existed — and
        // every one still built from the input — keeps its digest.
        if (subscription.source != NativeSeriesSource::Input) {
            u(3u);
            u(static_cast<std::uint64_t>(subscription.source));
        }
        u(subscription.authoritative_bars.size());
        for (const auto& bar : subscription.authoritative_bars) {
            d(bar.open); d(bar.high); d(bar.low); d(bar.close); d(bar.volume);
            i(bar.timestamp);
        }
    }
    return state;
}

std::uint64_t native_auxiliary_feed_digest(const NativeAuxiliaryFeed& feed) noexcept {
    std::uint64_t state = 1469598103934665603ULL;
    const auto bytes = [&state](const void* data, std::size_t count) noexcept {
        const auto* values = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) {
            state ^= values[i];
            state *= 1099511628211ULL;
        }
    };
    const auto u = [&bytes](std::uint64_t value) noexcept { bytes(&value, sizeof value); };
    const auto i = [&bytes](std::int64_t value) noexcept { bytes(&value, sizeof value); };
    const auto d = [&bytes](double value) noexcept { bytes(&value, sizeof value); };
    u(feed.tf.size());
    bytes(feed.tf.data(), feed.tf.size());
    u(feed.bars.size());
    for (const auto& bar : feed.bars) {
        d(bar.open); d(bar.high); d(bar.low); d(bar.close); d(bar.volume);
        i(bar.timestamp);
    }
    return state;
}

}  // inline namespace native_run_spec_v3
} // namespace pineforge
