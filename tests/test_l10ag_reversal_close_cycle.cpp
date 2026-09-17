// R4-D L10ag: on a switched route a one-bar reversal (strategy.entry of the
// opposite side followed by strategy.close of the held side) must leave the
// freshly opened lot OPEN, exactly as the legacy owner does.  The deferred
// close belongs to the position cycle it was issued in: when the next open's
// opposite entry applies first, the stale close is Removed rather than
// flattening the new lot at its own entry price for zero PnL
// (ab9714be src/source/pine_fills.cpp:7449-7459).
//
// Pinned literals are owner (engine ab9714b) trades #1-#3 of
// zz-pop-b3ar-trades-ehlers-dominant-cycle-stochastic-rsi-v2 on the
// data-BINANCE-BTCUSDT 15m lane feed, replayed here on the embedded bar rows
// below (copied verbatim out of ohlcv_BINANCE-BTCUSDT_15m.csv, 2025-03-31
// 13:30 .. 2025-04-01 16:00 UTC) with that probe's BTCUSDT facts
// (mintick 0.01, pointvalue 1, initial_capital 10000,
// default_qty_type=strategy.percent_of_equity, default_qty_value=10,
// pyramiding default, process_orders_on_close=false).  No corpus file is
// opened at runtime.
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
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

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 10000;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 10.0;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.calc_on_order_fills = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    c.margin_long = 100;
    c.margin_short = 100;
    return c;
}

// The probe script's order shape (strategy.pine:346-355): a bullish cross
// issues strategy.entry("Long") and then strategy.close("Short"); a bearish
// cross issues strategy.entry("Short") and then strategy.close("Long").
class ReversalHost : public source::PineStrategyHost {
public:
    ReversalHost() {
        configure_pine_strategy(cfg());
        set_syminfo_metadata("BTCUSDT", 0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {                                   // 2025-03-31 13:30
            strategy_entry("Short", false);
        } else if (i == 41) {                           // 2025-03-31 23:45
            strategy_entry("Long", true);
            strategy_close("Short");
        } else if (i == 65) {                           // 2025-04-01 05:45
            strategy_entry("Short", false);
            strategy_close("Long");
        } else if (i == 101) {                          // 2025-04-01 14:45
            strategy_entry("Long", true);
            strategy_close("Short");
        }
    }
};

std::vector<Bar> btcusdt_15m() {
    return {
    mk(1743427800000, 82553.8, 82613.28, 81759.2, 81912.97),
    mk(1743428700000, 81912.97, 82780.01, 81656.0, 82507.99),
    mk(1743429600000, 82507.99, 83425.05, 82507.49, 83338.48),
    mk(1743430500000, 83338.49, 83600.0, 82822.93, 82822.94),
    mk(1743431400000, 82822.95, 83479.84, 82822.95, 83173.25),
    mk(1743432300000, 83173.26, 83746.23, 83167.11, 83499.18),
    mk(1743433200000, 83499.18, 83827.79, 83405.66, 83440.04),
    mk(1743434100000, 83440.04, 83943.08, 83438.45, 83635.24),
    mk(1743435000000, 83635.25, 83750.0, 83440.85, 83503.77),
    mk(1743435900000, 83503.76, 83708.5, 83390.3, 83418.78),
    mk(1743436800000, 83418.78, 83694.29, 83260.39, 83339.99),
    mk(1743437700000, 83340.0, 83620.0, 83283.0, 83613.88),
    mk(1743438600000, 83613.88, 83613.88, 83333.95, 83376.16),
    mk(1743439500000, 83376.15, 83500.0, 83246.86, 83288.89),
    mk(1743440400000, 83288.88, 83545.55, 83256.41, 83465.73),
    mk(1743441300000, 83465.74, 83469.38, 83199.43, 83299.08),
    mk(1743442200000, 83299.08, 83387.95, 82755.4, 82807.31),
    mk(1743443100000, 82807.31, 82832.0, 82575.45, 82680.34),
    mk(1743444000000, 82680.35, 83309.92, 82676.0, 83138.5),
    mk(1743444900000, 83138.5, 83354.91, 83085.44, 83350.0),
    mk(1743445800000, 83350.0, 83566.0, 83299.71, 83416.66),
    mk(1743446700000, 83416.66, 83527.54, 83270.48, 83291.42),
    mk(1743447600000, 83291.42, 83443.38, 83273.91, 83367.94),
    mk(1743448500000, 83367.94, 83369.74, 82929.3, 82937.86),
    mk(1743449400000, 82937.85, 83040.0, 82795.65, 82806.11),
    mk(1743450300000, 82806.11, 82806.11, 82400.0, 82462.27),
    mk(1743451200000, 82462.26, 82693.48, 82365.0, 82591.0),
    mk(1743452100000, 82591.0, 82801.45, 82564.06, 82773.69),
    mk(1743453000000, 82773.7, 82846.2, 82676.31, 82745.69),
    mk(1743453900000, 82745.69, 82753.2, 82350.84, 82431.32),
    mk(1743454800000, 82431.32, 82560.0, 82422.0, 82556.33),
    mk(1743455700000, 82556.34, 82752.18, 82501.04, 82576.62),
    mk(1743456600000, 82576.63, 82698.61, 82547.15, 82618.0),
    mk(1743457500000, 82617.99, 82695.34, 82500.0, 82563.99),
    mk(1743458400000, 82563.99, 82571.98, 82404.34, 82420.14),
    mk(1743459300000, 82420.15, 82420.15, 82265.1, 82359.69),
    mk(1743460200000, 82359.68, 82402.19, 82283.81, 82333.01),
    mk(1743461100000, 82333.01, 82411.29, 82290.01, 82409.99),
    mk(1743462000000, 82410.0, 82638.82, 82388.21, 82560.64),
    mk(1743462900000, 82560.64, 82886.8, 82329.25, 82348.31),
    mk(1743463800000, 82348.32, 82547.96, 82283.9, 82472.54),
    mk(1743464700000, 82472.54, 82589.61, 82396.37, 82550.01),
    mk(1743465600000, 82550.0, 82712.54, 82483.49, 82483.49),
    mk(1743466500000, 82483.5, 82750.02, 82432.74, 82640.0),
    mk(1743467400000, 82640.0, 82735.09, 82543.27, 82727.18),
    mk(1743468300000, 82727.18, 82837.38, 82566.94, 82651.1),
    mk(1743469200000, 82651.09, 82676.88, 82460.0, 82568.14),
    mk(1743470100000, 82568.13, 82724.0, 82509.13, 82704.78),
    mk(1743471000000, 82704.78, 82741.8, 82548.83, 82578.27),
    mk(1743471900000, 82578.27, 82740.68, 82578.27, 82704.2),
    mk(1743472800000, 82704.2, 82845.11, 82649.34, 82845.09),
    mk(1743473700000, 82845.1, 83020.13, 82845.1, 82893.78),
    mk(1743474600000, 82893.77, 82956.85, 82795.65, 82950.01),
    mk(1743475500000, 82950.0, 83120.0, 82882.6, 83100.0),
    mk(1743476400000, 83100.0, 83200.0, 83031.72, 83180.8),
    mk(1743477300000, 83180.8, 83256.8, 83085.11, 83085.46),
    mk(1743478200000, 83085.46, 83121.5, 82991.29, 83006.01),
    mk(1743479100000, 83006.02, 83085.06, 82926.08, 82962.84),
    mk(1743480000000, 82962.84, 83023.25, 82900.0, 82935.99),
    mk(1743480900000, 82935.98, 83055.01, 82886.87, 83047.61),
    mk(1743481800000, 83047.62, 83235.96, 83047.61, 83168.09),
    mk(1743482700000, 83168.09, 83168.3, 83056.52, 83060.0),
    mk(1743483600000, 83059.99, 83169.82, 83013.39, 83169.82),
    mk(1743484500000, 83169.82, 83329.99, 83169.81, 83291.86),
    mk(1743485400000, 83291.85, 83329.31, 83128.04, 83171.51),
    mk(1743486300000, 83171.5, 83186.07, 82994.07, 83006.67),
    mk(1743487200000, 83006.67, 83049.99, 82891.71, 82991.66),
    mk(1743488100000, 82991.66, 83134.58, 82968.4, 83134.58),
    mk(1743489000000, 83134.57, 83386.0, 83118.03, 83356.5),
    mk(1743489900000, 83356.51, 83494.25, 83294.59, 83454.76),
    mk(1743490800000, 83454.76, 83514.26, 83380.58, 83510.01),
    mk(1743491700000, 83510.0, 83569.94, 83387.06, 83450.0),
    mk(1743492600000, 83450.0, 83597.07, 83405.65, 83531.38),
    mk(1743493500000, 83531.37, 83586.0, 83438.98, 83445.5),
    mk(1743494400000, 83445.5, 83700.0, 83410.0, 83696.52),
    mk(1743495300000, 83696.53, 84023.41, 83696.52, 83924.5),
    mk(1743496200000, 83924.5, 84180.0, 83924.5, 83989.51),
    mk(1743497100000, 83989.51, 84207.55, 83989.5, 84121.94),
    mk(1743498000000, 84121.94, 84329.0, 84116.27, 84156.62),
    mk(1743498900000, 84156.62, 84290.7, 84136.76, 84226.22),
    mk(1743499800000, 84226.23, 84363.6, 84204.46, 84335.99),
    mk(1743500700000, 84336.0, 84395.46, 84264.1, 84362.0),
    mk(1743501600000, 84362.01, 84394.48, 84162.42, 84284.88),
    mk(1743502500000, 84284.89, 84504.74, 84284.89, 84432.14),
    mk(1743503400000, 84432.15, 84441.44, 84178.45, 84244.44),
    mk(1743504300000, 84244.44, 84348.0, 84150.0, 84175.99),
    mk(1743505200000, 84175.99, 84203.0, 83955.86, 84097.15),
    mk(1743506100000, 84098.0, 84173.23, 83982.25, 84049.0),
    mk(1743507000000, 84048.99, 84048.99, 83823.67, 83982.02),
    mk(1743507900000, 83982.01, 84050.0, 83878.47, 83930.3),
    mk(1743508800000, 83930.31, 84132.93, 83924.52, 84097.99),
    mk(1743509700000, 84097.98, 84154.22, 83918.16, 84073.5),
    mk(1743510600000, 84073.49, 84073.49, 83681.12, 83749.99),
    mk(1743511500000, 83749.99, 83811.27, 83615.25, 83615.26),
    mk(1743512400000, 83615.26, 83821.89, 83596.0, 83777.68),
    mk(1743513300000, 83777.67, 83777.68, 83615.0, 83679.98),
    mk(1743514200000, 83679.99, 83830.16, 83416.66, 83734.27),
    mk(1743515100000, 83734.26, 83912.62, 83530.89, 83618.23),
    mk(1743516000000, 83622.96, 83712.48, 82647.05, 82796.1),
    mk(1743516900000, 82796.11, 83056.28, 82544.0, 82970.12),
    mk(1743517800000, 82970.12, 83653.88, 82940.0, 83507.99),
    mk(1743518700000, 83507.99, 84350.0, 83433.48, 84195.92),
    mk(1743519600000, 84195.92, 84910.0, 84182.89, 84906.0),
    mk(1743520500000, 84905.99, 85227.27, 84699.0, 84750.49),
    mk(1743521400000, 84750.49, 85331.57, 84750.46, 85331.24),
    mk(1743522300000, 85331.23, 85499.55, 85209.12, 85280.87),
    mk(1743523200000, 85280.88, 85493.0, 85105.99, 85179.73),
    };
}

}  // namespace

int main() {
    ReversalHost host;
    const auto bars = btcusdt_15m();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // Owner rows #1-#3.  The phantom shape this lane removes is a fourth row
    // pair opening and closing on the reversal bar at its own entry price.
    CHECK(host.trade_count() == 3);
    if (host.trade_count() < 3) {
        std::printf("test_l10ag_reversal_close_cycle: %d passed, %d failed\n",
                    passed, failed);
        return failed == 0 ? 0 : 1;
    }
    for (int t = 0; t < host.trade_count(); ++t) {
        const auto& row = host.get_trade(t);
        CHECK(!(row.entry_time == row.exit_time && near(row.pnl, 0.0)));
        CHECK(!row.exit_from_bracket);
    }

    // #1 Entry short 2025-03-31 13:45 @81912.97 -> Exit short 2025-04-01
    // 00:00 @82550.00, qty 0.01220808, PnL -7.776912 (-0.7777%).
    const auto& t1 = host.get_trade(0);
    CHECK(t1.entry_id == "Short");
    CHECK(!t1.is_long);
    CHECK(t1.entry_time == 1743428700000);
    CHECK(near(t1.entry_price, 81912.97));
    CHECK(t1.exit_time == 1743465600000);
    CHECK(near(t1.exit_price, 82550.00));
    CHECK(near(t1.qty, 0.01220808, 1e-8));
    CHECK(near(t1.pnl, -7.776912, 1e-4));
    CHECK(near(t1.pnl_pct, -0.7777, 1e-3));
    // Trade.max_drawdown stays positive (Pine's accessor convention); the
    // report's "Adverse excursion USD" column negates it.
    CHECK(near(t1.max_runup, 3.137110, 1e-3));
    CHECK(near(t1.max_drawdown, 24.783743, 1e-3));

    // #2 Entry long 2025-04-01 00:00 @82550.00, qty 0.01210445 -> Exit long
    // 06:00 @83006.67, PnL +5.527738 (+0.5532%).  The reversal bar's stale
    // close("Short") must NOT flatten this lot at 82550.00 for zero PnL.
    const auto& t2 = host.get_trade(1);
    CHECK(t2.entry_id == "Long");
    CHECK(t2.is_long);
    CHECK(t2.entry_time == 1743465600000);
    CHECK(near(t2.entry_price, 82550.00));
    CHECK(t2.exit_time == 1743487200000);
    CHECK(near(t2.exit_price, 83006.67));
    CHECK(near(t2.qty, 0.01210445, 1e-8));
    CHECK(near(t2.pnl, 5.527738, 1e-4));
    CHECK(near(t2.pnl_pct, 0.5532, 1e-3));
    CHECK(near(t2.max_runup, 9.441348, 1e-3));
    CHECK(near(t2.max_drawdown, 1.419368, 1e-3));

    // #3 Entry short 2025-04-01 06:00 @83006.67, qty 0.01204452 -> Exit short
    // 15:00 @84195.92, PnL -14.323939 (-1.4327%).  Its quantity carries the
    // owner equity path: the removed phantom round trips did not perturb it.
    const auto& t3 = host.get_trade(2);
    CHECK(t3.entry_id == "Short");
    CHECK(!t3.is_long);
    CHECK(t3.entry_time == 1743487200000);
    CHECK(near(t3.entry_price, 83006.67));
    CHECK(t3.exit_time == 1743519600000);
    CHECK(near(t3.exit_price, 84195.92));
    CHECK(near(t3.qty, 0.01204452, 1e-8));
    CHECK(near(t3.pnl, -14.323939, 1e-4));
    CHECK(near(t3.pnl_pct, -1.4327, 1e-3));
    CHECK(near(t3.max_runup, 5.572636, 1e-3));
    CHECK(near(t3.max_drawdown, 18.043527, 1e-3));

    // The last reversal's freshly opened long lot stays OPEN (owner trade #4).
    CHECK(host.live_position_size() > 0.0);

    std::printf("test_l10ag_reversal_close_cycle: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
