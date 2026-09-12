// SPDX-License-Identifier: Apache-2.0
#include "native_startup.hpp"
#include "transport.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/market_driver.hpp>
#include <pineforge/native_calendar.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pineforge::live {
namespace {

constexpr std::size_t kMaxFrame = 1024 * 1024;

bool line(std::istream& in, std::string& out) {
    out.clear();
    char c;
    while (in.get(c)) {
        if (c == '\n')
            return true;
        out += c;
        if (out.size() > kMaxFrame)
            throw std::runtime_error("input line exceeds 1 MiB");
    }
    if (in.bad())
        throw std::runtime_error("feed read failed");
    return !out.empty();
}

std::string require_text(const Json& obj, const char* key) {
    auto value = obj.at(key).text();
    if (value.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("native config string contains embedded NUL: ") + key);
    if (!semantic_utf8(value))
        throw std::runtime_error(std::string("native config string is not valid UTF-8: ") + key);
    return value;
}

double require_real(const Json& obj, const char* key) {
    double value = obj.at(key).real();
    if (value == 0.0)
        value = 0.0;
    return value;
}

const char* spec_error(pineforge::NativeRunSpecError error) {
    switch (error) {
    case pineforge::NativeRunSpecError::EmptyRequiredString:
        return "native spec has an empty required string";
    case pineforge::NativeRunSpecError::EmbeddedNul:
        return "native spec string contains embedded NUL";
    case pineforge::NativeRunSpecError::InvalidUtf8:
        return "native spec string is not valid UTF-8";
    case pineforge::NativeRunSpecError::ZeroRunNumber:
        return "native spec run_number must be positive";
    case pineforge::NativeRunSpecError::InvalidTimeframe:
        return "native spec timeframe is invalid";
    case pineforge::NativeRunSpecError::IncompatibleTimeframes:
        return "native spec timeframes are incompatible";
    case pineforge::NativeRunSpecError::UnresolvedTimezone:
        return "native spec timezone is unresolved";
    case pineforge::NativeRunSpecError::InvalidSession:
        return "native spec session is invalid";
    case pineforge::NativeRunSpecError::NotFinitePositive:
        return "native spec requires a finite positive number";
    case pineforge::NativeRunSpecError::SlippageOutOfRange:
        return "native spec slippage_ticks exceeds native integer range";
    case pineforge::NativeRunSpecError::UnknownFeeKind:
        return "unknown native fee_kind";
    case pineforge::NativeRunSpecError::NotFiniteNonnegative:
        return "native spec requires a finite nonnegative number";
    case pineforge::NativeRunSpecError::UnknownCloseExecution:
        return "unknown native close_execution";
    case pineforge::NativeRunSpecError::UnknownOpenDirections:
        return "unknown native allowed_open_directions";
    case pineforge::NativeRunSpecError::ZeroLotLimit:
        return "native spec max_open_lots must be positive when present";
    case pineforge::NativeRunSpecError::AllocationFailure:
        return "native spec validation allocation failed";
    case pineforge::NativeRunSpecError::CalendarFailure:
        return "native spec calendar validation failed";
    case pineforge::NativeRunSpecError::None:
        return "native spec is valid";
    }
    return "native spec is invalid";
}

Json identity_null() {
    Json j;
    j.kind = Json::Kind::Null;
    return j;
}

std::optional<std::string> read_resource_bytes(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) > 8ULL * 1024 * 1024) {
        ::close(fd);
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::read(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0)
            break;
        off += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (off == 0)
        return std::nullopt;
    bytes.resize(off);
    return bytes;
}

const char* timezone_kind_name(pineforge::native_calendar::TimezoneSourceKind kind) {
    using Kind = pineforge::native_calendar::TimezoneSourceKind;
    switch (kind) {
    case Kind::Utc:
        return "utc";
    case Kind::FixedOffset:
        return "fixed-offset";
    case Kind::PosixExplicit:
        return "posix-explicit";
    case Kind::PosixDefaultDst:
        return "posix-default-dst";
    case Kind::Tzfile:
        return "tzfile";
    }
    return "unknown";
}

}  // namespace

bool semantic_utf8(std::string_view bytes) {
    std::uint32_t value = 0, minimum = 0;
    unsigned remaining = 0;
    for (const unsigned char byte : bytes) {
        if (remaining) {
            if ((byte & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (byte & 0x3f);
            if (--remaining == 0 &&
                (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)))
                return false;
        } else if (byte <= 0x7f)
            continue;
        else if (byte >= 0xc2 && byte <= 0xdf) {
            value = byte & 0x1f;
            minimum = 0x80;
            remaining = 1;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            value = byte & 0x0f;
            minimum = 0x800;
            remaining = 2;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            value = byte & 0x07;
            minimum = 0x10000;
            remaining = 3;
        } else
            return false;
    }
    return remaining == 0;
}

Json json_real(double value) {
    if (!std::isfinite(value))
        throw std::runtime_error("nonfinite native identity number");
    if (value == 0.0)
        value = 0.0;
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(17) << value;
    return Json::number(s.str());
}

Json json_u64(std::uint64_t value) { return Json::number(std::to_string(value)); }

NativeConfigValues parse_native_config(const std::string& text) {
    auto root = parse_json(text);
    only_fields(root, {"run", "clock", "instrument", "execution"});
    const auto& run = root.at("run");
    const auto& clock = root.at("clock");
    const auto& instrument = root.at("instrument");
    const auto& execution = root.at("execution");
    only_fields(run, {"session_key", "run_number"});
    only_fields(clock, {"input_tf", "script_tf", "timezone", "session", "chart_timezone"});
    only_fields(instrument, {"ticker", "tickerid", "type", "currency", "basecurrency", "description",
                             "volumetype"});
    only_fields(execution, {"initial_capital", "point_value", "account_fx", "price_tick",
                            "slippage_ticks", "fee_kind", "fee_value", "quantity_grid",
                            "close_execution", "max_abs_units", "max_open_lots",
                            "allowed_open_directions", "initial_margin_fraction"});
    NativeConfigValues n;
    n.present = true;
    n.session_key = require_text(run, "session_key");
    n.run_number = run.at("run_number").integer<std::uint64_t>();
    n.input_tf = require_text(clock, "input_tf");
    n.script_tf = require_text(clock, "script_tf");
    n.timezone = require_text(clock, "timezone");
    n.session = require_text(clock, "session");
    n.chart_timezone = require_text(clock, "chart_timezone");
    n.ticker = require_text(instrument, "ticker");
    n.tickerid = require_text(instrument, "tickerid");
    n.type = require_text(instrument, "type");
    n.currency = require_text(instrument, "currency");
    n.basecurrency = require_text(instrument, "basecurrency");
    n.description = require_text(instrument, "description");
    n.volumetype = require_text(instrument, "volumetype");
    n.initial_capital = require_real(execution, "initial_capital");
    n.point_value = require_real(execution, "point_value");
    n.account_fx = require_real(execution, "account_fx");
    n.price_tick = require_real(execution, "price_tick");
    n.slippage_ticks = execution.at("slippage_ticks").integer<std::uint32_t>();
    if (n.slippage_ticks > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("native spec slippage_ticks exceeds native integer range");
    auto fee = require_text(execution, "fee_kind");
    if (fee == "Percent")
        n.fee_kind = 0;
    else if (fee == "CashPerUnit")
        n.fee_kind = 1;
    else if (fee == "CashPerExecution")
        n.fee_kind = 2;
    else
        throw std::runtime_error("unknown native fee_kind");
    n.fee_value = require_real(execution, "fee_value");
    auto close = require_text(execution, "close_execution");
    if (close == "NextEligiblePoint")
        n.close_execution = 0;
    else if (close == "AfterCalculation")
        n.close_execution = 1;
    else
        throw std::runtime_error("unknown native close_execution");
    n.allowed_open_directions = execution.at("allowed_open_directions").integer<std::uint32_t>();
    const auto optional_field = [&](const char* key, auto apply) {
        if (!execution.members.count(key))
            throw std::runtime_error(std::string("missing field ") + key);
        if (execution.members.at(key).kind == Json::Kind::Null)
            return;
        apply();
    };
    optional_field("quantity_grid", [&] {
        n.quantity_grid = require_real(execution, "quantity_grid");
        n.optional_mask |= 1u;
    });
    optional_field("max_abs_units", [&] {
        n.max_abs_units = require_real(execution, "max_abs_units");
        n.optional_mask |= 2u;
    });
    optional_field("initial_margin_fraction", [&] {
        n.initial_margin_fraction = require_real(execution, "initial_margin_fraction");
        n.optional_mask |= 4u;
    });
    optional_field("max_open_lots", [&] {
        n.max_open_lots = execution.at("max_open_lots").integer<std::uint64_t>();
        n.optional_mask |= 8u;
    });
    return n;
}

void apply_native_config(NativeClockBindings& clock, const NativeConfigValues& native) {
    auto match = [&](const char* flag, const std::string& cli, const std::string& cfg) {
        if (clock.explicit_flags.count(flag) && cli != cfg)
            throw std::runtime_error(std::string("CLI ") + flag + " contradicts native-config");
    };
    match("--input-tf", clock.input_tf, native.input_tf);
    match("--script-tf", clock.script_tf, native.script_tf);
    match("--timezone", clock.timezone, native.timezone);
    match("--session", clock.session, native.session);
    match("--chart-timezone", clock.chart_timezone, native.chart_timezone);
    match("--symbol", clock.symbol, native.tickerid);
    if (!clock.explicit_flags.count("--input-tf"))
        clock.input_tf = native.input_tf;
    if (!clock.explicit_flags.count("--script-tf"))
        clock.script_tf = native.script_tf;
    if (!clock.explicit_flags.count("--timezone"))
        clock.timezone = native.timezone;
    if (!clock.explicit_flags.count("--session"))
        clock.session = native.session;
    if (!clock.explicit_flags.count("--chart-timezone"))
        clock.chart_timezone = native.chart_timezone;
    if (!clock.explicit_flags.count("--symbol"))
        clock.symbol = native.tickerid;
}

pineforge::NativeRunSpec native_run_spec(const NativeConfigValues& native) {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = native.session_key;
    spec.identity.run_number = native.run_number;
    spec.input_tf = native.input_tf;
    spec.script_tf = native.script_tf;
    spec.ticker = native.ticker;
    spec.tickerid = native.tickerid;
    spec.type = native.type;
    spec.currency = native.currency;
    spec.basecurrency = native.basecurrency;
    spec.description = native.description;
    spec.volumetype = native.volumetype;
    spec.timezone = native.timezone;
    spec.session = native.session;
    spec.chart_timezone = native.chart_timezone;
    spec.initial_capital = native.initial_capital;
    spec.point_value = native.point_value;
    spec.account_fx = native.account_fx;
    spec.price_tick = native.price_tick;
    spec.slippage_ticks = native.slippage_ticks;
    spec.fee_kind = static_cast<pineforge::NativeFeeKind>(native.fee_kind);
    spec.fee_value = native.fee_value;
    spec.close_execution = static_cast<pineforge::NativeCloseExecution>(native.close_execution);
    spec.allowed_open_directions =
        static_cast<pineforge::NativeOpenDirections>(native.allowed_open_directions);
    if (native.optional_mask & 1u)
        spec.quantity_grid = native.quantity_grid;
    if (native.optional_mask & 2u)
        spec.max_abs_units = native.max_abs_units;
    if (native.optional_mask & 4u)
        spec.initial_margin_fraction = native.initial_margin_fraction;
    if (native.optional_mask & 8u)
        spec.max_open_lots = native.max_open_lots;
    return spec;
}

void validate_native_config(NativeConfigValues& native) {
    if (!native.input_tf.empty() && native.input_tf.back() == 'M')
        throw std::runtime_error("native stream refuses monthly input");
    auto spec = native_run_spec(native);
    const auto result = pineforge::normalize_native_run_spec(spec);
    if (!result)
        throw std::runtime_error(spec_error(result.error));
    native.fee_value = spec.fee_value;
    const auto input = pineforge::native_calendar::parse_timeframe(native.input_tf);
    const auto script = pineforge::native_calendar::parse_timeframe(native.script_tf);
    if (!input || !script)
        throw std::runtime_error("native spec timeframe is invalid");
    const auto stream = pineforge::native_calendar::stream_compatibility(*input, *script);
    if (stream.pairing == pineforge::native_calendar::TimeframePairing::StreamMonthlyInputRefused)
        throw std::runtime_error("native stream refuses monthly input");
    switch (stream.pairing) {
    case pineforge::native_calendar::TimeframePairing::Passthrough:
    case pineforge::native_calendar::TimeframePairing::SameUnitMultiple:
    case pineforge::native_calendar::TimeframePairing::FixedDivisible:
    case pineforge::native_calendar::TimeframePairing::FixedToCalendar:
    case pineforge::native_calendar::TimeframePairing::CalendarToCalendar:
        break;
    default:
        throw std::runtime_error("native spec timeframes are incompatible");
    }
}

std::vector<pf_bar_t> history(const std::string& csv, bool native) {
    std::istringstream in(csv);
    std::string row;
    if (!line(in, row))
        throw std::runtime_error("empty warmup");
    if (!row.empty() && row.back() == '\r')
        row.pop_back();
    if (row != "timestamp,open,high,low,close,volume")
        throw std::runtime_error("warmup CSV header must be timestamp,open,high,low,close,volume");
    std::vector<pf_bar_t> bars;
    while (line(in, row)) {
        if (!row.empty() && row.back() == '\r')
            row.pop_back();
        if (row.empty())
            throw std::runtime_error("blank warmup row");
        std::vector<std::string> cells;
        std::size_t begin = 0;
        for (;;) {
            auto end = row.find(',', begin);
            cells.push_back(row.substr(begin, end - begin));
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        if (cells.size() != 6)
            throw std::runtime_error("warmup requires six CSV columns");
        auto stamp = parse_json(cells[0]).integer<std::int64_t>();
        pf_bar_t b{};
        b.timestamp = stamp;
        b.open = parse_json(cells[1]).real();
        b.high = parse_json(cells[2]).real();
        b.low = parse_json(cells[3]).real();
        b.close = parse_json(cells[4]).real();
        b.volume = parse_json(cells[5]).real();
        if (stamp < 0 || b.low <= 0 || b.high < std::max(b.open, b.close) ||
            b.low > std::min(b.open, b.close) || b.high < b.low || b.volume < 0)
            throw std::runtime_error("invalid warmup bar");
        if (!native) {
            if (stamp % 60000 || stamp > INT64_MAX - 60000)
                throw std::runtime_error("invalid warmup bar");
            if (!bars.empty() && stamp != bars.back().timestamp + 60000)
                throw std::runtime_error("warmup must contain contiguous confirmed 1m bars");
        } else if (!bars.empty() && stamp <= bars.back().timestamp) {
            throw std::runtime_error("native warmup timestamps must be strictly increasing");
        }
        bars.push_back(b);
        if (bars.size() >= static_cast<std::size_t>(INT_MAX))
            throw std::runtime_error("too many warmup rows");
    }
    if (bars.empty())
        throw std::runtime_error("warmup requires at least one bar");
    return bars;
}

void require_native_warmup(const NativeConfigValues& spec, const std::vector<pf_bar_t>& bars) {
    if (bars.empty())
        throw std::runtime_error("warmup requires at least one bar");
    if (bars.size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("too many warmup rows");
    const auto input = pineforge::native_calendar::parse_timeframe(spec.input_tf);
    const auto script = pineforge::native_calendar::parse_timeframe(spec.script_tf);
    if (!input || !script)
        throw std::runtime_error("native spec timeframe is invalid");
    const auto stream = pineforge::native_calendar::stream_compatibility(*input, *script);
    if (stream.pairing == pineforge::native_calendar::TimeframePairing::StreamMonthlyInputRefused)
        throw std::runtime_error("native stream refuses monthly input");
    switch (stream.pairing) {
    case pineforge::native_calendar::TimeframePairing::Passthrough:
    case pineforge::native_calendar::TimeframePairing::SameUnitMultiple:
    case pineforge::native_calendar::TimeframePairing::FixedDivisible:
    case pineforge::native_calendar::TimeframePairing::FixedToCalendar:
    case pineforge::native_calendar::TimeframePairing::CalendarToCalendar:
        break;
    default:
        throw std::runtime_error("native spec timeframes are incompatible");
    }
    std::vector<pineforge::Bar> converted;
    converted.reserve(bars.size());
    for (const auto& raw : bars) {
        converted.push_back(
            pineforge::Bar{raw.open, raw.high, raw.low, raw.close, raw.volume, raw.timestamp});
    }
    const auto result = pineforge::preflight_native_inputs(
        native_run_spec(spec), converted.data(), static_cast<int>(converted.size()),
        pineforge::NativeInputPolicy::StreamWarmup);
    if (result.ok())
        return;
    switch (result.error) {
    case pineforge::NativeInputPreflightError::InSessionGap:
        throw std::runtime_error("native warmup has an in-session gap");
    case pineforge::NativeInputPreflightError::Unaligned:
    case pineforge::NativeInputPreflightError::OffGridLabel:
        throw std::runtime_error("native warmup bar is not aligned to the configured calendar");
    case pineforge::NativeInputPreflightError::OverlappingSlot:
        throw std::runtime_error("native warmup input intervals overlap");
    case pineforge::NativeInputPreflightError::NotStrictlyIncreasing:
        throw std::runtime_error("native warmup timestamps must be strictly increasing");
    case pineforge::NativeInputPreflightError::StructuralInvalid:
        throw std::runtime_error("invalid warmup bar");
    case pineforge::NativeInputPreflightError::CalendarFailure:
        throw std::runtime_error("native warmup calendar validation failed");
    default:
        throw std::runtime_error("native warmup preflight refused");
    }
}

Json timezone_rule_identity(std::string_view timezone, bool required) {
    if (timezone.empty()) {
        if (required)
            throw std::runtime_error("native spec timezone is unresolved");
        return identity_null();
    }
    if (!semantic_utf8(timezone) || timezone.find('\0') != std::string_view::npos)
        throw std::runtime_error("native spec timezone is unresolved");
    auto facts = pineforge::native_calendar::timezone_identity_descriptor(timezone);
    if (!facts || !facts->valid() ||
        facts->semantics_version !=
            pineforge::native_calendar::TimezoneIdentityDescriptor::kSemanticsVersion)
        throw std::runtime_error(
            "native timezone identity cannot be established: calendar resource facts are required");
    Json resources;
    resources.kind = Json::Kind::Array;
    for (const auto& path : facts->resource_paths) {
        auto bytes = read_resource_bytes(path);
        if (!bytes)
            throw std::runtime_error(
                "native timezone identity cannot be established: calendar resource is unreadable");
        resources.items.push_back(Json::object({{"sha256", Json::string(sha256_hex(*bytes))}}));
    }
    return Json::object({
        {"semantics_version", json_u64(facts->semantics_version)},
        {"kind", json_u64(static_cast<std::uint32_t>(facts->kind))},
        {"kind_name", Json::string(timezone_kind_name(facts->kind))},
        {"input", Json::string(facts->input)},
        {"effective_definition", Json::string(facts->effective_definition)},
        {"zoneinfo_root", facts->zoneinfo_root.empty() ? identity_null()
                                                       : Json::string(facts->zoneinfo_root)},
        {"resources", std::move(resources)},
    });
}

std::string identity_document(const LegacyIdentityFields& fields, const std::string& warmup,
                              const std::string& library, const std::string& parser_bytes,
                              const std::string& parser_config) {
    Json j = Json::object({{"schema", Json::string("pineforge-native-ledger/v1")},
                           {"library", Json::string(sha256_hex(library))},
                           {"warmup", Json::string(sha256_hex(warmup))},
                           {"mode", Json::string(fields.mode)},
                           {"input_tf", Json::string(fields.input_tf)},
                           {"script_tf", Json::string(fields.script_tf)},
                           {"session", Json::string(fields.session)},
                           {"timezone", Json::string(fields.timezone)},
                           {"chart_timezone", Json::string(fields.chart_timezone)},
                           {"symbol", Json::string(fields.symbol)},
                           {"name", Json::string(fields.name)},
                           {"webhook", Json::string(fields.webhook)},
                           {"parser", Json::string(sha256_hex(parser_bytes))},
                           {"parser_config", Json::string(sha256_hex(parser_config))}});
    Json input, overrides;
    input.kind = overrides.kind = Json::Kind::Array;
    for (const auto& [k, v] : fields.inputs)
        input.items.push_back(Json::object({{"key", Json::string(k)}, {"value", Json::string(v)}}));
    for (const auto& [k, v] : fields.overrides)
        overrides.items.push_back(
            Json::object({{"key", Json::string(k)}, {"value", Json::string(v)}}));
    Json metadata = Json::object({});
    for (const auto& [k, v] : fields.syminfo)
        metadata.members[k] = Json::string(v);
    j.members["syminfo"] = metadata;
    j.members["inputs"] = input;
    j.members["overrides"] = overrides;
    return j.dump();
}

std::string identity(const LegacyIdentityFields& fields, const std::string& warmup,
                     const std::string& library, const std::string& parser_bytes,
                     const std::string& parser_config) {
    return sha256_hex(identity_document(fields, warmup, library, parser_bytes, parser_config));
}

std::string native_identity_document(const NativeConfigValues& native, const std::string& mode,
                                     const std::string& name, const std::string& webhook,
                                     const std::string& warmup, const std::string& library,
                                     const std::string& parser_bytes,
                                     const std::string& parser_config) {
    Json execution = Json::object({
        {"initial_capital", json_real(native.initial_capital)},
        {"point_value", json_real(native.point_value)},
        {"account_fx", json_real(native.account_fx)},
        {"price_tick", json_real(native.price_tick)},
        {"slippage_ticks", json_u64(native.slippage_ticks)},
        {"fee_kind", json_u64(native.fee_kind)},
        {"fee_value", json_real(native.fee_value)},
        {"close_execution", json_u64(native.close_execution)},
        {"allowed_open_directions", json_u64(native.allowed_open_directions)},
        {"quantity_grid", native.optional_mask & 1u ? json_real(native.quantity_grid) : identity_null()},
        {"max_abs_units", native.optional_mask & 2u ? json_real(native.max_abs_units) : identity_null()},
        {"initial_margin_fraction",
         native.optional_mask & 4u ? json_real(native.initial_margin_fraction) : identity_null()},
        {"max_open_lots", native.optional_mask & 8u ? json_u64(native.max_open_lots) : identity_null()},
    });
    Json j = Json::object({
        {"schema", Json::string("pineforge-native-run/v1")},
        {"consumer", Json::string(pineforge::kNativeConsumerSemanticVersion)},
        {"driver", Json::string(pineforge::kNativeDriverSemanticVersion)},
        {"calendar", Json::string(pineforge::kNativeCalendarSemanticVersion)},
        {"library", Json::string(sha256_hex(library))},
        {"warmup", Json::string(sha256_hex(warmup))},
        {"mode", Json::string(mode)},
        {"parser", Json::string(sha256_hex(parser_bytes))},
        {"parser_config", Json::string(sha256_hex(parser_config))},
        {"timezone_dependency", timezone_rule_identity(native.timezone, true)},
        {"chart_timezone_dependency", timezone_rule_identity(native.chart_timezone, false)},
        {"run", Json::object({{"session_key", Json::string(native.session_key)},
                              {"run_number", json_u64(native.run_number)}})},
        {"clock", Json::object({{"input_tf", Json::string(native.input_tf)},
                                {"script_tf", Json::string(native.script_tf)},
                                {"timezone", Json::string(native.timezone)},
                                {"session", Json::string(native.session)},
                                {"chart_timezone", Json::string(native.chart_timezone)}})},
        {"instrument",
         Json::object({{"ticker", Json::string(native.ticker)},
                       {"tickerid", Json::string(native.tickerid)},
                       {"type", Json::string(native.type)},
                       {"currency", Json::string(native.currency)},
                       {"basecurrency", Json::string(native.basecurrency)},
                       {"description", Json::string(native.description)},
                       {"volumetype", Json::string(native.volumetype)}})},
        {"execution", execution},
        {"name", Json::string(name)},
        {"webhook", Json::string(webhook)},
    });
    return j.dump();
}

std::string native_identity(const NativeConfigValues& native, const std::string& mode,
                            const std::string& name, const std::string& webhook,
                            const std::string& warmup, const std::string& library,
                            const std::string& parser_bytes, const std::string& parser_config) {
    return sha256_hex(native_identity_document(native, mode, name, webhook, warmup, library,
                                               parser_bytes, parser_config));
}

}  // namespace pineforge::live
