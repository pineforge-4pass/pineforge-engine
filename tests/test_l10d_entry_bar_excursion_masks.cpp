// R4-D L10d: priced entries that fill mid-bar must not count the bar extreme
// reached BEFORE the fill (ab9714be pine_fills.cpp:42 skip_entry_bar_high/low
// + pine_risk.cpp:248-300). Literals are the legacy corpus engine_trades.csv
// rows that first diverge on 890da75.
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

#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

bool near(double a, double b, double tol = 1e-8) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

// The 15m ETH-USDT bars of the five replayed ranges, copied from the corpus
// derived feed (corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv) so the test
// runs without the corpus checkout (CI has no derived feeds).
struct FeedRow { std::int64_t ts; double o, h, l, c, v; };
const FeedRow kFeed[] = {
    {1743638400000LL, 1794.11, 1811.09, 1787.9, 1806.51, 116587.476},
    {1743639300000LL, 1806.5, 1819.26, 1803.16, 1810.67, 109245.479},
    {1743640200000LL, 1810.63, 1827.1, 1810.36, 1819.6, 78901.459},
    {1743641100000LL, 1819.55, 1821.42, 1810.89, 1819.6, 62465.596},
    {1743642000000LL, 1819.6, 1822.78, 1813.27, 1820.92, 40351.023},
    {1743642900000LL, 1820.92, 1826.09, 1818, 1824.86, 61577.211},
    {1743643800000LL, 1824.86, 1827, 1820.37, 1825.26, 38347.011},
    {1743644700000LL, 1825.25, 1834.12, 1824.68, 1825.37, 50098.789},
    {1743645600000LL, 1825.38, 1830.6, 1822.18, 1826.34, 41303.918},
    {1743646500000LL, 1826.33, 1827.5, 1817.05, 1823.47, 33106.454},
    {1743647400000LL, 1823.47, 1828.29, 1818.36, 1822.64, 30516.038},
    {1743648300000LL, 1822.65, 1832.14, 1822, 1829.41, 28524.327},
    {1743649200000LL, 1829.41, 1829.82, 1821.35, 1822.34, 23767.379},
    {1743650100000LL, 1822.34, 1828.3, 1821.98, 1825.35, 19451.132},
    {1743651000000LL, 1825.35, 1828.5, 1818.58, 1819.29, 28496.265},
    {1743651900000LL, 1819.29, 1825.01, 1819.22, 1823.75, 21582.318},
    {1743652800000LL, 1823.75, 1830, 1821.9, 1825.91, 31396.995},
    {1743653700000LL, 1825.91, 1827.37, 1823.52, 1824.36, 14824.433},
    {1743654600000LL, 1824.36, 1842.16, 1823.11, 1841.47, 88874.341},
    {1743655500000LL, 1841.47, 1844.97, 1833.7, 1834.7, 41725.407},
    {1743656400000LL, 1834.7, 1839.88, 1834.1, 1835.44, 22101.232},
    {1743657300000LL, 1835.45, 1835.62, 1830.3, 1832.8, 16827.446},
    {1743658200000LL, 1832.81, 1833.17, 1826, 1828.87, 46410.05},
    {1743659100000LL, 1828.86, 1828.86, 1820.18, 1820.97, 35573.648},
    {1743660000000LL, 1820.96, 1824.88, 1819.47, 1823.21, 23231.376},
    {1743660900000LL, 1823.21, 1827, 1819.59, 1825.69, 20801.06},
    {1743661800000LL, 1825.68, 1826.92, 1820.76, 1822.65, 27757.372},
    {1743662700000LL, 1822.66, 1825.83, 1813, 1816.61, 39797.919},
    {1743663600000LL, 1816.62, 1821.4, 1810.19, 1812.34, 42605.7},
    {1743664500000LL, 1812.34, 1820.76, 1805.56, 1819.24, 61190.762},
    {1743665400000LL, 1819.23, 1819.76, 1808, 1811.14, 30839.072},
    {1744617600000LL, 1634.92, 1638.66, 1628.64, 1630.11, 57355.204},
    {1744618500000LL, 1630.11, 1636.64, 1629.13, 1635.21, 32474.691},
    {1744619400000LL, 1635.2, 1641.81, 1632.23, 1638.65, 44238.333},
    {1744620300000LL, 1638.64, 1641.62, 1635.23, 1637.57, 41628.714},
    {1744621200000LL, 1637.58, 1644.78, 1635.76, 1642.69, 44823.912},
    {1744622100000LL, 1642.68, 1643.11, 1631.56, 1633.11, 46441.072},
    {1744623000000LL, 1633.11, 1635, 1630.6, 1634.56, 36407.175},
    {1744623900000LL, 1634.56, 1642.36, 1631.39, 1640.56, 52093.479},
    {1744624800000LL, 1640.56, 1654.99, 1638.44, 1646.28, 109966.13},
    {1744625700000LL, 1646.3, 1669.07, 1646.25, 1662.05, 225661.051},
    {1744626600000LL, 1662.06, 1680, 1662.04, 1678.4, 209337.024},
    {1744627500000LL, 1678.4, 1684.47, 1673.22, 1678.4, 140777.661},
    {1744628400000LL, 1678.4, 1688.62, 1671.82, 1685.55, 142627.641},
    {1744629300000LL, 1685.54, 1691.57, 1672.92, 1675.4, 152572.585},
    {1744630200000LL, 1675.4, 1679.56, 1670.86, 1672.02, 92706.238},
    {1744631100000LL, 1672.01, 1677.2, 1671.68, 1674.63, 34772.192},
    {1744632000000LL, 1674.63, 1677.58, 1665.18, 1666.11, 86750.841},
    {1744632900000LL, 1666.11, 1674.19, 1665.14, 1672.41, 46656.233},
    {1744633800000LL, 1672.42, 1676.08, 1670.75, 1671.77, 46299.72},
    {1744634700000LL, 1671.77, 1677.14, 1669.87, 1677.14, 33290.396},
    {1744635600000LL, 1677.14, 1679.94, 1669.5, 1674.42, 60008.625},
    {1744636500000LL, 1674.42, 1677.63, 1670.48, 1672.68, 45205.1},
    {1744637400000LL, 1672.69, 1674.85, 1651.04, 1657.08, 241849.236},
    {1744638300000LL, 1657.08, 1664.2, 1647.27, 1657.7, 137724.072},
    {1744639200000LL, 1657.71, 1666.02, 1654.15, 1661.93, 73451.864},
    {1744640100000LL, 1661.93, 1667.9, 1658.37, 1667.34, 64917.049},
    {1745340300000LL, 1693.52, 1696.22, 1685.32, 1691.13, 79865.342},
    {1745341200000LL, 1691.11, 1700.56, 1690.25, 1692.61, 101560.299},
    {1746885600000LL, 2436.99, 2444.05, 2418.65, 2424.71, 130809.474},
    {1746886500000LL, 2424.7, 2427.04, 2405.27, 2413.99, 126120.105},
    {1746887400000LL, 2413.99, 2427.69, 2412.66, 2427.13, 51967.473},
    {1746888300000LL, 2427.12, 2444.54, 2426.37, 2434.99, 117010.662},
    {1746889200000LL, 2434.99, 2438.12, 2421.14, 2430.2, 63395.67},
    {1746890100000LL, 2430.22, 2438.63, 2423.13, 2435.51, 53715.076},
    {1746891000000LL, 2435.5, 2445.5, 2423.79, 2430.1, 94852.75},
    {1746891900000LL, 2430.11, 2444.61, 2428.23, 2432.38, 78550.882},
    {1746892800000LL, 2432.38, 2442.57, 2427.65, 2438.69, 80184.143},
    {1746893700000LL, 2438.68, 2469.88, 2437.8, 2447.03, 208051.394},
    {1746894600000LL, 2447.04, 2474.66, 2443.2, 2460.67, 144638.358},
    {1746895500000LL, 2460.68, 2470.7, 2455.35, 2459.9, 92078.008},
    {1746896400000LL, 2459.91, 2482.81, 2454.08, 2476.91, 150576.094},
    {1746897300000LL, 2476.92, 2490, 2469.48, 2478.83, 127740.798},
    {1746898200000LL, 2478.83, 2512, 2477.35, 2494.24, 317824.816},
    {1746899100000LL, 2494.24, 2503.68, 2448.72, 2459.8, 317896.625},
    {1746900000000LL, 2459.79, 2481.21, 2456, 2480.15, 97650.242},
    {1746900900000LL, 2480.15, 2486.48, 2460.93, 2467.86, 96565.875},
    {1746901800000LL, 2467.86, 2481.56, 2462.11, 2479.87, 75516.87},
};

std::vector<Bar> load_15m(std::int64_t from_ms, std::int64_t to_ms) {
    std::vector<Bar> out;
    for (const auto& r : kFeed) {
        if (r.ts < from_ms || r.ts > to_ms) continue;
        out.push_back(Bar{r.o, r.h, r.l, r.c, r.v, r.ts});
    }
    return out;
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = (int)QtyType::FIXED;
    c.default_qty_value = 1;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    return c;
}

class DualStop : public source::PineStrategyHost {
public:
    DualStop() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("SE2", false, kNaN, current_bar_.close);
            strategy_entry("LE2", true, kNaN, current_bar_.close);
        }
    }
};

class GapShort : public source::PineStrategyHost {
public:
    GapShort() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("SE", false, kNaN, current_bar_.high * 10.0);
        if (pine_bar_index() == 24) strategy_close_all();
    }
};

class TouchShort : public source::PineStrategyHost {
public:
    TouchShort() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1 && std::isfinite(prev_low_))
            strategy_entry("SE", false, kNaN, prev_low_ - 0.01);
        if (pine_bar_index() == 17) strategy_close_all();
        prev_low_ = current_bar_.low;
    }
private:
    double prev_low_ = kNaN;
};

class CompositeShort : public source::PineStrategyHost {
public:
    CompositeShort() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("ShortOnGap", false, kNaN, current_bar_.close);
        if (pine_bar_index() == 1) strategy_close_all();
    }
};

class RangeLong : public source::PineStrategyHost {
public:
    RangeLong() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1 && std::isfinite(prev_high_))
            strategy_entry("LongOnRng", true, kNaN, prev_high_);
        if (pine_bar_index() == 28)
            strategy_entry("ShortOnRng", false, kNaN, 1813.0);
        prev_high_ = current_bar_.high;
    }
private:
    double prev_high_ = kNaN;
};

void expect_trade(const char* tag, const Trade& t, bool is_long,
                  double entry_px, double exit_px, double fav, double adv) {
    std::printf("%s %s @%.4f->%.4f mfe=%.6f mae=%.6f (want mfe=%.6f mae=%.6f)\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price,
                t.max_runup, t.max_drawdown, fav, adv);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px, 1e-6));
    CHECK(near(t.exit_price, exit_px, 1e-6));
    CHECK(near(t.max_runup, fav, 1e-6));
    CHECK(near(t.max_drawdown, adv, 1e-6));
}

} // namespace

int main() {
    {
        DualStop host;
        auto bars = load_15m(1745340300000LL, 1745341200000LL);
        if (bars.empty()) {
            bars = {
                mk(1745339100000LL, 1693.52, 1696.22, 1685.32, 1691.13),
                mk(1745340000000LL, 1691.11, 1700.56, 1690.25, 1692.61),
            };
        }
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() >= 1);
        if (host.trade_count() >= 1)
            expect_trade("dual-stop-open-tie#46", host.get_trade(0), false,
                         1691.11, 1691.13, 0.0, 0.02);
    }
    {
        GapShort host;
        auto bars = load_15m(1744617600000LL, 1744640100000LL);
        if (bars.empty()) {
            std::printf("FAIL gap-short: missing 15m csv\n");
            ++failed;
        } else {
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() >= 1);
            if (host.trade_count() >= 1)
                expect_trade("deferred-flip-gap-stops#29", host.get_trade(0), false,
                             1630.11, 1661.93, 0.0, 61.46);
        }
    }
    {
        TouchShort host;
        auto bars = load_15m(1746885600000LL, 1746901800000LL);
        if (bars.empty()) {
            std::printf("FAIL touch-short: missing 15m csv\n");
            ++failed;
        } else {
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() >= 1);
            if (host.trade_count() >= 1)
                expect_trade("stop-entry-touch-boundary#60", host.get_trade(0), false,
                             2413.99, 2467.86, 0.0, 98.01);
        }
    }
    {
        CompositeShort host;
        auto bars = load_15m(1743663600000LL, 1743665400000LL);
        if (bars.empty()) {
            bars = {
                mk(1743660000000LL, 1816.62, 1821.4, 1810.19, 1812.34),
                mk(1743660900000LL, 1812.34, 1820.76, 1805.56, 1819.24),
                mk(1743661800000LL, 1819.23, 1819.76, 1808.0, 1811.14),
            };
        }
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() >= 1);
        if (host.trade_count() >= 1)
            expect_trade("composite-bracket-cap#11", host.get_trade(0), false,
                         1812.34, 1819.23, 0.0, 8.42);
    }
    {
        RangeLong host;
        auto bars = load_15m(1743638400000LL, 1743664500000LL);
        if (bars.empty()) {
            std::printf("FAIL range-long: missing 15m csv\n");
            ++failed;
        } else {
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() >= 1);
            if (host.trade_count() >= 1)
                expect_trade("range-expansion-pending-stop#22", host.get_trade(0), true,
                             1811.09, 1812.34, 33.88, 5.53);
        }
    }
    std::printf("test_l10d_entry_bar_excursion_masks: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
