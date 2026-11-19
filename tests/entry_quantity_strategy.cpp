// The two entry patterns from issue #716, exposed through the ordinary module
// ABI. Only strategy lifecycle/inputs live here; metadata, sizing, funding,
// streaming and reporting all come from the linked engine.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

class EntryQuantityStrategy final : public pineforge::source::PineStrategyHost {
public:
    EntryQuantityStrategy() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
#ifdef PINEFORGE_ENTRY_PERCENT
        config.default_qty_type = static_cast<int>(pineforge::QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
#endif
        configure_pine_strategy(config);
        set_syminfo_string("currency", "USDT");
        set_syminfo_string("basecurrency", "BTC");
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (pine_bar_index() != 1) return;
#ifdef PINEFORGE_ENTRY_PERCENT
        const double quantity = std::numeric_limits<double>::quiet_NaN();
#else
        const double quantity = 0.123456789;
#endif
        // The optional input is the issue's explicitly sized item-1 control.
        strategy_entry("L", true, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::quiet_NaN(),
                       get_input_double("Quantity", quantity));
    }
};

EntryQuantityStrategy& host(pf_strategy_t state) {
    return *static_cast<EntryQuantityStrategy*>(
        static_cast<pineforge::BacktestEngine*>(state));
}

}  // namespace

extern "C" {

PF_API pf_strategy_t strategy_create(const char*) {
    try {
        return static_cast<pineforge::BacktestEngine*>(new EntryQuantityStrategy);
    } catch (...) {
        return nullptr;
    }
}

PF_API void strategy_free(pf_strategy_t state) {
    delete static_cast<pineforge::BacktestEngine*>(state);
}

PF_API void strategy_set_input(pf_strategy_t state, const char* key, const char* value) {
    host(state).set_input(key, value);
}

PF_API void strategy_set_override(pf_strategy_t state, const char* key, const char* value) {
    if (std::string(key) != "initial_capital")
        throw std::invalid_argument("unsupported fixture override");
    std::size_t consumed = 0;
    const double capital = std::stod(value, &consumed);
    if (consumed != std::string(value).size() || !std::isfinite(capital) || capital <= 0.0)
        throw std::invalid_argument("invalid fixture initial_capital");
    pineforge::source::StrategyOverrides overrides;
    overrides.initial_capital = capital;
    host(state).set_strategy_override(overrides);
}

PF_API void run_backtest_full(pf_strategy_t state, pineforge::Bar* bars, int count,
                             const char* input_tf, const char* script_tf,
                             int magnifier, int samples, int distribution,
                             pineforge::ReportC* out) {
    host(state).run(bars, count, input_tf, script_tf, magnifier != 0, samples,
                   static_cast<pineforge::MagnifierDistribution>(distribution));
    host(state).fill_report(out);
}

PF_API void run_backtest(pf_strategy_t state, pineforge::Bar* bars, int count,
                        pineforge::ReportC* out) {
    run_backtest_full(state, bars, count, "60", "60", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}

PF_API void report_free(pineforge::ReportC* out) {
    pineforge::BacktestEngine::free_report(out);
}

// Retain the archive object providing the production metadata and stream ABI,
// as runner/examples/strategy.cpp does.
PF_API int entry_quantity_fixture_abi_version() { return pf_abi_version(); }

}  // extern "C"
