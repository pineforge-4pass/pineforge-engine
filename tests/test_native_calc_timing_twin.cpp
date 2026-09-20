// Native calculation timing, the TWIN half (witness 8 of
// tests/test_native_calc_timing.cpp): the adapter's calc_on_order_fills probe
// from tests/test_native_l4c_coof_literals.cpp, re-expressed natively, both
// routes run in this process. This half binds pineforge/source, so
// tests/CMakeLists.txt registers it only when PINEFORGE_BUILD_SOURCE_LAYER is
// ON; witnesses 1-7 are source-free and run in the kernel-only profile.
#include "native_calc_timing_fixture.hpp"

#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace l5_fixture;


// The adapter fixture of tests/test_native_l4c_coof_literals.cpp, verbatim.
std::vector<Bar> coof_lower_bars() {
    std::vector<Bar> lower;
    for (int i = 0; i < 30; ++i) {
        const double open = i < 15 ? 100.0 : 100.0 + (i - 15) * 0.1;
        lower.push_back({open, open + 1.0, open - 1.0, open + 0.25, 500.0,
                         static_cast<std::int64_t>(i) * 60000});
    }
    return lower;
}

class AdapterProbe : public source::PineNativeHost {
public:
    AdapterProbe() {
        source::PineStrategyConfig config;
        config.calc_on_order_fills = true;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
    }
    std::string lot_id(int index) const { return open_trade_entry_id(index); }
    double lot_price(int index) const { return open_trade_entry_price(index); }
    std::int64_t lot_time(int index) const { return open_trade_entry_time(index); }
};

class AdapterRefill final : public AdapterProbe {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ <= 1 && std::abs(physical_position().signed_units) < 6.0)
            strategy_entry("L" + std::to_string(physical_position().lot_count), true);
    }
};

class AdapterSingleEntry final : public AdapterProbe {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 && physical_position().lot_count == 0)
            strategy_entry("once", true);
    }
};

class NativeTwin : public NativeStrategyHost {
public:
    int script_index = -1;
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        ++script_index;
    }
    std::string lot_id(int index) const { return open_trade_entry_id(index); }
    double lot_price(int index) const { return open_trade_entry_price(index); }
    std::int64_t lot_time(int index) const { return open_trade_entry_time(index); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

// The adapter's RefillProbe rule, expressed against the kernel's own cadence:
// it runs on every calculation, exactly as on_source_bar does under
// calc_on_order_fills.
class NativeRefill final : public NativeTwin {
public:
    void on_native_recalculate(const Bar&, const NativeDecisionContext&,
                               NativeCalculationReason,
                               const native_order::ExecutionAppliedEvent*) override {
        if (script_index > 1) return;
        if (std::abs(physical_position().signed_units) >= 6.0) return;
        submit_market({order_action::Transact{1.0},
                       "L" + std::to_string(physical_position().lot_count), ""});
    }
};

class NativeSingleEntry final : public NativeTwin {
public:
    void on_native_recalculate(const Bar&, const NativeDecisionContext&,
                               NativeCalculationReason,
                               const native_order::ExecutionAppliedEvent*) override {
        if (script_index == 0 && physical_position().lot_count == 0)
            submit_market({order_action::Transact{1.0}, "once", ""});
    }
};

NativeRunSpec twin_spec(const char* key) {
    auto spec = base_spec(key, "15");
    spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    IntrabarPath::lower_tf path;
    path.bars = coof_lower_bars();
    path.tf = "1";
    path.samples = 4;
    spec.intrabar.value = path;
    return spec;
}

void test_twin_against_the_adapter() {
    scenario = "twin: adapter calc_on_order_fills";
    const auto lower = coof_lower_bars();

    // (a) The TV waypoint rule is inactive: the recalculation the fill drives
    // places no order, so nothing is deferred to a chart waypoint. The two
    // routes must book the identical entry.
    AdapterSingleEntry adapter_single;
    adapter_single.run(lower.data(), static_cast<int>(lower.size()), "1", "15", true, 4,
                       MagnifierDistribution::ENDPOINTS);
    CHECK(adapter_single.last_error().empty());
    NativeSingleEntry native_single;
    CHECK(native_single.configure_native(twin_spec("native-calc-twin-single")).status
          == NativeSetupStatus::Applied);
    native_single.run(lower.data(), static_cast<int>(lower.size()), "1", "15", false, 4,
                      MagnifierDistribution::ENDPOINTS);
    CHECK(native_single.last_error().empty());
    CHECK(adapter_single.physical_position().lot_count == 1);
    CHECK(native_single.physical_position().lot_count
          == adapter_single.physical_position().lot_count);
    CHECK(native_single.lot_id(0) == adapter_single.lot_id(0));
    CHECK(same(native_single.lot_price(0), adapter_single.lot_price(0)));
    CHECK(native_single.lot_time(0) == adapter_single.lot_time(0));
    CHECK(same(native_single.physical_position().signed_units,
               adapter_single.physical_position().signed_units));

    // (b) The refill cascade of tests/test_native_l4c_coof_literals.cpp. The
    // rule reaches the same book on both routes — six lots, the same ids in
    // the same order — and the adapter keeps its own literal.
    AdapterRefill adapter;
    adapter.run(lower.data(), static_cast<int>(lower.size()), "1", "15", true, 4,
                MagnifierDistribution::ENDPOINTS);
    CHECK(adapter.last_error().empty());
    CHECK(adapter.physical_position().lot_count == 6);  // L4c literal

    NativeRefill native;
    CHECK(native.configure_native(twin_spec("native-calc-twin-refill")).status
          == NativeSetupStatus::Applied);
    native.run(lower.data(), static_cast<int>(lower.size()), "1", "15", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(native.last_error().empty());
    CHECK(native.physical_position().lot_count == 6);
    for (int i = 0; i < 6; ++i) CHECK(native.lot_id(i) == adapter.lot_id(i));
    CHECK(native.native_recalculation_count() == 6);
    CHECK(native.native_recalculations_skipped() == 0);

    // The ONE itemized difference: where each route delivers a request born
    // in a fill recalculation. The adapter re-presents it at the chart bar's
    // next waypoint (CT11, the TV-only `coof_next_waypoint` refill rule,
    // which R5-5 deliberately leaves in the source layer), so its six lots
    // fill along script bar 0's own O/L/H/C and then bar 1's open. The kernel
    // has no waypoint rule: a newborn market request is eligible at the next
    // discrete matching point of the delivered path, which under a retained
    // lower feed is the next sub-bar's opening. Nothing else differs: same
    // rule, same six ids, same order, same resulting book.
    const std::vector<double> adapter_prices = {100.0, 100.0, 99.0, 101.0, 100.25, 100.10};
    const std::vector<std::int64_t> adapter_times = {900000, 900000, 900000, 900000,
                                                     900000, 960000};
    const std::vector<double> native_prices = {100.0, 100.1, 100.2, 100.3, 100.4, 100.5};
    const std::vector<std::int64_t> native_times = {900000, 960000, 1020000, 1080000,
                                                    1140000, 1200000};
    for (int i = 0; i < 6; ++i) {
        CHECK(same(adapter.lot_price(i), adapter_prices[static_cast<std::size_t>(i)]));
        CHECK(adapter.lot_time(i) == adapter_times[static_cast<std::size_t>(i)]);
        CHECK(same(native.lot_price(i), native_prices[static_cast<std::size_t>(i)]));
        CHECK(native.lot_time(i) == native_times[static_cast<std::size_t>(i)]);
    }
}

}  // namespace

int main() {
    test_twin_against_the_adapter();
    std::printf("native calculation timing twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
