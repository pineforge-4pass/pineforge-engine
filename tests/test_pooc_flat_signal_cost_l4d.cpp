// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// Covered POOC controls pin rounded signal-cost admission while flat. A child
// bracket has no live owner until its sole parent entry is admitted.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

class FlatClose : public pineforge::source::PineStrategyHost {
public:
    bool children, long_entry;
    double frozen = qnan;
    FlatClose(double capital, double step, double tick, bool brackets, bool is_long = true)
        : children(brackets), long_entry(is_long) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = step;
        syminfo_mintick_ = tick;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry(long_entry ? "L" : "S", long_entry);
            for (const auto& order : pending_orders_) {
                if (order.type == OrderType::MARKET) frozen = order.frozen_default_qty;
            }
            if (children) {
                strategy_exit("LX", "L", qnan, qnan, 1000.0, 1000.0);
                strategy_exit("SX", "S", qnan, qnan, 1000.0, 1000.0);
            }
        }
        if (bar_index_ == 1) strategy_close_all();
    }
    bool entered() const {
        for (const auto& trade : trades_) if (trade.entry_time == 1000) return true;
        return false;
    }
};

void test_terminal_close_cost_with_child_brackets() {
    const std::vector<Bar> bars = {
        {106583.05, 106623.12, 106288.48, 106320.56, 1, 1000},
        {106320.56, 106360.15, 105852.37, 105852.38, 1, 2000},
    };
    for (bool children : {false, true}) {
        for (bool is_long : {false, true}) {
            for (double extra : {-0.001, 0.0, 0.000002, 0.000004, 0.001}) {
                FlatClose engine(3026704.995997007 + extra, 0.00001, 0.01, children, is_long);
                engine.run(bars.data(), static_cast<int>(bars.size()));
                const bool admitted = extra < 0.0 || extra >= 0.000004;
                CHECK(near(engine.frozen, extra < 0.0 ? 28.46772 : 28.46773));
                CHECK(engine.entered() == admitted);
            }
        }
    }
}

void test_close_cost_at_another_lot_and_price_scale() {
    const std::vector<Bar> bars = {
        {3445.31, 3446.015, 3443.295, 3443.625, 1, 1000},
        {3443.565, 3446.015, 3443.295, 3444.0, 1, 2000},
    };
    for (double extra : {-0.0001, 0.0001}) {
        FlatClose engine(1033087.5 + extra, 0.01, 0.001, false);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.frozen, 300.0));
        CHECK(engine.entered() == (extra > 0.0));
    }
    // The fractional 300.02 lot is a lower-rounded-cost boundary and admits
    // on both sides of the subsequent rounded required-margin event.
    for (double capital : {1033156.3729, 1033156.3731}) {
        FlatClose engine(capital, 0.01, 0.001, false);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.frozen, 300.02));
        CHECK(engine.entered());
    }
}
}

int main() {
    test_terminal_close_cost_with_child_brackets();
    test_close_cost_at_another_lot_and_price_scale();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
