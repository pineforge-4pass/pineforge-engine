#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

class ServiceExample final : public pineforge::source::PineStrategyHost {
public:
    ServiceExample() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_value = 1;
        configure_pine_strategy(config);
    }
    void on_source_bar(const pineforge::Bar&) override {
        if (bar_index_ % 4 == 1) strategy_entry("Long", true);
        if (bar_index_ % 4 == 3) strategy_close_all();
        const auto* gate = std::getenv("PINEFORGE_TEST_MESSAGE_GATE");
        if (!gate || bar_index_ != 1) return;
        std::ofstream(gate).put('1');
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::filesystem::exists(std::string(gate) + ".resume")) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("test gate expired");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

extern "C" {
PF_API void* strategy_create(const char*) { return new ServiceExample; }
PF_API void strategy_free(void* state) { delete static_cast<ServiceExample*>(state); }
PF_API void strategy_set_input(void* state, const char* key, const char* value) {
    static_cast<ServiceExample*>(state)->set_input(key, value);
}
PF_API void strategy_set_override(void*, const char*, const char*) {}
PF_API void run_backtest(void* state, pineforge::Bar* bars, int count, pineforge::ReportC* report) {
    auto* strategy = static_cast<ServiceExample*>(state);
    strategy->run(bars, count);
    strategy->fill_report(report);
}
PF_API void run_backtest_full(void* state, pineforge::Bar* bars, int count, const char* input_tf,
    const char* script_tf, int magnifier, int samples, int distribution, pineforge::ReportC* report) {
    auto* strategy = static_cast<ServiceExample*>(state);
    strategy->run(bars, count, input_tf ? input_tf : "", script_tf ? script_tf : "", magnifier != 0,
        samples, static_cast<pineforge::MagnifierDistribution>(distribution));
    strategy->fill_report(report);
}
PF_API void report_free(pineforge::ReportC* report) { pineforge::BacktestEngine::free_report(report); }
PF_API int native_example_abi_version() { return pf_abi_version(); }
}
