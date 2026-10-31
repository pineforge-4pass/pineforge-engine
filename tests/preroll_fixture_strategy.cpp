// Source-host selected-window fixture: every supplied bar runs through the
// kernel, which records the equity curve. The strategy places no orders.
#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {
class PrerollHost final : public pineforge::source::PineStrategyHost {
public:
    PrerollHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const pineforge::Bar&) override {}
};

pineforge::BacktestEngine* engine(pf_strategy_t state) {
    return static_cast<pineforge::BacktestEngine*>(state);
}
}  // namespace

extern "C" {
pf_strategy_t strategy_create(const char*) {
    try { return static_cast<pineforge::BacktestEngine*>(new PrerollHost()); }
    catch (...) { return nullptr; }
}
void strategy_free(pf_strategy_t state) { delete engine(state); }
void strategy_set_input(pf_strategy_t, const char*, const char*) {}
void strategy_set_override(pf_strategy_t, const char*, const char*) {}

void run_backtest_full(pf_strategy_t state, pf_bar_t* bars, int count,
                       const char* input_tf, const char* script_tf,
                       int magnifier, int samples,
                       pf_magnifier_distribution_t distribution, pf_report_t* report) {
    if (report) std::memset(report, 0, sizeof(*report));
    if (!state || count < 0 || (count && !bars)) return;
    try {
        const std::string effective_input_tf = input_tf && *input_tf ? input_tf : "1";
        const std::string effective_script_tf = script_tf && *script_tf
            ? script_tf : effective_input_tf;
        std::vector<pineforge::Bar> input;
        input.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index)
            input.push_back({bars[index].open, bars[index].high, bars[index].low,
                             bars[index].close, bars[index].volume, bars[index].timestamp});
        engine(state)->run(input.data(), count, effective_input_tf, effective_script_tf,
                           magnifier != 0, samples,
                           static_cast<pineforge::MagnifierDistribution>(distribution));
        if (report) engine(state)->fill_report(reinterpret_cast<pineforge::ReportC*>(report));
    } catch (const std::exception& error) {
        if (report) {
            pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
            std::memset(report, 0, sizeof(*report));
        }
        pineforge::note_run_failure(*engine(state), "run_backtest_full", error);
    }
}
void report_free(pf_report_t* report) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
}
}  // extern "C"
