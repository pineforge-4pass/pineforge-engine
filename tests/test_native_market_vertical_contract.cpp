// Independent bounded native vertical contract (F1–F5, focused L1/L2).
// Expected finance/state come from R1-INTEGRATION-CONTRACT-v9 literals.
// Commands go through NativeStrategyHost + inherited run/stream only.

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using pineforge::Bar;
using pineforge::NativeCloseExecution;
using pineforge::NativeCompletion;
using pineforge::NativeDecisionContext;
using pineforge::NativeEventKind;
using pineforge::NativeFailureCode;
using pineforge::NativeFeeKind;
using pineforge::NativeLifecycleKind;
using pineforge::NativeMarketEvent;
using pineforge::NativeOpenDirections;
using pineforge::NativePriceProvenance;
using pineforge::NativeRunPhase;
using pineforge::NativeRunSpec;
using pineforge::NativeSetupStatus;
using pineforge::NativeStrategyHost;
using pineforge::execution::Flatten;
using pineforge::native_order::CancelResult;
using pineforge::native_order::CancelStatus;
using pineforge::native_order::CancelledEvent;
using pineforge::native_order::CommandEvent;
using pineforge::native_order::ExecutionAppliedEvent;
using pineforge::native_order::InvalidHandleEvent;
using pineforge::native_order::MatchRejectedEvent;
using pineforge::native_order::MatchRejectReason;
using pineforge::native_order::NoEffectEvent;
using pineforge::native_order::NotWorkingEvent;
using pineforge::native_order::RejectedEvent;
using pineforge::native_order::ReplaceRejectedEvent;
using pineforge::native_order::ReplaceResult;
using pineforge::native_order::ReplaceStatus;
using pineforge::native_order::ReplacedEvent;
using pineforge::native_order::Request;
using pineforge::native_order::RequestHandle;
using pineforge::native_order::RequestRejectReason;
using pineforge::native_order::SubmitResult;
using pineforge::native_order::SubmitStatus;
using pineforge::native_order::AcceptedEvent;
using pineforge::order_action::Reduce;
using pineforge::order_action::Transact;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(x)                                                              \
    do {                                                                      \
        ++checks;                                                             \
        if (!(x)) {                                                           \
            ++failures;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);          \
        }                                                                     \
    } while (0)

void near(double actual, double expected) {
    const double scale = std::max(1.0, std::max(std::abs(actual), std::abs(expected)));
    const bool ok = std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected) <= 1e-12 * scale;
    if (!ok) {
        std::printf("actual=%.17g expected=%.17g\n", actual, expected);
    }
    CHECK(ok);
}

constexpr int64_t kMinute = 60000;
// Independent 1-minute UTC 24x7 grid: 2025-01-06 14:30:00 UTC.
constexpr int64_t kT0 = 1736173800000LL;
constexpr int64_t kT1 = kT0 + kMinute;
constexpr int64_t kT2 = kT0 + 2 * kMinute;
constexpr int64_t kT3 = kT0 + 3 * kMinute;
constexpr double kCapital = 10000.0;
constexpr double kFee = 6.0;
constexpr double kOpen100 = 100.0;
constexpr double kOpen102 = 102.0;
constexpr double kOpen104 = 104.0;
constexpr double kRawOffTick = 100.004;
constexpr double kTick = 0.01;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar bar_at(int64_t open_ms, double o, double h, double l, double c) {
    return Bar{o, h, l, c, 1.0, open_ms};
}

Bar b100(int64_t t) { return bar_at(t, kOpen100, 101.0, 99.0, 100.5); }
Bar b102(int64_t t) { return bar_at(t, kOpen102, 103.0, 101.0, 102.5); }
Bar b103(int64_t t) { return bar_at(t, 103.0, 104.0, 102.0, 103.5); }
Bar b104(int64_t t) { return bar_at(t, kOpen104, 105.0, 103.0, 104.5); }
Bar b_raw(int64_t t) { return bar_at(t, kRawOffTick, 100.01, 100.0, kRawOffTick); }

Request tx(double q, const char* label = "", const char* comment = "") {
    return Request{Transact{q}, label, comment};
}
Request rd(double q, const char* label = "", const char* comment = "") {
    return pineforge::native_order::market_request(Reduce{q}, label, comment);
}
Request flat(const char* label = "", const char* comment = "") {
    return Request{Flatten{}, label, comment};
}

NativeRunSpec spec_for(const std::string& key, uint64_t run) {
    NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = run;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "NV";
    spec.tickerid = "TEST:NV";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native-contract";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone.clear();
    spec.initial_capital = kCapital;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = kTick;
    spec.slippage_ticks = 0;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = kFee;
    spec.close_execution = NativeCloseExecution::NextEligiblePoint;
    spec.allowed_open_directions = NativeOpenDirections::Both;
    return spec;
}

class FixtureHost final : public NativeStrategyHost {
public:
    std::function<void(FixtureHost&, const Bar&, const NativeDecisionContext&)> script;
    int callbacks = 0;
    RequestHandle live{};
    RequestHandle predecessor{};
    RequestHandle foreign{};
    SubmitResult submitted{};
    SubmitResult rejected{};
    SubmitResult rejected_reduce{};
    SubmitResult rejected_grid{};
    ReplaceResult nan_replace{};
    ReplaceResult replaced{};
    CancelResult cancelled{};
    CancelResult stale{};
    CancelResult foreign_cancel{};
    bool source_caught = false;

    void invoke_source_entry() { strategy_entry("legacy", true); }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        ++callbacks;
        if (script) script(*this, bar, context);
    }
};

struct FedBar {
    const char* kind;
    uint64_t ordinal;
    int64_t time_ms;
    double price;
};

struct ScenarioArt {
    std::string id;
    std::string status = "passed";
    std::string native_configuration;
    std::string run_identity;
    std::string calendar;
    std::string logical_inputs;
    std::string lifecycle_events;
    std::string physical_effects;
    std::string observations;
    std::string comparisons = "[]";
};

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

[[maybe_unused]] bool is_sha256_hex(const char* s) {
    if (!s) return false;
    std::size_t n = 0;
    for (; s[n]; ++n) {
        const char c = s[n];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return n == 64;
}

const char* event_kind_name(const CommandEvent& event) {
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
        if constexpr (std::is_same_v<T, pineforge::native_order::CloseBoundEvent>) {
            return "CloseBound";
        }
        if constexpr (std::is_same_v<T, pineforge::native_order::ActivatedEvent>) return "Activated";
        if constexpr (std::is_same_v<T, pineforge::native_order::ReservationReducedEvent>) {
            return "ReservationReduced";
        }
        if constexpr (std::is_same_v<T, pineforge::native_order::DeferredGroupAdjustmentEvent>) {
            return "DeferredGroupAdjustment";
        }
        if constexpr (std::is_same_v<T, pineforge::native_order::QuantityBoundEvent>) {
            return "QuantityBound";
        }
        if constexpr (std::is_same_v<T, pineforge::native_order::ArmedEvent>) return "Armed";
        return "Unknown";
    }, event);
}

bool r1_command_event(const CommandEvent& event) {
    const char* kind = event_kind_name(event);
    return std::strcmp(kind, "Accepted") == 0
        || std::strcmp(kind, "Rejected") == 0
        || std::strcmp(kind, "Replaced") == 0
        || std::strcmp(kind, "ReplaceRejected") == 0
        || std::strcmp(kind, "Cancelled") == 0
        || std::strcmp(kind, "NotWorking") == 0
        || std::strcmp(kind, "InvalidHandle") == 0
        || std::strcmp(kind, "NoEffect") == 0
        || std::strcmp(kind, "MatchRejected") == 0
        || std::strcmp(kind, "ExecutionApplied") == 0;
}

uint64_t event_ordinal(const CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

template <class T>
const T* as_event(const CommandEvent& event) {
    return std::get_if<T>(&event);
}

std::vector<CommandEvent> copy_commands(const FixtureHost& host) {
    std::vector<CommandEvent> out;
    for (const auto& row : host.native_events(0)) {
        if (row.kind == NativeEventKind::Command && row.command.has_value()) {
            out.push_back(*row.command);
        }
    }
    return out;
}

int count_applied(const std::vector<CommandEvent>& events) {
    int n = 0;
    for (const auto& e : events) if (as_event<ExecutionAppliedEvent>(e)) ++n;
    return n;
}

int count_kind(const std::vector<CommandEvent>& events, const char* kind) {
    int n = 0;
    for (const auto& e : events) if (std::strcmp(event_kind_name(e), kind) == 0) ++n;
    return n;
}

double sum_tickets(const std::vector<CommandEvent>& events) {
    double sum = 0.0;
    for (const auto& e : events) {
        if (const auto* applied = as_event<ExecutionAppliedEvent>(e)) {
            sum += applied->current_ticket;
        }
    }
    return sum;
}

const ExecutionAppliedEvent* applied_at(const std::vector<CommandEvent>& events, int index) {
    int n = 0;
    for (const auto& e : events) {
        if (const auto* applied = as_event<ExecutionAppliedEvent>(e)) {
            if (n == index) return applied;
            ++n;
        }
    }
    return nullptr;
}

std::string lifecycle_json(const std::vector<CommandEvent>& events) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& event : events) {
        if (!r1_command_event(event)) continue;
        if (!first) out << ",";
        first = false;
        out << "{\"kind\":" << json_escape(event_kind_name(event))
            << ",\"ordinal\":" << json_u64(event_ordinal(event));
        if (as_event<ExecutionAppliedEvent>(event)) {
            out << ",\"physicalEffectsPresent\":true";
        }
        if (const auto* rejected = as_event<RejectedEvent>(event)) {
            out << ",\"reason\":" << json_u64(static_cast<uint64_t>(rejected->reason));
        }
        if (const auto* rr = as_event<ReplaceRejectedEvent>(event)) {
            out << ",\"reason\":" << json_u64(static_cast<uint64_t>(rr->reason));
        }
        if (const auto* mr = as_event<MatchRejectedEvent>(event)) {
            out << ",\"reason\":" << json_u64(static_cast<uint64_t>(mr->reason));
        }
        out << "}";
    }
    out << "]";
    return out.str();
}

std::string physical_json(const FixtureHost& host, const std::vector<CommandEvent>& events) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    auto comma = [&] {
        if (!first) out << ",";
        first = false;
    };
    for (const auto& event : events) {
        const auto* applied = as_event<ExecutionAppliedEvent>(event);
        if (!applied) continue;
        if (applied->closed_trade_count > 0) {
            const int idx = static_cast<int>(applied->first_trade_index);
            comma();
            out << "{\"kind\":\"CloseLot\",\"ordinal\":" << json_u64(applied->ordinal)
                << ",\"price\":" << json_num(applied->resolved_price)
                << ",\"timestampMs\":" << json_i64(applied->effective_time_ms())
                << ",\"provenance\":" << json_u64(applied->provenance());
            if (idx >= 0 && idx < host.trade_count()) {
                out << ",\"quantity\":" << json_num(host.get_trade(idx).qty);
            }
            out << "}";
        }
        if (applied->opened_lot_incarnation != 0) {
            comma();
            out << "{\"kind\":\"OpenLot\",\"ordinal\":" << json_u64(applied->ordinal)
                << ",\"price\":" << json_num(applied->resolved_price)
                << ",\"timestampMs\":" << json_i64(applied->effective_time_ms())
                << ",\"provenance\":" << json_u64(applied->provenance())
                << "}";
        }
        if (applied->closed_trade_count == 0 && applied->opened_lot_incarnation == 0) {
            comma();
            out << "{\"kind\":\"ExecutionApplied\",\"ordinal\":" << json_u64(applied->ordinal)
                << ",\"price\":" << json_num(applied->resolved_price)
                << ",\"timestampMs\":" << json_i64(applied->effective_time_ms())
                << "}";
        }
    }
    out << "]";
    return out.str();
}

std::string inputs_json(const std::vector<FedBar>& inputs) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (i) out << ",";
        const auto& in = inputs[i];
        out << "{\"kind\":" << json_escape(in.kind)
            << ",\"ordinal\":" << json_u64(in.ordinal)
            << ",\"effectiveTimeMs\":" << json_i64(in.time_ms)
            << ",\"price\":" << json_num(in.price) << "}";
    }
    out << "]";
    return out.str();
}

std::string default_calendar_json() {
    return "{\"timezone\":\"UTC\",\"session\":\"24x7\",\"inputTf\":\"1\","
           "\"scriptTf\":\"1\",\"chartTimezone\":\"\"}";
}

std::string config_json(const NativeRunSpec& spec) {
    std::ostringstream out;
    out << "{\"initialCapital\":" << json_num(spec.initial_capital)
        << ",\"feeValue\":" << json_num(spec.fee_value)
        << ",\"feeKind\":" << json_u64(static_cast<uint64_t>(spec.fee_kind))
        << ",\"priceTick\":" << json_num(spec.price_tick)
        << ",\"pointValue\":" << json_num(spec.point_value)
        << ",\"accountFx\":" << json_num(spec.account_fx)
        << ",\"slippageTicks\":" << json_u64(spec.slippage_ticks) << "}";
    return out.str();
}

std::string identity_json(const NativeRunSpec& spec) {
    std::ostringstream out;
    out << "{\"sessionKey\":" << json_escape(spec.identity.session_key)
        << ",\"runNumber\":" << json_u64(spec.identity.run_number) << "}";
    return out.str();
}

std::string observations_json(const FixtureHost& host, double mark,
                              const std::vector<CommandEvent>& events) {
    const auto pos = host.physical_position();
    std::ostringstream out;
    out << "{\"signedUnits\":" << json_num(pos.signed_units)
        << ",\"lotCount\":" << json_u64(pos.lot_count)
        << ",\"markedEquity\":" << json_num(host.native_marked_equity(mark))
        << ",\"currentTickets\":" << json_num(sum_tickets(events))
        << ",\"appliedCount\":" << count_applied(events)
        << ",\"tradeCount\":" << host.trade_count()
        << ",\"lifecycleKind\":" << json_u64(static_cast<uint64_t>(host.native_state().kind))
        << ",\"consumedHighWater\":" << json_u64(host.native_consumed_high_water())
        << ",\"callbacks\":" << host.callbacks << "}";
    return out.str();
}

ScenarioArt make_art(const std::string& id, const NativeRunSpec& spec,
                     const FixtureHost& host, const std::vector<FedBar>& inputs,
                     double mark, int fail_before) {
    const auto events = copy_commands(host);
    ScenarioArt s;
    s.id = id;
    s.status = (failures == fail_before) ? "passed" : "failed";
    s.native_configuration = config_json(spec);
    s.run_identity = identity_json(spec);
    s.calendar = default_calendar_json();
    s.logical_inputs = inputs_json(inputs);
    s.lifecycle_events = lifecycle_json(events);
    s.physical_effects = physical_json(host, events);
    s.observations = observations_json(host, mark, events);
    return s;
}

void require_applied_setup(FixtureHost& host, const NativeRunSpec& spec) {
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
}

void expect_completed(const FixtureHost& host) {
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

void expect_modeled_open_fill(const ExecutionAppliedEvent* applied,
                              double raw, double resolved, int64_t time_ms) {
    CHECK(applied != nullptr);
    if (!applied) return;
    near(applied->raw_price, raw);
    near(applied->resolved_price, resolved);
    CHECK(applied->effective_time_ms() == time_ms);
    CHECK(applied->provenance()
          == static_cast<std::uint8_t>(NativePriceProvenance::ModeledOHLCOpen));
    near(applied->current_ticket, kFee);
}

void bind_pair(std::vector<ScenarioArt>& arts, const char* name,
               const char* left_id, const char* right_id, const char* fields) {
    ScenarioArt* left = nullptr;
    ScenarioArt* right = nullptr;
    for (auto& s : arts) {
        if (s.id == left_id) left = &s;
        if (s.id == right_id) right = &s;
    }
    if (!left || !right) return;
    const bool same_effects = left->physical_effects == right->physical_effects;
    const bool same_obs = left->observations == right->observations;
    const bool pass = same_effects && same_obs
        && left->status == "passed" && right->status == "passed";
    std::ostringstream cmp;
    cmp << "[{\"name\":" << json_escape(name)
        << ",\"pass\":" << (pass ? "true" : "false")
        << ",\"leftScenarioId\":" << json_escape(left_id)
        << ",\"rightScenarioId\":" << json_escape(right_id)
        << ",\"fields\":" << fields << "}]";
    left->comparisons = cmp.str();
    right->comparisons = cmp.str();
}

[[maybe_unused]] bool write_proof(const std::vector<ScenarioArt>& scenarios, const char* sha) {
    const char* dir = std::getenv("PINEFORGE_NATIVE_PROOF_OUTPUT");
    if (!dir || !*dir) return true;
    std::ostringstream json;
    json << "{\n  \"schemaVersion\": \"pineforge-native-scenario-artifact/v1\",\n"
         << "  \"testName\": \"test_native_market_vertical_contract\",\n"
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
    const std::string path = std::string(dir) + "/test_native_market_vertical_contract.json";
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) return false;
    out << json.str();
    return static_cast<bool>(out);
}

}  // namespace

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
#endif
    }

    std::vector<ScenarioArt> arts;

    const Bar two_long[] = {b100(kT0), b102(kT1)};
    const Bar three_cross[] = {b100(kT0), b102(kT1), b104(kT2)};
    const Bar four_exit[] = {b100(kT0), b102(kT1), b103(kT2), b104(kT3)};
    const Bar four_raw[] = {b100(kT0), b_raw(kT1), b103(kT2), b_raw(kT3)};

    const std::vector<FedBar> fed_two = {
        {"confirmedBar", 0, kT0, kOpen100},
        {"confirmedBar", 1, kT1, kOpen102},
    };
    const std::vector<FedBar> fed_three = {
        {"confirmedBar", 0, kT0, kOpen100},
        {"confirmedBar", 1, kT1, kOpen102},
        {"confirmedBar", 2, kT2, kOpen104},
    };
    const std::vector<FedBar> fed_four = {
        {"confirmedBar", 0, kT0, kOpen100},
        {"confirmedBar", 1, kT1, kOpen102},
        {"confirmedBar", 2, kT2, 103.0},
        {"confirmedBar", 3, kT3, kOpen104},
    };
    const std::vector<FedBar> fed_raw = {
        {"confirmedBar", 0, kT0, kOpen100},
        {"confirmedBar", 1, kT1, kRawOffTick},
        {"confirmedBar", 2, kT2, 103.0},
        {"confirmedBar", 3, kT3, kRawOffTick},
    };

    // F1 long: queued Transact{+1} fills exactly once at next eligible open 102.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F1-long-queued-open", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                h.submitted = h.submit_market(tx(1.0, "long"));
                CHECK(h.submitted.status == SubmitStatus::Accepted);
                CHECK(h.submitted.handle.has_value());
                h.live = *h.submitted.handle;
                CHECK(h.live.incarnation != 0);
                near(h.physical_position().signed_units, 0.0);
            }
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2, "1", "1");
        expect_completed(host);
        CHECK(host.callbacks == 2);
        CHECK(host.native_state().completion == NativeCompletion::BatchComplete);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 1);
        CHECK(count_kind(events, "Accepted") == 1);
        expect_modeled_open_fill(applied_at(events, 0), kOpen102, kOpen102, kT1);
        near(host.physical_position().signed_units, 1.0);
        CHECK(host.physical_position().lot_count == 1);
        near(host.native_marked_equity(kOpen102), 9994.0);
        near(sum_tickets(events), kFee);
        arts.push_back(make_art("R1-native-contract-F1-long-queued-open", spec, host,
                                fed_two, kOpen102, before));
    }

    // F1 short: opposite sign, same queued-open rule.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F1-short-queued-open", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                h.submitted = h.submit_market(tx(-1.0, "short"));
                CHECK(h.submitted.status == SubmitStatus::Accepted);
                near(h.physical_position().signed_units, 0.0);
            }
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 1);
        expect_modeled_open_fill(applied_at(events, 0), kOpen102, kOpen102, kT1);
        near(host.physical_position().signed_units, -1.0);
        near(host.native_marked_equity(kOpen102), 9994.0);
        arts.push_back(make_art("R1-native-contract-F1-short-queued-open", spec, host,
                                fed_two, kOpen102, before));
    }

    // F1 cancel before the eligible open: no fill; stale cancel is NotWorking.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F1-cancel-before-open", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                h.submitted = h.submit_market(tx(1.0, "long"));
                CHECK(h.submitted.status == SubmitStatus::Accepted);
                h.live = *h.submitted.handle;
                h.cancelled = h.cancel(h.live);
                CHECK(h.cancelled.status == CancelStatus::Cancelled);
                h.stale = h.cancel(h.live);
                CHECK(h.stale.status == CancelStatus::NotWorking);
            }
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 0);
        CHECK(count_kind(events, "Cancelled") == 1);
        CHECK(count_kind(events, "NotWorking") == 1);
        near(host.physical_position().signed_units, 0.0);
        near(host.native_marked_equity(kOpen102), kCapital);
        near(sum_tickets(events), 0.0);
        arts.push_back(make_art("R1-native-contract-F1-cancel-before-open", spec, host,
                                fed_two, kOpen102, before));
    }

    // F2: NaN replace preserves live row; valid +2 births new priority; invalid
    // qty/grid reject without a handle; cancel of predecessor is NotWorking.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F2-replace-priority", 1);
        spec.quantity_grid = 0.25;
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks != 1) return;
            h.submitted = h.submit_market(tx(1.0, "I"));
            CHECK(h.submitted.status == SubmitStatus::Accepted);
            h.live = *h.submitted.handle;
            h.nan_replace = h.replace_market(h.live, tx(kNaN, "nan"));
            CHECK(h.nan_replace.status == ReplaceStatus::ReplaceRejected);
            CHECK(h.nan_replace.reason == RequestRejectReason::InvalidQuantity);
            CHECK(!h.nan_replace.successor.has_value());
            h.rejected = h.submit_market(tx(0.0, "zero"));
            CHECK(h.rejected.status == SubmitStatus::Rejected);
            CHECK(!h.rejected.handle.has_value());
            h.rejected_reduce = h.submit_market(rd(-1.0, "neg"));
            CHECK(h.rejected_reduce.status == SubmitStatus::Rejected);
            CHECK(!h.rejected_reduce.handle.has_value());
            auto zero_reduce = h.submit_market(rd(0.0, "r0"));
            CHECK(zero_reduce.status == SubmitStatus::Rejected);
            h.rejected_grid = h.submit_market(tx(0.30, "off-grid"));
            CHECK(h.rejected_grid.status == SubmitStatus::Rejected);
            CHECK(h.rejected_grid.reason == RequestRejectReason::OffGrid);
            CHECK(!h.rejected_grid.handle.has_value());
            h.replaced = h.replace_market(h.live, tx(2.0, "J"));
            CHECK(h.replaced.status == ReplaceStatus::Replaced);
            CHECK(h.replaced.successor.has_value());
            h.predecessor = h.live;
            h.live = *h.replaced.successor;
            CHECK(h.live.incarnation != h.predecessor.incarnation);
            h.cancelled = h.cancel(h.predecessor);
            CHECK(h.cancelled.status == CancelStatus::NotWorking);
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_kind(events, "ReplaceRejected") >= 1);
        CHECK(count_kind(events, "Replaced") == 1);
        CHECK(count_applied(events) == 1);
        expect_modeled_open_fill(applied_at(events, 0), kOpen102, kOpen102, kT1);
        near(host.physical_position().signed_units, 2.0);
        near(host.native_marked_equity(kOpen102), 9994.0);
        near(sum_tickets(events), kFee);
        arts.push_back(make_art("R1-native-contract-F2-replace-priority", spec, host,
                                fed_two, kOpen102, before));
    }

    // F3: short1 then one-ticket Transact{+2} crossing; total tickets 12, equity 9986.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F3-short-cross-long", 1);
        spec.max_open_lots = 1; // The resulting crossing book contains one lot.
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(-1.0, "short"));
            if (h.callbacks == 2) h.submit_market(tx(2.0, "flip"));
        };
        require_applied_setup(host, spec);
        host.run(three_cross, 3);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 2);
        expect_modeled_open_fill(applied_at(events, 0), kOpen102, kOpen102, kT1);
        expect_modeled_open_fill(applied_at(events, 1), kOpen104, kOpen104, kT2);
        CHECK(applied_at(events, 1)->closed_trade_count >= 1);
        CHECK(applied_at(events, 1)->opened_lot_incarnation != 0);
        CHECK(host.trade_count() == 1);
        CHECK(host.get_trade(0).entry_time == kT1);
        CHECK(host.get_trade(0).exit_time == kT2);
        near(host.get_trade(0).entry_price, kOpen102);
        near(host.get_trade(0).exit_price, kOpen104);
        near(host.physical_position().signed_units, 1.0);
        near(sum_tickets(events), 12.0);
        near(host.native_marked_equity(kOpen104), 9986.0);
        arts.push_back(make_art("R1-native-contract-F3-short-cross-long", spec, host,
                                fed_three, kOpen104, before));
    }

    // F3 opposite orientation: long then Transact{-2}.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F3-long-cross-short", 1);
        spec.max_open_lots = 1; // The resulting crossing book contains one lot.
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "long"));
            if (h.callbacks == 2) h.submit_market(tx(-2.0, "flip"));
        };
        require_applied_setup(host, spec);
        host.run(three_cross, 3);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 2);
        CHECK(host.trade_count() == 1);
        CHECK(host.get_trade(0).entry_time == kT1);
        CHECK(host.get_trade(0).exit_time == kT2);
        near(host.physical_position().signed_units, -1.0);
        near(sum_tickets(events), 12.0);
        near(host.native_marked_equity(kOpen104), 9990.0);
        arts.push_back(make_art("R1-native-contract-F3-long-cross-short", spec, host,
                                fed_three, kOpen104, before));
    }

    // F3 Flatten queued to the next eligible open; two tickets; balance 9990.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F3-flatten-queued", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "long"));
            if (h.callbacks == 3) h.submit_market(flat("out"));
        };
        require_applied_setup(host, spec);
        host.run(four_exit, 4);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 2);
        expect_modeled_open_fill(applied_at(events, 1), kOpen104, kOpen104, kT3);
        CHECK(host.trade_count() == 1);
        CHECK(host.get_trade(0).entry_time == kT1);
        CHECK(host.get_trade(0).exit_time == kT3);
        CHECK(host.get_trade(0).entry_bar_index != host.get_trade(0).exit_bar_index);
        near(host.physical_position().signed_units, 0.0);
        near(sum_tickets(events), 12.0);
        near(host.native_marked_equity(kOpen104), 9990.0);
        arts.push_back(make_art("R1-native-contract-F3-flatten-queued", spec, host,
                                fed_four, kOpen104, before));
    }

    // F3 Reduce{5} clips to actual exposure; does not reverse.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F3-reduce-clips", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "long"));
            if (h.callbacks == 3) h.submit_market(rd(5.0, "clip"));
        };
        require_applied_setup(host, spec);
        host.run(four_exit, 4);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 2);
        CHECK(count_kind(events, "NoEffect") == 0);
        near(host.physical_position().signed_units, 0.0);
        near(sum_tickets(events), 12.0);
        near(host.native_marked_equity(kOpen104), 9990.0);
        CHECK(host.trade_count() == 1);
        CHECK(host.get_trade(0).entry_time == kT1);
        arts.push_back(make_art("R1-native-contract-F3-reduce-clips", spec, host,
                                fed_four, kOpen104, before));
    }

    // F3 flat Reduce/Flatten terminate NoEffect; no fee, no physical action.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F3-flat-no-effect", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                auto a = h.submit_market(rd(5.0, "flat-reduce"));
                auto b = h.submit_market(flat("flat-flatten"));
                CHECK(a.status == SubmitStatus::Accepted);
                CHECK(b.status == SubmitStatus::Accepted);
                near(h.physical_position().signed_units, 0.0);
            }
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 0);
        CHECK(count_kind(events, "NoEffect") == 2);
        CHECK(host.trade_count() == 0);
        near(host.physical_position().signed_units, 0.0);
        near(sum_tickets(events), 0.0);
        near(host.native_marked_equity(kOpen102), kCapital);
        arts.push_back(make_art("R1-native-contract-F3-flat-no-effect", spec, host,
                                fed_two, kOpen102, before));
    }

    // F4 same-point +2, Reduce1, Flatten sees preceding effects; three tickets 18.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F4-same-point-order", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                CHECK(h.submit_market(tx(2.0, "add")).status == SubmitStatus::Accepted);
                CHECK(h.submit_market(rd(1.0, "clip")).status == SubmitStatus::Accepted);
                CHECK(h.submit_market(flat("out")).status == SubmitStatus::Accepted);
            }
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 3);
        CHECK(count_kind(events, "NoEffect") == 0);
        for (int i = 0; i < 3; ++i) {
            expect_modeled_open_fill(applied_at(events, i), kOpen102, kOpen102, kT1);
        }
        near(host.physical_position().signed_units, 0.0);
        near(sum_tickets(events), 18.0);
        near(host.native_marked_equity(kOpen102), 9982.0);
        arts.push_back(make_art("R1-native-contract-F4-same-point-order", spec, host,
                                fed_two, kOpen102, before));
    }

    // F4 resulting-unit denial rejects the whole crossing before closing old exposure.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F4-units-reject-crossing", 1);
        spec.max_abs_units = 1.0;
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(-1.0, "short"));
            if (h.callbacks == 2) h.submit_market(tx(3.0, "flip"));
        };
        require_applied_setup(host, spec);
        host.run(three_cross, 3);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 1);
        CHECK(count_kind(events, "MatchRejected") == 1);
        bool units_reason = false;
        for (const auto& e : events) {
            if (const auto* mr = as_event<MatchRejectedEvent>(e)) {
                units_reason = mr->reason == MatchRejectReason::MaxAbsUnits;
            }
        }
        CHECK(units_reason);
        CHECK(host.trade_count() == 0);
        near(host.physical_position().signed_units, -1.0);
        CHECK(host.physical_position().lot_count == 1);
        near(sum_tickets(events), kFee);
        near(host.native_marked_equity(kOpen102), 9994.0);
        near(host.native_marked_equity(kOpen104), 9992.0);
        arts.push_back(make_art("R1-native-contract-F4-units-reject-crossing", spec, host,
                                fed_three, kOpen104, before));
    }

    // F4: adding a same-side lot would exceed the resulting-book lot limit.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F4-lots-reject-add", 1);
        spec.max_open_lots = 1;
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "first"));
            if (h.callbacks == 2) h.submit_market(tx(1.0, "second"));
        };
        require_applied_setup(host, spec);
        host.run(three_cross, 3);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 1);
        CHECK(count_kind(events, "MatchRejected") == 1);
        bool lots_reason = false;
        for (const auto& e : events) {
            if (const auto* mr = as_event<MatchRejectedEvent>(e))
                lots_reason = mr->reason == MatchRejectReason::MaxOpenLots;
        }
        CHECK(lots_reason);
        CHECK(host.trade_count() == 0);
        near(host.physical_position().signed_units, 1.0);
        CHECK(host.physical_position().lot_count == 1);
        near(sum_tickets(events), kFee);
        near(host.native_marked_equity(kOpen104), 9996.0);
        arts.push_back(make_art("R1-native-contract-F4-lots-reject-add", spec, host,
                                fed_three, kOpen104, before));
    }

    // F4 direction denial of the opening remainder also refuses the close leg.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F4-direction-reject-crossing", 1);
        spec.allowed_open_directions = NativeOpenDirections::Long;
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "long"));
            if (h.callbacks == 2) h.submit_market(tx(-2.0, "flip"));
        };
        require_applied_setup(host, spec);
        host.run(three_cross, 3);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 1);
        CHECK(count_kind(events, "MatchRejected") == 1);
        bool dir_reason = false;
        for (const auto& e : events) {
            if (const auto* mr = as_event<MatchRejectedEvent>(e)) {
                dir_reason = mr->reason == MatchRejectReason::OpeningDirection;
            }
        }
        CHECK(dir_reason);
        CHECK(host.trade_count() == 0);
        near(host.physical_position().signed_units, 1.0);
        near(host.native_marked_equity(kOpen104), 9996.0);
        arts.push_back(make_art("R1-native-contract-F4-direction-reject-crossing", spec, host,
                                fed_three, kOpen104, before));
    }

    // F5 raw off-tick, slip 0: both sides keep 100.004; close preserves entry time.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F5-raw-off-tick", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "long"));
            if (h.callbacks == 3) h.submit_market(flat("out"));
        };
        require_applied_setup(host, spec);
        host.run(four_raw, 4);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 2);
        expect_modeled_open_fill(applied_at(events, 0), kRawOffTick, kRawOffTick, kT1);
        expect_modeled_open_fill(applied_at(events, 1), kRawOffTick, kRawOffTick, kT3);
        CHECK(host.trade_count() == 1);
        near(host.get_trade(0).entry_price, kRawOffTick);
        near(host.get_trade(0).exit_price, kRawOffTick);
        CHECK(host.get_trade(0).entry_time == kT1);
        CHECK(host.get_trade(0).exit_time == kT3);
        arts.push_back(make_art("R1-native-contract-F5-raw-off-tick", spec, host,
                                fed_raw, kRawOffTick, before));
    }

    // F5 slip 2 ticks: buy plus / sell minus once; no mintick snap.
    {
        const int before = failures;
        const double buy = kRawOffTick + 2.0 * kTick;
        const double sell = kRawOffTick - 2.0 * kTick;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F5-slippage-two-ticks", 1);
        spec.slippage_ticks = 2;
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "long"));
            if (h.callbacks == 3) h.submit_market(flat("out"));
        };
        require_applied_setup(host, spec);
        host.run(four_raw, 4);
        expect_completed(host);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 2);
        expect_modeled_open_fill(applied_at(events, 0), kRawOffTick, buy, kT1);
        expect_modeled_open_fill(applied_at(events, 1), kRawOffTick, sell, kT3);
        CHECK(host.trade_count() == 1);
        near(host.get_trade(0).entry_price, buy);
        near(host.get_trade(0).exit_price, sell);
        CHECK(host.get_trade(0).entry_time == kT1);
        CHECK(host.get_trade(0).exit_time == kT3);
        arts.push_back(make_art("R1-native-contract-F5-slippage-two-ticks", spec, host,
                                fed_raw, sell, before));
    }

    // L1 Ready reuse / invalid specification.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-L1-ready-reuse", 1);
        require_applied_setup(host, spec);
        const auto again = host.configure_native(spec);
        CHECK(again.status == NativeSetupStatus::Failed);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::Contract);
        CHECK(host.native_consumed_high_water() == 0);
        host.run(two_long, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        near(host.physical_position().signed_units, 0.0);

        FixtureHost bad;
        auto invalid = spec_for("R1-native-contract-L1-ready-reuse", 1);
        invalid.identity.session_key.clear();
        const auto rejected = bad.configure_native(invalid);
        CHECK(rejected.status == NativeSetupStatus::Failed);
        CHECK(bad.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(bad.native_state().failure.code == NativeFailureCode::InvalidSpecification);
        arts.push_back(make_art("R1-native-contract-L1-ready-reuse", spec, host,
                                {}, kOpen100, before));
    }

    // L1 conflicting timeframe args refuse before identity consumption.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-L1-timeframe-conflict", 1);
        require_applied_setup(host, spec);
        host.run(two_long, 2, "5", "1");
        CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(host.native_consumed_high_water() == 0);
        CHECK(!host.last_error().empty());
        host.run(two_long, 2, "", "");
        expect_completed(host);
        CHECK(host.native_consumed_high_water() == 1);
        arts.push_back(make_art("R1-native-contract-L1-timeframe-conflict", spec, host,
                                fed_two, kOpen102, before));
    }

    // L1 completed → next run; foreign/old handles cannot cancel the new request.
    {
        const int before = failures;
        FixtureHost host;
        auto spec1 = spec_for("R1-native-contract-L1-completed-next-run", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                h.submitted = h.submit_market(tx(1.0, "n1"));
                h.live = *h.submitted.handle;
            }
        };
        require_applied_setup(host, spec1);
        host.run(two_long, 2);
        expect_completed(host);
        host.foreign = host.live;
        CHECK(host.native_consumed_high_water() == 1);
        auto spec2 = spec_for("R1-native-contract-L1-completed-next-run", 2);
        host.callbacks = 0;
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                h.foreign_cancel = h.cancel(h.foreign);
                CHECK(h.foreign_cancel.status == CancelStatus::InvalidHandle);
                RequestHandle other;
                other.run.session_key = "other-session";
                other.run.run_number = 1;
                other.incarnation = 1;
                auto also = h.cancel(other);
                CHECK(also.status == CancelStatus::InvalidHandle);
                h.submitted = h.submit_market(tx(-1.0, "n2"));
                CHECK(h.submitted.status == SubmitStatus::Accepted);
            }
        };
        CHECK(host.configure_native(spec2).status == NativeSetupStatus::Applied);
        CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
        host.run(two_long, 2);
        expect_completed(host);
        CHECK(host.native_consumed_high_water() == 2);
        const auto events = copy_commands(host);
        CHECK(count_kind(events, "InvalidHandle") >= 1);
        CHECK(count_applied(events) == 1);
        near(host.physical_position().signed_units, -1.0);
        arts.push_back(make_art("R1-native-contract-L1-completed-next-run", spec2, host,
                                fed_two, kOpen102, before));
    }

    // L1 warmup → realtime carry-over: queued warmup request fills on first realtime open.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-L1-warmup-realtime-carry", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) {
                h.submitted = h.submit_market(tx(1.0, "carry"));
                CHECK(h.submitted.status == SubmitStatus::Accepted);
                near(h.physical_position().signed_units, 0.0);
            }
        };
        require_applied_setup(host, spec);
        const Bar warmup[] = {b100(kT0)};
        CHECK(host.stream_begin(warmup, 1, "1", "1"));
        CHECK(host.native_state().kind == NativeLifecycleKind::Running);
        CHECK(host.native_state().phase == NativeRunPhase::Realtime);
        near(host.physical_position().signed_units, 0.0);
        CHECK(host.stream_push_bar(b102(kT1)));
        CHECK(host.stream_end(false));
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.native_state().completion == NativeCompletion::StreamEnded);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 1);
        expect_modeled_open_fill(applied_at(events, 0), kOpen102, kOpen102, kT1);
        near(host.physical_position().signed_units, 1.0);
        near(host.native_marked_equity(kOpen102), 9994.0);
        const std::vector<FedBar> fed_stream = {
            {"warmupBar", 0, kT0, kOpen100},
            {"realtimeBar", 1, kT1, kOpen102},
        };
        arts.push_back(make_art("R1-native-contract-L1-warmup-realtime-carry", spec, host,
                                fed_stream, kOpen102, before));
    }

    // L1 same-N fresh replay of the F1 long path.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-F1-long-queued-open", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks == 1) h.submit_market(tx(1.0, "long"));
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2, "1", "1");
        expect_completed(host);
        CHECK(host.native_consumed_high_water() == 1);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 1);
        near(host.physical_position().signed_units, 1.0);
        near(host.native_marked_equity(kOpen102), 9994.0);
        arts.push_back(make_art("R1-native-contract-L1-fresh-replay", spec, host,
                                fed_two, kOpen102, before));
    }

    // L1 callback throw latches Failed; later advancement refuses.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-L1-callback-throw", 1);
        host.script = [](FixtureHost&, const Bar&, const NativeDecisionContext&) {
            throw std::runtime_error("native-contract-callback");
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::CallbackException);
        CHECK(!host.last_error().empty());
        host.run(two_long, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::CallbackException);
        near(host.physical_position().signed_units, 0.0);
        arts.push_back(make_art("R1-native-contract-L1-callback-throw", spec, host,
                                fed_two, kOpen100, before));
    }

    // L1/L2 forbidden source call, even if the strategy catches it.
    {
        const int before = failures;
        FixtureHost host;
        auto spec = spec_for("R1-native-contract-L1-forbidden-source", 1);
        host.script = [](FixtureHost& h, const Bar&, const NativeDecisionContext&) {
            if (h.callbacks != 1) return;
            try {
                h.invoke_source_entry();
            } catch (...) {
                h.source_caught = true;
            }
            CHECK(h.source_caught);
            try {
                h.submitted = h.submit_market(tx(1.0, "after-source"));
            } catch (...) {
            }
        };
        require_applied_setup(host, spec);
        host.run(two_long, 2);
        CHECK(host.source_caught);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(host.native_state().failure.code == NativeFailureCode::UnsupportedSource);
        const auto events = copy_commands(host);
        CHECK(count_applied(events) == 0);
        near(host.physical_position().signed_units, 0.0);
        host.run(two_long, 2);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);

        FixtureHost ready_guard;
        auto spec_guard = spec_for("R1-native-contract-L1-forbidden-source", 1);
        require_applied_setup(ready_guard, spec_guard);
        bool set_input_threw = false;
        try {
            ready_guard.set_input("qty", "1");
        } catch (...) {
            set_input_threw = true;
        }
        CHECK(set_input_threw);
        CHECK(ready_guard.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(ready_guard.native_state().failure.code == NativeFailureCode::UnsupportedSource);
        arts.push_back(make_art("R1-native-contract-L1-forbidden-source", spec, host,
                                fed_two, kOpen100, before));
    }

    bind_pair(arts, "R1-native-contract-fresh-replay",
              "R1-native-contract-F1-long-queued-open",
              "R1-native-contract-L1-fresh-replay",
              "[\"physicalEffects\",\"observations\"]");

    if (proof_dir && *proof_dir) {
        if (!write_proof(arts, proof_sha)) {
            std::printf("FAIL could not write native scenario artifact\n");
            return 1;
        }
    }

    std::printf("%s %d checks %d failures %zu scenarios\n",
                failures ? "FAIL" : "PASS", checks, failures, arts.size());
    return failures ? 1 : 0;
}
