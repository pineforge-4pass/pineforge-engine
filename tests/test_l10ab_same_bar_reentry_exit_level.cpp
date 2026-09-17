// R4-D L10ab: zz-pop-stevenygabbyperez-fast-scalper-with-stops #15-#17 replay.
// On the switched route a same-direction re-entry on the bar where the previous
// lot of the same id was stopped out is NOT closed on that same bar at the
// previous lot's exit level.
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

bool near(double a, double b, double tol = 1e-4) {
    return std::abs(a - b) <= tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

// 81 bars embedded from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv:
// 2025-04-01 19:45 UTC to 2025-04-02 15:45 UTC.
std::vector<Bar> sample_bars() {
    return {
        mk(1743536700000LL, 1905.99, 1910.50, 1904.70, 1909.12, 31359.364), // 0: 19:45 signal bar (#14)
        mk(1743537600000LL, 1909.11, 1909.75, 1904.16, 1908.36, 28960.208), // 1: 20:00 entry bar (#14)
        mk(1743538500000LL, 1908.35, 1912.65, 1900.00, 1901.60, 32868.284), // 2
        mk(1743539400000LL, 1901.61, 1907.49, 1895.90, 1905.00, 43535.045), // 3
        mk(1743540300000LL, 1905.00, 1915.20, 1904.60, 1912.06, 44179.766), // 4
        mk(1743541200000LL, 1912.07, 1916.56, 1907.55, 1913.53, 30791.517), // 5
        mk(1743542100000LL, 1913.52, 1916.11, 1912.43, 1914.01, 10333.544), // 6
        mk(1743543000000LL, 1914.00, 1921.98, 1910.70, 1917.24, 39196.076), // 7
        mk(1743543900000LL, 1917.23, 1917.24, 1911.61, 1914.19, 12319.285), // 8
        mk(1743544800000LL, 1914.20, 1916.73, 1910.76, 1915.54, 23016.947), // 9
        mk(1743545700000LL, 1915.55, 1917.10, 1912.69, 1916.01, 27918.883), // 10
        mk(1743546600000LL, 1916.02, 1919.30, 1914.21, 1914.47, 15517.902), // 11
        mk(1743547500000LL, 1914.47, 1916.00, 1910.09, 1911.00, 14411.016), // 12
        mk(1743548400000LL, 1911.01, 1912.13, 1903.86, 1904.52, 25837.120), // 13: 23:00 signal bar (#15, #16)
        mk(1743549300000LL, 1904.52, 1910.64, 1904.51, 1910.06, 14948.886), // 14: 23:15 entry bar (#15, #16), exit bar (#14, #15)
        mk(1743550200000LL, 1910.05, 1914.36, 1908.00, 1909.32, 16539.141), // 15
        mk(1743551100000LL, 1909.32, 1909.32, 1903.77, 1904.21, 15025.522), // 16
        mk(1743552000000LL, 1904.21, 1908.01, 1900.09, 1900.21, 40336.673), // 17
        mk(1743552900000LL, 1900.21, 1904.46, 1896.20, 1896.99, 31026.742), // 18
        mk(1743553800000LL, 1896.99, 1899.98, 1891.62, 1893.74, 39420.219), // 19
        mk(1743554700000LL, 1893.74, 1896.46, 1886.04, 1889.06, 53466.340), // 20
        mk(1743555600000LL, 1889.07, 1891.84, 1882.02, 1883.91, 50546.777), // 21
        mk(1743556500000LL, 1883.90, 1888.46, 1882.69, 1884.99, 35527.597), // 22
        mk(1743557400000LL, 1884.99, 1895.84, 1884.00, 1891.95, 51463.389), // 23
        mk(1743558300000LL, 1891.96, 1893.29, 1887.94, 1891.71, 18083.236), // 24
        mk(1743559200000LL, 1891.71, 1892.41, 1875.50, 1876.71, 70709.866), // 25
        mk(1743560100000LL, 1876.71, 1881.28, 1871.31, 1880.71, 52124.133), // 26
        mk(1743561000000LL, 1880.70, 1883.45, 1877.07, 1878.10, 32257.169), // 27
        mk(1743561900000LL, 1878.10, 1882.74, 1877.12, 1877.57, 15936.818), // 28
        mk(1743562800000LL, 1877.56, 1884.54, 1876.68, 1883.61, 22261.401), // 29
        mk(1743563700000LL, 1883.60, 1886.45, 1880.34, 1882.81, 24772.014), // 30
        mk(1743564600000LL, 1882.80, 1883.07, 1875.44, 1879.46, 20407.298), // 31
        mk(1743565500000LL, 1879.45, 1879.65, 1874.46, 1875.41, 19276.052), // 32
        mk(1743566400000LL, 1875.40, 1881.71, 1874.79, 1880.68, 18449.907), // 33
        mk(1743567300000LL, 1880.67, 1882.31, 1878.63, 1880.67, 15573.626), // 34
        mk(1743568200000LL, 1880.66, 1880.67, 1876.19, 1876.59, 15442.421), // 35
        mk(1743569100000LL, 1876.59, 1879.81, 1874.72, 1878.14, 15700.915), // 36
        mk(1743570000000LL, 1878.14, 1878.38, 1862.53, 1866.16, 88547.217), // 37: 05:00 exit bar (#16)
        mk(1743570900000LL, 1866.16, 1866.46, 1853.57, 1855.88, 146270.205), // 38
        mk(1743571800000LL, 1855.88, 1862.18, 1854.03, 1859.99, 46681.148), // 39
        mk(1743572700000LL, 1859.99, 1863.69, 1854.10, 1855.08, 53916.625), // 40
        mk(1743573600000LL, 1855.07, 1859.35, 1853.86, 1857.20, 42443.813), // 41
        mk(1743574500000LL, 1857.20, 1861.45, 1850.69, 1855.80, 63467.607), // 42
        mk(1743575400000LL, 1855.79, 1861.72, 1851.70, 1860.89, 42271.908), // 43
        mk(1743576300000LL, 1860.89, 1860.90, 1852.63, 1852.84, 35317.698), // 44
        mk(1743577200000LL, 1852.84, 1856.59, 1851.34, 1854.99, 29196.478), // 45
        mk(1743578100000LL, 1854.98, 1861.60, 1853.33, 1856.15, 53135.188), // 46
        mk(1743579000000LL, 1856.16, 1863.88, 1855.20, 1858.90, 55632.844), // 47
        mk(1743579900000LL, 1858.90, 1865.78, 1858.33, 1864.07, 34732.681), // 48
        mk(1743580800000LL, 1864.06, 1866.44, 1861.81, 1864.52, 33820.020), // 49
        mk(1743581700000LL, 1864.51, 1869.36, 1861.61, 1867.93, 32073.213), // 50: 08:15 signal bar (#17)
        mk(1743582600000LL, 1867.93, 1875.54, 1867.11, 1871.42, 57174.436), // 51: 08:30 entry bar (#17)
        mk(1743583500000LL, 1871.41, 1872.60, 1865.88, 1866.91, 17980.898), // 52
        mk(1743584400000LL, 1866.90, 1887.23, 1865.38, 1883.98, 133661.846), // 53
        mk(1743585300000LL, 1883.98, 1885.00, 1874.00, 1878.59, 34794.978), // 54
        mk(1743586200000LL, 1878.58, 1881.73, 1876.00, 1876.58, 20336.010), // 55
        mk(1743587100000LL, 1876.57, 1880.38, 1874.98, 1878.44, 14083.235), // 56
        mk(1743588000000LL, 1878.43, 1883.80, 1876.15, 1880.03, 29455.190), // 57
        mk(1743588900000LL, 1880.02, 1886.70, 1878.19, 1878.91, 34278.998), // 58
        mk(1743589800000LL, 1878.90, 1878.98, 1871.85, 1874.48, 37542.352), // 59
        mk(1743590700000LL, 1874.47, 1876.52, 1870.00, 1871.92, 24803.139), // 60
        mk(1743591600000LL, 1871.93, 1877.62, 1871.00, 1874.59, 23225.240), // 61
        mk(1743592500000LL, 1874.59, 1877.53, 1872.50, 1872.60, 14322.558), // 62
        mk(1743593400000LL, 1872.60, 1873.99, 1868.50, 1871.40, 33717.537), // 63
        mk(1743594300000LL, 1871.39, 1872.64, 1867.32, 1868.05, 35603.983), // 64
        mk(1743595200000LL, 1868.08, 1869.97, 1859.53, 1865.27, 69805.742), // 65
        mk(1743596100000LL, 1865.26, 1865.99, 1857.06, 1857.87, 44507.866), // 66
        mk(1743597000000LL, 1857.87, 1867.48, 1857.54, 1866.86, 44885.792), // 67
        mk(1743597900000LL, 1866.85, 1872.55, 1863.50, 1871.16, 46399.418), // 68
        mk(1743598800000LL, 1871.16, 1871.17, 1860.21, 1860.90, 37079.111), // 69
        mk(1743599700000LL, 1860.91, 1863.27, 1856.89, 1858.15, 39716.662), // 70
        mk(1743600600000LL, 1858.15, 1876.61, 1853.12, 1872.19, 123887.785), // 71
        mk(1743601500000LL, 1872.27, 1892.70, 1869.82, 1870.41, 204369.033), // 72
        mk(1743602400000LL, 1870.40, 1880.71, 1856.50, 1863.13, 143174.077), // 73
        mk(1743603300000LL, 1863.14, 1872.65, 1860.01, 1870.75, 65884.918), // 74
        mk(1743604200000LL, 1870.75, 1875.85, 1863.52, 1870.75, 54007.966), // 75
        mk(1743605100000LL, 1870.74, 1877.04, 1868.55, 1874.69, 35059.783), // 76
        mk(1743606000000LL, 1874.70, 1894.47, 1873.16, 1881.00, 139752.351), // 77
        mk(1743606900000LL, 1881.00, 1893.06, 1880.50, 1889.52, 76522.994), // 78
        mk(1743607800000LL, 1889.52, 1918.00, 1889.30, 1913.94, 334936.345), // 79: 15:30 exit bar (#17)
        mk(1743608700000LL, 1913.90, 1918.00, 1892.39, 1900.09, 313381.872), // 80
    };
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 999554.2178;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

class FastScalperSameBarReentryHost : public source::PineStrategyHost {
public:
    FastScalperSameBarReentryHost() {
        attach_pine_execution_adapter();
        configure_pine_strategy(cfg());
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 13) {
            strategy_entry("Short", false);
            strategy_exit("Exit Short", "Short", kNaN, bar.close * 1.01,
                          bar.close * 0.02 / 0.01);
        }
        if (i == 50) {
            strategy_entry("Long", true);
            strategy_exit("Exit Long", "Long", kNaN, bar.close * 0.99,
                          bar.close * 0.02 / 0.01);
        }
        if (i == 75) {
            strategy_exit("Exit Long", "Long", kNaN, bar.close * 0.99,
                          bar.close * 0.02 / 0.01);
        }
    }
};

void test_same_bar_reentry_exit_level() {
    FastScalperSameBarReentryHost host;
    const auto bars = sample_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    if (host.trade_count() != 3) {
        std::printf("Expected 3 trades, got %d:\n", host.trade_count());
        for (int i = 0; i < host.trade_count(); ++i) {
            const auto& t = host.get_trade(i);
            std::printf("  [%d] %s id=%s/%s px=%.2f->%.2f q=%.8f pnl=%.2f entry_t=%lld exit_t=%lld\n",
                        i, t.is_long ? "L" : "S", t.entry_id.c_str(), t.exit_id.c_str(),
                        t.entry_price, t.exit_price, t.qty, t.pnl,
                        static_cast<long long>(t.entry_time), static_cast<long long>(t.exit_time));
        }
    }
    CHECK(host.trade_count() == 3);
    if (host.trade_count() < 3) return;

    // Trade #0 (corpus #15): Short margin call slice at 1910.64 on the entry bar.
    const auto& t0 = host.get_trade(0);
    CHECK(!t0.is_long);
    CHECK(t0.entry_id == "Short");
    CHECK(t0.exit_id == "__margin_call__");
    CHECK(t0.entry_time == 1743549300000LL);
    CHECK(t0.exit_time == 1743549300000LL);
    CHECK(near(t0.entry_price, 1904.52));
    CHECK(near(t0.exit_price, 1910.64));
    CHECK(near(t0.qty, 13.44879463));
    CHECK(near(t0.pnl, -82.306623));
    CHECK(near(t0.max_runup, 0.134488));
    CHECK(near(t0.max_drawdown, 82.306623));

    // Trade #1 (corpus #16): Main lot must NOT be closed on bar 14 (23:15) at 1910.64.
    // It survives to bar 37 (05:00) and exits at 1866.42 via trailing stop.
    const auto& t1 = host.get_trade(1);
    CHECK(!t1.is_long);
    CHECK(t1.entry_id == "Short");
    CHECK(t1.entry_time == 1743549300000LL);
    CHECK(t1.exit_time == 1743570000000LL);
    CHECK(near(t1.entry_price, 1904.52));
    CHECK(near(t1.exit_price, 1866.42));
    CHECK(near(t1.qty, 511.38382343));
    CHECK(near(t1.pnl, 19483.723673));
    CHECK(near(t1.max_runup, 19483.723673));
    CHECK(near(t1.max_drawdown, 5032.016823));

    // Trade #2 (corpus #17): Long entry at 1867.93 exits at 1905.35 (not divergent 1905.29).
    const auto& t2 = host.get_trade(2);
    CHECK(t2.is_long);
    CHECK(t2.entry_id == "Long");
    CHECK(t2.entry_time == 1743582600000LL);
    CHECK(t2.exit_time == 1743607800000LL);
    CHECK(near(t2.entry_price, 1867.93));
    CHECK(near(t2.exit_price, 1905.35));
    CHECK(near(t2.qty, 545.49990353));
    CHECK(near(t2.pnl, 20412.606390));
    CHECK(near(t2.max_runup, 20412.606390));
    CHECK(near(t2.max_drawdown, 8078.853571));
}

}  // namespace

int main() {
    test_same_bar_reentry_exit_level();
    std::printf("test_l10ab_same_bar_reentry_exit_level: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
