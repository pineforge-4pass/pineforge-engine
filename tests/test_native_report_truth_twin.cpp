// R5 L2 report-truth, the §3.1b twin: one rule through the bare native host
// recording its own report and through the adapter recording the curve
// itself. This half binds pineforge/source, so tests/CMakeLists.txt registers
// it only when PINEFORGE_BUILD_SOURCE_LAYER is ON; the native witnesses live in
// tests/test_native_report_truth.cpp and run in the kernel-only profile.
#include "native_report_truth_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace r4_test;
using namespace l2_fixture;

namespace {

// ── Adapter twin ────────────────────────────────────────────────────────
// The same rule expressed in source commands. Nothing here configures a
// native report policy: the adapter keeps HostRecorded and records the curve
// itself, which is exactly what the twin has to show is equivalent.
class TwinAdapterProbe final : public pineforge::source::PineStrategyHost {
public:
    TwinAdapterProbe() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 1;
        config.slippage = 0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 2.0;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        const int index = seen_++;
        if (index == 10) strategy_entry("L", true, na<double>(), na<double>(), 2.0);
        else if (index == 30) strategy_close("L");
    }

private:
    int seen_ = 0;
};

void twin_native_rule(Host& host) {
    switch (host.calculations - 1) {
    case 10: host.submit(market(2.0, "twin-enter")); break;
    case 30: host.submit(market(-2.0, "twin-exit")); break;
    default: break;
    }
}

// 4. §3.1b twin: one rule, two hosts. The bare native host recording its own
//    report must produce the adapter's closed row and the adapter's curve.
void twin_native_and_adapter_agree() {
    const auto bars = feed(60);

    auto spec = report_spec("l2-report-truth-twin");
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    Host native;
    native.calculation = twin_native_rule;
    run_feed(native, spec, bars);
    completed(native);

    TwinAdapterProbe adapter;
    adapter.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(adapter.last_error().empty());

    Report native_report(native);
    Report adapter_report(adapter);

    REQUIRE(native_report.c.trades_len == 1);
    REQUIRE(adapter_report.c.trades_len == native_report.c.trades_len);
    for (int i = 0; i < native_report.c.trades_len; ++i) {
        const TradeC& left = native_report.c.trades[i];
        const TradeC& right = adapter_report.c.trades[i];
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(left.entry_price == right.entry_price);
        CHECK(left.exit_price == right.exit_price);
        CHECK(left.qty == right.qty);
        CHECK(left.pnl == right.pnl);
        CHECK(left.commission == right.commission);
        CHECK(left.is_long == right.is_long);
    }

    REQUIRE(native_report.c.equity_curve_len == 60);
    REQUIRE(adapter_report.c.equity_curve_len == native_report.c.equity_curve_len);
    for (std::int64_t i = 0; i < native_report.c.equity_curve_len; ++i) {
        CHECK(native_report.c.equity_curve[i].time_ms
              == adapter_report.c.equity_curve[i].time_ms);
        CHECK(native_report.c.equity_curve[i].equity
              == adapter_report.c.equity_curve[i].equity);
        CHECK(native_report.c.equity_curve[i].open_profit
              == adapter_report.c.equity_curve[i].open_profit);
    }
    CHECK(native_report.c.metrics.equity.max_equity_drawdown
          == adapter_report.c.metrics.equity.max_equity_drawdown);
    CHECK(native_report.c.metrics.equity.max_equity_runup
          == adapter_report.c.metrics.equity.max_equity_runup);
}

}  // namespace

int main() {
    test("twin_native_and_adapter_agree", twin_native_and_adapter_agree);
    std::printf("test_native_report_truth_twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
