// Real CLI fixture module, driven by cr559_map_capacity_cli.py. Against the
// frozen base its first 120,000-pair run fails at pair 50,001. No codegen or
// private fixtures are needed; inputs select population and integer/string keys.
#include <pineforge/pineforge.h>
#include <pineforge/checked_settings.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/map.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace pineforge;
using Scalar = std::int64_t;

class MapHost final : public NativeStrategyHost {
public:
    MapHost() { declare_outputs(1, 0, 0); }

    void set_fixture_input(const std::string& key, const std::string& value) {
        if (key == "pairs") {
            std::size_t used = 0;
            const auto parsed = std::stoll(value, &used);
            if (used != value.size() || parsed < 0) throw std::invalid_argument("invalid pair count");
            wanted = parsed;
        } else if (key == "string_keys" && (value == "true" || value == "false")) {
            string_keys = value == "true";
        } else {
            throw std::invalid_argument("unknown map fixture input");
        }
    }
    void set_fixture_override(const std::string& key, const std::string& value) {
        if (key != "initial_capital") throw std::invalid_argument("unknown map fixture override");
        std::size_t used = 0;
        const auto parsed = std::stod(value, &used);
        if (used != value.size() || !std::isfinite(parsed) || parsed <= 0)
            throw std::invalid_argument("invalid initial capital");
        capital = parsed;
    }
    void on_native_run_begin() override {
        integers = {};
        strings = {};
        built = false;
        output_run_begin();
    }
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        if (!built) {
            if (string_keys) populate(strings);
            else populate(integers);
            built = true;
        }
        output_bar(context.script_bar_open_ms, context.script_interval.next_period_open_ms);
        output_value(0, static_cast<double>(string_keys ? strings.size() : integers.size()));
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        NativeStrategyHost::hash_host_extension(sink);
        sink.s("cr559/map-cli");
        sink.i(wanted);
        sink.b(string_keys);
        sink.b(settings_valid);
        sink.b(built);
        sink.d(capital);
        sink.u(runs);
        sink.b(integers.is_na());
        if (!integers.is_na())
            integers.hash_into(sink, [](auto& out, Scalar key) { out.i(key); },
                               [](auto& out, Scalar value) { out.i(value); });
        sink.b(strings.is_na());
        if (!strings.is_na())
            strings.hash_into(sink, [](auto& out, const std::string& key) { out.s(key); },
                              [](auto& out, Scalar value) { out.i(value); });
    }

    Scalar wanted = 0;
    bool string_keys = false;
    bool settings_valid = true;
    double capital = 10000.0;
    std::uint64_t runs = 0;

private:
    template <class Key>
    static Key key_for(Scalar index) {
        if constexpr (std::is_same_v<Key, std::string>)
            return "key:" + std::to_string(index * 37 + 11);
        else
            return index * 37 + 11;
    }
    static Scalar value_for(Scalar index) { return index * 101 + 19; }
    static void require(bool condition) {
        if (!condition)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                           "map CLI fixture lost size, membership or value");
    }
    template <class Key>
    void populate(PineMap<Key, Scalar>& map) {
        map = PineMap<Key, Scalar>::new_(); // First-bar creation must use the captured run policy.
        for (Scalar index = 0; index < wanted; ++index)
            require(is_na(map.put(key_for<Key>(index), value_for(index))));
        if (wanted != 0) require(map.put(key_for<Key>(0), -31) == value_for(0));
        require(map.size() == wanted);
        for (Scalar index = 0; index < wanted; ++index) {
            require(map.contains(key_for<Key>(index)));
            require(map.get(key_for<Key>(index)) == (index == 0 ? -31 : value_for(index)));
        }
    }
    PineMap<Scalar, Scalar> integers;
    PineMap<std::string, Scalar> strings;
    bool built = false;
};

MapHost& host_of(pf_strategy_t state) {
    return *static_cast<MapHost*>(static_cast<BacktestEngine*>(state));
}
} // namespace

extern "C" {
pf_strategy_t strategy_create(const char* settings) {
    if (settings && std::strcmp(settings, "{}") != 0) return nullptr;
    try { return static_cast<BacktestEngine*>(new MapHost()); }
    catch (...) { return nullptr; }
}
void strategy_free(pf_strategy_t state) {
    if (state) delete &host_of(state);
}
void strategy_set_input(pf_strategy_t state, const char* key, const char* value) {
    if (!state) return;
    auto& host = host_of(state);
    try {
        host.set_fixture_input(key ? key : "", value ? value : "");
    } catch (const std::exception& error) {
        host.settings_valid = false;
        note_run_failure(host, error.what(), RunFailureCode::setting_rejected,
                          {{"entrypoint", "strategy_set_input"}, {"reason", "unparseable_value"}});
    }
}
void strategy_set_override(pf_strategy_t state, const char* key, const char* value) {
    if (!state) return;
    auto& host = host_of(state);
    try {
        host.set_fixture_override(key ? key : "", value ? value : "");
    } catch (const std::exception& error) {
        host.settings_valid = false;
        note_run_failure(host, error.what(), RunFailureCode::setting_rejected,
                          {{"entrypoint", "strategy_set_override"}, {"reason", "unparseable_value"}});
    }
}
void run_backtest_full(pf_strategy_t state, pf_bar_t* bars, int count,
                       const char* input_tf, const char* script_tf, int magnifier, int,
                       pf_magnifier_distribution_t, pf_report_t* report) {
    if (report) std::memset(report, 0, sizeof(*report));
    if (!state) return;
    auto& host = host_of(state);
    if (!host.settings_valid) return;
    if (count < 0 || (count != 0 && !bars) || magnifier != 0) {
        note_run_failure(host, "invalid map fixture run arguments",
                          RunFailureCode::run_request_invalid, {{"option", "arguments"}});
        return;
    }
    NativeRunSpec spec;
    spec.identity.session_key = "cr559-map-fixture";
    spec.identity.run_number = ++host.runs;
    spec.input_tf = input_tf && *input_tf ? input_tf : "5";
    spec.script_tf = script_tf && *script_tf ? script_tf : spec.input_tf;
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = host.capital;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) return;
    std::vector<Bar> input;
    for (int index = 0; index < count; ++index)
        input.push_back({bars[index].open, bars[index].high, bars[index].low,
                         bars[index].close, bars[index].volume, bars[index].timestamp});
    host.run(input.data(), count);
    if (report && host.last_run_status() == 0 && host.last_error().empty())
        host.fill_report(reinterpret_cast<ReportC*>(report));
}
void report_free(pf_report_t* report) {
    BacktestEngine::free_report(reinterpret_cast<ReportC*>(report));
}
uint32_t strategy_outputs_api_version() { return PF_OUTPUTS_API_VERSION; }
int strategy_outputs_manifest(pf_strategy_t state, char* json, size_t capacity,
                              size_t* required, char* error, size_t error_capacity) {
    return checked_settings::boundary(error, error_capacity, [&] {
        checked_settings::require(state != nullptr, "null strategy");
        checked_settings::receipt(
            R"({"constants":[],"declaration":{"kind":"indicator","overlay":false,"title":"Map fixture"},"message_format":"pineforge/v1","outputs":[{"id":"size","index":0,"kind":"plot","line":1,"series":[0],"title":"Map size"}],"schema_version":"pineforge-outputs-manifest/v1","series":[{"output":"size","role":"value","slot":0}],"source_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})",
            json, capacity, required);
    });
}
} // extern "C"
