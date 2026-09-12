#include <pineforge/native_run_spec.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

// Deterministically exercise a dependency allocation failure without relying
// on huge allocations or exhausting the machine. This executable is serial.
static bool refuse_allocation = false;
void* operator new(std::size_t size) {
    if (refuse_allocation) throw std::bad_alloc{};
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {
using namespace pineforge;
using Error = NativeRunSpecError;
using Field = NativeRunSpecField;
int checks = 0;
int failures = 0;

void check(bool pass, const char* what) {
    ++checks;
    if (!pass) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

NativeRunSpec complete_spec() {
    NativeRunSpec spec;
    spec.identity = {u8"native/台北/é/😀", 1};
    spec.input_tf = "1";
    spec.script_tf = "5";
    spec.ticker = "XYZ";
    spec.tickerid = "TEST:XYZ";
    spec.type = "futures";
    spec.currency = "USD";
    spec.basecurrency = "XYZ";
    spec.description = u8"Native test — 交易";
    spec.volumetype = "contracts";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "Asia/Taipei";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 6.0;
    return spec;
}

// Snapshot every semantic field, including exact floating-point object bytes
// and optional presence. No struct padding or NaN equality is involved.
template<class T>
void append(std::string& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void append(std::string& out, const std::string& value) {
    append(out, value.size());
    out += value;
}
template<class T>
void append(std::string& out, const std::optional<T>& value) {
    append(out, value.has_value());
    if (value) append(out, *value);
}
std::string snapshot(const NativeRunSpec& s) {
    std::string out;
    append(out, s.identity.session_key); append(out, s.identity.run_number);
    append(out, s.input_tf); append(out, s.script_tf);
    append(out, s.ticker); append(out, s.tickerid); append(out, s.type);
    append(out, s.currency); append(out, s.basecurrency);
    append(out, s.description); append(out, s.volumetype);
    append(out, s.timezone); append(out, s.session); append(out, s.chart_timezone);
    append(out, s.initial_capital); append(out, s.point_value);
    append(out, s.account_fx); append(out, s.price_tick);
    append(out, s.slippage_ticks); append(out, s.fee_kind); append(out, s.fee_value);
    append(out, s.quantity_grid); append(out, s.close_execution);
    append(out, s.max_abs_units); append(out, s.max_open_lots);
    append(out, s.allowed_open_directions); append(out, s.initial_margin_fraction);
    return out;
}

void expect_refusal(NativeRunSpec spec, Error error, Field field) {
    const auto before = snapshot(spec);
    const auto validation = validate_native_run_spec(spec);
    if (validation.error != error || validation.field != field) {
        std::cerr << "Refusal expected error=" << static_cast<unsigned>(error)
                  << " field=" << static_cast<unsigned>(field)
                  << "; got error=" << static_cast<unsigned>(validation.error)
                  << " field=" << static_cast<unsigned>(validation.field) << '\n';
    }
    check(validation.error == error, "validation error code");
    check(validation.field == field, "validation error field");
    check(snapshot(spec) == before, "const validation preserves every input bit");
    const auto normalization = normalize_native_run_spec(spec);
    check(normalization.error == error, "normalization error code");
    check(normalization.field == field, "normalization error field");
    check(snapshot(spec) == before, "refused normalization preserves every input bit");
}

void expect_acceptance(NativeRunSpec spec) {
    const auto before = snapshot(spec);
    check(validate_native_run_spec(spec).ok(), "valid complete spec admitted");
    check(snapshot(spec) == before, "validation does not normalize");
    check(normalize_native_run_spec(spec).ok(), "valid complete spec normalized");
    check(snapshot(spec) == before, "nonzero/canonical spec values preserved");
}

void strings_and_identity() {
    expect_acceptance(complete_spec());
    expect_refusal({}, Error::EmptyRequiredString, Field::SessionKey);
    auto spec = complete_spec();
    spec.identity.run_number = 0;
    expect_refusal(spec, Error::ZeroRunNumber, Field::RunNumber);
    spec.identity.run_number = std::numeric_limits<std::uint64_t>::max();
    expect_acceptance(spec); // Host high-water is a separate begin obligation.

    const struct {
        std::string NativeRunSpec::* member;
        Field field;
        bool required;
    } fields[] = {
        {&NativeRunSpec::input_tf, Field::InputTimeframe, true},
        {&NativeRunSpec::script_tf, Field::ScriptTimeframe, true},
        {&NativeRunSpec::ticker, Field::Ticker, false},
        {&NativeRunSpec::tickerid, Field::TickerId, true},
        {&NativeRunSpec::type, Field::Type, false},
        {&NativeRunSpec::currency, Field::Currency, false},
        {&NativeRunSpec::basecurrency, Field::BaseCurrency, false},
        {&NativeRunSpec::description, Field::Description, false},
        {&NativeRunSpec::volumetype, Field::VolumeType, false},
        {&NativeRunSpec::timezone, Field::Timezone, true},
        {&NativeRunSpec::session, Field::Session, false},
        {&NativeRunSpec::chart_timezone, Field::ChartTimezone, false},
    };
    for (const auto& field : fields) {
        spec = complete_spec();
        (spec.*field.member).clear();
        if (field.required) expect_refusal(spec, Error::EmptyRequiredString, field.field);
        else expect_acceptance(spec);
        spec = complete_spec();
        spec.fee_value = -0.0;
        (spec.*field.member).append("\0suffix", 7);
        expect_refusal(spec, Error::EmbeddedNul, field.field);
        spec = complete_spec();
        spec.fee_value = -0.0;
        (spec.*field.member).append("\xC0\xAF", 2);
        expect_refusal(spec, Error::InvalidUtf8, field.field);
    }
    spec = complete_spec();
    spec.identity.session_key.clear();
    expect_refusal(spec, Error::EmptyRequiredString, Field::SessionKey);
    spec.identity.session_key = std::string("run\0other", 9);
    expect_refusal(spec, Error::EmbeddedNul, Field::SessionKey);
    // Required means nonempty, without an invented trim/ASCII restriction.
    spec.identity.session_key = "   ";
    expect_acceptance(spec);
    const std::string invalid[] = {
        "\x80", "\xBF", "\xC0\x80", "\xC1\xBF", "\xC2", "\xC2x",
        "\xE0\x80\x80", "\xE0\xA0", "\xED\xA0\x80", "\xED\xBF\xBF",
        "\xF0\x80\x80\x80", "\xF0\x90\x80", "\xF4\x90\x80\x80",
        "\xF5\x80\x80\x80", "\xF8\x88\x80\x80\x80", "\xFF",
    };
    for (const auto& bad : invalid) {
        spec = complete_spec();
        spec.identity.session_key = bad;
        expect_refusal(spec, Error::InvalidUtf8, Field::SessionKey);
    }
    // Valid scalar boundaries, including a Unicode noncharacter; no NFC fold.
    for (const std::string good : {"\x7F", "\xC2\x80", "\xDF\xBF",
            "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80",
            "\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF",
            "e\xCC\x81"}) {
        spec = complete_spec();
        spec.identity.session_key = good;
        expect_acceptance(spec);
    }
}

void financial_values_and_options() {
    const double invalid[] = {0.0, -0.0, -1.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::nan("1234")};
    const double positive[] = {std::numeric_limits<double>::denorm_min(), 0.125,
        1.0, std::numeric_limits<double>::max()};
    const struct { double NativeRunSpec::* member; Field field; } fields[] = {
        {&NativeRunSpec::initial_capital, Field::InitialCapital},
        {&NativeRunSpec::point_value, Field::PointValue},
        {&NativeRunSpec::account_fx, Field::AccountFx},
        {&NativeRunSpec::price_tick, Field::PriceTick},
    };
    for (const auto& field : fields) {
        for (double value : invalid) {
            auto spec = complete_spec();
            spec.fee_value = -0.0;
            spec.*field.member = value;
            expect_refusal(spec, Error::NotFinitePositive, field.field);
        }
        for (double value : positive) {
            auto spec = complete_spec();
            spec.*field.member = value;
            expect_acceptance(spec); // No invented arithmetic/cap restriction.
        }
    }
    const struct {
        std::optional<double> NativeRunSpec::* member;
        Field field;
    } options[] = {
        {&NativeRunSpec::quantity_grid, Field::QuantityGrid},
        {&NativeRunSpec::max_abs_units, Field::MaxAbsUnits},
        {&NativeRunSpec::initial_margin_fraction, Field::InitialMarginFraction},
    };
    for (const auto& option : options) {
        for (double value : invalid) {
            auto spec = complete_spec();
            spec.fee_value = -0.0;
            spec.*option.member = value;
            expect_refusal(spec, Error::NotFinitePositive, option.field);
        }
        for (double value : positive) {
            auto spec = complete_spec();
            spec.*option.member = value;
            expect_acceptance(spec);
        }
    }
    auto spec = complete_spec();
    spec.max_open_lots = 0;
    spec.fee_value = -0.0;
    expect_refusal(spec, Error::ZeroLotLimit, Field::MaxOpenLots);
    spec.fee_value = 6.0;
    spec.max_open_lots = std::numeric_limits<std::uint64_t>::max();
    expect_acceptance(spec);
    spec = complete_spec();
    spec.quantity_grid = 0.3;
    spec.max_abs_units = 0.2;
    spec.max_open_lots = 1;
    spec.initial_margin_fraction = 1.5;
    spec.allowed_open_directions = NativeOpenDirections::None;
    expect_acceptance(spec); // A setup may disallow opening; no resizing policy.

    for (auto fee : {NativeFeeKind::Percent, NativeFeeKind::CashPerUnit,
                     NativeFeeKind::CashPerExecution}) {
        for (double value : {-1.0, std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(), std::nan("99")}) {
            spec = complete_spec(); spec.fee_kind = fee; spec.fee_value = value;
            expect_refusal(spec, Error::NotFiniteNonnegative, Field::FeeValue);
        }
        for (double value : {0.0, 150.0, std::numeric_limits<double>::denorm_min(),
                              std::numeric_limits<double>::max()}) {
            spec = complete_spec(); spec.fee_kind = fee; spec.fee_value = value;
            expect_acceptance(spec);
        }
        spec = complete_spec(); spec.fee_kind = fee; spec.fee_value = -0.0;
        check(validate_native_run_spec(spec).ok(), "fee negative zero admitted");
        check(std::signbit(spec.fee_value), "validation preserves negative zero");
        auto expected = spec; expected.fee_value = 0.0;
        check(normalize_native_run_spec(spec).ok(), "negative-zero normalization succeeds");
        check(!std::signbit(spec.fee_value), "fee canonical zero is positive");
        check(snapshot(spec) == snapshot(expected), "only admitted zero changes");
        expect_acceptance(spec); // Normalization is idempotent.
    }
    for (std::uint32_t unknown : {3u, 4u, std::numeric_limits<std::uint32_t>::max()}) {
        spec = complete_spec(); spec.fee_kind = static_cast<NativeFeeKind>(unknown);
        expect_refusal(spec, Error::UnknownFeeKind, Field::FeeKind);
    }
    for (std::uint32_t value : {0u, 1u, 2u, 3u}) {
        spec = complete_spec();
        spec.allowed_open_directions = static_cast<NativeOpenDirections>(value);
        expect_acceptance(spec);
    }
    for (std::uint32_t unknown : {4u, 7u, std::numeric_limits<std::uint32_t>::max()}) {
        spec = complete_spec();
        spec.allowed_open_directions = static_cast<NativeOpenDirections>(unknown);
        expect_refusal(spec, Error::UnknownOpenDirections, Field::AllowedOpenDirections);
    }
    for (auto close : {NativeCloseExecution::NextEligiblePoint,
                       NativeCloseExecution::AfterCalculation}) {
        spec = complete_spec(); spec.close_execution = close; expect_acceptance(spec);
    }
    for (std::uint32_t unknown : {2u, std::numeric_limits<std::uint32_t>::max()}) {
        spec = complete_spec(); spec.close_execution = static_cast<NativeCloseExecution>(unknown);
        expect_refusal(spec, Error::UnknownCloseExecution, Field::CloseExecution);
    }
    spec = complete_spec();
    spec.slippage_ticks = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    expect_acceptance(spec);
    ++spec.slippage_ticks;
    expect_refusal(spec, Error::SlippageOutOfRange, Field::SlippageTicks);
    spec.slippage_ticks = std::numeric_limits<std::uint32_t>::max();
    expect_refusal(spec, Error::SlippageOutOfRange, Field::SlippageTicks);
}

void complete_clock_contract() {
    const std::pair<const char*, const char*> allowed[] = {
        {"1S", "1S"}, {"15S", "1"}, {"1", "60S"}, {"1", "5"},
        {"1", "D"}, {"1", "W"}, {"1", "3M"},
        {"D", "D"}, {"D", "1D"}, {"1D", "D"}, {"2D", "2D"},
        {"D", "2D"}, {"W", "W"}, {"2W", "2W"}, {"W", "2W"},
        {"M", "M"}, {"3M", "3M"}, {"M", "3M"},
        {"D", "W"}, {"2D", "W"}, {"D", "M"}, {"W", "M"}, {"2W", "3M"},
    };
    for (const auto& pair : allowed) {
        auto spec = complete_spec(); spec.input_tf = pair.first; spec.script_tf = pair.second;
        expect_acceptance(spec); // Monthly is batch-valid; no stream guard here.
    }
    for (const char* literal : {"", "0", "01", "01D", "S", "0S", "1H", "1m",
            " 1", "1 ", "1\t", "1.0", "+1", "-1", "1DD", "2147483648",
            "999999999999999999999999999999999999999999999999999999999999999"}) {
        auto spec = complete_spec(); spec.input_tf = literal;
        expect_refusal(spec, literal[0] ? Error::InvalidTimeframe : Error::EmptyRequiredString,
                       Field::InputTimeframe);
        spec = complete_spec(); spec.script_tf = literal;
        expect_refusal(spec, literal[0] ? Error::InvalidTimeframe : Error::EmptyRequiredString,
                       Field::ScriptTimeframe);
    }
    const std::pair<const char*, const char*> denied[] = {
        {"5", "1"}, {"5", "7"}, {"45S", "1"}, {"2D", "3D"},
        {"2W", "W"}, {"3M", "M"}, {"W", "D"}, {"M", "W"}, {"D", "60"},
    };
    for (const auto& pair : denied) {
        auto spec = complete_spec(); spec.input_tf = pair.first; spec.script_tf = pair.second;
        spec.fee_value = -0.0;
        expect_refusal(spec, Error::IncompatibleTimeframes, Field::ScriptTimeframe);
    }
    for (const char* session : {"", "24x7", "0000-2400", "1700-1700", "1800-1700:23456",
            "0930-1130,1300-1600:23456", "1300-1600,0900-1500", "0230-0245"}) {
        auto spec = complete_spec(); spec.session = session; spec.timezone = "America/New_York";
        expect_acceptance(spec);
    }
    for (const char* session : {"bad", "2400-0100", "0930-2500", "0930-1600:0",
            "0930-1600:8", "0930-1600:", "0930-1600,", "0930-1600,,1300-1400"}) {
        auto spec = complete_spec(); spec.session = session; spec.fee_value = -0.0;
        expect_refusal(spec, Error::InvalidSession, Field::Session);
    }
    for (const char* zone : {"UTC", "Etc/UTC", "America/New_York", "Asia/Taipei",
                            "UTC+05:30", "GMT-4"}) {
        auto spec = complete_spec(); spec.timezone = zone; spec.chart_timezone = zone;
        expect_acceptance(spec);
    }
    // These are required contract checks, even while a frozen calendar
    // dependency is known to accept invalid names as libc's fallback UTC.
    for (const char* zone : {"No/Such_PineForge_Zone", "UTC+24:00", "UTC+01:99", " "}) {
        auto spec = complete_spec(); spec.timezone = zone; spec.fee_value = -0.0;
        expect_refusal(spec, Error::UnresolvedTimezone, Field::Timezone);
        spec = complete_spec(); spec.chart_timezone = zone; spec.fee_value = -0.0;
        expect_refusal(spec, Error::UnresolvedTimezone, Field::ChartTimezone);
    }
}

void failure_atomicity() {
    auto spec = complete_spec();
    spec.fee_value = -0.0;
    // Force parse_session's literal/string allocation beyond any SSO size.
    spec.session = std::string(200, ' ') + "0930-1600";
    const auto before = snapshot(spec);
    refuse_allocation = true;
    const auto validation = validate_native_run_spec(spec);
    const auto normalization = normalize_native_run_spec(spec);
    refuse_allocation = false;
    check(validation.error == Error::AllocationFailure, "allocation failure is typed");
    check(normalization.error == Error::AllocationFailure, "normalization allocation failure is typed");
    check(validation.field == Field::Session, "allocation failure names calendar stage");
    check(normalization.field == Field::Session, "normalization names failed calendar stage");
    check(snapshot(spec) == before, "allocation refusal preserves all values and negative zero");
    check(normalize_native_run_spec(spec).ok(), "same value normalizes after resources recover");
    check(!std::signbit(spec.fee_value), "recovered normalization canonicalizes zero");

    // Validate all late fields before touching early fields, including zeros.
    spec = complete_spec(); spec.fee_value = -0.0;
    spec.initial_margin_fraction = std::nan("42");
    expect_refusal(spec, Error::NotFinitePositive, Field::InitialMarginFraction);
}
} // namespace

int main() {
    strings_and_identity();
    financial_values_and_options();
    complete_clock_contract();
    failure_atomicity();
    std::cout << (checks - failures) << '/' << checks << " checks passed; "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
