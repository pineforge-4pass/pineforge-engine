// R4-D L10x: zz-pop-fran-pineda-strategy-461-ts-m15 #350-#352 replay.
#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
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

bool near(double a, double b, double tol = 1e-6) {
    return std::abs(a - b) < tol;
}

void expect_trade(const char* tag, const Trade& t,
                  std::int64_t entry_time, double entry_price,
                  std::int64_t exit_time, double exit_price, double qty,
                  double pnl, double pnl_pct, double runup, double drawdown) {
    std::printf(
        "%s entry t=%lld px=%.5f exit t=%lld px=%.5f qty=%.8f pnl=%.6f "
        "pct=%.6f mfe=%.6f mae=%.6f bracket=%d\n",
        tag, static_cast<long long>(t.entry_time), t.entry_price,
        static_cast<long long>(t.exit_time), t.exit_price, t.qty, t.pnl,
        t.pnl_pct, t.max_runup, t.max_drawdown, t.exit_from_bracket);
    CHECK(!t.is_long);
    CHECK(t.entry_id == "Short");
    CHECK(t.exit_id == "Short TP/SL");
    CHECK(t.exit_from_bracket);
    CHECK(t.entry_time == entry_time);
    CHECK(near(t.entry_price, entry_price));
    CHECK(t.exit_time == exit_time);
    CHECK(near(t.exit_price, exit_price));
    CHECK(near(t.qty, qty));
    CHECK(near(t.pnl, pnl));
    CHECK(near(t.pnl_pct, pnl_pct));
    CHECK(near(t.max_runup, runup));
    CHECK(near(t.max_drawdown, drawdown));
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 10049.976589;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 95.0;
    c.pyramiding = 0;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = 0.02;
    c.process_orders_on_close = true;
    return c;
}

// Start at the owner equity after #349 (initial capital + cumulative net
// PnL), so the percent-of-equity quantities continue from the corpus path.
// Corpus strategy shape: a market short at the close, with TP/SL levels from
// the entry bar (stop = high + 20 ticks, limit = close - risk), re-issued
// every bar while the position is open.
class FranPinedaHost : public source::PineStrategyHost {
public:
    FranPinedaHost() {
        configure_pine_strategy(cfg());
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (live_position_size() == 0.0 && (i == 0 || i == 6 || i == 10)) {
            shortSL_ = bar.high + 0.2;
            shortTP_ = bar.close - (shortSL_ - bar.close);
            strategy_entry("Short", false);
            strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
            return;
        }
        if (live_position_size() < 0.0) {
            strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
        }
    }

    double shortSL_ = kNaN;
    double shortTP_ = kNaN;
};

std::vector<Bar> bars() {
    return {
        mk(1747612800000LL, 2496.98, 2513.38, 2472.8, 2479.74, 260269.476),
        mk(1747613700000LL, 2479.73, 2482.94, 2450.59, 2450.75, 169368.719),
        mk(1747614600000LL, 2450.74, 2455.47, 2426.24, 2442.59, 208471.473),
        mk(1747615500000LL, 2442.59, 2449.29, 2429.03, 2442.21, 106207.805),
        mk(1747616400000LL, 2442.21, 2449.89, 2422.13, 2429.89, 125120.525),
        mk(1747617300000LL, 2429.89, 2438.8, 2412.93, 2432.43, 117272.825),
        mk(1747618200000LL, 2432.43, 2441.15, 2420.75, 2425.42, 62119.484),
        mk(1747619100000LL, 2425.42, 2437.68, 2423.21, 2427.63, 43457.516),
        mk(1747620000000LL, 2427.63, 2434.6, 2396.45, 2398.2, 152236.792),
        mk(1747620900000LL, 2398.2, 2409.99, 2391.3, 2408.15, 151117.207),
        mk(1747621800000LL, 2408.15, 2416.94, 2405.06, 2407.98, 57410.676),
        mk(1747622700000LL, 2407.99, 2409.36, 2396.53, 2400.2, 55872.125),
    };
}


class FranPineda469Host : public source::PineStrategyHost {
public:
    FranPineda469Host() {
        source::PineStrategyConfig c = cfg();
        c.initial_capital = 9875.681048;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar& bar) override {
        if (pine_bar_index() == 0) {
            shortSL_ = bar.high + 0.2;
            shortTP_ = bar.close - (shortSL_ - bar.close);
            strategy_entry("Short", false);
            strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
            return;
        }
        if (live_position_size() < 0.0)
            strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
    }
    double shortSL_ = kNaN;
    double shortTP_ = kNaN;
};

std::vector<Bar> bars469() {
    return {
        mk(1749136200000LL, 2576.68, 2583.00, 2573.22, 2576.19, 31180.123),
        mk(1749137100000LL, 2576.19, 2581.54, 2571.57, 2575.21, 23452.624),
        mk(1749138000000LL, 2575.21, 2577.50, 2562.19, 2563.31, 92382.285),
        mk(1749138900000LL, 2563.30, 2565.44, 2506.00, 2539.00, 354463.792),
    };
}


class FranPineda1574Host : public source::PineStrategyHost {
public:
    FranPineda1574Host() {
        source::PineStrategyConfig c = cfg();
        c.initial_capital = 7414.648962111;
        configure_pine_strategy(c);
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar& bar) override {
        if (pine_bar_index() == 0) {
            shortSL_ = bar.high + 0.2;
            shortTP_ = bar.close - (shortSL_ - bar.close);
            strategy_entry("Short", false);
            strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
            return;
        }
        if (live_position_size() < 0.0)
            strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
    }
    double shortSL_ = kNaN;
    double shortTP_ = kNaN;
};

std::vector<Bar> bars1574() {
    return {
        mk(1763388900000LL, 3122.14, 3141.22, 3113.37, 3119.54, 87692.41),
        mk(1763389800000LL, 3119.53, 3210.22, 3113.00, 3177.26, 351943.753),
    };
}


}  // namespace

int main() {
    {
        FranPineda469Host host;
        const auto b = bars469();
        host.run(b.data(), static_cast<int>(b.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1)
            expect_trade("#469", host.get_trade(0), 1749136200000LL, 2576.19,
                         1749138000000LL, 2569.18, 3.64104394, 21.776814,
                         0.232162, 23.647714, 21.355589);
    }

    FranPinedaHost host;
    const auto b = bars();
    host.run(b.data(), static_cast<int>(b.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 3);
    if (host.trade_count() >= 3) {
        // Owner engine_trades.csv rows #350-#352, with the internal positive
        // drawdown convention (the CSV records the same excursion negative).
        expect_trade("#350", host.get_trade(0), 1747612800000LL, 2479.74,
                     1747614600000LL, 2445.90, 3.84942318, 126.472306,
                     1.324932, 128.355367, 14.227268);
        expect_trade("#351", host.get_trade(1), 1747618200000LL, 2425.42,
                     1747620000000LL, 2409.49, 3.98516270, 59.630061,
                     0.616925, 61.550503, 50.791233);
        expect_trade("#352", host.get_trade(2), 1747621800000LL, 2407.98,
                     1747622700000LL, 2398.82, 4.03754622, 33.102388,
                     0.340478, 35.039457, 7.516280);
    }
    {
        class FullLifecycleHost : public source::PineStrategyHost {
        public:
            FullLifecycleHost() {
                source::PineStrategyConfig c = cfg();
                c.initial_capital = 9786.272367;
                configure_pine_strategy(c);
                set_syminfo_metadata("ETHUSDT", 0.01);
            }
            double longSL_ = kNaN;
            double longTP_ = kNaN;
            double shortSL_ = kNaN;
            double shortTP_ = kNaN;
            void on_source_bar(const Bar& bar) override {
                const int i = pine_bar_index();
                const bool long_bar = i == 13 || i == 17;
                const bool short_bar = i == 0 || i == 45 || i == 60 || i == 75;
                if (live_position_size() == 0.0 && (long_bar || short_bar)) {
                    if (long_bar) {
                        longSL_ = bar.low - 0.2;
                        longTP_ = bar.close + (bar.close - longSL_) * 1.0;
                        shortSL_ = kNaN;
                        shortTP_ = kNaN;
                        strategy_entry("Long", true);
                        strategy_exit("Long TP/SL", "Long", longTP_, longSL_);
                    } else {
                        shortSL_ = bar.high + 0.2;
                        shortTP_ = bar.close - (shortSL_ - bar.close) * 1.0;
                        longSL_ = kNaN;
                        longTP_ = kNaN;
                        strategy_entry("Short", false);
                        strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
                    }
                    return;
                }
                if (live_position_size() > 0.0)
                    strategy_exit("Long TP/SL", "Long", longTP_, longSL_);
                if (live_position_size() < 0.0)
                    strategy_exit("Short TP/SL", "Short", shortTP_, shortSL_);
            }
        };
        FullLifecycleHost host;
        const std::vector<Bar> b = {
            mk(1749080700000, 2610.91, 2611.55, 2605, 2606.62, 18865.257),
            mk(1749081600000, 2606.62, 2612, 2605.81, 2606.69, 25621.402),
            mk(1749082500000, 2606.69, 2609.93, 2600.11, 2607.57, 29806.215),
            mk(1749083400000, 2607.57, 2614.01, 2607.35, 2608.35, 33828.047),
            mk(1749084300000, 2608.35, 2611.89, 2603.2, 2610.77, 26457.673),
            mk(1749085200000, 2610.76, 2614.4, 2610.31, 2613.02, 21275.623),
            mk(1749086100000, 2613.01, 2615, 2605.61, 2608.15, 23418.085),
            mk(1749087000000, 2608.15, 2608.24, 2603.51, 2607.06, 22013.76),
            mk(1749087900000, 2607.04, 2608.7, 2603.11, 2607.62, 14875.353),
            mk(1749088800000, 2607.61, 2618.8, 2607.61, 2617.3, 35034.582),
            mk(1749089700000, 2617.3, 2618.94, 2611.07, 2612.02, 19001.839),
            mk(1749090600000, 2612.02, 2627.94, 2612.01, 2623.76, 66948.503),
            mk(1749091500000, 2623.75, 2626.36, 2618, 2618.91, 35534.585),
            mk(1749092400000, 2618.9, 2625, 2616.01, 2623.01, 26674.328),
            mk(1749093300000, 2623, 2627, 2621.45, 2622.33, 19310.305),
            mk(1749094200000, 2622.34, 2631.68, 2620.05, 2626.4, 46798.012),
            mk(1749095100000, 2626.4, 2628.96, 2623.3, 2624, 19215.292),
            mk(1749096000000, 2624.01, 2628, 2620.41, 2624.64, 18314.074),
            mk(1749096900000, 2624.63, 2631.32, 2624.63, 2627.11, 34813.044),
            mk(1749097800000, 2627.1, 2632.56, 2626, 2631.43, 19549.978),
            mk(1749098700000, 2631.43, 2633.76, 2628.73, 2629.58, 22266.93),
            mk(1749099600000, 2629.58, 2630.54, 2623.43, 2624.45, 23021.784),
            mk(1749100500000, 2624.45, 2626.97, 2618.47, 2620.59, 28049.517),
            mk(1749101400000, 2620.59, 2620.74, 2607.31, 2610.51, 80688.766),
            mk(1749102300000, 2610.5, 2614.22, 2608.11, 2612.08, 33199.361),
            mk(1749103200000, 2612.08, 2614.88, 2608.21, 2611.11, 24258.605),
            mk(1749104100000, 2611.11, 2617.13, 2605.14, 2616.11, 44952.372),
            mk(1749105000000, 2616.11, 2617.16, 2601.5, 2603.38, 61915.961),
            mk(1749105900000, 2603.39, 2606.8, 2601.22, 2603.38, 48141.761),
            mk(1749106800000, 2603.38, 2609.97, 2603.38, 2608.88, 32419.634),
            mk(1749107700000, 2608.88, 2614.56, 2608.88, 2611.55, 30958.47),
            mk(1749108600000, 2611.55, 2611.94, 2603.9, 2604.83, 32748.026),
            mk(1749109500000, 2604.83, 2605.4, 2600, 2603.21, 45480.95),
            mk(1749110400000, 2603.22, 2606.87, 2602, 2604.6, 26992.837),
            mk(1749111300000, 2604.59, 2609.4, 2596, 2607.22, 53203.175),
            mk(1749112200000, 2607.21, 2608.87, 2600.66, 2602.74, 27948.691),
            mk(1749113100000, 2602.74, 2606.66, 2600.67, 2604.23, 19522.496),
            mk(1749114000000, 2604.22, 2609.3, 2604.12, 2606.51, 24913.486),
            mk(1749114900000, 2606.52, 2608.24, 2601, 2604.38, 24482.685),
            mk(1749115800000, 2604.38, 2607.5, 2602.11, 2606.66, 13756.329),
            mk(1749116700000, 2606.66, 2609.38, 2604.6, 2608.79, 22585.618),
            mk(1749117600000, 2608.78, 2612.68, 2607.09, 2611.46, 30617.774),
            mk(1749118500000, 2611.46, 2612, 2600.8, 2601.8, 31347.731),
            mk(1749119400000, 2601.8, 2604.68, 2579.14, 2590.33, 170600.861),
            mk(1749120300000, 2590.33, 2597.43, 2585.66, 2595.64, 66530.322),
            mk(1749121200000, 2595.64, 2599, 2592.26, 2594.38, 35289.115),
            mk(1749122100000, 2594.37, 2604.13, 2592.81, 2602.73, 44833.498),
            mk(1749123000000, 2602.73, 2608.73, 2602.55, 2604.51, 45262.889),
            mk(1749123900000, 2604.5, 2608.69, 2604.5, 2606.64, 22566.97),
            mk(1749124800000, 2606.63, 2619.99, 2604.22, 2618.38, 100219.955),
            mk(1749125700000, 2618.37, 2627.7, 2617.07, 2624.95, 88498.083),
            mk(1749126600000, 2624.95, 2629.99, 2618.4, 2627.63, 75995.5),
            mk(1749127500000, 2627.63, 2640, 2625.68, 2630.27, 148969.408),
            mk(1749128400000, 2630.27, 2634.66, 2620, 2625.3, 87337.559),
            mk(1749129300000, 2625.31, 2631.06, 2620.8, 2628.2, 89382.506),
            mk(1749130200000, 2628.21, 2629.89, 2611.33, 2613.02, 109364.506),
            mk(1749131100000, 2613.02, 2613.65, 2578, 2588.64, 323879.368),
            mk(1749132000000, 2588.65, 2594.39, 2583.58, 2586.35, 137444.616),
            mk(1749132900000, 2586.34, 2588.06, 2560.01, 2581.63, 308650.5),
            mk(1749133800000, 2581.61, 2595.5, 2570.06, 2592.81, 174407.416),
            mk(1749134700000, 2592.8, 2615, 2578.03, 2586.67, 285580.818),
            mk(1749135600000, 2586.66, 2591.88, 2580.81, 2582.23, 65669.98),
            mk(1749136500000, 2582.23, 2601.61, 2580.76, 2599.18, 85103.975),
            mk(1749137400000, 2599.19, 2600.84, 2589.44, 2595.44, 44018.136),
            mk(1749138300000, 2595.45, 2596.27, 2586.99, 2588.34, 40490.151),
            mk(1749139200000, 2588.35, 2590.7, 2579.11, 2579.75, 60543.126),
            mk(1749140100000, 2579.81, 2580.1, 2556.05, 2563.62, 202311.795),
            mk(1749141000000, 2563.61, 2571.41, 2556.68, 2564.6, 106290.015),
            mk(1749141900000, 2564.6, 2565.79, 2550.01, 2558.35, 120486.021),
            mk(1749142800000, 2558.34, 2572.57, 2557.43, 2569.99, 90956.86),
            mk(1749143700000, 2570, 2576.67, 2564.9, 2570.82, 67620.476),
            mk(1749144600000, 2570.81, 2573.9, 2565.68, 2569.21, 23925.021),
            mk(1749145500000, 2569.22, 2570.2, 2562.14, 2569.22, 40029.284),
            mk(1749146400000, 2569.21, 2573.75, 2555.08, 2558.54, 69589.201),
            mk(1749147300000, 2558.55, 2577.63, 2553.9, 2576.68, 72243.036),
            mk(1749148200000, 2576.68, 2583, 2573.22, 2576.19, 69520.189),
            mk(1749149100000, 2576.19, 2581.54, 2571.57, 2575.21, 33633.101),
            mk(1749150000000, 2575.21, 2577.5, 2562.19, 2563.31, 62126.193),
            mk(1749150900000, 2563.3, 2565.44, 2506, 2539, 546141.746),
        };
        host.run(b.data(), static_cast<int>(b.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 6);
        for (int i = 0; i < host.trade_count(); ++i) {
            const auto& t = host.get_trade(i);
            std::printf("FULLCYCLE entry t=%lld px=%.5f exit t=%lld px=%.5f qty=%.8f pnl=%.6f\n",
                static_cast<long long>(t.entry_time), t.entry_price,
                static_cast<long long>(t.exit_time), t.exit_price, t.qty, t.pnl);
        }
    }
    {
        FranPineda1574Host host;
        const auto b = bars1574();
        host.run(b.data(), static_cast<int>(b.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1)
            expect_trade("#1574", host.get_trade(0), 1763388900000LL, 3119.54,
                         1763389800000LL, 3141.42, 2.25754695, -52.222010,
                         -0.741526, 13.355855, 50.803629);
    }
    std::printf("test_l10x_reissued_limit_touch_bar: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
