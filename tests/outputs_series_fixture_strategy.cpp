// A native strategy module with an operator-independent, constructor-time
// declaration. The test supplies its shape through the fixture environment.
#include <pineforge/pineforge.h>
#include <pineforge/checked_settings.hpp>
#include <pineforge/native_host.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

class SeriesHost final : public NativeStrategyHost {
public:
    SeriesHost() {
        const char* text = std::getenv("PINEFORGE_TEST_DECLARED_SERIES");
        slots = text ? std::stoi(text) : 65;
        declare_outputs(slots, 1, 0);
        manifest = R"({"constants":[],"declaration":{"kind":"indicator","overlay":false,"title":"Series fixture"},"message_format":"pineforge/v1","outputs":[{"id":"p","index":0,"kind":"plot","line":1,"series":[)";
        for (int slot = 0; slot < slots; ++slot) {
            if (slot) manifest += ',';
            manifest += std::to_string(slot);
        }
        manifest += R"(],"title":"Values"}],"schema_version":"pineforge-outputs-manifest/v1","series":[)";
        for (int slot = 0; slot < slots; ++slot) {
            if (slot) manifest += ',';
            manifest += R"({"output":"p","role":"value","slot":)" + std::to_string(slot) + '}';
        }
        manifest += R"(],"source_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})";
    }

    int slots = 0;
    std::string manifest;
    std::uint64_t runs = 0;

    void on_native_run_begin() override { output_run_begin(); }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        if (!outputs_enabled_) return;
        output_bar(context.script_bar_open_ms, context.script_interval.next_period_open_ms);
        for (int slot = 0; slot < slots; ++slot) output_value(slot, bar.close + slot);
    }
};

SeriesHost& host_of(pf_strategy_t state) {
    return *static_cast<SeriesHost*>(static_cast<BacktestEngine*>(state));
}
}  // namespace

extern "C" {
pf_strategy_t strategy_create(const char*) {
    try { return static_cast<BacktestEngine*>(new SeriesHost()); }
    catch (...) { return nullptr; }
}
void strategy_free(pf_strategy_t state) { delete &host_of(state); }
void strategy_set_input(pf_strategy_t, const char*, const char*) {}
void strategy_set_override(pf_strategy_t, const char*, const char*) {}

void run_backtest_full(pf_strategy_t state, pf_bar_t* bars, int count,
                       const char* input_tf, const char* script_tf, int, int,
                       pf_magnifier_distribution_t, pf_report_t* report) {
    if (report) std::memset(report, 0, sizeof(*report));
    if (!state || count < 0 || (count && !bars)) return;
    auto& host = host_of(state);
    NativeRunSpec spec;
    spec.identity.session_key = "series-fixture";
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
    spec.initial_capital = 10000.;
    spec.point_value = spec.account_fx = 1.;
    spec.price_tick = .01;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) return;
    std::vector<Bar> input;
    for (int index = 0; index < count; ++index)
        input.push_back({bars[index].open, bars[index].high, bars[index].low,
                         bars[index].close, bars[index].volume, bars[index].timestamp});
    host.run(input.data(), count);
    if (report) host.fill_report(reinterpret_cast<ReportC*>(report));
}
void report_free(pf_report_t* report) {
    BacktestEngine::free_report(reinterpret_cast<ReportC*>(report));
}
uint32_t strategy_outputs_api_version() { return PF_OUTPUTS_API_VERSION; }
int strategy_outputs_manifest(pf_strategy_t state, char* json, size_t capacity,
                              size_t* required, char* error, size_t error_capacity) {
    return checked_settings::boundary(error, error_capacity, [&] {
        checked_settings::require(state != nullptr, "null strategy");
        checked_settings::receipt(host_of(state).manifest, json, capacity, required);
    });
}
}  // extern "C"
