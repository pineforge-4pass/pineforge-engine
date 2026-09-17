// R4-D L10q: On the switched route the staged-pyramid exit of corpus scenario
// pyramid-terrace-staged-entry-01 fills on the legacy owner's bar and price
// (trades #647/#648: owner Exit long 2025-10-26 01:45 at 3926.03; this tree
// 02:00 at 3931.40), so the scenario replays identically.
// Bars are embedded from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv —
// this test must never open corpus files (CI has no corpus checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-4) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg(int pyr) {
    source::PineStrategyConfig c;
    c.initial_capital = 100000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyr;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = 0.05;
    c.slippage = 1;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    c.process_orders_on_close = false;
    c.calc_on_order_fills = false;
    return c;
}

void expect_trade(const char* tag, const Trade& t, bool is_long,
                  double entry_px, double exit_px, double pnl,
                  double fav, double adv) {
    std::printf("%s %s @%.4f->%.4f pnl=%.4f mfe=%.4f mae=%.4f "
                "(want @%.4f->%.4f pnl=%.4f mfe=%.4f mae=%.4f)\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price, t.pnl,
                t.max_runup, t.max_drawdown, entry_px, exit_px, pnl, fav, adv);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px));
    CHECK(near(t.exit_price, exit_px));
    CHECK(near(t.pnl, pnl));
    CHECK(near(t.max_runup, fav, 1e-2));
    CHECK(near(t.max_drawdown, adv, 1e-2));
}

// Replays Cycle A (Trade 521: strategy.close issued along with exit stop, where
// the stop hits first and flattens the position), followed by Cycle B
// (Trades #647/#648/#649: staged terrace entries with global Terrace Guard stop).
// Prior to L10q, the deferred strategy.close from Cycle A survived in live_handles_
// as a zombie order and blocked reservation of the global stop exit in Cycle B,
// causing trades #647/#648 to miss the 01:45 stop exit at 3926.03.
class PyramidTerraceHost : public source::PineStrategyHost {
public:
    PyramidTerraceHost() {
        configure_pine_strategy(cfg(3));
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        // Cycle A: Trade 521
        if (i == 0) {
            strategy_entry("Terrace One", true, kNaN, kNaN, 1.0, "");
        } else if (i == 1) {
            strategy_exit("Terrace Guard", "", kNaN, 4426.0);
            strategy_close("Terrace One", "", kNaN, kNaN, false, 154618822675ULL);
        }
        // Bar i == 2: stop hits at open 4426.0, position flattens.

        // Cycle B: Trades #647, #648, #649
        if (i == 3) {
            // 2025-10-25 18:15: first terrace entry signal
            strategy_entry("Terrace One", true, kNaN, kNaN, 1.0, "");
        } else if (i >= 4 && i <= 15) {
            // First lot live (entry at 18:30 open 3944.12)
            strategy_exit("Terrace Guard", "", kNaN, 3919.45);
            if (i == 15) {
                // 2025-10-25 21:15: second terrace entry signal
                strategy_entry("Terrace One", true, kNaN, kNaN, 1.0, "");
            }
        } else if (i >= 16 && i <= 27) {
            // Second lot live (entry at 21:30 open 3945.38)
            strategy_exit("Terrace Guard", "", kNaN, 3920.66);
            if (i == 27) {
                // 2025-10-26 00:15: third terrace entry signal
                strategy_entry("Terrace One", true, kNaN, kNaN, 1.0, "");
            }
        } else if (i >= 28 && i <= 32) {
            // Third lot live (entry at 00:30 open 3953.42)
            if (i == 32) {
                // 2025-10-26 01:30: exact stop level before the 01:45 drop
                strategy_exit("Terrace Guard", "", kNaN, 3926.0466);
            } else {
                strategy_exit("Terrace Guard", "", kNaN, 3925.32);
            }
        } else if (i == 33) {
            // 2025-10-26 01:45: low is 3914.30, stop at 3926.0466 hits at 3926.03.
            // If the stop was improperly blocked, the trend turn calls strategy_close.
            if (live_position_size() > 0.0) {
                strategy_close("Terrace One", "", kNaN, kNaN, false, 154618822675ULL);
            }
        }
    }
};

std::vector<Bar> terrace_bars() {
    return {
        // Cycle A bars (2025-09-17)
        mk(1758130200000LL, 4480.63, 4532.16, 4480.0, 4508.75), // 0: 17:30
        mk(1758131100000LL, 4508.74, 4527.0, 4426.0, 4426.0),   // 1: 17:45
        mk(1758132000000LL, 4426.0, 4520.39, 4404.08, 4455.08), // 2: 18:00 (stop exit)

        // Cycle B bars (2025-10-25 18:15 to 2025-10-26 02:00)
        mk(1761416100000LL, 3936.81, 3945.2, 3934.67, 3944.1),   // 3: 18:15
        mk(1761417000000LL, 3944.11, 3968.93, 3943.99, 3960.37), // 4: 18:30 (Trade #647 entry)
        mk(1761417900000LL, 3960.36, 3962.9, 3953.42, 3958.21),  // 5: 18:45
        mk(1761418800000LL, 3958.21, 3959.0, 3950.13, 3951.62),  // 6: 19:00
        mk(1761419700000LL, 3951.62, 3960.43, 3951.62, 3956.54), // 7: 19:15
        mk(1761420600000LL, 3956.54, 3962.44, 3956.36, 3960.12), // 8: 19:30
        mk(1761421500000LL, 3960.12, 3960.65, 3954.2, 3956.05),  // 9: 19:45
        mk(1761422400000LL, 3956.06, 3957.78, 3954.73, 3955.54), // 10: 20:00
        mk(1761423300000LL, 3955.53, 3956.78, 3948.8, 3951.62),  // 11: 20:15
        mk(1761424200000LL, 3951.62, 3956.38, 3942.0, 3951.4),   // 12: 20:30
        mk(1761425100000LL, 3951.4, 3954.84, 3943.68, 3943.7),   // 13: 20:45
        mk(1761426000000LL, 3943.7, 3948.14, 3943.17, 3943.44),  // 14: 21:00
        mk(1761426900000LL, 3943.45, 3946.25, 3932.5, 3945.36),  // 15: 21:15
        mk(1761427800000LL, 3945.37, 3950.0, 3944.73, 3946.8),   // 16: 21:30 (Trade #648 entry)
        mk(1761428700000LL, 3946.8, 3951.47, 3940.97, 3951.47),  // 17: 21:45
        mk(1761429600000LL, 3951.46, 3951.6, 3944.95, 3947.2),   // 18: 22:00
        mk(1761430500000LL, 3947.2, 3953.63, 3946.99, 3953.0),   // 19: 22:15
        mk(1761431400000LL, 3953.01, 3956.44, 3950.99, 3955.79), // 20: 22:30
        mk(1761432300000LL, 3955.79, 3963.47, 3954.48, 3959.7),  // 21: 22:45
        mk(1761433200000LL, 3959.7, 3962.22, 3956.12, 3958.79),  // 22: 23:00
        mk(1761434100000LL, 3958.78, 3959.21, 3952.21, 3955.84), // 23: 23:15
        mk(1761435000000LL, 3955.83, 3955.83, 3951.11, 3951.11), // 24: 23:30
        mk(1761435900000LL, 3951.11, 3953.42, 3948.38, 3952.25), // 25: 23:45
        mk(1761436800000LL, 3952.24, 3952.24, 3944.17, 3948.14), // 26: 00:00
        mk(1761437700000LL, 3948.15, 3955.82, 3946.14, 3953.42), // 27: 00:15
        mk(1761438600000LL, 3953.41, 3960.0, 3953.41, 3955.47),  // 28: 00:30 (Trade #649 entry)
        mk(1761439500000LL, 3955.48, 3958.22, 3947.28, 3948.2),  // 29: 00:45
        mk(1761440400000LL, 3948.2, 3953.83, 3946.02, 3951.84),  // 30: 01:00
        mk(1761441300000LL, 3951.83, 3952.91, 3946.79, 3948.99), // 31: 01:15
        mk(1761442200000LL, 3949.0, 3952.34, 3946.79, 3948.15),  // 32: 01:30
        mk(1761443100000LL, 3948.16, 3949.15, 3914.3, 3931.4),   // 33: 01:45 (Stop exit 3926.03)
        mk(1761444000000LL, 3931.41, 3931.94, 3920.86, 3927.2),  // 34: 02:00
    };
}

} // namespace

int main() {
    PyramidTerraceHost host;
    const auto bars = terrace_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    // 1 trade from Cycle A + 3 trades from Cycle B = 4 trades total
    CHECK(host.trade_count() == 4);

    if (host.trade_count() >= 4) {
        // Trade 0: Cycle A (Trade 521)
        expect_trade("cycleA#521", host.get_trade(0), true,
                     4508.75, 4425.99, -87.2274, 15.9956, 85.0144);

        // Trade 1: Trade #647
        expect_trade("terrace#647", host.get_trade(1), true,
                     3944.12, 3926.03, -22.025075, 22.837940, 20.062060);

        // Trade 2: Trade #648
        expect_trade("terrace#648", host.get_trade(2), true,
                     3945.38, 3926.03, -23.285705, 16.117310, 21.322690);

        // Trade 3: Trade #649
        expect_trade("terrace#649", host.get_trade(3), true,
                     3953.42, 3926.03, -31.329725, 4.603290, 29.366710);
    }

    std::printf("test_l10q_pyramid_terrace_exit: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
