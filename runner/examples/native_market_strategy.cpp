#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>

#include <stdexcept>

namespace {

class NativeMarketExample final : public pineforge::NativeStrategyHost {
    int bars_ = 0;
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1 && physical_position().signed_units == 0.0) {
            submit_market({pineforge::order_action::Transact{1.0}, "native-open", ""});
        } else if (bars_ == 4 && physical_position().signed_units != 0.0) {
            submit_market({pineforge::execution::Flatten{}, "native-flat", ""});
        }
    }
};

void refuse_source(const char* name) {
    throw std::runtime_error(std::string("native example refuses source API: ") + name);
}

}  // namespace

extern "C" {

PF_API pf_strategy_t strategy_create(const char*) {
    try {
        return static_cast<pf_strategy_t>(new NativeMarketExample());
    } catch (...) {
        return nullptr;
    }
}

PF_API void strategy_free(pf_strategy_t s) {
    delete static_cast<NativeMarketExample*>(s);
}

PF_API void strategy_set_input(pf_strategy_t, const char*, const char*) {
    refuse_source("strategy_set_input");
}

PF_API void strategy_set_override(pf_strategy_t, const char*, const char*) {
    refuse_source("strategy_set_override");
}

PF_API void run_backtest(pf_strategy_t s, pf_bar_t* bars, int n, pf_report_t*) {
    if (!s) return;
    static_cast<NativeMarketExample*>(s)->run(reinterpret_cast<pineforge::Bar*>(bars), n);
}

PF_API void run_backtest_full(pf_strategy_t s, pf_bar_t* bars, int n,
                              const char* input_tf, const char* script_tf,
                              int, int, pf_magnifier_distribution_t, pf_report_t*) {
    if (!s) return;
    static_cast<NativeMarketExample*>(s)->run(
        reinterpret_cast<pineforge::Bar*>(bars), n,
        input_tf ? input_tf : "", script_tf ? script_tf : "");
}

PF_API void report_free(pf_report_t*) {}

// Pull PF_API runtime/stream/native-config symbols from the static engine.
PF_API int native_market_example_abi_version() { return pf_abi_version(); }

}  // extern "C"
