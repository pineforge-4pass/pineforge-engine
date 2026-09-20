#include <pineforge/native_run_spec.hpp>
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
    case NativeSlotLabelPolicy::LegacyTolerant:
        return true;
    }
    return false;
}

bool valid_report_policy(NativeReportPolicy policy) noexcept {
    switch (policy) {
    case NativeReportPolicy::HostRecorded:
    case NativeReportPolicy::KernelRecorded:
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
// -1 for any "*M"). Two declared series that share this key could not own
// their own authoritative bars, so the spec refuses them. Overflow-safe: a
// count whose duration is not representable answers 0, which never matches a
// valid key and leaves the pairing check to name the failure.
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
    if (!positive(margin.initial_long) || !positive(margin.initial_short)) {
        return {Error::NotFinitePositive, Field::MarginInitial};
    }
    if (margin.maintenance_long && !positive(*margin.maintenance_long)) {
        return {Error::NotFinitePositive, Field::MarginMaintenance};
    }
    if (margin.maintenance_short && !positive(*margin.maintenance_short)) {
        return {Error::NotFinitePositive, Field::MarginMaintenance};
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
    return {};
}

bool valid_legacy_tolerance(NativeLegacyTolerance tolerance) noexcept {
    constexpr std::uint32_t kKnown =
        static_cast<std::uint32_t>(NativeLegacyTolerance::BatchStructuralBars)
        | static_cast<std::uint32_t>(NativeLegacyTolerance::WarmupNonNegativeOHLC);
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
    if (!valid_report_policy(spec.report_policy))
        return {Error::UnknownReportPolicy, Field::ReportPolicy};
    if (!spec.subscriptions.empty() && spec.timeframe_undetected) {
        return {Error::SubscriptionWithoutTimeframe, Field::SubscriptionTimeframe};
    }
    for (const auto& subscription : spec.subscriptions) {
        const auto tf =
            validate_string(subscription.tf, Field::SubscriptionTimeframe, true);
        if (!tf) return tf;
        const auto& bars = subscription.authoritative_bars;
        for (std::size_t i = 1; i < bars.size(); ++i) {
            if (bars[i].timestamp <= bars[i - 1].timestamp) {
                return {Error::UnorderedSubscriptionBars, Field::SubscriptionBars};
            }
        }
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
            // Declared higher-timeframe series pair with the INPUT timeframe
            // exactly as script_tf does. A strictly finer request is named
            // separately: it is a lower-timeframe array contract, never a
            // silently promoted aggregate.
            active_field = Field::SubscriptionTimeframe;
            std::vector<std::int64_t> keys;
            keys.reserve(spec.subscriptions.size());
            for (const auto& subscription : spec.subscriptions) {
                const auto requested = native_calendar::parse_timeframe(subscription.tf);
                if (!requested) {
                    return {Error::InvalidSubscriptionTimeframe, active_field};
                }
                switch (native_calendar::compatibility(*input, *requested).pairing) {
                case native_calendar::TimeframePairing::Passthrough:
                case native_calendar::TimeframePairing::SameUnitMultiple:
                case native_calendar::TimeframePairing::FixedDivisible:
                case native_calendar::TimeframePairing::FixedToCalendar:
                case native_calendar::TimeframePairing::CalendarToCalendar:
                    break;
                case native_calendar::TimeframePairing::ScriptFiner:
                    return {Error::SubscriptionFinerThanInput, active_field};
                default:
                    return {Error::InvalidSubscriptionTimeframe, active_field};
                }
                const std::int64_t key = subscription_period_key(*requested);
                for (const std::int64_t seen : keys) {
                    if (seen == key) {
                        return {Error::DuplicateSubscriptionTimeframe, active_field};
                    }
                }
                keys.push_back(key);
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
    return state;
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
        u(subscription.authoritative_bars.size());
        for (const auto& bar : subscription.authoritative_bars) {
            d(bar.open); d(bar.high); d(bar.low); d(bar.close); d(bar.volume);
            i(bar.timestamp);
        }
    }
    return state;
}

}  // inline namespace native_run_spec_v3
} // namespace pineforge
