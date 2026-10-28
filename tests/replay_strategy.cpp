// A real strategy module for test_run_json_replay_engine.py. Its only job is
// to expose the shared test orders through run_json's ordinary module ABI.
// Runtime accessors and report production come from the linked engine.
#include "replay_fixture.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

namespace {

std::map<pf_strategy_t, std::unique_ptr<replay_fixture::Scenario>> scenarios;

replay_fixture::Scenario* scenario_for(pf_strategy_t handle) {
    const auto found = scenarios.find(handle);
    return found == scenarios.end() ? nullptr : found->second.get();
}

}  // namespace

extern "C" {

pf_strategy_t strategy_create(const char*) {
    try {
        auto scenario = std::make_unique<replay_fixture::Scenario>();
        const auto callbacks = replay_fixture::callbacks_for(*scenario);
        scenario->handle = strategy_native_host_create_v1(&callbacks);
        const auto handle = scenario->handle;
        if (!handle) return nullptr;
        scenarios.emplace(handle, std::move(scenario));
        return handle;
    } catch (...) {
        return nullptr;
    }
}

void strategy_free(pf_strategy_t handle) {
    scenarios.erase(handle);
}

void strategy_set_input(pf_strategy_t handle, const char* key, const char* value) {
    auto* scenario = scenario_for(handle);
    if (scenario && key && value && std::strcmp(key, "trade") == 0)
        scenario->trade = std::strcmp(value, "false") != 0;
}

void strategy_set_override(pf_strategy_t handle, const char* key, const char* value) {
    auto* scenario = scenario_for(handle);
    if (!scenario || !key || !value || std::strcmp(key, "initial_capital") != 0) return;
    char* end = nullptr;
    const double capital = std::strtod(value, &end);
    if (end != value && *end == '\0' && std::isfinite(capital) && capital > 0.0)
        scenario->initial_capital = capital;
}

void run_backtest_full(pf_strategy_t handle, pf_bar_t* bars, int n,
                       const char* input_tf, const char* script_tf, int, int,
                       pf_magnifier_distribution_t, pf_report_t* out) {
    if (out) std::memset(out, 0, sizeof(*out));
    auto* scenario = scenario_for(handle);
    if (!scenario) return;
    const auto spec = replay_fixture::spec_for(*scenario, input_tf, script_tf);
    const auto ext = replay_fixture::report_policy();
    if (strategy_configure_native_ext_v1(handle, &spec, &ext) != PF_NATIVE_OK) return;
    strategy_native_run_v1(handle, bars, n, out);
}

void run_backtest(pf_strategy_t handle, pf_bar_t* bars, int n, pf_report_t* out) {
    run_backtest_full(handle, bars, n, "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}

void report_free(pf_report_t* report) {
    strategy_native_report_free_v1(report);
}

}  // extern "C"
