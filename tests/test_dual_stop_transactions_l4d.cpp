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

/*
 * Literal TradingView dual-stop transaction controls, exported 2026-09-08.
 * OANDA:XAUUSD 15m, 2025-08-18..20; no indicator or corpus execution here.
 * The low-first and high-first bars each touch two stops armed while flat.
 * TV reduces / flattens / reverses by the second transaction, independently
 * of source call order; OCA cancel removes the second order. Capital 10548
 * distinguishes a frozen default BUY 3.16 from a live re-size to 3.15.
 * The independent short-only margin-amount question is outside this test.
 */
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
namespace {
int passed = 0;
int failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)
const double NaN = std::numeric_limits<double>::quiet_NaN();

struct Control {
    const char* name;
    bool high_first = false;
    double long_qty = 3.15;
    double short_qty = 3.16;
    double capital = 20000;
    double margin = 100;
    bool reverse_calls = false;
    bool oca = false;
    bool default_percent = false;
    int forced_path = 0;
    double injected_long_snapshot = NaN;
};

class Pair : public pineforge::source::PineStrategyHost {
public:
    explicit Pair(Control control) : c(control) {
        initial_capital_ = c.capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 1;
        margin_long_ = margin_short_ = c.margin;
        qty_step_ = 0.01;
        syminfo_mintick_ = 0.001;
        set_margin_call_enabled(true);
        set_path_order(c.forced_path);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            auto enter = [&](bool is_long) {
                strategy_entry(is_long ? "L" : "S", is_long, NaN,
                    is_long ? long_stop() : short_stop(),
                    c.default_percent ? NaN : (is_long ? c.long_qty : c.short_qty),
                    "", c.oca ? "pair" : "", c.oca ? 1 : 0);
            };
            enter(!c.reverse_calls);
            enter(c.reverse_calls);
            // Derived ABI values at the stable flat signal boundary remain
            // the same quantities the covered transactions later consume.
            for (size_t i = 0; i < pending_orders_.size(); ++i) {
                auto& order = pending_orders_[i];
                // Test-only mutation canary, never a TradingView oracle.
                // Make the actual admission path face an existing snapshot
                // that is larger than its live-equity re-size.
                if (order.is_long && std::isfinite(c.injected_long_snapshot)) {
                    order.default_stop_placement_qty = c.injected_long_snapshot;
                }
                double q = NaN;
                int close_only = -1, partition = -1;
                CHECK(probe_fill_qty(static_cast<int>(i),
                    order.is_long ? long_stop() : short_stop(),
                    &q, &close_only, &partition) == 0);
                if (order.is_long) abi_long_qty = q;
                else abi_short_qty = q;
            }
        }
        if (bar_index_ == 1) {
            after_fills_signed_qty = signed_position_size();
            // Quote a fresh native order through the public pending projection
            // instead of calling the retired owner-only sizing helper.
            strategy_entry("__l4d_quote__", true, NaN, long_stop());
            const int quote = pending_order_count() - 1;
            double quoted = NaN;
            int close_only = -1, partition = -1;
            if (quote >= 0 && probe_fill_qty(quote, long_stop(), &quoted,
                                             &close_only, &partition) == 0)
                after_fills_live_buy_qty = quoted;
            strategy_cancel_all();
            strategy_close_all();
        }
    }
    double long_stop() const { return c.high_first ? 3333.0 : 3337.762; }
    double short_stop() const { return c.high_first ? 3331.5 : 3327.593; }
    const std::vector<Trade>& closed() const { return trades_; }
    bool flat_and_empty() const { return position_side_ == PositionSide::FLAT && pending_orders_.empty(); }
    double abi_long_qty = NaN;
    double abi_short_qty = NaN;
    double after_fills_signed_qty = NaN;
    double after_fills_live_buy_qty = NaN;
private:
    Control c;
};

struct Expected {
    bool is_long;
    double qty;
    double entry;
    double exit;
    int exit_bar;
};

void run(const Control& control, std::vector<Expected> expected) {
    std::printf("-- %s --\n", control.name);
    std::vector<Bar> bars;
    if (control.high_first) {
        bars = {
            {3333.595, 3333.61, 3331.785, 3332.325, 627, 1755558900000LL},
            {3332.36, 3333.275, 3331.37, 3332.88, 1185, 1755559800000LL},
            {3332.84, 3332.87, 3330.565, 3330.965, 1497, 1755560700000LL},
        };
    } else {
        bars = {
            {3332.84, 3332.87, 3330.565, 3330.965, 1497, 1755560700000LL},
            {3330.915, 3337.985, 3326.285, 3336.315, 5952, 1755561600000LL},
            {3336.29, 3339.355, 3335.65, 3337.485, 2256, 1755562500000LL},
        };
    }
    Pair pair(control);
    pair.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(pair.closed().size() == expected.size());
    const size_t n = std::min(pair.closed().size(), expected.size());
    for (size_t i = 0; i < n; ++i) {
        const auto& got = pair.closed()[i];
        const auto& want = expected[i];
        CHECK(got.is_long == want.is_long);
        CHECK(std::abs(got.qty - want.qty) < 1e-10);
        CHECK(std::abs(got.entry_price - want.entry) < 1e-9);
        CHECK(std::abs(got.exit_price - want.exit) < 1e-9);
        CHECK(got.entry_time == bars[1].timestamp);
        CHECK(got.exit_time == bars[want.exit_bar].timestamp);
        CHECK(got.entry_bar_index == 1);
        CHECK(got.exit_bar_index == want.exit_bar);
    }
    CHECK(pair.flat_and_empty());
    double ignored = 0;
    int close_only = 0, partition = 0;
    CHECK(pair.probe_fill_qty(0, 3333, &ignored, &close_only, &partition) == -1);
    if (control.default_percent) {
        CHECK(std::abs(pair.abi_short_qty - 3.16) < 1e-10);
        CHECK(std::abs(pair.abi_long_qty - (control.capital == 10548 ? 3.16 : 3.15)) < 1e-10);
    }
}
// Unlike the literal TV controls, this is a mutation-sensitive component
// test. A 3.17 snapshot cannot fit capital10548 at the later3337.762 fill,
// while live re-sizing would approve3.15. Run the real scanner: if either
// admission caller loses the pair context it approves3.15 but dispatches3.17,
// incorrectly reversing long0.01. The preserved short proves rejection.
void admission_snapshot_canary() {
    Control control{"admission uses dispatched snapshot",false,3.16,3.16,
                    10548,100,false,false,true,0,3.17};
    Pair pair(control);
    const Bar bars[] = {
        {3332.84,3332.87,3330.565,3330.965,1497,1755560700000LL},
        {3330.915,3337.985,3326.285,3336.315,5952,1755561600000LL},
        {3336.29,3339.355,3335.65,3337.485,2256,1755562500000LL},
    };
    pair.run(bars,3);
    CHECK(std::abs(pair.after_fills_live_buy_qty - 3.15) < 1e-10);
    CHECK(pair.flat_and_empty());
}
} // namespace

int main() {
    const std::vector<Expected> less = {{false,3.15,3327.593,3337.762,1}, {false,0.01,3327.593,3336.29,2}};
    run({"short first: partial"}, less);
    run({"short first: equal",false,3.16,3.16}, {{false,3.16,3327.593,3337.762,1}});
    run({"short first: excess",false,3.17,3.16}, {{false,3.16,3327.593,3337.762,1}, {true,0.01,3337.762,3336.29,2}});
    run({"short first: reversed source calls",false,3.15,3.16,20000,100,true}, less);
    run({"short first: OCA cancel",false,3.15,3.16,20000,100,false,true}, {{false,3.16,3327.593,3336.29,2}});
    run({"short first: default percent",false,3.15,3.16,10542.28225,100,false,false,true}, less);
    run({"high first: partial",true,3.16,3.15}, {{true,3.15,3333,3331.5,1}, {true,0.01,3333,3332.84,2}});
    run({"high first: equal",true,3.16,3.16}, {{true,3.16,3333,3331.5,1}});
    run({"high first: excess",true,3.16,3.17}, {{true,3.16,3333,3331.5,1}, {false,0.01,3331.5,3332.84,2}});
    run({"default snapshot discriminator",false,3.16,3.16,10548,100,false,false,true}, {{false,3.16,3327.593,3337.762,1}});
    run({"default snapshot reversed calls",false,3.16,3.16,10548,100,true,false,true}, {{false,3.16,3327.593,3337.762,1}});
    run({"default snapshot margin disabled",false,3.16,3.16,10548,0,false,false,true}, {{false,3.16,3327.593,3337.762,1}});
    // Forcing the already-natural order changes no transaction policy.
    run({"same low-first path forced",false,3.15,3.16,20000,100,false,false,false,2}, less);
    run({"same high-first path forced",true,3.16,3.15,20000,100,false,false,false,1}, {{true,3.15,3333,3331.5,1}, {true,0.01,3333,3332.84,2}});
    admission_snapshot_canary();
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
