// Compile/link every benchmark bridge export against the real runtime; the
// Python test separately checks its ctypes argument/layout usage.
#include <pineforge/pineforge.h>
#include <pineforge/native_module.hpp>
#include <pineforge/report_output.hpp>
#include <pineforge/detail/scratch_defaults.hpp>
#include "../benchmarks/throughput/hpo_bridge.hpp"
#include "cr150_hpo_heap.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace {
class BridgeFixture : public pineforge::NativeStrategyHost {
public:
    BridgeFixture() {
        pineforge::NativeRunSpec spec;
        spec.identity = {"cr150-hpo-bridge", 1};
        spec.input_tf = "60"; spec.script_tf = "60";
        spec.tickerid = "TEST:BRIDGE"; spec.timezone = "UTC"; spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = 1.0; spec.account_fx = 1.0; spec.price_tick = .25;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        spec.report_open_position_at_end = true;
        spec.close_execution = pineforge::NativeCloseExecution::AfterCalculation;
        if (configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
            throw std::runtime_error("bridge fixture setup refused");
    }
    void on_native_run_begin() override { calculations = 0; }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (calculations++ != 0) return;
        pineforge::native_order::Request request;
        request.intent = pineforge::native_order::Transact{2.0};
        request.label = "held";
        if (submit(request).status != pineforge::native_order::SubmitStatus::Accepted)
            throw std::runtime_error("bridge fixture entry refused");
    }
    void hash_host_extension(pineforge::BrokerStateHashSink& sink) const override {
        sink.s("cr150-hpo-bridge/v1"); sink.i(calculations);
    }
    int calculations = 0;
};
}
PINEFORGE_EXPORT_NATIVE_STRATEGY(BridgeFixture);

// The unit is also the bridge TU so the lane can register it through the
// standard unit helper without editing the shared tests/CMakeLists.txt.
#include "../benchmarks/throughput/hpo_bridge.cpp"

int main() {
    assert(pf_report_output_version_v1() == 1);
    assert(strategy_set_report_output_mode_v1(nullptr, 1) != 0);
    assert(strategy_set_hpo_scratch_manifest_v1(nullptr, nullptr) != 0);
    assert(strategy_hpo_scratch_manifest_v1(nullptr, nullptr) != 0);
    assert(strategy_run_hpo_v1(nullptr, nullptr, 0) != 0);
    assert(strategy_report_summary_v1(nullptr, nullptr) != 0);

    std::array<pf_bar_t, 4097> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double p = 100.0 + (i % 13) * .25;
        bars[i] = {p, p + .5, p - .5, p + .25, double(i + 1),
                   1704067200000LL + static_cast<std::int64_t>(i) * 3600000};
    }
    auto reference = strategy_create(nullptr);
    assert(reference);
    assert(strategy_set_report_output_mode_v1(reference, 0) == 0);
    pf_report_t report{};
    run_backtest(reference, bars.data(), static_cast<int>(bars.size()), &report);
    assert(strategy_last_run_status(reference) == 0);
    assert(report.total_trades == 1);
    assert(report.trades_len == 1 && report.trades[0].open_at_end == 1);
    assert(report.equity_curve_len == static_cast<std::int64_t>(bars.size()));

    auto summary = strategy_create(nullptr);
    assert(summary);
    assert(strategy_set_report_output_mode_v1(summary, 99) != 0);
    assert(strategy_set_report_output_mode_v1(summary, 1) == 0);
    const auto expected = pineforge::detail::derive_hpo_scratch_manifest(
        pineforge::detail::HpoScratchSpec{});
    pf_hpo_scratch_manifest_v1 manifest{};
    std::memcpy(&manifest, &expected, sizeof manifest);
    assert(strategy_set_hpo_scratch_manifest_v1(summary, &manifest) == 0);
    pf_hpo_scratch_manifest_v1 receipt{};
    receipt.struct_size = sizeof receipt; receipt.version = 1;
    assert(strategy_hpo_scratch_manifest_v1(summary, &receipt) == 0);
    assert(std::memcmp(&manifest, &receipt, sizeof manifest) == 0);
    pf_report_summary_v1 result{};
    result.struct_size = sizeof result; result.version = 1;
    assert(strategy_report_summary_v1(summary, &result) != 0); // Ready is not Completed.
    assert(strategy_run_hpo_v1(summary, bars.data(), static_cast<int>(bars.size())) == 0);
    const auto before = cr150_hpo_heap::allocations.load();
    assert(strategy_report_summary_v1(summary, &result) == 0);
    assert(cr150_hpo_heap::allocations.load() == before);
    assert(result.complete == 1 && result.output_mode == 1);
    assert(result.total_trades == 1);
    assert(result.equity_point_count == static_cast<std::int64_t>(bars.size()));
    assert(result.input_bars_processed == static_cast<std::int64_t>(bars.size()));
    assert(result.script_bars_processed == static_cast<std::int64_t>(bars.size()));
    assert(std::memcmp(&result.net_profit, &report.net_profit, sizeof(double)) == 0);
    assert(std::memcmp(&result.metrics, &report.metrics, sizeof(result.metrics)) == 0);
    assert(static_cast<BridgeFixture*>(static_cast<pineforge::BacktestEngine*>(summary))
               ->calculations == static_cast<int>(bars.size()));
    auto repeated = result;
    assert(strategy_report_summary_v1(summary, &repeated) == 0);
    assert(std::memcmp(&result, &repeated, sizeof result) == 0);
    report_free(&report);
    strategy_free(reference);
    strategy_free(summary);
}
