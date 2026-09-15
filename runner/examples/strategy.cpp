// SPDX-License-Identifier: Apache-2.0
// A hand-written native strategy. Codegen output exposes the same lifecycle.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

class NativeExample final : public pineforge::source::PineStrategyHost {
  public:
    NativeExample() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_value = 1;
        configure_pine_strategy(config);
    }
    void on_source_bar(const pineforge::Bar &) override {
        // Alternating market entry/close gives a small deterministic test.
        if (bar_index_ % 4 == 1)
            strategy_entry("Long", true);
        if (bar_index_ % 4 == 3)
            strategy_close_all();
    }
};
extern "C" {
PF_API void *strategy_create(const char *) {
    try {
        return new NativeExample;
    } catch (...) {
        return nullptr;
    }
}
PF_API void strategy_free(void *s) { delete static_cast<NativeExample *>(s); }
PF_API void strategy_set_input(void *s, const char *key, const char *value) {
    static_cast<NativeExample *>(s)->set_input(key, value);
}
PF_API void strategy_set_override(void *, const char *, const char *) {}
PF_API void run_backtest(void *s, pineforge::Bar *bars, int n, pineforge::ReportC *out) {
    auto *strategy = static_cast<NativeExample *>(s);
    strategy->run(bars, n);
    strategy->fill_report(out);
}
PF_API void run_backtest_full(void *s, pineforge::Bar *bars, int n, const char *input_tf,
                              const char *script_tf, int magnifier, int samples, int distribution,
                              pineforge::ReportC *out) {
    auto *strategy = static_cast<NativeExample *>(s);
    strategy->run(bars, n, input_tf ? input_tf : "", script_tf ? script_tf : "", magnifier != 0,
                  samples, static_cast<pineforge::MagnifierDistribution>(distribution));
    strategy->fill_report(out);
}
PF_API void report_free(pineforge::ReportC *out) { pineforge::BacktestEngine::free_report(out); }
// Retain the core object that exports native streaming symbols from the static
// engine library; no per-strategy streaming implementation is necessary.
PF_API int native_example_abi_version() { return pf_abi_version(); }
}
