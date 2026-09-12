// Independent R1 v10 C1-C9 / D1-D5 witnesses through NativeStrategyHost.
// Calendar expectations are literal UTC epochs (including independently
// converted New York civil dates), never answers from the calendar under test.
// These small native fixtures do not claim campaign/Pine parity.

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
using native_order::AcceptedEvent;
using native_order::CancelledEvent;
using native_order::CommandEvent;
using native_order::ExecutionAppliedEvent;
using native_order::InvalidHandleEvent;
using native_order::MatchRejectedEvent;
using native_order::NoEffectEvent;
using native_order::NotWorkingEvent;
using native_order::RejectedEvent;
using native_order::ReplaceRejectedEvent;
using native_order::ReplacedEvent;
using native_order::Request;
using native_order::SubmitResult;
using native_order::SubmitStatus;
using order_action::Reduce;
using order_action::Transact;

int checks = 0;
int failures = 0;
int scenarios = 0;
const char* scenario = "initialization";

#define CHECK(expression)                                                     \
    do {                                                                      \
        ++checks;                                                             \
        if (!(expression)) {                                                  \
            ++failures;                                                       \
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,          \
                        #expression);                                         \
        }                                                                     \
    } while (false)

void near(double actual, double expected) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-10) {
        std::printf("[%s] actual %.17g expected %.17g\n", scenario, actual, expected);
    }
    CHECK(std::isfinite(actual) && std::abs(actual - expected) <= 1e-10);
}

Bar flat(int64_t timestamp, double price = 100.0, double volume = 1.0) {
    return Bar{price, price, price, price, volume, timestamp};
}

Request buy() { return Request{Transact{1.0}, "calendar-contract", ""}; }

bool proof_capture = false;
int next_host_ordinal = 0;

struct HostSnap {
    int ordinal = 0;
    std::string spec_json = "{}";
    std::string identity_json =
        "{\"sessionKey\":\"calendar-driver-contract\",\"runNumber\":1}";
    std::string calendar_json = "{}";
    std::vector<std::string> inputs;
    std::vector<std::string> lifecycle;
    std::vector<std::string> physical;
    std::string observations_json = "{}";
};

std::vector<HostSnap> group_snaps;

struct ScenarioArt {
    std::string id;
    std::string status = "passed";
    std::string native_configuration = "{}";
    std::string run_identity =
        "{\"sessionKey\":\"calendar-driver-contract\",\"runNumber\":1}";
    std::string calendar = "{}";
    std::string logical_inputs = "[]";
    std::string lifecycle_events = "[]";
    std::string physical_effects = "[]";
    std::string observations = "{}";
    std::string comparisons = "[]";
};

std::vector<ScenarioArt> arts;

std::string json_escape(const std::string& s) {
    std::string out;
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

std::string json_num(double x) {
    std::ostringstream out;
    out << std::setprecision(17) << x;
    return out.str();
}

std::string json_i64(int64_t x) {
    std::ostringstream out;
    out << x;
    return out.str();
}

std::string json_u64(uint64_t x) {
    std::ostringstream out;
    out << x;
    return out.str();
}

std::string json_bool(bool v) { return v ? "true" : "false"; }

std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

uint64_t f64_bits(double x) {
    uint64_t bits = 0;
    std::memcpy(&bits, &x, sizeof(bits));
    return bits;
}

std::string unsupported_number(double x) {
    const char* tag = "nonfinite";
    if (std::isnan(x)) tag = "NaN";
    else if (std::isinf(x)) tag = x > 0 ? "+Inf" : "-Inf";
    std::ostringstream out;
    out << "{\"unsupported\":" << json_escape(tag)
        << ",\"ieee754Bits\":" << json_escape(hex64(f64_bits(x))) << "}";
    return out.str();
}

void append_f64(std::ostringstream& out, const char* key, double x) {
    out << json_escape(key) << ":";
    if (std::isfinite(x)) out << json_num(x);
    else out << unsupported_number(x);
}

std::string json_array(const std::vector<std::string>& parts) {
    std::string out = "[";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ",";
        out += parts[i];
    }
    out += "]";
    return out;
}

std::string flatten_items(const std::vector<HostSnap>& hosts,
                          std::vector<std::string> HostSnap::*field) {
    std::vector<std::string> all;
    for (const auto& host : hosts) {
        const auto& items = host.*field;
        all.insert(all.end(), items.begin(), items.end());
    }
    return json_array(all);
}

bool is_sha256_hex(const char* s) {
    if (!s) return false;
    std::size_t n = 0;
    for (; s[n]; ++n) {
        const char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return n == 64;
}

std::string identity_json(const NativeRunSpec& spec) {
    std::ostringstream out;
    out << "{\"sessionKey\":" << json_escape(spec.identity.session_key)
        << ",\"runNumber\":" << json_u64(spec.identity.run_number) << "}";
    return out.str();
}

std::string calendar_json(const NativeRunSpec& spec, int host_ordinal) {
    std::ostringstream out;
    out << "{\"hostOrdinal\":" << host_ordinal
        << ",\"timezone\":" << json_escape(spec.timezone)
        << ",\"session\":" << json_escape(spec.session)
        << ",\"inputTf\":" << json_escape(spec.input_tf)
        << ",\"scriptTf\":" << json_escape(spec.script_tf)
        << ",\"chartTimezone\":" << json_escape(spec.chart_timezone) << "}";
    return out.str();
}

void append_optional_f64(std::ostringstream& out, const char* key,
                         const std::optional<double>& value) {
    out << "," << json_escape(key) << ":";
    if (!value) out << "null";
    else if (std::isfinite(*value)) out << json_num(*value);
    else out << unsupported_number(*value);
}

std::string spec_config_json(const NativeRunSpec& spec, int host_ordinal) {
    std::ostringstream out;
    out << "{\"hostOrdinal\":" << host_ordinal
        << ",\"sessionKey\":" << json_escape(spec.identity.session_key)
        << ",\"runNumber\":" << json_u64(spec.identity.run_number)
        << ",\"inputTf\":" << json_escape(spec.input_tf)
        << ",\"scriptTf\":" << json_escape(spec.script_tf)
        << ",\"ticker\":" << json_escape(spec.ticker)
        << ",\"tickerid\":" << json_escape(spec.tickerid)
        << ",\"type\":" << json_escape(spec.type)
        << ",\"currency\":" << json_escape(spec.currency)
        << ",\"basecurrency\":" << json_escape(spec.basecurrency)
        << ",\"description\":" << json_escape(spec.description)
        << ",\"volumetype\":" << json_escape(spec.volumetype)
        << ",\"timezone\":" << json_escape(spec.timezone)
        << ",\"session\":" << json_escape(spec.session)
        << ",\"chartTimezone\":" << json_escape(spec.chart_timezone)
        << ",";
    append_f64(out, "initialCapital", spec.initial_capital);
    out << ",";
    append_f64(out, "pointValue", spec.point_value);
    out << ",";
    append_f64(out, "accountFx", spec.account_fx);
    out << ",";
    append_f64(out, "priceTick", spec.price_tick);
    out << ",\"slippageTicks\":" << json_u64(spec.slippage_ticks)
        << ",\"feeKind\":" << json_u64(static_cast<uint64_t>(spec.fee_kind))
        << ",";
    append_f64(out, "feeValue", spec.fee_value);
    append_optional_f64(out, "quantityGrid", spec.quantity_grid);
    out << ",\"closeExecution\":"
        << json_u64(static_cast<uint64_t>(spec.close_execution));
    append_optional_f64(out, "maxAbsUnits", spec.max_abs_units);
    out << ",\"maxOpenLots\":";
    if (!spec.max_open_lots) out << "null";
    else out << json_u64(*spec.max_open_lots);
    out << ",\"allowedOpenDirections\":"
        << json_u64(static_cast<uint64_t>(spec.allowed_open_directions));
    append_optional_f64(out, "initialMarginFraction", spec.initial_margin_fraction);
    out << "}";
    return out.str();
}

std::string interval_json(const native_calendar::NativeInterval& interval) {
    std::ostringstream out;
    out << "{\"openMs\":" << json_i64(interval.open_ms)
        << ",\"eligibleOpenMs\":" << json_i64(interval.eligible_open_ms)
        << ",\"lastTradedCloseMs\":" << json_i64(interval.last_traded_close_ms)
        << ",\"nextPeriodOpenMs\":" << json_i64(interval.next_period_open_ms)
        << ",\"nextInputOpenMs\":" << json_i64(interval.next_input_open_ms) << "}";
    return out.str();
}

std::string coordinate_json(const NativeCoordinate& c) {
    std::ostringstream out;
    out << "{\"ordinal\":" << json_u64(c.ordinal)
        << ",\"intervalIndex\":" << c.interval_index
        << ",\"openMs\":" << json_i64(c.open_ms)
        << ",\"eligibleOpenMs\":" << json_i64(c.eligible_open_ms)
        << ",\"lastTradedCloseMs\":" << json_i64(c.last_traded_close_ms)
        << ",\"nextPeriodOpenMs\":" << json_i64(c.next_period_open_ms)
        << ",\"nextInputOpenMs\":" << json_i64(c.next_input_open_ms)
        << ",\"effectiveTimeMs\":" << json_i64(c.effective_time_ms)
        << ",\"sourcePriceTimeMs\":" << json_i64(c.source_price_time_ms)
        << ",\"provenance\":" << json_u64(static_cast<uint64_t>(c.provenance))
        << ",\"pathPhase\":" << json_u64(static_cast<uint64_t>(c.path_phase))
        << ",\"completion\":" << json_u64(static_cast<uint64_t>(c.completion)) << "}";
    return out.str();
}

void append_bar_fields(std::ostringstream& out, const Bar& bar) {
    append_f64(out, "open", bar.open);
    out << ",";
    append_f64(out, "high", bar.high);
    out << ",";
    append_f64(out, "low", bar.low);
    out << ",";
    append_f64(out, "close", bar.close);
    out << ",";
    append_f64(out, "volume", bar.volume);
    out << ",\"timestampMs\":" << json_i64(bar.timestamp);
}

std::string action_json(const Request& request) {
    return std::visit([](const auto& action) -> std::string {
        using T = std::decay_t<decltype(action)>;
        std::ostringstream out;
        if constexpr (std::is_same_v<T, Transact>) {
            out << "{\"type\":\"Transact\",";
            append_f64(out, "signedUnits", action.signed_units);
            out << "}";
        } else if constexpr (std::is_same_v<T, Reduce>) {
            out << "{\"type\":\"Reduce\",";
            append_f64(out, "units", action.units);
            out << "}";
        } else {
            out << "{\"type\":\"Flatten\"}";
        }
        return out.str();
    }, request.action);
}

const char* command_kind_name(const CommandEvent& event) {
    return std::visit([](const auto& payload) -> const char* {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, AcceptedEvent>) return "Accepted";
        if constexpr (std::is_same_v<T, RejectedEvent>) return "Rejected";
        if constexpr (std::is_same_v<T, ReplacedEvent>) return "Replaced";
        if constexpr (std::is_same_v<T, ReplaceRejectedEvent>) return "ReplaceRejected";
        if constexpr (std::is_same_v<T, CancelledEvent>) return "Cancelled";
        if constexpr (std::is_same_v<T, NotWorkingEvent>) return "NotWorking";
        if constexpr (std::is_same_v<T, InvalidHandleEvent>) return "InvalidHandle";
        if constexpr (std::is_same_v<T, NoEffectEvent>) return "NoEffect";
        if constexpr (std::is_same_v<T, MatchRejectedEvent>) return "MatchRejected";
        if constexpr (std::is_same_v<T, ExecutionAppliedEvent>) return "ExecutionApplied";
        return "Unknown";
    }, event);
}

uint64_t command_ordinal(const CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

double request_quantity(const Request& request) {
    return std::visit([](const auto& action) -> double {
        using T = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<T, Transact>) return action.signed_units;
        if constexpr (std::is_same_v<T, Reduce>) return action.units;
        return 0.0;
    }, request.action);
}

NativeRunSpec spec(const char* input = "1", const char* script = "1") {
    NativeRunSpec result;
    result.identity = {"calendar-driver-contract", 1};
    result.input_tf = input;
    result.script_tf = script;
    result.ticker = "CLOCK";
    result.tickerid = "TEST:CLOCK";
    result.type = "crypto";
    result.currency = "USD";
    result.basecurrency = "USD";
    result.description = "independent native calendar driver fixture";
    result.volumetype = "base";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 10000.0;
    result.point_value = 1.0;
    result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.fee_kind = NativeFeeKind::CashPerExecution;
    result.fee_value = 0.0;
    return result;
}

class TraceHost final : public NativeStrategyHost {
public:
    std::vector<Bar> bars;
    std::vector<NativeDecisionContext> contexts;
    std::vector<double> callback_positions;
    std::function<void(TraceHost&)> callback;

    TraceHost() {
        if (proof_capture) host_ordinal_ = next_host_ordinal++;
    }

    ~TraceHost() override { snapshot(); }

    NativeSetupResult configure_native(const NativeRunSpec& value) {
        attempted_spec_ = value;
        configured_ = true;
        const auto result = NativeStrategyHost::configure_native(value);
        setup_status_ = result.status;
        setup_error_ = static_cast<uint64_t>(result.validation.error);
        setup_field_ = static_cast<uint64_t>(result.validation.field);
        return result;
    }

    void run(const Bar* input, int n) {
        NativeStrategyHost::run(input, n);
        if (!proof_capture) return;
        const bool admitted = last_error().empty();
        record_bars("batchBar", input, n, admitted);
        record_operation("batchRun", n > 0 && input ? input[0].timestamp : 0, admitted,
                         "{\"barCount\":" + json_i64(n) + "}");
    }

    bool stream_begin(const Bar* warmup, int n, const std::string& input_tf,
                      const std::string& script_tf = "") {
        const bool ok = NativeStrategyHost::stream_begin(warmup, n, input_tf, script_tf);
        if (proof_capture) {
            record_bars("warmupBar", warmup, n, ok);
            std::ostringstream extra;
            extra << "{\"barCount\":" << n
                  << ",\"inputTf\":" << json_escape(input_tf)
                  << ",\"scriptTf\":" << json_escape(script_tf) << "}";
            record_operation("streamBegin", n > 0 && warmup ? warmup[0].timestamp : 0, ok,
                             extra.str());
        }
        return ok;
    }

    bool stream_push_bar(const Bar& bar) {
        const bool ok = NativeStrategyHost::stream_push_bar(bar);
        if (proof_capture) record_bar("pushBar", bar, ok);
        return ok;
    }

    bool stream_push_tick(const TradeTick& tick) {
        const bool ok = NativeStrategyHost::stream_push_tick(tick);
        if (proof_capture) record_tick("tick", tick, ok);
        return ok;
    }

    bool stream_push_ticks(const TradeTick* ticks, int n) {
        const bool ok = NativeStrategyHost::stream_push_ticks(ticks, n);
        if (proof_capture) record_tick_array(ticks, n, ok);
        return ok;
    }

    bool stream_advance_time(int64_t timestamp_ms) {
        const bool ok = NativeStrategyHost::stream_advance_time(timestamp_ms);
        if (proof_capture) {
            std::ostringstream out;
            out << "{\"kind\":\"advanceTime\",\"ordinal\":" << json_u64(input_ordinal_++)
                << ",\"effectiveTimeMs\":" << json_i64(timestamp_ms)
                << ",\"calendar\":{\"hostOrdinal\":" << host_ordinal_
                << ",\"admitted\":" << json_bool(ok) << "}}";
            input_items_.push_back(out.str());
        }
        return ok;
    }

    bool stream_end(bool finalize_partial_input_bar = false) {
        const bool ok = NativeStrategyHost::stream_end(finalize_partial_input_bar);
        if (proof_capture) {
            std::ostringstream out;
            out << "{\"kind\":\"streamEnd\",\"ordinal\":" << json_u64(input_ordinal_++)
                << ",\"calendar\":{\"hostOrdinal\":" << host_ordinal_
                << ",\"finalizePartial\":" << json_bool(finalize_partial_input_bar)
                << ",\"admitted\":" << json_bool(ok) << "}}";
            input_items_.push_back(out.str());
        }
        return ok;
    }

    SubmitResult submit_market(const Request& request) {
        const auto result = NativeStrategyHost::submit_market(request);
        if (proof_capture) {
            std::ostringstream out;
            out << "{\"kind\":\"request\",\"ordinal\":" << json_u64(input_ordinal_++);
            const double qty = request_quantity(request);
            if (std::isfinite(qty)) out << ",\"quantity\":" << json_num(qty);
            out << ",\"calendar\":{\"hostOrdinal\":" << host_ordinal_
                << ",\"label\":" << json_escape(request.label)
                << ",\"comment\":" << json_escape(request.comment)
                << ",\"action\":" << action_json(request)
                << ",\"admitted\":" << json_bool(result.status == SubmitStatus::Accepted)
                << ",\"submitStatus\":"
                << json_u64(static_cast<uint64_t>(result.status))
                << ",\"eventOrdinal\":" << json_u64(result.event_ordinal);
            if (!std::isfinite(qty)) {
                out << ",\"quantity\":" << unsupported_number(qty);
            }
            out << "}}";
            input_items_.push_back(out.str());
        }
        return result;
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        bars.push_back(bar);
        contexts.push_back(context);
        callback_positions.push_back(physical_position().signed_units);
        if (callback) callback(*this);
    }

    double runup() const { return open_trade_max_runup(0); }
    double drawdown() const { return open_trade_max_drawdown(0); }

private:
    int host_ordinal_ = 0;
    uint64_t input_ordinal_ = 0;
    bool configured_ = false;
    NativeSetupStatus setup_status_ = NativeSetupStatus::Failed;
    uint64_t setup_error_ = 0;
    uint64_t setup_field_ = 0;
    NativeRunSpec attempted_spec_{};
    std::vector<std::string> input_items_;

    void record_operation(const char* kind, int64_t time_ms, bool admitted,
                          const std::string& extra_object) {
        std::ostringstream out;
        out << "{\"kind\":" << json_escape(kind)
            << ",\"ordinal\":" << json_u64(input_ordinal_++)
            << ",\"effectiveTimeMs\":" << json_i64(time_ms)
            << ",\"calendar\":{\"hostOrdinal\":" << host_ordinal_
            << ",\"admitted\":" << json_bool(admitted);
        if (extra_object.size() >= 2 && extra_object.front() == '{' && extra_object.back() == '}') {
            const std::string inner = extra_object.substr(1, extra_object.size() - 2);
            if (!inner.empty()) out << "," << inner;
        }
        out << "}}";
        input_items_.push_back(out.str());
    }

    void record_bar(const char* kind, const Bar& bar, bool admitted) {
        std::ostringstream out;
        out << "{\"kind\":" << json_escape(kind)
            << ",\"ordinal\":" << json_u64(input_ordinal_++)
            << ",\"effectiveTimeMs\":" << json_i64(bar.timestamp);
        if (std::isfinite(bar.open)) out << ",\"price\":" << json_num(bar.open);
        if (std::isfinite(bar.volume)) out << ",\"quantity\":" << json_num(bar.volume);
        out << ",\"calendar\":{\"hostOrdinal\":" << host_ordinal_
            << ",\"admitted\":" << json_bool(admitted) << ",";
        append_bar_fields(out, bar);
        out << "}}";
        input_items_.push_back(out.str());
    }

    void record_bars(const char* kind, const Bar* bars, int n, bool admitted) {
        if (n <= 0 || !bars) return;
        for (int i = 0; i < n; ++i) record_bar(kind, bars[i], admitted);
    }

    void record_tick_body(std::ostringstream& out, const TradeTick& tick) {
        out << "\"effectiveTimeMs\":" << json_i64(tick.timestamp)
            << ",\"sequence\":" << json_u64(tick.sequence);
        if (std::isfinite(tick.price)) out << ",\"price\":" << json_num(tick.price);
        if (std::isfinite(tick.quantity)) out << ",\"quantity\":" << json_num(tick.quantity);
        out << ",\"calendar\":{\"hostOrdinal\":" << host_ordinal_;
        if (!std::isfinite(tick.price)) {
            out << ",\"price\":" << unsupported_number(tick.price);
        }
        if (!std::isfinite(tick.quantity)) {
            out << ",\"quantity\":" << unsupported_number(tick.quantity);
        }
        out << "}";
    }

    void record_tick(const char* kind, const TradeTick& tick, bool admitted) {
        std::ostringstream out;
        out << "{\"kind\":" << json_escape(kind)
            << ",\"ordinal\":" << json_u64(input_ordinal_++) << ",";
        record_tick_body(out, tick);
        // admitted is a logical-input fact; keep it inside calendar.
        std::string body = out.str();
        const auto insert_at = body.rfind('}');
        std::ostringstream full;
        full << body.substr(0, insert_at) << ",\"admitted\":" << json_bool(admitted)
             << body.substr(insert_at) << "}";
        input_items_.push_back(full.str());
    }

    void record_tick_array(const TradeTick* ticks, int n, bool admitted) {
        std::ostringstream out;
        out << "{\"kind\":\"tickArray\",\"ordinal\":" << json_u64(input_ordinal_++)
            << ",\"calendar\":{\"hostOrdinal\":" << host_ordinal_
            << ",\"admitted\":" << json_bool(admitted)
            << ",\"count\":" << n << ",\"ticks\":[";
        for (int i = 0; i < n; ++i) {
            if (i) out << ",";
            const TradeTick& tick = ticks[i];
            out << "{\"effectiveTimeMs\":" << json_i64(tick.timestamp)
                << ",\"sequence\":" << json_u64(tick.sequence);
            if (std::isfinite(tick.price)) out << ",\"price\":" << json_num(tick.price);
            else out << ",\"price\":" << unsupported_number(tick.price);
            if (std::isfinite(tick.quantity)) out << ",\"quantity\":" << json_num(tick.quantity);
            else out << ",\"quantity\":" << unsupported_number(tick.quantity);
            out << "}";
        }
        out << "]}}";
        input_items_.push_back(out.str());
    }

    std::string lifecycle_items(const std::vector<NativeMarketEvent>& events,
                                std::vector<std::string>* physical) const {
        std::vector<std::string> rows;
        for (const auto& event : events) {
            if (event.command) {
                const auto& command = *event.command;
                const char* kind = command_kind_name(command);
                std::ostringstream out;
                out << "{\"kind\":" << json_escape(kind)
                    << ",\"ordinal\":" << json_u64(command_ordinal(command));
                if (std::strcmp(kind, "ExecutionApplied") == 0)
                    out << ",\"physicalEffectsPresent\":true";
                if (const auto* rejected = std::get_if<RejectedEvent>(&command)) {
                    out << ",\"reason\":"
                        << json_u64(static_cast<uint64_t>(rejected->reason));
                }
                if (const auto* rr = std::get_if<ReplaceRejectedEvent>(&command)) {
                    out << ",\"reason\":" << json_u64(static_cast<uint64_t>(rr->reason));
                }
                if (const auto* mr = std::get_if<MatchRejectedEvent>(&command)) {
                    out << ",\"reason\":" << json_u64(static_cast<uint64_t>(mr->reason));
                }
                out << ",\"labels\":[\"host-" << host_ordinal_ << "\"]";
                out << "}";
                rows.push_back(out.str());
                if (const auto* applied = std::get_if<ExecutionAppliedEvent>(&command)) {
                    std::ostringstream effect;
                    effect << "{\"kind\":\"ExecutionApplied\",\"ordinal\":"
                           << json_u64(applied->ordinal) << ",";
                    append_f64(effect, "price", applied->raw_price);
                    effect << ",\"timestampMs\":" << json_i64(applied->effective_time_ms)
                           << ",\"provenance\":" << json_u64(applied->provenance)
                           << ",";
                    append_f64(effect, "quantity", request_quantity(applied->request));
                    effect << ",\"observations\":{\"hostOrdinal\":" << host_ordinal_
                           << ",\"resolvedPrice\":";
                    if (std::isfinite(applied->resolved_price))
                        effect << json_num(applied->resolved_price);
                    else effect << unsupported_number(applied->resolved_price);
                    effect << ",\"currentTicket\":";
                    if (std::isfinite(applied->current_ticket))
                        effect << json_num(applied->current_ticket);
                    else effect << unsupported_number(applied->current_ticket);
                    effect << ",\"openedLotIncarnation\":"
                           << json_u64(applied->opened_lot_incarnation)
                           << ",\"closedTradeCount\":"
                           << json_u64(applied->closed_trade_count) << "}}";
                    physical->push_back(effect.str());
                }
            }
            if (event.driver) {
                const auto& point = *event.driver;
                std::ostringstream effect;
                effect << "{\"kind\":\"DriverPoint\",\"ordinal\":"
                       << json_u64(point.coordinate.ordinal) << ",";
                append_f64(effect, "price", point.raw_price);
                effect << ",\"timestampMs\":"
                       << json_i64(point.coordinate.effective_time_ms)
                       << ",\"provenance\":"
                       << json_u64(static_cast<uint64_t>(point.coordinate.provenance))
                       << ",\"observations\":{\"hostOrdinal\":" << host_ordinal_
                       << ",\"matching\":" << json_bool(point.matching)
                       << ",\"excursion\":" << json_bool(point.excursion)
                       << ",\"coordinate\":" << coordinate_json(point.coordinate);
                if (point.sequence)
                    effect << ",\"sequence\":" << json_u64(*point.sequence);
                else effect << ",\"sequence\":null";
                effect << "}}";
                physical->push_back(effect.str());
            }
        }
        return json_array(rows);
    }

    std::string callbacks_json() const {
        std::vector<std::string> rows;
        const std::size_t n = std::min(bars.size(),
            std::min(contexts.size(), callback_positions.size()));
        for (std::size_t i = 0; i < n; ++i) {
            std::ostringstream out;
            out << "{\"ordinal\":" << json_u64(i)
                << ",\"coordinate\":" << coordinate_json(contexts[i].coordinate)
                << ",\"decisionFloorMs\":" << json_i64(contexts[i].decision_floor_ms)
                << ",\"inputInterval\":" << interval_json(contexts[i].input_interval)
                << ",\"scriptInterval\":" << interval_json(contexts[i].script_interval)
                << ",";
            append_bar_fields(out, bars[i]);
            out << ",";
            append_f64(out, "signedUnits", callback_positions[i]);
            out << "}";
            rows.push_back(out.str());
        }
        return json_array(rows);
    }

    void snapshot() {
        if (!proof_capture) return;
        HostSnap snap;
        snap.ordinal = host_ordinal_;
        const auto state = native_state();
        const NativeRunSpec* live = state.spec;
        const NativeRunSpec& spec = live ? *live : attempted_spec_;
        snap.spec_json = spec_config_json(spec, host_ordinal_);
        snap.identity_json = identity_json(spec);
        snap.calendar_json = calendar_json(spec, host_ordinal_);
        snap.inputs = input_items_;
        const auto events = native_events(0);
        const std::string lifecycle = lifecycle_items(events, &snap.physical);
        // lifecycle_items returns a JSON array; split is unnecessary — store
        // the objects by rebuilding from events again into snap.lifecycle.
        snap.lifecycle.clear();
        {
            // Re-parse is avoided: rebuild command rows only.
            for (const auto& event : events) {
                if (!event.command) continue;
                const auto& command = *event.command;
                const char* kind = command_kind_name(command);
                std::ostringstream out;
                out << "{\"kind\":" << json_escape(kind)
                    << ",\"ordinal\":" << json_u64(command_ordinal(command));
                if (std::strcmp(kind, "ExecutionApplied") == 0)
                    out << ",\"physicalEffectsPresent\":true";
                if (const auto* rejected = std::get_if<RejectedEvent>(&command)) {
                    out << ",\"reason\":"
                        << json_u64(static_cast<uint64_t>(rejected->reason));
                }
                if (const auto* rr = std::get_if<ReplaceRejectedEvent>(&command)) {
                    out << ",\"reason\":"
                        << json_u64(static_cast<uint64_t>(rr->reason));
                }
                if (const auto* mr = std::get_if<MatchRejectedEvent>(&command)) {
                    out << ",\"reason\":"
                        << json_u64(static_cast<uint64_t>(mr->reason));
                }
                out << ",\"labels\":[\"host-" << host_ordinal_ << "\"]}";
                snap.lifecycle.push_back(out.str());
            }
        }
        (void)lifecycle;
        const auto position = physical_position();
        std::ostringstream obs;
        obs << "{\"hostOrdinal\":" << host_ordinal_
            << ",\"configured\":" << json_bool(configured_)
            << ",\"setupStatus\":" << json_u64(static_cast<uint64_t>(setup_status_))
            << ",\"setupError\":" << json_u64(setup_error_)
            << ",\"setupField\":" << json_u64(setup_field_)
            << ",\"lifecycleKind\":" << json_u64(static_cast<uint64_t>(state.kind))
            << ",\"phase\":" << json_u64(static_cast<uint64_t>(state.phase))
            << ",\"completion\":" << json_u64(static_cast<uint64_t>(state.completion))
            << ",\"failureCode\":" << json_u64(static_cast<uint64_t>(state.failure.code))
            << ",\"failureOperation\":"
            << json_u64(static_cast<uint64_t>(state.failure.operation))
            << ",\"failureOrdinal\":" << json_u64(state.failure.ordinal)
            << ",\"consumedHighWater\":" << json_u64(native_consumed_high_water())
            << ",\"decisionFloorMs\":" << json_i64(native_decision_floor())
            << ",\"continuationHash\":" << json_escape(hex64(native_continuation_hash()))
            << ",\"lastError\":" << json_escape(last_error())
            << ",";
        append_f64(obs, "signedUnits", position.signed_units);
        obs << ",";
        append_f64(obs, "averagePrice", position.average_price);
        obs << ",\"lotCount\":" << json_u64(position.lot_count)
            << ",\"tradeCount\":" << trade_count()
            << ",\"callbackCount\":" << json_u64(bars.size())
            << ",\"nativeConfiguration\":" << snap.spec_json
            << ",\"calendar\":" << snap.calendar_json
            << ",\"callbacks\":" << callbacks_json()
            << "}";
        snap.observations_json = obs.str();
        group_snaps.push_back(std::move(snap));
    }
};

bool setup(TraceHost& host, const NativeRunSpec& value) {
    const bool ok = host.configure_native(value).status == NativeSetupStatus::Applied;
    CHECK(ok);
    return ok;
}

bool count(const TraceHost& host, std::size_t expected) {
    CHECK(host.bars.size() == expected);
    CHECK(host.contexts.size() == expected);
    return host.bars.size() == expected && host.contexts.size() == expected;
}

std::vector<ExecutionAppliedEvent> fills(const TraceHost& host) {
    std::vector<ExecutionAppliedEvent> result;
    for (const auto& event : host.native_events(0)) {
        if (event.command) {
            if (const auto* applied = std::get_if<ExecutionAppliedEvent>(&*event.command))
                result.push_back(*applied);
        }
    }
    return result;
}

std::vector<NativeDriverPoint> points(const TraceHost& host, NativePriceProvenance kind) {
    std::vector<NativeDriverPoint> result;
    for (const auto& event : host.native_events(0))
        if (event.driver && event.driver->coordinate.provenance == kind)
            result.push_back(*event.driver);
    return result;
}

void ordered(const TraceHost& host) {
    uint64_t previous = 0;
    for (const auto& event : host.native_events(0)) {
        // Account observations may project the execution ordinal whose
        // physical effect they describe; they do not allocate a second fill.
        CHECK(event.ordinal >= previous);
        if (event.kind != NativeEventKind::Account) CHECK(event.ordinal > previous);
        previous = event.ordinal;
    }
    int64_t floor = std::numeric_limits<int64_t>::min();
    for (const auto& context : host.contexts) {
        CHECK(context.decision_floor_ms >= floor);
        CHECK(context.decision_floor_ms >= context.coordinate.effective_time_ms);
        floor = context.decision_floor_ms;
    }
}

void bar_values(const Bar& actual, double o, double h, double l, double c, double v) {
    near(actual.open, o);
    near(actual.high, h);
    near(actual.low, l);
    near(actual.close, c);
    near(actual.volume, v);
}

std::vector<Bar> minutes(int first, int n) {
    std::vector<Bar> result;
    for (int i = first; i < first + n; ++i)
        result.push_back(flat(static_cast<int64_t>(i) * 60000, 100.0 + i));
    return result;
}

void equal_timeframe_families() {
    struct Case { const char* tf; int64_t boundaries[4]; };
    const Case cases[] = {
        {"1S", {0, 1000, 2000, 3000}},
        {"15S", {0, 15000, 30000, 45000}},
        {"1", {0, 60000, 120000, 180000}},
        {"5", {0, 300000, 600000, 900000}},
        // January 12/13/14/15 and 12/14/16/18, 2025, UTC midnight.
        {"D", {1736640000000LL, 1736726400000LL, 1736812800000LL, 1736899200000LL}},
        {"2D", {1736640000000LL, 1736812800000LL, 1736985600000LL, 1737158400000LL}},
        {"W", {1736121600000LL, 1736726400000LL, 1737331200000LL, 1737936000000LL}},
        // 2W anchors Dec30, Jan13, Jan27, Feb10: Jan6 is NOT a 2W anchor.
        {"2W", {1735516800000LL, 1736726400000LL, 1737936000000LL, 1739145600000LL}},
        {"M", {1735689600000LL, 1738368000000LL, 1740787200000LL, 1743465600000LL}},
        {"3M", {1735689600000LL, 1743465600000LL, 1751328000000LL, 1759276800000LL}},
    };
    for (const auto& item : cases) {
        TraceHost host;
        if (!setup(host, spec(item.tf, item.tf))) continue;
        const Bar input[] = {flat(item.boundaries[0], 101, 2),
                             flat(item.boundaries[1], 102, 3),
                             flat(item.boundaries[2], 103, 5)};
        host.run(input, 3);
        CHECK(host.last_error().empty());
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        if (!count(host, 3)) continue;
        for (int i = 0; i < 3; ++i) {
            CHECK(host.contexts[i].coordinate.open_ms == item.boundaries[i]);
            CHECK(host.contexts[i].coordinate.last_traded_close_ms == item.boundaries[i + 1]);
            CHECK(host.contexts[i].coordinate.next_period_open_ms == item.boundaries[i + 1]);
            CHECK(host.contexts[i].coordinate.effective_time_ms == item.boundaries[i + 1]);
            bar_values(host.bars[i], 101 + i, 101 + i, 101 + i, 101 + i, input[i].volume);
        }
        CHECK(host.native_decision_floor() == item.boundaries[3]);
        ordered(host);
    }
}

void stable_count_and_cross_unit_grouping() {
    // Dropping the leading Jan12 row changes the observed opening, never the
    // Jan12 2D anchor. Jan12 is day ordinal 20100 from 1970-01-01.
    CHECK(1736640000000LL / 86400000LL == 20100);
    CHECK(20100 % 2 == 0);
    for (int skip = 0; skip < 2; ++skip) {
        TraceHost host;
        if (!setup(host, spec("D", "2D"))) continue;
        const Bar input[] = {flat(1736640000000LL, 100, 2), flat(1736726400000LL, 104, 3)};
        host.run(input + skip, 2 - skip);
        if (!count(host, 1)) continue;
        CHECK(host.contexts[0].coordinate.open_ms == 1736640000000LL);
        CHECK(host.contexts[0].coordinate.effective_time_ms == 1736812800000LL);
        bar_values(host.bars[0], skip ? 104 : 100, 104, skip ? 104 : 100, 104, skip ? 3 : 5);
        const auto opening = points(host, NativePriceProvenance::ModeledOHLCOpen);
        CHECK(opening.size() == 1);
        if (!opening.empty()) {
            CHECK(opening[0].coordinate.effective_time_ms == input[skip].timestamp);
            CHECK(opening[0].coordinate.source_price_time_ms == input[skip].timestamp);
        }
    }
    TraceHost seconds;
    if (!setup(seconds, spec("15S", "1"))) return;
    const Bar input[] = {flat(0, 100, 1), flat(15000, 102, 2),
                         flat(30000, 99, 3), flat(45000, 101, 4)};
    seconds.run(input, 4);
    if (count(seconds, 1)) {
        bar_values(seconds.bars[0], 100, 102, 99, 101, 10);
        CHECK(seconds.contexts[0].coordinate.effective_time_ms == 60000);
    }
    // Different valid literals for the same unit/count remain accepted.
    TraceHost aliases;
    if (!setup(aliases, spec("D", "1D"))) return;
    const Bar daily = flat(1736640000000LL);
    aliases.run(&daily, 1);
    count(aliases, 1);
    CHECK(aliases.native_state().spec->script_tf == "1D");
}

void daily_to_week_and_month() {
    struct Case { const char* script; int64_t first; int64_t last; int64_t end; };
    const Case cases[] = {
        {"W", 1736121600000LL, 1736640000000LL, 1736726400000LL}, // Jan6..Jan12
        {"M", 1735689600000LL, 1738281600000LL, 1738368000000LL}, // Jan1..Jan31
    };
    for (const auto& item : cases) {
        for (int skip = 0; skip < 2; ++skip) {
            TraceHost host;
            if (!setup(host, spec("D", item.script))) continue;
            const Bar input[] = {flat(item.first, 100, 2), flat(item.last, 105, 7)};
            host.run(input + skip, 2 - skip); // sparse batch is intentionally legal
            if (!count(host, 1)) continue;
            CHECK(host.contexts[0].coordinate.open_ms == item.first);
            CHECK(host.contexts[0].coordinate.next_period_open_ms == item.end);
            CHECK(host.contexts[0].coordinate.effective_time_ms == item.end);
            bar_values(host.bars[0], skip ? 105 : 100, 105, skip ? 105 : 100, 105, skip ? 7 : 9);
        }
    }
}

void explicit_refusals_and_empty_batch() {
    struct Pair { const char* input; const char* script; };
    const Pair bad[] = {{"5", "7"}, {"5", "1"}, {"D", "60"}, {"W", "D"},
                        {"M", "W"}, {"2W", "3W"}, {"1H", "1H"}, {"0", "1"},
                        {"01", "1"}, {"1S", "S"}, {"D", ""}};
    for (const auto& item : bad) {
        TraceHost host;
        CHECK(host.configure_native(spec(item.input, item.script)).status == NativeSetupStatus::Failed);
        CHECK(host.native_consumed_high_water() == 0);
        count(host, 0);
    }
    for (const char* tf : {"M", "3M"}) {
        TraceHost host;
        if (!setup(host, spec(tf, tf))) continue;
        const auto before = host.native_continuation_hash();
        const Bar monthly = flat(1735689600000LL);
        CHECK(!host.stream_begin(&monthly, 1, "", ""));
        CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(host.native_consumed_high_water() == 0);
        CHECK(host.native_continuation_hash() == before);
        host.run(&monthly, 1);
        count(host, 1);
    }
    TraceHost empty;
    if (setup(empty, spec())) {
        empty.run(nullptr, 0);
        CHECK(empty.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(empty.native_events(0).empty());
        count(empty, 0);
    }
    TraceHost mismatch;
    if (!setup(mismatch, spec("1", "5"))) return;
    const auto warmup = minutes(0, 3);
    const auto before = mismatch.native_continuation_hash();
    CHECK(!mismatch.stream_begin(warmup.data(), 3, "5", "5"));
    CHECK(!mismatch.stream_begin(warmup.data(), 3, "1", "1"));
    CHECK(mismatch.native_state().kind == NativeLifecycleKind::Ready);
    CHECK(mismatch.native_consumed_high_water() == 0);
    CHECK(mismatch.native_continuation_hash() == before);
    CHECK(mismatch.stream_begin(warmup.data(), 3, "", ""));
    CHECK(mismatch.stream_end(false));
    count(mismatch, 0);
}

void ny_daily_dst() {
    const int64_t dates[][4] = {
        {1741410000000LL, 1741496400000LL, 1741579200000LL, 1741665600000LL}, // Mar8..11
        {1761969600000LL, 1762056000000LL, 1762146000000LL, 1762232400000LL}, // Nov1..4
    };
    CHECK(dates[0][2] - dates[0][1] == 23LL * 3600000);
    CHECK(dates[1][2] - dates[1][1] == 25LL * 3600000);
    for (const auto& row : dates) {
        TraceHost host;
        auto value = spec("D", "D");
        value.timezone = "America/New_York";
        if (!setup(host, value)) continue;
        const Bar warmup[] = {flat(row[0], 100), flat(row[1], 101)};
        CHECK(host.stream_begin(warmup, 2, "D", "D"));
        if (!count(host, 2)) continue;
        CHECK(host.native_decision_floor() == row[2]);
        // A guessed 24-hour boundary is invalid on both DST transition days.
        const auto before = host.native_continuation_hash();
        CHECK(!host.stream_push_bar(flat(row[1] + 86400000LL, 102)));
        CHECK(host.native_continuation_hash() == before);
        CHECK(host.stream_push_bar(flat(row[2], 102)));
        if (count(host, 3)) {
            for (int i = 0; i < 3; ++i) {
                CHECK(host.contexts[i].coordinate.open_ms == row[i]);
                CHECK(host.contexts[i].coordinate.last_traded_close_ms == row[i + 1]);
                CHECK(host.contexts[i].coordinate.next_period_open_ms == row[i + 1]);
                CHECK(host.contexts[i].coordinate.effective_time_ms == row[i + 1]);
            }
        }
        CHECK(host.stream_end(false));
        ordered(host);
    }
}

void ny_fixed_gap_and_fold_adjacency() {
    // Gap: 01:59 EST -> 03:00 EDT. Fold: 01:59 EDT -> 01:00 EST.
    const int64_t rows[][3] = {
        {1741503540000LL, 1741503600000LL, 1741503660000LL},
        {1762063140000LL, 1762063200000LL, 1762063260000LL},
    };
    for (const auto& row : rows) {
        TraceHost host;
        auto value = spec();
        value.timezone = "America/New_York";
        if (!setup(host, value)) continue;
        const Bar first = flat(row[0]);
        CHECK(host.stream_begin(&first, 1, "", ""));
        CHECK(host.stream_push_bar(flat(row[1], 101)));
        CHECK(host.stream_push_bar(flat(row[2], 102)));
        if (count(host, 3)) {
            CHECK(host.contexts[0].coordinate.next_period_open_ms == row[1]);
            CHECK(host.contexts[1].coordinate.next_period_open_ms == row[2]);
        }
        CHECK(host.stream_end(false));
    }
}

void lunch_quiet_slot() {
    TraceHost host;
    auto value = spec("60", "60");
    value.session = "0930-1130,1300-1600";
    if (!setup(host, value)) return;
    const Bar morning = flat(1749465000000LL, 101); // Jun9 10:30..11:30 UTC
    CHECK(host.stream_begin(&morning, 1, "", ""));
    CHECK(host.stream_advance_time(1749475800000LL)); // 13:30
    if (count(host, 2)) {
        const auto& c = host.contexts[1].coordinate;
        CHECK(c.open_ms == 1749472200000LL); // nominal 12:30
        CHECK(c.eligible_open_ms == 1749474000000LL); // clipped 13:00
        CHECK(c.last_traded_close_ms == 1749475800000LL);
        bar_values(host.bars[1], 101, 101, 101, 101, 0);
    }
    const auto carried = points(host, NativePriceProvenance::CarriedOpen);
    CHECK(carried.size() == 1);
    if (!carried.empty()) {
        CHECK(carried[0].coordinate.open_ms == 1749472200000LL);
        CHECK(carried[0].coordinate.effective_time_ms == 1749474000000LL);
    }
    CHECK(host.stream_end(false));
}

void lunch_real_off_session_and_replay() {
    TraceHost a, b;
    auto value = spec("60", "60");
    value.session = "0930-1130,1300-1600";
    if (!setup(a, value) || !setup(b, value)) return;
    const Bar morning = flat(1749465000000LL, 100);
    for (TraceHost* host : {&a, &b}) {
        CHECK(host->stream_begin(&morning, 1, "", ""));
        CHECK(host->submit_market(buy()).status == SubmitStatus::Accepted);
    }
    CHECK(a.native_continuation_hash() == b.native_continuation_hash());
    for (TraceHost* host : {&a, &b}) {
        CHECK(host->stream_push_tick(TradeTick{1749473100000LL, 1, 123.0, 2.0})); // closed 12:45
        if (host == &a) (void)host->native_continuation_hash();
        CHECK(host->stream_advance_time(1749474000000LL)); // 13:00
        if (host == &a) (void)host->native_continuation_hash();
        CHECK(host->stream_push_tick(TradeTick{1749474300000LL, 2, 125.0, 3.0})); // 13:05
        CHECK(host->stream_advance_time(1749475800000LL)); // 13:30
        if (count(*host, 2)) {
            CHECK(host->contexts[1].coordinate.open_ms == 1749472200000LL);
            bar_values(host->bars[1], 123, 125, 123, 125, 5);
        }
        CHECK(points(*host, NativePriceProvenance::ObservedPrint).size() == 2);
        CHECK(points(*host, NativePriceProvenance::CarriedOpen).empty());
        const auto executed = fills(*host);
        CHECK(executed.size() == 1);
        if (!executed.empty()) {
            CHECK(executed[0].effective_time_ms == 1749473100000LL);
            CHECK(executed[0].provenance == static_cast<std::uint8_t>(NativePriceProvenance::ObservedPrint));
            near(executed[0].raw_price, 123);
        }
        CHECK(host->stream_end(false));
        ordered(*host);
    }
    CHECK(a.native_continuation_hash() == b.native_continuation_hash());
}

void nominal_and_clipped_confirmed_labels() {
    auto value = spec("60", "60");
    value.session = "0930-1130,1300-1600";
    const Bar duplicate[] = {flat(1749472200000LL), flat(1749474000000LL)};
    TraceHost refused;
    if (!setup(refused, value)) return;
    const auto before = refused.native_continuation_hash();
    CHECK(!refused.stream_begin(duplicate, 2, "", ""));
    CHECK(refused.native_state().kind == NativeLifecycleKind::Ready);
    CHECK(refused.native_consumed_high_water() == 0);
    CHECK(refused.native_continuation_hash() == before);
    for (const auto& input : duplicate) {
        TraceHost host;
        if (!setup(host, value)) continue;
        host.run(&input, 1);
        if (count(host, 1)) {
            CHECK(host.contexts[0].coordinate.open_ms == 1749472200000LL);
            CHECK(host.contexts[0].coordinate.next_period_open_ms == 1749475800000LL);
        }
        const auto opening = points(host, NativePriceProvenance::ModeledOHLCOpen);
        CHECK(opening.size() == 1);
        if (!opening.empty()) CHECK(opening[0].coordinate.effective_time_ms == input.timestamp);
    }
}

void friday_monday_continuity() {
    auto value = spec();
    value.session = "0930-1600:23456";
    TraceHost host;
    if (!setup(host, value)) return;
    const Bar friday = flat(1749225540000LL); // Jun6 Fri 15:59
    CHECK(host.stream_begin(&friday, 1, "", ""));
    if (count(host, 1)) {
        CHECK(host.contexts[0].coordinate.last_traded_close_ms == 1749225600000LL);
        CHECK(host.contexts[0].coordinate.next_input_open_ms == 1749461400000LL);
    }
    CHECK(host.native_decision_floor() == 1749225600000LL);
    const auto before = host.native_continuation_hash();
    CHECK(!host.stream_push_bar(flat(1749466800000LL))); // Mon11 skipped Mon09:30
    CHECK(host.native_continuation_hash() == before);
    CHECK(host.stream_push_bar(flat(1749461400000LL)));
    CHECK(host.stream_end(false));

    TraceHost quiet;
    if (!setup(quiet, value)) return;
    CHECK(quiet.stream_begin(&friday, 1, "", ""));
    CHECK(quiet.submit_market(buy()).status == SubmitStatus::Accepted);
    CHECK(quiet.stream_advance_time(1749461400000LL)); // Monday opening, no whole quiet slot yet
    CHECK(fills(quiet).empty());
    CHECK(points(quiet, NativePriceProvenance::CarriedOpen).empty());
    count(quiet, 1);
    CHECK(quiet.stream_advance_time(1749461460000LL));
    const auto carried = points(quiet, NativePriceProvenance::CarriedOpen);
    CHECK(carried.size() == 1);
    if (!carried.empty()) CHECK(carried[0].coordinate.effective_time_ms == 1749461400000LL);
    CHECK(fills(quiet).size() == 1);
    CHECK(quiet.stream_end(false));
}

void overnight_cycle_origin_and_window_permutations() {
    for (const char* session : {"1800-1700:23456", "1800-0000,0000-1200,1200-1700:23456",
                                "1800-0000,1200-1700,0000-1200:23456"}) {
        TraceHost host;
        auto value = spec("D", "D");
        value.session = session;
        if (!setup(host, value)) continue;
        const Bar sunday = flat(1736704800000LL); // Jan12 Sunday18:00, Monday trading date
        host.run(&sunday, 1);
        if (!count(host, 1)) continue;
        CHECK(host.contexts[0].coordinate.open_ms == 1736704800000LL);
        CHECK(host.contexts[0].coordinate.last_traded_close_ms == 1736787600000LL); // Mon17
        CHECK(host.contexts[0].coordinate.next_period_open_ms == 1736791200000LL); // Mon18
    }
    // All windows participate in the daily close; the first morning window is
    // not a substitute for the normalized split-session union.
    TraceHost split;
    auto value = spec("D", "D");
    value.session = "0930-1130,1300-1600";
    if (!setup(split, value)) return;
    const Bar monday = flat(1749461400000LL);
    split.run(&monday, 1);
    if (count(split, 1)) CHECK(split.contexts[0].coordinate.last_traded_close_ms == 1749484800000LL);
}

void cross_calendar_straddles() {
    struct Case {
        const char* input;
        const char* script;
        int64_t first, first_close, next, script_open, script_seal;
    };
    const Case cases[] = {
        {"W", "M", 1737936000000LL, 1738540800000LL, 1738540800000LL,
         1735689600000LL, 1738368000000LL}, // Jan27..Feb3 belongs wholly to January
        {"2D", "W", 1736640000000LL, 1736812800000LL, 1736812800000LL,
         1736121600000LL, 1736726400000LL}, // SunJan12..TueJan14 belongs to weekJan6
    };
    for (const auto& item : cases) {
        for (auto policy : {NativeCloseExecution::NextEligiblePoint, NativeCloseExecution::AfterCalculation}) {
            TraceHost host;
            auto value = spec(item.input, item.script);
            value.close_execution = policy;
            if (!setup(host, value)) continue;
            const Bar first{100, 110, 90, 105, 7, item.first};
            CHECK(host.stream_begin(&first, 1, "", ""));
            if (!count(host, 1)) continue;
            const auto& c = host.contexts[0];
            CHECK(c.coordinate.open_ms == item.script_open);
            CHECK(c.coordinate.next_period_open_ms == item.script_seal);
            CHECK(c.coordinate.last_traded_close_ms == item.script_seal);
            CHECK(c.coordinate.effective_time_ms == item.first_close);
            CHECK(c.decision_floor_ms == item.first_close);
            CHECK(c.script_interval.next_period_open_ms == item.script_seal);
            bar_values(host.bars[0], 100, 110, 90, 105, 7);
            CHECK(host.native_decision_floor() == item.first_close);
            CHECK(host.submit_market(buy()).status == SubmitStatus::Accepted);
            CHECK(host.stream_push_bar(flat(item.next, 120, 11)));
            CHECK(fills(host).empty()); // next script remains incomplete
            CHECK(host.stream_end(false));
            count(host, 1); // do not split/recount the straddling volume
            ordered(host);
        }
    }
}

void cycle_date_week_month_and_counts() {
    // Monday Jan13 is week index2872 from Monday1969-12-29.
    CHECK((1736726400000LL + 259200000LL) / 604800000LL == 2872);
    CHECK(2025 * 12 == 24300);
    struct Case { const char* tf; int64_t input; int64_t open; int64_t end; };
    const Case cases[] = {
        {"D", 1736701200000LL, 1736701200000LL, 1736787600000LL}, // Sunday17 -> Monday date
        {"2D", 1736701200000LL, 1736614800000LL, 1736787600000LL}, // Jan12/13 cycle dates
        {"W", 1736701200000LL, 1736701200000LL, 1737306000000LL},
        {"2W", 1736701200000LL, 1736701200000LL, 1737910800000LL},
        {"M", 1735664400000LL, 1735664400000LL, 1738342800000LL}, // Dec31 17 -> Jan date
        {"3M", 1735664400000LL, 1735664400000LL, 1743440400000LL},
    };
    for (const auto& item : cases) {
        TraceHost host;
        auto value = spec("1", item.tf);
        value.session = "1700-1700";
        if (!setup(host, value)) continue;
        // Sparse batch: a next-key input proves completion; only the first
        // bucket completes. Its first source open need not equal its anchor.
        const Bar input[] = {flat(item.input, 100, 2), flat(item.end, 105, 3)};
        host.run(input, 2);
        if (!count(host, 1)) continue;
        CHECK(host.contexts[0].coordinate.open_ms == item.open);
        CHECK(host.contexts[0].coordinate.next_period_open_ms == item.end);
        CHECK(host.contexts[0].script_interval.open_ms == item.open);
        bar_values(host.bars[0], 100, 100, 100, 100, 2);
    }
}

void masked_and_gap_empty_cycle_attribution() {
    struct Case {
        const char* timezone;
        const char* session;
        const char* script;
        int64_t observed, proving, open, end;
    };
    const Case cases[] = {
        {"UTC", "0930-1600:23456", "D", 1749385800000LL, 1749461400000LL,
         1749375000000LL, 1749461400000LL}, // SundayJun8 closed, never remapped to Monday
        {"UTC", "0930-1600:23456", "W", 1749385800000LL, 1749461400000LL,
         1748856600000LL, 1749461400000LL}, // retains weekJun2
        {"UTC", "0930-1600:23456", "M", 1704024000000LL, 1704101400000LL,
         1701423000000LL, 1704101400000LL}, // SundayDec31 noon retains December
        {"UTC", "0930-1600:23456", "W", 1704024000000LL, 1704101400000LL,
         1703496600000LL, 1704101400000LL}, // retains weekDec25
        {"America/New_York", "0230-0245", "D", 1741536000000LL, 1741588200000LL,
         1741503600000LL, 1741588200000LL}, // Mar9 local gap collapses coverage, not cycle
    };
    for (const auto& item : cases) {
        TraceHost host;
        auto value = spec("1", item.script);
        value.timezone = item.timezone;
        value.session = item.session;
        if (!setup(host, value)) continue;
        const Bar input[] = {flat(item.observed, 123, 2), flat(item.proving, 125, 3)};
        host.run(input, 2);
        if (!count(host, 1)) continue;
        const auto& c = host.contexts[0];
        CHECK(c.coordinate.open_ms == item.open);
        CHECK(c.coordinate.next_period_open_ms == item.end);
        CHECK(c.script_interval.open_ms <= item.observed);
        CHECK(item.observed < c.script_interval.next_period_open_ms);
        bar_values(host.bars[0], 123, 123, 123, 123, 2);
        const auto opening = points(host, NativePriceProvenance::ModeledOHLCOpen);
        CHECK(opening.size() == 1);
        if (!opening.empty()) {
            CHECK(opening[0].coordinate.effective_time_ms == item.observed);
            near(opening[0].raw_price, 123);
        }
    }
}

void partial_coarser_warmup_confirmed() {
    for (int n : {3, 7}) {
        TraceHost host;
        if (!setup(host, spec("1", "5"))) continue;
        const auto warmup = minutes(0, n);
        CHECK(host.stream_begin(warmup.data(), n, "", ""));
        count(host, static_cast<std::size_t>(n / 5));
        CHECK(host.native_decision_floor() == n * 60000LL);
        CHECK(host.native_state().phase == NativeRunPhase::Realtime);
        const int end = n < 5 ? 5 : 10;
        for (int i = n; i < end; ++i) CHECK(host.stream_push_bar(flat(i * 60000LL, 100 + i)));
        if (count(host, static_cast<std::size_t>(end / 5))) {
            const auto last = host.bars.size() - 1;
            const int first = end - 5;
            CHECK(host.contexts[last].coordinate.open_ms == first * 60000LL);
            CHECK(host.contexts[last].coordinate.effective_time_ms == end * 60000LL);
            bar_values(host.bars[last], 100 + first, 100 + end - 1, 100 + first, 100 + end - 1, 5);
        }
        CHECK(host.stream_end(false));
    }
    TraceHost refused;
    if (!setup(refused, spec("1", "5"))) return;
    const Bar gap[] = {flat(0), flat(120000)};
    const auto before = refused.native_continuation_hash();
    CHECK(!refused.stream_begin(gap, 2, "", ""));
    CHECK(refused.native_consumed_high_water() == 0);
    CHECK(refused.native_continuation_hash() == before);
}

void rth_partial_daily_warmup_seal() {
    auto value = spec("1", "D");
    value.session = "0930-1600:23456";
    const Bar last = flat(1749225540000LL, 101); // Fri15:59
    TraceHost ended;
    if (!setup(ended, value)) return;
    CHECK(ended.stream_begin(&last, 1, "", ""));
    count(ended, 0); // scheduled16:00 is not a nominal daily seal
    CHECK(ended.native_decision_floor() == 1749225600000LL);
    CHECK(ended.stream_end(false));
    count(ended, 0);

    TraceHost advanced;
    if (!setup(advanced, value)) return;
    CHECK(advanced.stream_begin(&last, 1, "", ""));
    CHECK(advanced.stream_advance_time(1749288600000LL)); // Saturday09:30 nominal exhaustion
    if (count(advanced, 1)) {
        CHECK(advanced.contexts[0].coordinate.open_ms == 1749202200000LL);
        CHECK(advanced.contexts[0].coordinate.last_traded_close_ms == 1749225600000LL);
        CHECK(advanced.contexts[0].coordinate.next_period_open_ms == 1749288600000LL);
        bar_values(advanced.bars[0], 101, 101, 101, 101, 1);
    }
    CHECK(points(advanced, NativePriceProvenance::CarriedOpen).empty());
    CHECK(advanced.native_decision_floor() == 1749288600000LL);
    CHECK(advanced.stream_end(false));
    count(advanced, 1);
}

void ticks_same_timestamp_and_atomic_refusal() {
    TraceHost host;
    if (!setup(host, spec())) return;
    const Bar warmup = flat(0);
    CHECK(host.stream_begin(&warmup, 1, "", ""));
    CHECK(host.stream_push_tick(TradeTick{60000, 1, 100.25, 1}));
    CHECK(host.submit_market(buy()).status == SubmitStatus::Accepted);
    CHECK(fills(host).empty());
    CHECK(host.stream_push_tick(TradeTick{60000, 2, 100.50, 2}));
    const auto executed = fills(host);
    CHECK(executed.size() == 1);
    if (!executed.empty()) {
        near(executed[0].raw_price, 100.50);
        CHECK(executed[0].effective_time_ms == 60000);
        CHECK(executed[0].provenance == static_cast<std::uint8_t>(NativePriceProvenance::ObservedPrint));
    }
    const auto before = host.native_continuation_hash();
    CHECK(!host.stream_push_tick(TradeTick{60000, 2, 200, 2}));
    CHECK(host.native_continuation_hash() == before);
    const TradeTick invalid_array[] = {{60100, 3, 101, 1},
        {60200, 4, std::numeric_limits<double>::quiet_NaN(), 1}};
    CHECK(!host.stream_push_ticks(invalid_array, 2));
    CHECK(host.native_continuation_hash() == before);
    CHECK(points(host, NativePriceProvenance::ObservedPrint).size() == 2);
    CHECK(host.stream_advance_time(120000));
    CHECK(fills(host).size() == 1);
    if (count(host, 2)) bar_values(host.bars[1], 100.25, 100.50, 100.25, 100.50, 3);
    CHECK(host.stream_end(false));
    ordered(host);
}

void no_preentry_tick_extrema_replay() {
    TraceHost host;
    if (!setup(host, spec())) return;
    const Bar warmup = flat(0);
    CHECK(host.stream_begin(&warmup, 1, "", ""));
    CHECK(host.stream_push_tick(TradeTick{60100, 1, 150, 1}));
    CHECK(host.stream_push_tick(TradeTick{60200, 2, 50, 1}));
    CHECK(host.submit_market(buy()).status == SubmitStatus::Accepted);
    CHECK(host.stream_push_tick(TradeTick{60300, 3, 100, 1}));
    CHECK(host.stream_push_tick(TradeTick{60400, 4, 102, 1}));
    near(host.runup(), 2);
    near(host.drawdown(), 0);
    CHECK(host.stream_advance_time(120000));
    near(host.runup(), 2);
    near(host.drawdown(), 0);
    if (count(host, 2)) bar_values(host.bars[1], 150, 150, 50, 102, 4);
    CHECK(points(host, NativePriceProvenance::ObservedPrint).size() == 4);
    CHECK(points(host, NativePriceProvenance::ModeledOHLCOpen).size() == 1); // warmup only
    CHECK(host.stream_end(false));
}

void quiet_birth_floor() {
    for (bool advance_before_submit : {false, true}) {
        TraceHost host;
        if (!setup(host, spec())) continue;
        const Bar warmup = flat(0);
        CHECK(host.stream_begin(&warmup, 1, "", ""));
        if (advance_before_submit) CHECK(host.stream_advance_time(90000));
        CHECK(host.native_decision_floor() == (advance_before_submit ? 90000 : 60000));
        CHECK(host.submit_market(buy()).status == SubmitStatus::Accepted);
        CHECK(host.stream_advance_time(180000));
        const auto carried = points(host, NativePriceProvenance::CarriedOpen);
        CHECK(carried.size() == 2);
        if (carried.size() == 2) {
            CHECK(carried[0].coordinate.effective_time_ms == 60000);
            CHECK(carried[1].coordinate.effective_time_ms == 120000);
        }
        const auto executed = fills(host);
        CHECK(executed.size() == 1);
        if (!executed.empty()) {
            CHECK(executed[0].effective_time_ms == (advance_before_submit ? 120000 : 60000));
            CHECK(executed[0].provenance == static_cast<std::uint8_t>(NativePriceProvenance::CarriedOpen));
        }
        CHECK(host.native_decision_floor() == 180000);
        CHECK(host.stream_end(false));
    }
}

void confirmed_close_policy() {
    for (auto policy : {NativeCloseExecution::NextEligiblePoint, NativeCloseExecution::AfterCalculation}) {
        TraceHost host;
        auto value = spec();
        value.close_execution = policy;
        if (!setup(host, value)) continue;
        host.callback = [](TraceHost& h) { CHECK(h.submit_market(buy()).status == SubmitStatus::Accepted); };
        const Bar input{100, 110, 90, 101, 1, 60000};
        host.run(&input, 1);
        if (!count(host, 1)) continue;
        near(host.callback_positions[0], 0);
        CHECK(host.contexts[0].coordinate.effective_time_ms == 120000);
        const auto executed = fills(host);
        CHECK(executed.size() == (policy == NativeCloseExecution::AfterCalculation ? 1U : 0U));
        if (!executed.empty()) {
            CHECK(executed[0].effective_time_ms == 120000);
            CHECK(executed[0].ordinal > host.contexts[0].coordinate.ordinal);
                CHECK(executed[0].provenance == static_cast<std::uint8_t>(NativePriceProvenance::AfterCalculationClose));
            near(executed[0].raw_price, 101);
            near(host.runup(), 0);
            near(host.drawdown(), 0);
        }
    }
}

void delayed_open_after_external_birth() {
    for (auto policy : {NativeCloseExecution::NextEligiblePoint, NativeCloseExecution::AfterCalculation}) {
        TraceHost host;
        auto value = spec("1", "5");
        value.close_execution = policy;
        if (!setup(host, value)) continue;
        const auto warmup = minutes(0, 5);
        CHECK(host.stream_begin(warmup.data(), 5, "", ""));
        CHECK(host.stream_push_bar(flat(300000, 105)));
        CHECK(host.stream_push_bar(flat(360000, 106)));
        CHECK(host.native_decision_floor() == 420000);
        CHECK(host.submit_market(buy()).status == SubmitStatus::Accepted);
        for (int i = 7; i < 10; ++i) CHECK(host.stream_push_bar(flat(i * 60000LL, 100 + i)));
        if (count(host, 2)) CHECK(host.contexts[1].coordinate.effective_time_ms == 600000);
        const auto executed = fills(host);
        CHECK(executed.size() == (policy == NativeCloseExecution::AfterCalculation ? 1U : 0U));
        if (!executed.empty()) {
            CHECK(executed[0].effective_time_ms == 600000);
            near(executed[0].raw_price, 109);
        }
        for (const auto& fill : executed) CHECK(fill.effective_time_ms >= 420000);
        CHECK(host.stream_end(false));
        ordered(host);
    }
}

void mixed_warmup_tick_quiet_aggregation() {
    for (bool quiet : {false, true}) {
        TraceHost host;
        if (!setup(host, spec("1", "5"))) continue;
        const Bar warmup[] = {{100, 110, 90, 101, 2, 0}, flat(60000, 102, 3), flat(120000, 103, 5)};
        CHECK(host.stream_begin(warmup, 3, "", ""));
        count(host, 0);
        if (!quiet) {
            CHECK(host.submit_market(buy()).status == SubmitStatus::Accepted);
            CHECK(host.stream_push_tick(TradeTick{180100, 1, 104, 7}));
            CHECK(host.stream_push_tick(TradeTick{181000, 2, 106, 11}));
            CHECK(host.stream_push_tick(TradeTick{240100, 3, 105, 13}));
        }
        CHECK(host.stream_advance_time(300000));
        if (count(host, 1)) {
            bar_values(host.bars[0], 100, 110, 90, quiet ? 103 : 105, quiet ? 10 : 41);
            CHECK(host.contexts[0].coordinate.effective_time_ms == 300000);
        }
        // The confirmed warmup contributes aggregate values, but finalizing
        // a mixed observed/quiet aggregate cannot replay historical OHLC.
        CHECK(points(host, NativePriceProvenance::ModeledOHLCOpen).empty());
        CHECK(points(host, NativePriceProvenance::ModeledOHLCClose).empty());
        CHECK(points(host, NativePriceProvenance::ObservedPrint).size() == (quiet ? 0U : 3U));
        CHECK(points(host, NativePriceProvenance::CarriedOpen).size() == (quiet ? 2U : 0U));
        if (!quiet) {
            CHECK(fills(host).size() == 1);
            near(host.runup(), 2); // aggregate high110 predates entry104
            near(host.drawdown(), 0); // aggregate low90 also predates entry
        }
        CHECK(host.stream_end(false));
        count(host, 1);
    }
}

void partial_end_equal_clock_and_policy() {
    for (auto policy : {NativeCloseExecution::NextEligiblePoint, NativeCloseExecution::AfterCalculation}) {
        for (bool finalize : {false, true}) {
            TraceHost host;
            auto value = spec();
            value.close_execution = policy;
            if (!setup(host, value)) continue;
            const Bar warmup = flat(0);
            CHECK(host.stream_begin(&warmup, 1, "", ""));
            host.callback = [](TraceHost& h) { CHECK(h.submit_market(buy()).status == SubmitStatus::Accepted); };
            CHECK(host.stream_push_tick(TradeTick{60100, 1, 107, 3}));
            CHECK(host.stream_advance_time(90000));
            CHECK(host.stream_end(finalize));
            CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
            CHECK(host.native_decision_floor() == 90000);
            if (!count(host, finalize ? 2 : 1)) continue;
            if (finalize) {
                const auto& c = host.contexts[1];
                CHECK(c.coordinate.open_ms == 60000);
                CHECK(c.coordinate.source_price_time_ms == 60100);
                CHECK(c.coordinate.effective_time_ms == 90000);
                CHECK(c.coordinate.completion == NativeCompletionKind::PartialFinalized);
                CHECK(c.decision_floor_ms == 90000);
                bar_values(host.bars[1], 107, 107, 107, 107, 3);
            }
            const bool should_fill = finalize && policy == NativeCloseExecution::AfterCalculation;
            const auto executed = fills(host);
            CHECK(executed.size() == (should_fill ? 1U : 0U));
            if (should_fill && !executed.empty()) {
                CHECK(executed[0].effective_time_ms == 90000);
                near(executed[0].raw_price, 107);
                const auto modeled = points(host, NativePriceProvenance::AfterCalculationClose);
                // Warmup has one after-calculation point even with no request.
                CHECK(modeled.size() == 2);
                if (modeled.size() == 2) {
                    CHECK(modeled[1].coordinate.source_price_time_ms == 60100);
                    CHECK(modeled[1].coordinate.effective_time_ms == 90000);
                }
            }
            CHECK(points(host, NativePriceProvenance::ObservedPrint).size() == 1);
            ordered(host);
        }
    }
}

void partial_end_does_not_seal_coarser_script() {
    for (auto policy : {NativeCloseExecution::NextEligiblePoint, NativeCloseExecution::AfterCalculation}) {
        TraceHost host;
        auto value = spec("1", "5");
        value.close_execution = policy;
        if (!setup(host, value)) continue;
        const auto warmup = minutes(0, 5);
        CHECK(host.stream_begin(warmup.data(), 5, "", ""));
        CHECK(host.stream_push_tick(TradeTick{300100, 1, 105, 1}));
        CHECK(host.stream_push_tick(TradeTick{360100, 2, 106, 1}));
        CHECK(host.stream_push_tick(TradeTick{420100, 3, 107, 1}));
        CHECK(host.stream_advance_time(450000));
        CHECK(host.stream_end(true));
        count(host, 1); // two completed child slots and a partial third are not 5m
        CHECK(host.native_decision_floor() == 450000);
        CHECK(points(host, NativePriceProvenance::ObservedPrint).size() == 3);
        CHECK(points(host, NativePriceProvenance::AfterCalculationClose).size()
              == (policy == NativeCloseExecution::AfterCalculation ? 1U : 0U));
        CHECK(fills(host).empty());
    }
}

ScenarioArt assemble_group(const char* id, bool passed) {
    ScenarioArt art;
    art.id = id;
    art.status = passed ? "passed" : "failed";
    std::vector<std::string> configs;
    std::vector<std::string> calendars;
    std::vector<std::string> observations;
    configs.reserve(group_snaps.size());
    calendars.reserve(group_snaps.size());
    observations.reserve(group_snaps.size());
    for (const auto& host : group_snaps) {
        configs.push_back(host.spec_json);
        calendars.push_back(host.calendar_json);
        observations.push_back(host.observations_json);
    }
    std::ostringstream config;
    config << "{\"hostCount\":" << group_snaps.size()
           << ",\"hosts\":" << json_array(configs) << "}";
    art.native_configuration = config.str();
    std::ostringstream cal;
    cal << "{\"hostCount\":" << group_snaps.size()
        << ",\"hosts\":" << json_array(calendars) << "}";
    art.calendar = cal.str();
    if (!group_snaps.empty()) art.run_identity = group_snaps.front().identity_json;
    art.logical_inputs = flatten_items(group_snaps, &HostSnap::inputs);
    art.lifecycle_events = flatten_items(group_snaps, &HostSnap::lifecycle);
    art.physical_effects = flatten_items(group_snaps, &HostSnap::physical);
    std::ostringstream obs;
    obs << "{\"hostCount\":" << group_snaps.size()
        << ",\"assertionDeltaPassed\":" << json_bool(passed)
        << ",\"hosts\":" << json_array(observations) << "}";
    art.observations = obs.str();
    art.comparisons = "[]";
    return art;
}

bool write_proof(const std::vector<ScenarioArt>& scenarios, const char* sha) {
    const char* dir = std::getenv("PINEFORGE_NATIVE_PROOF_OUTPUT");
    if (!dir || !*dir) return true;
    std::ostringstream json;
    json << "{\n  \"schemaVersion\": \"pineforge-native-scenario-artifact/v1\",\n"
         << "  \"testName\": \"test_native_calendar_driver_contract\",\n"
         << "  \"scenarios\": [\n";
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        const auto& s = scenarios[i];
        json << "    {\n"
             << "      \"scenarioId\": " << json_escape(s.id) << ",\n"
             << "      \"status\": " << json_escape(s.status) << ",\n"
             << "      \"assertionsEnabled\": true,\n"
             << "      \"fixture\": { \"synthetic-source\": " << json_escape(sha) << " },\n"
             << "      \"nativeConfiguration\": " << s.native_configuration << ",\n"
             << "      \"runIdentity\": " << s.run_identity << ",\n"
             << "      \"calendar\": " << s.calendar << ",\n"
             << "      \"logicalInputs\": " << s.logical_inputs << ",\n"
             << "      \"lifecycleEvents\": " << s.lifecycle_events << ",\n"
             << "      \"physicalEffects\": " << s.physical_effects << ",\n"
             << "      \"observations\": " << s.observations << ",\n"
             << "      \"comparisons\": " << s.comparisons << "\n"
             << "    }" << (i + 1 == scenarios.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    const std::string path = std::string(dir) + "/test_native_calendar_driver_contract.json";
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) return false;
    out << json.str();
    return static_cast<bool>(out);
}

void run_case(const char* id, const char* name, void (*body)()) {
    scenario = name;
    ++scenarios;
    group_snaps.clear();
    next_host_ordinal = 0;
    const int before = failures;
    try { body(); }
    catch (const std::exception& error) {
        ++failures;
        std::printf("FAIL [%s] unexpected exception: %s\n", scenario, error.what());
    }
    catch (...) {
        ++failures;
        std::printf("FAIL [%s] unexpected nonstandard exception\n", scenario);
    }
    const bool passed = failures == before;
    std::printf("%s [%s]\n", passed ? "PASS" : "FAIL", name);
    if (proof_capture) arts.push_back(assemble_group(id, passed));
}
} // namespace

int main() {
    const char* proof_dir = std::getenv("PINEFORGE_NATIVE_PROOF_OUTPUT");
    const char* proof_sha = nullptr;
    if (proof_dir && *proof_dir) {
#ifdef NDEBUG
        std::printf("FAIL proof output requested with NDEBUG; assertions would be compiled out\n");
        return 1;
#endif
#if !defined(PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256)
        std::printf("FAIL PINEFORGE_NATIVE_PROOF_OUTPUT is set but "
                    "PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256 is not supplied\n");
        return 1;
#else
        proof_sha = PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256;
        if (!is_sha256_hex(proof_sha)) {
            std::printf("FAIL PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256 is missing or not a 64-char lowercase digest\n");
            return 1;
        }
        proof_capture = true;
#endif
    }

    run_case("C1-equal-timeframe-families-and-final-monthly-bars",
             "C1 equal timeframe families and final monthly bars",
             equal_timeframe_families);
    run_case("C1-stable-counts-shifted-prefix-and-second-minute-pairing",
             "C1 stable counts shifted prefix and second/minute pairing",
             stable_count_and_cross_unit_grouping);
    run_case("C1-sparse-D-to-W-M-opening-attribution",
             "C1 sparse D-to-W/M opening attribution",
             daily_to_week_and_month);
    run_case("C1-C6-explicit-refusals-timeframe-arguments-empty-batch",
             "C1/C6 explicit refusals timeframe arguments empty batch",
             explicit_refusals_and_empty_batch);
    run_case("C2-NY-actual-23-25-hour-daily-close-and-continuity",
             "C2 NY actual 23/25-hour daily close and continuity",
             ny_daily_dst);
    run_case("C2-C3-NY-fixed-gap-and-fold-adjacency",
             "C2/C3 NY fixed gap and fold adjacency",
             ny_fixed_gap_and_fold_adjacency);
    run_case("C3-lunch-quiet-canonical-slot-and-clipped-opening",
             "C3 lunch quiet canonical slot and clipped opening",
             lunch_quiet_slot);
    run_case("C3-C4-real-off-session-prints-suppress-quiet-synthesis-and-replay",
             "C3/C4 real off-session prints suppress quiet synthesis and replay",
             lunch_real_off_session_and_replay);
    run_case("C3-C4-nominal-clipped-labels-and-duplicate-refusal",
             "C3/C4 nominal clipped labels and duplicate refusal",
             nominal_and_clipped_confirmed_labels);
    run_case("C3-Friday-close-Monday-opening-and-missing-slot-refusal",
             "C3 Friday close Monday opening and missing-slot refusal",
             friday_monday_continuity);
    run_case("C3-overnight-first-origin-and-reordered-later-windows",
             "C3 overnight first origin and reordered later windows",
             overnight_cycle_origin_and_window_permutations);
    run_case("C5-whole-W-to-M-and-2D-to-W-straddling-inputs",
             "C5 whole W-to-M and 2D-to-W straddling inputs",
             cross_calendar_straddles);
    run_case("C7-same-overnight-cycle-date-for-nD-nW-nM",
             "C7 same overnight cycle date for nD/nW/nM",
             cycle_date_week_month_and_counts);
    run_case("C8-masked-and-DST-empty-cycles-preserve-attribution",
             "C8 masked and DST-empty cycles preserve attribution",
             masked_and_gap_empty_cycle_attribution);
    run_case("C9-partial-coarser-warmup-carries-into-confirmed-realtime",
             "C9 partial coarser warmup carries into confirmed realtime",
             partial_coarser_warmup_confirmed);
    run_case("C9-RTH-scheduled-close-does-not-seal-unfinished-daily-warmup",
             "C9 RTH scheduled close does not seal unfinished daily warmup",
             rth_partial_daily_warmup_seal);
    run_case("D1-same-timestamp-sequence-and-atomic-tick-array-refusal",
             "D1 same timestamp sequence and atomic tick-array refusal",
             ticks_same_timestamp_and_atomic_refusal);
    run_case("D1-tick-finalization-cannot-replay-pre-entry-extrema",
             "D1 tick finalization cannot replay pre-entry extrema",
             no_preentry_tick_extrema_replay);
    run_case("D2-carried-opening-eligibility-uses-immutable-birth-floor",
             "D2 carried opening eligibility uses immutable birth floor",
             quiet_birth_floor);
    run_case("D3-confirmed-after-calculation-point-is-later-than-callback",
             "D3 confirmed after-calculation point is later than callback",
             confirmed_close_policy);
    run_case("D4-external-birth-after-child-inputs-cannot-use-delayed-opening",
             "D4 external birth after child inputs cannot use delayed opening",
             delayed_open_after_external_birth);
    run_case("D4-C9-mixed-warmup-ticks-quiet-aggregation-without-price-replay",
             "D4/C9 mixed warmup ticks quiet aggregation without price replay",
             mixed_warmup_tick_quiet_aggregation);
    run_case("D5-partial-equal-TF-end-retains-actual-floor-and-source-time",
             "D5 partial equal-TF end retains actual floor and source time",
             partial_end_equal_clock_and_policy);
    run_case("D5-partial-child-does-not-force-coarser-callback",
             "D5 partial child does not force coarser callback",
             partial_end_does_not_seal_coarser_script);

    if (proof_dir && *proof_dir) {
        if (!write_proof(arts, proof_sha)) {
            std::printf("FAIL could not write native scenario artifact\n");
            return 1;
        }
    }

    std::printf("%s native calendar driver: %d scenarios, %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", scenarios, checks, failures);
    return failures ? 1 : 0;
}
