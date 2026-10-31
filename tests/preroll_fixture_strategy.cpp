// Native selected-window fixture: every supplied bar runs through the kernel,
// which records the equity curve. The strategy places no orders.
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>

#include <cstring>
#include <vector>

namespace {
class PrerollHost final : public pineforge::NativeStrategyHost {
public:
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {}
};

PrerollHost& host_of(pf_strategy_t state) {
    return *static_cast<PrerollHost*>(static_cast<pineforge::BacktestEngine*>(state));
}
}  // namespace

extern "C" {
pf_strategy_t strategy_create(const char*) {
    try { return static_cast<pineforge::BacktestEngine*>(new PrerollHost()); }
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
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "preroll-fixture";
    spec.identity.run_number = 1;
    spec.input_tf = input_tf && *input_tf ? input_tf : "1";
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
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    if (host.configure_native(spec).status != pineforge::NativeSetupStatus::Applied) return;
    std::vector<pineforge::Bar> input;
    input.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        input.push_back({bars[index].open, bars[index].high, bars[index].low,
                         bars[index].close, bars[index].volume, bars[index].timestamp});
    host.run(input.data(), count);
    if (report) host.fill_report(reinterpret_cast<pineforge::ReportC*>(report));
}
void report_free(pf_report_t* report) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
}
}  // extern "C"
