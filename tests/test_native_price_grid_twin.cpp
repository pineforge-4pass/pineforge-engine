// R5 L8 price grid, the TWIN half: one adapter-driven probe through the native
// lowering. The adapter keeps TradingView's own tick rule, so the exit price
// below is the identity diff that proves the kernel grid never reached
// src/source (its clean-main witness is diffed bit-for-bit). This half binds
// pineforge/source, so tests/CMakeLists.txt registers it only when
// PINEFORGE_BUILD_SOURCE_LAYER is ON; the kernel-side witnesses live in
// tests/test_native_price_grid.cpp and run in the kernel-only profile.
#include "l4a_native_route_guard.hpp"

#include "native_price_grid_fixture.hpp"

#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {
using namespace l8_fixture;

// --- twin: one adapter-driven probe through the native lowering ------------
source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = (int)QtyType::FIXED;
    c.default_qty_value = 1;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.commission_type = (int)CommissionType::PERCENT;
    c.slippage = 0;
    return c;
}

class MidBarStop final : public source::PineStrategyHost {
public:
    MidBarStop() { configure_pine_strategy(cfg()); set_syminfo_mintick(0.01); }
    void on_source_bar(const Bar& bar) override {
        if (placed_) return;
        strategy_entry("L", true, kNaN, kNaN, 1.0, "entry long");
        strategy_exit("X", "L", kNaN, (bar.open + bar.high) * 0.5, kNaN, kNaN, kNaN, 100.0,
                      "mid-bar stop");
        placed_ = true;
    }
private:
    bool placed_ = false;
};

void adapter_twin_is_untouched() {
    scenario = "L8-twin the adapter keeps its own tick rule";
    const std::vector<Bar> bars = {
        {1804.00, 1813.014, 1803.33, 1811.96, 49634.773, 1743397200000LL},
        {1811.96, 1812.000, 1801.08, 1808.93, 51943.482, 1743398100000LL},
        {1801.93, 1807.960, 1800.92, 1806.37, 37418.258, 1743420600000LL},
        {1806.37, 1819.000, 1805.97, 1812.52, 92807.927, 1743421500000LL},
        {1812.51, 1815.000, 1809.24, 1809.48, 39810.958, 1743422400000LL},
        {1809.49, 1818.800, 1808.15, 1816.41, 45388.380, 1743423300000LL},
    };
    MidBarStop host;
    host.run(bars.data(), (int)bars.size(), "15", "15");
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() != 1) return;
    const auto& t = host.get_trade(0);
    // Diffed by identity against the clean-main witness, not by trade number.
    CHECK(t.is_long);
    CHECK(same_bits(t.qty, 1.0));
    CHECK(t.entry_time == 1743398100000LL);
    CHECK(same_bits(t.entry_price, 1811.96));
    CHECK(t.exit_time == 1743398100000LL);
    // The raw stop level is 1808.507. The adapter floors a long's protective
    // stop onto its own 0.01 ladder; a kernel grid leaking into this path
    // would book the nearest tick, 1808.51, so this price is the identity
    // diff that keeps the two layers apart.
    CHECK(same_bits(t.exit_price, 1808.50));
    CHECK(same_bits(t.pnl, -3.4600000000000364));
}

}  // namespace

int main() {
    adapter_twin_is_untouched();
    std::printf("%s native price grid twin: %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
