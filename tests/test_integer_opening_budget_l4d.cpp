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

// Literal broker fixtures for the TV-proven whole-lot budget boundary.
// No registered strategy or reference tape is executed by this test.
#include <cmath>
#include <cstdio>
#include <limits>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
namespace {
int passed = 0, failed = 0;
#define CHECK(condition) do { if (condition) ++passed; else { ++failed; \
    std::printf("FAIL %d: %s\n", __LINE__, #condition); } } while (0)
constexpr double NA = std::numeric_limits<double>::quiet_NaN();
constexpr double PRICE = 9.17;
constexpr double BUDGET = 978503.19;
const Bar bars[] = {
    {PRICE, PRICE, PRICE, PRICE, 1, 1000},
    {PRICE, PRICE, PRICE, PRICE, 1, 2000},
    {PRICE, PRICE, PRICE, PRICE, 1, 3000},
    {PRICE, PRICE, PRICE, PRICE, 1, 4000},
    {PRICE, PRICE, PRICE, PRICE, 1, 5000},
};

class Opening : public pineforge::source::PineStrategyHost {
public:
    Opening(double budget, bool new_long, bool seed, bool same_side = false,
            bool competing = false, double percent = 100)
        : new_long_(new_long), seed_(seed), same_side_(same_side),
          competing_(competing) {
        initial_capital_ = budget;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = percent;
        margin_long_ = margin_short_ = 100;
        pyramiding_ = 1;
        qty_step_ = syminfo_.pointvalue = 1;
        set_syminfo_mintick(.01);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 && seed_)
            strategy_entry("Seed", same_side_ ? new_long_ : !new_long_, NA, NA, 1);
        if (bar_index_ == 1) {
            if (seed_) strategy_close("Seed");
            if (competing_)
                strategy_entry("Resting", new_long_, new_long_ ? 1 : 1000, NA, 1);
            strategy_entry("New", new_long_);
        }
        if (bar_index_ == 2) opened_qty = position_qty_;
        if (bar_index_ == 3) strategy_close("New");
    }
    double opened_qty = -1;
private:
    bool new_long_, seed_, same_side_, competing_;
};

void check(double budget, bool new_long, bool seed, bool expected,
           bool same_side = false, bool competing = false, double percent = 100) {
    Opening engine(budget, new_long, seed, same_side, competing, percent);
    engine.run(bars, 5);
    if (engine.opened_qty != (expected ? (percent == 100 ? 106707 : 105639) : 0)) {
        std::printf("budget=%.17g long=%d seed=%d same=%d competing=%d percent=%.1f opened=%.17g trades=%d\n",
                    budget, new_long, seed, same_side, competing, percent,
                    engine.opened_qty, engine.trade_count());
    }
    CHECK(engine.last_error().empty());
    CHECK(engine.opened_qty == (expected ? (percent == 100 ? 106707 : 105639) : 0));
    CHECK(engine.trade_count() == (seed ? 1 : 0) + (expected ? 1 : 0));
    if (seed && engine.trade_count() > 0) {
        const auto& close = engine.get_trade(0);
        CHECK(close.entry_bar_index == 1);
        CHECK(close.exit_bar_index == 2);
        CHECK(close.qty == 1);
        CHECK(close.entry_price == PRICE && close.exit_price == PRICE);
    }
}
}  // namespace

int main() {
    const double below = std::nextafter(BUDGET, 0.0);
    const double above = std::nextafter(BUDGET, std::numeric_limits<double>::infinity());
    for (bool new_long : {false, true}) {
        for (bool seed : {false, true}) {
            check(below, new_long, seed, false);
            check(BUDGET, new_long, seed, true);
            check(above, new_long, seed, true);
            check(below, new_long, seed, true, false, false, 99);
            check(below, new_long, seed, true, false, true);
        }
        check(below, new_long, true, false, true);
        // A same-direction call made while the seed is still held is over
        // the pyramiding cap and follows the existing post-close removal.
        check(BUDGET, new_long, true, false, true);
    }
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
