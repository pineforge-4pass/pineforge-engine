#include <pineforge/native_run_spec.hpp>
#include <pineforge/native_calendar.hpp>

#include <cmath>
#include <limits>
#include <new>
#include <string_view>

namespace pineforge {
inline namespace native_run_spec_v1 {
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

Result validate_values(const NativeRunSpec& spec) noexcept {
    const struct {
        const std::string& value;
        Field field;
        bool required;
    } strings[] = {
        {spec.identity.session_key, Field::SessionKey, true},
        {spec.input_tf, Field::InputTimeframe, true},
        {spec.script_tf, Field::ScriptTimeframe, true},
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
    if (spec.identity.run_number == 0) return {Error::ZeroRunNumber, Field::RunNumber};
    const struct { double value; Field field; } financial[] = {
        {spec.initial_capital, Field::InitialCapital},
        {spec.point_value, Field::PointValue},
        {spec.account_fx, Field::AccountFx},
        {spec.price_tick, Field::PriceTick},
    };
    for (const auto& value : financial) {
        if (!positive(value.value)) return {Error::NotFinitePositive, value.field};
    }
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
    return {};
}

} // namespace

NativeRunSpecValidation validate_native_run_spec(const NativeRunSpec& spec) noexcept {
    const auto values = validate_values(spec);
    if (!values) return values;

    Field active_field = Field::InputTimeframe;
    try {
        const auto input = native_calendar::parse_timeframe(spec.input_tf);
        if (!input) return {Error::InvalidTimeframe, active_field};
        active_field = Field::ScriptTimeframe;
        const auto script = native_calendar::parse_timeframe(spec.script_tf);
        if (!script) return {Error::InvalidTimeframe, active_field};
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

}  // inline namespace native_run_spec_v1
} // namespace pineforge
